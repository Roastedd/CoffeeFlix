#include "gfx/images.hpp"

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/http.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "logger/logger.hpp"
#include "player/thumbnailer.hpp"
#include "services/smb.hpp"

namespace images {

namespace {

enum State { QUEUED, LOADING, DECODED, READY, FAILED };

struct Entry {
    std::string url;
    int max_w = 0, max_h = 0, flags = 0;
    State state = QUEUED;
    SDL_Surface* pending = nullptr;  // decoded, waiting for upload (guarded by g_m)
    Image img;
    uint64_t last_used = 0;
    size_t bytes = 0;
    double failed_at = 0;
};

std::mutex g_m;
std::unordered_map<std::string, std::shared_ptr<Entry>> g_entries;
std::vector<std::shared_ptr<Entry>> g_ready_to_upload;
uint64_t g_frame = 0;
size_t g_budget = 96u << 20;
size_t g_used = 0;

SDL_Surface* to_rgba(SDL_Surface* s) {
    if (!s) return nullptr;
    if (s->format->format == SDL_PIXELFORMAT_RGBA32) return s;
    SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(s);
    return c;
}

SDL_Surface* scale_to(SDL_Surface* s, int w, int h) {
    if (!s || w <= 0 || h <= 0 || (w == s->w && h == s->h)) return s;
    SDL_Surface* d = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!d) return s;
    // Large reductions in several halving steps look much better than one
    // bilinear stretch (which only samples 4 texels per output pixel).
    SDL_Surface* cur = s;
    while (cur->w / 2 >= w * 2 && cur->h / 2 >= h * 2) {
        SDL_Surface* half = SDL_CreateRGBSurfaceWithFormat(0, cur->w / 2, cur->h / 2, 32, SDL_PIXELFORMAT_RGBA32);
        if (!half || SDL_SoftStretchLinear(cur, nullptr, half, nullptr) != 0) {
            if (half) SDL_FreeSurface(half);
            break;
        }
        if (cur != s) SDL_FreeSurface(cur);
        cur = half;
    }
    if (SDL_SoftStretchLinear(cur, nullptr, d, nullptr) != 0) {
        SDL_FreeSurface(d);
        d = nullptr;
    }
    if (cur != s) SDL_FreeSurface(cur);
    if (!d) return s;
    SDL_FreeSurface(s);
    return d;
}

void box_blur(SDL_Surface* s, int radius, int passes) {
    int w = s->w, h = s->h;
    std::vector<uint8_t> tmp((size_t)w * h * 4);
    uint8_t* px = (uint8_t*)s->pixels;
    for (int p = 0; p < passes; p++) {
        // horizontal
        for (int y = 0; y < h; y++) {
            uint8_t* row = px + (size_t)y * s->pitch;
            for (int x = 0; x < w; x++) {
                int acc[4] = {0, 0, 0, 0}, n = 0;
                for (int k = -radius; k <= radius; k++) {
                    int xx = std::clamp(x + k, 0, w - 1);
                    for (int c = 0; c < 4; c++) acc[c] += row[xx * 4 + c];
                    n++;
                }
                for (int c = 0; c < 4; c++) tmp[((size_t)y * w + x) * 4 + c] = (uint8_t)(acc[c] / n);
            }
        }
        // vertical
        for (int y = 0; y < h; y++) {
            uint8_t* row = px + (size_t)y * s->pitch;
            for (int x = 0; x < w; x++) {
                int acc[4] = {0, 0, 0, 0}, n = 0;
                for (int k = -radius; k <= radius; k++) {
                    int yy = std::clamp(y + k, 0, h - 1);
                    for (int c = 0; c < 4; c++) acc[c] += tmp[((size_t)yy * w + x) * 4 + c];
                    n++;
                }
                for (int c = 0; c < 4; c++) row[x * 4 + c] = (uint8_t)(acc[c] / n);
            }
        }
    }
}

constexpr int64_t MAX_PIXELS = 16 << 20;  // 4096 x 4096

uint32_t be16(const std::string& d, size_t at) { return ((uint32_t)(uint8_t)d[at] << 8) | (uint8_t)d[at + 1]; }
uint32_t be32(const std::string& d, size_t at) { return (be16(d, at) << 16) | be16(d, at + 2); }
uint32_t le16(const std::string& d, size_t at) { return (uint8_t)d[at] | ((uint32_t)(uint8_t)d[at + 1] << 8); }
uint32_t le24(const std::string& d, size_t at) { return le16(d, at) | ((uint32_t)(uint8_t)d[at + 2] << 16); }
uint32_t le32(const std::string& d, size_t at) { return le16(d, at) | (le16(d, at + 2) << 16); }

// (a header can state 4 billion either way: the product is only made of sizes that can't overflow it)
bool over_limit(int64_t w, int64_t h) { return w > MAX_PIXELS || h > MAX_PIXELS || w * h > MAX_PIXELS; }

}  // namespace

