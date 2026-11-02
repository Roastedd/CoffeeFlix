// "Who's watching?": the profiles as round avatars, a "+" to add one, and a Manage mode to rename,
// recolor and delete. Opened at the start when there is more than one profile, and from Settings.
#include <cstring>

#include "core/i18n.hpp"
#include "core/lock.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "services/kids.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

constexpr uint32_t COLORS[] = {0xE53935, 0x1E88E5, 0x43A047, 0xFB8C00, 0x8E24AA, 0x00ACC1};
constexpr int NUM_COLORS = sizeof(COLORS) / sizeof(COLORS[0]);

const char* color_name(int i) {
    static const char* const names[] = {N_("Red"), N_("Blue"), N_("Green"), N_("Orange"), N_("Purple"), N_("Teal")};
    return tr(names[((i % NUM_COLORS) + NUM_COLORS) % NUM_COLORS]);
}

Color avatar_color(int i) {
    const uint32_t c = COLORS[((i % NUM_COLORS) + NUM_COLORS) % NUM_COLORS];
    return Color((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
}

// What a profile is called: its name, or "Main" for the first one before it's been named.
std::string shown_name(const store::Profile& p) {
    return !p.name.empty() ? p.name : p.id == store::MAIN_PROFILE ? std::string(tr("Main")) : std::string(tr("Profile"));
}

// The first character of the name (a whole UTF-8 character), upper-cased when it's a plain letter.
std::string initial(const std::string& name) {
    if (name.empty()) return "?";
    size_t n = 1;
    while (n < name.size() && ((unsigned char)name[n] & 0xC0) == 0x80) n++;
    std::string s = name.substr(0, n);
    if (s.size() == 1) s[0] = (char)toupper((unsigned char)s[0]);
    return s;
}

// A color not used by the profiles yet, for the next one.
int free_color() {
    bool used[NUM_COLORS] = {};
    for (const auto& p : store::profiles()) used[((p.color % NUM_COLORS) + NUM_COLORS) % NUM_COLORS] = true;
    for (int i = 0; i < NUM_COLORS; i++)
        if (!used[i]) return i;
    return (int)store::profiles().size() % NUM_COLORS;
}

class ProfilePicker : public app::Screen {
public:
    explicit ProfilePicker(bool at_start) : at_start_(at_start) { want_focus_ = at_start ? store::last_profile() : store::active_profile(); }

    bool fullscreen() const override { return true; }
    bool draws_background() const override { return true; }  // plain: no picture of the page behind it shows through
    app::Section section() const override { return app::SEC_NONE; }

    // From the start there is nothing behind it to go back to; B only ends managing.
    bool on_back() override {
        if (managing_) {
            managing_ = false;
            return true;
        }
        return at_start_;
    }

    void frame() override {
        const Theme& t = theme();
        gfx::fill_rect_vgrad(Rect(0, 0, W, H), t.bg_top, t.bg_bottom);
        Id g = id("profiles");
        const auto list = store::profiles();
        const bool can_add = list.size() < store::MAX_PROFILES;
        const int tiles = (int)list.size() + (can_add ? 1 : 0);

        // The title: centred, inside the area a TV may cut off at the edges.
        text::draw_fit(font::display, W * 0.5f, 104, W - 2 * SAFE_X, managing_ ? tr("Manage profiles") : tr("Who's watching?"),
                       t.text, text::CENTER);

        // One row, centred: 170 wide each with 40 between (five of them, with the "+", still fit). A name may be a little
        // wider than its tile, into the gap.
        constexpr float TILE = 170, GAP = 40, AVATAR = 140, TOP = 236;
        const float total = tiles * TILE + (tiles - 1) * GAP;
        float x = (W - total) * 0.5f;
        for (size_t i = 0; i < list.size(); i++, x += TILE + GAP) draw_profile(g, list[i], x, TOP, TILE, AVATAR);
        if (can_add) draw_add(g, x, TOP, TILE, AVATAR);

        Id bg = id(g, "bar");
        const char* label = managing_ ? tr("Done") : tr("Manage profiles");
        const float bw = std::max(260.0f, measure_button(label, managing_ ? ic::CHECK : ic::EDIT));
        if (button(id(bg, "manage"), Rect((W - bw) * 0.5f, 520, bw, 52), label, managing_ ? ic::CHECK : ic::EDIT,
                   managing_ ? BTN_PRIMARY : BTN_NORMAL, bg)) {
            managing_ = !managing_;
        }
        if (at_start_) hint_bar({{"A", tr("Select")}});
        else hint_bar({{"A", managing_ ? tr("Edit") : tr("Select")}, {"B", managing_ ? tr("Done") : tr("Back")}});
    }

private:
    static constexpr float SAFE_X = 64;  // 5% of the width, where a TV may cut the picture off

    void draw_profile(Id g, const store::Profile& p, float x, float top, float tile, float avatar) {
        const Theme& t = theme();
        const std::string name = shown_name(p);
        const Rect hit(x - 6, top - 6, tile + 12, avatar + 130);
        const bool want = p.id == want_focus_;
        Item it = focusable(id(g, p.id.c_str()), hit, g, want ? F_DEFAULT : 0);
        const float f = it.f;
        const float cx = x + tile * 0.5f, cy = top + avatar * 0.5f + 4;
        const float r = avatar * 0.5f * (1 + 0.06f * f);
        gfx::fill_circle(cx, cy, r, avatar_color(p.color));
        text::draw(font::display, cx, cy - text::line_height(font::display) * 0.5f, initial(name), Color(255, 255, 255, 255), text::CENTER);
        if (f > 0.01f) focus_ring(Rect(cx - r - 8, cy - r - 8, 2 * r + 16, 2 * r + 16), r + 8, f);
        if (managing_) {  // a pencil on the avatar's corner
            gfx::fill_circle(cx + r * 0.72f, cy + r * 0.72f, 22, Color(30, 28, 36, 255));
            text::icon(ic::EDIT, 24, cx + r * 0.72f, cy + r * 0.72f, Color(255, 255, 255, 255));
        }
        text::draw_fit(font::label, cx, top + avatar + 30, tile + 30, name, gfx::lerp(t.text2, t.text, f), text::CENTER);
        // Under the name: a kids profile says so, and the one in use says that.
        const bool now = p.id == store::active_profile() && !at_start_;
        if (p.kids || now) {
            std::string line = p.kids ? tr("Kids") : "";
            if (now) line += (line.empty() ? "" : "  \xC2\xB7  ") + std::string(tr("Watching now"));
            text::draw_fit(font::small_bold, cx, top + avatar + 66, tile + 30, line, p.kids ? t.accent : t.text3, text::CENTER);
        }
        if (it.clicked) {
            if (managing_) edit(p);
            else app::use_profile(p.id, true);
        }
    }

    void draw_add(Id g, float x, float top, float tile, float avatar) {
        const Theme& t = theme();
        const Rect hit(x - 6, top - 6, tile + 12, avatar + 130);
        Item it = focusable(id(g, "add"), hit, g);
        const float f = it.f;
        const float cx = x + tile * 0.5f, cy = top + avatar * 0.5f + 4;
        const float r = avatar * 0.5f * (1 + 0.06f * f);
        gfx::fill_circle(cx, cy, r, Color(255, 255, 255, (uint8_t)(24 + 30 * f)));
        text::icon(ic::ADD, 56, cx, cy, gfx::lerp(t.text2, t.text, f));
        if (f > 0.01f) focus_ring(Rect(cx - r - 8, cy - r - 8, 2 * r + 16, 2 * r + 16), r + 8, f);
        text::draw_fit(font::label, cx, top + avatar + 30, tile + 30, tr("Add profile"), gfx::lerp(t.text2, t.text, f), text::CENTER);
        if (it.clicked) {
            prompt_text(tr("Name of the new profile"), "", tr("Their name"), [this](std::string name) {
                name = store::clean_profile_name(name);
                const int n = (int)store::profiles().size();
                if (name.empty()) name = util::fmt(tr("Profile %d"), n + 1);
                const std::string made = store::add_profile(name, free_color(), false);
                if (made.empty()) {
                    toast(tr("There is no room for more profiles"), ic::ERROR_OUTLINE);
                    return;
                }
                want_focus_ = made;
                set_focus(id(id("profiles"), made.c_str()));  // the new one, ready to open
            });
        }
    }

    void edit(const store::Profile& p) {
        const std::string id = p.id, name = shown_name(p);
        std::vector<MenuItem> items;
        items.push_back({tr("Rename"), ic::EDIT, [this, id, name] {
                             prompt_text(tr("Rename the profile"), name, tr("Their name"), [this, id](std::string v) {
                                 v = store::clean_profile_name(v);
                                 if (!v.empty()) store::rename_profile(id, v);
                                 want_focus_ = id;
                             });
                         }, ""});
        items.push_back({tr("Change color"), ic::PALETTE, [this, id] { pick_color(id); }, ""});
        if (p.kids) items.push_back({tr("Screen time"), ic::SCHEDULE, [id] { app::push(make_screen_time(id)); }, tr("How long a day, and quiet hours at night")});
        items.push_back({tr("Birthday"), ic::STAR, [id] { app::push(make_birthday(id)); }, tr("The month and day, kept on this console")});
        items.push_back({tr("Activity"), ic::HISTORY, [id] { app::push(make_activity(id)); }, tr("What they watched and listened to")});
        if (id != store::MAIN_PROFILE) {
            const bool kids = p.kids;
            items.push_back({kids ? tr("Make it a standard profile") : tr("Make it a kids profile"), ic::PERSON,
                             [this, id, kids] { set_kids(id, !kids); },
                             kids ? "" : tr("Only the channels you allow, no search, and a code to leave")});
            items.push_back({tr("Delete profile"), ic::DELETE, [this, id, name] { confirm_delete(id, name); },
                             tr("Removes its sign-ins, favorites and continue watching")});
        }
        show_menu(name, "", std::move(items));
    }

    // A kids profile needs the grown-ups' code, to leave it: one is set first if there isn't one.
    void set_kids(const std::string& id, bool kids) {
        auto apply = [this, id, kids] {
            store::set_profile_kids(id, kids);
            if (kids) {  // something to watch at once: the suggested channels, podcasts and radio (the grown-ups can change them)
                const int n = kids::seed_defaults_once();
                if (n) toast(util::fmt(tr("%d suggested channels, podcasts and radio stations added for kids"), n), ic::CHECK_CIRCLE);
            }
            want_focus_ = id;
            if (id == store::active_profile()) app::use_profile(id, true);  // the one in use: start over as it is now
        };
        if (kids && !lock::is_set()) app::push(make_lock_setup([apply](bool ok) { if (ok) apply(); }));
        else apply();
    }

    void pick_color(const std::string& id) {
        std::vector<MenuItem> items;
        for (int i = 0; i < NUM_COLORS; i++)
            items.push_back({color_name(i), ic::PALETTE, [this, id, i] {
                                 store::set_profile_color(id, i);
                                 want_focus_ = id;
                             }, ""});
        show_menu(tr("Change color"), "", std::move(items));
    }

    void confirm_delete(const std::string& id, const std::string& name) {
        show_menu(util::fmt(tr("Delete %s?"), name.c_str()), tr("This can't be undone."),
                  {{tr("Delete"), ic::DELETE, [this, id] {
                        if (id == store::active_profile()) app::use_profile(store::MAIN_PROFILE, false);  // not the one in use
                        store::remove_profile(id);
                        want_focus_ = store::MAIN_PROFILE;
                        reset_focus();
                    }, ""},
                   {tr("Keep it"), ic::CLOSE, [] {}, ""}});
    }

    bool at_start_;
    bool managing_ = false;
    std::string want_focus_;
};

}  // namespace

void draw_profile_avatar(const std::string& profile_id, float cx, float cy, float r) {
    const store::Profile p = store::profile(profile_id);
    if (p.id.empty()) return;
    gfx::fill_circle(cx, cy, r, avatar_color(p.color));
    text::Font f{text::BOLD, r * 1.1f};
    text::draw(f, cx, cy - text::line_height(f) * 0.5f, initial(shown_name(p)), Color(255, 255, 255, 255), text::CENTER);
}

std::unique_ptr<app::Screen> make_profile_picker(bool at_start) { return std::make_unique<ProfilePicker>(at_start); }

}  // namespace screens
