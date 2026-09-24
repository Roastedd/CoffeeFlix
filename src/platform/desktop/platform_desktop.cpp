// Desktop preview build: keyboard/mouse/gamepad input, plus a scripted mode
// used to drive the UI and capture screenshots without a display:
//
//   SDL_VIDEODRIVER=offscreen COFFEEFLIX_SCRIPT=script.txt ./coffeeflix
//
// Script commands (one per line): wait <frames> | press <button> |
// hold <button> <frames> | tap <x> <y> | type <text> | shot <file.png> | quit
#include "platform/platform.hpp"

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>

#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/text_input.hpp"

namespace platform {

namespace {

bool g_quit = false;
SDL_GameController* g_pad = nullptr;
float g_wheel = 0;
std::string g_content = "content";
std::string g_data = "data";

// text input
TextInputState g_text_state = TEXT_IDLE;
std::string g_text;

// scripting
struct Cmd { std::string op, a, b; };
std::deque<Cmd> g_script;
bool g_scripted = false;
int g_wait = 0;
uint32_t g_script_held = 0;
int g_hold_frames = 0;
bool g_tap_pending = false;
int g_tap_phase = 0;
float g_tap_x = 0, g_tap_y = 0;
std::string g_shot;

Button parse_button(const std::string& s) {
    static const char* names[] = {"A", "B", "X", "Y", "PLUS", "MINUS", "UP", "DOWN", "LEFT", "RIGHT",
                                  "L", "R", "ZL", "ZR", "LSTICK", "RSTICK"};
    for (int i = 0; i < BTN_COUNT; i++)
        if (strcasecmp(s.c_str(), names[i]) == 0) return (Button)i;
    return BTN_COUNT;
}

void load_script(const char* path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        Cmd c;
        ss >> c.op;
        if (c.op.empty() || c.op[0] == '#') continue;
        if (c.op == "type") {
            std::getline(ss, c.a);
            c.a = util::trim(c.a);
        } else {
            ss >> c.a >> c.b;
        }
        g_script.push_back(c);
    }
    g_scripted = true;
    log_message(LOG_OK, "Platform", "Loaded script with %zu commands", g_script.size());
}

void step_script(RawInput& raw) {
    raw.held |= g_script_held;
    if (g_hold_frames > 0 && --g_hold_frames == 0) g_script_held = 0;
    if (g_tap_pending) {
        raw.touch = g_tap_phase < 2;
        raw.tx = g_tap_x;
        raw.ty = g_tap_y;
        if (++g_tap_phase > 2) g_tap_pending = false;
        return;
    }
    if (g_wait > 0) {
        g_wait--;
        return;
    }
    while (!g_script.empty() && g_wait == 0 && g_hold_frames == 0 && !g_tap_pending) {
        Cmd c = g_script.front();
        g_script.pop_front();
        if (c.op == "wait") {
            g_wait = std::max(1, atoi(c.a.c_str()));
        } else if (c.op == "press" || c.op == "hold") {
            Button b = parse_button(c.a);
            if (b == BTN_COUNT) continue;
            g_script_held = bit(b);
            g_hold_frames = c.op == "hold" ? std::max(1, atoi(c.b.c_str())) : 2;
            raw.held |= g_script_held;
        } else if (c.op == "tap") {
            g_tap_pending = true;
            g_tap_phase = 0;
            g_tap_x = (float)atof(c.a.c_str());
            g_tap_y = (float)atof(c.b.c_str());
        } else if (c.op == "type") {
            if (g_text_state == TEXT_ACTIVE) {
                g_text = c.a;
                g_text_state = TEXT_DONE;
            }
        } else if (c.op == "shot") {
            g_shot = c.a;
            g_wait = 1;
        } else if (c.op == "quit") {
            g_quit = true;
        }
    }
    if (g_script.empty() && g_wait == 0 && g_hold_frames == 0 && !g_tap_pending && g_shot.empty()) {
        // Script finished: exit on the next frame.
        static int grace = 2;
        if (--grace <= 0) g_quit = true;
    }
}

uint32_t keyboard_buttons() {
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    uint32_t h = 0;
    if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_Z]) h |= bit(BTN_A);
    if (k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_X]) h |= bit(BTN_B);
    if (k[SDL_SCANCODE_C]) h |= bit(BTN_X);
    if (k[SDL_SCANCODE_V]) h |= bit(BTN_Y);
    if (k[SDL_SCANCODE_EQUALS] || k[SDL_SCANCODE_TAB]) h |= bit(BTN_PLUS);
    if (k[SDL_SCANCODE_MINUS]) h |= bit(BTN_MINUS);
    if (k[SDL_SCANCODE_UP]) h |= bit(BTN_UP);
    if (k[SDL_SCANCODE_DOWN]) h |= bit(BTN_DOWN);
    if (k[SDL_SCANCODE_LEFT]) h |= bit(BTN_LEFT);
    if (k[SDL_SCANCODE_RIGHT]) h |= bit(BTN_RIGHT);
    if (k[SDL_SCANCODE_Q]) h |= bit(BTN_L);
    if (k[SDL_SCANCODE_E]) h |= bit(BTN_R);
    if (k[SDL_SCANCODE_1]) h |= bit(BTN_ZL);
    if (k[SDL_SCANCODE_3]) h |= bit(BTN_ZR);
    return h;
}

