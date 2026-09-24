// Text rendering through the shared atlas: glyphs are rasterized once with
// SDL_ttf (at the physical output resolution) and then drawn as batched quads.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "gfx/gfx.hpp"

namespace text {

enum Weight { REGULAR, MEDIUM, SEMIBOLD, BOLD, ICONS, WEIGHT_COUNT };

struct Font {
    Weight weight = REGULAR;
    float size = 20;
};

constexpr Font font(Weight w, float size) { return Font{w, size}; }

enum Align { LEFT, CENTER, RIGHT };

// Loads the font files; returns false if the regular face is missing.
bool init(const std::string& content_dir);
void shutdown();

float line_height(Font f);
float ascent(Font f);
float measure(Font f, std::string_view s);

// Draws a single line; `y` is the top of the line box. Returns the advance.
float draw(Font f, float x, float y, std::string_view s, gfx::Color c, Align align = LEFT);
// Single line, truncated with an ellipsis to fit max_w.
float draw_fit(Font f, float x, float y, float max_w, std::string_view s, gfx::Color c, Align align = LEFT);
// Word-wrapped paragraph limited to max_lines (last line ellipsized).
// Returns the height used.
float draw_wrapped(Font f, const gfx::Rect& box, std::string_view s, gfx::Color c, int max_lines = 0,
                   Align align = LEFT, float line_spacing = 1.0f);
float measure_wrapped(Font f, float width, std::string_view s, int max_lines = 0, float line_spacing = 1.0f);

std::string ellipsize(Font f, std::string_view s, float max_w);
std::vector<std::string> wrap(Font f, std::string_view s, float width, int max_lines = 0);

// Material icon glyph centered on (cx, cy).
void icon(int codepoint, float size, float cx, float cy, gfx::Color c);

// Soft text shadow helper for text over images.
float draw_shadowed(Font f, float x, float y, std::string_view s, gfx::Color c, Align align = LEFT,
                    gfx::Color shadow = gfx::Color(0, 0, 0, 160));

// Decodes one UTF-8 code point and advances i.
uint32_t next_codepoint(std::string_view s, size_t& i);

}  // namespace text

// Material Icons code points used across the app.
namespace ic {
constexpr int HOME = 0xe88a, MOVIE = 0xe02c, MUSIC = 0xe405, PHOTO = 0xe410, RADIO = 0xe03e,
              PODCASTS = 0xf048, LIVE_TV = 0xe639, SETTINGS = 0xe8b8, SEARCH = 0xe8b6, PLAY = 0xe037,
              PAUSE = 0xe034, FOLDER = 0xe2c7, SMART_DISPLAY = 0xf06a, SUBSCRIPTIONS = 0xe064,
              BOOK = 0xea19, LAN = 0xeb2f, DNS = 0xe875, VIDEO_LIBRARY = 0xe04a, ALBUM = 0xe019,
              FAVORITE = 0xe87d, REPLAY_10 = 0xe059, FORWARD_10 = 0xe056, SKIP_NEXT = 0xe044,
              SKIP_PREV = 0xe045, VOLUME_UP = 0xe050, ARROW_BACK = 0xe5c4, BACKSPACE = 0xe14a,
              RETURN = 0xe31b, SPACE = 0xe256, TRENDING = 0xe8e5, HISTORY = 0xe889, STAR = 0xe838,
              WIFI = 0xe63e, CLOUD = 0xe2bd, TV = 0xe333, PERSON = 0xe7fd, LOCK = 0xe897, CHECK = 0xe5ca,
              CLOSE = 0xe5cd, ERROR = 0xe000, INFO = 0xe88e, REFRESH = 0xe5d5, HEADPHONES = 0xf01f,
              QUEUE_MUSIC = 0xe03d, EXPLORE = 0xe87a, GRID = 0xe9b0, LIBRARY_MUSIC = 0xe030,
              COLLECTIONS = 0xe3b6, STORIES = 0xe666, COMPUTER = 0xe30a, STORAGE = 0xe1db, USB = 0xe1e0,
              SD_CARD = 0xe623, LANGUAGE = 0xe894, PALETTE = 0xe40a, VOLUME_OFF = 0xe04f,
              CC = 0xe01c, HQ = 0xe024, SPEED = 0xe9e4, FULLSCREEN = 0xe5d0, MIC = 0xe029, CLOUDY = 0xe2bd;
// Filled/extra glyphs available in MaterialIcons-Regular.
constexpr int CHEVRON_RIGHT = 0xe5cc, CHEVRON_LEFT = 0xe5cb, EXPAND_MORE = 0xe5cf, MORE_HORIZ = 0xe5d3,
              ADD = 0xe145, REMOVE = 0xe15b, DELETE = 0xe872, KEYBOARD = 0xe312, SHUFFLE = 0xe043,
              REPEAT = 0xe040, STOP = 0xe047, FAST_FORWARD = 0xe01f, FAST_REWIND = 0xe020,
              IMAGE = 0xe3f4, AUDIOTRACK = 0xe3a1, THEATERS = 0xe8da, VISIBILITY = 0xe8f4,
              LINK = 0xe157, QR = 0xef6b, EQUALIZER = 0xe01d, GRAPHIC_EQ = 0xe1b8, DESCRIPTION = 0xe873,
              WARNING = 0xe002, BOLT = 0xea0b, SPORTS_ESPORTS = 0xea28, NEWSPAPER = 0xeb81,
              PUBLIC = 0xe80b, TUNE = 0xe429, SUBTITLES = 0xe048, HD = 0xe052, PEOPLE = 0xe7fb,
              VIDEOCAM = 0xe04b, SCHEDULE = 0xe8b5, CAST = 0xe307, KEY = 0xe73c, LOGOUT = 0xe9ba,
              CIRCLE = 0xef4a, NAVIGATE_NEXT = 0xe409, ZOOM_IN = 0xe8ff, ZOOM_OUT = 0xe900,
              CLOUD_DOWNLOAD = 0xe2c0, LIST = 0xe896, PLAYLIST_PLAY = 0xe05f, RSS = 0xe0e5,
              VOLUME_DOWN = 0xe04d, BRIGHTNESS = 0xe3ab, AUTO_AWESOME = 0xe65f, MOVIE_FILTER = 0xe43a,
              LOCAL_CAFE = 0xe541, GAMEPAD = 0xe30f, CHECK_CIRCLE = 0xe86c, ERROR_OUTLINE = 0xe001,
              WIFI_OFF = 0xe648, FAVORITE_BORDER = 0xe87e, ARROW_FORWARD = 0xe5c8, CAPSLOCK = 0xe318,
              TRANSLATE = 0xe8e2, PLAY_CIRCLE = 0xe038, SENSORS = 0xe51e, MENU = 0xe5d2, MUSIC_VIDEO = 0xe063,
              SPORTS_SOCCER = 0xea2f, PERSON_ADD = 0xe7fe;
}  // namespace ic
