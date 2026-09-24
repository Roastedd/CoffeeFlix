#include "ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

#include "audio/mixer.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"

namespace ui {

namespace {

struct Reg {
    Id id;
    Rect r;       // screen space
    Rect clip;    // visible part (for touch)
    Id group;
    int layer;
    int flags;
};

struct AnimState {
    float v = 0;
    anim::Spring s;
    uint64_t last = 0;
    bool init = false;
};

struct ScrollState {
    float target = 0;
    anim::Spring s;
    bool init = false;
    float drag_velocity = 0;
    bool dragging = false;
};

struct Toast {
    std::string msg;
    int icon;
    Color color;
    double start;
};

Input* g_in = nullptr;
float g_dt = 1.0f / 60.0f;
double g_time = 0;
uint64_t g_frame = 0;

std::vector<Reg> g_cur, g_prev;
Id g_focus = 0;
int g_focus_grace = 0;  // frames to keep an explicitly set focus that isn't drawn yet
Id g_last_focus_group = 0;
int g_layer = 0;
std::vector<int> g_layer_stack;
bool g_nav_suspended = false;
std::unordered_map<Id, Id> g_group_memory;
std::unordered_map<Id, Id> g_group_entry;
std::unordered_map<Id, AnimState> g_anim;
std::unordered_map<Id, ScrollState> g_scroll;
std::unordered_map<Id, float> g_press;

// bump feedback when navigation hits an edge
Id g_bump_id = 0;
float g_bump_dx = 0, g_bump_dy = 0;
double g_bump_t = -10;

std::deque<Toast> g_toasts;

// backdrop crossfade
std::string g_backdrop, g_prev_backdrop;
double g_backdrop_t = -10;

int g_accent = 0;
Theme g_theme;

struct Accent { const char* name; uint32_t a, b; };
const Accent ACCENTS[] = {
    {"Caramel", 0xFFA24C, 0xFF5F6D},
    {"Berry", 0xFF4D8D, 0xA855F7},
    {"Mint", 0x2DD4BF, 0x22C55E},
    {"Ocean", 0x38BDF8, 0x6366F1},
    {"Gold", 0xFACC15, 0xF97316},
};

void build_theme() {
    g_theme.bg_top = gfx::rgb(0x120E17);
    g_theme.bg_bottom = gfx::rgb(0x07060A);
    g_theme.surface = Color(255, 255, 255, 16);
    g_theme.surface_hi = Color(255, 255, 255, 30);
    g_theme.surface_focus = Color(255, 255, 255, 235);
    g_theme.text = gfx::rgb(0xF6F4F8);
    g_theme.text2 = Color(236, 232, 245, 165);
    g_theme.text3 = Color(236, 232, 245, 100);
    g_theme.accent = gfx::rgb(ACCENTS[g_accent].a);
    g_theme.accent2 = gfx::rgb(ACCENTS[g_accent].b);
    g_theme.good = gfx::rgb(0x34D399);
    g_theme.warn = gfx::rgb(0xFBBF24);
    g_theme.bad = gfx::rgb(0xF87171);
}

const Reg* find(const std::vector<Reg>& v, Id id) {
    for (auto& r : v)
        if (r.id == id) return &r;
    return nullptr;
}

int top_layer(const std::vector<Reg>& v) {
    int top = 0;
    for (auto& r : v) top = std::max(top, r.layer);
    return top;
}

// Spatial navigation: best candidate in direction (dx, dy).
const Reg* pick(const std::vector<Reg>& v, const Reg& from, int dx, int dy, int layer) {
    const Reg* best = nullptr;
    float best_score = 1e30f;
    const Rect& f = from.r;
    for (auto& c : v) {
        if (c.id == from.id || c.layer != layer) continue;
        if ((c.flags & F_SIDE_ENTRY) && dy != 0 && c.group != from.group) continue;
        const Rect& r = c.r;
        float primary, ortho_gap, center_diff;
        if (dx > 0) {
            if (r.cx() <= f.cx() + 1 || r.x < f.x + 1) continue;
            primary = std::max(0.0f, r.x - f.r());
            ortho_gap = std::max(0.0f, std::max(f.y, r.y) - std::min(f.b(), r.b()));
            center_diff = std::fabs(r.cy() - f.cy());
        } else if (dx < 0) {
            if (r.cx() >= f.cx() - 1 || r.r() > f.r() - 1) continue;
            primary = std::max(0.0f, f.x - r.r());
            ortho_gap = std::max(0.0f, std::max(f.y, r.y) - std::min(f.b(), r.b()));
            center_diff = std::fabs(r.cy() - f.cy());
        } else if (dy > 0) {
            if (r.cy() <= f.cy() + 1 || r.y < f.y + 1) continue;
            primary = std::max(0.0f, r.y - f.b());
            ortho_gap = std::max(0.0f, std::max(f.x, r.x) - std::min(f.r(), r.r()));
            center_diff = std::fabs(r.cx() - f.cx());
        } else {
            if (r.cy() >= f.cy() - 1 || r.b() > f.b() - 1) continue;
            primary = std::max(0.0f, f.y - r.b());
            ortho_gap = std::max(0.0f, std::max(f.x, r.x) - std::min(f.r(), r.r()));
            center_diff = std::fabs(r.cx() - f.cx());
        }
        float score = primary + ortho_gap * 3.0f + center_diff * 0.15f;
        // Strongly prefer staying in the same group when overlapping.
        if (c.group == from.group && ortho_gap == 0) score *= 0.8f;
        if (score < best_score) {
            best_score = score;
            best = &c;
        }
    }
    return best;
}

void do_navigation() {
    if (!g_in) return;
    int top = top_layer(g_cur);
    const Reg* cur = find(g_cur, g_focus);

    if (g_focus_grace > 0) {
        g_focus_grace--;
        if (!cur) return;  // the target screen appears next frame
    }
    // Keep focus valid: fall back to a default item in the top layer.
    if (!cur || cur->layer != top) {
        const Reg* def = nullptr;
        // Prefer the remembered item of the last active group.
        if (Id mem = group_memory(g_last_focus_group)) {
            const Reg* m = find(g_cur, mem);
            if (m && m->layer == top) def = m;
        }
        for (auto& r : g_cur)
            if (!def && r.layer == top && (r.flags & F_DEFAULT)) def = &r;
        for (auto& r : g_cur)
            if (!def && r.layer == top && !(r.flags & F_SIDE_ENTRY)) def = &r;
        for (auto& r : g_cur) {
            if (def || r.layer != top) continue;
            auto e = g_group_entry.find(r.group);
            def = (e != g_group_entry.end()) ? find(g_cur, e->second) : &r;
        }
        if (def) {
            g_focus = def->id;
            g_last_focus_group = def->group;
        }
        return;
    }
    if (g_nav_suspended) return;

    int dx = 0, dy = 0;
    if (g_in->rep(BTN_LEFT)) dx = -1;
    else if (g_in->rep(BTN_RIGHT)) dx = 1;
    else if (g_in->rep(BTN_UP)) dy = -1;
    else if (g_in->rep(BTN_DOWN)) dy = 1;
    if (!dx && !dy) return;

    const Reg* next = pick(g_cur, *cur, dx, dy, top);
    if (next && next->group != cur->group && next->group && g_group_entry.count(next->group)) {
        const Reg* entry = find(g_cur, g_group_entry[next->group]);
        if (entry && entry->layer == top) next = entry;
    } else if (next && next->group != cur->group && next->group) {
        // Entering another group: return to where we were in it.
        auto it = g_group_memory.find(next->group);
        if (it != g_group_memory.end()) {
            const Reg* mem = find(g_cur, it->second);
            if (mem && mem->layer == top) {
                bool ok = (dx > 0 && mem->r.cx() > cur->r.cx()) || (dx < 0 && mem->r.cx() < cur->r.cx()) ||
                          (dy > 0 && mem->r.cy() > cur->r.cy()) || (dy < 0 && mem->r.cy() < cur->r.cy());
                if (ok) next = mem;
            }
        }
    }
    if (next) {
        if (cur->group && !(cur->flags & F_NO_MEMORY)) g_group_memory[cur->group] = cur->id;
        g_focus = next->id;
        g_last_focus_group = next->group;
        if (next->group && !(next->flags & F_NO_MEMORY)) g_group_memory[next->group] = next->id;
        if (!(next->flags & F_SILENT)) audio::play(audio::SFX_MOVE);
    } else {
        g_bump_id = g_focus;
        g_bump_dx = (float)dx;
        g_bump_dy = (float)dy;
        g_bump_t = g_time;
        audio::play(audio::SFX_BUMP, 0.5f);
    }
}

AnimState& anim_state(Id id) {
    AnimState& a = g_anim[id];
    a.last = g_frame;
    return a;
}

}  // namespace

// --- ids ----------------------------------------------------------------------------

Id id(const char* s) { return util::hash64(s); }
Id id(Id parent, const char* s) { return util::hash64(s, parent * 1099511628211ull + 7); }
Id id(Id parent, const std::string& s) { return util::hash64(s, parent * 1099511628211ull + 7); }
Id id(Id parent, int64_t i) {
    uint64_t h = parent ^ (0x9E3779B97F4A7C15ull + (uint64_t)i + (parent << 6) + (parent >> 2));
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

// --- theme ---------------------------------------------------------------------------

const Theme& theme() { return g_theme; }
void set_accent(int index) {
    g_accent = std::clamp(index, 0, accent_count() - 1);
    build_theme();
}
int accent_count() { return (int)(sizeof(ACCENTS) / sizeof(ACCENTS[0])); }
const char* accent_name(int index) { return ACCENTS[std::clamp(index, 0, accent_count() - 1)].name; }

// --- frame --------------------------------------------------------------------------------

void init() { build_theme(); }

void begin_frame(Input& in, float dt) {
    g_in = &in;
    g_dt = std::min(dt, 0.1f);
    g_time += g_dt;
    g_frame++;
    g_prev.swap(g_cur);
    g_cur.clear();
    g_layer = 0;
    g_layer_stack.clear();
    g_nav_suspended = false;
    for (auto it = g_press.begin(); it != g_press.end();) {
        it->second -= g_dt * 3.5f;
        if (it->second <= 0) it = g_press.erase(it);
        else ++it;
    }
    // Occasionally drop animation state for ids that disappeared.
    if (g_frame % 600 == 0) {
        for (auto it = g_anim.begin(); it != g_anim.end();) {
            if (g_frame - it->second.last > 600) it = g_anim.erase(it);
            else ++it;
        }
    }
}

void end_frame() {
    do_navigation();
}

Input& input() { return *g_in; }
float dt() { return g_dt; }
double time() { return g_time; }
uint64_t frame() { return g_frame; }

// --- focus -------------------------------------------------------------------------------

Item focusable(Id id, const Rect& r, Id group, int flags) {
    Rect sr = gfx::to_screen(r);
    Rect clip = gfx::current_clip();
    g_cur.push_back(Reg{id, sr, clip, group, g_layer, flags});

    Item it;
    int top = top_layer(g_prev);
    bool in_top = g_layer >= top;

    if (in_top && g_in) {
        bool visible = sr.intersects(clip);
        auto hit = [&](float x, float y) { return visible && sr.contains(x, y) && clip.contains(x, y); };
        if (g_in->pointer_moved && hit(g_in->px, g_in->py) && g_focus != id) {
            g_focus = id;
            g_last_focus_group = group;
            if (group && !(flags & F_NO_MEMORY)) g_group_memory[group] = id;
        }
        if (g_in->tap && hit(g_in->tx, g_in->ty)) {
            g_focus = id;
            g_last_focus_group = group;
            if (group && !(flags & F_NO_MEMORY)) g_group_memory[group] = id;
            it.clicked = true;
            g_in->tap = false;
        }
    }

    it.focused = (g_focus == id) && in_top;
    if (it.focused && g_in && g_in->pressed_(BTN_A)) {
        it.clicked = true;
        g_in->eat(BTN_A);
    }
    if (it.clicked) {
        g_press[id] = 1.0f;
        audio::play(audio::SFX_SELECT);
    }
    auto p = g_press.find(id);
    it.press = p != g_press.end() ? p->second : 0.0f;
    it.f = spring(util::hash64("focus", id), it.focused ? 1.0f : 0.0f, 320.0f, 26.0f);
    return it;
}

void reset_focus() {
    g_focus = 0;
    g_last_focus_group = 0;
}

void set_focus(Id id) {
    g_focus = id;
    g_focus_grace = 2;
    if (const Reg* r = find(g_cur, id)) g_last_focus_group = r->group;
}

Id focused() { return g_focus; }

bool focus_in_group(Id group) {
    for (auto& r : g_cur)
        if (r.id == g_focus) return r.group == group;
    for (auto& r : g_prev)
        if (r.id == g_focus) return r.group == group;
    return false;
}

Id group_memory(Id group) {
    auto it = g_group_memory.find(group);
    return it == g_group_memory.end() ? 0 : it->second;
}

void forget_group(Id group) { g_group_memory.erase(group); }
void set_group_entry(Id group, Id entry) { g_group_entry[group] = entry; }
void suspend_nav() { g_nav_suspended = true; }

void push_layer() {
    g_layer_stack.push_back(g_layer);
    g_layer = (int)g_layer_stack.size();
}

void pop_layer() {
    if (g_layer_stack.empty()) return;
    g_layer = g_layer_stack.back();
    g_layer_stack.pop_back();
}

int layer() { return g_layer; }

static float bump_curve() {
    float t = (float)(g_time - g_bump_t);
    if (t > 0.35f) return 0;
    return std::sin(t * 38.0f) * std::exp(-t * 11.0f) * 7.0f;
}

float bump_x(Id id) { return id == g_bump_id ? bump_curve() * g_bump_dx : 0; }
float bump_y(Id id) { return id == g_bump_id ? bump_curve() * g_bump_dy : 0; }

// --- animation ------------------------------------------------------------------------------

float tween(Id id, float target, float speed) {
    AnimState& a = anim_state(id);
    if (!a.init) {
        a.v = target;
        a.init = true;
    }
    a.v = anim::approach(a.v, target, speed, g_dt);
    if (std::fabs(a.v - target) < 0.0005f) a.v = target;
    return a.v;
}

float spring(Id id, float target, float stiffness, float damping) {
    AnimState& a = anim_state(id);
    if (!a.init) {
        a.s.x = target;
        a.init = true;
    }
    a.s.update(target, g_dt, stiffness, damping);
    return a.s.x;
}

void set_value(Id id, float v) {
    AnimState& a = anim_state(id);
    a.v = v;
    a.s.x = v;
    a.s.v = 0;
    a.init = true;
}

float scroll_follow(Id id, float a, float b, float view, float content, float margin, bool active) {
    ScrollState& s = g_scroll[id];
    float max_off = std::max(0.0f, content - view);
    if (active && !s.dragging) {
        if (a - margin < s.target) s.target = a - margin;
        if (b + margin > s.target + view) s.target = b + margin - view;
    }
    s.target = std::clamp(s.target, 0.0f, max_off);
    if (!s.init) {
        s.s.x = s.target;
        s.init = true;
    }
    if (s.dragging) s.s.x = s.target;
    else s.s.update(s.target, g_dt, 170.0f, 26.0f);
    return s.s.x;
}

float scroll_drag(Id id, const Rect& area, bool vertical, float content, float view) {
    ScrollState& s = g_scroll[id];
    float max_off = std::max(0.0f, content - view);
    if (g_in) {
        Rect sa = gfx::to_screen(area);
        if (g_in->touching && g_in->dragging && sa.contains(g_in->touch_start_x, g_in->touch_start_y)) {
            float d = vertical ? g_in->tdy : g_in->tdx;
            s.target -= d;
            s.drag_velocity = -d / std::max(g_dt, 0.001f);
            s.dragging = true;
        } else if (s.dragging) {
            s.dragging = false;
            s.target += s.drag_velocity * 0.18f;  // fling
        }
        if (g_in->wheel != 0 && g_in->pointer && sa.contains(g_in->px, g_in->py)) s.target -= g_in->wheel * 90.0f;
    }
    s.target = std::clamp(s.target, 0.0f, max_off);
    return s.target;
}

// --- chrome ------------------------------------------------------------------------------------

void toast(const std::string& msg, int icon, Color c) {
    if (c.a == 0) c = g_theme.accent;
    if (!g_toasts.empty() && g_toasts.back().msg == msg) {
        g_toasts.back().start = g_time;
        return;
    }
    g_toasts.push_back(Toast{msg, icon, c, g_time});
    if (g_toasts.size() > 3) g_toasts.pop_front();
}

void set_backdrop(const std::string& url) {
    if (url == g_backdrop) return;
    g_prev_backdrop = g_backdrop;
    g_backdrop = url;
    g_backdrop_t = g_time;
}

void draw_background() {
    const Theme& t = g_theme;
    gfx::fill_rect_vgrad(Rect(0, 0, W, H), t.bg_top, t.bg_bottom);

    // Blurred backdrop of the focused item, crossfaded.
    float bt = anim::smoothstep((float)(g_time - g_backdrop_t) / 0.7f);
    auto draw_backdrop = [&](const std::string& url, float alpha) {
        if (url.empty() || alpha <= 0.01f) return;
        const images::Image* img = images::get(url, 0, 0, images::BLUR);
        if (img && img->ready) {
            float a = alpha * images::fade(img, 0.5f);
            gfx::image_cover(img->tex, img->w, img->h, Rect(-40, -40, W + 80, H + 80), 0, Color(255, 255, 255, (uint8_t)(150 * a)));
        }
    };
    draw_backdrop(g_prev_backdrop, 1.0f - bt);
    draw_backdrop(g_backdrop, bt);

    // Slowly drifting accent glows for depth.
    float s = (float)g_time;
    gfx::glow(Rect(-260 + std::sin(s * 0.11f) * 80, -300 + std::cos(s * 0.07f) * 60, 900, 820), t.accent.alpha(0.16f));
    gfx::glow(Rect(W - 560 + std::cos(s * 0.09f) * 90, H - 520 + std::sin(s * 0.13f) * 70, 900, 820), t.accent2.alpha(0.12f));
    gfx::glow(Rect(W * 0.35f + std::sin(s * 0.05f) * 160, H * 0.3f, 700, 600), Color(120, 90, 255, 14));

    // Readability scrims.
    gfx::fill_rect_vgrad(Rect(0, H * 0.45f, W, H * 0.55f), Color(0, 0, 0, 0), Color(5, 4, 8, 190));
    gfx::fill_rect_hgrad(Rect(0, 0, W * 0.6f, H), Color(5, 4, 8, 120), Color(5, 4, 8, 0));
}

void draw_overlays() {
    // Toasts: slide down from the top center.
    float y = 22;
    for (auto it = g_toasts.begin(); it != g_toasts.end();) {
        float age = (float)(g_time - it->start);
        const float life = 2.8f;
        if (age > life) {
            it = g_toasts.erase(it);
            continue;
        }
        float in = anim::ease_out_back(age / 0.35f);
        float out = 1.0f - anim::smoothstep((age - (life - 0.35f)) / 0.35f);
        float a = std::min(1.0f, age / 0.15f) * out;
        float w = text::measure(font::body_bold, it->msg) + (it->icon ? 76 : 48);
        Rect r(W * 0.5f - w * 0.5f, y - (1 - in) * 30, w, 50);
        gfx::push_alpha(a);
        gfx::shadow(r, 22, Color(0, 0, 0, 150));
        gfx::fill_rrect(r, 25, Color(28, 24, 34, 245));
        gfx::stroke_rrect(r, 25, 1.5f, Color(255, 255, 255, 30));
        float tx = r.x + 24;
        if (it->icon) {
            text::icon(it->icon, 24, tx + 10, r.cy(), it->color);
            tx += 32;
        }
        text::draw(font::body_bold, tx, r.cy() - text::line_height(font::body_bold) * 0.5f, it->msg, g_theme.text);
        gfx::pop_alpha();
        y += 60 * a;
        ++it;
    }

    // Wii Remote / mouse pointer.
    if (g_in && g_in->pointer && !g_in->touching && g_in->idle_time < 4.0) {
        float a = std::clamp(4.0f - (float)g_in->idle_time, 0.0f, 1.0f);
        gfx::push_alpha(a);
        gfx::fill_circle(g_in->px, g_in->py, 11, Color(0, 0, 0, 90));
        gfx::fill_circle(g_in->px, g_in->py, 8, Color(255, 255, 255, 235));
        gfx::fill_circle(g_in->px, g_in->py, 4.5f, g_theme.accent);
        gfx::pop_alpha();
    }
}

void button_glyph(float cx, float cy, const char* button, float size) {
    Color c = Color(255, 255, 255, 230);
    float r = size * 0.5f;
    bool wide = strlen(button) > 1;
    Rect rr(cx - (wide ? r * 1.5f : r), cy - r, wide ? size * 1.5f : size, size);
    gfx::fill_rrect(rr, r, Color(255, 255, 255, 36));
    gfx::stroke_rrect(rr, r, 1.5f, Color(255, 255, 255, 80));
    text::Font f{text::BOLD, size * 0.55f};
    text::draw(f, cx, cy - text::line_height(f) * 0.5f, button, c, text::CENTER);
}

void hint_bar(std::initializer_list<Hint> hints, float y) {
    float x = W - 48;
    for (auto it = std::rbegin(hints); it != std::rend(hints); ++it) {
        float lw = text::measure(font::small_bold, it->label);
        x -= lw;
        text::draw(font::small_bold, x, y - text::line_height(font::small_bold) * 0.5f, it->label, g_theme.text2);
        float gw = strlen(it->button) > 1 ? 36 : 24;
        x -= 8 + gw * 0.5f;
        button_glyph(x, y, it->button, 24);
        x -= gw * 0.5f + 26;
    }
}

// --- widgets ------------------------------------------------------------------------------------

void spinner(float cx, float cy, float r, Color c, float width) {
    float t = (float)g_time;
    float head = t * 5.2f;
    float len = 0.6f + 0.45f * (1.0f + std::sin(t * 2.6f));
    gfx::stroke_circle(cx, cy, r, width, c.alpha(0.15f));
    gfx::stroke_arc(cx, cy, r, head, head + len * 2.2f, width, c);
}

void skeleton(const Rect& r, float radius) {
    float pulse = 0.5f + 0.5f * std::sin((float)g_time * 3.0f + r.x * 0.01f);
    gfx::fill_rrect(r, radius, Color(255, 255, 255, (uint8_t)(12 + 10 * pulse)));
    // A soft light band sweeping across.
    float period = 1.6f;
    float p = std::fmod((float)g_time + r.x * 0.0015f, period) / period;
    float bw = r.w * 0.5f;
    float bx = r.x - bw + (r.w + bw * 2) * p;
    float x0 = std::max(r.x + radius * 0.5f, bx), x1 = std::min(r.r() - radius * 0.5f, bx + bw);
    if (x1 > x0) {
        float mid = bx + bw * 0.5f;
        Color edge(255, 255, 255, 0), peak(255, 255, 255, 18);
        Rect inner(x0, r.y + radius * 0.3f, x1 - x0, r.h - radius * 0.6f);
        if (mid > x0 && mid < x1) {
            gfx::fill_rect_hgrad(Rect(x0, inner.y, mid - x0, inner.h), gfx::lerp(edge, peak, (x0 - bx) / (bw * 0.5f)), peak);
            gfx::fill_rect_hgrad(Rect(mid, inner.y, x1 - mid, inner.h), peak, gfx::lerp(peak, edge, (x1 - mid) / (bw * 0.5f)));
        }
    }
}

void focus_ring(const Rect& r, float radius, float f) {
    if (f <= 0.01f) return;
    float grow = 4 + 2 * f;
    Rect rr(r.x - grow, r.y - grow, r.w + grow * 2, r.h + grow * 2);
    gfx::stroke_rrect(rr, radius + grow, 3.5f, Color(255, 255, 255, (uint8_t)(240 * f)));
}

void focus_glow(const Rect& r, float f) {
    if (f <= 0.01f) return;
    gfx::glow(r.inset(-std::max(r.w, r.h) * 0.35f), g_theme.accent.alpha(0.30f * f));
    gfx::shadow(r, 26, Color(0, 0, 0, (uint8_t)(150 * f)));
}

float measure_button(const char* label, int icon) {
    float w = label && *label ? text::measure(font::label, label) + 52 : 0;
    if (icon) w += label && *label ? 34 : 56;
    return w;
}

bool button(Id id, const Rect& r, const char* label, int icon, int style, Id group, int flags) {
    Item it = focusable(id, r, group, flags);
    float s = 1.0f + 0.06f * it.f - 0.05f * it.press;
    Rect br = r.scaled(s).offset(bump_x(id), bump_y(id));
    const Theme& t = g_theme;
    float rad = br.h * 0.5f;

    if (it.f > 0.01f) gfx::shadow(br, 18, Color(0, 0, 0, (uint8_t)(120 * it.f)));
    Color fg;
    if (style == BTN_PRIMARY) {
        gfx::fill_rrect_hgrad(br, rad, t.accent, t.accent2);
        if (it.f > 0.01f) gfx::stroke_rrect(br.inset(-3), rad + 3, 3, Color(255, 255, 255, (uint8_t)(230 * it.f)));
        fg = gfx::rgb(0x1A1016);
    } else if (style == BTN_GHOST) {
        gfx::fill_rrect(br, rad, gfx::lerp(Color(255, 255, 255, 0), t.surface_focus, it.f));
        fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f);
    } else {
        gfx::fill_rrect(br, rad, gfx::lerp(t.surface_hi, t.surface_focus, it.f));
        fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f);
        if (style == BTN_DANGER) fg = gfx::lerp(t.bad, gfx::rgb(0xB91C1C), it.f);
    }
    if (style == BTN_PRIMARY) {
        fg = gfx::rgb(0x1A1016);
    }