bool stated_size(const std::string& d, int64_t& w, int64_t& h) {
    const size_t n = d.size();
    if (n >= 24 && d.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0 && d.compare(12, 4, "IHDR") == 0) {
        w = be32(d, 16);
        h = be32(d, 20);
        return true;
    }
    if (n >= 10 && (d.compare(0, 6, "GIF87a") == 0 || d.compare(0, 6, "GIF89a") == 0)) {
        w = le16(d, 6);
        h = le16(d, 8);
        return true;
    }
    if (n >= 25 && d.compare(0, 4, "RIFF") == 0 && d.compare(8, 4, "WEBP") == 0) {
        if (n >= 30 && d.compare(12, 4, "VP8X") == 0) {  // extended: the canvas
            w = 1 + le24(d, 24);
            h = 1 + le24(d, 27);
            return true;
        }
        if (n >= 30 && d.compare(12, 4, "VP8 ") == 0 && d.compare(23, 3, "\x9d\x01\x2a", 3) == 0) {  // lossy: 14 bits each
            w = le16(d, 26) & 0x3fff;
            h = le16(d, 28) & 0x3fff;
            return true;
        }
        if (d.compare(12, 4, "VP8L") == 0 && (uint8_t)d[20] == 0x2f) {  // lossless: 14 bits each, less one
            const uint32_t bits = le32(d, 21);
            w = 1 + (bits & 0x3fff);
            h = 1 + ((bits >> 14) & 0x3fff);
            return true;
        }
        return false;
    }
    if (n >= 4 && (uint8_t)d[0] == 0xff && (uint8_t)d[1] == 0xd8) {
        // The segments up to the frame header. Those of the camera's own thumbnail are skipped by
        // their length, not read.
        size_t at = 2;
        while (at + 4 <= n) {
            if ((uint8_t)d[at] != 0xff) return false;
            const uint8_t marker = (uint8_t)d[at + 1];
            if (marker == 0xff) {  // padding
                at++;
            } else if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) {  // no length
                at += 2;
            } else if (marker == 0xd9 || marker == 0xda) {  // the end, or picture data without a frame header
                return false;
            } else if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
                if (at + 9 > n) return false;
                h = be16(d, at + 5);
                w = be16(d, at + 7);
                return true;
            } else {
                const size_t length = be16(d, at + 2);
                if (length < 2) return false;
                at += 2 + length;
            }
        }
    }
    return false;
}

bool too_big(const std::string& data) {
    int64_t w = 0, h = 0;
    return stated_size(data, w, h) && over_limit(w, h);
}

SDL_Surface* decode(const std::string& data, int max_w, int max_h, int flags) {
    int64_t stated_w = 0, stated_h = 0;
    if (stated_size(data, stated_w, stated_h) && over_limit(stated_w, stated_h)) {
        log_message(LOG_WARNING, "Images", "A picture of %lld x %lld pixels is too big to load", (long long)stated_w,
                    (long long)stated_h);
        return nullptr;
    }
    SDL_RWops* rw = SDL_RWFromConstMem(data.data(), (int)data.size());
    SDL_Surface* s = to_rgba(IMG_Load_RW(rw, 1));
    if (!s) return nullptr;
    if (flags & BLUR) {
        int w = 64, h = std::max(1, (int)std::lround(64.0 * s->h / s->w));
        s = scale_to(s, w, h);
        if (s && s->format->format == SDL_PIXELFORMAT_RGBA32) box_blur(s, 3, 2);
        return s;
    }
    if (max_w > 0 || max_h > 0) {
        float sx = max_w > 0 ? (float)max_w / s->w : 1e9f;
        float sy = max_h > 0 ? (float)max_h / s->h : 1e9f;
        float k = std::min(sx, sy);
        if (k < 1.0f) s = scale_to(s, std::max(1, (int)(s->w * k)), std::max(1, (int)(s->h * k)));
    }
    return s;
}

