// worldview.h — public API of the PopTB_World planet renderer.
//
// Renders a scrolling, mouse-pannable planetary sphere into an 8bpp
// palette-indexed framebuffer with optional atmosphere halo, star field and
// selection rectangle. All inputs are supplied explicitly by the caller —
// the module owns no globals.

#pragma once

#include "raster.h"

#include <cstdint>

namespace ptw {

// Fixed-point convenience aliases — the engine natively works in 1/512-cell
// units (Q9) for scroll/projection math and 1/65536 for shade/UV.
using Q9  = std::int32_t;    // 9 fractional bits (1 map cell = 512)
using Q16 = std::int32_t;    // 16 fractional bits

// Optional "flat mode" projection callback. When the morph blend is > 0 the
// sphere projection is mixed toward the output of this function. Receives
// map coords in Q9; writes screen pixels.
using FlatProjectFn = void (*)(Q9 mapXQ9, Q9 mapYQ9,
                               int *outScreenX, int *outScreenY,
                               void *userData);

class Planet
{
public:
    Planet();

    // ---- Configuration ---------------------------------------------------

    // Attach the 8bpp output surface (call on resize).
    void attachSurface(std::uint8_t *pixels, int width, int height, int pitch);

    // Attach the landscape texpage. `subpageRows` gives the vertical size of
    // a single sub-page (default 256). The full texpage is expected to be
    // 256 wide and `subpageRows * quadrantsY` tall, laid out as a grid of
    // (quadrantsX × quadrantsY) sub-pages. Pop3 uses a 4×4 layout giving a
    // 256×4096 buffer.
    void attachTexpage(const std::uint8_t *texpage,
                       int subpageCols  = 256,
                       int subpageRows  = 256,
                       int quadrantsX   = 4,
                       int quadrantsY   = 4);

    // Attach the palette tables.
    void attachFadeTable (const std::uint8_t (*fadeTable) [256]);
    void attachAlphaTable(const std::uint8_t (*alphaTable)[256]);

    // ---- View geometry ---------------------------------------------------

    // Place the sphere within the surface.
    void setViewport(int centerX, int centerY,
                     int sphereRadiusPx,
                     Q9  sphereMapRadiusQ9);

    // Map-space scroll. `spin` is velocity in Q9/frame; `spinCap` clamps it.
    void setScroll(Q9 mapXQ9, Q9 mapYQ9,
                   Q9 spinXQ9 = 0, Q9 spinYQ9 = 0,
                   Q9 spinCapQ9 = 4 << 9);

    void setAtmosphere(int colorIndex, int widthPx);

    // `blend` in [0..256]: 0 = pure sphere, 256 = pure `flatFn` output.
    void setProjectionMorph(int blend, FlatProjectFn flatFn, void *userData);

    // ---- Queries ---------------------------------------------------------

    void getScroll(Q9 *mapXQ9, Q9 *mapYQ9) const;

    int  sphereRadiusPx()        const { return sphereRadius_; }
    Q9   sphereMapRadiusQ9()     const { return sphereMapR9_; }

    // ---- Per-frame rendering --------------------------------------------

    // Call stars first (they write individual pixels).
    void drawStars(int count, std::uint8_t baseColor,
                   std::uint32_t seed = 0x075bcd15);

    // Returns number of triangles emitted.
    int  drawLandscape(int meshResolution = 8);

    // Call between drawLandscape and drawSelection to tint individual cells.
    void highlightCell(int mapX, int mapY, std::uint8_t tint);

    void drawSelectionBox(std::uint8_t tint);

    // ---- Map ↔ screen projection ----------------------------------------

    // Returns true if the map point is on the visible hemisphere. When over
    // the horizon the result is clamped to the sphere silhouette.
    bool projectWithHorizon(Q9 mapXQ9, Q9 mapYQ9,
                            int *screenX, int *screenY) const;

    // Pure spherical projection — no horizon clamp.
    void projectPure       (Q9 mapXQ9, Q9 mapYQ9,
                            int *screenX, int *screenY) const;

