// Network shares (SMB): the saved share list, the add/edit form and a folder
// browser that looks and behaves like My Media.
#include <algorithm>
#include <unordered_set>

#include "core/tasks.hpp"
#include "core/util.hpp"
#include "screens/media_actions.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "services/smb.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;
using namespace media;

// --- browsing a folder -----------------------------------------------------------------

struct Listing {
    std::vector<Entry> entries;               // folders and media, sorted
    std::unordered_set<std::string> files;    // every file, for sidecar lookups (.srt, cover.jpg)
    bool ok = false;
    std::string error;
};

Listing list_smb(const std::string& url) {
    Listing out;
    std::vector<smb::DirEntry> items;
    if (!smb::list_dir(url, items, out.error)) return out;
    for (const smb::DirEntry& d : items) {
        Entry e;
        e.name = d.name;
        e.path = util::join_path(url, d.name);
        if (d.is_dir) {
            e.kind = K_DIR;
        } else {
            out.files.insert(e.path);
            e.kind = kind_of(d.name);
            e.size = d.size;
            if (e.kind == K_OTHER) continue;  // hide subtitles, nfo, etc.
        }
        out.entries.push_back(std::move(e));
    }
    sort_entries(out.entries);
    out.ok = true;
    return out;
}

class FolderScreen : public app::Screen {
public:
    FolderScreen(std::string url, std::string title)
        : url_(std::move(url)), title_(std::move(title)), where_(smb::display_path(url_)) {}

    app::Section section() const override { return app::SEC_MEDIA; }

    void on_enter() override { reload(); }  // also refreshes resume badges after playback

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id(url_.c_str());
        page_.begin(id(g, "page"));

        float y = page_.y(64);
        text::draw(font::display, x0, y, title_, t.text);
        y += 66;
        text::draw_fit(font::small, x0, y, W - x0 - 60, where_, t.text3);
        y += 40;

        bool failed = !loading_ && !listing_.ok;
        if (failed) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::ERROR_OUTLINE, listing_.error.c_str(), "");
            y += 280;
            float bw = measure_button("Try again", ic::REFRESH);
            if (button(id(g, "retry"), Rect(x0 + (W - x0 - 60 - bw) * 0.5f, y, bw, 56), "Try again", ic::REFRESH))
                reload();
            y += 80;
        } else if (!loading_ && listing_.entries.empty()) {
            empty_state(Rect(x0, y, W - x0 - 60, 260), ic::FOLDER, "Nothing to play here",
                        "This folder has no videos, music or photos.");
            y += 300;
        } else {
            GridSpec gs;
            gs.count = (int)listing_.entries.size();
            gs.cols = 4;
            gs.item_w = 252;
            gs.loading = loading_;
            gs.shape = CARD_WIDE;
            gs.item = [this](int i) { return entry_card(listing_.entries[i], "smb"); };
            gs.on_click = [this](int i) { open((size_t)i); };
            y += grid(id(g, "grid"), x0, y, gs, &page_);
        }
        page_.end(y + page_.scroll());
        hint_bar({{"A", failed ? "Try again" : "Open"}, {"B", "Back"}});
    }

private:
    void open(size_t i) {
        const Entry& e = listing_.entries[i];
        if (e.kind == K_DIR) {
            app::push(std::make_unique<FolderScreen>(e.path, e.name));
            return;
        }
        open_file(listing_.entries, i, "smb", [this](const std::string& p) { return listing_.files.count(p) > 0; });
    }

    void reload() {
        std::string url = url_;
        loading_ = listing_.entries.empty();
        scope_.reset();
        scope_.run<Listing>([url] { return list_smb(url); },
                            [this](Listing l) {
                                listing_ = std::move(l);
                                loading_ = false;
                            });
    }

    std::string url_, title_, where_;
    bool loading_ = true;
    Listing listing_;
    tasks::Scope scope_;
    Page page_;
};

// --- add / edit a share --------------------------------------------------------------------

class ShareForm : public app::Screen {
public:
    explicit ShareForm(smb::Share s = {}) : s_(std::move(s)), original_name_(s_.name) {}

