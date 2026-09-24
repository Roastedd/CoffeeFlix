#include "platform/platform.hpp"

#include <whb/proc.h>
#include <nn/ac.h>
#include <vpad/input.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>

#include <SDL2/SDL_syswm.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/text_input.hpp"

namespace platform {

namespace {

bool g_ac_ok = false;
uint32_t g_ip = 0;
float g_rumble_left = 0;
uint8_t g_rumble_pattern[15];

TextInputState g_text_state = TEXT_IDLE;
std::string g_text;
bool g_collecting = false;

void pump_sdl_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_TEXTINPUT && g_collecting) {
            g_text += e.text.text;
        } else if (e.type == SDL_SYSWMEVENT && e.syswm.msg && e.syswm.msg->subsystem == SDL_SYSWM_WIIU) {
            switch (e.syswm.msg->msg.wiiu.event) {
                case SDL_WIIU_SYSWM_SWKBD_OK_START_EVENT:
                    g_text.clear();
                    g_collecting = true;
                    break;
                case SDL_WIIU_SYSWM_SWKBD_OK_FINISH_EVENT:
                    g_collecting = false;
                    if (g_text_state == TEXT_ACTIVE) g_text_state = TEXT_DONE;
                    SDL_StopTextInput();
                    break;
                case SDL_WIIU_SYSWM_SWKBD_CANCEL_EVENT:
                    g_collecting = false;
                    if (g_text_state == TEXT_ACTIVE) g_text_state = TEXT_CANCELLED;
                    SDL_StopTextInput();
                    break;
                default: break;
            }
        }
    }
}

float axis(float v) { return std::fabs(v) < 0.12f ? 0.0f : std::clamp(v, -1.0f, 1.0f); }

uint32_t map_vpad(uint32_t h) {
    uint32_t o = 0;
    if (h & VPAD_BUTTON_A) o |= bit(BTN_A);
    if (h & VPAD_BUTTON_B) o |= bit(BTN_B);
    if (h & VPAD_BUTTON_X) o |= bit(BTN_X);
    if (h & VPAD_BUTTON_Y) o |= bit(BTN_Y);
    if (h & VPAD_BUTTON_PLUS) o |= bit(BTN_PLUS);
    if (h & VPAD_BUTTON_MINUS) o |= bit(BTN_MINUS);
    if (h & VPAD_BUTTON_UP) o |= bit(BTN_UP);
    if (h & VPAD_BUTTON_DOWN) o |= bit(BTN_DOWN);
    if (h & VPAD_BUTTON_LEFT) o |= bit(BTN_LEFT);
    if (h & VPAD_BUTTON_RIGHT) o |= bit(BTN_RIGHT);
    if (h & VPAD_BUTTON_L) o |= bit(BTN_L);
    if (h & VPAD_BUTTON_R) o |= bit(BTN_R);
    if (h & VPAD_BUTTON_ZL) o |= bit(BTN_ZL);
    if (h & VPAD_BUTTON_ZR) o |= bit(BTN_ZR);
    if (h & VPAD_BUTTON_STICK_L) o |= bit(BTN_STICK_L);
    if (h & VPAD_BUTTON_STICK_R) o |= bit(BTN_STICK_R);
    return o;
}

// Pro Controller and Classic Controller share the same bit layout.
uint32_t map_classic(uint32_t h) {
    uint32_t o = 0;
    if (h & WPAD_CLASSIC_BUTTON_A) o |= bit(BTN_A);
    if (h & WPAD_CLASSIC_BUTTON_B) o |= bit(BTN_B);
    if (h & WPAD_CLASSIC_BUTTON_X) o |= bit(BTN_X);
    if (h & WPAD_CLASSIC_BUTTON_Y) o |= bit(BTN_Y);
    if (h & WPAD_CLASSIC_BUTTON_PLUS) o |= bit(BTN_PLUS);
    if (h & WPAD_CLASSIC_BUTTON_MINUS) o |= bit(BTN_MINUS);
    if (h & WPAD_CLASSIC_BUTTON_UP) o |= bit(BTN_UP);
    if (h & WPAD_CLASSIC_BUTTON_DOWN) o |= bit(BTN_DOWN);
    if (h & WPAD_CLASSIC_BUTTON_LEFT) o |= bit(BTN_LEFT);
    if (h & WPAD_CLASSIC_BUTTON_RIGHT) o |= bit(BTN_RIGHT);
    if (h & WPAD_CLASSIC_BUTTON_L) o |= bit(BTN_L);
    if (h & WPAD_CLASSIC_BUTTON_R) o |= bit(BTN_R);
    if (h & WPAD_CLASSIC_BUTTON_ZL) o |= bit(BTN_ZL);
    if (h & WPAD_CLASSIC_BUTTON_ZR) o |= bit(BTN_ZR);
    return o;
}

