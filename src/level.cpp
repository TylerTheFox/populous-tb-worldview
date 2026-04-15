// level.cpp — LEVL3 file parser + altitude→texpage synthesis.

#include "level.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace ptw::level {

namespace {

constexpr std::size_t kHeaderSize = 952;   // LevelHeaderv3, default-aligned
constexpr std::size_t kMagicSize  = 5;

// Byte offsets into LevelHeaderv3.
constexpr std::size_t kOffName          = 56;      // 32 bytes
constexpr std::size_t kOffNumPlayers    = 88;
constexpr std::size_t kOffLevelType     = 96;
constexpr std::size_t kOffObjectsBank   = 97;
constexpr std::size_t kOffStartPos      = 612;
constexpr std::size_t kOffStartAngle    = 614;
constexpr std::size_t kOffMaxAltPoints  = 620;
constexpr std::size_t kOffMaxNumObjs    = 624;
constexpr std::size_t kOffMaxNumPlayers = 628;

// size of the PlayerSaveInfo record the loader must skip over.
constexpr std::size_t kPlayerSaveInfoSize = 24;  // verified against file sizes

inline std::uint16_t readU16(const std::uint8_t *p) {
    return (std::uint16_t)(p[0] | (p[1] << 8));
}
inline std::uint32_t readU32(const std::uint8_t *p) {
    return (std::uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
inline std::int16_t  readS16(const std::uint8_t *p) {
    return (std::int16_t)readU16(p);
}

// Compute a per-cell shade in [0..255] using simple neighbour altitude
// differences along +X and +Z, analogous to the game's update_shade_map_square
// but with a fixed sun direction. This lets us render with bigfade without
// pulling in the full gsi.LevelInfo.SunInfo structure.
// Classic 3×3 "cliff" metric, matching set_square_map_params in the engine:
// take the altitude spread across the 3×3 neighbourhood and scale by 1/8,
// clamped to [0, 127]. A cell flagged as a cliff gets an extra +75 altitude
// bump at bigfade-lookup time, which keeps sloped land safely above the
// water band regardless of per-texel displacement.
std::uint8_t cliffForCell(const std::int16_t *alts, int x, int z)
{
    int maxA = std::numeric_limits<int>::min();
    int minA = std::numeric_limits<int>::max();
    for (int dz = -1; dz <= 1; ++dz)
    for (int dx = -1; dx <= 1; ++dx) {
        const int nx = std::clamp(x + dx, 0, kMapCellsSide - 1);
        const int nz = std::clamp(z + dz, 0, kMapCellsSide - 1);
        const int a  = alts[nz * kMapCellsSide + nx];
        if (a > maxA) maxA = a;
        if (a < minA) minA = a;
    }
    if (maxA == 0) return 0;               // neighbourhood is sea level / void
    int c = (maxA - minA) >> 3;
    if (c < 1)   c = 1;
    if (c > 127) c = 127;
    return (std::uint8_t)c;
}

std::uint8_t shadeForCell(const std::int16_t *alts, int x, int z)
{
    const int nx = std::min(x + 1, kMapCellsSide - 1);
    const int nz = std::min(z + 1, kMapCellsSide - 1);
    const int a  = alts[z  * kMapCellsSide + x ];
    const int ax = alts[z  * kMapCellsSide + nx];
    const int az = alts[nz * kMapCellsSide + x ];

    // Matches the in-game update_shade_map_square. Sun normal is the
    // 45°/45°/45° unit vector (components all 147 in the engine's fixed
    // integer form). Large dhX/dhZ produce ±40 shade swings — the core
    // contributor to the planet's volumetric look.
    constexpr int sunComponent = 147;
    const int dhX = a - ax;
    const int dhZ = a - az;
    int s = sunComponent - dhX * sunComponent - dhZ * sunComponent;
    s /= 350;
    s += 128;
    if (s < 0)   s = 0;
    if (s > 255) s = 255;
    return (std::uint8_t)s;
}

} // namespace

namespace {

// Level v2 has no magic header. The file is a flat stream of four
// MAP_XZ_SIZE-length arrays: altitudes (SWORD), blocks (UBYTE), orients
// (UBYTE), no-access (UBYTE). Players / sunlight / things follow but are
// skipped here — the viewer only needs the heightfield.
// v2's companion .hdr file is a raw dump of LevelHeader (v2), 616 bytes:
//   [0..55] PlayerThings, [56..87] Name, [88] NumPlayers, [89..91] cpu idx,
//   [92..95] DefaultAllies, [96] LevelType, [97] ObjectsBank,
//   [98] LevelFlags, [99] pad, [100..611] Markers, [612..613] StartPos,
//   [614..615] StartAngle.
bool readCompanionHeader(const char *datPath, Metadata &metaOut)
{
    // Derive .hdr path: replace the last .dat / .DAT with .hdr.
    std::string hdrPath = datPath;
    auto dot = hdrPath.find_last_of('.');
    if (dot != std::string::npos) hdrPath.resize(dot);
    const std::string hdrUpper = hdrPath + ".HDR";
    const std::string hdrLower = hdrPath + ".hdr";
    std::FILE *hf = std::fopen(hdrLower.c_str(), "rb");
    if (!hf) hf = std::fopen(hdrUpper.c_str(), "rb");
    if (!hf) return false;

    std::uint8_t buf[616];
    const std::size_t got = std::fread(buf, 1, sizeof(buf), hf);
    std::fclose(hf);
    if (got < 97) return false;       // need at least up to LevelType

    metaOut.name.assign((const char *)&buf[56], 32);
    metaOut.name = metaOut.name.c_str();
    metaOut.numPlayers  = buf[88];
    metaOut.levelType   = buf[96];
    metaOut.objectsBank = buf[97];
    if (got >= 614) {
        const std::uint16_t startPos = (std::uint16_t)(buf[612] | (buf[613] << 8));
        metaOut.startCellX = startPos & 0xff;
        metaOut.startCellZ = startPos >> 8;
    }
    return true;
}

bool loadVersion2(std::FILE *f, const char *datPath, Level &lvl,
                  std::array<std::int16_t, kMapCells> &altsOut,
                  std::array<std::uint8_t, kMapCells> &blockOut,
                  Metadata &metaOut)
{
    std::rewind(f);
    if (std::fread(altsOut.data(), sizeof(std::int16_t), kMapCells, f) != kMapCells)
        return false;
    std::array<std::uint8_t, kMapCells> scratch;
    if (std::fread(scratch.data(), 1, kMapCells, f) != kMapCells) return false;  // blocks (unused)
    if (std::fread(scratch.data(), 1, kMapCells, f) != kMapCells) return false;  // orients (unused)
    if (std::fread(blockOut.data(), 1, kMapCells, f) != kMapCells) return false; // no-access

    // Pull level type / name / start position from the companion .hdr.
    metaOut.name        = "(LEVL2)";
    metaOut.levelType   = 0;
    metaOut.numPlayers  = 0;
    metaOut.objectsBank = 0;
    metaOut.startCellX  = 64;
    metaOut.startCellZ  = 64;
    metaOut.playerStarts.fill(PlayerStart{});
    readCompanionHeader(datPath, metaOut);
    (void)lvl;
    return true;
}

} // namespace

bool Level::load(const char *path)
{
    std::FILE *f = std::fopen(path, "rb");
    if (!f) return false;

    char magic[kMagicSize];
    if (std::fread(magic, 1, kMagicSize, f) != kMagicSize) {
        std::fclose(f);
        return false;
    }

    // Magic mismatch → try the pre-v3 (LEVL2) flat format.
    if (std::memcmp(magic, "LEVL3", kMagicSize) != 0) {
        const bool ok = loadVersion2(f, path, *this, alts_, block_, meta_);
        std::fclose(f);
        return ok;
    }

    std::uint8_t header[kHeaderSize];
    if (std::fread(header, 1, kHeaderSize, f) != kHeaderSize) {
        std::fclose(f);
        return false;
    }

    // Pull the pieces of metadata we expose.
    meta_.name.assign((const char *)&header[kOffName], 32);
    meta_.name = meta_.name.c_str();   // trim trailing NULs
    meta_.numPlayers  = header[kOffNumPlayers];
    meta_.levelType   = header[kOffLevelType];
    meta_.objectsBank = header[kOffObjectsBank];
    meta_.startCellX  = readU16(&header[kOffStartPos]) & 0xff;
    meta_.startCellZ  = readU16(&header[kOffStartPos]) >> 8;
    (void)readU16(&header[kOffStartAngle]);

    const std::uint32_t maxAlts    = readU32(&header[kOffMaxAltPoints]);
    const std::uint32_t maxObjects = readU32(&header[kOffMaxNumObjs]);
    const std::uint32_t maxPlayers = readU32(&header[kOffMaxNumPlayers]);

    // Altitudes: SWORD per map point, up to kMapCells. Clamp and discard
    // any overspill in larger-than-standard maps.
    alts_.fill(0);
    for (std::uint32_t i = 0; i < maxAlts; ++i) {
        std::int16_t alt;
        if (std::fread(&alt, 1, sizeof(alt), f) != sizeof(alt)) {
            std::fclose(f);
            return false;
        }
        if (i < alts_.size()) alts_[i] = alt;
    }

    // NoAccess flags: UBYTE per map point.
    block_.fill(0);
    for (std::uint32_t i = 0; i < maxAlts; ++i) {
        std::uint8_t flag;
        if (std::fread(&flag, 1, 1, f) != 1) {
            std::fclose(f);
            return false;
        }
        if (i < block_.size()) block_[i] = flag;
    }

    // Player start positions follow; read only to extract the start cells
    // we expose, then stop — thing/sunlight data isn't needed here.
    for (std::uint32_t i = 0; i < maxPlayers && i < 4; ++i) {
        std::uint8_t rec[kPlayerSaveInfoSize];
        if (std::fread(rec, 1, kPlayerSaveInfoSize, f) != kPlayerSaveInfoSize)
            break;
        // StartPos is the first 4 bytes (two UWORDs, X then Z in map cells).
        const std::uint16_t sx = readU16(&rec[0]);
        const std::uint16_t sz = readU16(&rec[2]);
        meta_.playerStarts[i] = PlayerStart{(int)sx, (int)sz};
    }

    (void)maxObjects;     // objects table is not read for the viewer
    std::fclose(f);
    return true;
}

namespace {

// Altitude-indexed displacement scale — same curve the in-game landtext
// fills into `DispWithAlt[1152]`. Water rows take a modest factor so the
// coastline develops surf/caustic texture, land rows interpolate up to
// a fixed mountain-range value.
inline int dispScaleForAlt(int v)
{
    constexpr int waterFactor = 320;   // WATER_DISP_WITH_ALT_FACTOR
    if (v < 128)   return waterFactor;
    if (v > 362)   return 1024;
    return waterFactor + (v - 128) * 3;
}

} // namespace

void Level::buildTexpage(const std::uint8_t *bigfade,
                         const std::int8_t  *dispMap,
                         std::uint8_t       *outTex) const
{
    if (!bigfade || !outTex) return;

    // bigfade is 256 (shade) wide × 1152 (altitude) tall. In-game the V-axis
    // is `altitude + BEACH_OFFSET/2` where BEACH_OFFSET = 150 — so sea-level
    // cells land at V=75 (the waterline band), negatives reach deep-water
    // rows, positives reach land/mountain rows. Without this shift every
    // low-altitude cell sampled row 0 and picked up the wrong hue (purple
    // in most palettes).
    auto sampleBigFade = [bigfade](int altitude, std::uint8_t shade) -> std::uint8_t {
        constexpr int kBeachOffsetHalf = 75;
        int v = altitude + kBeachOffsetHalf;
        if (v < 0)       v = 0;
        else if (v > 1151) v = 1151;
        return bigfade[v * 256 + shade];
    };

    // For each 32×32 map-cell block → one 256×256-texel sub-page.
    // Sub-pages are laid out linearly by (subZ * quadrantsX + subX) × 64KB.
    constexpr int kQuadrants = 4;
    constexpr int kBlockCells = 32;
    constexpr int kCellTexels = 8;          // 32 cells * 8 texels = 256 wide

    for (int subZ = 0; subZ < kQuadrants; ++subZ)
    {
        for (int subX = 0; subX < kQuadrants; ++subX)
        {
            std::uint8_t *pageBase = outTex
                + (std::size_t)(subZ * kQuadrants + subX) * (256 * 256);

            for (int dz = 0; dz < kBlockCells; ++dz)
            {
                for (int dx = 0; dx < kBlockCells; ++dx)
                {
                    const int cellX = subX * kBlockCells + dx;
                    const int cellZ = subZ * kBlockCells + dz;
                    const int nextX = std::min(cellX + 1, kMapCellsSide - 1);
                    const int nextZ = std::min(cellZ + 1, kMapCellsSide - 1);

                    // Four corner altitudes + shades, interpolated across
                    // the cell's 8×8 texel block. The bigfade LUT turns
                    // each (shade, alt) pair into a palette index — the
                    // per-texel interpolation gives the terrain its
                    // continuous gradient instead of 8-pixel colour slabs.
                    // Per-corner altitude; sloped cells (cliff != 0) get an
                    // extra +75 altitude so the displacement jitter can't
                    // drag them back across the waterline.
                    auto cornerAlt = [this](int cx, int cz) {
                        const int base = alts_[cz * kMapCellsSide + cx];
                        const int cliff = cliffForCell(alts_.data(), cx, cz);
                        return base + (cliff > 0 ? 75 : 0);
                    };
                    const int aTL = cornerAlt(cellX, cellZ);
                    const int aTR = cornerAlt(nextX, cellZ);
                    const int aBL = cornerAlt(cellX, nextZ);
                    const int aBR = cornerAlt(nextX, nextZ);
                    const int sTL = shadeForCell(alts_.data(), cellX, cellZ);
                    const int sTR = shadeForCell(alts_.data(), nextX, cellZ);
                    const int sBL = shadeForCell(alts_.data(), cellX, nextZ);
                    const int sBR = shadeForCell(alts_.data(), nextX, nextZ);

                    std::uint8_t *rowBase = pageBase
                        + (dz * kCellTexels) * 256
                        + (dx * kCellTexels);

                    // Base address into the displacement map for this
                    // cell. The game walks a 32×32 disp-pixel stripe per
                    // cell, advancing 4 disp pixels per texel (32/8) — so
                    // adjacent texels sample *non-adjacent* disp values.
                    // That decorrelation is what makes the grain look like
                    // per-pixel noise instead of correlated blotches.
                    constexpr int kDispStride = 32 / kCellTexels; // = 4
                    const int dmRow = (cellZ * 32) & 0xff;
                    const int dmCol = (cellX * 32) & 0xff;

                    for (int ty = 0; ty < kCellTexels; ++ty) {
                        const int wy = ty;
                        const int iwy = kCellTexels - wy;
                        std::uint8_t *texel = rowBase;
                        for (int tx = 0; tx < kCellTexels; ++tx) {
                            const int wx = tx;
                            const int iwx = kCellTexels - wx;
                            int altMix = (aTL * iwx * iwy + aTR * wx * iwy
                                        + aBL * iwx * wy  + aBR * wx * wy)
                                        / (kCellTexels * kCellTexels);
                            const int shMix  = (sTL * iwx * iwy + sTR * wx * iwy
                                              + sBL * iwx * wy  + sBR * wx * wy)
                                              / (kCellTexels * kCellTexels);
                            // Per-texel displacement — jitters which
                            // bigfade row we sample from, producing the
                            // grainy surface detail the in-game renderer
                            // shows. Water rows (altMix+75 < 128) get
                            // amplified jitter → surf look; land rows get
                            // light jitter → soil grain.
                            int shadeFinal = shMix;
                            if (dispMap) {
                                const int dmX = (dmCol + tx * kDispStride) & 0xff;
                                const int dmY = (dmRow + ty * kDispStride) & 0xff;
                                const int disp = dispMap[dmY * 256 + dmX];
                                const int vBase = altMix + 75;
                                const int dispAmt = (disp * dispScaleForAlt(
                                        vBase < 0 ? 0 : (vBase > 1151 ? 1151 : vBase))) >> 10;
                                altMix += dispAmt;
                                // BumpDetailMap contribution: the game adds
                                // (bump >> 2) into the SHADE column of the
                                // bigfade lookup to produce per-texel
                                // surface grain. Without a standalone bump
                                // file the game falls back to reusing the
                                // displacement map — we do the same.
                                shadeFinal += (disp >> 2);
                            }
                            *texel++ = sampleBigFade(altMix,
                                                     (std::uint8_t)std::clamp(shadeFinal, 0, 255));
                        }
                        rowBase += 256;
                    }
                }
            }
        }
    }
}

} // namespace ptw::level
