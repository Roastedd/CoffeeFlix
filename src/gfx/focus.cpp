#include "gfx/focus.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace focus {

namespace {

constexpr int GRID_W = 96;

struct Cell { float r, g, b; };

bool skin(const Cell& c) {
    const float y = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
    const float cb = 128 - 0.168736f * c.r - 0.331264f * c.g + 0.5f * c.b;
    const float cr = 128 + 0.5f * c.r - 0.418688f * c.g - 0.081312f * c.b;
    if (y < 50 || y > 245) return false;
    if (cb < 77 || cb > 127 || cr < 133 || cr > 173) return false;
    return c.r > 95 && c.g > 40 && c.b > 20 && c.r > c.g && c.r > c.b && std::fabs(c.r - c.g) > 12;
}

struct Blob {
    int area = 0, x0, y0, x1, y1;
    double sx = 0, sy = 0;
};

}  // namespace

Focus analyze(SDL_Surface* s) {
    Focus f;
    if (!s || s->w < 8 || s->h < 8 || s->format->BytesPerPixel != 4) return f;
    const int gw = GRID_W, gh = std::max(8, (int)std::lround((double)GRID_W * s->h / s->w));
    std::vector<Cell> cells((size_t)gw * gh);
    SDL_PixelFormat* fmt = s->format;
    // Each cell is the average of a few samples inside it.
    for (int gy = 0; gy < gh; gy++) {
        for (int gx = 0; gx < gw; gx++) {
            double r = 0, g = 0, b = 0;
            int n = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    const int x = std::min(s->w - 1, (int)(((gx + (sx + 0.5) / 3) * s->w) / gw));
                    const int y = std::min(s->h - 1, (int)(((gy + (sy + 0.5) / 3) * s->h) / gh));
                    const uint32_t p = ((const uint32_t*)((const uint8_t*)s->pixels + (size_t)y * s->pitch))[x];
                    uint8_t pr, pg, pb;
                    SDL_GetRGB(p, fmt, &pr, &pg, &pb);
                    r += pr; g += pg; b += pb;
                    n++;
                }
            cells[(size_t)gy * gw + gx] = {(float)(r / n), (float)(g / n), (float)(b / n)};
        }
    }
    // People: patches of skin tone, joined four ways.
    std::vector<uint8_t> mask((size_t)gw * gh), seen((size_t)gw * gh, 0);
    for (size_t i = 0; i < mask.size(); i++) mask[i] = skin(cells[i]);
    std::vector<Blob> blobs;
    std::vector<int> stack;
    for (int y0 = 0; y0 < gh; y0++)
        for (int x0 = 0; x0 < gw; x0++) {
            const size_t start = (size_t)y0 * gw + x0;
            if (!mask[start] || seen[start]) continue;
            Blob b;
            b.x0 = b.x1 = x0;
            b.y0 = b.y1 = y0;
            stack.assign(1, (int)start);
            seen[start] = 1;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                const int x = i % gw, y = i / gw;
                b.area++;
                b.sx += x;
                b.sy += y;
                b.x0 = std::min(b.x0, x); b.x1 = std::max(b.x1, x);
                b.y0 = std::min(b.y0, y); b.y1 = std::max(b.y1, y);
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; k++) {
                    if (nx[k] < 0 || nx[k] >= gw || ny[k] < 0 || ny[k] >= gh) continue;
                    const size_t j = (size_t)ny[k] * gw + nx[k];
                    if (mask[j] && !seen[j]) {
                        seen[j] = 1;
                        stack.push_back((int)j);
                    }
                }
            }
            blobs.push_back(b);
        }
    std::vector<Blob> kept;
    int largest = 0;
    const int total = gw * gh;
    for (const Blob& b : blobs) {
        const float bw = (float)(b.x1 - b.x0 + 1), bh = (float)(b.y1 - b.y0 + 1);
        const float aspect = bw / bh, fill = b.area / (bw * bh);
        if (b.area < 4 || b.area > total / 4) continue;       // a speck, or a wall of skin-coloured wood or sand
        if (aspect < 0.3f || aspect > 2.4f || fill < 0.4f) continue;  // not the shape of a head, or of a head and shoulders
        kept.push_back(b);
        largest = std::max(largest, b.area);
    }
    kept.erase(std::remove_if(kept.begin(), kept.end(), [&](const Blob& b) { return b.area * 5 < largest; }), kept.end());  // hands, knees
    if (kept.size() > 12) {
        std::sort(kept.begin(), kept.end(), [](const Blob& a, const Blob& b) { return a.area > b.area; });
        kept.resize(12);
    }
    if (!kept.empty()) {
        const Blob* main = &kept[0];
        float ax0 = 1, ay0 = 1, ax1 = 0, ay1 = 0;
        for (const Blob& b : kept) {
            if (b.area > main->area) main = &b;
            ax0 = std::min(ax0, (float)b.x0 / gw); ax1 = std::max(ax1, (float)(b.x1 + 1) / gw);
            ay0 = std::min(ay0, (float)b.y0 / gh); ay1 = std::max(ay1, (float)(b.y1 + 1) / gh);
        }
        f.people = (int)kept.size();
        f.cx = (float)(main->sx / main->area + 0.5) / gw;
        f.cy = (float)(main->sy / main->area + 0.5) / gh;
        f.x0 = ax0; f.x1 = ax1; f.y0 = ay0; f.y1 = ay1;
        f.main_w = (float)(main->x1 - main->x0 + 1) / gw;
        f.main_h = (float)(main->y1 - main->y0 + 1) / gh;
        f.found = true;
        return f;
    }
    // No people: the busiest part. How much each cell differs from its neighbours, with a lean toward the middle.
    std::vector<float> busy((size_t)gw * gh, 0);
    double sum = 0;
    for (int y = 1; y < gh - 1; y++)
        for (int x = 1; x < gw - 1; x++) {
            const Cell& c = cells[(size_t)y * gw + x];
            const Cell& l = cells[(size_t)y * gw + x - 1];
            const Cell& r = cells[(size_t)y * gw + x + 1];
            const Cell& u = cells[(size_t)(y - 1) * gw + x];
            const Cell& d = cells[(size_t)(y + 1) * gw + x];
            float e = std::fabs(l.r - r.r) + std::fabs(l.g - r.g) + std::fabs(l.b - r.b) + std::fabs(u.r - d.r) + std::fabs(u.g - d.g) +
                      std::fabs(u.b - d.b);
            const float dx = (x - gw * 0.5f) / (gw * 0.5f), dy = (y - gh * 0.5f) / (gh * 0.5f);
            e *= 1.0f - 0.35f * std::min(1.0f, dx * dx + dy * dy);
            (void)c;
            busy[(size_t)y * gw + x] = e;
            sum += e;
        }
    if (sum < 1) return f;  // flat
    std::vector<float> sorted(busy);
    std::sort(sorted.begin(), sorted.end());
    const float cut = sorted[sorted.size() * 85 / 100];
    double wx = 0, wy = 0, wsum = 0;
    int bx0 = gw, by0 = gh, bx1 = 0, by1 = 0;
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
            const float e = busy[(size_t)y * gw + x];
            if (e <= cut || e <= 0) continue;
            wx += x * e; wy += y * e; wsum += e;
            bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y);
        }
    if (wsum <= 0) return f;
    f.cx = (float)(wx / wsum + 0.5) / gw;
    f.cy = (float)(wy / wsum + 0.5) / gh;
    f.x0 = (float)bx0 / gw; f.x1 = (float)(bx1 + 1) / gw; f.y0 = (float)by0 / gh; f.y1 = (float)(by1 + 1) / gh;
    f.main_w = f.x1 - f.x0;
    f.main_h = f.y1 - f.y0;
    f.found = true;
    return f;
}

}  // namespace focus
