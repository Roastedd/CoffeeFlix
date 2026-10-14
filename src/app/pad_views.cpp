#include "app/pad_views.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/i18n.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "gfx/text.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "platform/text_input.hpp"
#include "player/player.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace pad_views {

namespace {

View g_view = VIEW_HYBRID;
bool g_playback_screen = false;
bool g_suspended = false;
bool g_tv_scene = false;
SDL_Renderer* g_renderer = nullptr;
int g_remote_sound = 0;           // 0 the TV, 1 the GamePad, 2 both (while a video plays on the TV)
bool g_sleeping = false;
SDL_Texture* g_target = nullptr;  // the GamePad's own picture in the remote view
bool g_scene = false;             // it is the GamePad's now (and the platform was told)

// Settings > Sound plays on: 0 the TV and the GamePad, 1 the TV, 2 the GamePad.
constexpr const char* SOUND_KEY = "sound_output";
// Settings > GamePad screen sleeps: seconds of no input, 0 never.
int sleep_after() { return (int)std::max<int64_t>(0, store::get_int("pad_sleep", 0)); }
int sound_output() { return (int)std::clamp<int64_t>(store::get_int(SOUND_KEY, 0), 0, 2); }

const char* sound_label(bool playback) {
    const int output = playback ? (g_remote_sound == 0 ? 1 : g_remote_sound == 1 ? 2 : 0) : sound_output();
    switch (output) {
    case 1: return tr("Sound: TV only");
    case 2: return tr("Sound: GamePad only");
    default: return tr("Sound: TV and GamePad");
    }
}

void make_target() {
    if (g_target || !g_renderer) return;
    g_target = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, (int)ui::W, (int)ui::H);
    if (!g_target) {
        log_message(LOG_ERROR, "Pad", "The GamePad's own picture can't be made: %s", SDL_GetError());
        return;
    }
    SDL_SetTextureScaleMode(g_target, SDL_ScaleModeLinear);
    clear_pad_scene();
    log_message(LOG_OK, "Pad", "The GamePad's own picture is made (%dx%d)", (int)ui::W, (int)ui::H);
}

void drop_target() {
    if (g_scene) platform::set_pad_texture(nullptr);  // before the texture goes
    g_scene = false;
    if (g_target) SDL_DestroyTexture(g_target);
    g_target = nullptr;
}

bool remote_view() { return g_view == VIEW_HYBRID && g_playback_screen; }
const char* view_name(View v) {
    return v == VIEW_HYBRID ? "Default" : v == VIEW_TV_ONLY ? "TV only" : "Mirror";
}

void prepare_scene() {
    if (!available()) return;
    if (g_sleeping && !(remote_view() && g_playback_screen)) g_sleeping = false;
    const bool keyboard = platform::text_input_state() == platform::TEXT_ACTIVE;
    const bool remote = remote_view() && !g_suspended;
    if (remote && !g_target) make_target();
    // Reuse the texture across hybrid transitions; release it for explicit non-remote modes.
    if (!remote && g_target && g_view != VIEW_HYBRID) drop_target();
    const bool scene = remote && g_target && !keyboard;
    if (scene != g_scene) {
        g_scene = scene;
        platform::set_pad_texture(scene ? g_target : nullptr);
    }
}

}  // namespace

bool available() { return platform::has_pad_outputs(); }
View view() { return g_view; }

void set_view(View v) {
    if (v != VIEW_HYBRID && v != VIEW_MIRROR && v != VIEW_TV_ONLY) v = VIEW_HYBRID;
    if (v == g_view) return;
    g_view = v;
    log_message(LOG_OK, "Pad", "View: %s", view_name(v));
    if (v == VIEW_TV_ONLY) ui::toast(tr("GamePad screen off. Press - for the panel"), ic::TV);
    else if (v == VIEW_HYBRID) ui::toast(tr("Mirror while browsing, remote while playing"), ic::GAMEPAD);
    else ui::toast(tr("GamePad screen back on"), ic::GAMEPAD);
}

Battery battery() {
    static int last_level = -2;
    const int level = platform::pad_status().battery;
    Battery b;
    if (level != last_level) {
        if (last_level != -2) log_message(LOG_OK, "Pad", "GamePad battery reading %d -> %d", last_level, level);
        last_level = level;
    }
    if (level < 0) return b;
    if (level == 0) b.charging = true;
    else if (level >= 6) { b.full = true; b.percent = 100; }
    else b.percent = 10 + 20 * (level - 1);
    b.low = b.percent >= 0 && b.percent <= 10;
    return b;
}

float draw_battery(float right, float cy, float scale, bool words) {
    const Battery b = battery();
    if (!b.known()) return 0;
    const ui::Theme& t = ui::theme();
    const float bw = 38 * scale, bh = 19 * scale;
    const ui::Color tint = b.low ? t.bad : b.charging || b.full ? t.good : t.text2;
    std::string label = b.charging ? tr("Charging") : b.full ? tr("Fully charged") : util::fmt(tr("About %d%%"), b.percent);
    const text::Font font = ui::font::small_bold;
    const float label_w = words ? text::measure(font, label) + 10 * scale : 0;
    const float x = right - bw - 5 * scale - label_w;  // the gauge's left edge
    if (words) text::draw(font, x + bw + 5 * scale + 10 * scale, cy - text::line_height(font) * 0.5f, label, tint);
    const ui::Rect body(x, cy - bh * 0.5f, bw, bh);
    gfx::stroke_rrect(body, 4 * scale, 2, tint);
    gfx::fill_rrect(ui::Rect(x + bw + 1, cy - 4 * scale, 4 * scale, 8 * scale), 1.5f * scale, tint);  // the nub
    const float fill = b.charging ? 0.0f : b.percent / 100.0f;
    if (fill > 0) gfx::fill_rrect(ui::Rect(x + 3, body.y + 3, (bw - 6) * fill, bh - 6), 2 * scale, tint);
    if (b.charging) text::icon(ic::BOLT, 18 * scale, x + bw * 0.5f, cy, tint);
    return right - x;
}

