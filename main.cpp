// =============================================================================
//  CAT's eye  -  ultra-light Win32 system-stats overlay  (CPU / RAM / GPU / FPS)
// -----------------------------------------------------------------------------
//  Design
//    * Pure Win32 + GDI+.  No STL, no exceptions, no frameworks.
//    * One borderless, per-pixel-alpha (UpdateLayeredWindow) window that is
//      always on top, 100% click-through and invisible to Alt+Tab / taskbar.
//    * Fully event-driven.  The UI thread sleeps in GetMessage().  A sampler
//      thread wakes once per second, reads the hardware, publishes the numbers
//      and posts ONE message; the UI thread repaints and goes back to sleep.
//      The hardware is never touched on the render path.
//    * Text is drawn as GDI+ glyph paths: soft drop-shadow + dark outline +
//      bright fill, so it stays readable on light and dark backgrounds alike.
//
//  Metrics
//    CPU : GetSystemTimes()          idle / kernel / user deltas
//    RAM : GlobalMemoryStatusEx()    used / total GB and %
//    GPU : PDH "\GPU Engine(*)\Utilization Percentage", summed per physical
//          engine over all processes; the busiest engine is shown (the rule
//          Task Manager uses).  Needs Windows 10 1709+ (WDDM 2.x), else "N/A".
//    FPS : Desktop Window Manager composition rate = frames DWM composed during
//          the last second (DwmGetCompositionTimingInfo -> DWM_TIMING_INFO::cFrame).
//
//  Global hotkeys:   Ctrl+Alt+F10 = show / hide       Ctrl+Alt+F11 = quit
//
//  Build (MSVC, x64 or x86 Native Tools prompt):
//    cl /nologo /O1 /GL /GR- /MT main.cpp /Fe:CATs_eye.exe /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib pdh.lib dwmapi.lib
//  Build (MinGW-w64):
//    g++ main.cpp -o CATs_eye.exe -Os -s -mwindows -static -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections -lgdiplus -lpdh -ldwmapi -lgdi32 -luser32
// =============================================================================

#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif
#ifndef WINVER
#  define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <gdiplus.h>     // text rendering (GDI+ glyph paths)
#include <pdh.h>         // GPU engine utilisation counters
#include <dwmapi.h>      // DWM composition timing (FPS)

#ifdef _MSC_VER          // lets a bare `cl main.cpp` link without extra arguments
#  pragma comment(lib, "user32.lib")
#  pragma comment(lib, "gdi32.lib")
#  pragma comment(lib, "gdiplus.lib")
#  pragma comment(lib, "pdh.lib")
#  pragma comment(lib, "dwmapi.lib")
#  pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#endif

using namespace Gdiplus;

// =============================================================================
//  SETTINGS  (sizes are for 96 DPI and are scaled automatically)
// =============================================================================
static const WCHAR kAppName[]   = L"CAT's eye";                       // window class + title
static const WCHAR kMutexName[] = L"Local\\CATs_eye_single_instance"; // one instance only

static const float kFontPx      = 15.0f;   // stat-line font size
static const float kHeaderScale = 0.80f;   // header size relative to the stat font
static const float kOutlinePx   = 3.0f;    // outline pen width (half of it shows outside the glyph)
static const float kShadowPx    = 1.5f;    // drop-shadow offset
static const int   kMarginPx    = 14;      // distance from the screen edge
static const int   kCorner      = 0;       // 0 = top-left  1 = top-right  2 = bottom-left  3 = bottom-right
static const DWORD kPollMs      = 1000;    // hardware polling period

#ifndef MOD_NOREPEAT
#  define MOD_NOREPEAT 0x4000
#endif
static const UINT kHotMods   = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
static const UINT kHotQuit   = VK_F11;     // Ctrl+Alt+F11 : quit
static const UINT kHotToggle = VK_F10;     // Ctrl+Alt+F10 : show / hide

// =============================================================================
//  1. SHARED STATE   (sampler thread writes, UI thread reads)
// =============================================================================
enum { STAT_PENDING = -1, STAT_NA = -2 };   // sentinels: "no sample yet" / "not available"
enum { WM_APP_STATS = WM_APP + 1 };         // sampler -> UI : new numbers are ready
enum { HK_QUIT = 1, HK_TOGGLE = 2 };        // hotkey ids

