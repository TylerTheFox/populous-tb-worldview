// datafiles.h — palette / fade / alpha / texpage loaders.
//
// Thin wrappers around fread() that know the expected binary layout of the
// PopTB_World demo data files. Each loader returns true on full success.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ptw::data {

// 1024-byte palette file: 256 entries × {R, G, B, 0} bytes.
bool loadPalette (const char *path, std::uint8_t (*out)[4]);

// 16384-byte fade-table file: 64 shade rows × 256 palette columns.
bool loadFadeTable (const char *path, std::uint8_t (*out)[256]);

// 65536-byte alpha-blend table: 256 tint rows × 256 background columns.
bool loadAlphaTable(const char *path, std::uint8_t (*out)[256]);

// Texpage (default 256×4096). Returns bytes read.
std::size_t loadTexpage(const char *path, std::uint8_t *buf, std::size_t cap);

} // namespace ptw::data