    app::Section section() const override { return app::SEC_MEDIA; }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("smb_form");
        float y = 64;
        text::draw(font::display, x0, y, original_name_.empty() ? "Add network share" : "Edit network share", t.text);
        y += 66;
        text::draw(font::body, x0, y, "A shared folder on a Windows PC, Mac, Linux server or NAS.", t.text2);
        y += 54;

        float w = std::min(780.0f, W - x0 - 60);
        Id rows = id(g, "rows");
        auto row = [&](const char* key, const char* label, const std::string& value, int icon) {
            bool clicked = value_row(id(g, key), Rect(x0, y, w, 62), label, value.c_str(), icon, rows);
            y += 72;
            return clicked && !connecting_;
        };

        if (row("host", "Computer", s_.host.empty() ? "Not set" : s_.host, ic::COMPUTER))
            prompt_text("Computer name or IP address", s_.host, "e.g. 192.168.1.20",
                        [this](std::string v) { set_host(v); }, false, true);
        if (row("share", "Shared folder", s_.share.empty() ? "Not set" : s_.share, ic::FOLDER))
            prompt_text("Shared folder", s_.share, "e.g. Media", [this](std::string v) { s_.share = clean_segment(v); });
        if (row("user", "Username", login().empty() ? "Guest" : login(), ic::PERSON))
            prompt_text("Username", login(), "Empty for guest access", [this](std::string v) { set_login(v); });
        if (row("password", "Password", s_.password.empty() ? "None" : masked(s_.password), ic::KEY))
            prompt_text("Password", s_.password, "", [this](std::string v) { s_.password = v; }, true);
        std::string auto_name = smb::default_name(s_);
        if (row("name", "Display name", !s_.name.empty() ? s_.name : !auto_name.empty() ? auto_name : "Automatic",
                ic::DESCRIPTION))
            prompt_text("Display name", s_.name, smb::default_name(s_), [this](std::string v) { s_.name = v; });

        y += 14;
        const char* label = connecting_ ? "Connecting\xE2\x80\xA6" : "Connect";
        float bw = std::max(measure_button("Connect", ic::LAN), measure_button(label));
        if (button(id(g, "connect"), Rect(x0, y, bw, 58), label, connecting_ ? 0 : ic::LAN, BTN_PRIMARY) && !connecting_)
            connect();
        if (connecting_) spinner(x0 + bw + 40, y + 29, 16, t.accent, 4);
        hint_bar({{"A", "Select"}, {"B", "Back"}});
    }

private:
    static std::string masked(const std::string& password) {
        std::string out;
        for (size_t i = 0; i < std::min<size_t>(password.size(), 12); i++) out += "\xE2\x80\xA2";  // bullet
        return out;
    }

    // "DOMAIN\user" <-> separate fields
    std::string login() const { return s_.domain.empty() ? s_.user : s_.domain + "\\" + s_.user; }
    void set_login(const std::string& v) {
        size_t bs = v.find('\\');
        s_.domain = bs == std::string::npos ? "" : v.substr(0, bs);
        s_.user = bs == std::string::npos ? v : v.substr(bs + 1);
    }

    static std::string clean_segment(std::string v) {
        v = util::replace_all(v, "\\", "/");
        while (!v.empty() && v.front() == '/') v.erase(0, 1);
        while (!v.empty() && v.back() == '/') v.pop_back();
        return v;
    }

    // Accepts a bare host as well as \\host\share or smb://host/share.
    void set_host(std::string v) {
        if (util::starts_with(util::lower(v), "smb://")) v = v.substr(6);
        v = clean_segment(v);
        size_t slash = v.find('/');
        if (slash != std::string::npos) {
            std::string share = clean_segment(v.substr(slash + 1));
            if (!share.empty()) s_.share = share;
            v = v.substr(0, slash);
        }
        s_.host = v;
    }

    void connect() {
        if (s_.host.empty() || s_.share.empty()) {
            toast(s_.host.empty() ? "Enter the computer name or IP address" : "Enter the shared folder name",
                  ic::ERROR_OUTLINE, theme().warn);
            set_focus(id(id("smb_form"), s_.host.empty() ? "host" : "share"));
            return;
        }
        struct Result {
            bool ok = false;
            std::string error;
        };
        connecting_ = true;
        smb::Share s = s_;
        scope_.run<Result>(
            [s] {
                Result r;
                r.ok = smb::test_share(s, r.error);
                return r;
            },
            [this](Result r) {
                connecting_ = false;
                if (!r.ok) {
                    toast(r.error, ic::ERROR_OUTLINE, theme().bad);
                    return;
                }
                if (!original_name_.empty()) smb::remove_share(original_name_);
                std::string name = smb::save_share(s_);
                toast("Connected to " + name, ic::CHECK_CIRCLE, theme().good);
                app::pop();
            });
    }

    smb::Share s_;
    std::string original_name_;  // editing: the entry to replace
    bool connecting_ = false;
    tasks::Scope scope_;
};