struct Stats
{
    int  cpu;           // 0..100 %
    int  ramPct;        // 0..100 %
    UINT ramUsedX10;    // GB * 10   (74 -> 7.4 GB)
    UINT ramTotalX10;   // GB * 10
    int  gpu;           // 0..100 %
    int  fps;           // frames per second
};

static Stats            g_stats = { STAT_PENDING, STAT_PENDING, 0, 0, STAT_PENDING, STAT_PENDING };
static CRITICAL_SECTION g_statsLock;
static HANDLE           g_stopEvent = NULL;
static HWND             g_hwnd      = NULL;

// =============================================================================
//  2. SAMPLER THREAD   (all hardware access lives here, once per kPollMs)
// =============================================================================
static inline ULONGLONG FtU64(const FILETIME& f)
{
    return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

// Identify a physical GPU engine from a PDH instance name such as
//   "pid_4321_luid_0x00000000_0x0000A1B2_phys_0_eng_0_engtype_3D"
// The "pid_<n>_" prefix is skipped, so one engine used by several processes
// maps to one key.  64-bit FNV-1a hash of the remainder.
static ULONGLONG EngineKey(const WCHAR* name)
{
    const WCHAR* p = name;
    for (const WCHAR* q = name; *q; ++q)
    {
        if (q[0] == L'l' && q[1] == L'u' && q[2] == L'i' && q[3] == L'd' && q[4] == L'_')
        {
            p = q;
            break;
        }
    }
    ULONGLONG h = 0xcbf29ce484222325ULL;
    for (; *p; ++p)
    {
        h ^= (ULONGLONG)(WORD)*p;
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Reads the wildcard counter, sums utilisation per physical engine across all
// processes and returns the busiest engine in percent (like Task Manager).
// Returns STAT_PENDING if the sample is not usable yet.
static int ReadGpuPercent(PDH_HCOUNTER counter)
{
    DWORD bytes = 0, items = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &items, NULL) != PDH_MORE_DATA || bytes == 0)
        return STAT_PENDING;

    bytes += bytes / 4 + 1024;   // headroom: new processes can appear between the two calls
    BYTE* buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!buf)
        return STAT_PENDING;

    int result = STAT_PENDING;
    DWORD cap  = bytes;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &cap, &items,
                                                 (PPDH_FMT_COUNTERVALUE_ITEM_W)buf);
    if (st == ERROR_SUCCESS)
    {
        struct Eng { ULONGLONG key; double sum; } eng[96];
        int n = 0;

        PPDH_FMT_COUNTERVALUE_ITEM_W it = (PPDH_FMT_COUNTERVALUE_ITEM_W)buf;
        for (DWORD i = 0; i < items; ++i)
        {
            const DWORD cs = it[i].FmtValue.CStatus;
            if (cs != (DWORD)PDH_CSTATUS_VALID_DATA && cs != (DWORD)PDH_CSTATUS_NEW_DATA)
                continue;

            const ULONGLONG key = EngineKey(it[i].szName);
            int j = 0;
            while (j < n && eng[j].key != key) ++j;
            if (j == n)
            {
                if (n == 96) continue;            // more engines than we track: ignore extras
                eng[n].key = key;
                eng[n].sum = 0.0;
                ++n;
            }
            eng[j].sum += it[i].FmtValue.doubleValue;
        }

        double best = 0.0;
        for (int j = 0; j < n; ++j)
            if (eng[j].sum > best) best = eng[j].sum;
        if (best > 100.0) best = 100.0;
        result = (int)(best + 0.5);
    }
    HeapFree(GetProcessHeap(), 0, buf);           // don't keep the (possibly large) buffer around
    return result;
}

// Number of frames the Desktop Window Manager has composed since it started.
static bool DwmFrameCount(ULONGLONG* out)
{
    DWM_TIMING_INFO ti;
    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(ti);
    if (FAILED(DwmGetCompositionTimingInfo(NULL, &ti)))
        return false;
    *out = (ULONGLONG)ti.cFrame;
    return true;
}

