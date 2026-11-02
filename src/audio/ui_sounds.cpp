#include "audio/ui_sounds.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace audio::ui_sounds {

namespace {

constexpr float PI = 3.14159265358979f;
using Samples = std::vector<float>;

float db(float x) { return std::pow(10.0f, x / 20.0f); }

void mix_into(Samples& dst, const Samples& src, float at) {
    const size_t off = (size_t)(at * RATE);
    if (dst.size() < off + src.size()) dst.resize(off + src.size(), 0.0f);
    for (size_t i = 0; i < src.size(); i++) dst[off + i] += src[i];
}

// A fixed noise (the same every run, so the sounds are the same on every console and in the tests).
struct Noise {
    uint32_t s = 22222;
    float next() {
        s = s * 1664525u + 1013904223u;
        return ((s >> 8) & 0xFFFF) / 32768.0f - 1.0f;
    }
};

// --- Subtle ---------------------------------------------------------------------------------

struct Partial { float mult, gain, tau_mult; };

Samples sine_env(float f, float dur, float tau, float attack, float gain, float f_end, const std::vector<Partial>& partials) {
    const int n = (int)(dur * RATE);
    Samples s(n);
    std::vector<float> ph(partials.size(), 0.0f);
    for (int i = 0; i < n; i++) {
        const float t = (float)i / RATE;
        const float fi = f_end > 0 ? f * std::pow(f_end / f, t / dur) : f;
        float v = 0;
        for (size_t k = 0; k < partials.size(); k++) {
            ph[k] += 2 * PI * fi * partials[k].mult / RATE;
            v += partials[k].gain * std::sin(ph[k]) * std::exp(-t / (tau * partials[k].tau_mult));
        }
        s[i] = v * std::min(1.0f, t / attack) * gain;
    }
    return s;
}
Samples tone(float f, float dur, float tau, float attack, float gain = 1.0f) { return sine_env(f, dur, tau, attack, gain, 0, {{1, 1, 1}}); }

Samples noise_burst(float dur, float lp_hz, float gain, float attack, float tau) {
    const int n = (int)(dur * RATE);
    Samples s(n);
    Noise noise;
    const float a = std::exp(-2 * PI * lp_hz / RATE);
    float y = 0;
    for (int i = 0; i < n; i++) {
        y = (1 - a) * noise.next() + a * y;
        const float t = (float)i / RATE;
        s[i] = y * gain * std::exp(-t / tau) * std::min(1.0f, t / attack);
    }
    return s;
}

// The master of each sound: a one-pole lowpass (the sounds have nothing worth hearing above it, and the 2 to 5 kHz
// the ear is most sensitive to is kept down), scaled to its peak, a soft clip, the first millisecond faded in and
// the last five out, so it starts and ends at zero and cannot pop.
Samples finish(Samples s, float peak_db, float lp_hz = 4000.0f) {
    const float a = std::exp(-2 * PI * lp_hz / RATE);
    float y = 0, m = 1e-9f;
    for (float& v : s) {
        y = (1 - a) * v + a * y;
        v = y;
        m = std::max(m, std::fabs(v));
    }
    const float k = db(peak_db) / m;
    for (float& v : s) v = std::tanh(v * k);
    const size_t fade_in = std::min(s.size(), (size_t)(0.001f * RATE)), fade_out = std::min(s.size(), (size_t)(0.005f * RATE));
    for (size_t i = 0; i < fade_in; i++) s[i] *= (float)i / fade_in;
    for (size_t i = 0; i < fade_out; i++) s[s.size() - 1 - i] *= (float)i / fade_out;
    if (!s.empty()) {
        s[0] = 0;
        s[s.size() - 1] = 0;
    }
    return s;
}

constexpr float A4 = 440.0f, FS4 = 370.0f, D5 = 587.3f, E5 = 659.3f, A5 = 880.0f, B5 = 987.8f, D6 = 1174.7f, E6 = 1318.5f, FS6 = 1480.0f, A6 = 1760.0f;

Samples subtle_move(int variant) {
    static const float cents[] = {-12, 0, 12};
    const float f = A5 * std::pow(2.0f, cents[std::clamp(variant, 0, 2)] / 1200.0f);
    return finish(sine_env(f, 0.045f, 0.009f, 0.001f, 1.0f, 0, {{1, 1, 1}, {3.9f, 0.07f, 0.6f}}), -26);  // wood, with a little click
}
Samples bell(float f, float gain) { return sine_env(f, 0.22f, 0.07f, 0.0015f, gain, 0, {{1, 1, 1}, {2.0f, 0.25f, 0.57f}, {2.76f, 0.10f, 0.29f}}); }
Samples subtle_select() {
    Samples s = bell(A5, 1.0f);
    mix_into(s, bell(E6, 0.8f), 0.05f);  // up a fifth
    return finish(s, -18);
}
Samples subtle_back() {
    Samples s = sine_env(A5, 0.12f, 0.035f, 0.0015f, 1.0f, D5, {{1, 1, 1}});  // down a fifth, the answer to select
    return finish(s, -20, 2500);
}
Samples subtle_bump() {
    Samples s = sine_env(260, 0.09f, 0.022f, 0.001f, 1.0f, 170, {{1, 1, 1}});
    mix_into(s, noise_burst(0.006f, 1200, 0.3f, 0.0005f, 0.002f), 0);  // body, for speakers that give nothing below 300 Hz
    return finish(s, -20, 3000);
}
Samples subtle_toggle(bool on) {
    const float first = on ? D5 : A5, second = on ? A5 : D5, gain = on ? 1.0f : db(-3);
    Samples s = tone(first, 0.04f, 0.010f, 0.001f, gain);
    mix_into(s, tone(second, 0.04f, 0.010f, 0.001f, gain), 0.035f);
    return finish(s, on ? -22 : -25);
}
Samples subtle_error() {  // a minor third down: not a buzz, nothing a child would find sharp
    const std::vector<Partial> p = {{1, 1, 1}, {2.0f, 0.3f, 0.5f}};
    Samples s = sine_env(A4, 0.13f, 0.05f, 0.008f, 1.0f, 0, p);
    mix_into(s, sine_env(FS4, 0.16f, 0.05f, 0.008f, 1.0f, 0, p), 0.1f);
    return finish(s, -20, 3000);
}
Samples subtle_open() {  // a breath of air and a small bell
    const int n = (int)(0.28f * RATE);
    Samples s(n);
    Noise noise;
    const float a = std::exp(-2 * PI * 1500 / RATE);
    float y = 0, ph = 0;
    for (int i = 0; i < n; i++) {
        const float t = (float)i / RATE;
        const float fc = 600 + 1400 * std::min(1.0f, t / 0.15f);
        y = (1 - a) * noise.next() + a * y;
        ph += 2 * PI * fc / RATE;
        s[i] = y * std::sin(ph) * 3.0f * std::min(1.0f, t / 0.04f) * std::exp(-std::max(0.0f, t - 0.04f) / 0.08f) * db(-18);
    }
    mix_into(s, tone(D6, 0.28f, 0.1f, 0.002f), 0.06f);
    return finish(s, -22);
}
Samples subtle_sparkle() {  // a birthday: five notes up
    const float notes[] = {A5, B5, D6, FS6, A6};
    Samples s;
    for (int i = 0; i < 5; i++) mix_into(s, tone(notes[i], 0.25f, 0.08f, 0.0015f, 1.0f - 0.125f * i), i * 0.045f);
    return finish(s, -24);
}
Samples subtle_shuffle() {  // Surprise me: a few quick ticks that slow down, then the bell
    static const float notes[] = {A4, D5, A5, E5, FS4, D5, A5};
    Samples s;
    float t = 0, gap = 0.05f;
    for (int i = 0; i < 7; i++) {
        mix_into(s, tone(notes[i], 0.04f, 0.008f, 0.001f), t);
        t += gap;
        gap = i < 3 ? std::max(0.025f, gap - 0.004f) : gap + 0.008f;
    }
    mix_into(s, subtle_select(), t + 0.03f);
    return finish(s, -24);
}

// --- Classic: the beeps CoffeeFlix had -------------------------------------------------------

Samples old_tone(float f0, float f1, float dur, float attack, float decay_pow, float gain, float harmonics = 0.0f) {
    const int n = (int)(dur * RATE);
    Samples s(n);
    float phase = 0;
    for (int i = 0; i < n; i++) {
        const float t = (float)i / n;
        phase += 2 * PI * (f0 + (f1 - f0) * t) / RATE;
        const float env = std::min(1.0f, (i / (float)RATE) / attack) * std::pow(1.0f - t, decay_pow);
        s[i] = (std::sin(phase) + harmonics * std::sin(2 * phase) * 0.5f + harmonics * std::sin(3 * phase) * 0.25f) * env * gain;
    }
    return s;
}

Samples classic(Sfx sfx) {
    Samples s;
    switch (sfx) {
        case SFX_MOVE: return old_tone(1850, 1500, 0.035f, 0.002f, 3.0f, 0.22f, 0.3f);
        case SFX_SELECT:
        case SFX_SHUFFLE:
            s = old_tone(620, 640, 0.07f, 0.003f, 2.0f, 0.3f, 0.4f);
            mix_into(s, old_tone(930, 960, 0.11f, 0.003f, 2.2f, 0.28f, 0.3f), 0.045f);
            return s;
        case SFX_BACK:
            s = old_tone(760, 560, 0.075f, 0.003f, 2.0f, 0.28f, 0.3f);
            mix_into(s, old_tone(520, 420, 0.09f, 0.003f, 2.4f, 0.22f, 0.2f), 0.04f);
            return s;
        case SFX_BUMP: return old_tone(170, 110, 0.09f, 0.002f, 2.5f, 0.45f, 0.6f);
        case SFX_TOGGLE:
        case SFX_TOGGLE_OFF: return old_tone(1200, 1300, 0.04f, 0.001f, 2.0f, 0.25f, 0.2f);
        case SFX_ERROR:
            s = old_tone(330, 300, 0.1f, 0.003f, 1.5f, 0.3f, 0.5f);
            mix_into(s, old_tone(260, 240, 0.14f, 0.003f, 1.5f, 0.3f, 0.5f), 0.12f);
            return s;
        case SFX_OPEN:
        case SFX_SPARKLE:
        default:
            s = old_tone(523, 530, 0.12f, 0.004f, 2.0f, 0.18f, 0.2f);
            mix_into(s, old_tone(659, 665, 0.12f, 0.004f, 2.0f, 0.16f, 0.2f), 0.05f);
            mix_into(s, old_tone(784, 790, 0.18f, 0.004f, 2.2f, 0.14f, 0.2f), 0.1f);
            return s;
    }
}

}  // namespace

