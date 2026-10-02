// Video orientation and pixel shape: rotation (display matrix) and sample aspect ratio read from
// real MP4/MOV/MKV files made by the ffmpeg command, the display sizes they give, stills made
// upright (checked against ffmpeg's own autorotation), the player's fit_rect(), and the renderer
// drawing a texture turned (with and without rounded corners).
//
//   make -f desktop.mk -f tools/tests/orientation.mk orientation-tests
extern "C" {
#include <libavformat/avformat.h>
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "audio/mixer.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/gfx.hpp"
#include "platform/platform.hpp"
#include "player/orientation.hpp"
#include "player/player.hpp"
#include "player/thumbnailer.hpp"

static int g_failures = 0;
#define CHECK(cond)                                                                             \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond << std::endl;    \
            g_failures++;                                                                       \
        }                                                                                       \
    } while (0)

static std::string g_dir, g_ffmpeg;

static void run(const std::string& args) {
    std::string cmd = "'" + g_ffmpeg + "' -nostdin -v error -y " + args;
    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "ffmpeg failed: " << cmd << std::endl;
        std::exit(2);
    }
}
static std::string at(const std::string& name) { return g_dir + "/" + name; }

// The four corners' colours, clockwise from the top left: yellow, green, black, red.
static const char* QUADRANTS =
    "color=c=black:s=64x36:r=10,format=yuv444p,"
    "geq=lum='if(lt(Y,H/2),if(lt(X,W/2),210,145),if(lt(X,W/2),81,16))'"
    ":cb='if(lt(Y,H/2),if(lt(X,W/2),16,54),if(lt(X,W/2),90,128))'"
    ":cr='if(lt(Y,H/2),if(lt(X,W/2),146,34),if(lt(X,W/2),240,128))'";

static void make_fixtures() {
    const std::string x264 = " -c:v libx264 -preset ultrafast -pix_fmt yuv420p ";
    run("-f lavfi -i testsrc=size=1920x1080:rate=30 -t 0.4" + x264 + "'" + at("base.mp4") + "'");
    // -display_rotation is an input option: degrees counter-clockwise, as FFmpeg reports them.
    for (int r : {0, 90, 180, 270})
        run(util::fmt("-display_rotation %d -i '%s' -c copy '%s'", r, at("base.mp4").c_str(),
                      at(util::fmt("r%d.mp4", r)).c_str()));
    run("-display_rotation -90 -i '" + at("base.mp4") + "' -c copy '" + at("rm90.mov") + "'");
    run("-display_rotation 90 -i '" + at("base.mp4") + "' -c copy '" + at("r90.mkv") + "'");
    run("-f lavfi -i testsrc=size=720x480:rate=30 -t 0.4 -vf setsar=32/27" + x264 + "'" + at("ana.mp4") + "'");
    run("-display_rotation 90 -i '" + at("ana.mp4") + "' -c copy '" + at("ana_r90.mp4") + "'");
    run("-f lavfi -i testsrc=size=1440x1080:rate=30 -t 0.4 -vf setsar=4/3" + x264 + "'" + at("hdv.mkv") + "'");
    run(std::string("-f lavfi -i \"") + QUADRANTS + "\" -t 0.5" + x264 + "'" + at("q.mp4") + "'");
    for (int r : {90, 180, -90}) {
        std::string name = util::fmt("q%d.mp4", r);
        run(util::fmt("-display_rotation %d -i '%s' -c copy '%s'", r, at("q.mp4").c_str(), at(name).c_str()));
        // ffmpeg's own upright first picture: the reference for which way is up.
        run(util::fmt("-i '%s' -frames:v 1 -f rawvideo -pix_fmt rgba '%s.rgba'", at(name).c_str(), at(name).c_str()));
    }
    run("-i '" + at("q.mp4") + "' -frames:v 1 -f rawvideo -pix_fmt rgba '" + at("q.mp4") + ".rgba'");
}

struct Probe {
    player::Orientation o;
    int w = 0, h = 0;
};
static Probe probe(const std::string& name) {
    Probe p;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, at(name).c_str(), nullptr, nullptr) < 0) {
        std::cerr << "can't open " << name << std::endl;
        g_failures++;
        return p;
    }
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        int v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (v >= 0) {
            p.o = player::stream_orientation(fmt->streams[v]);
            p.w = fmt->streams[v]->codecpar->width;
            p.h = fmt->streams[v]->codecpar->height;
        }
    }
    avformat_close_input(&fmt);
    return p;
}

