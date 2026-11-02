#include "audio/mixer.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "core/cpu.hpp"
#include "logger/logger.hpp"

namespace audio {

namespace {

constexpr size_t RING_FRAMES = RATE * 2;  // 2 seconds
constexpr int CALLBACK_FRAMES = 1024;
constexpr int MAX_VOICES = 8;
constexpr float PI = 3.14159265358979f;

SDL_AudioDeviceID g_dev = 0;
std::atomic<int> g_cpu_tag{-1};  // SDL's thread that calls callback() (cpu::tag); -2 once closing
std::mutex g_m;

// ring buffer (frames of 2 x int16)
std::vector<int16_t> g_ring;
uint64_t g_write = 0;  // total frames written
uint64_t g_read = 0;   // total frames consumed
struct Marker { uint64_t frame; double pts; float speed; };
std::deque<Marker> g_markers;
bool g_paused = true;
uint32_t g_generation = 0;
float g_volume = 1.0f;
std::atomic<float> g_fade{1.0f};
double g_underrun = 0;

// clock interpolation between callbacks
uint64_t g_cb_read_start = 0;
Uint64 g_cb_time = 0;
bool g_clock_valid = false;

// sfx: each sound in each style (the tick has three pitches), and what is playing
std::vector<float> g_sfx[2][SFX_COUNT][3];
struct Voice { int sfx = -1; int variant = 0; size_t pos = 0; float vol = 1; uint32_t started = 0; };
Voice g_voices[MAX_VOICES];
uint32_t g_voice_counter = 0;
std::atomic<bool> g_sfx_on{true}, g_sfx_ticks{true}, g_sfx_kids{false};
std::atomic<int> g_sfx_style{STYLE_SUBTLE};
std::atomic<float> g_sfx_vol{0.7f};
constexpr int MAX_SFX_VOICES = 3;          // more at once is mud
constexpr float SUBTLE_MAKEUP = 1.5f;      // the soft set is baked quiet; this is its room above the video's sound
ui_sounds::TickLimiter g_tick_limiter;
uint32_t g_rng = 12345;
float rand01() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return ((g_rng >> 8) & 0xFFFF) / 65535.0f;
}

// spectrum
float g_levels[16];
float g_fft_in[512];

// Last marker at or before `frame`.
const Marker* marker_at(uint64_t frame) {
    for (auto it = g_markers.rbegin(); it != g_markers.rend(); ++it)
        if (it->frame <= frame) return &*it;
    return nullptr;
}

double pts_at(uint64_t frame) {
    const Marker* m = marker_at(frame);
    if (!m) return -1;
    return m->pts + (double)(frame - m->frame) * m->speed / RATE;
}

void fft(std::complex<float>* a, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2 * PI / len;
        std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1);
            for (int j = 0; j < len / 2; j++) {
                std::complex<float> u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

void analyze(const int16_t* stereo, int frames) {
    const int N = 512;
    int n = std::min(frames, N);
    // shift history and append
    std::memmove(g_fft_in, g_fft_in + n, (N - n) * sizeof(float));
    for (int i = 0; i < n; i++)
        g_fft_in[N - n + i] = (stereo[(frames - n + i) * 2] + stereo[(frames - n + i) * 2 + 1]) * (0.5f / 32768.0f);
    std::complex<float> buf[N];
    for (int i = 0; i < N; i++) {
        float w = 0.5f - 0.5f * std::cos(2 * PI * i / (N - 1));  // Hann
        buf[i] = g_fft_in[i] * w;
    }
    fft(buf, N);
    // 16 log-spaced bands between ~60 Hz and ~16 kHz
    for (int b = 0; b < 16; b++) {
        float f0 = 60.0f * std::pow(2.0f, b * 8.0f / 16.0f);
        float f1 = 60.0f * std::pow(2.0f, (b + 1) * 8.0f / 16.0f);
        int i0 = std::max(1, (int)(f0 * N / RATE)), i1 = std::max(i0 + 1, (int)(f1 * N / RATE));
        float e = 0;
        for (int i = i0; i < i1 && i < N / 2; i++) e = std::max(e, std::abs(buf[i]));
        float db = 20.0f * std::log10(e + 1e-6f);
        float v = std::clamp((db + 18.0f) / 42.0f, 0.0f, 1.0f);
        g_levels[b] = std::max(v, g_levels[b] * 0.82f);
    }
}

std::atomic<void (*)(const int16_t*, int)> g_tap{nullptr};

void callback(void*, Uint8* out_bytes, int len) {
    int16_t* out = (int16_t*)out_bytes;
    int frames = len / (int)(sizeof(int16_t) * CHANNELS);
    if (g_cpu_tag == -1) {
        int id = cpu::tag("sound"), none = -1;
        if (!g_cpu_tag.compare_exchange_strong(none, id)) cpu::untag(id);
    }
    std::fill(out, out + frames * CHANNELS, 0);

    {
        std::lock_guard<std::mutex> lk(g_m);
        g_cb_read_start = g_read;
        g_cb_time = SDL_GetPerformanceCounter();
        if (!g_paused) {
            uint64_t avail = g_write - g_read;
            int n = (int)std::min<uint64_t>(avail, frames);
            for (int i = 0; i < n; i++) {
                size_t idx = (size_t)((g_read + i) % RING_FRAMES) * 2;
                const float vol = g_volume * g_fade;
                out[i * 2] = (int16_t)(g_ring[idx] * vol);
                out[i * 2 + 1] = (int16_t)(g_ring[idx + 1] * vol);
            }
            if (n > 0) {
                g_read += n;
                g_clock_valid = true;
            }
            if (n < frames && !g_markers.empty()) g_underrun += (double)(frames - n) / RATE;
            while (g_markers.size() > 1 && g_markers[1].frame <= g_cb_read_start) g_markers.pop_front();
            if (n > 0) analyze(out, n);
        } else {
            for (auto& l : g_levels) l *= 0.85f;
        }

        if (g_sfx_on) {
            const int style = g_sfx_style;
            const float sv = g_sfx_vol * g_sfx_vol * (g_sfx_kids ? 0.708f : 1.0f) * (style == STYLE_SUBTLE ? SUBTLE_MAKEUP : 1.0f);
            for (int i = 0; i < frames; i++) {
                float acc = 0;
                for (auto& v : g_voices) {
                    if (v.sfx < 0) continue;
                    const std::vector<float>& s = g_sfx[style][v.sfx][v.variant];
                    if (v.pos < s.size()) acc += s[v.pos++] * v.vol;
                    if (v.pos >= s.size()) v.sfx = -1;
                }
                if (acc == 0) continue;
                // A soft clip: several sounds at once bend instead of cracking.
                const int smp = (int)(std::tanh(acc * sv) * 32767.0f);
                out[i * 2] = (int16_t)std::clamp(out[i * 2] + smp, -32768, 32767);
                out[i * 2 + 1] = (int16_t)std::clamp(out[i * 2 + 1] + smp, -32768, 32767);
            }
        }
    }
    if (auto tap = g_tap.load()) tap(out, frames);
}

// --- the interface sounds are made once, in both styles (audio/ui_sounds.cpp) ---------------

void build_sfx() {
    for (int style = 0; style < 2; style++)
        for (int s = 0; s < SFX_COUNT; s++)
            for (int v = 0; v < ui_sounds::variants((Sfx)s, (SoundStyle)style); v++)
                g_sfx[style][s][v] = ui_sounds::render((Sfx)s, (SoundStyle)style, v);
}

}  // namespace

bool init() {
    g_ring.assign(RING_FRAMES * CHANNELS, 0);
    build_sfx();
    SDL_AudioSpec want{}, have{};
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = CHANNELS;
    want.samples = CALLBACK_FRAMES;
    want.callback = callback;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) {
        log_message(LOG_ERROR, "Audio", "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }
    if (have.freq != RATE || have.channels != CHANNELS || have.format != AUDIO_S16SYS)
        log_message(LOG_WARNING, "Audio", "Got %d Hz, %d ch, fmt %x", have.freq, have.channels, have.format);
    SDL_PauseAudioDevice(g_dev, 0);
    log_message(LOG_OK, "Audio", "Output %d Hz, %d frames/callback", have.freq, have.samples);
    return true;
}

void set_tap(void (*tap)(const int16_t* frames, int count)) { g_tap = tap; }

void shutdown() {
    cpu::untag(g_cpu_tag.exchange(-2));
    if (g_dev) SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
}

size_t stream_write(const int16_t* frames, size_t count, double pts, uint32_t generation, float speed) {
    std::lock_guard<std::mutex> lk(g_m);
    if (generation != g_generation) return count;  // stale: swallow
    size_t free_frames = RING_FRAMES - (size_t)(g_write - g_read);
    size_t n = std::min(count, free_frames);
    if (n == 0) return 0;
    g_markers.push_back(Marker{g_write, pts, speed});
    for (size_t i = 0; i < n; i++) {
        size_t idx = (size_t)((g_write + i) % RING_FRAMES) * 2;
        g_ring[idx] = frames[i * 2];
        g_ring[idx + 1] = frames[i * 2 + 1];
    }
    g_write += n;
    return n;
}

double stream_cut(uint32_t generation, double keep_seconds) {
    std::lock_guard<std::mutex> lk(g_m);
    if (generation != g_generation || g_markers.empty()) return -1;
    uint64_t end = g_read + (uint64_t)(keep_seconds * RATE);
    if (end >= g_write) return pts_at(g_write);
    while (g_markers.size() > 1 && g_markers.back().frame >= end) g_markers.pop_back();
    double pts = pts_at(end);
    // A quick fade to silence where it now stops, so the join with what comes next doesn't click.
    constexpr uint64_t FADE = RATE / 400;  // 2.5 ms
    uint64_t from = std::max(g_read, end > FADE ? end - FADE : 0);
    for (uint64_t f = from; f < end; f++) {
        float gain = (float)(end - f) / (float)(end - from + 1);
        size_t idx = (size_t)(f % RING_FRAMES) * 2;
        g_ring[idx] = (int16_t)(g_ring[idx] * gain);
        g_ring[idx + 1] = (int16_t)(g_ring[idx + 1] * gain);
    }
    g_write = end;
    return pts;
}

size_t stream_free_frames() {
    std::lock_guard<std::mutex> lk(g_m);
    return RING_FRAMES - (size_t)(g_write - g_read);
}

double stream_buffered_seconds() {
    std::lock_guard<std::mutex> lk(g_m);
    return (double)(g_write - g_read) / RATE;
}

void stream_reset(uint32_t generation) {
    std::lock_guard<std::mutex> lk(g_m);
    g_generation = generation;
    g_read = g_write;
    g_markers.clear();
    g_clock_valid = false;
    g_underrun = 0;
}

void stream_clear() {
    std::lock_guard<std::mutex> lk(g_m);
    g_read = g_write;
    g_markers.clear();
    g_clock_valid = false;
    g_underrun = 0;
}

void stream_pause(bool paused) {
    std::lock_guard<std::mutex> lk(g_m);
    g_paused = paused;
}

bool stream_paused() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_paused;
}

double stream_clock() {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_clock_valid || g_markers.empty()) return -1;
    const Marker* m = marker_at(g_cb_read_start);
    if (!m) return -1;
    double base = m->pts + (double)(g_cb_read_start - m->frame) * m->speed / RATE;
    double played = (double)(g_read - g_cb_read_start) / RATE;
    double since = g_paused ? 0 : (double)(SDL_GetPerformanceCounter() - g_cb_time) / SDL_GetPerformanceFrequency();
    // The buffer handed to the device in the last callback is heard after the
    // one before it, so the audible position lags by about one period.
    return base + (std::min(since, played) - (double)CALLBACK_FRAMES / RATE) * m->speed;
}

