#include "gfx/text.hpp"

#include <SDL2/SDL_ttf.h>

#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "core/i18n.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace text {

namespace {

struct Glyph {
    gfx::AtlasRegion region{0, 0, 0, 0};
    float off_x = 0, off_y = 0;  // raster px, relative to pen position / line top
    float advance = 0;           // raster px
    bool visible = false;
};

struct Metrics {
    float ascent, height;  // raster px
};

const char* FILES[WEIGHT_COUNT] = {
    "fonts/Inter-Regular.otf", "fonts/Inter-Medium.otf", "fonts/Inter-SemiBold.otf",
    "fonts/Inter-Bold.otf", "fonts/MaterialIcons-Regular.ttf",
};

std::vector<uint8_t> g_font_data[WEIGHT_COUNT];
std::unordered_map<uint32_t, TTF_Font*> g_fonts;  // (weight << 16) | raster size
std::unordered_map<uint64_t, Glyph> g_glyphs;
std::unordered_map<uint32_t, Metrics> g_metrics;
int g_atlas_gen = 0;

// Chinese, Japanese and Korean come from the system's fonts (Inter has none of it).
platform::FontFile g_cjk[platform::CJK_FONT_COUNT];
bool g_has_cjk[platform::CJK_FONT_COUNT] = {};
std::unordered_map<uint32_t, TTF_Font*> g_cjk_fonts;  // (font << 24) | (bold << 16) | raster size

inline int raster_size(Font f) { return std::max(4, (int)std::lround(f.size * gfx::output_scale())); }
inline float raster_scale(Font f) { return (float)raster_size(f) / f.size; }

TTF_Font* get_font(Weight w, int px) {
    if (w < 0 || w >= WEIGHT_COUNT) w = REGULAR;
    if (g_font_data[w].empty()) w = REGULAR;
    if (g_font_data[w].empty()) return nullptr;
    uint32_t key = ((uint32_t)w << 16) | (uint32_t)px;
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;
    SDL_RWops* rw = SDL_RWFromConstMem(g_font_data[w].data(), (int)g_font_data[w].size());
    TTF_Font* font = TTF_OpenFontRW(rw, 1, px);
    if (!font) {
        log_message(LOG_ERROR, "Text", "TTF_OpenFont failed: %s", TTF_GetError());
    } else {
        TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
    }
    g_fonts[key] = font;
    return font;
}

const Metrics& get_metrics(Weight w, int px) {
    uint32_t key = ((uint32_t)w << 16) | (uint32_t)px;
    auto it = g_metrics.find(key);
    if (it != g_metrics.end()) return it->second;
    Metrics m{(float)px * 0.8f, (float)px * 1.2f};
    if (TTF_Font* font = get_font(w, px)) {
        m.ascent = (float)TTF_FontAscent(font);
        m.height = (float)TTF_FontHeight(font);
    }
    return g_metrics[key] = m;
}

TTF_Font* get_cjk_font(int which, bool bold, int px) {
    if (!g_has_cjk[which]) return nullptr;
    uint32_t key = ((uint32_t)which << 24) | ((uint32_t)bold << 16) | (uint32_t)px;
    auto it = g_cjk_fonts.find(key);
    if (it != g_cjk_fonts.end()) return it->second;
    const platform::FontFile& f = g_cjk[which];
    SDL_RWops* rw = f.data ? SDL_RWFromConstMem(f.data, (int)f.size) : SDL_RWFromFile(f.path.c_str(), "rb");
    TTF_Font* font = rw ? TTF_OpenFontRW(rw, 1, px) : nullptr;
    if (!font) {
        log_message(LOG_ERROR, "Text", "Couldn't open system font %d: %s", which, TTF_GetError());
        g_has_cjk[which] = false;
    } else {
        TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
        if (bold) TTF_SetFontStyle(font, TTF_STYLE_BOLD);  // they come in one weight
    }
    return g_cjk_fonts[key] = font;
}

// Which system font to try first: Chinese characters are drawn differently in each language.
int cjk_order() {
    static int gen = -1, order = 0;
    if (gen != i18n::generation()) {
        gen = i18n::generation();
        std::string_view lang = i18n::current().code;
        order = lang == "zh" ? 1 : lang == "ko" ? 2 : 0;
    }
    return order;
}

void check_atlas_generation() {
    if (g_atlas_gen != gfx::atlas_generation()) {
        g_glyphs.clear();
        g_atlas_gen = gfx::atlas_generation();
    }
}

const Glyph& get_glyph(Weight w, int px, uint32_t cp) {
    check_atlas_generation();
    int order = cjk_order();
    uint64_t key = ((uint64_t)w << 56) | ((uint64_t)px << 32) | ((uint64_t)order << 24) | cp;
    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end()) return it->second;

