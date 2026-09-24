// Composite widgets shared by the content screens.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/app.hpp"
#include "ui/ui.hpp"

namespace screens {

using ui::Id;
using ui::Rect;

enum CardShape { CARD_WIDE, CARD_POSTER, CARD_SQUARE, CARD_CIRCLE };

struct CardInfo {
    std::string image;
    std::string title, subtitle;
    std::string badge;           // e.g. duration "12:34" (bottom right of the image)
    int icon = 0;                // placeholder icon when there is no image
    float progress = -1;         // resume bar 0..1
    bool live = false;           // red LIVE badge
    bool favorite = false;
    int image_w = 400;           // decode size hint
    ui::Color tint{0, 0, 0, 0};  // placeholder gradient color (default: accent-derived)
};

// Card with image on top and text below; returns true when activated.
bool card(Id id, const Rect& image_rect, CardShape shape, const CardInfo& c, Id group, int flags = 0);
float card_text_height(CardShape shape);

// Vertically scrolling page that keeps the focused widget in view.
class Page {
public:
    // Call at the start of the frame; returns the scroll offset.
    float begin(Id id, float view_top = 0, float view_h = ui::H);
    // Widgets report their content-space extent while focused.
    void focus_range(float top, float bottom);
    void end(float content_bottom);  // content-space bottom
    float scroll() const { return scroll_; }
    float y(float content_y) const { return content_y - scroll_; }

private:
    Id id_ = 0;
    float scroll_ = 0, view_top_ = 0, view_h_ = ui::H;
    float fa_ = 0, fb_ = 0, next_fa_ = 0, next_fb_ = 0;
    bool has_focus_ = false, next_has_focus_ = false;
    float content_h_ = 0;
};

// Horizontal row of cards. Returns the height used.
struct ShelfSpec {
    const char* title = nullptr;
    std::string title_str;
    int count = 0;
    CardShape shape = CARD_WIDE;
    float item_w = 300;
    bool loading = false;
    std::function<CardInfo(int)> item;
    std::function<void(int)> on_click;
    std::function<void(int)> on_focus;   // e.g. update the backdrop
    std::function<void(int)> on_x;       // secondary action (X button)
    std::function<void(int)> on_y;       // another one (Y button)
};
float shelf(Id id, float x, float y, const ShelfSpec& spec, Page* page = nullptr);

// Grid of cards (search results, libraries). Returns the height used.
struct GridSpec {
    int count = 0;
    int cols = 4;
    CardShape shape = CARD_WIDE;
    float item_w = 250;
    float gap_x = 22;
    bool loading = false;
    std::function<CardInfo(int)> item;
    std::function<void(int)> on_click;
    std::function<void(int)> on_focus;
    std::function<void(int)> on_x;
    std::function<void(int)> on_y;
    std::function<void()> on_reach_end;  // pagination
};
float grid(Id id, float x, float y, const GridSpec& spec, Page* page = nullptr);

void empty_state(const Rect& r, int icon, const char* title, const char* desc);
// Empty state with a focusable action button; returns true when pressed.
bool empty_state_action(Id id, const Rect& r, int icon, const char* title, const char* desc,
                        const char* action = "Try again", int action_icon = ic::REFRESH);
void section_title(float x, float y, const char* title, const char* subtitle = nullptr);
// Pill-shaped fake text field that opens the keyboard when activated.
bool search_bar(Id id, const Rect& r, const std::string& query, const char* placeholder, Id group = 0, int flags = 0);
void loading_indicator(float cx, float cy, const char* label = nullptr);

// Opens the system keyboard; `done` receives the text (not called on cancel).
void prompt_text(const std::string& title, const std::string& initial, const std::string& hint,
                 std::function<void(std::string)> done, bool password = false, bool url = false);
// Draws the keyboard overlay; called by the app every frame.
void draw_prompt();
bool prompt_active();

// Side panel of actions for one item (the X "More" button). B or an action closes it; the
// action runs after it has closed, so it can open screens or other menus.
struct MenuItem {
    std::string label;
    int icon = 0;
    std::function<void()> action;
};
void show_menu(const std::string& title, const std::string& subtitle, std::vector<MenuItem> items);
bool menu_active();
void close_menu();
// Draws the menu; called by the app every frame.
void draw_menu();

// Other screens
std::unique_ptr<app::Screen> make_media_folder(const std::string& path, const std::string& title);
std::unique_ptr<app::Screen> make_reader(const std::string& path);
std::unique_ptr<app::Screen> make_licenses();
std::unique_ptr<app::Screen> smb_browser_screen();
std::unique_ptr<app::Screen> dlna_servers_screen();

}  // namespace screens
