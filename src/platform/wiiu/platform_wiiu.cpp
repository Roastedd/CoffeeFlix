#include "platform/platform.hpp"

#include <whb/proc.h>
#include <proc_ui/procui.h>
#include <coreinit/dynload.h>
#include <coreinit/exit.h>
#include <coreinit/energysaver.h>
#include <coreinit/memory.h>
#include <coreinit/mutex.h>
#include <coreinit/core.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/userconfig.h>
#include <nn/ac.h>
#include <nn/act.h>
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
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "core/i18n.hpp"
#include "core/util.hpp"
#include "crash_wiiu.hpp"
#include "logger/logger.hpp"
#include "platform/text_input.hpp"

// Our FFmpeg's socket hook (tools/patches/ffmpeg/0002-tcp-socket-setup-hook.patch).
extern "C" void (*ff_tcp_socket_setup)(int fd);

namespace platform {

namespace {

bool g_ac_ok = false;
uint32_t g_ip = 0;
float g_rumble_left = 0;
Awake g_awake = AWAKE_NONE;  // what the app last asked for (main thread)
uint32_t g_dim_was_on = 0, g_apd_was_on = 0;
uint8_t g_rumble_pattern[15];

// The energy-saver calls are round trips to the system's power manager, which the frame making
// them would wait for. A thread of its own makes them; the main thread only says what it wants.
struct PowerRequests {
    std::mutex m;
    std::condition_variable cv;
    Awake wanted = AWAKE_NONE;
    bool quit = false;
    std::thread thread;
};
PowerRequests* g_power = nullptr;  // never destroyed: the thread may still be waiting on it as the process ends

TextInputState g_text_state = TEXT_IDLE;
std::string g_text;
bool g_collecting = false;
int g_main_priority = 16;

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

// Puts the console's dimming and auto power-down from one level of keep_awake() to another, only
// changing what its own settings had turned on.
void set_energy_saver(Awake from, Awake to) {
    const bool dim_off = to == AWAKE_FULL, was_dim_off = from == AWAKE_FULL;
    const bool apd_off = to != AWAKE_NONE, was_apd_off = from != AWAKE_NONE;
    const OSTime start = OSGetSystemTime();
    IMError err = 0;
    if (dim_off != was_dim_off && g_dim_was_on) {
        const IMError e = dim_off ? IMDisableDim() : IMEnableDim();
        if (e) err = e;
    }
    if (apd_off != was_apd_off && g_apd_was_on) {
        const IMError e = apd_off ? IMDisableAPD() : IMEnableAPD();
        if (e) err = e;
    }
    const int ms = (int)OSTicksToMilliseconds(OSGetSystemTime() - start);
    if (err) log_message(LOG_WARNING, "Platform", "The energy saver settings were refused (%d)", (int)err);
    if (ms >= 30) log_message(LOG_WARNING, "Platform", "The system took %d ms to change the energy saver settings", ms);
}

void power_thread() {
    set_thread_name("power settings");
    lower_thread_priority();
    PowerRequests& p = *g_power;
    Awake applied = AWAKE_NONE;
    std::unique_lock<std::mutex> lock(p.m);
    for (;;) {
        p.cv.wait(lock, [&] { return p.quit || p.wanted != applied; });
        const bool last = p.quit;  // the settings come back to the console's own before the app ends
        const Awake to = last ? AWAKE_NONE : p.wanted;
        lock.unlock();
        if (to != applied) set_energy_saver(applied, to);
        applied = to;
        if (last) return;
        lock.lock();
    }
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

namespace {

Lifecycle g_lifecycle;

// ProcUI calls these from inside ProcUIProcessMessages (WHBProcIsRunning) on the main thread. SDL
// registers its own for the same moments, at priority 100, to let go of the graphics memory it holds
// in front. The higher priorities run first (the logs show 1000, then ours, then 1): ours is above
// SDL's, so what uses that memory, a video's decoder and texture, is let go of before SDL frees it.
// The callback itself still must not touch SDL.
constexpr uint32_t RELEASE_BEFORE_SDL = 2000;
uint32_t proc_release(void*) {
    crash::background(true);
    crash::phase("HOME: in our release callback");
    log_message(LOG_OK, "Platform", "HOME pressed: giving up the foreground");
    if (g_lifecycle.leaving_foreground) g_lifecycle.leaving_foreground();
    log_message(LOG_OK, "Platform", "HOME: our release callback is done");
    crash::phase("HOME: after our release callback, in the others' or the system's");
    return 0;
}

// Not ours to do anything: two more release callbacks, one at each end of the priorities, so the
// log shows in which order ProcUI runs them, SDL's (priority 100, which frees the graphics memory
// in front) among them.
uint32_t proc_release_probe_high(void*) {
    log_message(LOG_OK, "Platform", "HOME: release callbacks, priority 1000");
    return 0;
}

uint32_t proc_release_probe_above_sdl(void*) {
    log_message(LOG_OK, "Platform", "HOME: release callbacks, priority 101 (just above SDL's)");
    return 0;
}

uint32_t proc_release_probe_below_sdl(void*) {
    log_message(LOG_OK, "Platform", "HOME: release callbacks, priority 99 (just below SDL's)");
    return 0;
}

uint32_t proc_release_probe_low(void*) {
    log_message(LOG_OK, "Platform", "HOME: release callbacks, priority 1");
    return 0;
}

uint32_t proc_acquire(void*) {
    crash::background(false);
    log_message(LOG_OK, "Platform", "Back in the foreground");
    if (g_lifecycle.returned) g_lifecycle.returned();
    return 0;
}

uint32_t proc_exit(void*) {
    log_message(LOG_OK, "Platform", "The system asks the app to close");
    if (g_lifecycle.exiting) g_lifecycle.exiting();
    return 0;
}

// The system stops and allows networking for apps in the background and on the way to sleep. Nothing to
// do about it yet, but the log tells what was going on around a freeze.
uint32_t proc_net_stop(void*) {
    log_message(LOG_OK, "Platform", "The system asks the app to stop using the network");
    return 0;
}

uint32_t proc_net_start(void*) {
    log_message(LOG_OK, "Platform", "The network may be used again");
    return 0;
}

void register_lifecycle_callbacks() {
    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, proc_release, nullptr, RELEASE_BEFORE_SDL);
    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, proc_release_probe_high, nullptr, 1000);
    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, proc_release_probe_above_sdl, nullptr, 101);
    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, proc_release_probe_below_sdl, nullptr, 99);
    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, proc_release_probe_low, nullptr, 1);
    ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, proc_acquire, nullptr, 50);
    ProcUIRegisterCallback(PROCUI_CALLBACK_EXIT, proc_exit, nullptr, 50);
    ProcUIRegisterCallback(PROCUI_CALLBACK_NET_IO_STOP, proc_net_stop, nullptr, 50);
    ProcUIRegisterCallback(PROCUI_CALLBACK_NET_IO_START, proc_net_start, nullptr, 50);
}

}  // namespace

