// atmo.cpp — atmosphere ring / disc renderer.
//
// Uses standard circle-scanline geometry: for each row, compute the
// horizontal half-chord against both the inner and outer radius, then fill
// two annular spans (left arc, right arc). Each output pixel goes through
// fade[shade][tint], where shade interpolates linearly between the inner
// and outer ring edges based on radial distance.

#include "atmo.h"

#include <cmath>

namespace ptw::atmo {

namespace {

inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline void blendSpan(std::uint8_t *row, int x0, int x1, int clipW,
                      int shadeNum, int shadeDen, int shadeBias,
                      std::uint8_t tint, const std::uint8_t (*fade)[256])
{
    if (x1 < x0) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= clipW) x1 = clipW - 1;
    if (x1 < x0) return;

    // Shade varies with horizontal distance from span centre; simple linear
    // remap is enough for the narrow rings used by the atmosphere effect.
    for (int x = x0; x <= x1; ++x) {
        int s = shadeBias + (shadeNum * (x - x0)) / (shadeDen ? shadeDen : 1);
        s = clampi(s, 0, 63);
        row[x] = fade[s][tint];
    }
}

void drawAnnulus(const Surface &dst,
                 int cx, int cy,
                 int inner, int outer,
                 int shadeIn, int shadeOut,
                 std::uint8_t tint,
                 const std::uint8_t (*fade)[256])
{
    if (!dst.pixels || !fade || outer <= 0) return;
    if (inner < 0) inner = 0;
    if (inner >= outer && outer > 0) return;

    const int outerSq = outer * outer;
    const int innerSq = inner * inner;

    const int y0 = cy - outer;
    const int y1 = cy + outer;

    for (int y = y0; y <= y1; ++y) {
        if (y < 0 || y >= dst.height) continue;
        const int dy = y - cy;
        const int dy2 = dy * dy;
        if (dy2 > outerSq) continue;

        const int outerHalf = (int)std::sqrt((double)(outerSq - dy2));
        int innerHalf = 0;
        bool hasHole = (dy2 < innerSq);
        if (hasHole) innerHalf = (int)std::sqrt((double)(innerSq - dy2));

        std::uint8_t *row = dst.pixels + (std::size_t)y * dst.pitch;

        if (!hasHole) {
            // Single contiguous span (above/below the inner hole, or
            // the whole row when inner==0).
            const int xL = cx - outerHalf;
            const int xR = cx + outerHalf;
            const int shadeDen = outerHalf + 1;
            const int shadeNum = shadeOut - shadeIn;
            // Walk left-edge to centre (shadeIn at centre, shadeOut at rim).
            // Simplify: just do linear across the whole span shadeIn..shadeIn.
            const int leftPartEnd  = cx - 1;
            const int rightPartEnd = xR;
            // Left part: rim (shadeOut) at xL, centre (shadeIn) at cx.
            blendSpan(row, xL, leftPartEnd, dst.width,
                      shadeIn - shadeOut, shadeDen, shadeOut, tint, fade);
            // Right part: centre (shadeIn) at cx, rim (shadeOut) at xR.
            blendSpan(row, cx, rightPartEnd, dst.width,
                      shadeOut - shadeIn, shadeDen, shadeIn, tint, fade);
        } else {
            // Two arcs: [cx-outerHalf .. cx-innerHalf-1] and [cx+innerHalf+1 .. cx+outerHalf].
            const int xLo = cx - outerHalf;
            const int xLi = cx - innerHalf - 1;
            const int xRi = cx + innerHalf + 1;
            const int xRo = cx + outerHalf;
            const int width = outerHalf - innerHalf;
            const int shadeNum = shadeIn - shadeOut; // from rim to inner
            blendSpan(row, xLo, xLi, dst.width,
                      shadeNum, width ? width : 1, shadeOut, tint, fade);
            blendSpan(row, xRi, xRo, dst.width,
                      -shadeNum, width ? width : 1, shadeIn, tint, fade);
        }
    }
}

} // namespace

void drawRing(const Surface &dst, int cx, int cy,
              int inner, int outer,
              int shadeIn, int shadeOut,
              std::uint8_t tint,
              const std::uint8_t (*fade)[256])
{
    drawAnnulus(dst, cx, cy, inner, outer, shadeIn, shadeOut, tint, fade);
}

void drawDisc(const Surface &dst, int cx, int cy, int radius,
              int shadeCentre, int shadeEdge,
              std::uint8_t tint,
              const std::uint8_t (*fade)[256])
{
    drawAnnulus(dst, cx, cy, 0, radius, shadeCentre, shadeEdge, tint, fade);
}

} // namespace ptw::atmo