uint32_t pad_buttons() {
    if (!g_pad) return 0;
    uint32_t h = 0;
    auto b = [&](SDL_GameControllerButton sb, Button ours) {
        if (SDL_GameControllerGetButton(g_pad, sb)) h |= bit(ours);
    };
    // Nintendo layout: A is the right face button.
    b(SDL_CONTROLLER_BUTTON_B, BTN_A);
    b(SDL_CONTROLLER_BUTTON_A, BTN_B);
    b(SDL_CONTROLLER_BUTTON_Y, BTN_X);
    b(SDL_CONTROLLER_BUTTON_X, BTN_Y);
    b(SDL_CONTROLLER_BUTTON_START, BTN_PLUS);
    b(SDL_CONTROLLER_BUTTON_BACK, BTN_MINUS);
    b(SDL_CONTROLLER_BUTTON_DPAD_UP, BTN_UP);
    b(SDL_CONTROLLER_BUTTON_DPAD_DOWN, BTN_DOWN);
    b(SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_LEFT);
    b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, BTN_RIGHT);
    b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BTN_L);
    b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BTN_R);
    if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) h |= bit(BTN_ZL);
    if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) h |= bit(BTN_ZR);
    return h;
}

float pad_axis(SDL_GameControllerAxis a, bool invert) {
    if (!g_pad) return 0;
    float v = SDL_GameControllerGetAxis(g_pad, a) / 32767.0f;
    if (std::fabs(v) < 0.15f) v = 0;
    return invert ? -v : v;
}

}  // namespace

bool init() {
    if (const char* c = getenv("COFFEEFLIX_CONTENT")) g_content = c;
    if (const char* d = getenv("COFFEEFLIX_DATA")) g_data = d;
    util::make_dirs(g_data);
    if (const char* s = getenv("COFFEEFLIX_SCRIPT")) load_script(s);
    return true;
}

void post_video_init(SDL_Window*, SDL_Renderer*) {
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_pad = SDL_GameControllerOpen(i);
            break;
        }
    }
    SDL_StopTextInput();
}

void shutdown() {
    if (g_pad) SDL_GameControllerClose(g_pad);
}

bool running() { return !g_quit; }