bool init() {
    g_main_priority = OSGetThreadPriority(OSGetCurrentThread());
    log_message(LOG_OK, "Platform", "Main thread: priority %d on core %u", g_main_priority, OSGetCoreId());
    nn::ac::ConfigIdNum config_id;
    nn::ac::Initialize();
    nn::ac::GetStartupId(&config_id);
    g_ac_ok = nn::ac::Connect(config_id);
    donate_socket_pool();
    ff_tcp_socket_setup = tune_socket;  // FFmpeg's own connections (Twitch, radio) too
    WHBProcInit();
    register_lifecycle_callbacks();
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
    if (g_power && g_power->thread.joinable()) {  // waits for the console's own settings to be back
        {
            std::lock_guard<std::mutex> lock(g_power->m);
            g_power->quit = true;
        }
        g_power->cv.notify_one();
        g_power->thread.join();
    }
    VPADStopMotor(VPAD_CHAN_0);
    KPADShutdown();
    WHBProcShutdown();
    nn::ac::Finalize();
}

bool running() {
    crash::phase("in the system's process loop (HOME, closing)");
    const bool going = WHBProcIsRunning();
    crash::frame();
    crash::phase("running a frame");
    return going;
}
void exit_to_menu() { SYSLaunchMenu(); }

void set_lifecycle(const Lifecycle& handlers) { g_lifecycle = handlers; }
void install_crash_reporter() { crash::install(); }
void main_phase(const char* stage) { crash::phase(stage); }

