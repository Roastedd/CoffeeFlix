#include "app/sound_settings.hpp"

#include <algorithm>
#include <cmath>

#include "audio/mixer.hpp"
#include "core/store.hpp"
#include "core/util.hpp"

namespace sound_settings {

namespace {
constexpr const char* ON_KEY = "ui_sounds";           // kept from before there were two styles: off is off
constexpr const char* STYLE_KEY = "ui_sound_style";
constexpr const char* VOLUME_KEY = "sfx_volume";      // percent
constexpr const char* TICKS_KEY = "sfx_ticks";
constexpr const char* MEDIA_KEY = "media_volume";     // percent
}  // namespace

Mode mode() {
    if (!store::get_bool(ON_KEY, true)) return OFF;
    return store::get_int(STYLE_KEY, SUBTLE) == CLASSIC ? CLASSIC : SUBTLE;
}

void set_mode(Mode m) {
    store::set_bool(ON_KEY, m != OFF);
    if (m != OFF) store::set_int(STYLE_KEY, m);
    apply();
}

float volume() { return (float)std::clamp<int64_t>(store::get_int(VOLUME_KEY, 70), 0, 100) / 100.0f; }
void set_volume(float v) {
    store::set_int(VOLUME_KEY, (int64_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 100));
    apply();
}
bool ticks() { return store::get_bool(TICKS_KEY, true); }
void set_ticks(bool on) {
    store::set_bool(TICKS_KEY, on);
    apply();
}
constexpr int MEDIA_MAX_PERCENT = (int)(audio::MAX_MEDIA_VOLUME * 100);
float media_volume() { return (float)std::clamp<int64_t>(store::get_int(MEDIA_KEY, 100), 0, MEDIA_MAX_PERCENT) / 100.0f; }
void set_media_volume(float v) {
    store::set_int(MEDIA_KEY, (int64_t)std::lround(std::clamp(v, 0.0f, audio::MAX_MEDIA_VOLUME) * 100));
    audio::set_media_volume(media_volume());
}
float media_slider() { return media_volume() / audio::MAX_MEDIA_VOLUME; }
void set_media_slider(float pos) { set_media_volume(std::clamp(pos, 0.0f, 1.0f) * audio::MAX_MEDIA_VOLUME); }
float nudge_media_volume(int steps) {
    // On the 5% grid, whatever it was before (a finger can leave it at 73%)
    const int pct = (int)std::lround(media_volume() * 100);
    const int grid = steps > 0 ? pct / 5 * 5 + 5 * steps : (pct + 4) / 5 * 5 + 5 * steps;
    set_media_volume((float)std::clamp(grid, 0, MEDIA_MAX_PERCENT) / 100.0f);
    return media_volume();
}
void toggle_media_mute() {
    static float before = 1.0f;
    const float v = media_volume();
    if (v > 0.001f) {
        before = v;
        set_media_volume(0);
    } else {
        set_media_volume(before > 0.001f ? before : 1.0f);
    }
}

void apply() {
    const Mode m = mode();
    audio::set_sfx_enabled(m != OFF);
    audio::set_sfx_style(m == CLASSIC ? audio::STYLE_CLASSIC : audio::STYLE_SUBTLE);
    audio::set_sfx_volume(volume());
    audio::set_sfx_ticks(ticks());
    audio::set_sfx_kids(store::kids_profile());
    audio::set_media_volume(media_volume());
}

void preview() {
    static double last = -1;
    const double now = util::now_seconds();
    if (last >= 0 && now - last < 0.25) return;
    last = now;
    audio::play(audio::SFX_SELECT);
}

}  // namespace sound_settings
