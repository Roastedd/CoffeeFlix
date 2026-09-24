#include "gfx/gfx.hpp"

#include <cmath>
#include <vector>
#include <cstring>

#include "logger/logger.hpp"

namespace gfx {

namespace {

constexpr int ATLAS_SIZE = 1024;
constexpr float PI = 3.14159265358979f;

// Static atlas regions (top rows); glyphs are packed below them.
constexpr AtlasRegion WHITE_REGION{0, 0, 4, 4};
constexpr AtlasRegion SHADOW_REGION{8, 0, 64, 64};
constexpr AtlasRegion GLOW_REGION{80, 0, 64, 64};
constexpr int DYNAMIC_TOP = 72;

struct Xform {
    float s = 1, tx = 0, ty = 0;
    float px(float x) const { return x * s + tx; }
    float py(float y) const { return y * s + ty; }
};

struct State {
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* atlas = nullptr;
    std::vector<uint8_t> atlas_px;  // RGBA8, ATLAS_SIZE * ATLAS_SIZE * 4
    SDL_Rect dirty{0, 0, 0, 0};
    bool has_dirty = false;
    int generation = 0;

    // shelf packer for the dynamic part of the atlas
    int shelf_x = 0, shelf_y = DYNAMIC_TOP, shelf_h = 0;

    std::vector<SDL_Vertex> verts;
    std::vector<int> idx;
    SDL_Texture* batch_tex = nullptr;

    std::vector<SDL_Rect> clips;
    std::vector<float> alphas;
    std::vector<Xform> xforms;
    float alpha = 1.0f;
    Xform xf;
    float out_scale = 1.0f;

    // scratch buffers for path generation
    std::vector<float> px, py, nx, ny;
} S;

inline SDL_FPoint white_uv() {
    return SDL_FPoint{(WHITE_REGION.x + 2.0f) / ATLAS_SIZE, (WHITE_REGION.y + 2.0f) / ATLAS_SIZE};
}

inline SDL_Color to_sdl(Color c) {
    if (S.alpha < 1.0f) c = c.alpha(S.alpha);
    return SDL_Color{c.r, c.g, c.b, c.a};
}

void use_texture(SDL_Texture* tex) {
    if (tex != S.batch_tex) {
        flush();
        S.batch_tex = tex;
    }
}

inline int add_vertex(float x, float y, SDL_Color c, float u, float v) {
    S.verts.push_back(SDL_Vertex{SDL_FPoint{x, y}, c, SDL_FPoint{u, v}});
    return (int)S.verts.size() - 1;
}

inline void add_tri(int a, int b, int c) {
    S.idx.push_back(a);
    S.idx.push_back(b);
    S.idx.push_back(c);
}

inline void add_quad(int a, int b, int c, int d) {  // a-b-c-d in order around the quad
    add_tri(a, b, c);
    add_tri(a, c, d);
}

void mark_dirty(int x, int y, int w, int h) {
    if (!S.has_dirty) {
        S.dirty = SDL_Rect{x, y, w, h};
        S.has_dirty = true;
        return;
    }
    int x0 = std::min(S.dirty.x, x), y0 = std::min(S.dirty.y, y);
    int x1 = std::max(S.dirty.x + S.dirty.w, x + w), y1 = std::max(S.dirty.y + S.dirty.h, y + h);
    S.dirty = SDL_Rect{x0, y0, x1 - x0, y1 - y0};
}

void upload_dirty() {
    if (!S.has_dirty || !S.atlas) return;
    const uint8_t* src = S.atlas_px.data() + ((size_t)S.dirty.y * ATLAS_SIZE + S.dirty.x) * 4;
    SDL_UpdateTexture(S.atlas, &S.dirty, src, ATLAS_SIZE * 4);
    S.has_dirty = false;
}

void set_px(int x, int y, uint8_t a) {
    uint8_t* p = &S.atlas_px[((size_t)y * ATLAS_SIZE + x) * 4];
    p[0] = 255; p[1] = 255; p[2] = 255; p[3] = a;
}

void build_static_regions() {
    for (int y = 0; y < WHITE_REGION.h; y++)
        for (int x = 0; x < WHITE_REGION.w; x++) set_px(WHITE_REGION.x + x, WHITE_REGION.y + y, 255);

    // Shadow: a square spanning texels 16..48 blurred with a gaussian, so its
    // half-intensity edge sits 16 texels in from the border.
    auto edge = [](float t) {
        const float sigma = 5.5f;
        float a = 0.5f * (1.0f + std::erf((t - 16.0f) / (sigma * 1.41421356f)));
        float b = 0.5f * (1.0f + std::erf((48.0f - t) / (sigma * 1.41421356f)));
        return std::min(a, b);
    };
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float v = edge(x + 0.5f) * edge(y + 0.5f);
            set_px(SHADOW_REGION.x + x, SHADOW_REGION.y + y, (uint8_t)std::lround(v * 255));
        }

