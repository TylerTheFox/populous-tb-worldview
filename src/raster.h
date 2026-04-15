// raster.h — 8bpp software triangle rasterizer.
//
// A small, self-contained scanline rasterizer tailored for the PopTB_World
// landscape view. Targets a palette-indexed byte framebuffer and samples
// palette-indexed textures through a shade (fade) table or an alpha blend
// table, matching the 1998 Bullfrog visual language without borrowing any
// of its code.

#pragma once

#include <cstdint>

namespace ptw::raster {

struct Vertex
{
    std::int32_t sx;       // screen pixel X (integer)
    std::int32_t sy;       // screen pixel Y (integer)
    std::int32_t u;        // texture U, 16.16 fixed-point, wraps at 256
    std::int32_t v;        // texture V, 16.16 fixed-point, wraps at tile height
    std::int32_t light;    // shade row into the fade table, 16.16 fixed-point
};

enum class FillMode
{
    Solid,             // one palette index, no shading
    SolidShaded,       // one palette index through fade_table[light][idx]
    Textured,          // texture sampled, no shading
    TexturedShaded,    // texture sampled, then fade_table[light][sample]
    AlphaBlend,        // alpha_table[tint][destination]  (no texture)
};

// A bound render target. The rasterizer never allocates — all buffers are
// owned by the caller. Thread-compatible (one context per thread).
class Canvas
{
public:
    // 8bpp target surface, row-major, `pitch` bytes per row.
    void attachSurface(std::uint8_t *pixels, int width, int height, int pitch);

    // Palette-indexed texture atlas. cols should be 256 (tile width);
    // `rowsPerTile` is the vertical period the V coordinate wraps within.
    void attachTextureAtlas(const std::uint8_t *base, int cols, int rowsPerTile);

    // Point `texBase` at the current sub-page for the triangle about to be
    // drawn. The rasterizer reads the full `cols * rowsPerTile` block from
    // this base. Pop3-style landscapes switch sub-page per cell block.
    void useTexturePage(const std::uint8_t *texBase) { texBase_ = texBase; }

    void attachFadeTable (const std::uint8_t (*fade) [256]);
    void attachAlphaTable(const std::uint8_t (*alpha)[256]);

    void drawTriangle(Vertex a, Vertex b, Vertex c,
                      FillMode mode, std::uint8_t tint = 0);

    // Pixel-precise plot with clip — used for star field.
    void plotPixel(int x, int y, std::uint8_t color);

    std::uint8_t *surface() const  { return pixels_; }
    int           width() const    { return width_;  }
    int           height() const   { return height_; }
    int           pitch() const    { return pitch_;  }

private:
    std::uint8_t *pixels_ = nullptr;
    int           width_  = 0;
    int           height_ = 0;
    int           pitch_  = 0;

    const std::uint8_t *texBase_     = nullptr;
    int                 texCols_     = 256;
    int                 texRows_     = 256;
    int                 texRowShift_ = 8;   // log2(texCols_)

    const std::uint8_t (*fade_) [256] = nullptr;
    const std::uint8_t (*alpha_)[256] = nullptr;
};

} // namespace ptw::raster
