// plugin_main.cpp — ITMPlugin implementation + data pipeline + crash guards
// Skeleton disciplines inherited from PNM (issue-level lessons kept):
//   - crash handler + VEH + invalid-param handler + WER LocalDumps, all
//     installed in InitOnce BEFORE the first window is created (issue #7)
//   - InitOnce double entry (OnInitialize / EI_CONFIG_DIR / first
//     DataRequired) for 1.85/1.86 host differences (issue #12)
//   - TimerQueue tick with CAS re-entrancy guard; cross-thread data only
//     via the m_data_lock snapshot + PostMessage (0xc0000374 lessons)
//   - double-buffered tooltip text (torn-read protection)
#include "plugin_main.h"
#include "options_dialog.h"
#include "utils.h"
#include <shellapi.h>
#include <dbghelp.h>
#include <commctrl.h>

#pragma comment(lib, "dbghelp.lib")

// ============================================================
// crash diagnostics (PNM-inherited, zero-CRT logging path)
// ============================================================

static LONG WINAPI PtmCrashHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    static const DWORD serious[] = {
        0xC0000005, 0xC000001D, 0xC000008C, 0xC0000094, 0xC00000FD,
        0xC0000374, 0xC0000409, 0xC0000139, 0xC0000135, 0xC0000142,
    };
    bool serious_crash = false;
    for (DWORD c : serious) if (code == c) { serious_crash = true; break; }
    if (!serious_crash) return EXCEPTION_CONTINUE_SEARCH;

    // de-dupe: same code+address within 10s (VEH sees first-chance too)
    static DWORD s_last_code = 0;
    static void* s_last_addr = nullptr;
    static ULONGLONG s_last_tick = 0;
    ULONGLONG now = GetTickCount64();
    if (code == s_last_code && ep->ExceptionRecord->ExceptionAddress == s_last_addr &&
        now - s_last_tick < 10000)
        return EXCEPTION_CONTINUE_SEARCH;
    s_last_code = code;
    s_last_addr = ep->ExceptionRecord->ExceptionAddress;
    s_last_tick = now;

    // crash.log FIRST (pure Win32): the original exception must be captured
    // before MiniDumpWriteDump, which can failfast again and lose everything
    void* stack[24] = {};
    USHORT nstack = CaptureStackBackTrace(2, 24, stack, nullptr);
    auto hexw = [](wchar_t* out, unsigned long long v, int nd) {
        for (int i = nd - 1; i >= 0; i--) { out[i] = L"0123456789abcdef"[v & 0xF]; v >>= 4; }
        out[nd] = 0;
    };
    auto decw = [](wchar_t* out, unsigned long long v) {
        wchar_t tmp[24]; int i = 0;
        if (v == 0) tmp[i++] = L'0';
        while (v && i < 23) { tmp[i++] = L"0123456789"[v % 10]; v /= 10; }
        for (int j = 0; j < i; j++) out[j] = tmp[i - 1 - j];
        out[i] = 0;
    };
    wchar_t dir[MAX_PATH] = L"";
    if (PTM_GetDebugDir(dir, MAX_PATH)) {
        size_t dlen = wcslen(dir);
        static const wchar_t lsuf[] = L"\\crash.log";
        size_t llen = (sizeof(lsuf) / sizeof(wchar_t)) - 1;
        if (dlen + llen < MAX_PATH) {
            wchar_t logpath[MAX_PATH];
            memcpy(logpath, dir, dlen * sizeof(wchar_t));
            memcpy(logpath + dlen, lsuf, (llen + 1) * sizeof(wchar_t));
            HANDLE hLog = CreateFileW(logpath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                      nullptr, OPEN_ALWAYS, 0, nullptr);
            if (hLog != INVALID_HANDLE_VALUE) {
                auto wr = [&](const wchar_t* s) {
                    DWORD w = 0;
                    WriteFile(hLog, s, (DWORD)wcslen(s) * sizeof(wchar_t), &w, nullptr);
                };
                SYSTEMTIME st; GetLocalTime(&st);
                wchar_t hh[4], mm[4], ss[4], cx[9], ax[17], th[12];
                decw(hh, st.wHour); decw(mm, st.wMinute); decw(ss, st.wSecond);
                hexw(cx, code, 8);
                hexw(ax, (unsigned long long)ep->ExceptionRecord->ExceptionAddress, 16);
                decw(th, GetCurrentThreadId());
                wr(L"["); wr(hh); wr(L":"); wr(mm); wr(L":"); wr(ss);
                wr(L"] CRASH code=0x"); wr(cx);
                wr(L" addr=0x"); wr(ax);
                wr(L" thread="); wr(th);
                for (USHORT si = 0; si < nstack; si++) {
                    wr(L" st=");
                    wchar_t sx[17];
                    hexw(sx, (unsigned long long)stack[si], 16);
                    wr(sx);
                }
                wr(L"\n");
                CloseHandle(hLog);
            }
        }
        static const wchar_t dsuf[] = L"\\crash.dmp";
        size_t dlen2 = (sizeof(dsuf) / sizeof(wchar_t)) - 1;
        if (dlen + dlen2 < MAX_PATH) {
            wchar_t dmppath[MAX_PATH];
            memcpy(dmppath, dir, dlen * sizeof(wchar_t));
            memcpy(dmppath + dlen, dsuf, (dlen2 + 1) * sizeof(wchar_t));
            HANDLE hFile = CreateFileW(dmppath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                                  MiniDumpNormal, &mei, nullptr, nullptr);
                CloseHandle(hFile);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// CRT invalid-parameter handler: log + failfast exactly like the CRT would
static void PtmInvalidParamHandler(const wchar_t* expr, const wchar_t* func,
                                   const wchar_t* file, unsigned line, uintptr_t) {
    static volatile LONG s_in_handler = 0;
    if (InterlockedIncrement(&s_in_handler) > 1)
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    void* stack[24] = {};
    USHORT nstack = CaptureStackBackTrace(2, 24, stack, nullptr);
    wchar_t path[MAX_PATH] = L"";
    if (PTM_GetDebugDir(path, MAX_PATH) && path[0]) {
        static const wchar_t suffix[] = L"\\crash.log";
        size_t plen = wcslen(path);
        size_t slen = (sizeof(suffix) / sizeof(wchar_t)) - 1;
        if (plen + slen < MAX_PATH) {
            wchar_t full[MAX_PATH];
            memcpy(full, path, plen * sizeof(wchar_t));
            memcpy(full + plen, suffix, (slen + 1) * sizeof(wchar_t));
            HANDLE hFile = CreateFileW(full, FILE_APPEND_DATA, FILE_SHARE_READ,
                                       nullptr, OPEN_ALWAYS, 0, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                auto wr = [&](const wchar_t* s) {
                    DWORD w = 0;
                    WriteFile(hFile, s, (DWORD)wcslen(s) * sizeof(wchar_t), &w, nullptr);
                };
                wr(L"INVALID_PARAM expr=");
                wr(expr ? expr : L"(null)");
                wr(L" func=");
                wr(func ? func : L"(null)");
                wr(L" file=");
                wr(file ? file : L"(null)");
                wchar_t lb[16];
                unsigned v = line;
                int i = 0;
                if (v == 0) lb[i++] = L'0';
                while (v && i < 15) { lb[i++] = L"0123456789abcdef"[v & 0xF]; v >>= 4; }
                wchar_t rb[16]; int j = 0;
                while (i > 0) rb[j++] = lb[--i];
                rb[j] = 0;
                wr(L" line=0x");
                wr(rb);
                for (USHORT si = 0; si < nstack; si++) {
                    wr(L" st=");
                    uintptr_t a = (uintptr_t)stack[si];
                    wchar_t hx[20];
                    int hi = 0;
                    do { hx[hi++] = L"0123456789abcdef"[a & 0xF]; a >>= 4; } while (a && hi < 18);
                    wchar_t hr[20]; int hj = 0;
                    while (hi > 0) hr[hj++] = hx[--hi];
                    hr[hj] = 0;
                    wr(hr);
                }
                wr(L"\n");
                CloseHandle(hFile);
            }
        }
    }
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

// WER LocalDumps: kernel-level dumps independent of in-process handlers
static void SetupWerLocalDumps() {
    HKEY hk = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\TrafficMonitor.exe",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr);
    if (st != ERROR_SUCCESS) return;
    wchar_t dump_dir[MAX_PATH] = L"";
    if (PTM_GetDebugDir(dump_dir, MAX_PATH) && dump_dir[0]) {
        wcscat_s(dump_dir, L"\\werdumps");
        CreateDirectoryW(dump_dir, nullptr);
        RegSetValueExW(hk, L"DumpFolder", 0, REG_SZ,
                       (const BYTE*)dump_dir, (DWORD)((wcslen(dump_dir) + 1) * sizeof(wchar_t)));
        DWORD type = 1;   // MiniDump
        RegSetValueExW(hk, L"DumpType", 0, REG_DWORD, (const BYTE*)&type, sizeof(type));
        DWORD count = 3;
        RegSetValueExW(hk, L"DumpCount", 0, REG_DWORD, (const BYTE*)&count, sizeof(count));
    }
    RegCloseKey(hk);
}

void CProcessTopPlugin::InitCrashHandlers() {
    SetUnhandledExceptionFilter(PtmCrashHandler);
    AddVectoredExceptionHandler(1, PtmCrashHandler);
    _set_invalid_parameter_handler(PtmInvalidParamHandler);
    SetupWerLocalDumps();
}

// ============================================================
// CTopItem — taskbar item (CPU Top1)
// ============================================================

wchar_t CTopItem::s_top_name[48] = L"-";
wchar_t CTopItem::s_top_value[32] = L"--";
bool CTopItem::s_show_item = true;
COLORREF CTopItem::s_value_color = 0;
bool CTopItem::s_has_value_color = false;

const wchar_t* CTopItem::GetItemName() const { return L"Top"; }
const wchar_t* CTopItem::GetItemId() const { return L"ProcTop01"; }
const wchar_t* CTopItem::GetItemLableText() const { return L""; }
const wchar_t* CTopItem::GetItemValueText() const { return s_top_value; }
const wchar_t* CTopItem::GetItemValueSampleText() const { return L"chrome.exe 32.4%"; }

int CTopItem::GetItemWidth() const { return 120; }

int CTopItem::GetItemWidthEx(void* hDC) const {
    HDC hdc = (HDC)hDC;
    const wchar_t* sample = L"abcdefghijkl.. 99.9%";
    SIZE sz{};
    GetTextExtentPoint32W(hdc, sample, (int)wcslen(sample), &sz);
    return sz.cx;
}

void CTopItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) {
    HDC hdc = (HDC)hDC;
    COLORREF color = s_has_value_color ? s_value_color
                                       : (dark_mode ? RGB(255, 255, 255) : RGB(0, 0, 0));
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);

    wchar_t name[48];
    wchar_t value[32];
    wcsncpy_s(name, 48, s_top_name, _TRUNCATE);
    wcsncpy_s(value, 32, s_top_value, _TRUNCATE);

    // 右列：占用，右对齐
    SIZE sz_v{};
    GetTextExtentPoint32W(hdc, value, (int)wcslen(value), &sz_v);
    RECT rc_v = { x + w - sz_v.cx, y, x + w, y + h };
    DrawTextW(hdc, value, -1, &rc_v, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // 左列：Top1 进程名，动态截断防重叠
    int avail = (x + w - sz_v.cx - 3) - x;
    SIZE sz_n{};
    GetTextExtentPoint32W(hdc, name, (int)wcslen(name), &sz_n);
    std::wstring left = name;
    if (sz_n.cx > avail && avail > 0) {
        SIZE sz_dot{};
        GetTextExtentPoint32W(hdc, L"..", 2, &sz_dot);
        while (left.size() > 3) {
            left.pop_back();
            GetTextExtentPoint32W(hdc, left.c_str(), (int)left.size(), &sz_n);
            if (sz_n.cx + sz_dot.cx <= avail) break;
        }
        left += L"..";
    }
    RECT rc_n = { x, y, x + w, y + h };
    DrawTextW(hdc, left.c_str(), (int)left.size(), &rc_n, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

int CTopItem::OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) {
    // M6 §4.3.3（抄 PNM :373-412）：任务栏点击 = pin/unpin；主悬浮窗点击 =
    // hover 态手动切换
    if (type != MT_LCLICKED) return 0;
    CProcessTopPlugin& plugin = CProcessTopPlugin::Instance();
    auto& mgr = WindowManager::Instance();
    bool is_taskbar = (flag & MF_TASKBAR_WND) != 0;

    if (is_taskbar) {
        if (plugin.m_settings.popup_mode == 1) {
            // 常驻模式：点任务栏 item = 临时取消常驻（HideGroup），再点
            // 恢复——语义与 hover 模式的 pin 对称（M6 §4.3.3）
            if (mgr.GroupVisible()) {
                mgr.HideGroup();
            } else {
                if (mgr.Count() == 0) plugin.RebuildDefaultWindow();
                mgr.ShowGroupAt(plugin.GetStartupAnchor());
            }
            return 1;
        }
        if (mgr.GroupVisible() && plugin.m_group_pinned) {
            // 再点 = 取消 pin + 收回（回 hover 态）
            plugin.m_group_pinned = false;
            mgr.HideGroup();
        } else {
            if (mgr.Count() == 0) plugin.RebuildDefaultWindow();
            // 锚 = 点击点 ±60px 带（ClientToScreen + GetWindowRect 钳位，
            // PNM :389-397 写法照抄）
            POINT pt = { x, y };
            ClientToScreen((HWND)hWnd, &pt);
            RECT wr;
            GetWindowRect((HWND)hWnd, &wr);
            int half = 60;
            RECT anchor = { pt.x - half, wr.top, pt.x + half, wr.bottom };
            if (anchor.left < wr.left) { anchor.left = wr.left; anchor.right = wr.left + 2 * half; }
            if (anchor.right > wr.right) { anchor.right = wr.right; anchor.left = wr.right - 2 * half; }
            plugin.m_group_pinned = true;
            mgr.ShowGroupAt(anchor);   // pin 态不挂锚跟随、宽限逻辑跳过
        }
        return 1;
    }
    // 主悬浮窗：hover 态手动切换（GroupVisible ? Hide : Show；M6 §4.3.3）
    plugin.m_group_pinned = false;
    if (mgr.GroupVisible()) {
        mgr.HideGroup();
    } else {
        if (mgr.Count() == 0) plugin.RebuildDefaultWindow();
        RECT anchor;
        GetWindowRect((HWND)hWnd, &anchor);
        mgr.ShowGroupAt(anchor, (HWND)hWnd);
    }
    return 1;
}

// ============================================================
// TM window classification (M6 §4.3.1，照抄 PNM plugin_main.cpp:1097-1221)
// ============================================================

// Check if a TrafficMonitor window is the floating main window (not on taskbar)
static bool IsTMMainWindow(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"TrafficMonitor", 14) != 0) return false;

    RECT rc;
    GetWindowRect(hwnd, &rc);

    APPBARDATA abd = { sizeof(abd) };
    UINT state = SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
    if (!state) return true;

    RECT tb = abd.rc;
    switch (abd.uEdge) {
    case ABE_BOTTOM: return rc.top < tb.top;
    case ABE_TOP:    return rc.bottom > tb.bottom;
    case ABE_LEFT:   return rc.right > tb.right;
    case ABE_RIGHT:  return rc.left < tb.left;
    }
    return true;
}

// Check if a TrafficMonitor window is embedded in the taskbar
static bool IsTMTaskbarWindow(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"TrafficMonitor", 14) != 0) return false;

    RECT rc;
    GetWindowRect(hwnd, &rc);

    APPBARDATA abd = { sizeof(abd) };
    UINT state = SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
    if (!state) return false;  // can't determine taskbar position

    // Check if window overlaps with taskbar
    RECT tb = abd.rc;
    RECT inter;
    return IntersectRect(&inter, &rc, &tb) != 0;
}