    // Glow: smooth radial falloff.
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float dx = (x + 0.5f - 32) / 32.0f, dy = (y + 0.5f - 32) / 32.0f;
            float d = std::min(1.0f, std::sqrt(dx * dx + dy * dy));
            float v = (1 - d) * (1 - d);
            v = v * v * (3 - 2 * v);
            set_px(GLOW_REGION.x + x, GLOW_REGION.y + y, (uint8_t)std::lround(v * 255));
        }
    mark_dirty(0, 0, ATLAS_SIZE, DYNAMIC_TOP);
}

// Rounded-rect outline in screen space: for every point, the corner center and
// the unit normal. Points at radius R are center + normal * R.
void rrect_centers(const Rect& r, float rad, int segs_hint = 0) {
    S.px.clear(); S.py.clear(); S.nx.clear(); S.ny.clear();
    int segs = segs_hint ? segs_hint : std::clamp((int)(rad * S.out_scale * 0.35f) + 2, 2, 14);
    const float cxs[4] = {r.x + rad, r.r() - rad, r.r() - rad, r.x + rad};
    const float cys[4] = {r.y + rad, r.y + rad, r.b() - rad, r.b() - rad};
    const float start[4] = {PI, 1.5f * PI, 0.0f, 0.5f * PI};
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i <= segs; i++) {
            float a = start[c] + (0.5f * PI) * i / segs;
            S.px.push_back(cxs[c]);
            S.py.push_back(cys[c]);
            S.nx.push_back(std::cos(a));
            S.ny.push_back(std::sin(a));
        }
    }
}

using ColorFn = SDL_Color (*)(float x, float y, const void* ctx);

// Filled convex shape from the current centers/normals at radius `rad`, with a
// feathered edge. Colors come from `fn` so gradients and textures share code.
void fill_path(float rad, float cx, float cy, SDL_Color (*color_at)(float, float, const void*), const void* ctx,
               SDL_FPoint (*uv_at)(float, float, const void*), const void* uvctx) {
    const float aa = 0.5f / S.out_scale;
    const size_t n = S.px.size();
    SDL_FPoint wuv = white_uv();

    auto uv = [&](float x, float y) { return uv_at ? uv_at(x, y, uvctx) : wuv; };

    SDL_FPoint cuv = uv(cx, cy);
    int center = add_vertex(cx, cy, color_at(cx, cy, ctx), cuv.x, cuv.y);
    int first_in = (int)S.verts.size();
    for (size_t i = 0; i < n; i++) {
        float ix = S.px[i] + S.nx[i] * (rad - aa), iy = S.py[i] + S.ny[i] * (rad - aa);
        float ox = S.px[i] + S.nx[i] * (rad + aa), oy = S.py[i] + S.ny[i] * (rad + aa);
        SDL_Color ci = color_at(ix, iy, ctx);
        SDL_Color co = ci;
        co.a = 0;
        SDL_FPoint iu = uv(ix, iy), ou = uv(ox, oy);
        add_vertex(ix, iy, ci, iu.x, iu.y);
        add_vertex(ox, oy, co, ou.x, ou.y);
    }
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;
        int in_i = first_in + (int)i * 2, out_i = in_i + 1;
        int in_j = first_in + (int)j * 2, out_j = in_j + 1;
        add_tri(center, in_i, in_j);
        add_quad(in_i, out_i, out_j, in_j);
    }
}