    Glyph g;
    TTF_Font* font = get_font(w, px);
    if (!font) return g_glyphs[key] = g;

    float shift = 0;  // lines up a system font's baseline with Inter's
    if (!TTF_GlyphIsProvided32(font, cp) && w != ICONS) {
        static const int ORDERS[3][3] = {{0, 1, 2}, {1, 0, 2}, {2, 0, 1}};
        for (int which : ORDERS[order]) {
            TTF_Font* cjk = get_cjk_font(which, w == SEMIBOLD || w == BOLD, px);
            if (cjk && TTF_GlyphIsProvided32(cjk, cp)) {
                shift = (float)(TTF_FontAscent(font) - TTF_FontAscent(cjk));
                font = cjk;
                break;
            }
        }
    }
    if (!TTF_GlyphIsProvided32(font, cp)) {
        // Emoji and pictographs (all over YouTube titles and captions) have no font here: leave
        // them out rather than drawing a box each. Other missing characters still show one.
        bool pictograph = (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) ||
                          (cp >= 0x2B00 && cp <= 0x2BFF) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x200D ||
                          cp == 0x20E3 || (cp >= 0xE0020 && cp <= 0xE007F);
        if (pictograph) return g_glyphs[key] = g;
        if (cp == 0x00A0 || cp == 0x2009 || cp == 0x202F) cp = ' ';
        else if (cp == 0x2019 || cp == 0x2018) cp = '\'';
        else if (cp == 0x201C || cp == 0x201D) cp = '"';
        else if (cp == 0x2013 || cp == 0x2014) cp = '-';
        else if (cp >= 0x80) cp = 0x25A1;  // white square as "tofu"
        if (!TTF_GlyphIsProvided32(font, cp)) cp = '?';
    }

    int minx = 0, maxx = 0, miny = 0, maxy = 0, advance = 0;
    TTF_GlyphMetrics32(font, cp, &minx, &maxx, &miny, &maxy, &advance);
    g.advance = (float)advance;

    SDL_Surface* s = TTF_RenderGlyph32_Blended(font, cp, SDL_Color{255, 255, 255, 255});
    if (s) {
        SDL_Surface* rgba = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(s);
        if (rgba) {
            // Trim fully transparent borders to save atlas space.
            int x0 = rgba->w, y0 = rgba->h, x1 = -1, y1 = -1;
            const uint8_t* px_data = (const uint8_t*)rgba->pixels;
            for (int y = 0; y < rgba->h; y++) {
                const uint8_t* row = px_data + (size_t)y * rgba->pitch;
                for (int x = 0; x < rgba->w; x++) {
                    if (row[x * 4 + 3]) {
                        x0 = std::min(x0, x); x1 = std::max(x1, x);
                        y0 = std::min(y0, y); y1 = std::max(y1, y);
                    }
                }
            }
            if (x1 >= x0 && y1 >= y0) {
                int gw = x1 - x0 + 1, gh = y1 - y0 + 1;
                gfx::AtlasRegion region;
                if (!gfx::atlas_alloc(gw, gh, &region)) {
                    log_message(LOG_WARNING, "Text", "Glyph atlas full, resetting");
                    gfx::atlas_reset_dynamic();
                    g_glyphs.clear();
                    g_atlas_gen = gfx::atlas_generation();
                    gfx::atlas_alloc(gw, gh, &region);
                }
                gfx::atlas_upload(region, px_data + (size_t)y0 * rgba->pitch + x0 * 4, rgba->pitch);
                g.region = region;
                g.off_x = (float)(std::min(0, minx) + x0);
                g.off_y = (float)y0 + shift;
                g.visible = true;
            }
            SDL_FreeSurface(rgba);
        }
    }
    return g_glyphs[key] = g;
}

}  // namespace

uint32_t next_codepoint(std::string_view s, size_t& i) {
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra && i < s.size(); k++) {
        unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xC0) != 0x80) break;
        cp = (cp << 6) | (cc & 0x3F);
        i++;
    }
    return extra ? cp : 0xFFFD;
}