// Walk the parent chain to find a TrafficMonitor window.
// Dialogs (class #32770) stop the chain: hovering a settings / connection
// dialog (owned by TM) must NOT trigger our popup. The taskbar dialog is
// the exception - its title contains "TrafficMonitorTaskbarWindow" and we
// keep walking up to its parent (the main TM window).
static HWND FindTMWindowInChain(HWND hwnd) {
    for (HWND cur = hwnd; cur; cur = GetParent(cur)) {
        wchar_t cls[64] = {};
        GetClassNameW(cur, cls, 64);
        if (wcsncmp(cls, L"TrafficMonitor", 14) == 0)
            return cur;
        // Popup menu (#32768): hovering any menu (incl. submenus) must NOT
        // trigger our popup - menus are owned by TM but are separate windows
        if (wcscmp(cls, L"#32768") == 0)
            return nullptr;
        if (wcscmp(cls, L"#32770") == 0) {
            wchar_t txt[256] = {};
            GetWindowTextW(cur, txt, 256);
            // Taskbar dialog: keep walking up to its parent (main TM window)
            if (wcsstr(txt, L"TrafficMonitorTaskbarWindow") != nullptr ||
                wcsstr(txt, L"TrafficMonitor") != nullptr)
                continue;
            // Any other dialog (options, connection details, ...): stop
            return nullptr;
        }
    }
    return nullptr;
}

