// Batched 2D renderer on top of SDL_Renderer.
//
// Everything that uses the shared atlas (solid shapes, shadows, glows and text)
// is accumulated into one vertex buffer and submitted with a single
// SDL_RenderGeometry call; a draw only breaks the batch when it switches to a
// different texture (e.g. a poster image) or changes the clip rect.
#pragma once

#include <SDL2/SDL.h>
#include <cstdint>
#include <algorithm>

namespace gfx {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;

    constexpr Color() = default;
    constexpr Color(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255) : r(r_), g(g_), b(b_), a(a_) {}

    constexpr Color alpha(float f) const {
        float v = a * f;
        return Color(r, g, b, (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v));
    }
    constexpr Color with_a(uint8_t na) const { return Color(r, g, b, na); }
};

constexpr Color rgb(uint32_t hex) { return Color((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, 255); }
constexpr Color rgba(uint32_t hex) { return Color(hex >> 24, (hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF); }
Color lerp(Color a, Color b, float t);

constexpr Color WHITE(255, 255, 255, 255);
constexpr Color BLACK(0, 0, 0, 255);
constexpr Color CLEAR(0, 0, 0, 0);

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    constexpr Rect() = default;
    constexpr Rect(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}

    constexpr float r() const { return x + w; }
    constexpr float b() const { return y + h; }
    constexpr float cx() const { return x + w * 0.5f; }
    constexpr float cy() const { return y + h * 0.5f; }
    constexpr Rect inset(float d) const { return Rect(x + d, y + d, w - 2 * d, h - 2 * d); }
    constexpr Rect inset(float dx, float dy) const { return Rect(x + dx, y + dy, w - 2 * dx, h - 2 * dy); }
    constexpr Rect offset(float dx, float dy) const { return Rect(x + dx, y + dy, w, h); }
    constexpr bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    constexpr bool intersects(const Rect& o) const { return x < o.r() && o.x < r() && y < o.b() && o.y < b(); }
    // Scale around the rect's center.
    constexpr Rect scaled(float s) const { return Rect(cx() - w * s * 0.5f, cy() - h * s * 0.5f, w * s, h * s); }
};

void init(SDL_Renderer* renderer);
void shutdown();
SDL_Renderer* renderer();

// Ratio of physical output pixels to logical pixels (1.0 at 720p, 1.5 at 1080p).
float output_scale();

void begin_frame(Color clear);
void end_frame();
void flush();

// --- state stacks -----------------------------------------------------------
void push_clip(const Rect& r);   // intersected with the current clip
void pop_clip();
Rect current_clip();             // screen space; whole screen when unclipped
void push_alpha(float a);        // multiplied into every color
void pop_alpha();
// Uniform scale around (cx, cy) followed by translation, composed with the
// current transform. Used for focus zoom and page transitions.
void push_transform(float scale, float cx, float cy, float tx = 0, float ty = 0);
void pop_transform();
float current_scale();
Rect to_screen(const Rect& r);   // apply the current transform

// --- primitives ---------------------------------------------------------------
void fill_rect(const Rect& r, Color c);
// Four corner colors: top-left, top-right, bottom-left, bottom-right.
void fill_rect_grad(const Rect& r, Color tl, Color tr, Color bl, Color br);
inline void fill_rect_vgrad(const Rect& r, Color top, Color bottom) { fill_rect_grad(r, top, top, bottom, bottom); }
inline void fill_rect_hgrad(const Rect& r, Color left, Color right) { fill_rect_grad(r, left, right, left, right); }

void fill_rrect(const Rect& r, float radius, Color c);
void fill_rrect_vgrad(const Rect& r, float radius, Color top, Color bottom);
void fill_rrect_hgrad(const Rect& r, float radius, Color left, Color right);
void stroke_rrect(const Rect& r, float radius, float width, Color c);

void fill_circle(float cx, float cy, float radius, Color c);
void stroke_circle(float cx, float cy, float radius, float width, Color c);
// Angles in radians, 0 = 3 o'clock, clockwise (screen space).
void stroke_arc(float cx, float cy, float radius, float a0, float a1, float width, Color c);
void fill_triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c);

// Soft drop shadow for a (rounded) rect. `blur` is the falloff distance.
void shadow(const Rect& r, float blur, Color c);
// Radial glow centered in r (fades to transparent at the edges of r).
void glow(const Rect& r, Color c);

// Textured quad. `uv` is in normalized texture coordinates.
void image(SDL_Texture* tex, const Rect& dst, Color tint = WHITE, float radius = 0,
           const Rect& uv = Rect(0, 0, 1, 1));
// Scale-to-fill with center crop ("object-fit: cover").
void image_cover(SDL_Texture* tex, int tex_w, int tex_h, const Rect& dst, float radius = 0,
                 Color tint = WHITE, float focus_y = 0.5f);
// Scale-to-fit, letterboxed ("object-fit: contain"). Returns the drawn rect.
Rect image_contain(SDL_Texture* tex, int tex_w, int tex_h, const Rect& dst, Color tint = WHITE, float radius = 0);

// --- atlas access for the text renderer ---------------------------------------
struct AtlasRegion { int x, y, w, h; };
// Allocates space in the shared atlas; returns false if full.
bool atlas_alloc(int w, int h, AtlasRegion* out);
// Copies RGBA8 pixels (tightly packed, row-major, straight alpha) into the atlas.
void atlas_upload(const AtlasRegion& r, const uint8_t* rgba, int pitch);
// Frees all dynamic atlas space (glyphs). Caller must drop cached regions.
void atlas_reset_dynamic();
int atlas_generation();
void atlas_quad(const Rect& dst, const AtlasRegion& src, Color c);

}  // namespace gfx
