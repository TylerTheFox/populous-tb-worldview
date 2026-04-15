// lod.h — level-of-detail mesh over a 2D square grid.
//
// Produces a variable-resolution triangulation of a power-of-two grid. The
// mesh is specified by per-vertex "enabled" bits: a vertex at (x, y) is on
// iff enable[y][x] != 0. The recursion emits the coarsest triangulation
// consistent with the on-bits (no T-junctions, right-isoceles layout).
//
// Standard bintree-of-triangles approach (Lindstrom et al.). The grid is
// fixed-size 128×128 here; parametrise the template-time constant if a
// different size is needed.

#pragma once

#include <cstdint>
#include <functional>

namespace ptw::lod {

inline constexpr int gridBits = 7;                 // 128 = 1<<7
inline constexpr int gridSize = 1 << gridBits;     // 128
inline constexpr int gridMask = gridSize - 1;      // 127

using EnableBits = std::uint8_t[gridSize][gridSize];

// Callback invoked once per emitted triangle. Coordinates are grid cells
// (integer 0..gridSize), CCW-ordered. The callback decides whether to turn
// these into screen triangles, display-list entries, debug output, etc.
using TriSink = std::function<void(int ax, int ay,
                                   int bx, int by,
                                   int cx, int cy)>;

// Returns the LOD "level" index for grid-cell coords (x,y). Lower levels
// are coarser (fewer cells on, larger triangles). Monotone in both axes
// within a quadrant — meaning the standard recursive subdivision layout.
int cellLodLevel(int x, int y);

// Seed the enable-bit array so the output matches the regular 4x4 LOD
// block layout. Similar to flood-fill up from the coarsest bintree.
void seedEnables(EnableBits enable);

// Emit the triangulation. Each triangle is passed to `sink`. Returns the
// number of triangles emitted.
int emitTriangles(const EnableBits enable, const TriSink &sink);

} // namespace ptw::lod