// EnumWindows 找悬浮主窗（常驻启动/pin 恢复的布局锚用；M6 §4.2.4）
static HWND PtFindTMMainWindow() {
    struct Ctx { HWND found = nullptr; } ctx;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        if (wcsncmp(cls, L"TrafficMonitor", 14) == 0 && IsTMMainWindow(hwnd)) {
            c->found = hwnd;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

// Classify the TM window under the cursor: floating main window or taskbar
// window. Returns nullptr when the cursor is not over any TrafficMonitor
// window. （M6 §4.3.1，照抄 PNM :1187-1218——先查自身再走 parent 链，
// 否则 FindTMWindowInChain 返回 parent（主窗）丢任务栏上下文）
static HWND DetectTMWindow(HWND hover_wnd, bool& is_taskbar) {
    is_taskbar = false;
    if (!hover_wnd) return nullptr;

    // TM's taskbar window is an MFC dialog with class "#32770" and window
    // text "TrafficMonitorTaskbarWindow". Its parent is the main TM window.
    wchar_t wnd_text[256] = {};
    GetWindowTextW(hover_wnd, wnd_text, 256);
    wchar_t hover_cls[64] = {};
    GetClassNameW(hover_wnd, hover_cls, 64);

    if (wcsstr(wnd_text, L"TrafficMonitorTaskbarWindow") != nullptr ||
        (wcscmp(hover_cls, L"#32770") == 0 && wcsstr(wnd_text, L"TrafficMonitor") != nullptr)) {
        HWND parent = GetParent(hover_wnd);
        if (parent) {
            wchar_t pcls[64] = {};
            GetClassNameW(parent, pcls, 64);
            if (wcsncmp(pcls, L"TrafficMonitor", 14) == 0) {
                is_taskbar = true;
                return parent;   // 返回主窗句柄（锚/跟随目标）
            }
        }
    }
    HWND tm = FindTMWindowInChain(hover_wnd);
    if (!tm) return nullptr;
    if (IsTMTaskbarWindow(tm)) { is_taskbar = true; return tm; }
    if (IsTMMainWindow(tm)) return tm;
    return nullptr;
}

// True while a menu opened by this thread (TrafficMonitor's right-click
// menu) is tracking. Our popup windows live on TM's UI thread, so querying
// thread 0 (current) reports exactly TM's menu state - a hard signal that
// does not depend on where the cursor sits. （M6 §4.3.1，抄 PNM :351-356）
static bool IsTMMenuMode() {
    GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
    if (!GetGUIThreadInfo(0, &gti)) return false;
    return (gti.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE)) != 0 ||
           gti.hwndMenuOwner != nullptr;
}

// Hide TM's native tooltip windows right now (same thing CToolTipCtrl::Pop
// does). Upstream pops them synchronously only in the taskbar window's
// right-click handler; the main window relies on a 1 s timer, so its
// tooltip can sit on top of the context menu for up to a second.
// （M6 §4.3.1，抄 PNM :363-371）
static void PopTMTooltips() {
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExW(nullptr, hwnd, L"tooltips_class32", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId())
            SendMessageW(hwnd, TTM_POP, 0, 0);
    }
}

