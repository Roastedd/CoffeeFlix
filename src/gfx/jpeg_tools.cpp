#include "gfx/jpeg_tools.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#if __has_include(<jpeglib.h>)
#include <jpeglib.h>
#define HAVE_JPEG 1
#else
#define HAVE_JPEG 0   // a platform build without libjpeg: SDL_image decodes, and nothing here is reduced or cached
#endif

namespace jpeg_tools {

namespace {

#if HAVE_JPEG
struct ErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf jump;
};
void on_error(j_common_ptr c) { longjmp(((ErrorMgr*)c->err)->jump, 1); }
void quiet(j_common_ptr, int) {}
#endif

uint32_t rd16(const unsigned char* p, bool le) { return le ? (uint32_t)(p[0] | (p[1] << 8)) : (uint32_t)((p[0] << 8) | p[1]); }
uint32_t rd32(const unsigned char* p, bool le) {
    return le ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)
              : ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// One IFD of the TIFF block: calls on each (tag, type, count, value-or-offset pointer).
template <typename F>
void each_entry(const unsigned char* tiff, size_t tiff_len, size_t ifd, bool le, F f, size_t* next_ifd = nullptr) {
    if (ifd + 2 > tiff_len) return;
    const uint32_t n = rd16(tiff + ifd, le);
    if (ifd + 2 + (size_t)n * 12 + 4 > tiff_len) return;
    for (uint32_t i = 0; i < n; i++) {
        const unsigned char* e = tiff + ifd + 2 + (size_t)i * 12;
        f(rd16(e, le), rd16(e + 2, le), rd32(e + 4, le), e + 8);
    }
    if (next_ifd) *next_ifd = rd32(tiff + ifd + 2 + (size_t)n * 12, le);
}

std::string ascii(const unsigned char* tiff, size_t tiff_len, uint32_t count, const unsigned char* field, bool le) {
    const unsigned char* p = count <= 4 ? field : tiff + rd32(field, le);
    if (count > 4 && (size_t)rd32(field, le) + count > tiff_len) return "";
    std::string s((const char*)p, count);
    while (!s.empty() && (s.back() == 0 || s.back() == ' ')) s.pop_back();
    return s;
}

void parse_exif(const unsigned char* seg, size_t len, size_t seg_start_in_data, Info& out) {
    if (len < 14 || memcmp(seg, "Exif\0\0", 6) != 0) return;
    const unsigned char* tiff = seg + 6;
    const size_t tl = len - 6;
    const bool le = tiff[0] == 'I';
    if (!(tiff[0] == 'I' || tiff[0] == 'M') || rd16(tiff + 2, le) != 42) return;
    size_t ifd0 = rd32(tiff + 4, le), next = 0, exif_ifd = 0;
    each_entry(tiff, tl, ifd0, le, [&](uint32_t tag, uint32_t type, uint32_t count, const unsigned char* val) {
        if (tag == 0x0112 && type == 3) out.orientation = (int)rd16(val, le);
        else if (tag == 0x010F || tag == 0x0110) {
            const std::string s = ascii(tiff, tl, count, val, le);
            if (tag == 0x0110 || out.camera.empty()) out.camera = tag == 0x0110 ? s : s;
        } else if (tag == 0x0132 && out.taken.empty()) out.taken = ascii(tiff, tl, count, val, le);
        else if (tag == 0x8769) exif_ifd = rd32(val, le);
    }, &next);
    if (exif_ifd)
        each_entry(tiff, tl, exif_ifd, le, [&](uint32_t tag, uint32_t, uint32_t count, const unsigned char* val) {
            if (tag == 0x9003) out.taken = ascii(tiff, tl, count, val, le);
        });
    if (next && next < tl) {
        uint32_t off = 0, length = 0;
        each_entry(tiff, tl, next, le, [&](uint32_t tag, uint32_t, uint32_t, const unsigned char* val) {
            if (tag == 0x0201) off = rd32(val, le);
            else if (tag == 0x0202) length = rd32(val, le);
        });
        if (off && length && (size_t)off + length <= tl) {
            out.thumb_offset = seg_start_in_data + 6 + off;
            out.thumb_length = length;
        }
    }
}

}  // namespace

bool is_jpeg(const std::string& d) { return d.size() >= 4 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8 && (unsigned char)d[2] == 0xFF; }

bool read_info(const std::string& data, Info& out) {
    if (!is_jpeg(data)) return false;
    out = Info();
    const unsigned char* d = (const unsigned char*)data.data();
    const size_t n = data.size();
    size_t at = 2;
    while (at + 4 <= n) {
        if (d[at] != 0xFF) break;
        const unsigned char marker = d[at + 1];
        if (marker == 0xFF) { at++; continue; }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) { at += 2; continue; }
        if (marker == 0xD9 || marker == 0xDA) break;
        const size_t len = rd16(d + at + 2, false);
        if (len < 2 || at + 2 + len > n) {
            if (marker == 0xE1 && at + 4 < n) parse_exif(d + at + 4, n - at - 4, at + 4, out);  // a cut-off EXIF: what is in the part
            break;
        }
        if (marker == 0xE1) parse_exif(d + at + 4, len - 2, at + 4, out);
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC && at + 9 <= n) {
            out.height = (int)rd16(d + at + 5, false);
            out.width = (int)rd16(d + at + 7, false);
            return true;
        }
        at += 2 + len;
    }
    return out.width > 0 || out.thumb_length > 0 || out.orientation != 1;
}

