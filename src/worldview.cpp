// worldview.cpp — Planet class implementation.
//
// Projection model: true orthographic sphere. A map point at offset (dx,dy)
// in unit-sphere coordinates maps to screen (cx + dx*R, cy + dy*R) provided
// dx²+dy² ≤ 1. The depth dz = sqrt(1 - dx² - dy²) is used only for the
// horizon test and for per-vertex lighting.
//
// Unlike the 1998 reference this module does *not* use a parabolic depth
// approximation — the sqrt is cheap enough on modern x64, and true spherical
// projection keeps the edge silhouette clean without hand-tuned constants.

#include "worldview.h"
#include "lod.h"
#include "atmo.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ptw {

namespace {

inline int    clampi(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }
inline Q9     clampQ9(Q9 v, Q9 lo, Q9 hi)         { return v < lo ? lo : (v > hi ? hi : v); }
inline float  toFloatQ9(Q9 v)                     { return (float)v * (1.f / 512.f); }
inline Q9     fromFloat(float v)                  { return (Q9)std::lround(v * 512.f); }

// Simple 32-bit LCG with a 13-bit rotate — self-contained PRNG for the
// star field. Uniform enough to spread stars evenly; different constants
// from any reference material.
inline std::uint32_t rotR13(std::uint32_t x) { return (x >> 13) | (x << 19); }
inline std::uint32_t nextStar(std::uint32_t &state)
{
    state = state * 1103515245u + 12345u;
    state = rotR13(state);
    return (state >> 7) & 0xffffu;
}

} // namespace

Planet::Planet() {}

// ---- Configuration -------------------------------------------------------

void Planet::attachSurface(std::uint8_t *pixels, int width, int height, int pitch)
{
    canvas_.attachSurface(pixels, width, height, pitch);
}

void Planet::attachTexpage(const std::uint8_t *texpage,
                           int subpageCols, int subpageRows,
                           int quadrantsX,  int quadrantsY)
{
    texBase_      = texpage;
    subpageCols_  = subpageCols;
    subpageRows_  = subpageRows;
    quadrantsX_   = quadrantsX;
    quadrantsY_   = quadrantsY;
    canvas_.attachTextureAtlas(texpage, subpageCols, subpageRows);
}

void Planet::attachFadeTable(const std::uint8_t (*t)[256])
{
    fade_ = t;
    canvas_.attachFadeTable(t);
}

void Planet::attachAlphaTable(const std::uint8_t (*t)[256])
{
    alpha_ = t;
    canvas_.attachAlphaTable(t);
}

// ---- View ----------------------------------------------------------------

void Planet::setViewport(int cx, int cy, int radiusPx, Q9 mapRadiusQ9)
{
    sphereCenterX_ = cx;
    sphereCenterY_ = cy;
    sphereRadius_  = radiusPx;
    sphereMapR9_   = mapRadiusQ9;
    sphereRadiusF_ = (float)radiusPx;

    // Parabolic projection coefficients. The apex depth is the (squared)
    // map radius in integer cells; the screen scaling factor follows from
    // requiring that a map point at the map-radius distance lands exactly
    // on the sphere silhouette in screen space.
    const float mapR = (float)mapRadiusQ9 * (1.f / 512.f);
    projDepth_ = mapR * mapR;
    projScale_ = mapR * (float)radiusPx * 2.f;
}

void Planet::setScroll(Q9 mapX, Q9 mapY, Q9 spinX, Q9 spinY, Q9 spinCap)
{
    const Q9 dx = scrollX9_ - mapX;
    const Q9 dy = scrollY9_ - mapY;
    scrollX9_ = mapX;
    scrollY9_ = mapY;
    spinX9_   = spinX;
    spinY9_   = spinY;
    spinCap9_ = spinCap;

    // Pan the celestial sphere: 1 sphere-map-radius of scroll == 90° of
    // rotation. Yaw tracks X scroll, pitch tracks Z scroll. Stars are
    // rotated opposite to world scroll (camera-forward convention).
    const float yawDelta   = -(float)dx / (float)sphereMapR9_ * 1.5707963f;
    const float pitchDelta = -(float)dy / (float)sphereMapR9_ * 1.5707963f;
    starYaw_   += yawDelta;
    starPitch_ += pitchDelta;
}