RECT CProcessTopPlugin::GetStartupAnchor() {
    HWND tm = PtFindTMMainWindow();
    if (tm) {
        RECT rc;
        if (GetWindowRect(tm, &rc)) return rc;
    }
    // 兜底：主屏工作区右上虚拟锚（贴顶边 → 组弹下方；常驻用户基本都
    // 有记忆位置，此路径只影响从未拖过的首启）
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    return { wa.right - 200, wa.top, wa.right, wa.top + 40 };
}

// ============================================================
// CProcessTopPlugin
// ============================================================

CProcessTopPlugin CProcessTopPlugin::s_instance;

CProcessTopPlugin::CProcessTopPlugin() {
    // The lock must be valid before ANY code path touches the snapshot:
    // 1.85.x never calls OnInitialize, and the all-zero CRITICAL_SECTION
    // crashes RtlpEnterCriticalSectionContended (PNM issue #12).
    if (!m_lock_inited) {
        InitializeCriticalSection(&m_data_lock);
        m_lock_inited = true;
    }
}

CProcessTopPlugin::~CProcessTopPlugin() {
    StopRefreshTimer();
    if (m_lock_inited) {
        DeleteCriticalSection(&m_data_lock);
        m_lock_inited = false;
    }
}

CProcessTopPlugin& CProcessTopPlugin::Instance() { return s_instance; }
IPluginItem* CProcessTopPlugin::GetItem(int i) {
    if (i == 0) return &m_item;
    return nullptr;
}

const wchar_t* CProcessTopPlugin::GetInfo(PluginInfoIndex i) {
    switch (i) {
    case TMI_NAME: return L"ProcessTopMonitor";
    case TMI_DESCRIPTION: return L"Per-process CPU/memory/disk usage ranking";
    case TMI_AUTHOR: return L"deYangar";
    case TMI_COPYRIGHT: return L"MIT";
    case TMI_VERSION: return L"0.1.0";
    case TMI_URL: return L"https://github.com/deYangar/ProcessTopMonitor";
    default: return L"";
    }
}

void CProcessTopPlugin::OnInitialize(ITrafficMonitor* p) {
    m_app = p;
    if (!m_inited) {
        m_inited = true;
        InitOnce(p->GetPluginConfigDir());
    }
}

void CProcessTopPlugin::EnsureInitialized() {
    if (m_inited) return;
    m_inited = true;
    InitOnce(m_tm_config_dir.empty() ? nullptr : m_tm_config_dir.c_str());
}

void CProcessTopPlugin::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) {
    if (index == EI_CONFIG_DIR && data && data[0]) {
        m_tm_config_dir = data;
        // EI_CONFIG_DIR arrives on the host MAIN thread at load time (both
        // 1.85.x and 1.86): init here, not on the first DataRequired (which
        // fires on TM's monitor worker thread — windows created there get
        // no message pump, PNM issue #12).
        EnsureInitialized();
    } else if (index == EI_VALUE_TEXT_COLOR && data && data[0]) {
        CTopItem::s_value_color = (COLORREF)wcstoul(data, nullptr, 10);
        CTopItem::s_has_value_color = true;
    }
}

void CProcessTopPlugin::OnMonitorInfo(const MonitorInfo& mi) {
    // host tick（TM 自己的线程）：整机 GPU 利用率缓存，G3 摘要兜底用；
    // 传感器三件套（M3-3 L0 白拿）——值为 0 表示 TM 未启用该项监控，
    // 窗口端据此自动隐藏传感器段。只写 atomic int，无锁。
    m_tm_gpu_usage.store((int)mi.gpu_usage);
    m_tm_cpu_temp.store((int)mi.cpu_temperature);
    m_tm_gpu_temp.store((int)mi.gpu_temperature);
    m_tm_cpu_freq.store((int)mi.cpu_freq);
}

void CProcessTopPlugin::InitOnce(const wchar_t* cfg_base) {
    InitCrashHandlers();

    // config dir: <plugins>\ProcessTopMonitor (fallback: DLL dir)
    if (cfg_base && cfg_base[0]) {
        m_config_dir = std::wstring(cfg_base) + L"\\ProcessTopMonitor";
    } else {
        wchar_t dll_path[MAX_PATH] = {};
        GetModuleFileNameW(s_dll_hinst ? s_dll_hinst : (HINSTANCE)GetModuleHandleW(nullptr),
                           dll_path, MAX_PATH);
        std::wstring dll_dir(dll_path);
        size_t slash = dll_dir.find_last_of(L"\\");
        if (slash != std::wstring::npos) dll_dir.resize(slash);
        m_config_dir = dll_dir + L"\\ProcessTopMonitor";
    }

    // settings first: everything below reads them
    PtLoadSettings(m_config_dir, m_settings);
    ApplySettingsToCollectors();
    EtwFileCollector::SetDebugLogs(m_settings.debug_logs);
    m_gpu.ProbeGrade();   // OS-version gate: menu gray-state on G3 systems

    // ---- i18n (PNM-inherited): scan <config>\lang\, auto-follow TM ----
    {
        wchar_t dll_path[MAX_PATH] = {};
        GetModuleFileNameW(s_dll_hinst ? s_dll_hinst : (HINSTANCE)GetModuleHandleW(nullptr),
                           dll_path, MAX_PATH);
        std::wstring dll_dir(dll_path);
        size_t slash = dll_dir.find_last_of(L"\\");
        if (slash != std::wstring::npos) {
            std::wstring lang_dir = dll_dir.substr(0, slash) + L"\\ProcessTopMonitor\\lang";
            CreateDirectoryW(lang_dir.c_str(), nullptr);
            I18n::ScanLangFiles(lang_dir);
            const wchar_t* bcp = (m_app ? m_app->GetStringRes(L"BCP_47", L"general") : nullptr);
            I18n::SetTmLang(bcp && bcp[0] ? bcp : L"zh-CN");
            I18n::SetLang(m_settings.lang);
            I18n::Reload();
        }
    }

    // ETW FileIo 会话按需启停（方案 §4.2/§8.3）：InitOnce 不预先开会话，
    // 由 RefreshTick 按可见窗口绑定位集决定——无磁盘 IO 窗口时零内核开销
    // （会话建立 ~10ms，首次切到 IO 视图时启动，可接受）

    // high-frequency refresh timer (independent of TM's 1s tick)
    StartRefreshTimer();

    // ranking windows: crash guards are in place, safe to create now.
    // RestoreAll 按 windows[] 恢复（空配置 = 悬浮窗模式退出，不建窗）；
    // 全新用户默认一扇窗，指标 = default_metric（M6 §3.1 默认 CPU）。
    // M6：窗创建后保持隐藏——hover 模式等状态机弹显，常驻模式直接显示
    WindowManager::Instance().RestoreAll(m_settings.windows);
    if (WindowManager::Instance().Count() == 0) {
        WindowCfg def;
        def.kind = m_settings.default_metric;
        def.topn = m_settings.topn;
        def.io_view = m_settings.io_view;
        WindowManager::Instance().AddWindow(def);
    }
    TopListRefreshFromSnapshot();   // 首拍数据就位（隐藏态只保鲜不上屏）
    // 常驻模式（popup_mode=1）：TM 启动即显示——有记忆位置的窗用记忆位
    // 置，其余组布局（锚=TM 主窗 rect，找不到用工作区兜底）；不挂锚子
    // 类化（常驻不跟锚，M6 §4.2.4）
    if (m_settings.popup_mode == 1 && WindowManager::Instance().Count() > 0) {
        WindowManager::Instance().ShowGroupAt(GetStartupAnchor());
    }
}