    float cw = measure_button(label, icon) - 52 + (icon && !(label && *label) ? -56 : 0);
    float x = br.cx() - cw * 0.5f;
    if (icon) {
        float isz = br.h * 0.55f;
        if (label && *label) {
            text::icon(icon, isz, x + 12, br.cy(), fg);
            x += 34;
        } else {
            text::icon(icon, isz, br.cx(), br.cy(), fg);
        }
    }
    if (label && *label)
        text::draw(font::label, x, br.cy() - text::line_height(font::label) * 0.5f, label, fg);
    return it.clicked;
}

bool icon_button(Id id, float cx, float cy, float radius, int icon, Id group, int flags, bool active) {
    Rect r(cx - radius, cy - radius, radius * 2, radius * 2);
    Item it = focusable(id, r, group, flags);
    float s = 1.0f + 0.12f * it.f - 0.08f * it.press;
    float rr = radius * s;
    const Theme& t = g_theme;
    cx += bump_x(id);
    cy += bump_y(id);
    if (it.f > 0.01f) gfx::shadow(Rect(cx - rr, cy - rr, rr * 2, rr * 2), 16, Color(0, 0, 0, (uint8_t)(110 * it.f)));
    Color bg = active ? t.accent : t.surface_hi;
    gfx::fill_circle(cx, cy, rr, gfx::lerp(bg, t.surface_focus, it.f));
    Color fg = gfx::lerp(active ? gfx::rgb(0x1A1016) : t.text, gfx::rgb(0x15121A), it.f);
    text::icon(icon, rr * 1.05f, cx, cy, fg);
    return it.clicked;
}