std::string memory_summary() {
    const struct mallinfo m = mallinfo();
    return util::fmt("%u KB in use of %u KB taken from the system, %u KB free in it", (unsigned)m.uordblks / 1024, (unsigned)m.arena / 1024,
                     (unsigned)m.fordblks / 1024);
}
void terminate_now(int code) {
    _Exit(code);
    for (;;) OSSleepTicks(OSMillisecondsToTicks(1000));  // _Exit doesn't return; this tells the compiler too
}

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

// The last reading of each controller. A read can find no new sample since the one before (the
// controllers report on their own clock, not the screen's); then the last one still holds.
// Treating it as nothing held would let go of every button and the touch for a frame, and the
// next frame would press them again: a second A press, or a second tap.
VPADStatus g_vpad{};
bool g_vpad_ok = false;
KPADStatus g_kpad[4]{};
bool g_kpad_ok[4] = {};

void poll(RawInput& raw) {
    raw = RawInput();
    const bool keyboard_open = g_text_state == TEXT_ACTIVE;

    VPADStatus fresh{};
    VPADReadError verr;
    if (VPADRead(VPAD_CHAN_0, &fresh, 1, &verr) > 0 && verr == VPAD_READ_SUCCESS) {
        g_vpad = fresh;
        g_vpad_ok = true;
        if (keyboard_open) {
            VPADStatus kb = fresh;
            VPADGetTPCalibratedPoint(VPAD_CHAN_0, &kb.tpNormal, &fresh.tpNormal);
            SDL_WiiUSetSWKBDVPAD(&kb);
        }
    } else if (verr != VPAD_READ_NO_SAMPLES) {
        g_vpad_ok = false;
    }
    if (g_vpad_ok) {
        VPADStatus& vpad = g_vpad;
        raw.held |= map_vpad(vpad.hold);
        raw.lx = axis(vpad.leftStick.x);
        raw.ly = axis(vpad.leftStick.y);
        raw.rx = axis(vpad.rightStick.x);
        raw.ry = axis(vpad.rightStick.y);

        // Near the screen's edges a touch can come with an unusable x or y for a moment: it's
        // still the same touch, where it last was.
        static float last_tx = 0, last_ty = 0;
        static bool was_touched = false;
        VPADTouchData tp{};
        VPADGetTPCalibratedPoint(VPAD_CHAN_0, &tp, &vpad.tpNormal);
        if (tp.touched && (tp.validity == VPAD_VALID || was_touched)) {
            raw.touch = true;
            if (!(tp.validity & VPAD_INVALID_X)) last_tx = tp.x;  // calibrated to 1280x720
            if (!(tp.validity & VPAD_INVALID_Y)) last_ty = tp.y;
            raw.tx = last_tx;
            raw.ty = last_ty;
        }
        was_touched = raw.touch;
    }

    for (int ch = 0; ch < 4; ch++) {
        KPADStatus fresh_k{};
        KPADError kerr;
        if (KPADReadEx((KPADChan)ch, &fresh_k, 1, &kerr) > 0 && kerr == KPAD_ERROR_OK) {
            g_kpad[ch] = fresh_k;
            g_kpad_ok[ch] = true;
            if (keyboard_open) SDL_WiiUSetSWKBDKPAD(ch, &fresh_k);
        } else if (kerr != KPAD_ERROR_NO_SAMPLES) {
            g_kpad_ok[ch] = false;
        }
        if (!g_kpad_ok[ch]) continue;
        const KPADStatus& k = g_kpad[ch];
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

// A persistent ID stays with a user for good; slot numbers are reused after one is deleted.
std::string user_id() {
    static const std::string id = [] {
        if (nn::act::Initialize().IsFailure()) return std::string();
        nn::act::PersistentId pid = nn::act::GetPersistentId();
        nn::act::Finalize();
        return pid ? util::fmt("%08x", (unsigned)pid) : std::string();
    }();
    return id;
}

int volumes(Volume* out, int max) {
    int n = 0;
    auto add = [&](const char* label, const char* path, int icon) {
        if (n < max && util::dir_exists(path)) out[n++] = Volume{label, path, icon};
    };
    add(tr("CoffeeFlix folder"), "/vol/external01/wiiu/apps/coffeeflix", 0xe2c7);
    add(tr("SD Card"), "/vol/external01", 0xe623);
    return n;
}

std::string system_language() {
    // cafe.language counts from Japanese (nn::swkbd::LanguageType).
    static const char* CODES[] = {"ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh-tw"};
    alignas(0x40) uint32_t lang = 1;
    alignas(0x40) UCSysConfig config;
    memset(&config, 0, sizeof(config));
    strncpy(config.name, "cafe.language", sizeof(config.name) - 1);
    config.dataType = UC_DATATYPE_UNSIGNED_INT;
    config.dataSize = sizeof(lang);
    config.data = &lang;
    UCHandle uc = UCOpen();
    if (uc < 0) return "en";
    UCError err = UCReadSysConfig(uc, 1, &config);
    UCClose(uc);
    return err == 0 && lang < sizeof(CODES) / sizeof(CODES[0]) ? CODES[lang] : "en";
}

bool cjk_font(CjkFont which, FontFile& out) {
    static const OSSharedDataType TYPES[CJK_FONT_COUNT] = {OS_SHAREDDATATYPE_FONT_STANDARD, OS_SHAREDDATATYPE_FONT_CHINESE,
                                                           OS_SHAREDDATATYPE_FONT_KOREAN};
    void* data = nullptr;
    uint32_t size = 0;
    if (which < 0 || which >= CJK_FONT_COUNT || !OSGetSharedData(TYPES[which], 0, &data, &size) || !data || !size)
        return false;
    out.data = data;
    out.size = size;
    return true;
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
    // Only speed is at stake: an option the system refuses is ignored, but counted for the log.
    int refused = 0, refused_errno = 0;
    auto set = [&](int option, int value) {
        if (setsockopt(fd, SOL_SOCKET, option, &value, sizeof(value)) == 0) return true;
        if (!refused++) refused_errno = errno;
        return false;
    };
    set(SO_WINSCALE, 1);  // windows over 64 KB
    set(SO_TCPSACK, 1);
    const bool from_pool = g_pool_ready && set(SO_RUSRBUF, 1);  // before SO_RCVBUF, which then draws from the pool

    // Half the size again while the system refuses: a pool with some room left still gives a
    // connection a bigger buffer than the 8 KB it starts with (one run had every socket on 8 KB
    // with the pool donated, downloads at a third of the speed).
    int got = 0;
    for (int want = SOCKET_RCVBUF;; want /= 2) {
        set(SO_RCVBUF, want);
        socklen_t len = sizeof(got);
        got = 0;
        getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &len);
        if (got >= want || want <= 64 * 1024) break;
    }

    // Logged for the first socket, and again whenever one gets less than any before (the pool
    // used up by the sockets open at the time: slower downloads).
    static std::atomic<int> least{-1};
    // The first sockets' numbers and buffers too: a run that piles sockets up (their numbers
    // climbing) or is handed less than it asked for shows here.
    static std::atomic<int> opened{0};
    if (const int n = ++opened; n <= 60)
        log_message(LOG_DEBUG, "Platform", "Socket %d opened: descriptor %d, receive buffer %d KB", n, fd, got / 1024);
    const int was = least.load();
    if (was < 0 || got < was) {
        least = got;
        std::string why;
        if (got < SOCKET_RCVBUF)  // what the system said to the options
            why = util::fmt(" (asked for %d KB%s; %d options refused, first with error %d)", SOCKET_RCVBUF / 1024,
                            from_pool ? "" : ", pool buffers not switched on", refused, refused_errno);
        log_message(was < 0 && got >= SOCKET_RCVBUF ? LOG_OK : LOG_WARNING, "Platform", "Socket receive buffer %d KB (memory pool %d KB)%s",
                    got / 1024, g_pool_bytes / 1024, why.c_str());
    }
}

void rumble(float seconds) {
    VPADControlMotor(VPAD_CHAN_0, g_rumble_pattern, 120);
    g_rumble_left = seconds;
}

void keep_awake(Awake level) {
    if (level == g_awake) return;
    g_awake = level;
    if (!g_power) {  // the first request: nothing was held off before it
        g_power = new PowerRequests;
        g_power->thread = std::thread(power_thread);
    }
    {
        std::lock_guard<std::mutex> lock(g_power->m);
        g_power->wanted = level;
    }
    g_power->cv.notify_one();
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

void* current_thread() { return OSGetCurrentThread(); }

// The scheduler's count of the ticks the thread ran on each core, up to when it last stopped
// running (OSThread::coreTimeConsumedNs barely moves: a log showed 0-4% for everything).
uint64_t thread_cpu_ns(void* thread) {
    if (!thread) return 0;
    const OSContext& c = ((OSThread*)thread)->context;
    return OSTicksToNanoseconds(c.coretime[0] + c.coretime[1] + c.coretime[2]);
}

std::string thread_clock_debug(void* thread) {
    if (!thread) return "";
    const OSThread* t = (const OSThread*)thread;
    const OSContext& c = t->context;
    return util::fmt("core %u [%llu %llu %llu] start %llu consumed %llu wakes %llu quantum %lld prio %d/%d aff %x "
                     "610 %lld 618 %lld 620 %lld 628 %lld",
                     c.upir, (unsigned long long)c.coretime[0], (unsigned long long)c.coretime[1],
                     (unsigned long long)c.coretime[2], (unsigned long long)c.starttime,
                     (unsigned long long)t->coreTimeConsumedNs, (unsigned long long)t->wakeCount,
                     (long long)t->runQuantumTicks, t->priority, t->basePriority, (unsigned)t->attr,
                     (long long)t->unk0x610, (long long)t->unk0x618, (long long)t->unk0x620, (long long)t->unk0x628);
}

namespace {

// Memory a pointer in another thread's fields may point to; a stale or garbled one must not crash
// the thread reading it.
bool in_app_memory(const void* p, uintptr_t from = 0x10000000) { return (uintptr_t)p >= from && (uintptr_t)p < 0x50000000; }

}  // namespace

size_t thread_where(void* thread, char* out, size_t capacity) {
    if (!thread || capacity < 2) return 0;
    OSThread* t = (OSThread*)thread;
    const char state = t->suspendCounter > 0                  ? 'S'
                       : (t->state & OS_THREAD_STATE_RUNNING) ? 'R'
                       : (t->state & OS_THREAD_STATE_WAITING) ? 'W'
                       : (t->state & OS_THREAD_STATE_READY)   ? 'r'
                                                              : '?';
    size_t n = (size_t)std::snprintf(out, capacity, "%c pc=%08x lr=%08x", state, (unsigned)t->context.srr0, (unsigned)t->context.lr);
    n = std::min(n, capacity - 1);
    const OSMutex* lock = t->mutex;
    const OSThreadQueue* queue = t->queue;
    if (in_app_memory(lock)) {
        OSThread* owner = lock->owner;
        const char* name = in_app_memory(owner) ? OSGetThreadName(owner) : nullptr;
        if (!in_app_memory(name, 0x01000000)) name = nullptr;
        n += (size_t)std::snprintf(out + n, capacity - n, " waits for lock %08x held by %.16s (%08x)", (unsigned)(uintptr_t)lock,
                                   owner ? (name ? name : "?") : "nobody", (unsigned)(uintptr_t)owner);
    } else if (in_app_memory(queue)) {
        n += (size_t)std::snprintf(out + n, capacity - n, " sleeps on queue %08x", (unsigned)(uintptr_t)queue);
    }
    return std::min(n, capacity - 1);
}

std::string clock_debug() {
    return util::fmt("time %lld, system time %lld, %u ticks/s", (long long)OSGetTime(), (long long)OSGetSystemTime(),
                     (unsigned)OSTimerClockSpeed);
}

void set_thread_name(const char* name) { OSSetThreadName(OSGetCurrentThread(), name); }

void lower_thread_priority() {
    // 0 is the highest, 31 the lowest.
    OSSetThreadPriority(OSGetCurrentThread(), std::min(31, g_main_priority + 4));
}

void raise_thread_priority() { OSSetThreadPriority(OSGetCurrentThread(), std::max(0, g_main_priority - 4)); }

bool scripted() { return false; }
float fixed_dt() { return 0; }
const char* screenshot_request() { return nullptr; }
void save_screenshot(SDL_Renderer*, const char*) {}
void record_frame(SDL_Renderer*) {}

}  // namespace platform
