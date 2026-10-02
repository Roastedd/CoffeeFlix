// Everything that differs between the Wii U and the desktop preview build.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <SDL2/SDL.h>

#include "core/input.hpp"

struct AVCodecContext;
struct AVFrame;

namespace platform {

// Called before SDL_Init (Wii U: ProcUI, network, logging).
bool init();
// Called after the window/renderer exist.
void post_video_init(SDL_Window* window, SDL_Renderer* renderer);
void shutdown();

// False once the system asked the app to quit (HOME menu -> close, window closed).
bool running();
// Asks to close the app, back to the Wii U Menu; running() turns false once the system agrees.
void exit_to_menu();

// What the console asks of the app besides drawing frames (the Wii U's ProcUI; a desktop has
// nothing like it and never calls these). Each runs on the thread that calls running(), from
// inside it, before the system carries on: they must be quick, and must not touch SDL or the
// GPU (SDL lets go of its own graphics memory in the same breath).
struct Lifecycle {
    void (*leaving_foreground)() = nullptr;  // HOME was pressed; the app is about to be put in the background
    void (*returned)() = nullptr;            // and it's back in front
    void (*exiting)() = nullptr;             // closed from the HOME menu (or by the power button)
};
void set_lifecycle(const Lifecycle& handlers);

// Ends the process at once, without cleaning up: for when closing is stuck.
[[noreturn]] void terminate_now(int code);
// For the thread that ends a stuck process: a priority above the main thread's, so a spinning
// thread can't keep it from running. Nothing on a desktop.
void raise_thread_priority();

// A crash (a bad memory access, an illegal instruction) on the Wii U ends the app with the last
// picture on the screen. Once installed, the system's exception handlers write what they know
// (which thread, where in the code) through app/crash_report and into the log before it ends.
// Also watches the main loop and logs when it stops for seconds. Nothing on a desktop, which has
// its own crash reports.
void install_crash_reporter();
// Main-thread stage for the console stall reporter; literals only, no allocation.
void main_phase(const char* stage);
// The heap's use in a few words, for the line logged when memory runs out.
std::string memory_summary();

// The app's own package on the SD card when it can be replaced in place (Wii U: the .wuhb Aroma
// started it from), else empty. Desktop tests name one with COFFEEFLIX_BUNDLE.
std::string app_bundle();
// Makes the loader let go of that package so it can be renamed. /vol/content goes with it:
// only while exiting, once nothing reads bundled files any more.
bool release_app_bundle();
// Pumps OS events and reads all controllers.
void poll(RawInput& raw);

const char* name();
bool is_wiiu();
// Read-only bundled assets (fonts, sounds, certificates).
std::string content_dir();
// Writable app folder (settings, caches) and the user's media root.
std::string data_dir();
std::string media_root();
// The Wii U user CoffeeFlix runs as ("80000001", for good), whose sign-ins it uses; empty when
// there is one set for everyone (desktop: from COFFEEFLIX_USER).
std::string user_id();
// Roots offered by the file browser as {label, path} pairs.
struct Volume { std::string label, path; int icon; };
int volumes(Volume* out, int max);

// The console's language: "en", "ja", "zh", "zh-tw"... (desktop: from COFFEEFLIX_LANGUAGE or LANG).
std::string system_language();
// Fonts for Chinese, Japanese and Korean, which the bundled ones don't have (Wii U: the console's
// own, already in memory; desktop: the computer's). False when there's none.
enum CjkFont { CJK_JAPANESE, CJK_CHINESE, CJK_KOREAN, CJK_FONT_COUNT };
struct FontFile {
    const void* data = nullptr;  // else read from path
    size_t size = 0;
    std::string path;
};
bool cjk_font(CjkFont which, FontFile& out);

bool network_connected();
std::string ip_address();
// Prepares a new TCP socket before it connects (Wii U: large receive buffers, which a single
// connection's speed depends on). Nothing elsewhere.
void tune_socket(int fd);

// Video the GPU draws from where the Wii U hardware decoder wrote it, with no copy on the way to
// the screen. Elsewhere the decoder's frames are left alone and uploaded.
// Before avcodec_open2: the decoder's NV12 frames come from memory the GPU can read, laid out like
// its textures. detach_video_frames undoes it, before avcodec_free_context (frames still in use
// stay valid).
void attach_video_frames(AVCodecContext* ctx);
void detach_video_frames(AVCodecContext* ctx);
// Points an NV12 texture at such a frame. False for any other frame, or when the texture can't
// take it: upload the frame then. On a successful replacement, SDL has waited for GPU
// reads of the previous binding to complete; texture destruction also waits.
bool show_video_frame(SDL_Texture* tex, const AVFrame* f);
// Before the CPU reads such a frame: it may still have what the memory held before cached.
void video_frame_cpu_read(const AVFrame* f);

// True when sig, an ECDSA signature in DER, signs the SHA-256 digest with the PEM public key
// (Wii U: mbedtls, desktop: OpenSSL).
bool verify_signature(const std::string& public_key_pem, const uint8_t digest[32], const std::vector<uint8_t>& sig);

// For the developer log's CPU use by thread. current_thread() stands for the calling thread;
// thread_cpu_ns tells how much CPU time it has used so far (0 where unknown), and may be asked
// from any thread while it lives. set_thread_name names the calling thread for the system's own
// tools; the name must stay valid (a string literal).
void* current_thread();
uint64_t thread_cpu_ns(void* thread);
void set_thread_name(const char* name);
// Wii U, developer log: the scheduler's raw counters for a thread and the clocks, to check what
// thread_cpu_ns reads against. Empty elsewhere.
std::string thread_clock_debug(void* thread);
std::string clock_debug();
// For the black box, from any thread, without allocating or waiting: where a thread is as far as
// the system tells, written into `out` (`capacity` bytes): its state (R running, r ready, W waiting,
// S suspended), the address it stopped at and its caller, and what it waits for (a lock, and the
// thread holding it; a queue). Returns the length, 0 where nothing is known.
size_t thread_where(void* thread, char* out, size_t capacity);

// For threads doing background work (thumbnails, parsing, writing the log): on the Wii U they
// get a lower priority than the main thread's, so they only take its core while it waits for
// the screen. Playback's threads keep the normal one. Nothing on a desktop, with cores to spare.
void lower_thread_priority();

// Rumble the GamePad briefly (no-op elsewhere).
void rumble(float seconds);

// Stops the system from dimming the screen or powering off while media plays.
// The console's own settings come back once playback stops.
enum Awake { AWAKE_NONE, AWAKE_NO_POWEROFF, AWAKE_FULL };
void keep_awake(Awake level);

// Desktop test harness: a script can drive input and request screenshots.
bool scripted();
float fixed_dt();              // >0 when frames should advance at a fixed rate
const char* screenshot_request();  // path to save after this frame, or nullptr
void save_screenshot(SDL_Renderer* r, const char* path);
void record_frame(SDL_Renderer* r);  // desktop: the frame goes into the COFFEEFLIX_RECORD video

}  // namespace platform