SDL_Color solid_color(float, float, const void* ctx) { return *(const SDL_Color*)ctx; }

struct GradCtx { SDL_Color a, b; float start, len; bool vertical; };
SDL_Color grad_color(float x, float y, const void* ctx) {
    const GradCtx* g = (const GradCtx*)ctx;
    float t = ((g->vertical ? y : x) - g->start) / g->len;
    t = std::clamp(t, 0.0f, 1.0f);
    return SDL_Color{(uint8_t)(g->a.r + (g->b.r - g->a.r) * t), (uint8_t)(g->a.g + (g->b.g - g->a.g) * t),
                     (uint8_t)(g->a.b + (g->b.b - g->a.b) * t), (uint8_t)(g->a.a + (g->b.a - g->a.a) * t)};
}

struct UvCtx { Rect dst; Rect uv; };
SDL_FPoint rect_uv(float x, float y, const void* ctx) {
    const UvCtx* u = (const UvCtx*)ctx;
    // SDL_RenderGeometry rejects the whole batch if any uv is outside [0, 1],
    // and the anti-aliasing fringe sits slightly outside the image.
    float fu = u->uv.x + (x - u->dst.x) / u->dst.w * u->uv.w;
    float fv = u->uv.y + (y - u->dst.y) / u->dst.h * u->uv.h;
    return SDL_FPoint{std::clamp(fu, 0.0f, 1.0f), std::clamp(fv, 0.0f, 1.0f)};
}

// Ring between radii r0 < r1 along the current path, with feathered edges.
void stroke_path(float r_center, float width, SDL_Color c, bool closed) {
    const float aa = 0.5f / S.out_scale;
    const size_t n = S.px.size();
    const float half = width * 0.5f;
    const float radii[4] = {r_center - half - aa, r_center - half + aa, r_center + half - aa, r_center + half + aa};
    SDL_Color transparent = c;
    transparent.a = 0;
    const SDL_Color cols[4] = {transparent, c, c, transparent};
    SDL_FPoint wuv = white_uv();

    int base = (int)S.verts.size();
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 4; k++) {
            float rr = std::max(0.0f, radii[k]);
            add_vertex(S.px[i] + S.nx[i] * rr, S.py[i] + S.ny[i] * rr, cols[k], wuv.x, wuv.y);
        }
    size_t segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; i++) {
        size_t j = (i + 1) % n;
        for (int k = 0; k < 3; k++) {
            int a = base + (int)i * 4 + k, b = base + (int)j * 4 + k;
            add_quad(a, b, b + 1, a + 1);
        }
    }
}

}  // namespace

Color lerp(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return Color((uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                 (uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.a + (b.a - a.a) * t));
}

void init(SDL_Renderer* renderer) {
    S.renderer = renderer;
    S.atlas_px.assign((size_t)ATLAS_SIZE * ATLAS_SIZE * 4, 0);
    for (size_t i = 0; i < S.atlas_px.size(); i += 4) {
        S.atlas_px[i] = S.atlas_px[i + 1] = S.atlas_px[i + 2] = 255;  // white, transparent
    }
    S.atlas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, ATLAS_SIZE, ATLAS_SIZE);
    if (!S.atlas) {
        log_message(LOG_ERROR, "gfx", "Failed to create atlas: %s", SDL_GetError());
        return;
    }
    SDL_SetTextureBlendMode(S.atlas, SDL_BLENDMODE_BLEND);
    build_static_regions();
    mark_dirty(0, 0, ATLAS_SIZE, ATLAS_SIZE);
    upload_dirty();

    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    S.out_scale = h > 0 ? std::max(1.0f, h / 720.0f) : 1.0f;
    S.verts.reserve(16384);
    S.idx.reserve(32768);
}

