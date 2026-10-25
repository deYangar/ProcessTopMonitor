#pragma once
#include <cstdio>
#include <windows.h>
#include <shellscalingapi.h>

// DLL's HINSTANCE (captured in plugin_main.cpp DllMain)
extern HINSTANCE s_dll_hinst;

// Debug output directory: <dll dir>\ProcessTopMonitor\debug\ — everything
// plugin-related stays in one place next to the config folder (settings.json
// lives in <dll dir>\ProcessTopMonitor\). ALL diagnostic outputs go here:
// crash.log, crash.dmp, werdumps/, etw_file.log.
// Pure Win32 + manual loop: safe to call from the zero-CRT invalid-parameter
// handler.
inline bool PTM_GetDebugDir(wchar_t* buf, size_t cap) {
    if (!buf || cap < 64) return false;
    buf[0] = 0;
    HINSTANCE h = s_dll_hinst;
    DWORD n = h ? GetModuleFileNameW(h, buf, (DWORD)cap) : 0;
    if (n == 0 || n >= cap) return false;
    size_t last = (size_t)-1;
    for (size_t i = 0; i < n; i++) if (buf[i] == L'\\') last = i;
    if (last == (size_t)-1) return false;
    // 1) <dll dir>\ProcessTopMonitor (config folder - may not exist yet)
    static const wchar_t sub1[] = L"\\ProcessTopMonitor";
    size_t sub1len = (sizeof(sub1) / sizeof(wchar_t)) - 1;
    if (last + sub1len >= cap) return false;
    memcpy(buf + last, sub1, (sub1len + 1) * sizeof(wchar_t));
    CreateDirectoryW(buf, nullptr);
    // 2) <dll dir>\ProcessTopMonitor\debug
    static const wchar_t sub2[] = L"\\debug";
    size_t sub2len = (sizeof(sub2) / sizeof(wchar_t)) - 1;
    size_t cur = last + sub1len;
    if (cur + sub2len >= cap) return false;
    memcpy(buf + cur, sub2, (sub2len + 1) * sizeof(wchar_t));
    CreateDirectoryW(buf, nullptr);
    return true;
}

// Posted to top-list windows from the refresh timer thread: the window then
// pulls the latest snapshot on ITS OWN (UI) thread — never touch window
// members (std::vector etc.) from another thread (heap corruption risk).
#define WM_PTM_REFRESH (WM_APP + 1)
#define WM_PTM_LANG_CHANGED (WM_APP + 6)
// Posted (possibly from the tick thread) to a live window: run WindowManager
// dead-window reclamation on the UI thread (vector mutation is UI-only)
#define WM_PTM_RECLAIM (WM_APP + 7)

// Measure text width (pixels) with a given font. DPI-native by definition.
inline int PTM_MeasureText(HDC hdc, HFONT hFont, const wchar_t* text) {
    if (!text || !text[0]) return 0;
    HFONT hOld = hFont ? (HFONT)SelectObject(hdc, hFont) : nullptr;
    SIZE sz = {};
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
    if (hOld) SelectObject(hdc, hOld);
    return sz.cx;
}

// ---- shared value formatting (raw values kept, no premature rounding of
// the underlying doubles beyond display units) ----

inline void FormatSpeed(double bps, wchar_t* buf, int n) {
    if (bps < 0.01) wcsncpy_s(buf, n, L"0 B/s", _TRUNCATE);
    else if (bps < 1024) swprintf_s(buf, n, L"%.0f B/s", bps);
    else if (bps < 1048576) swprintf_s(buf, n, L"%.1f KB/s", bps / 1024.0);
    else swprintf_s(buf, n, L"%.2f MB/s", bps / 1048576.0);
}

// Memory amount: MB/GB with the raw fractional value shown
inline void FormatMem(uint64_t bytes, wchar_t* buf, int n) {
    double mb = bytes / 1048576.0;
    if (mb < 1024.0) swprintf_s(buf, n, L"%.0f MB", mb);
    else swprintf_s(buf, n, L"%.2f GB", mb / 1024.0);
}

