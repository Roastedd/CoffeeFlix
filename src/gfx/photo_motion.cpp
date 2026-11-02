#include "gfx/photo_motion.hpp"

#include <algorithm>
#include <cmath>

namespace photo_motion {

namespace {

float unit(uint64_t seed, int k) {  // 0..1, the same for the same seed
    uint64_t h = seed * 0x9E3779B97F4A7C15ull + (uint64_t)k * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 31;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    return (float)(h & 0xFFFFFF) / (float)0x1000000;
}

// The zoom at which a box of the picture (fractions of its width and height) fills `fill` of the screen's
// width or height, whichever it fills first.
float zoom_for_box(float bw, float bh, float fill, float picture_aspect, float screen_aspect) {
    // At zoom 1 the picture is fitted: its width is screen width if it is the wider shape, else its height is the screen height.
    const float dw = picture_aspect >= screen_aspect ? 1.0f : picture_aspect / screen_aspect;   // picture width, as a share of the screen's
    const float dh = picture_aspect >= screen_aspect ? screen_aspect / picture_aspect : 1.0f;
    const float zw = bw > 0 ? fill / (bw * dw) : 1e9f, zh = bh > 0 ? fill / (bh * dh) : 1e9f;
    return std::min(zw, zh);
}

}  // namespace

View clamp(View v, float picture_aspect, float screen_aspect) {
    const float dw = picture_aspect >= screen_aspect ? 1.0f : picture_aspect / screen_aspect;
    const float dh = picture_aspect >= screen_aspect ? screen_aspect / picture_aspect : 1.0f;
    // The part of the picture across the screen, as a fraction of the picture: 1 / (zoom * share).
    const float vw = std::min(1.0f, 1.0f / (v.zoom * dw)), vh = std::min(1.0f, 1.0f / (v.zoom * dh));
    v.cx = vw >= 1 ? 0.5f : std::clamp(v.cx, vw * 0.5f, 1 - vw * 0.5f);
    v.cy = vh >= 1 ? 0.5f : std::clamp(v.cy, vh * 0.5f, 1 - vh * 0.5f);
    return v;
}

Plan plan(const focus::Focus& f, float pa, float sa, uint64_t seed, bool still) {
    Plan p;
    p.from = p.to = View();
    if (still) return p;
    const float side = unit(seed, 1) > 0.5f ? 1.0f : -1.0f;
    if (f.found && f.people == 1) {
        // Close in on the person: until they fill about half of the screen, no more than 1.7 times, no less than 1.15.
        const float z = std::clamp(zoom_for_box(f.main_w * 1.6f, f.main_h * 2.2f, 0.9f, pa, sa), 1.15f, 1.7f);
        p.from = {1.0f, 0.5f + (f.cx - 0.5f) * 0.25f, 0.5f + (f.cy - 0.5f) * 0.25f};
        p.to = {z, f.cx, f.cy + f.main_h * 0.15f};   // a little below the middle of the face: room above the head
        p.kind = "close";
    } else if (f.found && f.people >= 2 && f.people <= 3) {
        const float bw = f.x1 - f.x0, bh = f.y1 - f.y0;
        const float z = std::clamp(zoom_for_box(bw * 1.25f, bh * 1.6f, 0.95f, pa, sa), 1.05f, 1.4f);
        p.from = {1.0f, 0.5f, 0.5f};
        p.to = {z, (f.x0 + f.x1) * 0.5f, (f.y0 + f.y1) * 0.5f};
        p.kind = "group";
    } else if (f.found && f.people >= 4) {
        // A crowd: stay back, and either draw back from a little in (so the whole group comes into view) or move along them.
        const float bw = f.x1 - f.x0;
        const float gx = (f.x0 + f.x1) * 0.5f, gy = (f.y0 + f.y1) * 0.5f;
        if (bw > 0.6f || unit(seed, 2) > 0.5f) {
            p.from = {1.12f, f.x0 + 0.2f * bw, gy};
            p.to = {1.12f, f.x1 - 0.2f * bw, gy};
            if (side < 0) std::swap(p.from.cx, p.to.cx);
            p.kind = "crowd";
        } else {
            p.from = {1.12f, gx, gy};
            p.to = {1.0f, 0.5f, 0.5f};
            p.kind = "crowd";
        }
    } else if (f.found) {
        // Nothing alive in it: toward what stands out, a modest 8-14%.
        const float z = 1.08f + 0.06f * unit(seed, 3);
        if (unit(seed, 4) > 0.35f) {
            p.from = {1.0f, 0.5f, 0.5f};
            p.to = {z, 0.5f + (f.cx - 0.5f) * 0.7f, 0.5f + (f.cy - 0.5f) * 0.7f};
        } else {
            p.from = {z, 0.5f + (f.cx - 0.5f) * 0.7f, 0.5f + (f.cy - 0.5f) * 0.7f};
            p.to = {1.0f, 0.5f, 0.5f};
        }
        p.kind = "drift";
    } else {
        const float z = 1.06f + 0.05f * unit(seed, 3);
        const View a{1.0f, 0.5f, 0.5f}, b{z, 0.5f + side * 0.03f, 0.5f + (unit(seed, 5) - 0.5f) * 0.05f};
        if (unit(seed, 4) > 0.4f) { p.from = a; p.to = b; } else { p.from = b; p.to = a; }
        p.kind = "drift";
    }
    p.from = clamp(p.from, pa, sa);
    p.to = clamp(p.to, pa, sa);
    return p;
}

Plan soften(Plan p, float k) {
    for (View* v : {&p.from, &p.to}) {
        v->zoom = 1 + (v->zoom - 1) * k;
        v->cx = 0.5f + (v->cx - 0.5f) * k;
        v->cy = 0.5f + (v->cy - 0.5f) * k;
    }
    return p;
}

View at(const Plan& plan, float t) {
    t = std::clamp(t, 0.0f, 1.25f);
    float e;
    if (t <= 1.0f) {
        const float s = t * t * (3 - 2 * t);
        e = 0.5f * s + 0.5f * t;                    // eased in, but at pace by the end
    } else {
        e = 1.0f + (t - 1.0f) * 0.5f;               // keeps going at the pace it had (slope 0.5 at t=1)
    }
    View v;
    v.zoom = plan.from.zoom + (plan.to.zoom - plan.from.zoom) * e;
    v.cx = plan.from.cx + (plan.to.cx - plan.from.cx) * e;
    v.cy = plan.from.cy + (plan.to.cy - plan.from.cy) * e;
    return v;
}

Transform transform(const View& v, float drawn_w, float drawn_h) {
    // The point (cx, cy) of the picture sits at (cx - 0.5) * drawn from the picture's middle; scaled by zoom about the screen's
    // middle and shifted back so that point is in the middle.
    return {v.zoom, -(v.cx - 0.5f) * drawn_w * v.zoom, -(v.cy - 0.5f) * drawn_h * v.zoom};
}

}  // namespace photo_motion