    // Screen point → map cell (Q9). Returns false if the point is outside
    // the sphere silhouette.
    bool unproject         (int screenX, int screenY,
                            Q9 *mapXQ9, Q9 *mapYQ9) const;

    // ---- Right-mouse-drag pan --------------------------------------------

    void panBegin (int screenX, int screenY);
    void panUpdate(int screenX, int screenY);
    void panEnd();

    // ---- Rectangular selection drag -------------------------------------

    bool selectBoxBegin (int screenX, int screenY,
                         int maxWidth, int maxHeight);
    bool selectBoxUpdate(int screenX, int screenY);
    bool selectBoxEnd   (int screenX, int screenY,
                         int *outAx, int *outAy, int *outBx, int *outBy);

private:
    // Populate the LOD enable-bit map with the subdivision pattern for the
    // current view (silhouette cells get higher detail).
    void buildDetailMap();

    // Emit a single landscape triangle through the rasterizer.
    void emitLandscapeTriangle(int ax, int ay,
                               int bx, int by,
                               int cx, int cy);

    // Perspective / lighting helpers.
    Q16 lightAt(int screenX, int screenY) const;
    bool mapInView(Q9 mapXQ9, Q9 mapYQ9) const;

    // Apply per-frame mouse pan / spin to the scroll position.
    void integrateSpin();

    // ---- State ----------------------------------------------------------

    raster::Canvas canvas_;

    int  sphereCenterX_ = 0;
    int  sphereCenterY_ = 0;
    int  sphereRadius_  = 0;
    Q9   sphereMapR9_   = 0;
    float sphereRadiusF_ = 1.f;     // cached float for lighting math

    // Parabolic projection coefficients derived from (mapRadius, pixRadius).
    // screen = centre + (x, y) * projScale / (projDepth + x² + y²)
    // where (x, y) are map-cell offsets from scroll, in integer units.
    float projDepth_ = 1.f;         // z-offset of the parabola apex
    float projScale_ = 1.f;         // overall screen scaling factor

    Q9   scrollX9_ = 0;
    Q9   scrollY9_ = 0;
    Q9   spinX9_   = 0;
    Q9   spinY9_   = 0;
    Q9   spinCap9_ = 4 << 9;

    // Accumulated starfield orientation in radians. Each star is a random
    // unit vector that gets yaw+pitch-rotated at draw time, giving an
    // orbital "space station" view of the sky as the player pans.
    float starYaw_   = 0.f;
    float starPitch_ = 0.f;

    int  atmoColor_ = -1;
    int  atmoWidth_ = 0;
public:
    // Optional left/right or top/bottom mirror of the rendered sphere, to
    // match whatever axis convention the texpage/scroll caller uses.
    void setMirrorX(bool m) { mirrorX_ = m; }
    void setMirrorY(bool m) { mirrorY_ = m; }
    bool mirrorX() const    { return mirrorX_; }
    bool mirrorY() const    { return mirrorY_; }
private:
    bool mirrorX_ = false;
    bool mirrorY_ = true;

    int           morph_       = 0;
    FlatProjectFn flatFn_      = nullptr;
    void         *flatUser_    = nullptr;

    // Texture layout.
    const std::uint8_t *texBase_ = nullptr;
    int subpageCols_ = 256;
    int subpageRows_ = 256;
    int quadrantsX_  = 4;
    int quadrantsY_  = 4;

    const std::uint8_t (*fade_) [256] = nullptr;
    const std::uint8_t (*alpha_)[256] = nullptr;

    int tmpTriCount_ = 0;

    // Pan state.
    int  panAnchorSx_   = 0;
    int  panAnchorSy_   = 0;
    Q9   panAnchorMx9_  = 0;
    Q9   panAnchorMy9_  = 0;
    bool panActive_     = false;

    // Selection box state.
    int  selAx_ = 0, selAy_ = 0;
    int  selBx_ = 0, selBy_ = 0;
    int  selMaxW_ = 0, selMaxH_ = 0;
    int  selState_ = 0;     // 0 = idle, 1 = active
};

} // namespace ptw
