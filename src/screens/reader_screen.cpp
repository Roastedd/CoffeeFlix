// Document reader: comic books (CBZ) and, when built with MuPDF, PDF and EPUB.
//
// Pages are decoded one at a time on the image worker pool (so a document is
// never used by two threads at once) and uploaded as textures on the main
// thread. The current page and its neighbours stay resident.
#include <SDL2/SDL.h>
#include <zip.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <vector>

#ifdef HAVE_MUPDF
#include <mupdf/fitz.h>
#endif

#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/images.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"

namespace screens {

namespace {

using namespace ui;

struct SurfaceFree {
    void operator()(SDL_Surface* s) const { SDL_FreeSurface(s); }
};
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceFree>;

enum Fit { FIT_PAGE, FIT_WIDTH };

// Pages are rendered to fit this box: the screen in output pixels plus headroom
// for zooming (only the width counts in fit width), capped for texture memory.
struct Box {
    int w = 0, h = 0;
    bool operator==(const Box& o) const { return w == o.w && h == o.h; }
};

Box page_box(Fit fit) {
    int limit = platform::is_wiiu() ? 1600 : 2400;  // texture memory is tight on the console
    float s = gfx::output_scale() * 1.5f;
    return Box{std::min(limit, (int)(W * s)), fit == FIT_WIDTH ? limit : std::min(limit, (int)(H * s))};
}

// A paged document. Only one thread uses it at a time.
class Book {
public:
    virtual ~Book() = default;
    // Returns a user-facing error, or "" on success.
    virtual std::string open(const std::string& path) = 0;
    virtual int page_count() const = 0;
    // RGBA page that fits max_w x max_h; nullptr on failure.
    virtual SDL_Surface* render(int index, int max_w, int max_h) = 0;
    // Asks a render in progress to stop early (any thread).
    virtual void abort() {}
};

// Comic book archive: a zip of images in natural file name order.
class CbzBook : public Book {
public:
    ~CbzBook() override {
        if (zip_) zip_discard(zip_);
    }

    std::string open(const std::string& path) override {
        int err = 0;
        zip_ = zip_open(path.c_str(), ZIP_RDONLY, &err);
        if (!zip_) return "The file may be damaged or isn't a comic book archive.";
        std::vector<std::pair<std::string, zip_uint64_t>> images;
        zip_int64_t n = zip_get_num_entries(zip_, 0);
        for (zip_int64_t i = 0; i < n; i++) {
            const char* name = zip_get_name(zip_, (zip_uint64_t)i, ZIP_FL_ENC_GUESS);
            if (!name || util::starts_with(name, "__MACOSX/") || util::file_name(name)[0] == '.') continue;
            std::string ext = util::file_extension(name);
            if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "webp" || ext == "gif")
                images.emplace_back(name, (zip_uint64_t)i);
        }
        std::sort(images.begin(), images.end(), [](const auto& a, const auto& b) { return util::natural_less(a.first, b.first); });
        for (auto& img : images) entries_.push_back(img.second);
        return entries_.empty() ? "There are no pages in this comic book." : "";
    }

    int page_count() const override { return (int)entries_.size(); }

    SDL_Surface* render(int index, int max_w, int max_h) override {
        zip_uint64_t entry = entries_[index];
        zip_stat_t st;
        if (zip_stat_index(zip_, entry, 0, &st) != 0 || !(st.valid & ZIP_STAT_SIZE) || st.size > (64u << 20)) return nullptr;
        std::string data(st.size, '\0');
        zip_file_t* f = zip_fopen_index(zip_, entry, 0);
        if (!f) return nullptr;
        zip_int64_t got = zip_fread(f, data.data(), st.size);
        zip_fclose(f);
        if (got != (zip_int64_t)st.size) return nullptr;
        return images::decode(data, max_w, max_h);
    }

private:
    zip_t* zip_ = nullptr;
    std::vector<zip_uint64_t> entries_;
};

#ifdef HAVE_MUPDF
// PDF and EPUB through MuPDF. Reflowable books are laid out as portrait pages
// with type large enough to read from the couch.
class MupdfBook : public Book {
public:
    ~MupdfBook() override {
        if (!ctx_) return;
        fz_drop_document(ctx_, doc_);
        fz_drop_context(ctx_);
    }