uint32_t map_pro(uint32_t h) {
    uint32_t o = map_classic(h);
    if (h & WPAD_PRO_BUTTON_STICK_L) o |= bit(BTN_STICK_L);
    if (h & WPAD_PRO_BUTTON_STICK_R) o |= bit(BTN_STICK_R);
    return o;
}

// Wii Remote held upright, pointed at the TV.
uint32_t map_wiimote(uint32_t h) {
    uint32_t o = 0;
    if (h & WPAD_BUTTON_A) o |= bit(BTN_A);
    if (h & WPAD_BUTTON_B) o |= bit(BTN_B);
    if (h & WPAD_BUTTON_1) o |= bit(BTN_Y);
    if (h & WPAD_BUTTON_2) o |= bit(BTN_X);
    if (h & WPAD_BUTTON_PLUS) o |= bit(BTN_PLUS);
    if (h & WPAD_BUTTON_MINUS) o |= bit(BTN_MINUS);
    if (h & WPAD_BUTTON_UP) o |= bit(BTN_UP);
    if (h & WPAD_BUTTON_DOWN) o |= bit(BTN_DOWN);
    if (h & WPAD_BUTTON_LEFT) o |= bit(BTN_LEFT);
    if (h & WPAD_BUTTON_RIGHT) o |= bit(BTN_RIGHT);
    if (h & WPAD_BUTTON_Z) o |= bit(BTN_ZR);
    if (h & WPAD_BUTTON_C) o |= bit(BTN_ZL);
    return o;
}

}  // namespace

bool init() {
    nn::ac::ConfigIdNum config_id;
    nn::ac::Initialize();
    nn::ac::GetStartupId(&config_id);
    g_ac_ok = nn::ac::Connect(config_id);
    WHBProcInit();
    VPADInit();
    KPADInit();
    WPADEnableURCC(true);  // Pro Controller support
    if (nn::ac::GetAssignedAddress(&g_ip)) {
        log_message(LOG_OK, "Platform", "IP %u.%u.%u.%u", g_ip >> 24, (g_ip >> 16) & 255, (g_ip >> 8) & 255, g_ip & 255);
    } else {
        log_message(LOG_WARNING, "Platform", "No network address");
    }
    for (auto& b : g_rumble_pattern) b = 0xFF;
    util::make_dirs(data_dir());
    return true;
}

void post_video_init(SDL_Window*, SDL_Renderer*) {
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    SDL_StopTextInput();
}

void shutdown() {
    VPADStopMotor(VPAD_CHAN_0);
    KPADShutdown();
    WHBProcShutdown();
    nn::ac::Finalize();
}

bool running() { return WHBProcIsRunning(); }

void poll(RawInput& raw) {
    raw = RawInput();
    const bool keyboard_open = g_text_state == TEXT_ACTIVE;

    VPADStatus vpad{};
    VPADReadError verr;
    if (VPADRead(VPAD_CHAN_0, &vpad, 1, &verr) > 0 && verr == VPAD_READ_SUCCESS) {
        if (keyboard_open) {
            VPADStatus kb = vpad;
            VPADGetTPCalibratedPoint(VPAD_CHAN_0, &kb.tpNormal, &vpad.tpNormal);
            SDL_WiiUSetSWKBDVPAD(&kb);
        }
        raw.held |= map_vpad(vpad.hold);
        raw.lx = axis(vpad.leftStick.x);
        raw.ly = axis(vpad.leftStick.y);
        raw.rx = axis(vpad.rightStick.x);
        raw.ry = axis(vpad.rightStick.y);

        VPADTouchData tp{};
        VPADGetTPCalibratedPoint(VPAD_CHAN_0, &tp, &vpad.tpNormal);
        if (tp.touched && tp.validity == VPAD_VALID) {
            raw.touch = true;
            raw.tx = tp.x;  // calibrated to 1280x720
            raw.ty = tp.y;
        }
    }

    for (int ch = 0; ch < 4; ch++) {
        KPADStatus k{};
        KPADError kerr;
        if (KPADReadEx((KPADChan)ch, &k, 1, &kerr) == 0 || kerr != KPAD_ERROR_OK) continue;
        if (keyboard_open) SDL_WiiUSetSWKBDKPAD(ch, &k);
        switch (k.extensionType) {
            case WPAD_EXT_PRO_CONTROLLER:
                raw.held |= map_pro(k.pro.hold);
                if (raw.lx == 0 && raw.ly == 0) { raw.lx = axis(k.pro.leftStick.x); raw.ly = axis(k.pro.leftStick.y); }
                if (raw.rx == 0 && raw.ry == 0) { raw.rx = axis(k.pro.rightStick.x); raw.ry = axis(k.pro.rightStick.y); }
                break;
            case WPAD_EXT_CLASSIC:
            case WPAD_EXT_MPLUS_CLASSIC:
                raw.held |= map_classic(k.classic.hold) | map_wiimote(k.hold);
                if (raw.lx == 0 && raw.ly == 0) { raw.lx = axis(k.classic.leftStick.x); raw.ly = axis(k.classic.leftStick.y); }
                if (raw.rx == 0 && raw.ry == 0) { raw.rx = axis(k.classic.rightStick.x); raw.ry = axis(k.classic.rightStick.y); }
                break;
            case WPAD_EXT_NUNCHUK:
            case WPAD_EXT_MPLUS_NUNCHUK:
                raw.held |= map_wiimote(k.hold);
                if (raw.lx == 0 && raw.ly == 0) { raw.lx = axis(k.nunchuk.stick.x); raw.ly = axis(k.nunchuk.stick.y); }
                break;
            default:
                raw.held |= map_wiimote(k.hold);
                break;
        }
        if (k.posValid > 0 && !raw.pointer) {
            raw.pointer = true;
            raw.px = (k.pos.x + 1.0f) * 0.5f * 1280.0f;
            raw.py = (k.pos.y + 1.0f) * 0.5f * 720.0f;
        }
    }

    // Pumping SDL also runs the swkbd state machine (text arrives as events).
    pump_sdl_events();
    if (keyboard_open) raw = RawInput();  // the keyboard owns the controllers

    if (g_rumble_left > 0) {
        g_rumble_left -= 1.0f / 60.0f;
        if (g_rumble_left <= 0) VPADStopMotor(VPAD_CHAN_0);
    }
}

