// viewer.cpp — Win32 viewer for PopTB_World.
//
// Creates a fixed-size window backed by an 8bpp palette-indexed DIB, builds
// a landscape texpage from a selected Pop3 level, and renders the planet.
//
// Command-line usage:
//   World.exe                  — loads data/ demo defaults (fade.dat etc.)
//   World.exe LEVL2001.DAT     — loads levels/LEVL2001.DAT
//   World.exe 2001             — shortcut for "LEVL2001.DAT"
//
// Key bindings:
//   [ / ]     cycle atmosphere palette index
//   < / >     cycle level type char (manual override if auto is wrong)
//   F5        reload current level
//
// Expected files in data/:
//   data/pal0-N.dat       palette (per level type N: '0'..'f')
//   data/fade0-N.dat      shade table (per level type)
//   data/al0-N.dat        alpha table (per level type)
//   data/bigf0-N.dat      big-fade (altitude × shade → palette)
//   levels/<file>         level .dat file

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "../src/worldview.h"
#include "../src/level.h"
#include "datafiles.h"

namespace {

constexpr int kFbInitial = 320;

int gFbSide = kFbInitial;       // current square side in pixels

HWND     gWindow   = nullptr;
HDC      gMemDC    = nullptr;
HBITMAP  gDib      = nullptr;
HBITMAP  gOldBmp   = nullptr;
std::uint8_t *gFrame = nullptr;

ptw::Planet     gPlanet;
ptw::level::Level gLevel;
bool            gLevelLoaded = false;

std::uint8_t gFade   [64 ][256];
std::uint8_t gAlpha  [256][256];
std::uint8_t gPalette[256][4];
std::uint8_t *gBigFade  = nullptr;   // 256 × 1152
std::int8_t  *gDispMap  = nullptr;   // 256 × 256 (signed)
std::uint8_t *gTexpage  = nullptr;   // 256 × 4096

int         gAtmoIdx = 128;
std::string gLevelArg;
char        gLevelTypeChar = '0';
bool        gDragging = false;
std::uint8_t gStarColor  = 255;      // updated per level type (see renderFrame)

char levelTypeToChar(std::uint8_t t)
{
    if (t < 10) return (char)('0' + t);
    return (char)('a' + t - 10);
}

std::string dataPath(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

void installPalette(char levelTypeCh)
{
    const std::string path = dataPath("data/pal0-%c.dat", levelTypeCh);
    ptw::data::loadPalette(path.c_str(), gPalette);

    // Build BITMAPINFO with 256 RGBQUAD entries.
    alignas(BITMAPINFOHEADER)
    BYTE buf[sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD)] = {};
    auto *bi = reinterpret_cast<BITMAPINFO *>(buf);
    bi->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi->bmiHeader.biWidth       = gFbSide;
    bi->bmiHeader.biHeight      = -gFbSide;
    bi->bmiHeader.biPlanes      = 1;
    bi->bmiHeader.biBitCount    = 8;
    bi->bmiHeader.biCompression = BI_RGB;
    bi->bmiHeader.biClrUsed     = 256;

    for (int i = 0; i < 256; ++i) {
        bi->bmiColors[i].rgbRed      = gPalette[i][0];
        bi->bmiColors[i].rgbGreen    = gPalette[i][1];
        bi->bmiColors[i].rgbBlue     = gPalette[i][2];
        bi->bmiColors[i].rgbReserved = 0;
    }
    // Force palette index 0 to pure black so the "clear" that the renderer
    // paints outside the sphere shows up as space, not as whatever the
    // per-level palette happens to put at index 0 (brown on some sets).
    bi->bmiColors[0].rgbRed   = 0;
    bi->bmiColors[0].rgbGreen = 0;
    bi->bmiColors[0].rgbBlue  = 0;

    // Pick the palette entry with the highest luminance for the star field
    // — different landscape types put "white" at different indices.
    int bestSum = -1;
    for (int i = 1; i < 256; ++i) {
        const int sum = (int)gPalette[i][0] + gPalette[i][1] + gPalette[i][2];
        if (sum > bestSum) { bestSum = sum; gStarColor = (std::uint8_t)i; }
    }

    // Pick the palette entry closest to pure blue for the atmosphere ring —
    // mirrors the game's COLOUR(CLR_BLUE) = LbPalette_FindColour(pal,0,0,63)
    // logic, which is why the in-game halo reads as blue.
    {
        int bestDist = std::numeric_limits<int>::max();
        int bestIdx  = 128;
        for (int i = 1; i < 256; ++i) {
            const int dr = gPalette[i][0];          // want 0
            const int dg = gPalette[i][1];          // want 0
            const int db = 255 - gPalette[i][2];    // want 255
            const int d  = dr*dr + dg*dg + db*db;
            if (d < bestDist) { bestDist = d; bestIdx = i; }
        }
        gAtmoIdx = bestIdx;
        gPlanet.setAtmosphere(gAtmoIdx, 6);
    }

    // Replace the existing DIB — first release the old one if any.
    if (gMemDC) {
        if (gOldBmp) SelectObject(gMemDC, gOldBmp);
        DeleteDC(gMemDC);
        gMemDC = nullptr;
    }
    if (gDib) DeleteObject(gDib);

    HDC hdc = GetDC(gWindow);
    void *pixelBase = nullptr;
    gDib = CreateDIBSection(hdc, bi, DIB_RGB_COLORS, &pixelBase, nullptr, 0);
    gFrame = static_cast<std::uint8_t *>(pixelBase);
    gMemDC  = CreateCompatibleDC(hdc);
    gOldBmp = static_cast<HBITMAP>(SelectObject(gMemDC, gDib));
    ReleaseDC(gWindow, hdc);

    // 8-bpp DIB rows pad to a 4-byte boundary. Pass the real stride to the
    // renderer or every frame ends up skewed diagonally when the window
    // isn't a multiple of 4 pixels wide.
    const int pitch = (gFbSide + 3) & ~3;
    gPlanet.attachSurface(gFrame, gFbSide, gFbSide, pitch);
}

// Load the fade / alpha / bigfade tables for a given level-type char.
void loadTablesForLevelType(char ch)
{
    const std::string fadePath  = dataPath("data/fade0-%c.dat", ch);
    const std::string alphaPath = dataPath("data/al0-%c.dat",   ch);
    const std::string bigfPath  = dataPath("data/bigf0-%c.dat", ch);

    ptw::data::loadFadeTable (fadePath.c_str(),  gFade);
    ptw::data::loadAlphaTable(alphaPath.c_str(), gAlpha);
    if (!gBigFade) gBigFade = (std::uint8_t *)std::malloc(256 * 1152);
    std::memset(gBigFade, 0, 256 * 1152);
    ptw::data::loadTexpage(bigfPath.c_str(), gBigFade, 256 * 1152);

    // Per-level displacement map (signed) — drives per-texel surface grain.
    const std::string dispPath = dataPath("data/disp0-%c.dat", ch);
    if (!gDispMap) gDispMap = (std::int8_t *)std::malloc(256 * 256);
    std::memset(gDispMap, 0, 256 * 256);
    ptw::data::loadTexpage(dispPath.c_str(),
                           (std::uint8_t *)gDispMap, 256 * 256);

    gPlanet.attachFadeTable (gFade);
    gPlanet.attachAlphaTable(gAlpha);
}

bool loadLevel(const std::string &leafName)
{
    // Accept "LEVL2001.DAT", bare "2001", or a full path. Try data/levels/
    // first and fall back to data/ at the root — the original game ships
    // levels under ./levels but our demo put them next to the other tables.
    auto tryLoad = [](const std::string &p) { return gLevel.load(p.c_str()); };
    std::string path;
    bool ok = false;
    const bool looksLikeBareNumber =
        leafName.find('.')  == std::string::npos
        && leafName.find('\\') == std::string::npos
        && leafName.find('/')  == std::string::npos;
    const bool absolute =
        leafName.find(':') != std::string::npos
        || leafName.find('\\') != std::string::npos
        || leafName.find('/')  != std::string::npos;

    // Layout: levels/ holds level files, data/ holds palettes + tables.
    const std::string candidates[] = {
        looksLikeBareNumber ? dataPath("levels/LEVL%s.DAT", leafName.c_str()) : std::string(),
        looksLikeBareNumber ? dataPath("levels/levl%s.dat", leafName.c_str()) : std::string(),
        !looksLikeBareNumber && !absolute ? dataPath("levels/%s", leafName.c_str()) : std::string(),
        absolute ? leafName : std::string(),
    };
    for (const auto &c : candidates) {
        if (c.empty()) continue;
        if (tryLoad(c)) { path = c; ok = true; break; }
    }

    if (!ok) {
        MessageBoxA(gWindow,
                    (std::string("Tried candidates for: ") + leafName).c_str(),
                    "Level load failed", MB_OK);
        return false;
    }

    gLevelTypeChar = levelTypeToChar(gLevel.metadata().levelType);
    loadTablesForLevelType(gLevelTypeChar);
    installPalette(gLevelTypeChar);

    if (!gTexpage) gTexpage = (std::uint8_t *)std::malloc(256 * 4096);
    std::memset(gTexpage, 0, 256 * 4096);
    gLevel.buildTexpage(gBigFade, gDispMap, gTexpage);
    gPlanet.attachTexpage(gTexpage, 256, 256, 4, 4);

    // Centre the scroll on the level's recorded start position.
    const auto &md = gLevel.metadata();
    const ptw::Q9 sx = (ptw::Q9)md.startCellX << 9;
    const ptw::Q9 sz = (ptw::Q9)md.startCellZ << 9;
    gPlanet.setScroll(sx ? sx : (32 << 9), sz ? sz : (32 << 9));
    gPlanet.setAtmosphere(gAtmoIdx, 6);

    gLevelLoaded = true;
    return true;
}

void renderFrame()
{
    const int pitch = (gFbSide + 3) & ~3;
    std::memset(gFrame, 0, (std::size_t)pitch * gFbSide);

    gPlanet.drawStars(2000, gStarColor);
    const int tris = gPlanet.drawLandscape(8);

    char title[256];
    const char *name = gLevelLoaded ? gLevel.metadata().name.c_str() : "(no level)";
    _snprintf_s(title, sizeof(title), _TRUNCATE,
                "PopTB_World   %s   Polys:%d   type=%c   atm=%d",
                name, tris, gLevelTypeChar, gAtmoIdx);
    SetWindowTextA(gWindow, title);
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wp == VK_OEM_4) { gAtmoIdx = (gAtmoIdx + 255) & 255;
                              gPlanet.setAtmosphere(gAtmoIdx, 6); }
        if (wp == VK_OEM_6) { gAtmoIdx = (gAtmoIdx + 1) & 255;
                              gPlanet.setAtmosphere(gAtmoIdx, 6); }
        if (wp == VK_OEM_COMMA  /* < */ || wp == VK_OEM_PERIOD /* > */) {
            int delta = (wp == VK_OEM_PERIOD) ? 1 : -1;
            int v = (gLevelTypeChar >= 'a')
                        ? 10 + (gLevelTypeChar - 'a')
                        : gLevelTypeChar - '0';
            v = (v + 36 + delta) % 36;
            gLevelTypeChar = levelTypeToChar((std::uint8_t)v);
            loadTablesForLevelType(gLevelTypeChar);
            installPalette(gLevelTypeChar);
            if (gLevelLoaded) {
                std::memset(gTexpage, 0, 256 * 4096);
                gLevel.buildTexpage(gBigFade, gDispMap, gTexpage);
            }
        }
        if (wp == VK_F5 && !gLevelArg.empty()) loadLevel(gLevelArg);
        if (wp == 'M') gPlanet.setMirrorX(!gPlanet.mirrorX());
        if (wp == 'N') gPlanet.setMirrorY(!gPlanet.mirrorY());
        return 0;
    case WM_RBUTTONDOWN:
        gDragging = true;
        gPlanet.panBegin((short)LOWORD(lp), (short)HIWORD(lp));
        SetCapture(h);
        return 0;
    case WM_MOUSEMOVE:
        if (gDragging) gPlanet.panUpdate((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_RBUTTONUP:
        if (gDragging) {
            gPlanet.panEnd();
            gDragging = false;
            ReleaseCapture();
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        BitBlt(hdc, 0, 0, gFbSide, gFbSide, gMemDC, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        // Don't let the user shrink below 160 px square (keeps the
        // sphere renderer usable) nor grow beyond a reasonable cap.
        auto *mmi = reinterpret_cast<MINMAXINFO *>(lp);
        RECT frame{0, 0, 160, 160};
        AdjustWindowRect(&frame, GetWindowLongA(h, GWL_STYLE), FALSE);
        mmi->ptMinTrackSize.x = frame.right  - frame.left;
        mmi->ptMinTrackSize.y = frame.bottom - frame.top;
        return 0;
    }
    case WM_SIZING: {
        // Lock aspect to 1:1 while the user drags the frame. Whichever
        // dimension changed more is the authoritative one; mirror it
        // onto the other axis so the planet stays round.
        auto *r = reinterpret_cast<RECT *>(lp);
        RECT frameClient{0, 0, 0, 0};
        AdjustWindowRect(&frameClient, GetWindowLongA(h, GWL_STYLE), FALSE);
        const int frameW = (0 - frameClient.left) + frameClient.right;
        const int frameH = (0 - frameClient.top)  + frameClient.bottom;
        int cw = (r->right  - r->left) - frameW;
        int ch = (r->bottom - r->top)  - frameH;
        const int side = (cw > ch) ? cw : ch;
        switch (wp) {
        case WMSZ_LEFT:
            r->left  = r->right  - (side + frameW); break;
        case WMSZ_RIGHT:
            r->right = r->left   + (side + frameW); break;
        case WMSZ_TOP:
            r->top    = r->bottom - (side + frameH); break;
        case WMSZ_BOTTOM:
            r->bottom = r->top    + (side + frameH); break;
        default:
            r->right  = r->left + (side + frameW);
            r->bottom = r->top  + (side + frameH);
            break;
        }
        return TRUE;
    }
    case WM_SIZE: {
        // Resize the DIB + planet surface to match the new client rect.
        // LOWORD(lp) is client width, HIWORD(lp) is client height; both
        // are guaranteed equal by the WM_SIZING handler above.
        const int newSide = LOWORD(lp);
        if (newSide > 0 && newSide != gFbSide) {
            gFbSide = newSide;
            installPalette(gLevelTypeChar);
            gPlanet.setViewport(gFbSide / 2, gFbSide / 2,
                                gFbSide / 2 - 20, 40 << 9);
            InvalidateRect(h, nullptr, TRUE);
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY:    PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR cmdLine, int show)
{
    if (cmdLine && *cmdLine) gLevelArg = cmdLine;

    WNDCLASSA wc{};
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = "PopTBWorldViewer";
    RegisterClassA(&wc);

    // Resizable window, aspect locked to 1:1 on drag (see WM_SIZING).
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT want{0, 0, gFbSide, gFbSide};
    AdjustWindowRect(&want, style, FALSE);

    gWindow = CreateWindowA("PopTBWorldViewer", "PopTB_World",
                            style,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            want.right  - want.left,
                            want.bottom - want.top,
                            nullptr, nullptr, hInst, nullptr);
    if (!gWindow) return 1;

    installPalette(gLevelTypeChar);
    loadTablesForLevelType(gLevelTypeChar);

    gPlanet.setViewport(gFbSide / 2, gFbSide / 2, gFbSide / 2 - 20, 40 << 9);
    gPlanet.setProjectionMorph(0, nullptr, nullptr);
    gPlanet.setAtmosphere(gAtmoIdx, 6);
    gPlanet.setScroll(32 << 9, 32 << 9);

    // Default to level 2 (LEVL2002) when no level is specified on the CLI.
    if (gLevelArg.empty()) gLevelArg = "2002";
    loadLevel(gLevelArg);

    ShowWindow(gWindow, show);
    UpdateWindow(gWindow);

    MSG msg{};
    for (;;) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        renderFrame();
        InvalidateRect(gWindow, nullptr, FALSE);
        Sleep(16);
    }
done:
    if (gMemDC) {
        if (gOldBmp) SelectObject(gMemDC, gOldBmp);
        DeleteDC(gMemDC);
    }
    if (gDib) DeleteObject(gDib);
    std::free(gTexpage);
    std::free(gBigFade);
    return (int)msg.wParam;
}
