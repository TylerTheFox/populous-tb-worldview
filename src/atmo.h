// atmo.h — atmosphere ring / disc renderer.
//
// Draws a ring or filled disc into an 8bpp framebuffer, with each output
// pixel remapped through a fade table so the ring reads as an alpha-blended
// halo around the sphere silhouette. Stateless; caller supplies the target
// surface and fade LUT on every call.

#pragma once

#include <cstdint>

namespace ptw::atmo {

struct Surface
{
    std::uint8_t *pixels;
    int           width;
    int           height;
    int           pitch;
};

// Ring with smooth shade gradient between inner-radius and outer-radius.
//
//   centre  = (cx, cy) in screen pixels
//   inner   = inside radius (pixels)
//   outer   = outside radius (pixels); outer >= inner
//   shadeIn = fade-table row sampled at the inner edge (typ. 32)
//   shadeOut= fade-table row sampled at the outer edge (typ. 50)
//   tint    = base palette index the fade table is indexed against
//   fade    = 64×256 shade lookup; result = fade[shade][tint]
void drawRing(const Surface &dst,
              int cx, int cy,
              int inner, int outer,
              int shadeIn, int shadeOut,
              std::uint8_t tint,
              const std::uint8_t (*fade)[256]);

// Filled disc — same as drawRing with inner=0.
void drawDisc(const Surface &dst,
              int cx, int cy,
              int radius,
              int shadeCentre, int shadeEdge,
              std::uint8_t tint,
              const std::uint8_t (*fade)[256]);

} // namespace ptw::atmo
