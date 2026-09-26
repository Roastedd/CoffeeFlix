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
// Roots offered by the file browser as {label, path} pairs.
struct Volume { std::string label, path; int icon; };
int volumes(Volume* out, int max);

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
// take it: upload the frame then. The GPU can read the frame until another has been shown and
// the screen updated twice, or the texture destroyed.
bool show_video_frame(SDL_Texture* tex, const AVFrame* f);
// Before the CPU reads such a frame: it may still have what the memory held before cached.
void video_frame_cpu_read(const AVFrame* f);

// True when sig, an ECDSA signature in DER, signs the SHA-256 digest with the PEM public key
// (Wii U: mbedtls, desktop: OpenSSL).
bool verify_signature(const std::string& public_key_pem, const uint8_t digest[32], const std::vector<uint8_t>& sig);

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