static DWORD WINAPI SamplerThread(LPVOID)
{
    // ---- baselines ----------------------------------------------------------
    FILETIME fi, fk, fu;
    GetSystemTimes(&fi, &fk, &fu);
    ULONGLONG pIdle = FtU64(fi), pKern = FtU64(fk), pUser = FtU64(fu);

    PDH_HQUERY   query   = NULL;
    PDH_HCOUNTER counter = NULL;
    bool gpuOk = false;
    if (PdhOpenQueryW(NULL, 0, &query) == ERROR_SUCCESS)
    {
        if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) == ERROR_SUCCESS)
        {
            PdhCollectQueryData(query);           // first sample only primes the rate counter
            gpuOk = true;
        }
    }
    int lastGpu = gpuOk ? (int)STAT_PENDING : (int)STAT_NA;

    LARGE_INTEGER freq, tPrev;
    QueryPerformanceFrequency(&freq);
    ULONGLONG fPrev = 0;
    const bool fpsOk = DwmFrameCount(&fPrev);
    QueryPerformanceCounter(&tPrev);

    // ---- once per kPollMs until asked to stop -------------------------------
    while (WaitForSingleObject(g_stopEvent, kPollMs) == WAIT_TIMEOUT)
    {
        Stats s = { STAT_PENDING, STAT_PENDING, 0, 0, STAT_PENDING, STAT_PENDING };

        // CPU: busy = (kernel + user) - idle   (kernel time already includes idle)
        GetSystemTimes(&fi, &fk, &fu);
        const ULONGLONG idle = FtU64(fi), kern = FtU64(fk), user = FtU64(fu);
        ULONGLONG dIdle  = idle - pIdle;
        ULONGLONG dTotal = (kern - pKern) + (user - pUser);
        if (dIdle > dTotal) dIdle = dTotal;
        s.cpu = dTotal ? (int)(((dTotal - dIdle) * 100 + dTotal / 2) / dTotal) : 0;
        pIdle = idle; pKern = kern; pUser = user;

        // RAM
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms))
        {
            const ULONGLONG GiB = 1ULL << 30;
            s.ramPct      = (int)ms.dwMemoryLoad;
            s.ramTotalX10 = (UINT)((ms.ullTotalPhys * 10 + GiB / 2) / GiB);
            s.ramUsedX10  = (UINT)(((ms.ullTotalPhys - ms.ullAvailPhys) * 10 + GiB / 2) / GiB);
        }

        // GPU
        if (gpuOk)
        {
            if (PdhCollectQueryData(query) == ERROR_SUCCESS)
            {
                const int g = ReadGpuPercent(counter);
                if (g >= 0) lastGpu = g;          // keep the last good value on a bad sample
            }
            s.gpu = lastGpu;
        }
        else
        {
            s.gpu = STAT_NA;
        }

        // FPS: DWM frames composed per second (QPC-timed, so timer jitter doesn't matter)
        ULONGLONG fNow = 0;
        if (fpsOk && DwmFrameCount(&fNow))
        {
            LARGE_INTEGER tNow;
            QueryPerformanceCounter(&tNow);
            const ULONGLONG elapsed = (ULONGLONG)(tNow.QuadPart - tPrev.QuadPart);
            if (elapsed)
            {
                ULONGLONG fps = ((fNow - fPrev) * (ULONGLONG)freq.QuadPart + elapsed / 2) / elapsed;
                if (fps > 999) fps = 999;
                s.fps = (int)fps;
            }
            fPrev = fNow;
            tPrev = tNow;
        }
        else
        {
            s.fps = fpsOk ? (int)STAT_PENDING : (int)STAT_NA;
        }

        // publish + wake the UI thread
        EnterCriticalSection(&g_statsLock);
        g_stats = s;
        LeaveCriticalSection(&g_statsLock);
        PostMessageW(g_hwnd, WM_APP_STATS, 0, 0);
    }

    if (query) PdhCloseQuery(query);
    return 0;
}

// =============================================================================
//  3. RENDERER   (UI thread only)
//     GDI+ draws straight into a 32-bit premultiplied-ARGB DIB section which is
//     handed to UpdateLayeredWindow -> real per-pixel transparency, crisp AA.
// =============================================================================
static HDC         g_memDC  = NULL;
static HBITMAP     g_dib    = NULL;
static HGDIOBJ     g_dibOld = NULL;
static Bitmap*     g_bmp    = NULL;
static Graphics*   g_gfx    = NULL;
static FontFamily* g_family = NULL;

