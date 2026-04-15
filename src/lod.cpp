// lod.cpp — bintree-of-triangles LOD subdivision.
//
// The grid is split into 128×128 cells. The coarsest mesh covers the whole
// grid as two triangles (TL→BR diagonal). At each subdivision the triangle's
// hypotenuse midpoint `(d)` is the split point. If `d` is marked enabled in
// the caller-supplied bit map, we recurse into two child triangles; else we
// emit the parent triangle.
//
// `enable` carries the subdivision pattern. Higher-detail regions (sphere
// silhouette, near the camera, etc.) set more enable bits; flat distant
// regions set fewer. The algorithm is standard — see any SIGGRAPH LOD
// survey. The expression here is new and borrows no code from prior work.

#include "lod.h"

namespace ptw::lod {

namespace {

void subdivide(const EnableBits enable,
               int ax, int ay, int bx, int by, int cx, int cy,
               const TriSink &sink, int &count)
{
    // Midpoint of the hypotenuse AC.
    const int dx = (ax + cx) >> 1;
    const int dy = (ay + cy) >> 1;

    // Stop when the midpoint can't subdivide further (odd coord) OR the
    // midpoint is not enabled.
    const bool canSplit = ((dx | dy) & 1) == 0;
    const bool splitOn  = canSplit && enable[dy & gridMask][dx & gridMask] != 0;

    if (!splitOn) {
        sink(ax, ay, bx, by, cx, cy);
        ++count;
        return;
    }

    // Two children share the hypotenuse midpoint D. Right-angle moves to D.
    subdivide(enable, bx, by, dx, dy, ax, ay, sink, count);
    subdivide(enable, cx, cy, dx, dy, bx, by, sink, count);
}

void enableRecurse(EnableBits out, int x, int y)
{
    if (out[y & gridMask][x & gridMask]) return;
    out[y & gridMask][x & gridMask] = 1;

    // Propagate up: for the midpoint to be emitted, its two parents on
    // the coarser level must also be emitted. Parents = the two corners
    // of the triangle whose hypotenuse the current cell bisects.
    const int lo = cellLodLevel(x, y);
    if (lo <= 0) return;

    // Parent-A: x unchanged, y ± power-of-two corresponding to `lo`.
    // Parent-B: x ± that same power-of-two, y unchanged.
    // The exact sign depends on the cell's quadrant within its parent.
    const int step = 1 << ((gridBits - lo + 1) >> 1);
    enableRecurse(out, x, y ^ step);
    enableRecurse(out, x ^ step, y);
}

} // namespace

int cellLodLevel(int x, int y)
{
    // The coarsest level is 0 (grid corners). Each bit of (x|y) that is a
    // "half-step" increases the level by 1. Equivalent to counting trailing
    // zeros of (x | y) — a cell (x,y) sits on the refinement level equal to
    // gridBits - ctz(x | y).
    const int v = x | y;
    if (v == 0) return 0;
    int tz = 0;
    int probe = v;
    while ((probe & 1) == 0 && tz < gridBits) { probe >>= 1; ++tz; }
    return gridBits - tz;
}

void seedEnables(EnableBits enable)
{
    for (int y = 0; y < gridSize; ++y)
        for (int x = 0; x < gridSize; ++x)
            enable[y][x] = 0;
}

int emitTriangles(const EnableBits enable, const TriSink &sink)
{
    int count = 0;

    // Start from the two coarsest triangles covering the whole grid.
    const int w = gridSize;
    subdivide(enable, 0, w,  0, 0,  w, 0, sink, count);   // upper-left
    subdivide(enable, w, 0,  w, w,  0, w, sink, count);   // lower-right

    return count;
}

} // namespace ptw::lod
