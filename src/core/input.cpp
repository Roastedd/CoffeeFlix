#include "core/input.hpp"

#include <cmath>

namespace {

constexpr float REPEAT_DELAY = 0.36f;
constexpr float REPEAT_RATE = 0.085f;
constexpr float REPEAT_RATE_FAST = 0.04f;
constexpr float FAST_AFTER = 1.4f;
constexpr float DRAG_THRESHOLD = 14.0f;

float g_hold_time[BTN_COUNT];
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
    in.touch_began = raw.touch && !in.touching;
    in.touch_ended = !raw.touch && in.touching;
    in.tap = false;
    if (in.touch_began) {
        in.touch_start_x = in.tx = raw.tx;
        in.touch_start_y = in.ty = raw.ty;
        in.tdx = in.tdy = 0;
        in.dragging = false;
    } else if (raw.touch) {
        in.tdx = raw.tx - in.tx;
        in.tdy = raw.ty - in.ty;
        in.tx = raw.tx;
        in.ty = raw.ty;
        if (std::fabs(in.tx - in.touch_start_x) > DRAG_THRESHOLD || std::fabs(in.ty - in.touch_start_y) > DRAG_THRESHOLD)
            in.dragging = true;
    } else {
        in.tdx = in.tdy = 0;
    }
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