void open_drawer() {
    using screens::MenuItem;
    auto radio = [](View v) { return g_view == v ? ic::RADIO_CHECKED : ic::RADIO_UNCHECKED; };
    std::vector<MenuItem> items;
    items.push_back({tr("Default"), radio(VIEW_HYBRID), [] { set_view(VIEW_HYBRID); },
                     tr("Mirror while browsing, remote while playing")});
    items.push_back({tr("Mirror"), radio(VIEW_MIRROR), [] { set_view(VIEW_MIRROR); }, tr("Both screens show the same")});
    items.push_back({tr("TV only"), radio(VIEW_TV_ONLY), [] { set_view(VIEW_TV_ONLY); },
                     tr("GamePad screen and sound off, to save its battery")});
    if (g_view != VIEW_TV_ONLY) { // TV only fixes the output to the TV; don't offer an ineffective sound toggle.
        const bool playback_sound = g_scene && g_tv_scene;
        items.push_back({sound_label(playback_sound), ic::VOLUME_UP, [playback_sound] {
            if (playback_sound) cycle_remote_sound();
            else store::set_int(SOUND_KEY, (sound_output() + 1) % 3);
        }, ""});
    }
    screens::show_menu(tr("GamePad"), "", std::move(items), [](float x, float y, float w) {
        // The GamePad's battery: the gauge and how full (the console gives 7 steps), or that it is charging.
        if (!battery().known()) return 0.0f;
        text::draw_fit(ui::font::small, x + 4, y + 5, w * 0.45f, tr("GamePad battery"), ui::theme().text2);
        draw_battery(x + w - 4, y + 15, 1.0f, true);
        return 34.0f;
    });
}

void init(SDL_Renderer* renderer) {
    g_renderer = renderer;
    g_view = VIEW_HYBRID;
    g_playback_screen = g_tv_scene = g_sleeping = g_suspended = false;
    g_remote_sound = 0;
}

void shutdown() {
    drop_target();
    g_renderer = nullptr;
}

bool pad_scene() { return g_scene; }
void begin_frame(bool playback_screen) {
    g_playback_screen = playback_screen;
    prepare_scene();
}
void set_tv_scene(bool on) {
    if (on != g_tv_scene)
        log_message(LOG_OK, "Pad", on ? "Playback on the TV, controls on the GamePad" :
                    g_scene ? "Browsing on the GamePad, TV idle" : "Browsing mirrored on TV and GamePad");
    g_tv_scene = on;
}

PadPass::PadPass(bool on) : on_(on && g_target) {
    if (on_) gfx::set_target(g_target);
}
PadPass::~PadPass() {
    if (on_) gfx::set_target(nullptr);
}

void clear_pad_scene() {
    if (!g_target) return;
    gfx::set_target(g_target);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    gfx::set_target(nullptr);
}

void update() {
    if (!available()) return;
    // The frame has already been drawn. Target changes belong to begin_frame(),
    // so menu actions cannot detach/free the picture before it is presented.
    const bool keyboard = platform::text_input_state() == platform::TEXT_ACTIVE;
    if (g_suspended) return;

    const int sound = sound_output();
    const bool tv_only = g_view == VIEW_TV_ONLY;
    platform::PadOutputs o;
    if (tv_only && !keyboard) {
        o.screen = platform::PAD_SCREEN_OFF;
        o.picture = platform::PAD_PICTURE_NOT_SENT;
    }
    const bool video_on_tv = remote_view() && g_tv_scene;
    if (g_sleeping && !keyboard && video_on_tv) {
        o.screen = platform::PAD_SCREEN_STANDBY;
        o.picture = platform::PAD_PICTURE_NOT_SENT;
    }
    if (video_on_tv) {
        o.pad_sound = g_remote_sound != 0;
        o.tv_sound = g_remote_sound != 1;
    } else {
        o.pad_sound = !tv_only && sound != 1;
        o.tv_sound = tv_only || sound != 2;
    }
    platform::set_pad_outputs(o);
}

int remote_sound() { return g_remote_sound; }
void cycle_remote_sound() { g_remote_sound = (g_remote_sound + 1) % 3; }

void sleep_pad() {
    if (!remote_view() || !g_tv_scene) return;
    g_sleeping = true;
    log_message(LOG_OK, "Pad", "The GamePad's screen sleeps");
}
bool pad_sleeping() { return g_sleeping; }

void filter_input(Input& in) {
    if (!available()) return;
    if (g_sleeping) {
        const bool stick = std::fabs(in.lx) > 0.3f || std::fabs(in.ly) > 0.3f || std::fabs(in.rx) > 0.3f || std::fabs(in.ry) > 0.3f;
        const player::State st = player::state();
        if (in.pressed || in.touch_began || in.touching || stick || !g_tv_scene || st == player::ENDED || st == player::FAILED) {
            g_sleeping = false;
            log_message(LOG_OK, "Pad", "The GamePad's screen wakes");
            in.eat_all();  // the press that woke it was for that, not for the screen under it
        }
        return;
    }
    const int after = sleep_after();
    if (after > 0 && remote_view() && g_tv_scene && player::state() == player::PLAYING && in.idle_time > after) {
        g_sleeping = true;
        log_message(LOG_OK, "Pad", "The GamePad's screen sleeps after %d s without input", after);
    }
}

void suspend(bool on) { g_suspended = on; }

}  // namespace pad_views