const char* name() { return "Wii U"; }
bool is_wiiu() { return true; }
std::string content_dir() { return "/vol/content"; }
std::string data_dir() { return "/vol/external01/wiiu/apps/coffeeflix"; }
std::string media_root() { return "/vol/external01/wiiu/apps/coffeeflix"; }

int volumes(Volume* out, int max) {
    int n = 0;
    auto add = [&](const char* label, const char* path, int icon) {
        if (n < max && util::dir_exists(path)) out[n++] = Volume{label, path, icon};
    };
    add("CoffeeFlix folder", "/vol/external01/wiiu/apps/coffeeflix", 0xe2c7);
    add("SD Card", "/vol/external01", 0xe623);
    return n;
}

bool network_connected() {
    uint32_t ip = 0;
    return nn::ac::GetAssignedAddress(&ip) && ip != 0;
}

std::string ip_address() {
    uint32_t ip = 0;
    if (!nn::ac::GetAssignedAddress(&ip) || !ip) return "";
    return util::fmt("%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

void rumble(float seconds) {
    VPADControlMotor(VPAD_CHAN_0, g_rumble_pattern, 120);
    g_rumble_left = seconds;
}

// --- text input (system keyboard) -----------------------------------------------

void text_input_start(const TextInputOptions& opts) {
    g_text = opts.initial;
    g_collecting = false;
    SDL_WiiUSetSWKBDInitialText(opts.initial.c_str());
    SDL_WiiUSetSWKBDHintText(opts.hint.c_str());
    SDL_WiiUSetSWKBDOKLabel(opts.ok_label.c_str());
    SDL_WiiUSetSWKBDKeyboardMode(SDL_WIIU_SWKBD_KEYBOARD_MODE_FULL);
    SDL_WiiUSetSWKBDPasswordMode(opts.password ? SDL_WIIU_SWKBD_PASSWORD_MODE_FADE : SDL_WIIU_SWKBD_PASSWORD_MODE_SHOW);
    SDL_WiiUSetSWKBDShowWordSuggestions(opts.password || opts.url ? SDL_FALSE : SDL_TRUE);
    SDL_WiiUSetSWKBDHighlightInitialText(SDL_TRUE);
    g_text_state = TEXT_ACTIVE;
    SDL_StartTextInput();
}

TextInputState text_input_state() { return g_text_state; }
const std::string& text_input_value() { return g_text; }
bool text_input_native() { return true; }

void text_input_cancel() {
    if (g_text_state == TEXT_ACTIVE) SDL_StopTextInput();
    g_text_state = TEXT_IDLE;
}

void text_input_reset() { g_text_state = TEXT_IDLE; }

bool scripted() { return false; }
float fixed_dt() { return 0; }
const char* screenshot_request() { return nullptr; }
void save_screenshot(SDL_Renderer*, const char*) {}

}  // namespace platform