static void check_file(const std::string& name, int coded_w, int coded_h, int turns, int sar_num, int sar_den,
                       int disp_w, int disp_h) {
    Probe p = probe(name);
    int w = 0, h = 0;
    player::display_size(p.w, p.h, p.o, w, h);
    double aspect = player::display_aspect(p.w, p.h, p.o);
    std::cout << name << ": coded " << p.w << "x" << p.h << ", turns " << p.o.turns << ", sar " << p.o.sar_num << ":"
              << p.o.sar_den << ", display " << w << "x" << h << " (" << aspect << ")" << std::endl;
    CHECK(p.w == coded_w && p.h == coded_h);
    CHECK(p.o.turns == turns);
    CHECK(p.o.sar_num == sar_num && p.o.sar_den == sar_den);
    CHECK(w == disp_w && h == disp_h);
    CHECK(std::abs(aspect - (double)disp_w / disp_h) < 0.01);
    if (turns & 1) CHECK(h > w);  // upright phone video: portrait
}

// Corner colours of an RGBA picture, clockwise from the top left, sampled a quarter in.
struct Rgb { int r, g, b; };
static std::vector<Rgb> corners(const uint8_t* px, int w, int h, int pitch) {
    std::vector<Rgb> out;
    const float xs[4] = {0.25f, 0.75f, 0.75f, 0.25f}, ys[4] = {0.25f, 0.25f, 0.75f, 0.75f};
    for (int i = 0; i < 4; i++) {
        const uint8_t* p = px + (size_t)(int)(ys[i] * h) * pitch + (size_t)(int)(xs[i] * w) * 4;
        out.push_back(Rgb{p[0], p[1], p[2]});
    }
    return out;
}
static bool same(const Rgb& a, const Rgb& b, int tolerance = 70) {
    return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance && std::abs(a.b - b.b) <= tolerance;
}
static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void check_pure_logic() {
    CHECK(player::quarter_turns_cw(0) == 0);
    CHECK(player::quarter_turns_cw(90) == 3);    // 90° counter-clockwise = 3 quarter turns clockwise
    CHECK(player::quarter_turns_cw(-90) == 1);   // phones' usual portrait
    CHECK(player::quarter_turns_cw(180) == 2 && player::quarter_turns_cw(-180) == 2);
    CHECK(player::quarter_turns_cw(270) == 1 && player::quarter_turns_cw(-270) == 3);
    CHECK(player::quarter_turns_cw(360) == 0 && player::quarter_turns_cw(-89.4) == 1);
    CHECK(player::quarter_turns_cw(NAN) == 0);
    CHECK(player::matrix_quarter_turns(nullptr) == 0);

    player::Orientation o;
    int w, h;
    player::display_size(0, 0, o, w, h);
    CHECK(w == 0 && h == 0 && player::display_aspect(0, 0, o) == 0);
    o.sar_num = 8;
    o.sar_den = 9;  // tall pixels (4:3 NTSC DVD): made taller, not narrower
    player::display_size(720, 480, o, w, h);
    CHECK(w == 720 && h == 540);

    // rotate_pixels32 against the definition, 3x2 with every pixel distinct.
    const int W = 3, H = 2;
    uint32_t src[W * H], dst[W * H], back[W * H];
    for (int i = 0; i < W * H; i++) src[i] = 0x01020300u + i;
    for (int t = 0; t < 4; t++) {
        player::rotate_pixels32((const uint8_t*)src, W, H, W * 4, t, (uint8_t*)dst, (t & 1 ? H : W) * 4);
        const int dw = t & 1 ? H : W;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                // Where source (x, y) lands turned clockwise t times.
                int dx = x, dy = y;
                if (t == 1) dx = H - 1 - y, dy = x;
                if (t == 2) dx = W - 1 - x, dy = H - 1 - y;
                if (t == 3) dx = y, dy = W - 1 - x;
                CHECK(dst[dy * dw + dx] == src[y * W + x]);
            }
        // ...and turning the rest of the way round gives the original back.
        player::rotate_pixels32((const uint8_t*)dst, dw, t & 1 ? W : H, dw * 4, (4 - t) & 3, (uint8_t*)back, W * 4);
        for (int i = 0; i < W * H; i++) CHECK(back[i] == src[i]);
    }
    std::cout << (g_failures ? "(failures so far) " : "PASS ") << "pure orientation logic" << std::endl;
}

static void check_files() {
    check_file("base.mp4", 1920, 1080, 0, 1, 1, 1920, 1080);
    check_file("r0.mp4", 1920, 1080, 0, 1, 1, 1920, 1080);
    check_file("r90.mp4", 1920, 1080, 3, 1, 1, 1080, 1920);
    check_file("r180.mp4", 1920, 1080, 2, 1, 1, 1920, 1080);
    check_file("r270.mp4", 1920, 1080, 1, 1, 1, 1080, 1920);
    check_file("rm90.mov", 1920, 1080, 1, 1, 1, 1080, 1920);
    check_file("r90.mkv", 1920, 1080, 3, 1, 1, 1080, 1920);
    check_file("ana.mp4", 720, 480, 0, 32, 27, 853, 480);
    check_file("ana_r90.mp4", 720, 480, 3, 32, 27, 480, 853);
    check_file("hdv.mkv", 1440, 1080, 0, 4, 3, 1920, 1080);
    std::cout << (g_failures ? "(failures so far) " : "PASS ") << "rotation, pixel shape and display size of MP4/MOV/MKV fixtures" << std::endl;
}