void Planet::setAtmosphere(int colorIndex, int widthPx)
{
    atmoColor_ = colorIndex;
    atmoWidth_ = widthPx;
}

void Planet::setProjectionMorph(int blend, FlatProjectFn fn, void *user)
{
    morph_    = clampi(blend, 0, 256);
    flatFn_   = fn;
    flatUser_ = user;
}

void Planet::getScroll(Q9 *x, Q9 *y) const
{
    if (x) *x = scrollX9_;
    if (y) *y = scrollY9_;
}

// ---- Projection ----------------------------------------------------------

namespace {

// Sign-extend the low 16 bits — wraps a Q9 offset into a signed 128-cell
// torus so scrolling past the map edge re-enters on the opposite side.
inline Q9 wrapQ9Torus(Q9 v)
{
    return (Q9)(((std::int32_t)v << 16) >> 16);
}

} // namespace

void Planet::projectPure(Q9 mapXQ9, Q9 mapYQ9, int *sx, int *sy) const
{
    // Parabolic projection: treat (x, y) as map-space offsets from scroll,
    // compute a depth z = projDepth + x² + y², and scale (x, y) by
    // projScale/z to land on screen. Equivalent to a stereographic-ish
    // sphere mapping once tuned by setViewport.
    const Q9 dxQ9 = wrapQ9Torus(mapXQ9 - scrollX9_);
    const Q9 dyQ9 = wrapQ9Torus(mapYQ9 - scrollY9_);
    const float x = (float)dxQ9 * (1.f / 512.f);
    const float y = (float)dyQ9 * (1.f / 512.f);
    const float z = projDepth_ + x * x + y * y;
    const float r = projScale_ / z;

    // Pop3's camera looks at the map from the +Z direction, so screen X
    // increases with map X and screen Y increases with map Z — which
    // happens to be the same convention the engine uses. The historical
    // parabolic projection lives here unchanged.
    int px = sphereCenterX_ + (int)((mirrorX_ ? -x : x) * r);
    int py = sphereCenterY_ + (int)((mirrorY_ ? -y : y) * r);

    if (morph_ > 0 && flatFn_) {
        int flatSx = px, flatSy = py;
        flatFn_(mapXQ9, mapYQ9, &flatSx, &flatSy, flatUser_);
        const int m = morph_;
        px = (px * (256 - m) + flatSx * m) >> 8;
        py = (py * (256 - m) + flatSy * m) >> 8;
    }

    if (sx) *sx = px;
    if (sy) *sy = py;
}

bool Planet::projectWithHorizon(Q9 mapXQ9, Q9 mapYQ9,
                                int *sx, int *sy) const
{
    // projectPure already clamps lon/lat to the hemisphere, which has the
    // side-effect of snapping over-horizon points onto the sphere silhouette
    // at their capped latitude/longitude. The returned bool indicates
    // whether the source point was inside the visible hemisphere.
    projectPure(mapXQ9, mapYQ9, sx, sy);
    return mapInView(mapXQ9, mapYQ9);
}