namespace {

void load_job(std::shared_ptr<Entry> e) {
    tasks::submit(tasks::IMAGES, [e]() -> std::function<void()> {
        {
            std::lock_guard<std::mutex> lk(g_m);
            // Scrolled away before we got to it: put it back in the queue state.
            if (g_frame > e->last_used + 20) {
                e->state = QUEUED;
                return nullptr;
            }
            e->state = LOADING;
        }
        std::string data;
        bool ok;
        if (util::starts_with(e->url, "thumb://")) {
            // Still frame from a local video file.
            SDL_Surface* s = player::video_thumbnail(e->url.substr(8), e->max_w > 0 ? e->max_w : 320);
            s = to_rgba(s);
            std::lock_guard<std::mutex> lk(g_m);
            if (s) {
                e->pending = s;
                e->state = DECODED;
                g_ready_to_upload.push_back(e);
            } else {
                e->state = FAILED;
                e->failed_at = util::now_seconds() + 1e9;  // don't retry
                e->img.failed = true;
            }
            return nullptr;
        }
        if (util::starts_with(e->url, "http://") || util::starts_with(e->url, "https://")) {
            http::Request req;
            req.url = e->url;
            req.timeout = 20;
            req.max_bytes = 12u << 20;
            http::Response r = http::perform(req);
            ok = r.ok();
            data = std::move(r.body);
        } else if (smb::is_url(e->url)) {
            ok = smb::read_file(e->url, data);
        } else {
            ok = util::read_file(util::starts_with(e->url, "file://") ? e->url.substr(7) : e->url, data);
        }
        SDL_Surface* s = ok ? decode(data, e->max_w, e->max_h, e->flags) : nullptr;
        const bool hopeless = ok && !s && too_big(data);  // asking again gets the same picture
        std::lock_guard<std::mutex> lk(g_m);
        if (s) {
            e->pending = s;
            e->state = DECODED;
            g_ready_to_upload.push_back(e);
        } else {
            e->state = FAILED;
            e->failed_at = util::now_seconds() + (hopeless ? 1e9 : 0);  // 1e9: never asked for again
            e->img.failed = true;
        }
        return nullptr;
    });
}

void upload(Entry& e) {
    SDL_Surface* s = e.pending;
    e.pending = nullptr;
    if (!s) return;
    SDL_Texture* t = SDL_CreateTexture(gfx::renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, s->w, s->h);
    if (t) {
        SDL_UpdateTexture(t, nullptr, s->pixels, s->pitch);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
        e.img.tex = t;
        e.img.w = s->w;
        e.img.h = s->h;
        e.img.ready = true;
        e.img.ready_time = util::now_seconds();
        e.bytes = (size_t)s->w * s->h * 4;
        g_used += e.bytes;
        e.state = READY;
    } else {
        e.state = FAILED;
        e.img.failed = true;
        e.failed_at = util::now_seconds();
    }
    SDL_FreeSurface(s);
}

void evict_if_needed() {
    if (g_used <= g_budget) return;
    std::vector<std::shared_ptr<Entry>> ready;
    for (auto& [k, e] : g_entries)
        if (e->state == READY && !(e->flags & NO_EVICT) && e->last_used + 2 < g_frame) ready.push_back(e);
    std::sort(ready.begin(), ready.end(), [](auto& a, auto& b) { return a->last_used < b->last_used; });
    for (auto& e : ready) {
        if (g_used <= g_budget * 85 / 100) break;
        SDL_DestroyTexture(e->img.tex);
        g_used -= e->bytes;
        g_entries.erase(e->url + "|" + std::to_string(e->max_w) + "x" + std::to_string(e->max_h) + "|" +
                        std::to_string(e->flags));
    }
}

std::string make_key(const std::string& url, int w, int h, int flags) {
    return url + "|" + std::to_string(w) + "x" + std::to_string(h) + "|" + std::to_string(flags);
}

}  // namespace