// video_thumbnail() pictures against ffmpeg's autorotated ones, and its cache.
static void check_thumbnails() {
    for (const char* name : {"q.mp4", "q90.mp4", "q180.mp4", "q-90.mp4"}) {
        const std::string path = at(name);
        const unsigned long long key = util::hash64(path);
        const std::string stale = util::fmt("%s/thumbs/%016llx-64.jpg", g_dir.c_str(), key);
        util::make_dirs(g_dir + "/thumbs");
        std::ofstream(stale) << "stale";  // an unrotated still from before
        std::remove(util::fmt("%s/thumbs/%016llx-64-o1.jpg", g_dir.c_str(), key).c_str());  // an earlier run's
        SDL_Surface* s = player::video_thumbnail(path, 64);
        CHECK(s != nullptr);
        if (!s) continue;
        const bool odd = std::string(name) == "q90.mp4" || std::string(name) == "q-90.mp4";
        CHECK(s->format->format == SDL_PIXELFORMAT_RGBA32);
        CHECK(odd ? (s->w == 36 && s->h == 64) : (s->w == 64 && s->h == 36));
        std::vector<uint8_t> ref = read_file(path + ".rgba");
        const int rw = odd ? 36 : 64, rh = odd ? 64 : 36;
        CHECK(ref.size() == (size_t)rw * rh * 4);
        if (ref.size() == (size_t)rw * rh * 4) {
            auto got = corners((const uint8_t*)s->pixels, s->w, s->h, s->pitch);
            auto want = corners(ref.data(), rw, rh, rw * 4);
            for (int i = 0; i < 4; i++) {
                if (!same(got[i], want[i]))
                    std::cerr << name << " corner " << i << ": got " << got[i].r << "," << got[i].g << "," << got[i].b
                              << " want " << want[i].r << "," << want[i].g << "," << want[i].b << std::endl;
                CHECK(same(got[i], want[i]));
            }
        }
        SDL_FreeSurface(s);
        CHECK(!util::file_exists(stale));
        CHECK(util::file_exists(util::fmt("%s/thumbs/%016llx-64-o1.jpg", g_dir.c_str(), key)));
    }
    // Anamorphic 720x480 at 32:27 is 16:9.
    for (const char* name : {"ana.mp4", "ana_r90.mp4"})
        std::remove(util::fmt("%s/thumbs/%016llx-320-o1.jpg", g_dir.c_str(), (unsigned long long)util::hash64(at(name))).c_str());
    if (SDL_Surface* s = player::video_thumbnail(at("ana.mp4"), 320)) {
        CHECK(s->w == 320 && s->h == 180);
        SDL_FreeSurface(s);
    } else {
        CHECK(false);
    }
    if (SDL_Surface* s = player::video_thumbnail(at("ana_r90.mp4"), 320)) {
        CHECK(s->w == 320 && s->h == 568);
        SDL_FreeSurface(s);
    } else {
        CHECK(false);
    }
    std::cout << (g_failures ? "(failures so far) " : "PASS ") << "thumbnails upright (matching ffmpeg's autorotation), square pixels, new cache name" << std::endl;
}

