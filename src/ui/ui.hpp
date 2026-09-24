// Immediate-mode 10-foot UI toolkit.
//
// Screens draw every frame and register focusable rects as they go. At the end
// of the frame the D-pad moves focus spatially between this frame's rects, so
// layouts never need an explicit focus graph. Per-group memory makes moving
// between shelves return to the item you were last on (like streaming apps).
#pragma once

#include <cstdint>
#include <string>
#include <initializer_list>

#include "core/input.hpp"
#include "gfx/gfx.hpp"
#include "gfx/text.hpp"

namespace ui {

using Id = uint64_t;
using gfx::Color;
using gfx::Rect;

constexpr float W = 1280, H = 720;

Id id(const char* s);
Id id(Id parent, const char* s);
Id id(Id parent, const std::string& s);
Id id(Id parent, int64_t i);

// --- theme ---------------------------------------------------------------------
struct Theme {
    Color bg_top, bg_bottom;
    Color surface, surface_hi, surface_focus;
    Color text, text2, text3;
    Color accent, accent2;  // gradient pair
    Color good, warn, bad;
};
const Theme& theme();
void set_accent(int index);  // see accent_names()
int accent_count();
const char* accent_name(int index);

// Font presets
namespace font {
constexpr text::Font caption{text::MEDIUM, 14};
constexpr text::Font small{text::REGULAR, 16};
constexpr text::Font small_bold{text::SEMIBOLD, 16};
constexpr text::Font body{text::REGULAR, 19};
constexpr text::Font body_bold{text::SEMIBOLD, 19};
constexpr text::Font label{text::MEDIUM, 21};
constexpr text::Font title{text::BOLD, 26};
constexpr text::Font headline{text::BOLD, 34};
constexpr text::Font display{text::BOLD, 46};
}  // namespace font

// --- frame ------------------------------------------------------------------------
void init();
void begin_frame(Input& in, float dt);
void end_frame();
Input& input();
float dt();
double time();
uint64_t frame();

// --- focus ------------------------------------------------------------------------
enum ItemFlags {
    F_DEFAULT = 1,      // take focus if nothing in this layer is focused
    F_SILENT = 2,       // no move sound when focused
    F_NO_MEMORY = 4,    // don't remember as the group's last item
    F_SIDE_ENTRY = 8,   // only reachable horizontally from other groups (side rails)
};

struct Item {
    bool focused = false;
    bool clicked = false;   // A pressed / tapped this frame
    float f = 0;            // focus animation 0..1 (springy)
    float press = 0;        // 1 right after activation, decays (for a squish effect)
};

Item focusable(Id id, const Rect& r, Id group = 0, int flags = 0);
void set_focus(Id id);           // takes effect immediately
void reset_focus();              // next frame focuses the layer's F_DEFAULT (or first) item
Id focused();
bool focus_in_group(Id group);
Id group_memory(Id group);
// When navigation enters `group` from outside, land on `entry` (if present).
void set_group_entry(Id group, Id entry);
void forget_group(Id group);
// Disable automatic D-pad navigation this frame (custom handling).
void suspend_nav();
// Modal layers: only the topmost layer present receives focus/navigation.
void push_layer();
void pop_layer();
int layer();
// Offset (pixels) for the focused item while bumping into an edge.
float bump_x(Id id);
float bump_y(Id id);

// --- animation state keyed by id ---------------------------------------------------
float tween(Id id, float target, float speed = 14.0f);   // exponential approach
float spring(Id id, float target, float stiffness = 280.0f, float damping = 24.0f);
void set_value(Id id, float v);                           // snap an animated value

// Scroll helper: returns the smoothed offset for a scroller whose target keeps
// the range [a, b] (content space) visible inside a view of `view` pixels.
float scroll_follow(Id id, float a, float b, float view, float content, float margin, bool active);
// Touch/wheel scrolling on top of the follow target; returns the new offset.
float scroll_drag(Id id, const Rect& area, bool vertical, float content, float view);

// --- global chrome ----------------------------------------------------------------
void toast(const std::string& msg, int icon = 0, Color c = Color(0, 0, 0, 0));
void set_backdrop(const std::string& image_url);   // blurred ambient background
void draw_background();
void draw_overlays();                               // toasts, pointer cursor
struct Hint { const char* button; const char* label; };
void hint_bar(std::initializer_list<Hint> hints, float y = H - 44);

// --- widgets ------------------------------------------------------------------------
void spinner(float cx, float cy, float r, Color c, float width = 4);
void skeleton(const Rect& r, float radius);
void focus_ring(const Rect& r, float radius, float f);   // draw after the item
void focus_glow(const Rect& r, float f);                  // draw before the item
void button_glyph(float cx, float cy, const char* button, float size = 26);

enum ButtonStyle { BTN_NORMAL, BTN_PRIMARY, BTN_GHOST, BTN_DANGER };
bool button(Id id, const Rect& r, const char* label, int icon = 0, int style = BTN_NORMAL, Id group = 0,
            int flags = 0);
bool icon_button(Id id, float cx, float cy, float radius, int icon, Id group = 0, int flags = 0,
                 bool active = false);
bool chip(Id id, const Rect& r, const char* label, bool selected, Id group = 0, int icon = 0);
// Settings-style rows
bool toggle_row(Id id, const Rect& r, const char* label, const char* desc, bool* value, Id group = 0);
bool value_row(Id id, const Rect& r, const char* label, const char* value, int icon = 0, Id group = 0);
// Horizontal progress/seek bar with optional buffered range.
void progress_bar(const Rect& r, float progress, float buffered = 0, bool knob = false, float knob_scale = 1);

float measure_button(const char* label, int icon = 0);

}  // namespace ui
