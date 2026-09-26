// Fullscreen photo viewer with zoom/pan, crossfades and a Ken Burns slideshow.
#include <cmath>

#include "core/i18n.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

class PhotoViewer : public app::Screen {
public:
    PhotoViewer(std::vector<std::string> paths, int index) : paths_(std::move(paths)), index_(index) {}

    bool fullscreen() const override { return true; }
    bool draws_background() const override { return true; }

    bool on_back() override {
        if (zoom_ > 1.01f) {
            zoom_ = 1;
            pan_x_ = pan_y_ = 0;
            return true;
        }
        return false;
    }

    void frame() override {
        Input& in = input();
        const Theme& t = theme();
        suspend_nav();
        gfx::fill_rect(Rect(0, 0, W, H), gfx::BLACK);
        if (paths_.empty()) return;

        // Input
        if (in.any()) shown_at_ = ui::time();
        if (zoom_ <= 1.01f) {
            if (in.rep(BTN_RIGHT) || in.rep(BTN_R)) go(1);
            if (in.rep(BTN_LEFT) || in.rep(BTN_L)) go(-1);
            if (in.touch_ended && in.dragging && std::fabs(in.tx - in.touch_start_x) > 80) go(in.tx < in.touch_start_x ? 1 : -1);
        } else {
            float sp = 900 * ui::dt() / zoom_;
            if (in.down(BTN_RIGHT)) pan_x_ -= sp;
            if (in.down(BTN_LEFT)) pan_x_ += sp;
            if (in.down(BTN_DOWN)) pan_y_ -= sp;
            if (in.down(BTN_UP)) pan_y_ += sp;
            pan_x_ -= in.lx * sp * 1.4f;
            pan_y_ += in.ly * sp * 1.4f;
            if (in.touching && in.dragging) {
                pan_x_ += in.tdx / zoom_;
                pan_y_ += in.tdy / zoom_;
            }
        }
        if (in.pressed_(BTN_ZR) || in.pressed_(BTN_A)) zoom_ = std::min(zoom_ * 1.6f, 6.0f);
        if (in.pressed_(BTN_ZL)) zoom_ = std::max(zoom_ / 1.6f, 1.0f);
        if (in.pressed_(BTN_Y)) {
            slideshow_ = !slideshow_;
            slide_start_ = ui::time();
            toast(slideshow_ ? tr("Slideshow on") : tr("Slideshow off"), ic::PLAYLIST_PLAY);
        }
        if (slideshow_ && ui::time() - slide_start_ > 6.0) go(1);
        if (zoom_ <= 1.0f) pan_x_ = pan_y_ = 0;
        float z = spring(id("pv_zoom"), zoom_, 200, 24);
        float px = spring(id("pv_px"), pan_x_, 200, 26), py = spring(id("pv_py"), pan_y_, 200, 26);

        // Previous image fades out underneath the new one.
        float ft = anim::smoothstep((float)(ui::time() - changed_at_) / 0.45f);
        if (ft < 1 && prev_index_ >= 0) draw_image(prev_index_, 1.0f, 0, 0, 1.0f - ft, false);
        draw_image(index_, z, px, py, ft, true);

        // Info overlay
        float a = tween(id("pv_info"), ui::time() - shown_at_ < 2.5 ? 1.0f : 0.0f, 8);
        if (a > 0.01f) {
            gfx::push_alpha(a);
            gfx::fill_rect_vgrad(Rect(0, H - 130, W, 130), Color(0, 0, 0, 0), Color(0, 0, 0, 200));
            text::draw_fit(font::title, 60, H - 84, W - 500, util::file_name(paths_[index_]), t.text);
            text::draw(font::small_bold, 60, H - 46, util::fmt(tr("%d of %zu"), index_ + 1, paths_.size()), t.text2);
            hint_bar({{"A", tr("Zoom")}, {"Y", slideshow_ ? tr("Stop") : tr("Slideshow")}, {"B", tr("Back")}}, H - 60);
            gfx::pop_alpha();
        }
    }

private:
    void go(int d) {
        int n = (int)paths_.size();
        if (n <= 1) return;
        prev_index_ = index_;
        index_ = (index_ + d + n) % n;
        changed_at_ = ui::time();
        slide_start_ = ui::time();
        zoom_ = 1;
        pan_x_ = pan_y_ = 0;
        set_value(id("pv_zoom"), 1);
    }

    void draw_image(int idx, float z, float px, float py, float alpha, bool current) {
        const images::Image* img = images::get(paths_[idx], 1920, 1080);
        // Preload neighbours so paging is instant.
        if (current && paths_.size() > 1) {
            images::get(paths_[(idx + 1) % paths_.size()], 1920, 1080);
            images::get(paths_[(idx + paths_.size() - 1) % paths_.size()], 1920, 1080);
        }
        if (!img || !img->ready) {
            if (current && img && !img->failed) spinner(W * 0.5f, H * 0.5f, 28, theme().accent, 4);
            if (current && img && img->failed) empty_state(Rect(W * 0.5f - 300, H * 0.5f - 150, 600, 300), ic::ERROR_OUTLINE, tr("Can't open this image"), "");
            return;
        }
        float s = std::min(W / img->w, H / img->h);
        float kb = 1.0f;
        float kx = 0, ky = 0;
        if (slideshow_ && current) {
            // Ken Burns: slow zoom and drift.
            float p = (float)(ui::time() - slide_start_) / 6.0f;
            kb = 1.0f + 0.08f * p;
            kx = (idx % 2 ? 1 : -1) * 30 * p;
            ky = (idx % 3 ? -1 : 1) * 16 * p;
        }
        float w = img->w * s * z * kb, h = img->h * s * z * kb;
        Rect r(W * 0.5f - w * 0.5f + px * z + kx, H * 0.5f - h * 0.5f + py * z + ky, w, h);
        gfx::image(img->tex, r, Color(255, 255, 255, (uint8_t)(255 * alpha * images::fade(img, 0.3f))));
    }

    std::vector<std::string> paths_;
    int index_ = 0, prev_index_ = -1;
    double changed_at_ = -10, shown_at_ = 0, slide_start_ = 0;
    float zoom_ = 1, pan_x_ = 0, pan_y_ = 0;
    bool slideshow_ = false;
};

}  // namespace

std::unique_ptr<app::Screen> make_photo_viewer(std::vector<std::string> paths, int index) {
    return std::make_unique<PhotoViewer>(std::move(paths), index);
}

}  // namespace screens