    std::string open(const std::string& path) override {
        ctx_ = fz_new_context(nullptr, nullptr, 32u << 20);
        if (!ctx_) return "Not enough memory to open this document.";
        fz_set_error_callback(ctx_, [](void*, const char* msg) { log_message(LOG_ERROR, "MuPDF", "%s", msg); }, nullptr);
        fz_set_warning_callback(ctx_, [](void*, const char* msg) { log_message(LOG_WARNING, "MuPDF", "%s", msg); }, nullptr);
        const char* error = nullptr;
        fz_var(error);
        fz_try(ctx_) {
            fz_register_document_handlers(ctx_);
            doc_ = fz_open_document(ctx_, path.c_str());
            if (fz_needs_password(ctx_, doc_)) {
                error = "This document is password protected.";
            } else {
                if (fz_is_document_reflowable(ctx_, doc_)) fz_layout_document(ctx_, doc_, 400, 540, 15);
                count_ = fz_count_pages(ctx_, doc_);
            }
        }
        fz_catch(ctx_) {
            log_message(LOG_ERROR, "Reader", "Can't open %s", path.c_str());
            fz_report_error(ctx_);
            error = "The file may be damaged or in an unsupported format.";
        }
        if (!error && count_ <= 0) error = "This document has no pages.";
        return error ? error : "";
    }

    int page_count() const override { return count_; }

    // Draws straight into the SDL surface's pixels (MuPDF's RGBA layout matches RGBA32).
    SDL_Surface* render(int index, int max_w, int max_h) override {
        SDL_Surface* out = nullptr;
        fz_page* page = nullptr;
        fz_pixmap* pix = nullptr;
        fz_device* dev = nullptr;
        fz_var(out);
        fz_var(page);
        fz_var(pix);
        fz_var(dev);
        fz_try(ctx_) {
            page = fz_load_page(ctx_, doc_, index);
            fz_rect bounds = fz_bound_page(ctx_, page);
            float pw = std::max(1.0f, bounds.x1 - bounds.x0), ph = std::max(1.0f, bounds.y1 - bounds.y0);
            float s = std::min(max_w / pw, max_h / ph);
            fz_matrix ctm = fz_scale(s, s);
            fz_irect box = fz_round_rect(fz_transform_rect(bounds, ctm));
            int w = box.x1 - box.x0, h = box.y1 - box.y0;
            out = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
            if (!out || out->pitch != w * 4) fz_throw(ctx_, FZ_ERROR_SYSTEM, "can't allocate a %dx%d page", w, h);
            pix = fz_new_pixmap_with_bbox_and_data(ctx_, fz_device_rgb(ctx_), box, nullptr, 1, (unsigned char*)out->pixels);
            fz_clear_pixmap_with_value(ctx_, pix, 0xff);
            dev = fz_new_draw_device(ctx_, fz_identity, pix);
            fz_run_page(ctx_, page, dev, ctm, &cookie_);
            fz_close_device(ctx_, dev);
            if (cookie_.abort) fz_throw(ctx_, FZ_ERROR_ABORT, "aborted");
        }
        fz_always(ctx_) {
            fz_drop_device(ctx_, dev);
            fz_drop_pixmap(ctx_, pix);
            fz_drop_page(ctx_, page);
        }
        fz_catch(ctx_) {
            if (fz_caught(ctx_) == FZ_ERROR_ABORT) fz_ignore_error(ctx_);
            else fz_report_error(ctx_);
            SDL_FreeSurface(out);
            out = nullptr;
        }
        return out;
    }