static int   g_winW = 0, g_winH = 0;
static POINT g_winPos = { 0, 0 };
static bool  g_visible = true;

static REAL g_scale = 1.0f, g_em = 15.0f, g_emHead = 12.0f, g_outline = 3.0f, g_shadow = 1.5f;
static REAL g_padX = 6.0f, g_padY = 6.0f, g_valueX = 40.0f, g_lineH = 18.0f, g_headH = 14.0f, g_headGap = 2.0f;

static int CeilInt(REAL v)
{
    const int i = (int)v;
    return ((REAL)i < v) ? i + 1 : i;
}

static bool InitRenderer()
{
    // DPI scale (the process is system-DPI-aware, so this is the real pixel density)
    HDC screen = GetDC(NULL);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ReleaseDC(NULL, screen);

    g_scale   = (REAL)dpi / 96.0f;
    g_em      = kFontPx * g_scale;
    g_emHead  = g_em * kHeaderScale;
    g_outline = kOutlinePx * g_scale;
    g_shadow  = kShadowPx * g_scale;

    // Font: Consolas (monospace keeps the columns steady), generic monospace as fallback
    g_family = new FontFamily(L"Consolas");
    if (!g_family) return false;
    if (g_family->GetLastStatus() != Ok)
    {
        delete g_family;
        g_family = FontFamily::GenericMonospace()->Clone();
        if (!g_family || g_family->GetLastStatus() != Ok) return false;
    }

    // Line metrics
    const INT bold = FontStyleBold;
    REAL emH = (REAL)g_family->GetEmHeight(bold);
    if (emH < 1.0f) emH = 1.0f;
    const REAL spacing = (REAL)g_family->GetLineSpacing(bold);
    g_lineH   = spacing * g_em     / emH;
    g_headH   = spacing * g_emHead / emH;
    g_headGap = 2.0f * g_scale;
    g_padX = g_padY = g_outline * 0.5f + g_shadow + 3.0f * g_scale;   // room for outline + shadow

    // Measure the widest label / value once, so the window size never changes
    REAL labelW = 0.0f, valueW = 0.0f;
    {
        Bitmap   tmp(1, 1, PixelFormat32bppPARGB);
        Graphics mg(&tmp);
        Font     font(g_family, g_em, bold, UnitPixel);
        RectF    box;
        mg.MeasureString(L"RAM", -1, &font, PointF(0.0f, 0.0f), StringFormat::GenericTypographic(), &box);
        labelW = box.Width;
        mg.MeasureString(L"000.0 / 000.0 GB (100%)", -1, &font, PointF(0.0f, 0.0f), StringFormat::GenericTypographic(), &box);
        valueW = box.Width;
    }
    g_valueX = g_padX + labelW + g_em * 0.9f;
    g_winW   = CeilInt(g_valueX + valueW + g_padX);
    g_winH   = CeilInt(g_padY + g_headH + g_headGap + 4.0f * g_lineH + g_padY);

    // 32-bit top-down DIB section (a few KB-sized canvas: ~90 KB at 96 DPI)
    g_memDC = CreateCompatibleDC(NULL);
    if (!g_memDC) return false;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_winW;
    bi.bmiHeader.biHeight      = -g_winH;          // negative height = top-down rows
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    g_dib = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!g_dib || !bits) return false;
    g_dibOld = SelectObject(g_memDC, g_dib);

    // GDI+ draws directly into the DIB pixels (premultiplied ARGB), which is
    // exactly what UpdateLayeredWindow(AC_SRC_ALPHA) wants.
    g_bmp = new Bitmap(g_winW, g_winH, g_winW * 4, PixelFormat32bppPARGB, (BYTE*)bits);
    g_gfx = new Graphics(g_bmp);
    if (g_gfx->GetLastStatus() != Ok) return false;
    g_gfx->SetSmoothingMode(SmoothingModeAntiAlias);
    return true;
}

static void ShutdownRenderer()
{
    delete g_gfx;    g_gfx    = NULL;      // GDI+ objects must die before GdiplusShutdown()
    delete g_bmp;    g_bmp    = NULL;
    delete g_family; g_family = NULL;
    if (g_memDC)
    {
        if (g_dibOld) SelectObject(g_memDC, g_dibOld);
        DeleteDC(g_memDC);
        g_memDC = NULL;
    }
    if (g_dib) { DeleteObject(g_dib); g_dib = NULL; }
}