// gfx::image_rotated into a render target, read back.
static void check_rendering(SDL_Renderer* renderer) {
    const Rgb colors[4] = {{255, 255, 0}, {0, 255, 0}, {0, 0, 0}, {255, 0, 0}};  // TL, TR, BR, BL
    uint8_t px[2 * 2 * 4];
    const int at_xy[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; i++) {
        uint8_t* p = px + (at_xy[i][1] * 2 + at_xy[i][0]) * 4;
        p[0] = colors[i].r, p[1] = colors[i].g, p[2] = colors[i].b, p[3] = 255;
    }
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 2, 2);
    SDL_UpdateTexture(tex, nullptr, px, 8);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    SDL_Texture* target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, 128, 128);
    CHECK(tex && target);
    for (float radius : {0.0f, 6.0f}) {
        for (int turns = 0; turns < 4; turns++) {
            SDL_SetRenderTarget(renderer, target);
            gfx::begin_frame(gfx::Color(40, 40, 200));
            // The destination is in the turned shape: portrait for 1 and 3 turns.
            gfx::Rect dst = turns & 1 ? gfx::Rect(20, 10, 60, 100) : gfx::Rect(10, 20, 100, 60);
            gfx::image_rotated(tex, dst, turns, gfx::WHITE, radius);
            gfx::end_frame();
            std::vector<uint8_t> out(128 * 128 * 4);
            SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, out.data(), 128 * 4);
            SDL_SetRenderTarget(renderer, nullptr);
            // Destination corner i shows the texture's corner (i - turns) mod 4.
            const float xs[4] = {0.25f, 0.75f, 0.75f, 0.25f}, ys[4] = {0.25f, 0.25f, 0.75f, 0.75f};
            for (int i = 0; i < 4; i++) {
                int x = (int)(dst.x + xs[i] * dst.w), y = (int)(dst.y + ys[i] * dst.h);
                const uint8_t* p = &out[((size_t)y * 128 + x) * 4];
                Rgb want = colors[(i - turns + 4) & 3];
                if (!same(Rgb{p[0], p[1], p[2]}, want, 8))
                    std::cerr << "radius " << radius << " turns " << turns << " corner " << i << ": got " << (int)p[0]
                              << "," << (int)p[1] << "," << (int)p[2] << std::endl;
                CHECK(same(Rgb{p[0], p[1], p[2]}, want, 8));
            }
            // Outside the destination: the background.
            const uint8_t* bg = &out[((size_t)2 * 128 + 2) * 4];
            CHECK(bg[0] == 40 && bg[1] == 40 && bg[2] == 200);
        }
    }
    SDL_DestroyTexture(target);
    SDL_DestroyTexture(tex);
    std::cout << (g_failures ? "(failures so far) " : "PASS ") << "gfx::image_rotated, square and rounded corners" << std::endl;
}

static void tick() {
    SDL_PumpEvents();
    player::update();
    SDL_Delay(5);
}
static bool until(const std::function<bool()>& done, int timeout = 8000) {
    const auto start = SDL_GetTicks();
    while (!done() && SDL_GetTicks() - start < (unsigned)timeout) {
        tick();
        if (player::state() == player::FAILED) {
            std::cerr << player::error() << std::endl;
            return false;
        }
    }
    return done();
}

// The real player: rotation and the rect the picture is drawn into.
static void check_player() {
    struct Case { const char* name; int turns; int w, h; };
    const Case cases[] = {
        {"r0.mp4", 0, 1280, 720},  {"r90.mp4", 3, 405, 720},  {"rm90.mov", 1, 405, 720},
        {"r180.mp4", 2, 1280, 720}, {"ana.mp4", 0, 1280, 720}, {"hdv.mkv", 0, 1280, 720},
        {"ana_r90.mp4", 3, 405, 720},
    };
    for (const Case& c : cases) {
        player::Source src;
        src.url = at(c.name);
        src.title = c.name;
        src.remember_position = false;
        player::open(src);
        CHECK(until([] { return player::video_texture() != nullptr; }));
        SDL_Rect r = player::fit_rect(1280, 720);
        std::cout << c.name << ": player turns " << player::video_rotation() << ", fit_rect " << r.x << "," << r.y << " "
                  << r.w << "x" << r.h << ", coded " << player::video_width() << "x" << player::video_height() << std::endl;
        CHECK(player::video_rotation() == c.turns);
        CHECK(std::abs(r.w - c.w) <= 1 && std::abs(r.h - c.h) <= 1);
        CHECK(r.x == (1280 - r.w) / 2 && r.y == (720 - r.h) / 2);
        player::close();
        CHECK(player::video_rotation() == 0);
    }
    std::cout << (g_failures ? "(failures so far) " : "PASS ") << "player rotation and fit_rect" << std::endl;
}

int main(int argc, char** argv) {
    g_dir = argc > 1 ? argv[1] : "build-desktop/orientation-fixtures";
    g_ffmpeg = getenv("FFMPEG") ? getenv("FFMPEG") : "ffmpeg";
    util::make_dirs(g_dir);
    make_fixtures();
    setenv("COFFEEFLIX_DATA", g_dir.c_str(), 1);  // thumbnails' cache
    platform::init();

    check_pure_logic();
    check_files();
    check_thumbnails();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        std::cerr << SDL_GetError() << std::endl;
        return 2;
    }
    SDL_Window* window = SDL_CreateWindow("Orientation", 0, 0, 640, 360, SDL_WINDOW_HIDDEN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE) : nullptr;
    if (!renderer) {
        std::cerr << SDL_GetError() << std::endl;
        return 2;
    }
    gfx::init(renderer);
    check_rendering(renderer);

    store::load(g_dir + "/store.json");
    tasks::init();
    if (!audio::init()) {
        std::cerr << "no audio" << std::endl;
        return 2;
    }
    player::init();
    check_player();
    player::shutdown();
    tasks::shutdown();
    audio::shutdown();
    gfx::shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (g_failures) {
        std::cerr << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "All orientation checks passed" << std::endl;
    return 0;
}