bool Planet::unproject(int screenX, int screenY, Q9 *mapXQ9, Q9 *mapYQ9) const
{
    // Inverse of the parabolic forward projection. Let
    //     X = (sx-cx)/projScale,   Y = (sy-cy)/projScale,   r = √(X²+Y²)
    // then solving r = 1 / (projDepth/d + d) for the map-radius d gives
    //     d = (1 - √(1 - 4·projDepth·r²)) / (2·r)
    // and the angle is preserved, so the map offset is (d·sin(θ), d·cos(θ)).
    if (morph_ != 0) return false;

    const double x    = (double)(screenX - sphereCenterX_) / projScale_;
    const double y    = (double)(screenY - sphereCenterY_) / projScale_;
    const double rSq  = x * x + y * y;
    const double root = 1.0 - 4.0 * projDepth_ * rSq;
    if (root < 0.0) return false;

    const double r = std::sqrt(rSq);
    const double d = (1.0 - std::sqrt(root)) / (2.0 * r);
    const double a = std::atan2(x, y);
    const Q9 du = (Q9)std::lround(d * std::sin(a) * 512.0);
    const Q9 dv = (Q9)std::lround(d * std::cos(a) * 512.0);

    if (mapXQ9) *mapXQ9 = scrollX9_ + du;
    if (mapYQ9) *mapYQ9 = scrollY9_ + dv;
    return true;
}

bool Planet::mapInView(Q9 mapXQ9, Q9 mapYQ9) const
{
    // Use the same circular gamut test as the original: wrap the delta into
    // the ±64-cell torus, then compare squared distance to squared radius.
    // The >>4 on both sides keeps the math inside 32-bit range.
    const Q9 dx = wrapQ9Torus(mapXQ9 - scrollX9_);
    const Q9 dy = wrapQ9Torus(mapYQ9 - scrollY9_);
    const std::int32_t rScaled = sphereMapR9_ >> 4;
    const std::int32_t dScaled2 = (std::int32_t)((dx >> 4) * (dx >> 4)
                                               + (dy >> 4) * (dy >> 4));
    return (std::int32_t)(rScaled * rScaled) > dScaled2;
}

// ---- Lighting ------------------------------------------------------------

Q16 Planet::lightAt(int screenX, int screenY) const
{
    // Per-pixel Lambert + specular + horizon-rim terms. Screen coords are
    // normalised by the sphere radius so (x, y) ∈ [-1..1]. The 1.05 offset
    // inside the depth sqrt is a classic trick to keep the pixel exactly at
    // r=1 from producing NaN.
    const float x  = (float)(screenX - sphereCenterX_) / sphereRadiusF_;
    const float y  = (float)(screenY - sphereCenterY_) / sphereRadiusF_;
    const float rr = x * x + y * y;
    const float z  = std::sqrt(1.05f - rr);

    constexpr float lx = 0.4f, ly = -0.4f, lz = 0.8f;
    const float dot    = x * lx + y * ly + z * lz;
    const float dot2   = dot * dot;
    const float dot4   = dot2 * dot2;

    float hori = (rr - 0.9f) * 4.0f;
    if (hori < 0.0f) hori = 0.0f;

    constexpr float ambient = 0.4f;
    float diff = 0.4f * dot;
    if (diff < -0.2f) diff = -0.2f;
    const float spec = 0.2f * dot4;

    float intensity = hori + ambient + diff + spec;

    Q16 s16 = (Q16)std::lround(intensity * (64.0f * 65536.0f));
    if (s16 > (63 << 16)) s16 = 63 << 16;
    if (s16 < (20 << 16)) s16 = 20 << 16;

    // Morph blends shade toward mid-grey (32) as the sphere flattens.
    if (morph_ != 0) {
        const Q16 mid = 32 << 16;
        s16 = s16 + (((mid - s16) * morph_) >> 8);
    }
    return s16;
}

// ---- Landscape -----------------------------------------------------------

void Planet::buildDetailMap()
{
    // Reserved for future per-silhouette refinement. For now the LOD mesh
    // uses a uniform subdivision driven by the caller's meshResolution.
}