bool chip(Id id, const Rect& r, const char* label, bool selected, Id group, int icon) {
    Item it = focusable(id, r, group);
    float s = 1.0f + 0.05f * it.f - 0.04f * it.press;
    Rect br = r.scaled(s).offset(bump_x(id), bump_y(id));
    const Theme& t = g_theme;
    float rad = br.h * 0.5f;
    Color fg;
    if (it.f > 0.5f) {
        gfx::fill_rrect(br, rad, gfx::lerp(t.surface_hi, t.surface_focus, it.f));
        fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f);
    } else if (selected) {
        gfx::fill_rrect_hgrad(br, rad, t.accent.alpha(0.9f), t.accent2.alpha(0.9f));
        fg = gfx::rgb(0x1A1016);
        if (it.f > 0.01f) gfx::stroke_rrect(br.inset(-3), rad + 3, 2.5f, Color(255, 255, 255, (uint8_t)(230 * it.f)));
    } else {
        gfx::fill_rrect(br, rad, gfx::lerp(t.surface, t.surface_focus, it.f));
        fg = gfx::lerp(t.text2, gfx::rgb(0x15121A), it.f);
    }
    float tw = text::measure(font::small_bold, label) + (icon ? 26 : 0);
    float x = br.cx() - tw * 0.5f;
    if (icon) {
        text::icon(icon, 20, x + 9, br.cy(), fg);
        x += 26;
    }
    text::draw(font::small_bold, x, br.cy() - text::line_height(font::small_bold) * 0.5f, label, fg);
    return it.clicked;
}