void shutdown() {
    if (S.atlas) SDL_DestroyTexture(S.atlas);
    S.atlas = nullptr;
}

SDL_Renderer* renderer() { return S.renderer; }
float output_scale() { return S.out_scale; }

void begin_frame(Color clear) {
    S.clips.clear();
    S.alphas.clear();
    S.xforms.clear();
    S.alpha = 1.0f;
    S.xf = Xform();
    SDL_RenderSetClipRect(S.renderer, nullptr);
    SDL_SetRenderDrawColor(S.renderer, clear.r, clear.g, clear.b, clear.a);
    SDL_RenderClear(S.renderer);
    S.batch_tex = S.atlas;
}

void end_frame() { flush(); }

void flush() {
    if (S.idx.empty()) {
        S.verts.clear();
        return;
    }
    if (S.batch_tex == S.atlas) upload_dirty();
    if (SDL_RenderGeometry(S.renderer, S.batch_tex, S.verts.data(), (int)S.verts.size(), S.idx.data(), (int)S.idx.size()) != 0) {
        static int reported = 0;
        if (reported++ < 5) log_message(LOG_ERROR, "gfx", "RenderGeometry failed: %s", SDL_GetError());
    }
    S.verts.clear();
    S.idx.clear();
}

// --- state -------------------------------------------------------------------

void push_clip(const Rect& r) {
    flush();
    Rect sr = to_screen(r);
    SDL_Rect c{(int)std::floor(sr.x), (int)std::floor(sr.y), (int)std::ceil(sr.w), (int)std::ceil(sr.h)};
    if (!S.clips.empty()) {
        SDL_Rect out;
        if (!SDL_IntersectRect(&S.clips.back(), &c, &out)) out = SDL_Rect{0, 0, 0, 0};
        c = out;
    }
    S.clips.push_back(c);
    SDL_RenderSetClipRect(S.renderer, &c);
}

void pop_clip() {
    flush();
    if (!S.clips.empty()) S.clips.pop_back();
    SDL_RenderSetClipRect(S.renderer, S.clips.empty() ? nullptr : &S.clips.back());
}

Rect current_clip() {
    if (S.clips.empty()) return Rect(-10000, -10000, 20000, 20000);
    const SDL_Rect& c = S.clips.back();
    return Rect((float)c.x, (float)c.y, (float)c.w, (float)c.h);
}

void push_alpha(float a) {
    S.alphas.push_back(S.alpha);
    S.alpha *= std::clamp(a, 0.0f, 1.0f);
}

void pop_alpha() {
    if (S.alphas.empty()) return;
    S.alpha = S.alphas.back();
    S.alphas.pop_back();
}

void push_transform(float scale, float cx, float cy, float tx, float ty) {
    S.xforms.push_back(S.xf);
    Xform n;
    n.s = S.xf.s * scale;
    n.tx = S.xf.s * (cx - cx * scale + tx) + S.xf.tx;
    n.ty = S.xf.s * (cy - cy * scale + ty) + S.xf.ty;
    S.xf = n;
}

void pop_transform() {
    if (S.xforms.empty()) return;
    S.xf = S.xforms.back();
    S.xforms.pop_back();
}

float current_scale() { return S.xf.s; }

Rect to_screen(const Rect& r) { return Rect(S.xf.px(r.x), S.xf.py(r.y), r.w * S.xf.s, r.h * S.xf.s); }

// --- primitives ------------------------------------------------------------------

void fill_rect_grad(const Rect& lr, Color tl, Color tr, Color bl, Color br) {
    if (lr.w <= 0 || lr.h <= 0) return;
    use_texture(S.atlas);
    Rect r = to_screen(lr);
    SDL_FPoint uv = white_uv();
    int a = add_vertex(r.x, r.y, to_sdl(tl), uv.x, uv.y);
    int b = add_vertex(r.r(), r.y, to_sdl(tr), uv.x, uv.y);
    int c = add_vertex(r.r(), r.b(), to_sdl(br), uv.x, uv.y);
    int d = add_vertex(r.x, r.b(), to_sdl(bl), uv.x, uv.y);
    add_quad(a, b, c, d);
}