void CProcessTopPlugin::ApplySettingsToCollectors() {
    m_metrics.SetMemBasisWorkingSet(m_settings.memory_basis == 1);
    m_gpu.Configure(m_settings.GpuScope());
    // 外观三件套是全局设置：应用到全部窗口（M3-1 方案 §3.4）
    WindowManager::Instance().ForEach([](CTopListWindow& w, int) {
        w.SetAppearance(CProcessTopPlugin::Instance().m_settings.color_mode,
                        CProcessTopPlugin::Instance().m_settings.popup_effect,
                        CProcessTopPlugin::Instance().m_settings.popup_opacity);
    });
}

void CProcessTopPlugin::ApplyGpuScope(const GpuScopeConfig& cfg) {
    // UI 线程（窗口口径菜单）：settings 持久化 + 采集器即时生效
    if (cfg.all_engines || cfg.engines.empty()) {
        m_settings.gpu_engines = L"all";
    } else {
        std::wstring joined;
        for (size_t i = 0; i < cfg.engines.size(); i++) {
            if (i) joined += L",";
            joined += cfg.engines[i];
        }
        m_settings.gpu_engines = joined;
    }
    m_settings.gpu_cap_100 = cfg.cap_100;
    m_settings.vram_basis = cfg.vram_with_shared ? 1 : 0;
    m_settings_dirty = true;
    m_gpu.Configure(cfg);
}

// ============================================================
// hover 状态机 + pin（M6 §4.3.2/§4.3.3，照抄 PNM :1305-1400 全语义，
// popup→group 替换）
// ============================================================

void CProcessTopPlugin::RebuildDefaultWindow() {
    // 全部窗销毁后 item 点击重建默认窗（kind=default_metric，M6 §4.2.5）
    auto& mgr = WindowManager::Instance();
    if (mgr.Count() != 0) return;
    WindowCfg def;
    def.kind = m_settings.default_metric;
    def.topn = m_settings.topn;
    def.io_view = m_settings.io_view;
    mgr.AddWindow(def);
    m_settings_dirty = true;
}

void CProcessTopPlugin::PinGroup(const RECT* anchor_for_show) {
    // pin 翻转（任务栏 item / 菜单 360 共用，M6 §4.3.3/§4.6）：
    // pinned → 取消 pin + 收回（回 hover 态）；未 pin → pin=true
    // （组不可见时按 anchor 弹出；已可见就地固定并卸锚跟随）
    auto& mgr = WindowManager::Instance();
    if (m_group_pinned) {
        m_group_pinned = false;
        mgr.HideGroup();
    } else {
        m_group_pinned = true;
        if (!mgr.GroupVisible() && anchor_for_show) {
            mgr.ShowGroupAt(*anchor_for_show);   // pin 态不挂锚跟随
        } else {
            mgr.DetachAnchorTracker();   // 已可见：就地固定
        }
    }
}

