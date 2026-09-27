#include "core/input.hpp"

#include <cmath>

namespace {

constexpr float REPEAT_DELAY = 0.36f;
constexpr float REPEAT_RATE = 0.085f;
constexpr float REPEAT_RATE_FAST = 0.04f;
constexpr float FAST_AFTER = 1.4f;
constexpr float DRAG_THRESHOLD = 14.0f;

float g_hold_time[BTN_COUNT];

// Recent touch samples, to measure how fast the finger was moving when it let go.
struct TouchSample { double t; float x, y; };
constexpr int SAMPLES = 8;
TouchSample g_samples[SAMPLES];
int g_sample_count = 0;
double g_clock = 0;

void add_sample(float x, float y) {
    for (int i = SAMPLES - 1; i > 0; i--) g_samples[i] = g_samples[i - 1];
    g_samples[0] = TouchSample{g_clock, x, y};
    if (g_sample_count < SAMPLES) g_sample_count++;
}

// Velocity over the last ~80 ms: a finger that stopped before lifting doesn't fling.
void touch_velocity(float& vx, float& vy) {
    vx = vy = 0;
    if (g_sample_count < 2) return;
    const TouchSample& now = g_samples[0];
    int j = 1;
    while (j < g_sample_count - 1 && now.t - g_samples[j].t < 0.08) j++;
    double span = now.t - g_samples[j].t;
    if (span <= 0.001 || g_clock - now.t > 0.05) return;
    vx = (float)((now.x - g_samples[j].x) / span);
    vy = (float)((now.y - g_samples[j].y) / span);
}
float g_next_repeat[BTN_COUNT];
uint32_t g_stick_dirs = 0;

uint32_t stick_to_dirs(float x, float y, uint32_t prev) {
    // Hysteresis so a stick resting near the threshold doesn't flicker.
    auto on = [&](Button b, float v) {
        bool was = prev & bit(b);
        return v > (was ? 0.35f : 0.6f);
    };
    uint32_t d = 0;
    // Only the dominant axis to avoid diagonal double moves.
    if (std::fabs(x) > std::fabs(y)) {
        if (on(BTN_RIGHT, x)) d |= bit(BTN_RIGHT);
        if (on(BTN_LEFT, -x)) d |= bit(BTN_LEFT);
    } else {
        if (on(BTN_UP, y)) d |= bit(BTN_UP);
        if (on(BTN_DOWN, -y)) d |= bit(BTN_DOWN);
    }
    return d;
}

}  // namespace

void input_update(Input& in, const RawInput& raw, float dt) {
    g_stick_dirs = stick_to_dirs(raw.lx, raw.ly, g_stick_dirs);
    uint32_t held = raw.held | g_stick_dirs;

    in.pressed = held & ~in.held;
    in.released = in.held & ~held;
    in.held = held;
    in.repeat = in.pressed;

    const Button repeatable[] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_L, BTN_R, BTN_ZL, BTN_ZR};
    for (Button b : repeatable) {
        if (!(held & bit(b))) {
            g_hold_time[b] = 0;
            continue;
        }
        if (in.pressed & bit(b)) {
            g_hold_time[b] = 0;
            g_next_repeat[b] = REPEAT_DELAY;
            continue;
        }
        g_hold_time[b] += dt;
        if (g_hold_time[b] >= g_next_repeat[b]) {
            in.repeat |= bit(b);
            g_next_repeat[b] += g_hold_time[b] > FAST_AFTER ? REPEAT_RATE_FAST : REPEAT_RATE;
        }
    }

    in.lx = raw.lx; in.ly = raw.ly; in.rx = raw.rx; in.ry = raw.ry;

    // Touch
    g_clock += dt;
    in.touch_began = raw.touch && !in.touching;
    in.touch_ended = !raw.touch && in.touching;
    in.tap = false;
    in.drag_began = false;
    if (in.touch_began) {
        in.touch_start_x = in.tx = raw.tx;
        in.touch_start_y = in.ty = raw.ty;
        in.tdx = in.tdy = 0;
        in.dragging = false;
        in.drag_axis = 0;
        g_sample_count = 0;
        add_sample(raw.tx, raw.ty);
    } else if (raw.touch) {
        in.tdx = raw.tx - in.tx;
        in.tdy = raw.ty - in.ty;
        in.tx = raw.tx;
        in.ty = raw.ty;
        add_sample(raw.tx, raw.ty);
        float mx = std::fabs(in.tx - in.touch_start_x), my = std::fabs(in.ty - in.touch_start_y);
        if (!in.dragging && (mx > DRAG_THRESHOLD || my > DRAG_THRESHOLD)) {
            in.dragging = true;
            in.drag_began = true;
            in.drag_axis = mx > my ? AXIS_X : AXIS_Y;
        }
    } else {
        in.tdx = in.tdy = 0;
    }
    if (raw.touch || in.touch_ended) touch_velocity(in.tvx, in.tvy);
    if (in.touch_ended && !in.dragging) in.tap = true;
    in.touching = raw.touch;

    // Pointer
    in.pointer_moved = raw.pointer && (!in.pointer || std::fabs(raw.px - in.px) > 0.5f || std::fabs(raw.py - in.py) > 0.5f);
    in.pointer = raw.pointer;
    in.px = raw.px;
    in.py = raw.py;
    in.wheel = raw.wheel;

    bool active = in.pressed || raw.touch || in.pointer_moved || std::fabs(raw.lx) > 0.3f || std::fabs(raw.ly) > 0.3f ||
                  std::fabs(raw.rx) > 0.3f || std::fabs(raw.ry) > 0.3f || raw.wheel != 0;
    in.idle_time = active ? 0 : in.idle_time + dt;
}
