// YouTube comments beside the video, like the watch page: the top comments or the newest,
// each with its replies.
#include <algorithm>
#include <cmath>

#include "audio/mixer.hpp"
#include "core/i18n.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
#include "screens/youtube_common.hpp"
#include "ui/ui.hpp"

namespace screens {
namespace yt {

using namespace ui;

namespace {

constexpr float PAD = 22, AVATAR = 36, GAP = 14;
constexpr int COLLAPSED_LINES = 5;
const text::Font TEXT_FONT = font::small;

// One list: a video's comments, or one comment's replies.
struct List {
    std::vector<youtube::Comment> items;
    std::vector<std::vector<std::string>> lines;  // each comment's text, wrapped once
    std::vector<bool> expanded;
    std::string continuation, count, error;
    bool loading = false, loaded = false, off = false;
    float wrap_w = 0;
    tasks::Scope scope;

    void clear() {
        scope.reset();
        items.clear();
        lines.clear();
        expanded.clear();
        continuation.clear();
        count.clear();
        error.clear();
        loading = loaded = off = false;
    }
};

// "2,457,632 Comments" -> "2,457,632"
std::string count_number(const std::string& s) {
    size_t sp = s.find(' ');
    return sp == std::string::npos ? s : s.substr(0, sp);
}

}  // namespace

struct CommentsPanel::Impl {
    std::string video_id;
    bool newest = false;
    List main, replies;
    youtube::Comment parent;  // whose replies are open
    bool in_replies = false;
    bool want_focus = false;
    Id main_focus = 0;        // focus to go back to from the replies

    List& list() { return in_replies ? replies : main; }

    void load(List& l, const std::string& token) {
        if (l.loading) return;
        l.loading = true;
        std::string vid = video_id;
        bool nw = newest;
        List* lp = &l;
        l.scope.run<youtube::Comments>([vid, nw, token] { return youtube::comments(vid, nw, token); },
                                       [lp](youtube::Comments c) {
                                           lp->loading = false;
                                           lp->loaded = true;
                                           if (!c.ok) {
                                               lp->error = c.error;
                                               return;
                                           }
                                           if (!c.count.empty()) lp->count = c.count;
                                           lp->off = c.off;
                                           lp->continuation = c.continuation;
                                           for (auto& cm : c.items) lp->items.push_back(std::move(cm));
                                       });
    }

    void reload() {
        main.clear();
        in_replies = false;
        load(main, "");
    }

    void open_replies(const youtube::Comment& c) {
        main_focus = focused();
        parent = c;
        replies.clear();
        replies.items.push_back(c);  // the comment itself, above its replies
        in_replies = true;
        want_focus = true;
        load(replies, c.replies_token);
        audio::play(audio::SFX_OPEN, 0.6f);
    }

    bool back() {
        if (!in_replies) return false;
        in_replies = false;
        replies.clear();
        if (main_focus) set_focus(main_focus);
        audio::play(audio::SFX_BACK);
        return true;
    }

    // Replies sit a little to the right of the comment they answer.
    float indent(const List& l, size_t i) const { return &l == &replies && l.items[i].reply ? 30.0f : 0.0f; }

    // Wraps new comments' text for the panel's width.
    void layout(List& l, float w) {
        if (w != l.wrap_w) {
            l.lines.clear();
            l.wrap_w = w;
        }
        while (l.lines.size() < l.items.size()) {
            size_t i = l.lines.size();
            l.lines.push_back(text::wrap(TEXT_FONT, l.items[i].text, w - indent(l, i), 60));
            l.expanded.push_back(false);
        }
    }

    float item_height(const List& l, size_t i) const {
        const youtube::Comment& c = l.items[i];
        float lh = text::line_height(TEXT_FONT) * 1.15f;
        size_t n = l.lines[i].size();
        bool more = n > (size_t)COLLAPSED_LINES && !l.expanded[i];
        float h = 12 + 30;                         // padding, author row
        if (!c.pinned.empty()) h += 24;
        h += lh * (more ? COLLAPSED_LINES : n);
        if (more) h += 24;                         // "Read more"
        h += 6 + 24 + 10;                          // likes / replies, padding
        return h;
    }