static void row_base(const Rect& r, const Item& it, Id id) {
    const Theme& t = g_theme;
    Rect br = r.offset(bump_x(id), bump_y(id));
    if (it.f > 0.01f) gfx::shadow(br, 16, Color(0, 0, 0, (uint8_t)(100 * it.f)));
    gfx::fill_rrect(br, 16, gfx::lerp(t.surface, t.surface_focus, it.f));
}

bool toggle_row(Id id, const Rect& r, const char* label, const char* desc, bool* value, Id group, int flags) {
    Item it = focusable(id, r, group, flags);
    if (it.clicked) {
        *value = !*value;
        audio::play(audio::SFX_TOGGLE);
    }
    row_base(r, it, id);
    const Theme& t = g_theme;
    Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f);
    Color fg2 = gfx::lerp(t.text2, Color(21, 18, 26, 170), it.f);
    Rect br = r.offset(bump_x(id), bump_y(id));
    float ty = desc && *desc ? br.y + 14 : br.cy() - text::line_height(font::label) * 0.5f;
    text::draw(font::label, br.x + 24, ty, label, fg);
    if (desc && *desc) text::draw_fit(font::small, br.x + 24, ty + 30, br.w - 140, desc, fg2);

    float on = tween(ui::id(id, "knob"), *value ? 1.0f : 0.0f, 16.0f);
    Rect track(br.r() - 24 - 56, br.cy() - 16, 56, 32);
    gfx::fill_rrect(track, 16, gfx::lerp(Color(120, 120, 130, 110), t.accent, on));
    float kx = track.x + 16 + on * 24;
    gfx::fill_circle(kx, track.cy() + 1, 13, Color(0, 0, 0, 60));
    gfx::fill_circle(kx, track.cy(), 12.5f, gfx::WHITE);
    return it.clicked;
}