void fill_rect(const Rect& r, Color c) { fill_rect_grad(r, c, c, c, c); }

void fill_rrect(const Rect& lr, float radius, Color c) {
    if (lr.w <= 0 || lr.h <= 0 || c.a == 0) return;
    if (radius <= 0.5f) return fill_rect(lr, c);
    use_texture(S.atlas);
    Rect r = to_screen(lr);
    float rad = std::min({radius * S.xf.s, r.w * 0.5f, r.h * 0.5f});
    rrect_centers(r, rad);
    SDL_Color sc = to_sdl(c);
    fill_path(rad, r.cx(), r.cy(), solid_color, &sc, nullptr, nullptr);
}

static void fill_rrect_grad(const Rect& lr, float radius, Color a, Color b, bool vertical) {
    if (lr.w <= 0 || lr.h <= 0) return;
    use_texture(S.atlas);
    Rect r = to_screen(lr);
    float rad = std::max(0.01f, std::min({radius * S.xf.s, r.w * 0.5f, r.h * 0.5f}));
    rrect_centers(r, rad);
    GradCtx g{to_sdl(a), to_sdl(b), vertical ? r.y : r.x, vertical ? r.h : r.w, vertical};
    fill_path(rad, r.cx(), r.cy(), grad_color, &g, nullptr, nullptr);
}

void fill_rrect_vgrad(const Rect& r, float radius, Color top, Color bottom) { fill_rrect_grad(r, radius, top, bottom, true); }
void fill_rrect_hgrad(const Rect& r, float radius, Color left, Color right) { fill_rrect_grad(r, radius, left, right, false); }

void stroke_rrect(const Rect& lr, float radius, float width, Color c) {
    if (lr.w <= 0 || lr.h <= 0 || c.a == 0) return;
    use_texture(S.atlas);
    Rect r = to_screen(lr);
    float w = width * S.xf.s;
    // keep the stroke inside the rect
    Rect path = r.inset(w * 0.5f);
    float rad = std::max(0.01f, std::min({radius * S.xf.s - w * 0.5f, path.w * 0.5f, path.h * 0.5f}));
    rrect_centers(path, rad, 0);
    stroke_path(rad, w, to_sdl(c), true);
}

void fill_circle(float cx, float cy, float radius, Color c) {
    fill_rrect(Rect(cx - radius, cy - radius, radius * 2, radius * 2), radius, c);
}

void stroke_circle(float cx, float cy, float radius, float width, Color c) {
    stroke_rrect(Rect(cx - radius, cy - radius, radius * 2, radius * 2), radius, width, c);
}

void stroke_arc(float lcx, float lcy, float radius, float a0, float a1, float width, Color c) {
    if (a1 <= a0 || c.a == 0) return;
    use_texture(S.atlas);
    float cx = S.xf.px(lcx), cy = S.xf.py(lcy), rad = radius * S.xf.s, w = width * S.xf.s;
    int segs = std::clamp((int)((a1 - a0) * rad * S.out_scale / 6.0f) + 2, 3, 96);
    S.px.clear(); S.py.clear(); S.nx.clear(); S.ny.clear();
    for (int i = 0; i <= segs; i++) {
        float a = a0 + (a1 - a0) * i / segs;
        S.px.push_back(cx);
        S.py.push_back(cy);
        S.nx.push_back(std::cos(a));
        S.ny.push_back(std::sin(a));
    }
    SDL_Color sc = to_sdl(c);
    stroke_path(rad, w, sc, false);
    // round caps
    float cap = width * 0.5f;
    fill_circle(lcx + std::cos(a0) * radius, lcy + std::sin(a0) * radius, cap, c);
    fill_circle(lcx + std::cos(a1) * radius, lcy + std::sin(a1) * radius, cap, c);
}