    void abort() override { cookie_.abort = 1; }

private:
    fz_context* ctx_ = nullptr;
    fz_document* doc_ = nullptr;
    fz_cookie cookie_ = {};
    int count_ = 0;
};
#endif

std::unique_ptr<Book> make_book(const std::string& ext) {
    if (ext == "cbz") return std::make_unique<CbzBook>();
#ifdef HAVE_MUPDF
    return std::make_unique<MupdfBook>();
#else
    return nullptr;
#endif
}

struct Opened {
    std::shared_ptr<Book> book;
    std::string title, desc;  // error
};

Opened open_book(const std::string& path) {
    Opened o;
    std::string ext = util::file_extension(path);
    std::unique_ptr<Book> book = make_book(ext);
    if (!book) {
        o.title = ext == "epub" ? "EPUB support isn't available in this build" : "PDF support isn't available in this build";
        o.desc = "This build of CoffeeFlix can only open comic books (CBZ).";
        return o;
    }
    o.desc = book->open(path);
    if (o.desc.empty()) o.book = std::move(book);
    else o.title = "Can't open this book";
    return o;
}

struct Rendered {
    int page = -1;
    Box box;
    SurfacePtr surface;
};

struct PageTexture {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    Box box;  // what it was rendered for
    double ready_at = 0;
};

class Reader : public app::Screen {
public:
    explicit Reader(std::string path)
        : path_(std::move(path)), key_(util::fmt("reader_page:%016llx", (unsigned long long)util::hash64(path_))) {
        std::string name = util::file_name(path_);
        title_ = name.substr(0, name.find_last_of('.'));
        scope_.run<Opened>([path = path_] { return open_book(path); }, [this](Opened o) { opened(std::move(o)); },
                           tasks::IMAGES);
    }

    ~Reader() override {
        if (book_) book_->abort();
        for (auto& [p, pt] : pages_) SDL_DestroyTexture(pt.tex);
    }

    bool fullscreen() const override { return true; }
    bool draws_background() const override { return true; }

    bool on_back() override {
        if (zoom_ > 1.01f) {
            set_zoom(1);
            return true;
        }
        return false;
    }

    void frame() override {
        suspend_nav();
        gfx::fill_rect(Rect(0, 0, W, H), Color(12, 11, 14, 255));
        if (!book_) {
            if (error_title_.empty()) loading_indicator(W * 0.5f, H * 0.5f, title_.c_str());
            else empty_state(Rect(W * 0.5f - 340, H * 0.5f - 170, 680, 340), ic::ERROR_OUTLINE, error_title_.c_str(), error_desc_.c_str());
            hint_bar({{"B", "Back"}});
            return;
        }
        handle_input();
        request_pages();
        evict_pages();
        draw_pages();
        draw_overlay();
    }

private:
    void opened(Opened o) {
        if (!o.book) {
            error_title_ = o.title;
            error_desc_ = o.desc;
            return;
        }
        book_ = std::move(o.book);
        count_ = book_->page_count();
        page_ = std::clamp((int)store::get_int(key_.c_str(), 0), 0, count_ - 1);
        turned_at_ = shown_at_ = ui::time();
        pan_y_ = 1e6f;  // top of the page
        snap_ = true;
        if (page_ > 0) toast(util::fmt("Resuming on page %d", page_ + 1), ic::HISTORY);
    }

    void handle_input() {
        Input& in = input();
        if (in.any()) shown_at_ = ui::time();
        bool zoomed = zoom_ > 1.01f;

        int d = 0;
        if (in.rep(BTN_R)) d = 1;
        if (in.rep(BTN_L)) d = -1;
        if (!zoomed) {
            if (in.rep(BTN_RIGHT)) d = 1;
            if (in.rep(BTN_LEFT)) d = -1;
            float dx = in.tx - in.touch_start_x, dy = in.ty - in.touch_start_y;
            if (in.touch_ended && in.dragging && std::fabs(dx) > 80 && std::fabs(dx) > std::fabs(dy)) d = dx < 0 ? 1 : -1;
        }
        if (d) {
            turn(d);
            return;
        }

        if (in.pressed_(BTN_A) || in.pressed_(BTN_ZR)) set_zoom(zoom_ * 1.6f);
        if (in.pressed_(BTN_ZL)) set_zoom(zoom_ / 1.6f);
        if (in.pressed_(BTN_Y)) {
            fit_ = fit_ == FIT_PAGE ? FIT_WIDTH : FIT_PAGE;
            zoom_ = 1;
            pan_x_ = 0;
            pan_y_ = 1e6f;
            toast(fit_ == FIT_WIDTH ? "Fit width" : "Fit page", ic::FULLSCREEN);
        }

        // Pan when zoomed; scroll a page taller than the screen (fit width) at any zoom.
        float vx = (in.down(BTN_LEFT) ? 1.0f : 0.0f) - (in.down(BTN_RIGHT) ? 1.0f : 0.0f);
        float vy = (in.down(BTN_UP) ? 1.0f : 0.0f) - (in.down(BTN_DOWN) ? 1.0f : 0.0f);
        // The stick also drives the D-pad bits; its analog value wins so it isn't counted twice.
        if (std::fabs(in.lx) > 0.2f) vx = -in.lx * 1.4f;
        if (std::fabs(in.ly) > 0.2f) vy = in.ly * 1.4f;
        float sp = 900 * ui::dt();
        if (zoomed) pan_x_ += vx * sp;
        pan_y_ += vy * sp;
        if (in.touching && in.dragging) {
            if (zoomed) pan_x_ += in.tdx;
            pan_y_ += in.tdy;
        }
    }