void CProcessTopPlugin::HoverTick() {
    // 100ms WM_TIMER（id=2）挂组长窗；组隐藏时窗口 SW_HIDE 不停 timer。
    // 常驻模式（popup_mode==1）无 hover 逻辑——常驻组常显，item 点击的
    // 对称取消语义见 OnMouseEvent
    if (m_settings.popup_mode == 1) return;
    auto& mgr = WindowManager::Instance();
    if (mgr.Count() == 0) return;   // 无窗可弹（hover 无处显示直到 item 重建）

    // 1. TM 右键菜单打开：立即隐藏（无宽限，菜单绝不与弹窗同屏，pin 态
    // 也让位）+ 清 hover 状态 + Pop 原生 tooltip（防其盖菜单）
    if (IsTMMenuMode()) {
        if (mgr.GroupVisible()) {
            m_group_pinned = false;
            mgr.HideGroup();
        }
        m_hover_target = nullptr;
        m_hover_start_tick = 0;
        m_hover_leave_tick = 0;
        PopTMTooltips();   // idempotent；covers the main window's 1s gap
        return;
    }

    ULONGLONG now = GetTickCount64();
    POINT pt;
    GetCursorPos(&pt);
    HWND hover_wnd = WindowFromPoint(pt);

    // 2. over_group：光标窗 == 任一 PTM 窗，或组内 TrackMouseEvent
    //    leave 信号（光标短暂离开窗口矩形时窗口仍是 hover 目标）
    bool over_group = false;
    for (int i = 0; i < mgr.Count() && !over_group; i++) {
        CTopListWindow* w = mgr.At(i);
        if (!w) continue;
        if (hover_wnd && hover_wnd == w->GetHwnd()) over_group = true;
        else if (w->IsHovering()) over_group = true;
    }
    bool is_taskbar = false;
    HWND tm_wnd = DetectTMWindow(hover_wnd, is_taskbar);

    // 3. show / hide 状态机
    if (tm_wnd) {
        // 光标在 TM 窗上：清离开计时；换目标窗重置 hover 起点
        m_hover_leave_tick = 0;
        if (tm_wnd != m_hover_target) {
            m_hover_target = tm_wnd;
            m_hover_is_taskbar = is_taskbar;
            m_hover_start_tick = now;
        }
        if (!mgr.GroupVisible() && !m_group_pinned &&
            now - m_hover_start_tick >= 400) {
            RECT anchor;
            if (is_taskbar) {
                // 任务栏 → 光标 ±60px 锚带（用任务栏对话框 rect——parent
                // 主窗在桌面不在任务栏；PNM :1349-1358 钳位写法照抄）
                RECT wr;
                GetWindowRect(hover_wnd, &wr);
                int half = 60;
                anchor = { pt.x - half, wr.top, pt.x + half, wr.bottom };
                if (anchor.left < wr.left) { anchor.left = wr.left; anchor.right = wr.left + 2 * half; }
                if (anchor.right > wr.right) { anchor.right = wr.right; anchor.left = wr.right - 2 * half; }
            } else {
                GetWindowRect(tm_wnd, &anchor);   // 主悬浮窗 → 整窗 rect
            }
            mgr.ShowGroupAt(anchor, is_taskbar ? nullptr : tm_wnd);
        }
    } else if (!over_group) {
        // 光标在别处：组可见且未 pin → 300ms 宽限后隐藏（宽限期内回去
        // 取消）；组不可见 → 清 hover 目标
        m_hover_start_tick = 0;
        if (mgr.GroupVisible() && !m_group_pinned) {
            if (m_hover_leave_tick == 0) {
                m_hover_leave_tick = now;
            } else if (now - m_hover_leave_tick >= 300) {
                mgr.HideGroup();
                m_hover_target = nullptr;
            }
        } else if (!mgr.GroupVisible()) {
            m_hover_target = nullptr;
        }
    } else {
        // 光标在弹窗上：保持显示
        m_hover_leave_tick = 0;
    }

    // 4. 锚 reconcile：组可见且未 pin 且非任务栏锚 → 子类化锚窗跟随；
    //    否则卸载（pin/换目标在此每 tick 收敛；事件路径经回调同步回写
    //    m_popup_anchor，子类化被注入器剥掉时轮询兜底接管）
    {
        HWND follow_wnd = nullptr;
        if (mgr.GroupVisible() && !m_group_pinned && m_hover_target &&
            !m_hover_is_taskbar && IsWindow(m_hover_target))
            follow_wnd = m_hover_target;
        mgr.SetAnchorFollow(follow_wnd);
    }
    // 组可见期间数据刷新：WM_PTM_REFRESH 投递管线不变（轻量：锚没动
    // 不重排）
}

void CProcessTopPlugin::SaveSettingsIfDirty() {
    ULONGLONG now = GetTickCount64();
    if (now - m_last_save_tick < 30000 && !m_settings_dirty) return;
    m_last_save_tick = now;
    if (!m_settings_dirty) return;
    SaveWindowConfigsNow();
    m_settings_dirty = false;
}

void CProcessTopPlugin::SaveWindowConfigsNow() {
    // 窗口配置（含位置，拖动结束时已写进各自 cfg）全量收走落盘——
    // 缺陷①（M5/A1）的治本路径：不等 30s 节流
    m_settings.windows = WindowManager::Instance().CollectConfigs();
    PtSaveSettings(m_config_dir, m_settings);
}

// ---- tooltip (double-buffered publish, PNM discipline) ----

static const size_t kTipSize = 2048;

void CProcessTopPlugin::BuildTooltip() {
    MetricsSnapshot snap;
    {
        EnterCriticalSection(&m_data_lock);
        snap = m_snapshot;
        LeaveCriticalSection(&m_data_lock);
    }
    EnterCriticalSection(&m_data_lock);
    wchar_t* tip = TipWriteBuf();
    wchar_t line[256];
    wcscpy_s(tip, kTipSize, L"Process Top Monitor\n");
    if (snap.cpu_total_pct >= 0) {
        swprintf_s(line, 256, L"CPU: %.1f%%", snap.cpu_total_pct);
    } else {
        wcscpy_s(line, 256, L"CPU: --");
    }
    wcscat_s(tip, kTipSize, line);
    if (snap.mem_total) {
        swprintf_s(line, 256, L"  MEM: %.2f/%.2f GB",
                   snap.mem_used / 1073741824.0,
                   snap.mem_total / 1073741824.0);
        wcscat_s(tip, kTipSize, line);
    }
    wcscat_s(tip, kTipSize, L"\n");
    if (m_etw.Grade() == FileIoGrade::C) {
        wcscat_s(tip, kTipSize, TR(L"\u26a0 IO 口径：全句柄（含网络）——需要管理员权限获取精确磁盘口径\n"));
    }
    // CPU top3
    int count = 0;
    for (auto& r : snap.cpu.rows) {
        if (count >= 3) break;
        swprintf_s(line, 256, L"  %s %.1f%%\n", r.name.c_str(), r.value);
        wcscat_s(tip, kTipSize, line);
        count++;
    }
    PublishTooltip();
    LeaveCriticalSection(&m_data_lock);
}

const wchar_t* CProcessTopPlugin::GetTooltipInfo() {
    return m_tooltip[m_tooltip_cur.load(std::memory_order_acquire)];
}

// ---- tick pipeline (timer-queue thread) ----

void CALLBACK CProcessTopPlugin::RefreshTimerProc(void* param, BOOLEAN) {
    ((CProcessTopPlugin*)param)->RefreshTick();
}