double stream_take_underrun() {
    std::lock_guard<std::mutex> lk(g_m);
    double u = g_underrun;
    g_underrun = 0;
    return u;
}

void stream_set_volume(float v) {
    std::lock_guard<std::mutex> lk(g_m);
    g_volume = std::clamp(v, 0.0f, 1.0f);
}

void levels(float out[16]) {
    std::lock_guard<std::mutex> lk(g_m);
    std::copy(g_levels, g_levels + 16, out);
}

void play(Sfx s, float volume) {
    if (!g_sfx_on || s < 0 || s >= SFX_COUNT) return;
    const int style = g_sfx_style;
    if (s == SFX_MOVE && !g_sfx_ticks) return;
    std::lock_guard<std::mutex> lk(g_m);
    int variant = 0;
    float vol = volume;
    if (style == STYLE_SUBTLE) {
        const double now = (double)SDL_GetTicks() / 1000.0;
        if (s == SFX_MOVE) {  // thinned out when the button is held, and never quite the same twice
            float soften = 0;
            if (!g_tick_limiter.allow(now, &soften)) return;
            variant = (int)(rand01() * 2.999f);
            vol *= std::pow(10.0f, (soften + (rand01() * 3.0f - 1.5f)) / 20.0f);
        } else if (s == SFX_BUMP) {
            static double last_bump = -1;
            if (last_bump >= 0 && now - last_bump < 0.25) return;  // one thud at an edge, however long it is held against
            last_bump = now;
        }
    }
    if (g_sfx[style][s][variant].empty()) return;
    // At most three at once: the oldest gives way.
    int active = 0;
    Voice* free_slot = nullptr;
    Voice* oldest = nullptr;
    for (auto& v : g_voices) {
        if (v.sfx < 0) {
            if (!free_slot) free_slot = &v;
            continue;
        }
        active++;
        if (!oldest || v.started < oldest->started) oldest = &v;
    }
    Voice* slot = free_slot;
    if (active >= MAX_SFX_VOICES || !slot) slot = oldest ? oldest : &g_voices[0];
    slot->sfx = s;
    slot->variant = variant;
    slot->pos = 0;
    slot->vol = vol;
    slot->started = ++g_voice_counter;
}

void set_sfx_enabled(bool on) { g_sfx_on = on; }
void set_sfx_style(SoundStyle style) { g_sfx_style = style == STYLE_CLASSIC ? STYLE_CLASSIC : STYLE_SUBTLE; }
void set_sfx_volume(float v) { g_sfx_vol = std::clamp(v, 0.0f, 1.0f); }
void set_sfx_ticks(bool on) { g_sfx_ticks = on; }
void set_sfx_kids(bool on) { g_sfx_kids = on; }
void set_media_fade(float f) { g_fade = std::clamp(f, 0.0f, 1.0f); }
void set_media_volume(float v) { stream_set_volume(std::clamp(v, 0.0f, 1.0f) * std::clamp(v, 0.0f, 1.0f)); }

}  // namespace audio
