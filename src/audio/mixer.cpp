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

#include "logger/logger.hpp"

namespace audio {

namespace {

constexpr size_t RING_FRAMES = RATE * 2;  // 2 seconds
constexpr int CALLBACK_FRAMES = 1024;
constexpr int MAX_VOICES = 8;
constexpr float PI = 3.14159265358979f;

SDL_AudioDeviceID g_dev = 0;
std::mutex g_m;

// ring buffer (frames of 2 x int16)
std::vector<int16_t> g_ring;
uint64_t g_write = 0;  // total frames written
uint64_t g_read = 0;   // total frames consumed
struct Marker { uint64_t frame; double pts; };
std::deque<Marker> g_markers;
bool g_paused = true;
uint32_t g_generation = 0;
float g_volume = 1.0f;
double g_underrun = 0;

// clock interpolation between callbacks
uint64_t g_cb_read_start = 0;
Uint64 g_cb_time = 0;
bool g_clock_valid = false;

// sfx
std::vector<float> g_sfx[SFX_COUNT];
struct Voice { int sfx = -1; size_t pos = 0; float vol = 1; };
Voice g_voices[MAX_VOICES];
std::atomic<bool> g_sfx_on{true};
std::atomic<float> g_sfx_vol{0.55f};

// spectrum
float g_levels[16];
float g_fft_in[512];

double pts_at(uint64_t frame) {
    // Last marker at or before `frame`.
    const Marker* m = nullptr;
    for (auto it = g_markers.rbegin(); it != g_markers.rend(); ++it) {
        if (it->frame <= frame) {
            m = &*it;
            break;
        }
    }
    if (!m) return -1;
    return m->pts + (double)(frame - m->frame) / RATE;
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

void callback(void*, Uint8* out_bytes, int len) {
    int16_t* out = (int16_t*)out_bytes;
    int frames = len / (int)(sizeof(int16_t) * CHANNELS);
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
                out[i * 2] = (int16_t)(g_ring[idx] * g_volume);
                out[i * 2 + 1] = (int16_t)(g_ring[idx + 1] * g_volume);
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
            float sv = g_sfx_vol;
            for (auto& v : g_voices) {
                if (v.sfx < 0) continue;
                const std::vector<float>& s = g_sfx[v.sfx];
                for (int i = 0; i < frames && v.pos < s.size(); i++, v.pos++) {
                    int smp = (int)(s[v.pos] * v.vol * sv * 32767.0f);
                    out[i * 2] = (int16_t)std::clamp(out[i * 2] + smp, -32768, 32767);
                    out[i * 2 + 1] = (int16_t)std::clamp(out[i * 2 + 1] + smp, -32768, 32767);
                }
                if (v.pos >= s.size()) v.sfx = -1;
            }
        }
    }
}

// --- procedural UI sounds ------------------------------------------------------

std::vector<float> tone(float f0, float f1, float dur, float attack, float decay_pow, float gain,
                        float harmonics = 0.0f) {
    int n = (int)(dur * RATE);
    std::vector<float> s(n);
    float phase = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / n;
        float f = f0 + (f1 - f0) * t;
        phase += 2 * PI * f / RATE;
        float env = std::min(1.0f, (i / (float)RATE) / attack) * std::pow(1.0f - t, decay_pow);
        float v = std::sin(phase) + harmonics * std::sin(2 * phase) * 0.5f + harmonics * std::sin(3 * phase) * 0.25f;
        s[i] = v * env * gain;
    }
    return s;
}

void mix_into(std::vector<float>& dst, const std::vector<float>& src, float at_seconds) {
    size_t off = (size_t)(at_seconds * RATE);
    if (dst.size() < off + src.size()) dst.resize(off + src.size(), 0.0f);
    for (size_t i = 0; i < src.size(); i++) dst[off + i] += src[i];
}

void build_sfx() {
    g_sfx[SFX_MOVE] = tone(1850, 1500, 0.035f, 0.002f, 3.0f, 0.22f, 0.3f);
    g_sfx[SFX_SELECT] = tone(620, 640, 0.07f, 0.003f, 2.0f, 0.3f, 0.4f);
    mix_into(g_sfx[SFX_SELECT], tone(930, 960, 0.11f, 0.003f, 2.2f, 0.28f, 0.3f), 0.045f);
    g_sfx[SFX_BACK] = tone(760, 560, 0.075f, 0.003f, 2.0f, 0.28f, 0.3f);
    mix_into(g_sfx[SFX_BACK], tone(520, 420, 0.09f, 0.003f, 2.4f, 0.22f, 0.2f), 0.04f);
    g_sfx[SFX_BUMP] = tone(170, 110, 0.09f, 0.002f, 2.5f, 0.45f, 0.6f);
    g_sfx[SFX_TOGGLE] = tone(1200, 1300, 0.04f, 0.001f, 2.0f, 0.25f, 0.2f);
    g_sfx[SFX_ERROR] = tone(330, 300, 0.1f, 0.003f, 1.5f, 0.3f, 0.5f);
    mix_into(g_sfx[SFX_ERROR], tone(260, 240, 0.14f, 0.003f, 1.5f, 0.3f, 0.5f), 0.12f);
    // Small "whoosh up" arpeggio for opening a player/overlay.
    g_sfx[SFX_OPEN] = tone(523, 530, 0.12f, 0.004f, 2.0f, 0.18f, 0.2f);
    mix_into(g_sfx[SFX_OPEN], tone(659, 665, 0.12f, 0.004f, 2.0f, 0.16f, 0.2f), 0.05f);
    mix_into(g_sfx[SFX_OPEN], tone(784, 790, 0.18f, 0.004f, 2.2f, 0.14f, 0.2f), 0.1f);
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

void shutdown() {
    if (g_dev) SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
}

size_t stream_write(const int16_t* frames, size_t count, double pts, uint32_t generation) {
    std::lock_guard<std::mutex> lk(g_m);
    if (generation != g_generation) return count;  // stale: swallow
    size_t free_frames = RING_FRAMES - (size_t)(g_write - g_read);
    size_t n = std::min(count, free_frames);
    if (n == 0) return 0;
    g_markers.push_back(Marker{g_write, pts});
    for (size_t i = 0; i < n; i++) {
        size_t idx = (size_t)((g_write + i) % RING_FRAMES) * 2;
        g_ring[idx] = frames[i * 2];
        g_ring[idx + 1] = frames[i * 2 + 1];
    }
    g_write += n;
    return n;
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
    double base = pts_at(g_cb_read_start);
    if (base < 0) return -1;
    double played = (double)(g_read - g_cb_read_start) / RATE;
    double since = g_paused ? 0 : (double)(SDL_GetPerformanceCounter() - g_cb_time) / SDL_GetPerformanceFrequency();
    // The buffer handed to the device in the last callback is heard after the
    // one before it, so the audible position lags by about one period.
    return base + std::min(since, played) - (double)CALLBACK_FRAMES / RATE;
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
    std::lock_guard<std::mutex> lk(g_m);
    Voice* slot = &g_voices[0];
    for (auto& v : g_voices) {
        if (v.sfx < 0) { slot = &v; break; }
        if (v.pos > slot->pos) slot = &v;  // steal the oldest
    }
    slot->sfx = s;
    slot->pos = 0;
    slot->vol = volume;
}

void set_sfx_enabled(bool on) { g_sfx_on = on; }
void set_sfx_volume(float v) { g_sfx_vol = std::clamp(v, 0.0f, 1.0f); }

}  // namespace audio