int variants(Sfx s, SoundStyle style) { return style == STYLE_SUBTLE && s == SFX_MOVE ? 3 : 1; }

std::vector<float> render(Sfx s, SoundStyle style, int variant) {
    if (style == STYLE_CLASSIC) return classic(s);
    switch (s) {
        case SFX_MOVE: return subtle_move(variant);
        case SFX_SELECT: return subtle_select();
        case SFX_BACK: return subtle_back();
        case SFX_BUMP: return subtle_bump();
        case SFX_TOGGLE: return subtle_toggle(true);
        case SFX_TOGGLE_OFF: return subtle_toggle(false);
        case SFX_ERROR: return subtle_error();
        case SFX_OPEN: return subtle_open();
        case SFX_SPARKLE: return subtle_sparkle();
        case SFX_SHUFFLE: return subtle_shuffle();
        default: return {};
    }
}

bool TickLimiter::allow(double now, float* gain_db) {
    *gain_db = 0;
    // The attempts in the last second tell how long the button has been held.
    int recent = 0;
    for (int i = 0; i < count_; i++)
        if (now - attempts_[i] <= 1.0) attempts_[recent++] = attempts_[i];
    count_ = recent;
    if (count_ < MAX) attempts_[count_++] = now;
    const long n = total_++;
    if (last_played_ >= 0 && now - last_played_ < 0.06) return false;  // never closer than 60 ms
    bool play = true;
    if (count_ > 10) {  // a held button: every third tick, quieter still
        play = n % 3 == 0;
        *gain_db = -6;
    } else if (count_ > 6) {  // every other tick
        play = n % 2 == 0;
        *gain_db = -3;
    }
    if (play) last_played_ = now;
    return play;
}

}  // namespace audio::ui_sounds
