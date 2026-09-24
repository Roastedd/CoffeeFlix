// Controller-agnostic input: every pad (GamePad, Pro, Classic, Wii Remote,
// keyboard on desktop) is folded into one button set, with auto-repeat for
// navigation and the left stick acting as a second D-pad.
#pragma once

#include <cstdint>

enum Button : uint32_t {
    BTN_A, BTN_B, BTN_X, BTN_Y,
    BTN_PLUS, BTN_MINUS,
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_L, BTN_R, BTN_ZL, BTN_ZR,
    BTN_STICK_L, BTN_STICK_R,
    BTN_COUNT
};

constexpr uint32_t bit(Button b) { return 1u << b; }

struct RawInput {
    uint32_t held = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;   // -1..1, +y = up
    bool touch = false;
    float tx = 0, ty = 0;                    // logical 1280x720
    bool pointer = false;                    // Wii Remote IR / mouse
    float px = 0, py = 0;
    float wheel = 0;                         // desktop mouse wheel
};

struct Input {
    uint32_t held = 0, pressed = 0, released = 0, repeat = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;

    // touch (GamePad screen) / mouse
    bool touching = false, touch_began = false, touch_ended = false;
    bool tap = false;           // released without dragging
    bool dragging = false;
    float tx = 0, ty = 0, tdx = 0, tdy = 0, touch_start_x = 0, touch_start_y = 0;

    bool pointer = false;       // pointer visible
    bool pointer_moved = false;
    float px = 0, py = 0;
    float wheel = 0;

    double idle_time = 0;       // seconds since any input

    bool down(Button b) const { return held & bit(b); }
    bool pressed_(Button b) const { return pressed & bit(b); }
    // pressed this frame or auto-repeating (for navigation)
    bool rep(Button b) const { return repeat & bit(b); }
    bool any() const { return pressed || touch_began || pointer_moved; }

    // Consume a button so later handlers in the same frame don't see it.
    void eat(Button b) { pressed &= ~bit(b); repeat &= ~bit(b); }
    void eat_all() { pressed = 0; repeat = 0; tap = false; touch_began = false; }
};

void input_update(Input& in, const RawInput& raw, float dt);