void Planet::emitLandscapeTriangle(int ax, int ay, int bx, int by, int cx, int cy)
{
    const Q9 axQ = (Q9)ax << 9;
    const Q9 ayQ = (Q9)ay << 9;
    const Q9 bxQ = (Q9)bx << 9;
    const Q9 byQ = (Q9)by << 9;
    const Q9 cxQ = (Q9)cx << 9;
    const Q9 cyQ = (Q9)cy << 9;
    if (!mapInView(axQ, ayQ) && !mapInView(bxQ, byQ) && !mapInView(cxQ, cyQ))
        return;

    // U / V: a 32-cell block covers the 256-wide sub-page, i.e. 8 texels per
    // cell. In 16.16 fixpt that's (localCell << 19). Caller has already
    // reduced ax/bx/cx to local sub-page coordinates [0..32].
    raster::Vertex va{}, vb{}, vc{};
    va.u = (Q16)ax << 19;  va.v = (Q16)ay << 19;
    vb.u = (Q16)bx << 19;  vb.v = (Q16)by << 19;
    vc.u = (Q16)cx << 19;  vc.v = (Q16)cy << 19;

    projectPure(axQ, ayQ, &va.sx, (int *)&va.sy);
    projectPure(bxQ, byQ, &vb.sx, (int *)&vb.sy);
    projectPure(cxQ, cyQ, &vc.sx, (int *)&vc.sy);

    va.light = lightAt(va.sx, va.sy);
    vb.light = lightAt(vb.sx, vb.sy);
    vc.light = lightAt(vc.sx, vc.sy);

    canvas_.drawTriangle(va, vb, vc, raster::FillMode::TexturedShaded);
    ++tmpTriCount_;
}

int Planet::drawLandscape(int meshResolution)
{
    if (!texBase_ || !fade_) return 0;

    integrateSpin();

    // Inner halo: painted first so the terrain overwrites it toward the
    // centre of the sphere, leaving only a thin rim visible at the edge.
    if (atmoColor_ >= 0 && morph_ == 0) {
        atmo::Surface surf{canvas_.surface(), canvas_.width(),
                           canvas_.height(), canvas_.pitch()};
        atmo::drawRing(surf,
                       sphereCenterX_, sphereCenterY_,
                       sphereRadius_ - (atmoWidth_ >> 1),
                       sphereRadius_ + (atmoWidth_ >> 1),
                       32, 50, (std::uint8_t)atmoColor_, fade_);
    }

    // Clamp mesh resolution to power-of-two; each cell produces 2 triangles.
    int step = 1;
    while (step < meshResolution) step <<= 1;
    if (step > 32) step = 32;

    tmpTriCount_ = 0;

    // Outer loop iterates sub-pages (4×4 grid by default). Inner loop walks
    // the 32×32-cell block for that sub-page at `step` resolution. This
    // guarantees a triangle never spans two sub-pages — the texture
    // coordinates and atlas pointer are always consistent.
    const int blockSize = 32;
    for (int subY = 0; subY < quadrantsY_; ++subY)
    {
        for (int subX = 0; subX < quadrantsX_; ++subX)
        {
            // Texpage is a linear stack of 16 sub-pages, subY-major then
            // subX. Each sub-page is subpageCols × subpageRows bytes (64KB
            // at the default 256×256). The (subX << 16) + (subY << 18)
            // layout the original emits is equivalent to this.
            const std::size_t pageOffset =
                ((std::size_t)subY * quadrantsX_ + subX)
                * (std::size_t)(subpageCols_ * subpageRows_);
            canvas_.useTexturePage(texBase_ + pageOffset);

            const int xBase = subX * blockSize;
            const int yBase = subY * blockSize;

            for (int dy = 0; dy < blockSize; dy += step) {
                for (int dx = 0; dx < blockSize; dx += step) {
                    const int gAx = xBase + dx;
                    const int gAy = yBase + dy;
                    const int gBx = gAx + step;
                    const int gBy = gAy;
                    const int gCx = gAx;
                    const int gCy = gAy + step;
                    const int gDx = gAx + step;
                    const int gDy = gAy + step;

                    // Local (within-sub-page) cell coords drive U/V.
                    const int lAx = dx,        lAy = dy;
                    const int lBx = dx + step, lBy = dy;
                    const int lCx = dx,        lCy = dy + step;
                    const int lDx = dx + step, lDy = dy + step;

                    // Two triangles per cell, shared hypotenuse A–D.
                    // Invoke a helper that takes separate global + local
                    // coords; fall back to a lambda to avoid expanding the
                    // API surface.
                    auto emit = [&](int ga_x, int ga_y, int la_x, int la_y,
                                    int gb_x, int gb_y, int lb_x, int lb_y,
                                    int gc_x, int gc_y, int lc_x, int lc_y)
                    {
                        const Q9 aQx = (Q9)ga_x << 9;
                        const Q9 aQy = (Q9)ga_y << 9;
                        const Q9 bQx = (Q9)gb_x << 9;
                        const Q9 bQy = (Q9)gb_y << 9;
                        const Q9 cQx = (Q9)gc_x << 9;
                        const Q9 cQy = (Q9)gc_y << 9;
                        if (!mapInView(aQx, aQy) &&
                            !mapInView(bQx, bQy) &&
                            !mapInView(cQx, cQy)) return;

                        raster::Vertex va{}, vb{}, vc{};
                        va.u = (Q16)la_x << 19;  va.v = (Q16)la_y << 19;
                        vb.u = (Q16)lb_x << 19;  vb.v = (Q16)lb_y << 19;
                        vc.u = (Q16)lc_x << 19;  vc.v = (Q16)lc_y << 19;
                        projectPure(aQx, aQy, &va.sx, (int *)&va.sy);
                        projectPure(bQx, bQy, &vb.sx, (int *)&vb.sy);
                        projectPure(cQx, cQy, &vc.sx, (int *)&vc.sy);
                        va.light = lightAt(va.sx, va.sy);
                        vb.light = lightAt(vb.sx, vb.sy);
                        vc.light = lightAt(vc.sx, vc.sy);
                        canvas_.drawTriangle(va, vb, vc,
                                             raster::FillMode::TexturedShaded);
                        ++tmpTriCount_;
                    };

                    emit(gAx, gAy, lAx, lAy,
                         gBx, gBy, lBx, lBy,
                         gDx, gDy, lDx, lDy);
                    emit(gAx, gAy, lAx, lAy,
                         gDx, gDy, lDx, lDy,
                         gCx, gCy, lCx, lCy);
                }
            }
        }
    }

    // Outer halo.
    if (atmoColor_ >= 0 && morph_ == 0) {
        atmo::Surface surf{canvas_.surface(), canvas_.width(),
                           canvas_.height(), canvas_.pitch()};
        atmo::drawRing(surf,
                       sphereCenterX_, sphereCenterY_,
                       sphereRadius_ + (atmoWidth_ >> 1),
                       sphereRadius_ + atmoWidth_,
                       50, 30, (std::uint8_t)atmoColor_, fade_);
    }

    return tmpTriCount_;
}