void CProcessTopPlugin::RefreshTick() {
    bool expected = false;
    if (!m_refresh_busy.compare_exchange_strong(expected, true)) return;

    CheckLanguageChange();

    // 1) snapshot: CPU + memory + C-grade IO counters (1-5ms)
    CpuMemIoMetrics m;
    m_metrics.Tick(m);

    // 2) ETW exact file IO (A/B grades); C grade falls back to counters
    EtwFileCollector::IoDelta io_delta;
    bool have_etw = false;
    FileIoGrade grade = m_etw.Grade();
    if (m_etw.IsRunning() && (grade == FileIoGrade::A || grade == FileIoGrade::B)) {
        m_etw.TakeDelta(io_delta);
        have_etw = io_delta.valid;
    }

    // 2b) GPU 按需启停（绑定位集不含 GPU/显存 → PDH 全关，方案 §3.3）。
    // 关停时同步清空缓存结果，重开后的首拍不会拿旧数据冒充。
    // 位集语义（M6 §4.2.5）：「存在」（配置内）窗口并集——hover 隐藏 ≠
    // 关闭，窗随时弹回，采集保持预热
    int mask = WindowManager::Instance().BoundMask();
    bool gpu_bound = mask & (PtKindMask(MetricKind::Gpu) | PtKindMask(MetricKind::Vram));
    if (gpu_bound) {
        m_gpu.EnsureStarted();
    } else {
        m_gpu.EnsureStopped();
        m_gpu_last = GpuTickData{};
    }

    // 2c) ETW 按需启停（方案 §4.2/§8.3）：无可见磁盘 IO 窗口 → 停会话，
    // 内核侧零开销；首次出现 IO 窗口时启动（A/B 尝试，失败落 C 并如实
    // 标注）。只在绑定状态变化时动作，避免 C 档每拍重试。
    bool io_bound = (mask & PtKindMask(MetricKind::DiskIo)) != 0;
    if (io_bound != m_io_bound_prev) {
        if (io_bound) m_etw.Start();
        else m_etw.Stop();
        m_io_bound_prev = io_bound;
    }

    ULONGLONG now = GetTickCount64();
    static ULONGLONG prev_wall = 0;   // tick-thread only (CAS-guarded)
    double dt = prev_wall ? (double)(now - prev_wall) / 1000.0 : 0.0;
    prev_wall = now;
    if (dt < 0.05) dt = 0.05;
    if (dt > 10.0) dt = 10.0;

    // 3) aggregate: merge + sort + TopN
    AggregateSettings agg;
    // 多窗模式：快照视图按最大行数（15）供数，各窗在自己的
    // UpdateFromSnapshot 里按 cfg.topn 截断——全局 topn 只是新窗默认值
    agg.topn = 15;
    agg.vram_with_shared = (m_settings.vram_basis == 1);
    // GPU 采集单独降频（每 2 拍 ≈1s，方案 §2.4）。非采集拍沿用上一次结果
    // （GPU 数据本就是 1s 档口径），否则快照 valid 位逐拍闪烁。
    GpuTickData gpu_data;
    bool gpu_tick = (++m_gpu_tick_div % 2 == 0);
    if (m_gpu.IsRunning()) {
        if (gpu_tick) {
            // G1 (PDH) 不需要候选表；G2 (D3DKMT) 用快照 pid 做查询候选
            std::vector<DWORD> pids;
            pids.reserve(m.procs.size());
            for (auto& p : m.procs) pids.push_back(p.pid);
            m_gpu.Tick(m_gpu_last, pids);
        }
        gpu_data = m_gpu_last;
    }
    MetricsSnapshot snap;
    m_aggregator.Build(m, have_etw ? &io_delta : nullptr,
                       m_gpu.IsRunning() ? &gpu_data : nullptr,
                       dt, agg, m_metrics, snap);
    if (m_etw.IsRunning()) snap.io_grade = m_etw.Grade();   // true grade (A/B/C)
    snap.gpu_grade = m_gpu.Grade();                          // true grade even when off
    snap.tm_gpu_usage = m_tm_gpu_usage.load();
    // 宿主传感器（M3-3）：进快照供窗口摘要行取用（0 = 未采集）
    snap.tm_cpu_temp = m_tm_cpu_temp.load();
    snap.tm_gpu_temp = m_tm_gpu_temp.load();
    // 频率优先用自测值（MHz，能出 4.9GHz 一位小数）；宿主 cpu_freq 是
    // 整数 GHz（float 截断），只在自测不可用时兜底 ×1000
    snap.tm_cpu_freq = (m.cpu_freq_mhz > 0) ? m.cpu_freq_mhz
                                            : m_tm_cpu_freq.load() * 1000;

    // 4) publish + taskbar item text (lock)
    {
        EnterCriticalSection(&m_data_lock);
        m_snapshot = snap;
        m_snapshot_valid = true;
        if (!snap.cpu.rows.empty()) {
            auto& top1 = snap.cpu.rows[0];
            std::wstring nm = top1.name;
            if (nm.size() > 4) {
                std::wstring ext = nm.substr(nm.size() - 4);
                if (ext == L".exe" || ext == L".EXE") nm = nm.substr(0, nm.size() - 4);
            }
            wcsncpy_s(CTopItem::s_top_name, 48, nm.c_str(), _TRUNCATE);
            swprintf_s(CTopItem::s_top_value, 32, L"%.1f%%", top1.value);
        } else {
            wcsncpy_s(CTopItem::s_top_name, 48, L"-", _TRUNCATE);
            wcsncpy_s(CTopItem::s_top_value, 32, L"--", _TRUNCATE);
        }
        LeaveCriticalSection(&m_data_lock);
    }

    BuildTooltip();

    // 5) window refresh (posted: window pulls the snapshot on ITS OWN thread)
    WindowManager::Instance().ReclaimDead();   // × 钮销毁的窗口对象回收
    WindowManager::Instance().PostToAll(WM_PTM_REFRESH, 0, 0);

    // 6) throttled settings persistence + debug-log sync。
    // 墙钟 30s 节流（原 tick 计数 %60 在负载下 tick 走得慢，首槽实测
    // ~62s——cfg 变更落盘窗口不可控；墙钟才是「30s 节流」的诚实实现）
    if (now - m_last_save_tick >= 30000) {
        SaveSettingsIfDirty();
        EtwFileCollector::SetDebugLogs(m_settings.debug_logs);
    }
    // 全部窗口销毁（windows 清空）→ 定时器停（M6 §4.2.5：hover 隐藏不
    // 停采集，只有窗销毁才停）：CPU 归零，任务栏 item 退化为 TM tick 惰
    // 性读缓存；item 重建窗时由 AddWindow 重新拉起定时器
    if (WindowManager::Instance().Count() == 0) StopRefreshTimer(false);   // 回调内自停：不能等待自己
    m_refresh_busy.store(false);
}

void CProcessTopPlugin::StartRefreshTimer() {
    StopRefreshTimer();
    int ms = m_settings.refresh_ms;
    if (ms < 250) ms = 250;
    if (ms > 2000) ms = 2000;
    HANDLE h = nullptr;
    if (CreateTimerQueueTimer(&h, nullptr, RefreshTimerProc, this, ms, ms,
                              WT_EXECUTELONGFUNCTION)) {
        m_refresh_timer = h;
        m_refresh_timer_ok = true;
    }
}