// One string: soft shadow, then dark outline, then bright fill on top.
static void DrawOutlined(const WCHAR* text, REAL em, INT style, REAL x, REAL y, const Color& fill)
{
    GraphicsPath path(FillModeWinding);
    path.AddString(text, -1, g_family, style, em, PointF(x, y), StringFormat::GenericTypographic());

    // 1) drop shadow (shifted copy)
    {
        Pen        shadowPen(Color(110, 0, 0, 0), g_outline);
        SolidBrush shadowBrush(Color(110, 0, 0, 0));
        shadowPen.SetLineJoin(LineJoinRound);
        g_gfx->TranslateTransform(g_shadow, g_shadow);
        g_gfx->DrawPath(&shadowPen, &path);
        g_gfx->FillPath(&shadowBrush, &path);
        g_gfx->ResetTransform();
    }
    // 2) dark outline
    {
        Pen outlinePen(Color(235, 0, 0, 0), g_outline);
        outlinePen.SetLineJoin(LineJoinRound);
        g_gfx->DrawPath(&outlinePen, &path);
    }
    // 3) fill
    {
        SolidBrush fillBrush(fill);
        g_gfx->FillPath(&fillBrush, &path);
    }
}

static void Row(const WCHAR* label, const WCHAR* value, REAL y, const Color& valueColor)
{
    DrawOutlined(label, g_em, FontStyleRegular, g_padX,   y, Color(235, 196, 206, 216));
    DrawOutlined(value, g_em, FontStyleBold,    g_valueX, y, valueColor);
}

static void FmtPercent(WCHAR* out, int v)
{
    if (v == STAT_NA)  wsprintfW(out, L"N/A");
    else if (v < 0)    wsprintfW(out, L"--");
    else               wsprintfW(out, L"%d%%", v);
}

// White normally, amber when busy, red when hot.   Color(A, R, G, B)
static Color LoadColor(int pct)
{
    if (pct >= 90) return Color(255, 255,  96,  96);
    if (pct >= 70) return Color(255, 255, 196,  64);
    return Color(255, 255, 255, 255);
}

static void RenderFrame(const Stats& s)
{
    g_gfx->Clear(Color(0, 0, 0, 0));                          // fully transparent canvas

    DrawOutlined(L"CAT's eye", g_emHead, FontStyleBold, g_padX, g_padY, Color(215, 255, 208, 96));

    REAL  y = g_padY + g_headH + g_headGap;
    WCHAR b[64];

    FmtPercent(b, s.cpu);
    Row(L"CPU", b, y, LoadColor(s.cpu));
    y += g_lineH;

    if (s.ramPct < 0) wsprintfW(b, L"--");
    else wsprintfW(b, L"%u.%u / %u.%u GB (%d%%)",
                   s.ramUsedX10 / 10, s.ramUsedX10 % 10,
                   s.ramTotalX10 / 10, s.ramTotalX10 % 10, s.ramPct);
    Row(L"RAM", b, y, LoadColor(s.ramPct));
    y += g_lineH;

    FmtPercent(b, s.gpu);
    Row(L"GPU", b, y, LoadColor(s.gpu));
    y += g_lineH;

    if (s.fps == STAT_NA)  wsprintfW(b, L"N/A");
    else if (s.fps < 0)    wsprintfW(b, L"--");
    else                   wsprintfW(b, L"%d", s.fps);
    Row(L"FPS", b, y, Color(255, 255, 255, 255));
}

// Push the DIB to the screen (per-pixel alpha).
static void Present()
{
    g_gfx->Flush(FlushIntentionSync);

    POINT src  = { 0, 0 };
    SIZE  size = { g_winW, g_winH };
    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_hwnd, NULL, &g_winPos, &size, g_memDC, &src, 0, &bf, ULW_ALPHA);
}

static void ComputePosition()
{
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    const int m  = (int)(kMarginPx * g_scale + 0.5f);
    g_winPos.x = (kCorner == 1 || kCorner == 3) ? sw - g_winW - m : m;
    g_winPos.y = (kCorner == 2 || kCorner == 3) ? sh - g_winH - m : m;
}