bool value_row(Id id, const Rect& r, const char* label, const char* value, int icon, Id group, int flags) {
    Item it = focusable(id, r, group, flags);
    row_base(r, it, id);
    const Theme& t = g_theme;
    Color fg = gfx::lerp(t.text, gfx::rgb(0x15121A), it.f);
    Color fg2 = gfx::lerp(t.text2, Color(21, 18, 26, 170), it.f);
    Rect br = r.offset(bump_x(id), bump_y(id));
    float x = br.x + 24;
    if (icon) {
        text::icon(icon, 26, x + 12, br.cy(), fg);
        x += 40;
    }
    text::draw(font::label, x, br.cy() - text::line_height(font::label) * 0.5f, label, fg);
    if (value && *value) {
        float vw = std::min(text::measure(font::body, value), br.w * 0.5f);
        text::draw_fit(font::body, br.r() - 52 - vw, br.cy() - text::line_height(font::body) * 0.5f, vw, value, fg2);
    }
    text::icon(ic::CHEVRON_RIGHT, 26, br.r() - 30, br.cy(), fg2);
    return it.clicked;
}

void progress_bar(const Rect& r, float progress, float buffered, bool knob, float knob_scale) {
    const Theme& t = g_theme;
    progress = std::clamp(progress, 0.0f, 1.0f);
    buffered = std::clamp(buffered, 0.0f, 1.0f);
    float rad = r.h * 0.5f;
    gfx::fill_rrect(r, rad, Color(255, 255, 255, 45));
    if (buffered > progress) gfx::fill_rrect(Rect(r.x, r.y, r.w * buffered, r.h), rad, Color(255, 255, 255, 70));
    if (progress > 0) gfx::fill_rrect_hgrad(Rect(r.x, r.y, std::max(r.h, r.w * progress), r.h), rad, t.accent, t.accent2);
    if (knob) {
        float kx = r.x + r.w * progress, ks = 9 * knob_scale;
        gfx::fill_circle(kx, r.cy() + 1, ks + 1.5f, Color(0, 0, 0, 80));
        gfx::fill_circle(kx, r.cy(), ks, gfx::WHITE);
    }
}

}  // namespace ui