    void turn(int d) {
        int next = std::clamp(page_ + d, 0, count_ - 1);
        if (next == page_) {
            toast(d > 0 ? "Last page" : "First page", ic::BOOK);
            return;
        }
        out_page_ = page_;
        out_w_ = shown_w_;
        out_x_ = shown_x_;
        out_y_ = shown_y_;
        page_ = next;
        dir_ = d;
        turned_at_ = ui::time();
        zoom_ = 1;
        pan_x_ = 0;
        pan_y_ = d > 0 ? 1e6f : -1e6f;  // fit width: top of the next page, bottom of the previous
        snap_ = true;
        store::set_int(key_.c_str(), page_);
    }

    void set_zoom(float z) {
        z = std::clamp(z, 1.0f, 6.0f);
        // Keep the middle of the screen where it is.
        pan_x_ *= z / zoom_;
        pan_y_ *= z / zoom_;
        zoom_ = z;
    }

    // One render at a time: the current page first, then its neighbours. A page
    // rendered for another fit mode is shown scaled until its replacement is ready.
    void request_pages() {
        if (rendering_) return;
        Box box = page_box(fit_);
        for (int p : {page_, page_ + 1, page_ - 1}) {
            const PageTexture* pt = texture(p);
            if (p < 0 || p >= count_ || (pt && pt->box == box) || failed_.count(p)) continue;
            std::shared_ptr<Book> book = book_;
            rendering_ = true;
            scope_.run<Rendered>([book, p, box] { return Rendered{p, box, SurfacePtr(book->render(p, box.w, box.h))}; },
                                 [this](Rendered r) { rendered(std::move(r)); }, tasks::IMAGES);
            return;
        }
    }

    void rendered(Rendered r) {
        rendering_ = false;
        if (!r.surface) {
            if (!texture(r.page)) failed_.insert(r.page);
            return;
        }
        if (std::abs(r.page - page_) > 1) return;  // turned away in the meantime
        SDL_Surface* s = r.surface.get();
        SDL_Texture* tex = SDL_CreateTexture(gfx::renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, s->w, s->h);
        if (!tex) {
            if (!texture(r.page)) failed_.insert(r.page);
            return;
        }
        SDL_UpdateTexture(tex, nullptr, s->pixels, s->pitch);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
        PageTexture& pt = pages_[r.page];
        // A sharper replacement doesn't fade in again.
        double ready_at = pt.tex ? 0 : ui::time();
        if (pt.tex) SDL_DestroyTexture(pt.tex);
        pt = PageTexture{tex, s->w, s->h, r.box, ready_at};
    }

    void evict_pages() {
        bool turning = ui::time() - turned_at_ < 0.35;
        for (auto it = pages_.begin(); it != pages_.end();) {
            int p = it->first;
            if (std::abs(p - page_) <= 1 || (turning && p == out_page_)) {
                ++it;
                continue;
            }
            SDL_DestroyTexture(it->second.tex);
            it = pages_.erase(it);
        }
    }

    const PageTexture* texture(int p) const {
        auto it = pages_.find(p);
        return it == pages_.end() ? nullptr : &it->second;
    }

    // On-screen width at zoom 1 (animations work in screen pixels, so a sharper
    // texture replacing a blurry one doesn't move anything).
    float fitted_width(const PageTexture& pt) const {
        return fit_ == FIT_WIDTH ? W : std::min(W, H * pt.w / pt.h);
    }