void fill_triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
    use_texture(S.atlas);
    SDL_FPoint uv = white_uv();
    SDL_Color sc = to_sdl(c);
    int a = add_vertex(S.xf.px(x0), S.xf.py(y0), sc, uv.x, uv.y);
    int b = add_vertex(S.xf.px(x1), S.xf.py(y1), sc, uv.x, uv.y);
    int d = add_vertex(S.xf.px(x2), S.xf.py(y2), sc, uv.x, uv.y);
    add_tri(a, b, d);
}

// 9-slice of a 64x64 atlas region whose four 32x32 quadrants are the corners.
static void nine_slice_region(const Rect& outer, float corner, const AtlasRegion& reg, SDL_Color c) {
    use_texture(S.atlas);
    corner = std::min({corner, outer.w * 0.5f, outer.h * 0.5f});
    const float xs[4] = {outer.x, outer.x + corner, outer.r() - corner, outer.r()};
    const float ys[4] = {outer.y, outer.y + corner, outer.b() - corner, outer.b()};
    const float us[4] = {(float)reg.x, reg.x + 31.5f, reg.x + 32.5f, (float)(reg.x + reg.w)};
    const float vs[4] = {(float)reg.y, reg.y + 31.5f, reg.y + 32.5f, (float)(reg.y + reg.h)};
    int base = (int)S.verts.size();
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) add_vertex(xs[i], ys[j], c, us[i] / ATLAS_SIZE, vs[j] / ATLAS_SIZE);
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            int a = base + j * 4 + i;
            add_quad(a, a + 1, a + 5, a + 4);
        }
}

void shadow(const Rect& lr, float blur, Color c) {
    if (c.a == 0) return;
    Rect r = to_screen(lr);
    float b = std::max(1.0f, blur * S.xf.s);
    Rect outer(r.x - b, r.y - b, r.w + 2 * b, r.h + 2 * b);
    nine_slice_region(outer, 2 * b, SHADOW_REGION, to_sdl(c));
}

void glow(const Rect& lr, Color c) {
    if (c.a == 0) return;
    use_texture(S.atlas);
    Rect r = to_screen(lr);
    SDL_Color sc = to_sdl(c);
    float u0 = (float)GLOW_REGION.x / ATLAS_SIZE, v0 = (float)GLOW_REGION.y / ATLAS_SIZE;
    float u1 = (float)(GLOW_REGION.x + GLOW_REGION.w) / ATLAS_SIZE, v1 = (float)(GLOW_REGION.y + GLOW_REGION.h) / ATLAS_SIZE;
    int a = add_vertex(r.x, r.y, sc, u0, v0);
    int b = add_vertex(r.r(), r.y, sc, u1, v0);
    int d = add_vertex(r.r(), r.b(), sc, u1, v1);
    int e = add_vertex(r.x, r.b(), sc, u0, v1);
    add_quad(a, b, d, e);
}

void image(SDL_Texture* tex, const Rect& ldst, Color tint, float radius, const Rect& uv) {
    if (!tex || ldst.w <= 0 || ldst.h <= 0) return;
    use_texture(tex);
    Rect r = to_screen(ldst);
    SDL_Color sc = to_sdl(tint);
    if (radius <= 0.5f) {
        int a = add_vertex(r.x, r.y, sc, uv.x, uv.y);
        int b = add_vertex(r.r(), r.y, sc, uv.x + uv.w, uv.y);
        int c = add_vertex(r.r(), r.b(), sc, uv.x + uv.w, uv.y + uv.h);
        int d = add_vertex(r.x, r.b(), sc, uv.x, uv.y + uv.h);
        add_quad(a, b, c, d);
        return;
    }
    float rad = std::min({radius * S.xf.s, r.w * 0.5f, r.h * 0.5f});
    rrect_centers(r, rad);
    UvCtx u{r, uv};
    fill_path(rad, r.cx(), r.cy(), solid_color, &sc, rect_uv, &u);
}

