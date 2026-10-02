// Picture size limit: the size a header states (PNG, JPEG, GIF, WebP) is read without decoding, and
// a picture over 16 million pixels is refused before anything is allocated for it.
// make -f desktop.mk -f tools/tests/images.mk images-tests && build-desktop/images-test
// With a folder of files made by real encoders: python3 tools/tests/images.py
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "gfx/images.hpp"

static int g_failed = 0, g_checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        g_checks++;                                                     \
        if (!(cond)) {                                                  \
            g_failed++;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static std::string bytes(std::initializer_list<int> v) {
    std::string s;
    for (int b : v) s += (char)b;
    return s;
}

static std::string be16(unsigned v) { return bytes({(int)(v >> 8 & 255), (int)(v & 255)}); }
static std::string be32(uint32_t v) { return be16(v >> 16) + be16(v & 0xffff); }
static std::string le16(unsigned v) { return bytes({(int)(v & 255), (int)(v >> 8 & 255)}); }
static std::string le24(uint32_t v) { return le16(v & 0xffff) + bytes({(int)(v >> 16 & 255)}); }

// Headers only: what a file states, which is all the size check looks at.
static std::string png(uint32_t w, uint32_t h) {
    return bytes({0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}) + be32(13) + "IHDR" + be32(w) + be32(h) +
           bytes({8, 6, 0, 0, 0}) + be32(0);
}

static std::string gif(unsigned w, unsigned h) { return "GIF89a" + le16(w) + le16(h) + bytes({0xf7, 0, 0}); }

static std::string riff(const std::string& chunk) { return "RIFF" + std::string(4, '\0') + "WEBP" + chunk; }

static std::string webp_extended(uint32_t w, uint32_t h) {
    return riff("VP8X" + std::string(4, '\0') + bytes({0, 0, 0, 0}) + le24(w - 1) + le24(h - 1));
}

static std::string webp_lossy(unsigned w, unsigned h) {
    return riff("VP8 " + std::string(4, '\0') + bytes({0x30, 1, 0, 0x9d, 1, 0x2a}) + le16(w) + le16(h));
}

static std::string webp_lossless(unsigned w, unsigned h) {
    uint32_t bits = (w - 1) | ((h - 1) << 14);
    return riff("VP8L" + std::string(4, '\0') + bytes({0x2f, (int)(bits & 255), (int)(bits >> 8 & 255),
                                                       (int)(bits >> 16 & 255), (int)(bits >> 24 & 255)}));
}

// A frame header (SOF0 or another) after the segments a camera writes, one of them holding a small
// picture of its own with a frame header of its own.
static std::string jpeg(unsigned w, unsigned h, int frame = 0xc0, bool camera = true) {
    std::string s = bytes({0xff, 0xd8});
    if (camera) {
        std::string thumbnail = bytes({0xff, 0xd8, 0xff, 0xc0}) + be16(11) + bytes({8}) + be16(120) + be16(160) +
                                bytes({1, 1, 0x11, 0}) + bytes({0xff, 0xd9});
        std::string exif = "Exif" + std::string(2, '\0') + thumbnail;
        s += bytes({0xff, 0xe1}) + be16((unsigned)exif.size() + 2) + exif;
        s += bytes({0xff, 0xdb}) + be16(4) + bytes({0, 1});                         // a table
        s += bytes({0xff, 0xff, 0xff, 0xdb}) + be16(4) + bytes({1, 2});             // padding before a marker
    }
    s += bytes({0xff, frame}) + be16(11) + bytes({8}) + be16(h) + be16(w) + bytes({1, 1, 0x11, 0});
    s += bytes({0xff, 0xda});
    return s;
}

static bool size_of(const std::string& d, int64_t& w, int64_t& h) {
    w = h = -1;
    return images::stated_size(d, w, h);
}

static void check_size(const char* what, const std::string& d, int64_t want_w, int64_t want_h) {
    int64_t w, h;
    const bool ok = size_of(d, w, h);
    if (!ok || w != want_w || h != want_h) {
        g_failed++;
        std::printf("FAIL %s: read %s %lld x %lld, wanted %lld x %lld\n", what, ok ? "" : "nothing:", (long long)w,
                    (long long)h, (long long)want_w, (long long)want_h);
    }
    g_checks++;
}

static void test_headers() {
    check_size("png", png(1920, 1080), 1920, 1080);
    check_size("png, tall", png(7, 100000), 7, 100000);
    check_size("png, 32 bits", png(4000000000u, 4000000000u), 4000000000ll, 4000000000ll);
    check_size("gif", gif(640, 480), 640, 480);
    check_size("gif, largest", gif(65535, 65535), 65535, 65535);
    check_size("webp extended", webp_extended(3000, 2000), 3000, 2000);
    check_size("webp extended, largest", webp_extended(16777216, 16777216), 16777216, 16777216);
    check_size("webp lossy", webp_lossy(1280, 720), 1280, 720);
    check_size("webp lossy, scale bits left out", webp_lossy(0x4000 | 1280, 0x8000 | 720), 1280, 720);
    check_size("webp lossless", webp_lossless(300, 450), 300, 450);
    check_size("webp lossless, largest", webp_lossless(16384, 16384), 16384, 16384);
    check_size("jpeg", jpeg(4000, 3000), 4000, 3000);
    check_size("jpeg, progressive", jpeg(1234, 5678, 0xc2), 1234, 5678);
    check_size("jpeg, no camera segments", jpeg(50, 60, 0xc0, false), 50, 60);
    check_size("jpeg, largest", jpeg(65535, 65535), 65535, 65535);

    int64_t w, h;
    // Not read: other formats, a header cut short, and text.
    CHECK(!size_of("", w, h));
    CHECK(!size_of("GIF89a", w, h));
    CHECK(!size_of(png(10, 10).substr(0, 23), w, h));
    CHECK(!size_of(png(10, 10).substr(0, 12), w, h));
    CHECK(!size_of(gif(10, 10).substr(0, 9), w, h));
    CHECK(!size_of(webp_lossy(10, 10).substr(0, 29), w, h));
    CHECK(!size_of(webp_extended(10, 10).substr(0, 29), w, h));
    CHECK(!size_of(webp_lossless(10, 10).substr(0, 24), w, h));
    CHECK(!size_of(jpeg(10, 10).substr(0, 20), w, h));
    CHECK(!size_of(jpeg(10, 10, 0xc0, false).substr(0, 10), w, h));
    CHECK(!size_of(bytes({0xff, 0xd8}), w, h));
    CHECK(!size_of(bytes({0xff, 0xd8, 0xff, 0xd9, 0, 0}), w, h));
    CHECK(!size_of(bytes({0xff, 0xd8, 0xff, 0xda, 0, 8, 0, 0}), w, h));  // picture data first
    CHECK(!size_of(bytes({0xff, 0xd8, 0x12, 0x34, 0x56, 0x78}), w, h));
    CHECK(!size_of(bytes({0xff, 0xd8, 0xff, 0xe0, 0, 0, 0, 0}), w, h));  // a segment shorter than its own length
    CHECK(!size_of("BM" + std::string(60, '\0'), w, h));
    CHECK(!size_of("<html><body>404 not found</body></html>", w, h));
    CHECK(!size_of(riff("VP8 " + std::string(20, 'x')), w, h));
    CHECK(!size_of(riff("XXXX" + std::string(20, 'x')), w, h));
    CHECK(!size_of(std::string(100, 'A'), w, h));
}

static void test_limit() {
    // 16 million pixels (4096 x 4096) still load; more do not, however they are cut.
    CHECK(!images::too_big(png(4096, 4096)));
    CHECK(images::too_big(png(4097, 4096)));
    CHECK(images::too_big(png(4096, 4097)));
    CHECK(!images::too_big(png(16777216, 1)));
    CHECK(images::too_big(png(16777217, 1)));
    CHECK(!images::too_big(png(1, 16777216)));
    CHECK(!images::too_big(png(1920, 1080)));
    CHECK(!images::too_big(png(0, 0)));
    // States that would overflow a product of two 64 bit numbers.
    CHECK(images::too_big(png(4294967295u, 4294967295u)));
    CHECK(images::too_big(png(4294967295u, 1)));
    CHECK(images::too_big(gif(65535, 65535)));
    CHECK(!images::too_big(gif(4096, 4096)));
    CHECK(images::too_big(webp_extended(16777216, 16777216)));
    CHECK(images::too_big(webp_lossless(16384, 16384)));
    CHECK(!images::too_big(webp_lossless(4096, 4096)));
    CHECK(images::too_big(webp_lossy(16383, 16383)));
    CHECK(images::too_big(jpeg(6000, 6000)));
    CHECK(images::too_big(jpeg(65535, 65535)));
    CHECK(!images::too_big(jpeg(4096, 4096)));
    CHECK(!images::too_big(jpeg(3840, 2160)));
    CHECK(!images::too_big("not a picture"));
    CHECK(!images::too_big(""));
}

static double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static void test_refused_at_once() {
    // A picture that states more than there is memory for is refused, not attempted: nothing
    // is allocated for 100000 x 100000 pixels (40 GB), and the answer is immediate.
    const std::string bombs[] = {png(100000, 100000), png(2000000000u, 2000000000u), gif(65535, 65535),
                                 webp_extended(16777216, 16777216), jpeg(65535, 65535)};
    for (const std::string& bomb : bombs) {
        auto t0 = std::chrono::steady_clock::now();
        SDL_Surface* s = images::decode(bomb, 256, 256);
        CHECK(s == nullptr);
        CHECK(ms_since(t0) < 250);
        if (s) SDL_FreeSurface(s);
    }
}

static std::string encode(SDL_Surface* s, bool jpeg_format) {
    std::string buffer(1 << 20, '\0');
    SDL_RWops* rw = SDL_RWFromMem(&buffer[0], (int)buffer.size());
    int rc = jpeg_format ? IMG_SaveJPG_RW(s, rw, 0, 90) : IMG_SavePNG_RW(s, rw, 0);
    size_t used = (size_t)SDL_RWtell(rw);
    SDL_RWclose(rw);
    if (rc != 0) return "";
    buffer.resize(used);
    return buffer;
}

static void test_real_pictures() {
    SDL_Surface* pic = SDL_CreateRGBSurfaceWithFormat(0, 64, 48, 32, SDL_PIXELFORMAT_RGBA32);
    CHECK(pic != nullptr);
    if (!pic) return;
    SDL_FillRect(pic, nullptr, SDL_MapRGBA(pic->format, 200, 100, 50, 255));
    for (bool jpeg_format : {false, true}) {
        const char* kind = jpeg_format ? "jpeg" : "png";
        std::string file = encode(pic, jpeg_format);
        if (file.empty()) {
            std::printf("SKIP %s: this SDL_image cannot write it\n", kind);
            continue;
        }
        int64_t w, h;
        CHECK(size_of(file, w, h));
        CHECK(w == 64 && h == 48);
        CHECK(!images::too_big(file));
        SDL_Surface* whole = images::decode(file);
        CHECK(whole && whole->w == 64 && whole->h == 48);
        SDL_Surface* fitted = images::decode(file, 32, 0);
        CHECK(fitted && fitted->w == 32 && fitted->h == 24);
        SDL_Surface* blurred = images::decode(file, 0, 0, images::BLUR);
        CHECK(blurred && blurred->w == 64);
        for (SDL_Surface* s : {whole, fitted, blurred})
            if (s) SDL_FreeSurface(s);
        // The same header with a bigger size stated is refused, though what follows it is a real picture.
        std::string big = file;
        if (jpeg_format) {
            // SOF0 is the first FF C0 after the tables: 5 bytes on, height then width
            size_t at = big.find(bytes({0xff, 0xc0}));
            CHECK(at != std::string::npos);
            if (at != std::string::npos) big.replace(at + 5, 4, be16(6000) + be16(6000));
        } else {
            big.replace(16, 8, be32(6000) + be32(6000));
        }
        CHECK(images::too_big(big));
        CHECK(images::decode(big) == nullptr);
    }
    SDL_FreeSurface(pic);
}

static void test_garbage() {
    // Bytes that are no picture, or a torn one, fail without a crash.
    std::mt19937 rng(7);
    for (int i = 0; i < 200; i++) {
        std::string junk(1 + rng() % 3000, '\0');
        for (char& c : junk) c = (char)rng();
        if (i % 4 == 1) junk.replace(0, std::min<size_t>(junk.size(), 8), png(10, 10).substr(0, 8));
        if (i % 4 == 2 && junk.size() > 2) junk[0] = (char)0xff, junk[1] = (char)0xd8;
        if (i % 4 == 3 && junk.size() > 12) junk.replace(0, 12, riff("VP8X").substr(0, 12));
        int64_t w, h;
        size_of(junk, w, h);
        images::too_big(junk);
        SDL_Surface* s = images::decode(junk, 64, 64);
        if (s) SDL_FreeSurface(s);
    }
    CHECK(true);  // still here
    for (size_t cut : {0, 1, 7, 8, 12, 23, 24, 25, 40}) {
        std::string torn = png(64, 48).substr(0, std::min(cut, png(64, 48).size()));
        SDL_Surface* s = images::decode(torn);
        CHECK(s == nullptr);
    }
}

static std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// <folder>/sizes.txt: "<file> <width> <height> <ok|big>" for each file a real encoder made: the
// size the header is read as, and whether the picture loads or is refused for its size.
static void test_files(const std::string& folder) {
    std::ifstream list(folder + "/sizes.txt");
    std::string name, verdict;
    long long w, h;
    int files = 0;
    while (list >> name >> w >> h >> verdict) {
        files++;
        const std::string data = read_file(folder + "/" + name);
        check_size(name.c_str(), data, w, h);
        const bool big = verdict == "big";
        CHECK(images::too_big(data) == big);
        auto t0 = std::chrono::steady_clock::now();
        SDL_Surface* s = images::decode(data);
        if (big) {
            CHECK(s == nullptr);
            CHECK(ms_since(t0) < 250);
        } else {
            CHECK(s != nullptr);
            if (!s) std::printf("FAIL %s: not decoded (%s)\n", name.c_str(), SDL_GetError());
            else CHECK(s->w == w && s->h == h);
        }
        if (s) SDL_FreeSurface(s);
    }
    CHECK(files > 0);
    std::printf("%d files made by real encoders\n", files);
}

int main(int argc, char** argv) {
    SDL_Init(0);
    if (argc > 1) {
        test_files(argv[1]);
    } else {
        test_headers();
        test_limit();
        test_refused_at_once();
        test_real_pictures();
        test_garbage();
    }
    SDL_Quit();
    std::printf("%s: %d checks, %d failed\n", g_failed ? "FAIL" : "PASS", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