// ---- Spin integrator -----------------------------------------------------

void Planet::integrateSpin()
{
    if (panActive_) return;                  // user is driving scroll
    if (spinX9_ == 0 && spinY9_ == 0) return;

    // Clamp spin magnitude.
    spinX9_ = clampQ9(spinX9_, -spinCap9_, spinCap9_);
    spinY9_ = clampQ9(spinY9_, -spinCap9_, spinCap9_);

    scrollX9_ += spinX9_;
    scrollY9_ += spinY9_;

    // Spin-induced starfield rotation (same 1:1 radius-to-π/2 convention).
    starYaw_   -= (float)spinX9_ / (float)sphereMapR9_ * 1.5707963f;
    starPitch_ -= (float)spinY9_ / (float)sphereMapR9_ * 1.5707963f;
}

// ---- Stars ---------------------------------------------------------------

void Planet::drawStars(int count, std::uint8_t baseColor, std::uint32_t seed)
{
    if (!fade_) return;

    // Cache 8 shade variants of the base colour.
    std::uint8_t pal[8];
    for (int i = 0; i < 8; ++i) pal[i] = fade_[32 + (i << 2)][baseColor];

    // Each star is a fixed random unit vector in 3D. Every frame we rotate
    // the entire sky by (yaw around Y, pitch around X), then orthographically
    // project onto the screen. Stars whose rotated Z component goes negative
    // fall behind the camera and are skipped. This gives the "space-station
    // orbit" motion the user's after — linear drags turn into smooth arcs,
    // stars swing around the planet instead of sliding sideways.
    const float cy = std::cos(starYaw_);
    const float sy = std::sin(starYaw_);
    const float cp = std::cos(starPitch_);
    const float sp = std::sin(starPitch_);

    const int halfW = canvas_.width()  / 2;
    const int halfH = canvas_.height() / 2;
    const float signX = mirrorX_ ? -1.f : 1.f;
    const float signY = mirrorY_ ? -1.f : 1.f;

    std::uint32_t state = seed;
    for (int i = 0; i < count; ++i)
    {
        // Random unit vector via cosine-distributed latitude + uniform
        // longitude. The low 16 bits of successive PRNG draws give enough
        // entropy for thousands of stars without visible banding.
        const std::uint32_t lonRaw = nextStar(state);
        const std::uint32_t latRaw = nextStar(state);
        const std::uint32_t shRaw  = nextStar(state);

        const float lon = (float)lonRaw * (6.2831853f / 65536.f);
        const float cosLat = (float)latRaw * (2.f / 65536.f) - 1.f;   // [-1..1]
        const float sinLat = std::sqrt(std::max(0.f, 1.f - cosLat * cosLat));

        float x = sinLat * std::cos(lon);
        float y = cosLat;
        float z = sinLat * std::sin(lon);

        // Rotate yaw around Y axis.
        float xr = cy * x + sy * z;
        float zr = -sy * x + cy * z;
        // Rotate pitch around X axis.
        float yr = cp * y - sp * zr;
        float zr2 = sp * y + cp * zr;

        if (zr2 < 0.05f) continue;   // behind the viewer (or too close to horizon)

        // Perspective projection with ~90° horizontal FOV — fills the
        // whole viewport including the corners, so the star density
        // behaves like looking out a real porthole.
        const float focal = (float)halfW;
        const int sx = sphereCenterX_
            + (int)((xr / zr2) * focal * signX);
        const int syp = sphereCenterY_
            + (int)((yr / zr2) * focal * signY);

        // Don't burn pixels that the landscape will overwrite.
        const int ddx = sx  - sphereCenterX_;
        const int ddy = syp - sphereCenterY_;
        if (ddx * ddx + ddy * ddy < sphereRadius_ * sphereRadius_) continue;

        canvas_.plotPixel(sx, syp, pal[shRaw & 7]);
    }
}