void CProcessTopPlugin::StopRefreshTimer(bool wait) {
    if (m_refresh_timer) {
        // wait=true：DeleteTimerQueueTimer 传 INVALID_HANDLE_VALUE 会等待
        // 正在执行的回调结束——从回调内部（RefreshTick 的「全部隐藏」路径）
        // 调用会等自己，死锁。回调内必须传 nullptr（异步删除）。
        DeleteTimerQueueTimer(nullptr, m_refresh_timer,
                              wait ? INVALID_HANDLE_VALUE : nullptr);
        m_refresh_timer = nullptr;
    }
    m_refresh_timer_ok = false;
}

void CProcessTopPlugin::CheckLanguageChange() {
    const wchar_t* bcp = (m_app ? m_app->GetStringRes(L"BCP_47", L"general") : nullptr);
    std::wstring tm = bcp && bcp[0] ? bcp : L"zh-CN";
    if (I18n::CheckAndReload(tm)) {
        WindowManager::Instance().PostToAll(WM_PTM_LANG_CHANGED, 0, 0);
    }
}

void CProcessTopPlugin::ApplyLanguageNow() {
    // M4 选项对话框「确定」路径：手动语言切换即时生效
    I18n::SetLang(m_settings.lang);
    I18n::Reload();
    WindowManager::Instance().PostToAll(WM_PTM_LANG_CHANGED, 0, 0);
}

void CProcessTopPlugin::RestartRefreshTimer() {
    StartRefreshTimer();
}

void CProcessTopPlugin::ApplyWindowMode() {
    // M6 §4.5：三项生效。多窗开关取消 = 收敛到单窗（保留第一窗，其余
    // 销毁——对话框 OK 的 UI 线程不在被销毁窗的消息栈里，同步 erase 安全）；
    // 默认指标确定 = 重置当前单窗指标（§3.2 持久化语义）；出现方式：
    // 常驻 → 立即显示（记忆位置或组布局，不挂锚），hover → 收起回 hover 态
    auto& mgr = WindowManager::Instance();
    if (!m_settings.multi_window) {
        while (mgr.Count() > 1) mgr.DestroyAt(mgr.Count() - 1);
    }
    if (!m_settings.multi_window && mgr.Count() == 1) {
        if (auto* w = mgr.At(0))
            w->SetKind((MetricKind)m_settings.default_metric);
    }
    if (m_settings.popup_mode == 1) {
        if (!mgr.GroupVisible()) {
            mgr.ShowGroupAt(GetStartupAnchor());
            mgr.DetachAnchorTracker();   // 常驻不跟锚
        }
    } else {
        m_group_pinned = false;
        mgr.HideGroup();
    }
    m_settings_dirty = true;
}

// ---- UI-thread entry points ----

void CProcessTopPlugin::TopListRefreshFromSnapshot() {
    // 锁内拷一次快照 → 逐窗灌数据（每窗 kind/io_view/topn 来自各自 cfg，
    // 不再回写全局 settings——cfg 即配置本体，方案 §3.4）
    MetricsSnapshot snap;
    bool valid;
    {
        EnterCriticalSection(&m_data_lock);
        snap = m_snapshot;
        valid = m_snapshot_valid;
        LeaveCriticalSection(&m_data_lock);
    }
    WindowManager::Instance().RefreshAll(snap, valid, m_settings.topn);
}

ITMPlugin::OptionReturn CProcessTopPlugin::ShowOptionsDialog(void* hParent) {
    return PtShowOptionsDialog((HWND)hParent);
}

// ---- M4 选项对话框接缝 ----

void CProcessTopPlugin::CopySnapshot(MetricsSnapshot& out, bool& valid) {
    EnterCriticalSection(&m_data_lock);
    out = m_snapshot;
    valid = m_snapshot_valid;
    LeaveCriticalSection(&m_data_lock);
}

std::vector<std::wstring> CProcessTopPlugin::EnumerateGpuEnginesForDialog() {
    if (m_gpu.IsRunning()) {
        MetricsSnapshot snap;
        bool valid;
        CopySnapshot(snap, valid);
        if (!snap.gpu_engtypes.empty()) return snap.gpu_engtypes;
    }
    return m_gpu.EnumerateEngines();
}

void CProcessTopPlugin::GetIoStatusForDialog(wchar_t* out, size_t cap) {
    if (!out || cap == 0) return;
    FileIoGrade g = m_etw.Grade();
    switch (g) {
    case FileIoGrade::A:
        wcsncpy_s(out, cap, TR(L"A：精确口径（文件读写，ETW 会话 PTMFileIoSession）"), _TRUNCATE);
        break;
    case FileIoGrade::B:
        wcsncpy_s(out, cap, TR(L"B：精确口径（Win7 NT Kernel Logger）"), _TRUNCATE);
        break;
    case FileIoGrade::C: {
        wcsncpy_s(out, cap, TR(L"C：全句柄口径（含网络）"), _TRUNCATE);
        // 原因追加（GetStatusText 的 "C: reason" 段）
        wchar_t base[160];
        m_etw.GetStatusText(base, 160);
        const wchar_t* p = wcschr(base, L':');
        if (p && p[1] == L' ' && p[2] && wcslen(out) + 4 + wcslen(p + 2) < cap) {
            wcscat_s(out, cap, TR(L"—— "));
            wcscat_s(out, cap, p + 2);
        }
        break;
    }
    default:
        wcsncpy_s(out, cap, L"off", _TRUNCATE);
        break;
    }
    uint64_t lost = 0;
    {
        EnterCriticalSection(&m_data_lock);
        lost = m_snapshot.io_events_lost;
        LeaveCriticalSection(&m_data_lock);
    }
    if (lost > 0) {
        wchar_t lbuf[48];
        swprintf_s(lbuf, 48, TR(L"｜丢事件：%llu"), (unsigned long long)lost);
        if (wcslen(out) + wcslen(lbuf) < cap) wcscat_s(out, cap, lbuf);
    }
}

void CProcessTopPlugin::DataRequired() {
    if (!m_inited) EnsureInitialized();
    if (m_refresh_timer_ok) return;   // fast path: the refresh timer owns collection
    // timer could not be created: degrade to TM's 1s tick
    RefreshTick();
}

// ============================================================
// DLL entry + export
// ============================================================

extern "C" BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        s_dll_hinst = (HINSTANCE)hModule;
    } else if (reason == DLL_PROCESS_DETACH) {
        // Mark shutdown BEFORE anything else: window teardown must not
        // DestroyWindow through synchronous callbacks (issue #7 discipline)
        g_shutting_down = true;
        CProcessTopPlugin::Instance().ShutdownForDllUnload();
    }
    return TRUE;
}

extern "C" {
    __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
        return &CProcessTopPlugin::Instance();
    }
}