void init(size_t budget_bytes) { g_budget = budget_bytes; }

void shutdown() { clear(); }

void begin_frame() {
    std::vector<std::shared_ptr<Entry>> todo;
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_frame++;
        // Upload budget: a few textures per frame keeps frame times flat.
        size_t budget_px = 700 * 1000;
        size_t used_px = 0;
        auto it = g_ready_to_upload.begin();
        while (it != g_ready_to_upload.end() && (todo.empty() || used_px < budget_px)) {
            SDL_Surface* s = (*it)->pending;
            used_px += s ? (size_t)s->w * s->h : 0;
            todo.push_back(*it);
            it = g_ready_to_upload.erase(it);
        }
    }
    for (auto& e : todo) upload(*e);
    std::lock_guard<std::mutex> lk(g_m);
    evict_if_needed();
}

const Image* get(const std::string& url, int max_w, int max_h, int flags) {
    if (url.empty()) return nullptr;
    std::string key = make_key(url, max_w, max_h, flags);
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_entries.find(key);
    if (it == g_entries.end() && (max_w || max_h)) {
        // Images inserted with put() (e.g. embedded cover art) have no size key.
        auto put_it = g_entries.find(make_key(url, 0, 0, flags));
        if (put_it != g_entries.end() && put_it->second->state == READY) it = put_it;
    }
    std::shared_ptr<Entry> e;
    if (it == g_entries.end()) {
        e = std::make_shared<Entry>();
        e->url = url;
        e->max_w = max_w;
        e->max_h = max_h;
        e->flags = flags;
        e->last_used = g_frame;
        g_entries[key] = e;
        load_job(e);
        return &e->img;
    }
    e = it->second;
    e->last_used = g_frame;
    if (e->state == QUEUED) {
        e->state = LOADING;  // re-queue after being skipped
        load_job(e);
    } else if (e->state == FAILED && util::now_seconds() - e->failed_at > 45) {
        e->state = LOADING;
        e->img.failed = false;
        load_job(e);
    }
    return &e->img;
}

float fade(const Image* img, float duration) {
    if (!img || !img->ready) return 0;
    return std::clamp((float)((util::now_seconds() - img->ready_time) / duration), 0.0f, 1.0f);
}

void put(const std::string& key, SDL_Surface* surface, int flags) {
    surface = to_rgba(surface);
    if (!surface) return;
    if (flags & BLUR) {
        int w = 64, h = std::max(1, (int)std::lround(64.0 * surface->h / surface->w));
        surface = scale_to(surface, w, h);
        box_blur(surface, 3, 2);
    }
    auto e = std::make_shared<Entry>();
    e->url = key;
    e->flags = flags;
    e->last_used = g_frame;
    e->pending = surface;
    std::lock_guard<std::mutex> lk(g_m);
    std::string k = make_key(key, 0, 0, flags);
    auto it = g_entries.find(k);
    if (it != g_entries.end() && it->second->img.tex) {
        SDL_DestroyTexture(it->second->img.tex);
        g_used -= it->second->bytes;
    }
    g_entries[k] = e;
    upload(*e);
}

void clear() {
    std::lock_guard<std::mutex> lk(g_m);
    for (auto& [k, e] : g_entries) {
        if (e->img.tex) SDL_DestroyTexture(e->img.tex);
        if (e->pending) SDL_FreeSurface(e->pending);
        e->pending = nullptr;
        e->img.tex = nullptr;
    }
    g_entries.clear();
    g_ready_to_upload.clear();
    g_used = 0;
}

size_t bytes_used() { return g_used; }

}  // namespace images