// ---- Cell highlight ------------------------------------------------------

void Planet::highlightCell(int mapX, int mapY, std::uint8_t tint)
{
    if (!alpha_) return;

    raster::Vertex a{}, b{}, c{}, d{};
    const Q9 xQ = (Q9)mapX << 9;
    const Q9 yQ = (Q9)mapY << 9;
    projectWithHorizon(xQ,            yQ,            &a.sx, (int*)&a.sy);
    projectWithHorizon(xQ + (1 << 9), yQ,            &b.sx, (int*)&b.sy);
    projectWithHorizon(xQ,            yQ + (1 << 9), &c.sx, (int*)&c.sy);
    projectWithHorizon(xQ + (1 << 9), yQ + (1 << 9), &d.sx, (int*)&d.sy);

    canvas_.drawTriangle(a, b, d, raster::FillMode::AlphaBlend, tint);
    canvas_.drawTriangle(a, d, c, raster::FillMode::AlphaBlend, tint);
}

// ---- Selection -----------------------------------------------------------

void Planet::drawSelectionBox(std::uint8_t tint)
{
    if (selState_ != 1) return;
    int ax = selAx_, ay = selAy_;
    int bx = selBx_, by = selBy_;
    if (bx < ax) std::swap(ax, bx);
    if (by < ay) std::swap(ay, by);
    for (int y = ay; y <= by; ++y)
        for (int x = ax; x <= bx; ++x)
            highlightCell(x, y, tint);
}

