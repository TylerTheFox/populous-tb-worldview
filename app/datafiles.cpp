// datafiles.cpp — data file I/O.

#include "datafiles.h"

#include <cstdio>

namespace ptw::data {

namespace {
template <typename T>
bool readAll(const char *path, T *dst, std::size_t bytes)
{
    std::FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    const std::size_t got = std::fread(dst, 1, bytes, f);
    std::fclose(f);
    return got == bytes;
}
} // namespace

bool loadPalette(const char *path, std::uint8_t (*out)[4])
{
    return readAll(path, out, 256 * 4);
}

bool loadFadeTable(const char *path, std::uint8_t (*out)[256])
{
    return readAll(path, out, 64 * 256);
}

bool loadAlphaTable(const char *path, std::uint8_t (*out)[256])
{
    return readAll(path, out, 256 * 256);
}

std::size_t loadTexpage(const char *path, std::uint8_t *buf, std::size_t cap)
{
    std::FILE *f = std::fopen(path, "rb");
    if (!f) return 0;
    const std::size_t got = std::fread(buf, 1, cap, f);
    std::fclose(f);
    return got;
}

} // namespace ptw::data