// Games / other topmost windows can steal the top of the topmost band; take it back.
static void KeepOnTop()
{
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Move idle pages to the standby list so the Working Set stays tiny between ticks.
static void TrimWorkingSet()
{
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static Stats SnapshotStats()
{
    Stats s;
    EnterCriticalSection(&g_statsLock);
    s = g_stats;
    LeaveCriticalSection(&g_statsLock);
    return s;
}

// =============================================================================
//  4. WINDOW + ENTRY POINT
// =============================================================================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_APP_STATS:                       // once per second, from the sampler thread
        if (g_visible)
        {
            RenderFrame(SnapshotStats());
            Present();
            KeepOnTop();
            TrimWorkingSet();
        }
        return 0;

    case WM_HOTKEY:
        if ((int)wp == HK_QUIT)
        {
            DestroyWindow(hwnd);
        }
        else if ((int)wp == HK_TOGGLE)
        {
            g_visible = !g_visible;
            if (g_visible)
            {
                RenderFrame(SnapshotStats());
                Present();
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            }
            else
            {
                ShowWindow(hwnd, SW_HIDE);
            }
        }
        return 0;

    case WM_DISPLAYCHANGE:                   // resolution / primary monitor changed
        ComputePosition();
        if (g_visible) Present();
        return 0;

    case WM_NCHITTEST:                       // belt and braces on top of WS_EX_TRANSPARENT
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnableDpiAwareness()
{
    typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    PFN_SetProcessDPIAware fn = (PFN_SetProcessDPIAware)GetProcAddress(user32, "SetProcessDPIAware");
    if (fn) fn();
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    EnableDpiAwareness();

    // Single instance
    HANDLE mutex = CreateMutexW(NULL, FALSE, kMutexName);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }

    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);   // never compete with the game

    GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    if (GdiplusStartup(&gdiToken, &gsi, NULL) != Ok)
    {
        MessageBoxW(NULL, L"GDI+ failed to start.", kAppName, MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }
    InitializeCriticalSection(&g_statsLock);                // before any window / thread exists

    int rc = 1;
    do
    {
        if (!InitRenderer())
        {
            MessageBoxW(NULL, L"Could not create the render surface.", kAppName, MB_ICONERROR);
            break;
        }

        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = kAppName;                        // class name: "CAT's eye"
        if (!RegisterClassExW(&wc)) break;

        ComputePosition();
        g_hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kAppName, kAppName, WS_POPUP,
            g_winPos.x, g_winPos.y, g_winW, g_winH,
            NULL, NULL, hInst, NULL);
        if (!g_hwnd) break;

        RenderFrame(g_stats);                               // first frame: "--" placeholders
        Present();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

        const BOOL hkQuit   = RegisterHotKey(g_hwnd, HK_QUIT,   kHotMods, kHotQuit);
        const BOOL hkToggle = RegisterHotKey(g_hwnd, HK_TOGGLE, kHotMods, kHotToggle);

        g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE worker = CreateThread(NULL, 0, SamplerThread, NULL, 0, NULL);
        if (worker) SetThreadPriority(worker, THREAD_PRIORITY_BELOW_NORMAL);

        TrimWorkingSet();

        if (!hkQuit)
            MessageBoxW(NULL,
                L"Ctrl+Alt+F11 is already used by another program, so it cannot close CAT's eye.\n"
                L"Quit it by ending CATs_eye.exe in Task Manager (or change kHotQuit in the source).",
                kAppName, MB_ICONWARNING);
        if (!hkToggle)
            MessageBoxW(NULL,
                L"Ctrl+Alt+F10 is already used by another program, so it cannot toggle CAT's eye.\n"
                L"Change kHotToggle in the source to use a different key.",
                kAppName, MB_ICONWARNING);

        MSG msg;
        msg.wParam = 0;
        while (GetMessageW(&msg, NULL, 0, 0) > 0)           // blocks: 0% CPU between ticks
            DispatchMessageW(&msg);
        rc = (int)msg.wParam;

        SetEvent(g_stopEvent);
        if (worker) { WaitForSingleObject(worker, 3000); CloseHandle(worker); }
        CloseHandle(g_stopEvent);
    } while (0);

    ShutdownRenderer();
    DeleteCriticalSection(&g_statsLock);
    GdiplusShutdown(gdiToken);
    CloseHandle(mutex);
    return rc;
}