    // Draws a comment at y; returns true when it's activated.
    bool draw_comment(List& l, size_t i, Id iid, Id group, const Rect& box, float x0, float tw) {
        const Theme& t = theme();
        const youtube::Comment& c = l.items[i];
        Item it = focusable(iid, box, group, i == 0 ? F_DEFAULT : 0);
        if (it.f > 0.01f) {
            gfx::fill_rrect(box, 16, Color(255, 255, 255, (uint8_t)(20 * it.f)));
            gfx::stroke_rrect(box, 16, 2.5f, Color(255, 255, 255, (uint8_t)(200 * it.f)));
        }
        float y = box.y + 12;
        float ax = box.x + PAD - 8;
        if (!c.pinned.empty()) {
            text::icon(ic::PUSH_PIN, 15, x0 + 7, y + 9, t.text3);
            text::draw_fit(font::caption, x0 + 20, y, tw - 20, c.pinned, t.text3);
            y += 24;
        }
        // Avatar
        const images::Image* img = c.avatar.empty() ? nullptr : images::get(c.avatar, 88, 88);
        Rect ar(ax, y, AVATAR, AVATAR);
        if (img && img->ready)
            gfx::image_cover(img->tex, img->w, img->h, ar, AVATAR * 0.5f, Color(255, 255, 255, (uint8_t)(255 * images::fade(img))));
        else
            gfx::fill_circle(ar.cx(), ar.cy(), AVATAR * 0.5f, t.surface_hi);

        // Author, badges, age. The video's creator gets a highlighted name, like on YouTube.
        float nx = x0;
        float nw = std::min(text::measure(font::small_bold, c.author), tw * 0.62f);
        if (c.creator) {
            gfx::fill_rrect(Rect(nx - 6, y - 1, nw + 12, 24), 12, Color(255, 255, 255, 40));
            nx += 6;
        }
        text::draw_fit(font::small_bold, nx - (c.creator ? 6 : 0), y + 1, nw, c.author, c.creator ? t.text : t.text2);
        nx += nw + (c.creator ? 12 : 6);
        if (c.verified) {
            text::icon(ic::VERIFIED, 15, nx + 7, y + 11, t.text3);
            nx += 20;
        }
        if (!c.published.empty()) text::draw_fit(font::small, nx, y + 1, x0 + tw - nx, util::fmt("· %s", c.published.c_str()), t.text3);
        y += 30;

        // Text
        float lh = text::line_height(TEXT_FONT) * 1.15f;
        const auto& lines = l.lines[i];
        bool more = lines.size() > (size_t)COLLAPSED_LINES && !l.expanded[i];
        size_t shown = more ? COLLAPSED_LINES : lines.size();
        for (size_t k = 0; k < shown; k++) {
            if (y + lh > 0 && y < H) text::draw(TEXT_FONT, x0, y, lines[k], t.text);
            y += lh;
        }
        if (more) {
            text::draw(font::small_bold, x0, y + 2, tr("Read more"), t.text2);
            y += 24;
        }

        // Likes, the creator's heart, replies
        y += 6;
        float fx = x0;
        text::icon(ic::THUMB_UP, 17, fx + 8, y + 12, t.text3);
        fx += 22;
        if (!c.likes.empty()) {
            text::draw(font::caption, fx, y + 4, c.likes, t.text3);
            fx += text::measure(font::caption, c.likes) + 18;
        } else {
            fx += 8;
        }
        if (c.hearted) {
            text::icon(ic::FAVORITE, 17, fx + 8, y + 12, gfx::rgb(0xFF4E6A));
            fx += 30;
        }
        if (!c.replies.empty() && !c.replies_token.empty() && !c.reply) {
            std::string r = util::fmt(tr("%s replies"), c.replies.c_str());
            if (c.replies == "1") r = tr("1 reply");
            text::icon(ic::SUBDIRECTORY_ARROW_RIGHT, 17, fx + 8, y + 12, t.accent);
            text::draw(font::caption, fx + 20, y + 4, r, t.accent);
        }
        return it.clicked;
    }

