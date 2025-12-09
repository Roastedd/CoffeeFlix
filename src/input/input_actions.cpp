#include <cstdio>
#include <cmath>

#include <vpad/input.h>
#include <padscore/wpad.h>

#include "logger/logger.hpp"

#include "input/input_actions.hpp"

static WPADExtensionType s_current_wpad_type = WPAD_EXT_DEV_NOT_FOUND;
static bool wpad_init = false;
static uint32_t s_last_wpad_buttons = 0;  // Track last frame's buttons for trigger detection

void input_poll(InputState& state) {
    static uint64_t last_buttons = 0;
    static bool last_touch = false;
    static float last_touch_x = 0.0f, last_touch_y = 0.0f;

    if (!wpad_init) {
        WPADInit();
        WPADEnableURCC(true);
        wpad_init = true;
    }

    VPADStatus vpad {};
    WPADStatusProController wpad_pro {};
    WPADStatusNunchuk wpad_nunchuk {};
    WPADStatus wpad_core {};
    VPADTouchData touch_calibrated {};

    state.pressed = 0;
    state.held = 0;
    state.left_stick = StickState{};
    state.right_stick = StickState{};
    state.touch.move_x = 0.0f;
    state.touch.move_y = 0.0f;
    state.using_pro_controller = false;

    WPADExtensionType extType;
    bool wpad_connected = (WPADProbe(WPAD_CHAN_0, &extType) == 0);
    
    // Handle controller connection/disconnection logging
    if (wpad_connected && extType != s_current_wpad_type) {
        switch (extType) {
            case WPAD_EXT_PRO_CONTROLLER:
                log_message(LOG_OK, "Input", "Pro Controller connected");
                WPADSetDataFormat(WPAD_CHAN_0, WPAD_FMT_PRO_CONTROLLER);
                break;
            case WPAD_EXT_NUNCHUK:
                log_message(LOG_OK, "Input", "Wii Remote + Nunchuk connected");
                WPADSetDataFormat(WPAD_CHAN_0, WPAD_FMT_NUNCHUK);
                break;
            case WPAD_EXT_CORE:
                log_message(LOG_OK, "Input", "Wii Remote connected");
                WPADSetDataFormat(WPAD_CHAN_0, WPAD_FMT_CORE);
                break;
            default:
                break;
        }
        s_current_wpad_type = extType;
    } else if (!wpad_connected && s_current_wpad_type != WPAD_EXT_DEV_NOT_FOUND) {
        log_message(LOG_OK, "Input", "WPAD controller disconnected");
        s_current_wpad_type = WPAD_EXT_DEV_NOT_FOUND;
    }

    // Read Pro Controller
    if (wpad_connected && extType == WPAD_EXT_PRO_CONTROLLER) {
        state.using_pro_controller = true;
        WPADRead(WPAD_CHAN_0, &wpad_pro.core);
        
        // Calculate trigger (buttons pressed this frame but not last frame)
        uint32_t wpad_trigger = wpad_pro.buttons & ~s_last_wpad_buttons;

        // Use trigger for single-shot button presses
        if (wpad_trigger & WPAD_PRO_BUTTON_A) set_button(state, BTN_A);
        if (wpad_trigger & WPAD_PRO_BUTTON_B) set_button(state, BTN_B);
        if (wpad_trigger & WPAD_PRO_BUTTON_X) set_button(state, BTN_X);
        if (wpad_trigger & WPAD_PRO_BUTTON_Y) set_button(state, BTN_Y);
        if (wpad_trigger & WPAD_PRO_BUTTON_PLUS) set_button(state, BTN_PLUS);
        if (wpad_trigger & WPAD_PRO_BUTTON_MINUS) set_button(state, BTN_MINUS);
        if (wpad_trigger & WPAD_PRO_BUTTON_LEFT) set_button(state, BTN_LEFT);
        if (wpad_trigger & WPAD_PRO_BUTTON_RIGHT) set_button(state, BTN_RIGHT);
        if (wpad_trigger & WPAD_PRO_BUTTON_UP) set_button(state, BTN_UP);
        if (wpad_trigger & WPAD_PRO_BUTTON_DOWN) set_button(state, BTN_DOWN);
        if (wpad_trigger & WPAD_PRO_BUTTON_L) set_button(state, BTN_L);
        if (wpad_trigger & WPAD_PRO_BUTTON_ZL) set_button(state, BTN_ZL);
        if (wpad_trigger & WPAD_PRO_BUTTON_R) set_button(state, BTN_R);
        if (wpad_trigger & WPAD_PRO_BUTTON_ZR) set_button(state, BTN_ZR);
        
        // Use hold for continuous actions
        if (wpad_pro.buttons & WPAD_PRO_BUTTON_A) set_hold(state, BTN_A);
        if (wpad_pro.buttons & WPAD_PRO_BUTTON_B) set_hold(state, BTN_B);
        if (wpad_pro.buttons & WPAD_PRO_BUTTON_X) set_hold(state, BTN_X);
        if (wpad_pro.buttons & WPAD_PRO_BUTTON_Y) set_hold(state, BTN_Y);
        if (wpad_pro.buttons & WPAD_PRO_BUTTON_LEFT) set_hold(state, BTN_LEFT);
        if (wpad_pro.buttons & WPAD_PRO_BUTTON_RIGHT) set_hold(state, BTN_RIGHT);
        if (wpad_pro.buttons & WPAD_PRO_BUTTON_UP) set_hold(state, BTN_UP);
        if (wpad_pro.buttons & WPAD_PRO_BUTTON_DOWN) set_hold(state, BTN_DOWN);
        
        s_last_wpad_buttons = wpad_pro.buttons;

        state.left_stick.x  = wpad_pro.leftStick.x;
        state.left_stick.y  = wpad_pro.leftStick.y;
        state.right_stick.x = wpad_pro.rightStick.x;
        state.right_stick.y = wpad_pro.rightStick.y;

    } else if (wpad_connected && extType == WPAD_EXT_NUNCHUK) {
        // Read Wii Remote with Nunchuk

        WPADRead(WPAD_CHAN_0, &wpad_nunchuk.core);
        
        // Calculate trigger (buttons pressed this frame but not last frame)
        uint32_t wpad_trigger = wpad_nunchuk.core.buttons & ~s_last_wpad_buttons;

        // Map Wii Remote buttons - use trigger for presses
        if (wpad_trigger & WPAD_BUTTON_A) set_button(state, BTN_A);
        if (wpad_trigger & WPAD_BUTTON_B) set_button(state, BTN_B);
        if (wpad_trigger & WPAD_BUTTON_1) set_button(state, BTN_Y);
        if (wpad_trigger & WPAD_BUTTON_2) set_button(state, BTN_X);
        if (wpad_trigger & WPAD_BUTTON_PLUS) set_button(state, BTN_PLUS);
        if (wpad_trigger & WPAD_BUTTON_MINUS) set_button(state, BTN_MINUS);
        if (wpad_trigger & WPAD_BUTTON_LEFT) set_button(state, BTN_LEFT);
        if (wpad_trigger & WPAD_BUTTON_RIGHT) set_button(state, BTN_RIGHT);
        if (wpad_trigger & WPAD_BUTTON_UP) set_button(state, BTN_UP);
        if (wpad_trigger & WPAD_BUTTON_DOWN) set_button(state, BTN_DOWN);

        // Map Nunchuk buttons (Z and C)
        if (wpad_trigger & WPAD_BUTTON_Z) set_button(state, BTN_ZL);
        if (wpad_trigger & WPAD_BUTTON_C) set_button(state, BTN_L);
        
        // Hold state for continuous actions
        if (wpad_nunchuk.core.buttons & WPAD_BUTTON_LEFT) set_hold(state, BTN_LEFT);
        if (wpad_nunchuk.core.buttons & WPAD_BUTTON_RIGHT) set_hold(state, BTN_RIGHT);
        if (wpad_nunchuk.core.buttons & WPAD_BUTTON_UP) set_hold(state, BTN_UP);
        if (wpad_nunchuk.core.buttons & WPAD_BUTTON_DOWN) set_hold(state, BTN_DOWN);
        
        s_last_wpad_buttons = wpad_nunchuk.core.buttons;

        // Map Nunchuk analog stick to left stick (range is -128 to 127)
        state.left_stick.x = wpad_nunchuk.stick.x / 128.0f;
        state.left_stick.y = wpad_nunchuk.stick.y / 128.0f;

    } else if (wpad_connected && extType == WPAD_EXT_CORE) {
        // Read basic Wii Remote (no extension)

        WPADRead(WPAD_CHAN_0, &wpad_core);
        
        // Calculate trigger
        uint32_t wpad_trigger = wpad_core.buttons & ~s_last_wpad_buttons;

        // Map Wii Remote buttons - use trigger for presses
        if (wpad_trigger & WPAD_BUTTON_A) set_button(state, BTN_A);
        if (wpad_trigger & WPAD_BUTTON_B) set_button(state, BTN_B);
        if (wpad_trigger & WPAD_BUTTON_1) set_button(state, BTN_Y);
        if (wpad_trigger & WPAD_BUTTON_2) set_button(state, BTN_X);
        if (wpad_trigger & WPAD_BUTTON_PLUS) set_button(state, BTN_PLUS);
        if (wpad_trigger & WPAD_BUTTON_MINUS) set_button(state, BTN_MINUS);
        if (wpad_trigger & WPAD_BUTTON_LEFT) set_button(state, BTN_LEFT);
        if (wpad_trigger & WPAD_BUTTON_RIGHT) set_button(state, BTN_RIGHT);
        if (wpad_trigger & WPAD_BUTTON_UP) set_button(state, BTN_UP);
        if (wpad_trigger & WPAD_BUTTON_DOWN) set_button(state, BTN_DOWN);
        
        // Hold state for continuous actions
        if (wpad_core.buttons & WPAD_BUTTON_LEFT) set_hold(state, BTN_LEFT);
        if (wpad_core.buttons & WPAD_BUTTON_RIGHT) set_hold(state, BTN_RIGHT);
        if (wpad_core.buttons & WPAD_BUTTON_UP) set_hold(state, BTN_UP);
        if (wpad_core.buttons & WPAD_BUTTON_DOWN) set_hold(state, BTN_DOWN);
        
        s_last_wpad_buttons = wpad_core.buttons;
    }

    // Always read GamePad (works alongside WPAD controllers)
    if (VPADRead(VPAD_CHAN_0, &vpad, 1, nullptr)) {
        // Use trigger for single-shot button presses (crucial for navigation)
        if (vpad.trigger & VPAD_BUTTON_A) set_button(state, BTN_A);
        if (vpad.trigger & VPAD_BUTTON_B) set_button(state, BTN_B);
        if (vpad.trigger & VPAD_BUTTON_X) set_button(state, BTN_X);
        if (vpad.trigger & VPAD_BUTTON_Y) set_button(state, BTN_Y);
        if (vpad.trigger & VPAD_BUTTON_PLUS) set_button(state, BTN_PLUS);
        if (vpad.trigger & VPAD_BUTTON_MINUS) set_button(state, BTN_MINUS);
        if (vpad.trigger & VPAD_BUTTON_LEFT) set_button(state, BTN_LEFT);
        if (vpad.trigger & VPAD_BUTTON_RIGHT) set_button(state, BTN_RIGHT);
        if (vpad.trigger & VPAD_BUTTON_UP) set_button(state, BTN_UP);
        if (vpad.trigger & VPAD_BUTTON_DOWN) set_button(state, BTN_DOWN);
        if (vpad.trigger & VPAD_BUTTON_L) set_button(state, BTN_L);
        if (vpad.trigger & VPAD_BUTTON_ZL) set_button(state, BTN_ZL);
        if (vpad.trigger & VPAD_BUTTON_R) set_button(state, BTN_R);
        if (vpad.trigger & VPAD_BUTTON_ZR) set_button(state, BTN_ZR);
        
        // Use hold for continuous actions (scrolling, seeking, etc.)
        if (vpad.hold & VPAD_BUTTON_LEFT) set_hold(state, BTN_LEFT);
        if (vpad.hold & VPAD_BUTTON_RIGHT) set_hold(state, BTN_RIGHT);
        if (vpad.hold & VPAD_BUTTON_UP) set_hold(state, BTN_UP);
        if (vpad.hold & VPAD_BUTTON_DOWN) set_hold(state, BTN_DOWN);
        if (vpad.hold & VPAD_BUTTON_L) set_hold(state, BTN_L);
        if (vpad.hold & VPAD_BUTTON_R) set_hold(state, BTN_R);

        state.left_stick.x  = vpad.leftStick.x;
        state.left_stick.y  = vpad.leftStick.y;
        state.right_stick.x = vpad.rightStick.x;
        state.right_stick.y = vpad.rightStick.y;

        VPADGetTPCalibratedPoint(VPAD_CHAN_0, &touch_calibrated, &vpad.tpNormal);

        if (touch_calibrated.touched) {
            float tx = (float)touch_calibrated.x;
            float ty = (float)touch_calibrated.y;

            if (!state.touch.touched) {
                state.touch.x = state.touch.old_x = tx;
                state.touch.y = state.touch.old_y = ty;
                state.touch.move_x = 0.0f;
                state.touch.move_y = 0.0f;
            } else {
                state.touch.old_x = state.touch.x;
                state.touch.old_y = state.touch.y;
                state.touch.x = tx;
                state.touch.y = ty;
                state.touch.move_x = state.touch.x - state.touch.old_x;
                state.touch.move_y = state.touch.y - state.touch.old_y;
            }
            state.touch.touched = true;
        } else {
            state.touch.touched = false;
            state.touch.move_x = 0.0f;
            state.touch.move_y = 0.0f;
        }
    }

#ifdef DEBUG
    uint64_t changed = state.pressed ^ last_buttons;
    if (changed) {
        for (int i = 0; i < 64; ++i) {
            if (changed & (1ull << i)) {
                bool now = state.pressed & (1ull << i);
                log_message(LOG_DEBUG, "Input", "Button %d %s", i, now ? "pressed" : "released");
            }
        }
        last_buttons = state.pressed;
    }

    auto log_stick = [&](const char* name, const StickState& stick) {
        if (fabs(stick.x) > 0.15f || fabs(stick.y) > 0.15f)
            log_message(LOG_DEBUG, "Input", "%s Stick: X=%.2f Y=%.2f", name, stick.x, stick.y);
    };
    log_stick("Left", state.left_stick);
    log_stick("Right", state.right_stick);

    if (state.touch.touched && !last_touch) {
        log_message(LOG_DEBUG, "Input", "Touch START at (%.1f, %.1f)", state.touch.x, state.touch.y);
    } else if (!state.touch.touched && last_touch) {
        log_message(LOG_DEBUG, "Input", "Touch END at (%.1f, %.1f)", last_touch_x, last_touch_y);
    } else if (state.touch.touched) {
        if (fabs(state.touch.move_x) || fabs(state.touch.move_y))
            log_message(LOG_DEBUG, "Input", "Touch MOVE (%.1f, %.1f) Δ(%.1f, %.1f)",
                        state.touch.x, state.touch.y, state.touch.move_x, state.touch.move_y);
    }

    last_touch = state.touch.touched;
    last_touch_x = state.touch.x;
    last_touch_y = state.touch.y;
#endif
}
