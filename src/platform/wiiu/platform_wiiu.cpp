#include "platform/platform.hpp"

#include <whb/proc.h>
#include <coreinit/dynload.h>
#include <coreinit/energysaver.h>
#include <nn/ac.h>
#include <nn/nets2/somemopt.h>
#include <sys/socket.h>
#include <vpad/input.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <sysapp/launch.h>

#include <SDL2/SDL_syswm.h>

#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/text_input.hpp"

// Our FFmpeg's socket hook (tools/patches/ffmpeg/0002-tcp-socket-setup-hook.patch).
extern "C" void (*ff_tcp_socket_setup)(int fd);

namespace platform {

namespace {

bool g_ac_ok = false;
uint32_t g_ip = 0;
float g_rumble_left = 0;
Awake g_awake = AWAKE_NONE;
uint32_t g_dim_was_on = 0, g_apd_was_on = 0;
uint8_t g_rumble_pattern[15];

TextInputState g_text_state = TEXT_IDLE;
std::string g_text;
bool g_collecting = false;

// Socket receive buffers come from a pool that is small by default, which holds each
// connection to about 64 KB per round trip (100-200 KB/s from YouTube). somemopt() donates
// memory to the pool; a socket draws from it once SO_RUSRBUF is set. NUSspli measured the
// same limit: github.com/V10lator/NUSspli/pull/491
constexpr uint32_t SOCKET_POOL_SIZE = 0x300000;  // the most somemopt() accepts
constexpr int SOCKET_RCVBUF = 256 * 1024;        // ~5 MB/s at 50 ms round trips
std::atomic<bool> g_pool_ready{false};
std::atomic<bool> g_pool_thread_done{false};
int g_pool_bytes = 0;

void donate_socket_pool() {
    void* pool = memalign(0x40, SOCKET_POOL_SIZE);  // the network stack keeps it until we quit
    if (!pool) return;
    // SOMEMOPT_REQUEST_INIT only returns when the stack shuts down: it gets its own thread.
    std::thread([pool] {
        somemopt(SOMEMOPT_REQUEST_INIT, pool, SOCKET_POOL_SIZE, SOMEMOPT_FLAGS_BIG_BUFFERS);
        g_pool_thread_done = true;
    }).detach();
    // Sockets opened before the donation lands get small buffers, so wait for it (up to a
    // second: INIT returns early if it was refused).
    for (int i = 0; i < 100 && !g_pool_thread_done; i++) {
        int used = somemopt(SOMEMOPT_REQUEST_GET_BYTES_USED, nullptr, 0, SOMEMOPT_FLAGS_NONE);
        if (used > 0) {
            g_pool_bytes = used;
            g_pool_ready = true;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    log_message(LOG_WARNING, "Platform", "Socket memory pool not available");
}

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
    donate_socket_pool();
    ff_tcp_socket_setup = tune_socket;  // FFmpeg's own connections (Twitch, radio) too
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
    IMIsDimEnabled(&g_dim_was_on);
    IMIsAPDEnabled(&g_apd_was_on);
    util::make_dirs(data_dir());
    return true;
}

void post_video_init(SDL_Window*, SDL_Renderer*) {
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    SDL_StopTextInput();
}

void shutdown() {
    keep_awake(AWAKE_NONE);
    VPADStopMotor(VPAD_CHAN_0);
    KPADShutdown();
    WHBProcShutdown();
    nn::ac::Finalize();
}

bool running() { return WHBProcIsRunning(); }
void exit_to_menu() { SYSLaunchMenu(); }

namespace {

// Aroma's RPX loader, which started us from the .wuhb: which file that was, and letting go of
// it so the updater can replace it. Its exports as librpxloader (wiiu-env) reaches them: the
// path needs its API version 2, unmounting version 1. Functions return 0 on success.
struct RpxLoader {
    uint32_t version = 0;
    int (*get_path)(char* out, uint32_t size) = nullptr;
    int (*unmount)() = nullptr;
};

const RpxLoader& rpx_loader() {
    static RpxLoader l;
    static bool tried = false;
    if (tried) return l;
    tried = true;
    // Only from a bundle: the Tiramisu build runs from the Homebrew Launcher, without it.
    if (content_dir() != "/vol/content") return l;
    OSDynLoad_Module m = nullptr;
    int (*get_version)(uint32_t*) = nullptr;
    if (OSDynLoad_Acquire("homebrew_rpx_loader", &m) != OS_DYNLOAD_OK ||
        OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, "RL_GetVersion", (void**)&get_version) != OS_DYNLOAD_OK ||
        get_version(&l.version) != 0) {
        log_message(LOG_WARNING, "Platform", "Aroma's RPX loader isn't answering");
        return l;
    }
    if (l.version >= 2)
        OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, "RL_GetPathOfRunningExecutable", (void**)&l.get_path);
    if (l.version >= 1)
        OSDynLoad_FindExport(m, OS_DYNLOAD_EXPORT_FUNC, "RL_UnmountCurrentRunningBundle", (void**)&l.unmount);
    return l;
}

}  // namespace

std::string app_bundle() {
    const RpxLoader& l = rpx_loader();
    char buf[256] = {};
    if (!l.get_path || l.get_path(buf, sizeof(buf) - 1) != 0) return "";
    // Relative to the SD card's root.
    std::string p = buf;
    if (util::starts_with(p, "fs:")) p.erase(0, 3);
    while (util::starts_with(p, "/")) p.erase(0, 1);
    if (!util::starts_with(p, "vol/external01/")) p = "vol/external01/" + p;
    p = "/" + p;
    if (util::file_extension(p) != "wuhb" || !util::file_exists(p)) {
        log_message(LOG_WARNING, "Platform", "Started from %s, which can't be updated in place", buf);
        return "";
    }
    return p;
}

bool release_app_bundle() {
    const RpxLoader& l = rpx_loader();
    if (!l.unmount) return false;
    int rc = l.unmount();
    if (rc != 0) log_message(LOG_ERROR, "Platform", "Aroma didn't let go of the bundle (%d)", rc);
    return rc == 0;
}

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
// The .wuhb bundle mounts its files at /vol/content. The Tiramisu build is a bare .rpx,
// launched from the Homebrew Launcher, with the same files in a folder next to it.
std::string content_dir() {
    static const std::string dir =
        util::dir_exists("/vol/content/fonts") ? "/vol/content" : "/vol/external01/wiiu/apps/coffeeflix/content";
    return dir;
}
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

void tune_socket(int fd) {
    // Only speed is at stake: an option the system refuses is ignored.
    auto set = [fd](int option, int value) { setsockopt(fd, SOL_SOCKET, option, &value, sizeof(value)); };
    set(SO_WINSCALE, 1);  // windows over 64 KB
    set(SO_TCPSACK, 1);
    if (g_pool_ready) set(SO_RUSRBUF, 1);  // before SO_RCVBUF, which then draws from the pool
    set(SO_RCVBUF, SOCKET_RCVBUF);

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        int got = 0;
        socklen_t len = sizeof(got);
        getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &len);
        log_message(LOG_OK, "Platform", "Socket receive buffer %d KB (memory pool %d KB)", got / 1024, g_pool_bytes / 1024);
    }
}

void rumble(float seconds) {
    VPADControlMotor(VPAD_CHAN_0, g_rumble_pattern, 120);
    g_rumble_left = seconds;
}

void keep_awake(Awake level) {
    if (level == g_awake) return;
    bool dim_off = level == AWAKE_FULL, apd_off = level != AWAKE_NONE;
    bool was_dim_off = g_awake == AWAKE_FULL, was_apd_off = g_awake != AWAKE_NONE;
    if (dim_off != was_dim_off && g_dim_was_on) dim_off ? IMDisableDim() : IMEnableDim();
    if (apd_off != was_apd_off && g_apd_was_on) apd_off ? IMDisableAPD() : IMEnableAPD();
    g_awake = level;
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
void record_frame(SDL_Renderer*) {}

}  // namespace platform
