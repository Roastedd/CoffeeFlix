// Everything that differs between the Wii U and the desktop preview build.
#pragma once

#include <string>
#include <SDL2/SDL.h>

#include "core/input.hpp"

namespace platform {

// Called before SDL_Init (Wii U: ProcUI, network, logging).
bool init();
// Called after the window/renderer exist.
void post_video_init(SDL_Window* window, SDL_Renderer* renderer);
void shutdown();

// False once the system asked the app to quit (HOME menu -> close, window closed).
bool running();
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

}  // namespace platform
