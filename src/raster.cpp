// raster.cpp — implementation of the 8bpp scanline rasterizer.

#include "raster.h"

#include <algorithm>
#include <cstring>

namespace ptw::raster {

namespace {

struct EdgeDda
{
    std::int32_t x;   // 16.16 fixpt
    std::int32_t u;   // 16.16 fixpt
    std::int32_t v;   // 16.16 fixpt
    std::int32_t l;   // 16.16 fixpt shade
    std::int32_t dx, du, dv, dl;
};

// Initialise a DDA walking from vertex p to vertex q, one step per scanline.
// Degenerate (same Y) edges collapse to zero-delta.
inline void edgeSetup(const Vertex &p, const Vertex &q, EdgeDda &e)
{
    const std::int32_t dy = q.sy - p.sy;
    e.x = p.sx << 16;
    e.u = p.u;
    e.v = p.v;
    e.l = p.light;
    if (dy <= 0) {
        e.dx = e.du = e.dv = e.dl = 0;
    } else {
        e.dx = ((q.sx - p.sx) << 16) / dy;
        e.du =  (q.u  - p.u ) / dy;
        e.dv =  (q.v  - p.v ) / dy;
        e.dl =  (q.light - p.light) / dy;
    }
}

inline void edgeAdvance(EdgeDda &e, int steps)
{
    e.x += e.dx * steps;
    e.u += e.du * steps;
    e.v += e.dv * steps;
    e.l += e.dl * steps;
}

inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int ilog2_round(int v)
{
    int n = 0;
    while ((1 << n) < v) ++n;
    return n;
}

} // namespace

void Canvas::attachSurface(std::uint8_t *pixels, int w, int h, int p)
{
    pixels_ = pixels;
    width_  = w;
    height_ = h;
    pitch_  = p;
}

void Canvas::attachTextureAtlas(const std::uint8_t *base, int cols, int rows)
{
    texBase_     = base;
    texCols_     = cols;
    texRows_     = rows;
    texRowShift_ = ilog2_round(cols);
}

void Canvas::attachFadeTable(const std::uint8_t (*tbl)[256])
{
    fade_ = tbl;
}

void Canvas::attachAlphaTable(const std::uint8_t (*tbl)[256])
{
    alpha_ = tbl;
}

void Canvas::plotPixel(int x, int y, std::uint8_t color)
{
    if (x < 0 || y < 0 || x >= width_ || y >= height_ || !pixels_) return;
    pixels_[y * pitch_ + x] = color;
}

void Canvas::drawTriangle(Vertex a, Vertex b, Vertex c, FillMode mode,
                          std::uint8_t tint)
{
    if (!pixels_ || pitch_ <= 0) return;

    // Y-sort so a.sy ≤ b.sy ≤ c.sy.
    if (b.sy < a.sy) std::swap(a, b);
    if (c.sy < b.sy) std::swap(b, c);
    if (b.sy < a.sy) std::swap(a, b);

    if (c.sy == a.sy) return;     // zero-height triangle

    const int tileMaskU = texCols_ - 1;
    const int tileMaskV = texRows_ - 1;

    EdgeDda longEdge;
    edgeSetup(a, c, longEdge);

    for (int half = 0; half < 2; ++half)
    {
        int y0, y1;
        EdgeDda shortEdge;
        if (half == 0) {
            if (a.sy == b.sy) { continue; }
            edgeSetup(a, b, shortEdge);
            y0 = a.sy;
            y1 = b.sy;
        } else {
            if (b.sy == c.sy) { continue; }
            edgeSetup(b, c, shortEdge);
            y0 = b.sy;
            y1 = c.sy;
        }

        // Vertical clip of the span.
        if (y0 < 0) {
            const int skip = -y0;
            edgeAdvance(longEdge,  skip);
            edgeAdvance(shortEdge, skip);
            y0 = 0;
        }
        if (y1 > height_) y1 = height_;

        for (int y = y0; y < y1; ++y)
        {
            // Pick the left/right edge for this scanline.
            std::int32_t lx = longEdge.x  >> 16;
            std::int32_t rx = shortEdge.x >> 16;
            std::int32_t lu = longEdge.u,  ru = shortEdge.u;
            std::int32_t lv = longEdge.v,  rv = shortEdge.v;
            std::int32_t ll = longEdge.l,  rl = shortEdge.l;
            if (rx < lx) {
                std::swap(lx, rx);
                std::swap(lu, ru);
                std::swap(lv, rv);
                std::swap(ll, rl);
            }

            const int span = (int)(rx - lx);
            if (span > 0)
            {
                const std::int32_t du = (ru - lu) / span;
                const std::int32_t dv = (rv - lv) / span;
                const std::int32_t dl = (rl - ll) / span;

                int x0 = (int)lx;
                int x1 = (int)rx;

                if (x0 < 0) {
                    const int skip = -x0;
                    lu += du * skip;
                    lv += dv * skip;
                    ll += dl * skip;
                    x0 = 0;
                }
                if (x1 > width_) x1 = width_;

                std::uint8_t *row = pixels_ + (std::size_t)y * pitch_ + x0;

                switch (mode)
                {
                case FillMode::Solid:
                {
                    std::memset(row, tint, (std::size_t)(x1 - x0));
                    break;
                }
                case FillMode::SolidShaded:
                {
                    const std::uint8_t *pal = fade_ ? fade_[0] : nullptr;
                    for (int x = x0; x < x1; ++x) {
                        int s = clampi((int)(ll >> 16), 0, 63);
                        *row++ = fade_ ? fade_[s][tint] : tint;
                        ll += dl;
                        (void)pal;
                    }
                    break;
                }
                case FillMode::Textured:
                {
                    if (!texBase_) { row += (x1 - x0); break; }
                    for (int x = x0; x < x1; ++x) {
                        // Clamp (not wrap) so a vertex that lands exactly
                        // on the sub-page's right/bottom edge samples the
                        // last texel instead of wrapping to the opposite
                        // side — avoids the 1-pixel seam at every 32-cell
                        // block boundary.
                        int tx = lu >> 16;
                        int ty = lv >> 16;
                        if (tx < 0) tx = 0; else if (tx > tileMaskU) tx = tileMaskU;
                        if (ty < 0) ty = 0; else if (ty > tileMaskV) ty = tileMaskV;
                        std::uint8_t s = texBase_[(ty << texRowShift_) | tx];
                        if (s) *row = s;
                        ++row;
                        lu += du;
                        lv += dv;
                    }
                    break;
                }
                case FillMode::TexturedShaded:
                {
                    if (!texBase_ || !fade_) { row += (x1 - x0); break; }
                    for (int x = x0; x < x1; ++x) {
                        int tx = lu >> 16;
                        int ty = lv >> 16;
                        if (tx < 0) tx = 0; else if (tx > tileMaskU) tx = tileMaskU;
                        if (ty < 0) ty = 0; else if (ty > tileMaskV) ty = tileMaskV;
                        std::uint8_t s = texBase_[(ty << texRowShift_) | tx];
                        if (s) {
                            int lvl = clampi((int)(ll >> 16), 0, 63);
                            *row = fade_[lvl][s];
                        }
                        ++row;
                        lu += du;
                        lv += dv;
                        ll += dl;
                    }
                    break;
                }
                case FillMode::AlphaBlend:
                {
                    if (!alpha_) { row += (x1 - x0); break; }
                    const std::uint8_t *lut = alpha_[tint];
                    for (int x = x0; x < x1; ++x) {
                        *row = lut[*row];
                        ++row;
                    }
                    break;
                }
                }
            }

            longEdge.x  += longEdge.dx;
            longEdge.u  += longEdge.du;
            longEdge.v  += longEdge.dv;
            longEdge.l  += longEdge.dl;
            shortEdge.x += shortEdge.dx;
            shortEdge.u += shortEdge.du;
            shortEdge.v += shortEdge.dv;
            shortEdge.l += shortEdge.dl;
        }
    }
}

} // namespace ptw::raster