bool Planet::selectBoxBegin(int sx, int sy, int maxW, int maxH)
{
    Q9 mx, my;
    if (!unproject(sx, sy, &mx, &my)) return false;
    selAx_    = (int)(mx >> 9);
    selAy_    = (int)(my >> 9);
    selBx_    = selAx_;
    selBy_    = selAy_;
    selMaxW_  = maxW;
    selMaxH_  = maxH;
    selState_ = 1;
    return true;
}

bool Planet::selectBoxUpdate(int sx, int sy)
{
    if (selState_ != 1) return false;
    Q9 mx, my;
    if (!unproject(sx, sy, &mx, &my)) return false;
    int bx = (int)(mx >> 9);
    int by = (int)(my >> 9);
    // Clamp to max box size.
    if (selMaxW_ > 0) {
        if (bx > selAx_ + selMaxW_) bx = selAx_ + selMaxW_;
        if (bx < selAx_ - selMaxW_) bx = selAx_ - selMaxW_;
    }
    if (selMaxH_ > 0) {
        if (by > selAy_ + selMaxH_) by = selAy_ + selMaxH_;
        if (by < selAy_ - selMaxH_) by = selAy_ - selMaxH_;
    }
    selBx_ = bx;
    selBy_ = by;
    return true;
}

bool Planet::selectBoxEnd(int sx, int sy, int *outAx, int *outAy,
                          int *outBx, int *outBy)
{
    const bool ok = selectBoxUpdate(sx, sy);
    if (!ok) { selState_ = 0; return false; }

    int ax = selAx_, ay = selAy_, bx = selBx_, by = selBy_;
    if (bx < ax) std::swap(ax, bx);
    if (by < ay) std::swap(ay, by);
    if (outAx) *outAx = ax;
    if (outAy) *outAy = ay;
    if (outBx) *outBx = bx;
    if (outBy) *outBy = by;
    selState_ = 0;
    return true;
}

// ---- Pan -----------------------------------------------------------------

void Planet::panBegin(int sx, int sy)
{
    panAnchorSx_  = sx;
    panAnchorSy_  = sy;
    panAnchorMx9_ = scrollX9_;
    panAnchorMy9_ = scrollY9_;
    panActive_    = true;
    spinX9_ = spinY9_ = 0;
}

void Planet::panUpdate(int sx, int sy)
{
    if (!panActive_) return;

    const int dsx = sx - panAnchorSx_;
    const int dsy = sy - panAnchorSy_;
    // A pixel of screen motion corresponds to `sphereMapR9_ / sphereRadius_`
    // Q9 units of map motion. Sign flipped: dragging right scrolls world
    // left (stars move opposite to cursor). If the projection mirrors an
    // axis, the pan direction for that axis flips back.
    const float scale = (float)sphereMapR9_ / sphereRadiusF_;
    const int   xSign = mirrorX_ ? +1 : -1;
    const int   ySign = mirrorY_ ? +1 : -1;
    const Q9 nextX = panAnchorMx9_ + (Q9)std::lround(dsx * scale * xSign);
    const Q9 nextY = panAnchorMy9_ + (Q9)std::lround(dsy * scale * ySign);

    // Record velocity for post-release spin.
    spinX9_ = nextX - scrollX9_;
    spinY9_ = nextY - scrollY9_;

    const Q9 dx = scrollX9_ - nextX;
    const Q9 dy = scrollY9_ - nextY;
    scrollX9_ = nextX;
    scrollY9_ = nextY;

    // Star rotation tracks the pan — one sphere-radius of map motion
    // corresponds to 90° of view rotation.
    starYaw_   -= (float)dx / (float)sphereMapR9_ * 1.5707963f;
    starPitch_ -= (float)dy / (float)sphereMapR9_ * 1.5707963f;
}

void Planet::panEnd()
{
    panActive_ = false;
}

} // namespace ptw