inline void FormatBytes(uint64_t bytes, wchar_t* buf, int n) {
    if (bytes == 0) wcsncpy_s(buf, n, L"0 B", _TRUNCATE);
    else if (bytes < 1024ULL) swprintf_s(buf, n, L"%llu B", bytes);
    else if (bytes < 1048576ULL) swprintf_s(buf, n, L"%.1f KB", bytes / 1024.0);
    else if (bytes < 1073741824ULL) swprintf_s(buf, n, L"%.2f MB", bytes / 1048576.0);
    else swprintf_s(buf, n, L"%.2f GB", bytes / 1073741824.0);
}

// ============================================================
// OS version gate
// RtlGetVersion via GetProcAddress: the truth-telling API (no
// VerifyVersionInfo compatibility lie), and never enters the import
// table so the DLL keeps loading on any Windows.
// ============================================================

// OS build number (9200 = Win8, 10240 = Win10 1507, 16299 = 1709, ...).
// False only if RtlGetVersion itself is unavailable.
inline bool PTM_GetOSBuild(DWORD& build) {
    typedef LONG(WINAPI* PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
    static DWORD cached = 0;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        auto fn = (PFN_RtlGetVersion)GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        RTL_OSVERSIONINFOW vi = { sizeof(vi) };
        if (fn && fn(&vi) == 0) cached = vi.dwBuildNumber;
    }
    build = cached;
    return cached != 0;
}

// Multiple system logger sessions exist from Win8 (build 9200): our own
// EVENT_TRACE_SYSTEM_LOGGER_MODE FileIo session can coexist with PNM's
// NT Kernel Logger. Below Win8 only the single NT Kernel Logger exists.
inline bool PTM_IsWin8Plus() {
    DWORD build = 0;
    return PTM_GetOSBuild(build) && build >= 9200;
}

// SWCA accent blur/acrylic (ACCENT_ENABLE_BLURBEHIND/ACRYLICBLURBEHIND)
// actually renders starting with Win10 1703 (build 15063). Below that the
// popup's acrylic/blur backdrops silently degrade to plain transparency.
inline bool PTM_IsAccentBlurSupported() {
    DWORD build = 0;
    return PTM_GetOSBuild(build) && build >= 15063;
}

// ============================================================
// DPI / multi-monitor helpers
// Same approach as TrafficMonitor's own DPIFromRect:
// MonitorFromRect + shcore!GetDpiForMonitor(MDT_EFFECTIVE_DPI).
// Works per-monitor regardless of host process DPI awareness.
// ============================================================

typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);

inline PFN_GetDpiForMonitor PTM_GetDpiForMonitorProc() {
    static PFN_GetDpiForMonitor pfn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore)
            pfn = (PFN_GetDpiForMonitor)GetProcAddress(shcore, "GetDpiForMonitor");
    }
    return pfn;
}

// DPI scale (1.0 = 100%) for a given monitor
inline float PTM_GetDpiScaleForMonitor(HMONITOR mon) {
    if (!mon) return 1.0f;
    auto pfn = PTM_GetDpiForMonitorProc();
    if (pfn) {
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(pfn(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) && dy > 0)
            return (float)dy / 96.0f;
    }
    return 1.0f;
}

// DPI scale for the monitor that contains (or is nearest to) the given rect
inline float PTM_GetDpiScaleForRect(const RECT& rc) {
    return PTM_GetDpiScaleForMonitor(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST));
}

// Work area of the monitor that contains (or is nearest to) the given rect
inline bool PTM_GetWorkAreaForRect(const RECT& rc, RECT& out_work, RECT& out_monitor) {
    HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!mon || !GetMonitorInfoW(mon, &mi)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &out_work, 0);
        out_monitor = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        return false;
    }
    out_work = mi.rcWork;
    out_monitor = mi.rcMonitor;
    return true;
}