bool init(const std::string& content_dir) {
    if (TTF_Init() != 0) {
        log_message(LOG_ERROR, "Text", "TTF_Init failed: %s", TTF_GetError());
        return false;
    }
    for (int w = 0; w < WEIGHT_COUNT; w++) {
        std::string path = content_dir + "/" + FILES[w];
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            log_message(LOG_WARNING, "Text", "Missing font %s", path.c_str());
            continue;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        g_font_data[w].resize(n > 0 ? n : 0);
        if (n > 0 && fread(g_font_data[w].data(), 1, n, f) != (size_t)n) g_font_data[w].clear();
        fclose(f);
    }
    for (int i = 0; i < platform::CJK_FONT_COUNT; i++) g_has_cjk[i] = platform::cjk_font((platform::CjkFont)i, g_cjk[i]);
    return !g_font_data[REGULAR].empty();
}

void shutdown() {
    for (auto& [k, f] : g_fonts)
        if (f) TTF_CloseFont(f);
    for (auto& [k, f] : g_cjk_fonts)
        if (f) TTF_CloseFont(f);
    g_fonts.clear();
    g_cjk_fonts.clear();
    g_glyphs.clear();
    g_metrics.clear();
    TTF_Quit();
}

float line_height(Font f) {
    int px = raster_size(f);
    return get_metrics(f.weight, px).height / raster_scale(f);
}

float ascent(Font f) {
    int px = raster_size(f);
    return get_metrics(f.weight, px).ascent / raster_scale(f);
}

float measure(Font f, std::string_view s) {
    int px = raster_size(f);
    float rs = raster_scale(f);
    float w = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = next_codepoint(s, i);
        if (cp == '\n') break;
        w += get_glyph(f.weight, px, cp).advance;
    }
    return w / rs;
}

float draw(Font f, float x, float y, std::string_view s, gfx::Color c, Align align) {
    if (s.empty() || c.a == 0) return 0;
    float w = measure(f, s);
    if (align == CENTER) x -= w * 0.5f;
    else if (align == RIGHT) x -= w;
    int px = raster_size(f);
    float rs = raster_scale(f);
    // Snap to the physical pixel grid for crisp glyphs.
    float os = gfx::output_scale() * gfx::current_scale();
    float pen = std::round(x * os) / os;
    y = std::round(y * os) / os;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = next_codepoint(s, i);
        if (cp == '\n') break;
        const Glyph& g = get_glyph(f.weight, px, cp);
        if (g.visible) {
            gfx::atlas_quad(gfx::Rect(pen + g.off_x / rs, y + g.off_y / rs, g.region.w / rs, g.region.h / rs),
                            g.region, c);
        }
        pen += g.advance / rs;
    }
    return w;
}

static std::string cut_with_ellipsis(Font f, std::string_view s, float max_w) {
    const std::string dots = "\xE2\x80\xA6";  // …
    float dots_w = measure(f, dots);
    int px = raster_size(f);
    float rs = raster_scale(f);
    float w = 0;
    size_t cut = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = next_codepoint(s, i);
        float adv = get_glyph(f.weight, px, cp).advance / rs;
        if (w + adv + dots_w > max_w) break;
        w += adv;
        cut = i;
    }
    std::string out(s.substr(0, cut));
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + dots;
}

std::string ellipsize(Font f, std::string_view s, float max_w) {
    if (measure(f, s) <= max_w) return std::string(s);
    return cut_with_ellipsis(f, s, max_w);
}

float draw_fit(Font f, float x, float y, float max_w, std::string_view s, gfx::Color c, Align align) {
    if (measure(f, s) <= max_w) return draw(f, x, y, s, c, align);
    std::string e = ellipsize(f, s, max_w);
    return draw(f, x, y, e, c, align);
}

namespace {

// Chinese, Japanese and Korean don't need spaces to break a line: it can break next to any of
// their characters, except before closing punctuation (、。」) or after opening punctuation (「).
bool is_cjk(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF) || cp >= 0x20000;
}

bool no_break_before(uint32_t cp) {
    static const std::u32string_view CLOSE =
        U",.!?:;)]}%\u2019\u201D\u2026\u3001\u3002\u3005\u3009\u300B\u300D\u300F\u3011\u3015\u3017\u3019\u301F"
        U"\u3041\u3043\u3045\u3047\u3049\u3063\u3083\u3085\u3087\u308E\u309D\u309E\u30A1\u30A3\u30A5\u30A7\u30A9"
        U"\u30C3\u30E3\u30E5\u30E7\u30EE\u30F5\u30F6\u30FB\u30FC\uFF01\uFF05\uFF09\uFF0C\uFF0E\uFF1A\uFF1B\uFF1F"
        U"\uFF3D\uFF5D";
    return CLOSE.find((char32_t)cp) != std::u32string_view::npos;
}

