// level.h — Pop3 level (.dat) loader + texpage synthesiser.
//
// Reads an in-game LEVL3 file, extracts the altitude grid, and builds the
// 256×4096 palette-indexed texpage PopTB_World renders onto its sphere.
// Only a subset of the level metadata is kept (enough to pick the right
// palette and label the planet); the thing/object/script tables in the
// file are skipped.

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace ptw::level {

constexpr int kMapCellsSide = 128;                           // 128×128 world
constexpr int kMapCells     = kMapCellsSide * kMapCellsSide;

struct PlayerStart
{
    int cellX = 0;
    int cellZ = 0;
};

struct Metadata
{
    std::string  name;               // level display name
    std::uint8_t levelType = 0;      // palette / texture variant selector
    std::uint8_t numPlayers = 0;
    std::uint8_t objectsBank = 0;
    std::uint16_t startCellX = 64;
    std::uint16_t startCellZ = 64;
    std::array<PlayerStart, 4> playerStarts{};
};

class Level
{
public:
    // Load a LEVL3 file from disk. Returns false on I/O error or bad magic.
    // On success, altitudes() / noAccess() / metadata() are populated.
    bool load(const char *path);

    const std::int16_t *altitudes() const { return alts_.data(); }
    const std::uint8_t *noAccess()  const { return block_.data(); }
    const Metadata     &metadata()  const { return meta_; }

    // Synthesise a 256×4096 palette-indexed texpage using the supplied
    // big-fade table. Writes exactly 1 MiB into `outTex`.
    //   bigfade:    256 × 1152 UBYTE, indexed [alt+75][shade] -> colour
    //   dispMap:    256 × 256 SBYTE, per-texel displacement noise
    //               (may be nullptr — terrain will be smooth without it)
    //   texpage:    256 × 4096 UBYTE (16 sub-pages of 256×256 stacked)
    void buildTexpage(const std::uint8_t *bigfade,
                      const std::int8_t  *dispMap,
                      std::uint8_t       *outTex) const;

private:
    std::array<std::int16_t, kMapCells> alts_{};
    std::array<std::uint8_t, kMapCells> block_{};
    Metadata                            meta_;
};

} // namespace ptw::level