    void draw_pages() {
        float t = (float)(ui::time() - turned_at_) / 0.35f;
        float e = anim::ease_out_cubic(t);
        bool turning = t < 1 && out_page_ >= 0;
        const float slide = 120;
        if (turning) draw_page(out_page_, out_w_, out_x_ - dir_ * slide * e, out_y_, 1 - e);

        const PageTexture* cur = texture(page_);
        if (!cur) {
            if (failed_.count(page_))
                empty_state(Rect(W * 0.5f - 300, H * 0.5f - 150, 600, 300), ic::ERROR_OUTLINE, "Can't show this page", "");
            else if (ui::time() - turned_at_ > 0.2)
                spinner(W * 0.5f, H * 0.5f, 28, theme().accent, 4);
            return;
        }

        // Keep the page on screen: pan only as far as it overflows.
        float tw = fitted_width(*cur) * zoom_, th = tw * cur->h / cur->w;
        float mx = std::max(0.0f, (tw - W) * 0.5f), my = std::max(0.0f, (th - H) * 0.5f);
        pan_x_ = std::clamp(pan_x_, -mx, mx);
        pan_y_ = std::clamp(pan_y_, -my, my);
        if (snap_) {
            set_value(id("rd_w"), tw);
            set_value(id("rd_px"), pan_x_);
            set_value(id("rd_py"), pan_y_);
            snap_ = false;
        }
        shown_w_ = spring(id("rd_w"), tw, 200, 24);
        shown_x_ = spring(id("rd_px"), pan_x_, 200, 26);
        shown_y_ = spring(id("rd_py"), pan_y_, 200, 26);
        float fade = std::clamp((float)(ui::time() - cur->ready_at) / 0.25f, 0.0f, 1.0f);
        draw_page(page_, shown_w_, shown_x_ + (turning ? dir_ * slide * (1 - e) : 0), shown_y_, (turning ? e : 1) * fade);
    }

    void draw_page(int p, float w, float x, float y, float alpha) {
        const PageTexture* pt = texture(p);
        if (!pt || alpha <= 0.01f) return;
        float h = w * pt->h / pt->w;
        Rect r(W * 0.5f - w * 0.5f + x, H * 0.5f - h * 0.5f + y, w, h);
        gfx::shadow(r, 28, Color(0, 0, 0, (uint8_t)(150 * alpha)));
        gfx::image(pt->tex, r, Color(255, 255, 255, (uint8_t)(255 * alpha)));
    }

    void draw_overlay() {
        const Theme& t = theme();
        float a = tween(id("rd_info"), ui::time() - shown_at_ < 2.5 ? 1.0f : 0.0f, 8);
        if (a <= 0.01f) return;
        gfx::push_alpha(a);
        gfx::fill_rect_vgrad(Rect(0, H - 150, W, 150), Color(0, 0, 0, 0), Color(0, 0, 0, 210));
        text::draw_fit(font::title, 60, H - 116, W - 120, title_, t.text);
        text::draw(font::small_bold, 60, H - 76, util::fmt("%d / %d", page_ + 1, count_), t.text2);
        progress_bar(Rect(60, H - 34, W - 120, 4), (float)(page_ + 1) / count_);
        if (zoom_ > 1.01f)
            hint_bar({{"L/R", "Page"}, {"A", "Zoom in"}, {"ZL", "Zoom out"}, {"B", "Reset zoom"}}, H - 66);
        else
            hint_bar({{"L/R", "Page"}, {"A", "Zoom"}, {"Y", fit_ == FIT_PAGE ? "Fit width" : "Fit page"}, {"B", "Back"}}, H - 66);
        gfx::pop_alpha();
    }

    std::string path_, key_, title_;
    std::string error_title_, error_desc_;
    std::shared_ptr<Book> book_;
    int count_ = 0, page_ = 0;
    std::map<int, PageTexture> pages_;
    std::set<int> failed_;
    bool rendering_ = false;

    Fit fit_ = FIT_PAGE;
    float zoom_ = 1, pan_x_ = 0, pan_y_ = 0;  // targets; pan in screen pixels from the centre
    float shown_w_ = 0, shown_x_ = 0, shown_y_ = 0;
    bool snap_ = false;  // jump straight to the targets once the page is ready

    // Page turn: the outgoing page slides away with the pose it had.
    int out_page_ = -1, dir_ = 1;
    float out_w_ = 0, out_x_ = 0, out_y_ = 0;
    double turned_at_ = -10, shown_at_ = 0;

    tasks::Scope scope_;
};

}  // namespace

std::unique_ptr<app::Screen> make_reader(const std::string& path) { return std::make_unique<Reader>(path); }

}  // namespace screens