bool no_break_after(uint32_t cp) {
    static const std::u32string_view OPEN =
        U"([{\u2018\u201C\u3008\u300A\u300C\u300E\u3010\u3014\u3016\u3018\u301D\uFF08\uFF3B\uFF5B";
    return OPEN.find((char32_t)cp) != std::u32string_view::npos;
}

}  // namespace

std::vector<std::string> wrap(Font f, std::string_view s, float width, int max_lines) {
    // The pieces a line can break between: words, and single CJK characters.
    struct Piece {
        size_t a, b;
        bool space;    // a space comes before it
        bool newline;  // it is a line break
    };
    std::vector<Piece> pieces;
    bool space = false, glue = false, prev_cjk = false;
    for (size_t i = 0; i < s.size();) {
        size_t a = i;
        uint32_t cp = next_codepoint(s, i);
        if (cp == ' ') {
            space = true;
            continue;
        }
        if (cp == '\n') {
            pieces.push_back({a, i, false, true});
            space = false;
            continue;
        }
        bool cjk = is_cjk(cp);
        bool join = !pieces.empty() && !pieces.back().newline && !space &&
                    (glue || no_break_before(cp) || (!cjk && !prev_cjk));
        if (join) pieces.back().b = i;
        else pieces.push_back({a, i, space, false});
        space = false;
        glue = no_break_after(cp);
        prev_cjk = cjk;
    }

    std::vector<std::string> lines;
    std::string line;
    float line_w = 0;
    float space_w = measure(f, " ");
    auto push_line = [&]() {
        lines.push_back(line);
        line.clear();
        line_w = 0;
    };
    size_t k = 0;
    while (k < pieces.size()) {
        const Piece& p = pieces[k++];
        if (p.newline) {
            push_line();
        } else {
            std::string_view word = s.substr(p.a, p.b - p.a);
            float ww = measure(f, word);
            float gap = p.space && !line.empty() ? space_w : 0;
            if (!line.empty() && line_w + gap + ww > width) {
                push_line();
                gap = 0;
            }
            if (gap > 0) line += ' ';
            line.append(word);
            line_w += gap + ww;
        }
        if (max_lines > 0 && (int)lines.size() >= max_lines) break;
    }
    if (!line.empty()) push_line();
    bool truncated = k < pieces.size();
    if (max_lines > 0 && (int)lines.size() > max_lines) {
        lines.resize(max_lines);
        truncated = true;
    }
    if (truncated && !lines.empty()) {
        const std::string dots = "\xE2\x80\xA6";
        std::string& last = lines.back();
        if (measure(f, last + dots) <= width) last += dots;
        else last = cut_with_ellipsis(f, last, width);
    }
    return lines;
}

float measure_wrapped(Font f, float width, std::string_view s, int max_lines, float line_spacing) {
    auto lines = wrap(f, s, width, max_lines);
    return lines.size() * line_height(f) * line_spacing;
}

float draw_wrapped(Font f, const gfx::Rect& box, std::string_view s, gfx::Color c, int max_lines, Align align,
                   float line_spacing) {
    auto lines = wrap(f, s, box.w, max_lines);
    float lh = line_height(f) * line_spacing;
    float y = box.y;
    float x = align == CENTER ? box.cx() : align == RIGHT ? box.r() : box.x;
    for (auto& l : lines) {
        draw(f, x, y, l, c, align);
        y += lh;
    }
    return lines.size() * lh;
}

void icon(int codepoint, float size, float cx, float cy, gfx::Color c) {
    Font f{ICONS, size};
    int px = raster_size(f);
    float rs = raster_scale(f);
    const Glyph& g = get_glyph(ICONS, px, (uint32_t)codepoint);
    if (!g.visible) return;
    float w = g.region.w / rs, h = g.region.h / rs;
    gfx::atlas_quad(gfx::Rect(cx - w * 0.5f, cy - h * 0.5f, w, h), g.region, c);
}

float draw_shadowed(Font f, float x, float y, std::string_view s, gfx::Color c, Align align, gfx::Color shadow) {
    draw(f, x, y + 1.5f, s, shadow.alpha(c.a / 255.0f), align);
    return draw(f, x, y, s, c, align);
}

}  // namespace text