void image_cover(SDL_Texture* tex, int tw, int th, const Rect& dst, float radius, Color tint, float focus_y) {
    if (!tex || tw <= 0 || th <= 0 || dst.w <= 0 || dst.h <= 0) return;
    float tex_aspect = (float)tw / th, dst_aspect = dst.w / dst.h;
    Rect uv(0, 0, 1, 1);
    if (tex_aspect > dst_aspect) {
        uv.w = dst_aspect / tex_aspect;
        uv.x = (1 - uv.w) * 0.5f;
    } else {
        uv.h = tex_aspect / dst_aspect;
        uv.y = (1 - uv.h) * std::clamp(focus_y, 0.0f, 1.0f);
    }
    image(tex, dst, tint, radius, uv);
}

Rect image_contain(SDL_Texture* tex, int tw, int th, const Rect& dst, Color tint, float radius) {
    if (!tex || tw <= 0 || th <= 0) return dst;
    float s = std::min(dst.w / tw, dst.h / th);
    Rect r(dst.cx() - tw * s * 0.5f, dst.cy() - th * s * 0.5f, tw * s, th * s);
    image(tex, r, tint, radius);
    return r;
}

// --- atlas --------------------------------------------------------------------------

bool atlas_alloc(int w, int h, AtlasRegion* out) {
    const int pad = 1;
    if (w + pad > ATLAS_SIZE || h + pad > ATLAS_SIZE - DYNAMIC_TOP) return false;
    if (S.shelf_x + w + pad > ATLAS_SIZE) {
        S.shelf_x = 0;
        S.shelf_y += S.shelf_h + pad;
        S.shelf_h = 0;
    }
    if (S.shelf_y + h + pad > ATLAS_SIZE) return false;
    *out = AtlasRegion{S.shelf_x, S.shelf_y, w, h};
    S.shelf_x += w + pad;
    S.shelf_h = std::max(S.shelf_h, h);
    return true;
}

void atlas_upload(const AtlasRegion& r, const uint8_t* rgba, int pitch) {
    for (int y = 0; y < r.h; y++)
        memcpy(&S.atlas_px[((size_t)(r.y + y) * ATLAS_SIZE + r.x) * 4], rgba + (size_t)y * pitch, (size_t)r.w * 4);
    mark_dirty(r.x, r.y, r.w, r.h);
}

void atlas_reset_dynamic() {
    flush();
    for (size_t y = DYNAMIC_TOP; y < ATLAS_SIZE; y++) {
        uint8_t* row = &S.atlas_px[y * ATLAS_SIZE * 4];
        for (int x = 0; x < ATLAS_SIZE; x++) row[x * 4 + 3] = 0;
    }
    mark_dirty(0, DYNAMIC_TOP, ATLAS_SIZE, ATLAS_SIZE - DYNAMIC_TOP);
    S.shelf_x = 0;
    S.shelf_y = DYNAMIC_TOP;
    S.shelf_h = 0;
    S.generation++;
}

int atlas_generation() { return S.generation; }

void atlas_quad(const Rect& ldst, const AtlasRegion& src, Color c) {
    use_texture(S.atlas);
    Rect r = to_screen(ldst);
    SDL_Color sc = to_sdl(c);
    float u0 = (float)src.x / ATLAS_SIZE, v0 = (float)src.y / ATLAS_SIZE;
    float u1 = (float)(src.x + src.w) / ATLAS_SIZE, v1 = (float)(src.y + src.h) / ATLAS_SIZE;
    int a = add_vertex(r.x, r.y, sc, u0, v0);
    int b = add_vertex(r.r(), r.y, sc, u1, v0);
    int d = add_vertex(r.r(), r.b(), sc, u1, v1);
    int e = add_vertex(r.x, r.b(), sc, u0, v1);
    add_quad(a, b, d, e);
}

}  // namespace gfx
