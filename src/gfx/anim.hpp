// Small animation toolkit: easing curves, exponential smoothing and springs.
#pragma once

#include <cmath>
#include <algorithm>

namespace anim {

constexpr float PI = 3.14159265358979f;

inline float clamp01(float t) { return std::clamp(t, 0.0f, 1.0f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

inline float ease_out_cubic(float t) { t = clamp01(t); float u = 1 - t; return 1 - u * u * u; }
inline float ease_in_out_cubic(float t) {
    t = clamp01(t);
    return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
}
inline float ease_out_back(float t, float s = 1.70158f) {
    t = clamp01(t);
    float u = t - 1;
    return 1 + (s + 1) * u * u * u + s * u * u;
}
inline float ease_out_expo(float t) { t = clamp01(t); return t >= 1 ? 1 : 1 - std::pow(2.0f, -10 * t); }
inline float smoothstep(float t) { t = clamp01(t); return t * t * (3 - 2 * t); }

// Frame-rate independent exponential approach. `speed` ~ 1/time-constant.
inline float approach(float current, float target, float speed, float dt) {
    return target + (current - target) * std::exp(-speed * dt);
}

// Damped spring; stiffness/damping tuned for UI (slight overshoot at defaults).
struct Spring {
    float x = 0, v = 0;

    void update(float target, float dt, float stiffness = 260.0f, float damping = 22.0f) {
        // Sub-step for stability with large dt (e.g. frame hitches).
        int steps = std::max(1, (int)std::ceil(dt / (1.0f / 120.0f)));
        float h = dt / steps;
        for (int i = 0; i < steps; i++) {
            float a = -stiffness * (x - target) - damping * v;
            v += a * h;
            x += v * h;
        }
    }
    bool settled(float target, float eps = 0.001f) const { return std::fabs(x - target) < eps && std::fabs(v) < eps; }
};

}  // namespace anim