// --- saved shares ------------------------------------------------------------------------

class SharesScreen : public app::Screen {
public:
    app::Section section() const override { return app::SEC_MEDIA; }

    void on_enter() override { shares_ = smb::saved_shares(); }

    void frame() override {
        const Theme& t = theme();
        float x0 = content_x();
        Id g = id("smb_shares");
        page_.begin(id(g, "page"));

        float y = page_.y(64);
        text::draw(font::display, x0, y, "Network shares", t.text);
        y += 66;
        text::draw(font::body, x0, y, "Videos, music and photos shared from a PC, Mac or NAS on your network.", t.text2);
        y += 50;

        // Actions are applied after the grid: they change the list it is drawing.
        int focused = -1, remove = -1;
        GridSpec gs;
        gs.count = (int)shares_.size() + 1;
        gs.cols = 4;
        gs.item_w = 252;
        gs.shape = CARD_WIDE;
        gs.item = [this](int i) {
            CardInfo c;
            if (i == (int)shares_.size()) {
                c.title = "Add share";
                c.subtitle = "Connect to a computer or NAS";
                c.icon = ic::ADD;
                return c;
            }
            const smb::Share& s = shares_[i];
            c.title = s.name;
            c.subtitle = "\\\\" + s.host + "\\" + s.share;
            c.icon = ic::DNS;
            return c;
        };
        gs.on_click = [this](int i) {
            if (i == (int)shares_.size()) app::push(std::make_unique<ShareForm>());
            else app::push(std::make_unique<FolderScreen>(smb::share_url(shares_[i]), shares_[i].name));
        };
        gs.on_focus = [&](int i) { focused = i; };
        gs.on_x = [&](int i) { remove = i; };
        y += grid(id(g, "grid"), x0, y, gs, &page_);

        if (shares_.empty()) {
            y += text::draw_wrapped(font::body, Rect(x0, y + 10, std::min(760.0f, W - x0 - 60), 120),
                                    "Share a folder on your computer first (on Windows: right-click it, "
                                    "Properties \xE2\x80\xBA Sharing), then add it here with the computer's name "
                                    "or IP address.",
                                    t.text3, 4);
        }
        page_.end(y + 20 + page_.scroll());

        bool on_share = focused >= 0 && focused < (int)shares_.size();
        if (on_share && input().pressed_(BTN_Y)) {
            input().eat(BTN_Y);
            app::push(std::make_unique<ShareForm>(shares_[focused]));
        }
        if (remove >= 0 && remove < (int)shares_.size()) confirm_remove(shares_[remove].name);
        if (on_share) hint_bar({{"A", "Open"}, {"Y", "Edit"}, {"X", "Remove"}, {"B", "Back"}});
        else hint_bar({{"A", "Add"}, {"B", "Back"}});
    }

private:
    // X twice to remove, so a stray press doesn't forget a password.
    void confirm_remove(const std::string& name) {
        if (pending_remove_ != name || ui::time() - pending_since_ > 4) {
            pending_remove_ = name;
            pending_since_ = ui::time();
            toast("Press X again to remove " + name, ic::DELETE, theme().warn);
            return;
        }
        smb::remove_share(name);
        pending_remove_.clear();
        shares_ = smb::saved_shares();
        toast("Removed " + name, ic::DELETE);
    }

    std::vector<smb::Share> shares_;
    std::string pending_remove_;
    double pending_since_ = 0;
    Page page_;
};

}  // namespace

std::unique_ptr<app::Screen> smb_browser_screen() { return std::make_unique<SharesScreen>(); }

}  // namespace screens