void poll(RawInput& raw) {
    raw = RawInput();
    g_wheel = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT: g_quit = true; break;
            case SDL_MOUSEWHEEL: g_wheel += e.wheel.preciseY; break;
            case SDL_TEXTINPUT:
                if (g_text_state == TEXT_ACTIVE) g_text += e.text.text;
                break;
            case SDL_KEYDOWN:
                if (g_text_state == TEXT_ACTIVE) {
                    if (e.key.keysym.sym == SDLK_RETURN) g_text_state = TEXT_DONE;
                    else if (e.key.keysym.sym == SDLK_ESCAPE) g_text_state = TEXT_CANCELLED;
                    else if (e.key.keysym.sym == SDLK_BACKSPACE && !g_text.empty()) {
                        // drop one UTF-8 code point
                        size_t n = g_text.size() - 1;
                        while (n > 0 && ((unsigned char)g_text[n] & 0xC0) == 0x80) n--;
                        g_text.resize(n);
                    }
                    if (g_text_state != TEXT_ACTIVE) SDL_StopTextInput();
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (!g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
                break;
            default: break;
        }
    }

    if (g_text_state != TEXT_ACTIVE) {
        raw.held = keyboard_buttons() | pad_buttons();
        raw.lx = pad_axis(SDL_CONTROLLER_AXIS_LEFTX, false);
        raw.ly = pad_axis(SDL_CONTROLLER_AXIS_LEFTY, true);
        raw.rx = pad_axis(SDL_CONTROLLER_AXIS_RIGHTX, false);
        raw.ry = pad_axis(SDL_CONTROLLER_AXIS_RIGHTY, true);
        raw.wheel = g_wheel;

        int mx = 0, my = 0;
        Uint32 mb = SDL_GetMouseState(&mx, &my);
        SDL_Renderer* r = SDL_GetRenderer(SDL_GetMouseFocus());
        float lx = (float)mx, ly = (float)my;
        if (r) SDL_RenderWindowToLogical(r, mx, my, &lx, &ly);
        if (SDL_GetMouseFocus()) {
            raw.pointer = true;
            raw.px = lx;
            raw.py = ly;
            if (mb & SDL_BUTTON(SDL_BUTTON_LEFT)) {
                raw.touch = true;
                raw.tx = lx;
                raw.ty = ly;
            }
        }
    }
    if (g_scripted) {
        raw.pointer = false;
        step_script(raw);
    }
}

const char* name() { return "Desktop"; }
bool is_wiiu() { return false; }
std::string content_dir() { return g_content; }
std::string data_dir() { return g_data; }
std::string media_root() { return g_data + "/media"; }

int volumes(Volume* out, int max) {
    int n = 0;
    std::string media = media_root();
    util::make_dirs(media);
    if (n < max) out[n++] = Volume{"CoffeeFlix folder", media, 0xe2c7};
    const char* home = getenv("HOME");
    if (home && n < max) out[n++] = Volume{"Home", home, 0xe88a};
    return n;
}

bool network_connected() { return true; }
std::string ip_address() { return "127.0.0.1"; }
void rumble(float) {}

bool scripted() { return g_scripted; }
float fixed_dt() { return g_scripted ? 1.0f / 60.0f : 0.0f; }

const char* screenshot_request() { return g_shot.empty() ? nullptr : g_shot.c_str(); }

void save_screenshot(SDL_Renderer* r, const char* path) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(r, &w, &h);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (s && SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_RGBA32, s->pixels, s->pitch) == 0) {
        IMG_SavePNG(s, path);
        log_message(LOG_OK, "Platform", "Saved screenshot %s", path);
    } else {
        log_message(LOG_ERROR, "Platform", "Screenshot failed: %s", SDL_GetError());
    }
    if (s) SDL_FreeSurface(s);
    g_shot.clear();
}

// --- text input ------------------------------------------------------------

void text_input_start(const TextInputOptions& opts) {
    g_text = opts.initial;
    g_text_state = TEXT_ACTIVE;
    if (!g_scripted) SDL_StartTextInput();
}

TextInputState text_input_state() { return g_text_state; }
const std::string& text_input_value() { return g_text; }
bool text_input_native() { return false; }

void text_input_cancel() {
    if (g_text_state == TEXT_ACTIVE) SDL_StopTextInput();
    g_text_state = TEXT_IDLE;
}

void text_input_reset() { g_text_state = TEXT_IDLE; }

}  // namespace platform