#if HAVE_JPEG
SDL_Surface* decode_scaled(const std::string& data, int want_w, int want_h, int min_denom) {
    if (!is_jpeg(data)) return nullptr;
    jpeg_decompress_struct cinfo;
    ErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = on_error;
    jerr.pub.emit_message = quiet;
    SDL_Surface* volatile surf = nullptr;   // volatile: read after a longjmp (a damaged picture), or the surface leaks
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        if (surf) SDL_FreeSurface(surf);
        return nullptr;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (const unsigned char*)data.data(), (unsigned long)data.size());
    jpeg_read_header(&cinfo, TRUE);
    if (cinfo.num_components != 3 && cinfo.num_components != 1) {  // CMYK and the like: SDL_image does those
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }
    int denom = 1;
    for (int d : {8, 4, 2}) {
        const int w = (int)((cinfo.image_width + d - 1) / d), h = (int)((cinfo.image_height + d - 1) / d);
        if ((want_w <= 0 || w >= want_w) && (want_h <= 0 || h >= want_h) && (want_w > 0 || want_h > 0)) {
            denom = d;
            break;
        }
    }
    denom = std::max(denom, min_denom);
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    cinfo.out_color_space = JCS_EXT_RGBA;
    cinfo.dct_method = denom >= 4 ? JDCT_IFAST : JDCT_ISLOW;
    cinfo.do_fancy_upsampling = denom >= 2 ? FALSE : TRUE;
    jpeg_start_decompress(&cinfo);
    surf = SDL_CreateRGBSurfaceWithFormat(0, (int)cinfo.output_width, (int)cinfo.output_height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char* row = (unsigned char*)surf->pixels + (size_t)cinfo.output_scanline * surf->pitch;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return surf;
}
#else
SDL_Surface* decode_scaled(const std::string&, int, int, int) { return nullptr; }
#endif

SDL_Surface* decode_thumbnail(const std::string& data, const Info& info) {
    if (!info.thumb_length || info.thumb_offset + info.thumb_length > data.size()) return nullptr;
    return decode_scaled(data.substr(info.thumb_offset, info.thumb_length), 0, 0);
}

SDL_Surface* rotate_quarters(SDL_Surface* s, int turns) {
    turns = ((turns % 4) + 4) % 4;
    if (!s || turns == 0) return s;
    const int w = s->w, h = s->h;
    const int nw = turns == 2 ? w : h, nh = turns == 2 ? h : w;
    SDL_Surface* d = SDL_CreateRGBSurfaceWithFormat(0, nw, nh, 32, SDL_PIXELFORMAT_RGBA32);
    if (!d) return s;
    for (int y = 0; y < h; y++) {
        const uint32_t* src = (const uint32_t*)((const uint8_t*)s->pixels + (size_t)y * s->pitch);
        for (int x = 0; x < w; x++) {
            int dx, dy;
            if (turns == 1) { dx = h - 1 - y; dy = x; }          // clockwise
            else if (turns == 2) { dx = w - 1 - x; dy = h - 1 - y; }
            else { dx = y; dy = w - 1 - x; }                      // counter-clockwise
            ((uint32_t*)((uint8_t*)d->pixels + (size_t)dy * d->pitch))[dx] = src[x];
        }
    }
    SDL_FreeSurface(s);
    return d;
}

static SDL_Surface* mirror(SDL_Surface* s, bool horizontal) {  // a flip, in place
    if (!s) return s;
    const int w = s->w, h = s->h;
    for (int y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)s->pixels + (size_t)y * s->pitch);
        if (horizontal) {
            for (int x = 0; x < w / 2; x++) std::swap(row[x], row[w - 1 - x]);
        } else if (y < h / 2) {
            uint32_t* other = (uint32_t*)((uint8_t*)s->pixels + (size_t)(h - 1 - y) * s->pitch);
            for (int x = 0; x < w; x++) std::swap(row[x], other[x]);
        }
    }
    return s;
}

SDL_Surface* orient(SDL_Surface* s, int o) {
    if (!s) return s;
    switch (o) {
        case 2: return mirror(s, true);
        case 3: return rotate_quarters(s, 2);
        case 4: return mirror(s, false);
        case 5: return mirror(rotate_quarters(s, 1), true);   // transpose
        case 6: return rotate_quarters(s, 1);
        case 7: return mirror(rotate_quarters(s, 3), true);   // transverse
        case 8: return rotate_quarters(s, 3);
        default: return s;
    }
}

#if HAVE_JPEG
std::string encode(SDL_Surface* s, int quality) {
    if (!s || s->format->format != SDL_PIXELFORMAT_RGBA32) return "";
    jpeg_compress_struct cinfo;
    ErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = on_error;
    jerr.pub.emit_message = quiet;
    unsigned char* out = nullptr;
    unsigned long out_size = 0;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_compress(&cinfo);
        free(out);
        return "";
    }
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &out, &out_size);
    cinfo.image_width = (JDIMENSION)s->w;
    cinfo.image_height = (JDIMENSION)s->h;
    cinfo.input_components = 4;
    cinfo.in_color_space = JCS_EXT_RGBA;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        unsigned char* row = (unsigned char*)s->pixels + (size_t)cinfo.next_scanline * s->pitch;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    std::string result((const char*)out, out_size);
    free(out);
    return result;
}
#else
std::string encode(SDL_Surface*, int) { return ""; }
#endif

}  // namespace jpeg_tools