    void frame(const Rect& r, float alpha) {
        const Theme& t = theme();
        if (alpha <= 0.01f) return;
        gfx::push_alpha(alpha);
        gfx::fill_rect_hgrad(Rect(r.x - 60, r.y, 60, r.h), Color(10, 8, 14, 0), Color(10, 8, 14, 200));
        gfx::fill_rect(r, Color(16, 13, 21, 246));

        List& l = list();
        Id g = id("yt_comments");
        float y = r.y + 32;
        float hx = r.x + PAD;
        if (in_replies) {
            if (icon_button(id(g, "back"), hx + 20, y + 16, 20, ic::ARROW_BACK, id(g, "head"))) back();
            text::draw(font::title, hx + 52, y, tr("Replies"), t.text);
        } else {
            text::draw(font::title, hx, y, tr("Comments"), t.text);
            if (!main.count.empty()) {
                std::string n = count_number(main.count);
                float nx = hx + text::measure(font::title, tr("Comments")) + 12;
                text::draw(font::body, nx, y + 5, n, t.text2);
            }
        }
        y += 50;
        if (!in_replies) {
            // Sort, like the "Sort by" menu
            Id sg = id(g, "sort");
            const char* top_l = tr("Top");
            const char* new_l = tr("Newest");
            float w1 = text::measure(font::small_bold, top_l) + 40, w2 = text::measure(font::small_bold, new_l) + 40;
            if (chip(id(sg, "top"), Rect(hx, y, w1, 38), top_l, !newest, sg) && newest) {
                newest = false;
                reload();
            }
            if (chip(id(sg, "new"), Rect(hx + w1 + 10, y, w2, 38), new_l, newest, sg) && !newest) {
                newest = true;
                reload();
            }
            y += 54;
        }

        // The list
        float top = y, view_h = r.b() - top;
        float x0 = r.x + PAD + AVATAR + GAP - 8, tw = r.r() - PAD - x0;
        List* lp = &l;
        layout(l, tw);

        // Heights and the focused comment's range, before scrolling.
        Id group = id(g, in_replies ? "replies" : "list");
        Id sid = id(g, in_replies ? "rscroll" : "scroll");
        std::vector<float> ys(l.items.size() + 1, 0);
        float cy = 0, fa = 0, fb = 0;
        bool has_focus = false;
        Id fid = focused();
        for (size_t i = 0; i < l.items.size(); i++) {
            ys[i] = cy;
            float h = item_height(l, i);
            if (id(group, (int64_t)i) == fid) {
                fa = cy;
                fb = cy + h;
                has_focus = true;
            }
            cy += h + 8;
        }
        ys[l.items.size()] = cy;
        float content = cy + 90;
        scroll_drag(sid, Rect(r.x, top, r.w, view_h), true, content, view_h);
        float scroll = scroll_follow(sid, fa, fb, view_h, content, 40, has_focus);

        gfx::push_clip(Rect(r.x, top - 6, r.w, view_h + 6));
        int focus_index = -1;
        for (size_t i = 0; i < l.items.size(); i++) {
            float iy = top + ys[i] - scroll;
            float h = ys[i + 1] - ys[i] - 8;
            Id iid = id(group, (int64_t)i);
            Rect box(r.x + 8, iy, r.w - 16, h);
            bool visible = iy < r.b() + 20 && iy + h > top - 20;
            bool clicked;
            if (visible) {
                float in = indent(l, i);
                clicked = draw_comment(l, i, iid, group, Rect(box.x + in, box.y, box.w - in, box.h), x0 + in, tw - in);
            } else {
                clicked = focusable(iid, box, group, i == 0 ? F_DEFAULT : 0).clicked;
            }
            if (focused() == iid) focus_index = (int)i;
            if (clicked) {
                // A: the whole text first, then the replies.
                bool truncated = lp->lines[i].size() > (size_t)COLLAPSED_LINES && !lp->expanded[i];
                if (truncated) {
                    lp->expanded[i] = true;
                } else if (!in_replies && !lp->items[i].replies_token.empty() && !lp->items[i].replies.empty()) {
                    open_replies(lp->items[i]);
                    break;
                }
            }
        }
        float end_y = top + cy - scroll;
        if (l.loading) {
            spinner(r.cx(), l.items.empty() ? top + 80 : end_y + 30, 18, t.accent, 3);
        } else if (!l.error.empty() && l.items.empty()) {
            text::draw_wrapped(font::small, Rect(r.x + PAD, top + 20, r.w - PAD * 2, 80), l.error, t.bad, 3);
            if (button(id(g, "retry"), Rect(r.x + PAD, top + 90, 150, 44), tr("Retry"), ic::REFRESH, BTN_NORMAL, group, F_DEFAULT)) {
                l.error.clear();
                load(l, in_replies ? parent.replies_token : "");
            }
        } else if (l.loaded && l.items.empty()) {
            const char* msg = l.off ? tr("Comments are turned off for this video") : tr("No comments yet");
            text::icon(ic::FORUM, 40, r.cx(), top + 70, t.text3);
            text::draw_wrapped(font::body, Rect(r.x + PAD, top + 104, r.w - PAD * 2, 60), msg, t.text2, 2, text::CENTER);
        }
        gfx::pop_clip();

        // The next page when the focus or the scroll gets near the end.
        bool near_end = (focus_index >= 0 && focus_index + 3 >= (int)l.items.size()) || scroll + view_h > content - 500;
        if (near_end && !l.loading && !l.continuation.empty() && l.loaded) {
            std::string c = l.continuation;
            l.continuation.clear();
            load(l, c);
        }

        // Into the list once there's something in it (until then the sort chips have it).
        if (want_focus && !l.items.empty()) {
            want_focus = false;
            set_focus(id(group, (int64_t)0));
        } else if (want_focus && l.loaded) {
            want_focus = false;
        }
        gfx::pop_alpha();
    }
};

CommentsPanel::CommentsPanel() : p_(new Impl) {}
CommentsPanel::~CommentsPanel() = default;

void CommentsPanel::show(const std::string& video_id) {
    Impl& p = *p_;
    if (video_id != p.video_id) {
        p.video_id = video_id;
        p.newest = false;
        p.reload();
    } else if (p.main.error.size() && p.main.items.empty()) {
        p.reload();
    }
    p.in_replies = false;
    p.want_focus = true;
}

void CommentsPanel::frame(const Rect& r, float alpha) { p_->frame(r, alpha); }

bool CommentsPanel::back() { return p_->back(); }

}  // namespace yt
}  // namespace screens
