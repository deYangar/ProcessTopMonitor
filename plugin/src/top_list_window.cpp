// top_list_window.cpp — ranking popup: ULW pipeline (PNM-inherited) + N rows
#include "top_list_window.h"
#include "utils.h"
#include "i18n.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <imm.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "msimg32.lib")

HINSTANCE s_dll_hinst = nullptr;                 // captured in DllMain (plugin_main.cpp)
volatile bool g_shutting_down = false;

void PtSafeDestroyWindow(HWND hwnd) {
    if (!hwnd) return;
    // Detach IME first (third-party IME hooks must not run in the destroy
    // callback), SEH as the last resort — issue #7 discipline, PNM-inherited.
    ImmAssociateContext(hwnd, nullptr);
    __try {
        DestroyWindow(hwnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ============================================================
// multi-instance dispatch: HWND -> this
// ============================================================
namespace {
struct WndReg {
    HWND hwnd;
    CTopListWindow* self;
};
std::vector<WndReg>& RegTable() {
    static std::vector<WndReg> t;    // UI-thread only, no lock
    return t;
}
} // namespace

CTopListWindow* CTopListWindow::FromHwnd(HWND hwnd) {
    for (auto& r : RegTable())
        if (r.hwnd == hwnd) return r.self;
    return nullptr;
}

LRESULT CALLBACK CTopListWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (auto* self = FromHwnd(hwnd))
        return self->HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// SWCA (undocumented but stable since Win8) — PNM-inherited verbatim
// ============================================================
namespace {

enum PTM_ACCENT_STATE {
    PTM_ACCENT_ENABLE_BLURBEHIND = 3,
    PTM_ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
};
struct PTM_ACCENT_POLICY {
    int nAccentState;
    int nFlags;
    UINT nColor;       // ABGR
    int nAnimationId;
};
struct PTM_WINDOWCOMPOSITIONATTRIBDATA {
    int nAttribute;    // 19 = WCA_ACCENT_POLICY
    void* pData;
    ULONG dataSize;
};
typedef BOOL(WINAPI* PFN_SetWindowCompositionAttribute)(HWND, PTM_WINDOWCOMPOSITIONATTRIBDATA*);

PFN_SetWindowCompositionAttribute PtGetSWCA() {
    static PFN_SetWindowCompositionAttribute pfn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32)
            pfn = (PFN_SetWindowCompositionAttribute)GetProcAddress(
                user32, "SetWindowCompositionAttribute");
    }
    return pfn;
}

} // namespace

// ============================================================
// construction / destruction
// ============================================================

CTopListWindow::CTopListWindow() {
    m_dark_mode = true;
    m_text_dark = true;
}

CTopListWindow::~CTopListWindow() {
    if (m_hwnd) {
        if (g_shutting_down || !IsWindow(m_hwnd)) {
            m_hwnd = nullptr;   // DLL unload or already-destroyed HWND (issue #7)
        } else {
            PtSafeDestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }
    DestroyBlurLayer();
    HDC dcs[4] = { m_cov_a_dc, m_cov_c_dc, m_ulw_dc, m_snap_dc };
    HBITMAP bmps[4] = { m_cov_a_bmp, m_cov_c_bmp, m_ulw_bmp, m_snap_bmp };
    for (int i = 0; i < 4; i++) {
        if (dcs[i]) DeleteDC(dcs[i]);
        if (bmps[i]) DeleteObject(bmps[i]);
    }
    m_cov_a_dc = m_cov_c_dc = m_ulw_dc = m_snap_dc = nullptr;
    m_cov_a_bmp = m_cov_c_bmp = m_ulw_bmp = m_snap_bmp = nullptr;
    m_cov_a_bits = m_cov_c_bits = m_ulw_bits = m_snap_bits = nullptr;
    for (auto& [k, icon] : m_icon_cache)
        if (icon) DestroyIcon(icon);
    if (m_font_normal) DeleteObject(m_font_normal);
    if (m_font_small) DeleteObject(m_font_small);
    if (m_pen_separator) DeleteObject(m_pen_separator);
    // 不 UnregisterClass：窗口类是 DLL 级共享的（多实例同类名），第一个
    // 实例销毁就注销会让其余实例的消息分发崩——类随进程退出自然清理
    // deregister from the dispatch table
    auto& t = RegTable();
    for (size_t i = 0; i < t.size(); i++)
        if (t[i].self == this) { t.erase(t.begin() + i); break; }
}

bool CTopListWindow::EnsureClassRegistered(HINSTANCE hInst) {
    wchar_t cls[64];
    swprintf_s(cls, 64, L"PTMTopList_%p", hInst);
    WNDCLASSEXW wcExist = {};
    wcExist.cbSize = sizeof(wcExist);
    if (GetClassInfoExW(hInst, cls, &wcExist)) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW;     // subclassed per-window at Create
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = cls;
    return RegisterClassExW(&wc) != 0;
}

bool CTopListWindow::Create(const POINT* suggested) {
    m_hinst = s_dll_hinst ? s_dll_hinst : (HINSTANCE)GetModuleHandleW(nullptr);
    if (!EnsureClassRegistered(m_hinst)) return false;

    wchar_t cls[64];
    swprintf_s(cls, 64, L"PTMTopList_%p", m_hinst);
    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_APPWINDOW,
        cls, L"", WS_POPUP,
        0, 0, BASE_WIDTH, 400,
        nullptr, nullptr, m_hinst, nullptr);
    if (!m_hwnd) return false;

    RegTable().push_back({ m_hwnd, this });
    SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, (LONG_PTR)StaticWndProc);

    // DWM: no system rounding / NC rendering — ULW alpha owns the shape
    // (PNM: DWMWCP_DEFAULT policy rounding would draw a dark halo)
    int corner_pref = 1;   // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(m_hwnd, 33, &corner_pref, sizeof(corner_pref));
    int nc_policy = 2;     // DWMNCRP_DISABLED
    DwmSetWindowAttribute(m_hwnd, 2, &nc_policy, sizeof(nc_policy));

    // 顶部惯性：右上角级联（无持久化位置时），不压任务栏
    POINT pos;
    if (suggested && (suggested->x != INT_MIN)) {
        pos = *suggested;
    } else {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        pos.x = wa.right - BASE_WIDTH - 40;
        pos.y = wa.top + 60;
    }
    SetWindowPos(m_hwnd, nullptr, pos.x, pos.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    m_dark_mode = IsDarkMode();
    m_text_dark = m_dark_mode;
    UpdateDpiScale();
    RecreateScaledResources();

    // icon pump timer: fills uncached icons frame-split (draw is cache-only)
    SetTimer(m_hwnd, 1, 200, nullptr);
    return true;
}

void CTopListWindow::Destroy() {
    if (m_hwnd && !g_shutting_down) {
        PtSafeDestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_visible = false;
}

void CTopListWindow::GetWindowPos(POINT& pt) const {
    pt = { INT_MIN, INT_MIN };
    if (!m_hwnd) return;
    RECT rc;
    if (GetWindowRect(m_hwnd, &rc)) {
        pt.x = rc.left;
        pt.y = rc.top;
    }
}

// ============================================================
// dark mode / colors
// ============================================================

bool CTopListWindow::IsDarkMode() {
    if (m_color_mode == 1) return true;
    if (m_color_mode == 2) return false;
    ULONGLONG now = GetTickCount64();
    if (now - m_dark_mode_tick < 5000) return m_dark_mode_cached;
    m_dark_mode_tick = now;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1, size = sizeof(DWORD);
        RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, (BYTE*)&val, &size);
        RegCloseKey(hKey);
        m_dark_mode_cached = (val == 0);
    }
    return m_dark_mode_cached;
}

COLORREF CTopListWindow::GetBgColor() {
    return UseTextTheme() ? RGB(32, 32, 32) : RGB(249, 249, 249);
}

COLORREF CTopListWindow::GetTextColor() {
    return UseTextTheme() ? RGB(240, 240, 240) : RGB(30, 30, 30);
}

// 指标 accent 色（排行数值/标题），深浅两变体跟文字主题。
// CPU 青蓝 / 内存 紫 / IO 橙（与 PNM 橙绿体系区分，一眼辨指标）
COLORREF CTopListWindow::GetAccentColor() {
    bool light = UseTextTheme();
    switch (m_kind) {
    case MetricKind::Mem:
        return light ? RGB(200, 150, 255) : RGB(160, 100, 220);
    case MetricKind::DiskIo:
        return light ? RGB(230, 150, 40) : RGB(230, 126, 34);
    default:
        return light ? RGB(90, 190, 255) : RGB(60, 160, 230);
    }
}

const wchar_t* CTopListWindow::MetricTitle() const {
    switch (m_kind) {
    case MetricKind::Mem: return TR(L"内存");
    case MetricKind::DiskIo: return TR(L"磁盘 IO");
    default: return TR(L"CPU");
    }
}

// ============================================================
// DPI
// ============================================================

void CTopListWindow::UpdateDpiScale() {
    RECT rc;
    float s = 1.0f;
    if (m_hwnd && GetWindowRect(m_hwnd, &rc))
        s = PTM_GetDpiScaleForRect(rc);
    else if (m_hwnd)
        s = PTM_GetDpiScaleForMonitor(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST));
    if (s < 0.5f) s = 1.0f;
    if (m_dpi_inited && fabsf(s - m_dpi_scale) < 0.01f) return;
    m_dpi_scale = s;
    m_dpi_inited = true;
    PADDING = (int)(BASE_PADDING * s);
    ROW_HEIGHT = (int)(BASE_ROW_HEIGHT * s);
    ICON_SIZE = (int)(BASE_ICON_SIZE * s);
    CORNER_RADIUS = std::max(1, (int)(BASE_CORNER_RADIUS * s));
    TITLE_H = (int)(BASE_TITLE_H * s);
    SUMMARY_H = (int)(BASE_SUMMARY_H * s);
    STATUS_H = (int)(BASE_STATUS_H * s);
    VALUE_AREA_W = (int)(BASE_VALUE_AREA_W * s);
    IDX_W = (int)(BASE_IDX_W * s);
    RecreateScaledResources();
}

void CTopListWindow::RecreateScaledResources() {
    if (m_font_normal) DeleteObject(m_font_normal);
    if (m_font_small) DeleteObject(m_font_small);
    if (m_pen_separator) DeleteObject(m_pen_separator);
    m_font_normal = CreateFontW((int)(-14 * m_dpi_scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    m_font_small = CreateFontW((int)(-13 * m_dpi_scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    m_pen_separator = CreatePen(PS_SOLID, 1, UseTextTheme() ? RGB(45, 45, 45) : RGB(235, 235, 235));
    m_pen_dark = UseTextTheme();
}

// ============================================================
// message handling
// ============================================================

LRESULT CTopListWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_TIMER:
        if (wp == 1) {
            ResolvePendingPaths();
            if (PumpIconExtraction(2))
                InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_PTM_REFRESH:
        return 0;   // data arrives via UpdateFromSnapshot; reserved

    case WM_DPICHANGED: {
        RECT* prc = (RECT*)lp;
        if (prc && !m_dpi_inited) UpdateDpiScale();
        UpdateDpiScale();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!m_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hwnd, 0 };
            TrackMouseEvent(&tme);
            m_tracking = true;
        }
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        bool hc = PtInRect(&m_rc_close, pt) != FALSE;
        if (hc != m_hover_close) {
            m_hover_close = hc;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        m_tracking = false;
        if (m_hover_close) {
            m_hover_close = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(m_hwnd, &pt);
            SetCursor(LoadCursor(nullptr, PtInRect(&m_rc_close, pt) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        return DefWindowProcW(m_hwnd, msg, wp, lp);

    case WM_NCHITTEST: {
        // 标题栏 = 系统托管拖动（ULW 下同样工作）；关闭按钮与内容区 = 客户区
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(m_hwnd, &pt);
        if (PtInRect(&m_rc_close, pt)) return HTCLIENT;
        if (pt.y < TITLE_H) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (PtInRect(&m_rc_close, pt)) {
            // 关闭语义：销毁本窗（plugin_main 持有实例指针负责 delete）
            PostMessage(m_hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    case WM_NCRBUTTONUP:
        if (wp == HTCAPTION) {
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            BuildContextMenu(pt);
            return 0;
        }
        return DefWindowProcW(m_hwnd, msg, wp, lp);

    case WM_CONTEXTMENU: {
        // 右键客户区（行区等）同样弹菜单
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (lp == (LPARAM)-1) GetCursorPos(&pt);
        BuildContextMenu(pt);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        // 滚轮循环切换指标（方案 §6.2）：零额外采集，只换快照子集
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        static const MetricKind order[] = {
            MetricKind::Cpu, MetricKind::Mem, MetricKind::DiskIo
        };
        int idx = 0;
        for (int i = 0; i < 3; i++) if (order[i] == m_kind) idx = i;
        idx = (idx + (delta > 0 ? 1 : 2)) % 3;
        SetKind(order[idx]);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= 100 && id <= 199) {
            // 指标切换菜单
            static const MetricKind kinds[] = {
                MetricKind::Cpu, MetricKind::Mem, MetricKind::DiskIo
            };
            int sel = id - 100;
            if (sel >= 0 && sel < 3) SetKind(kinds[sel]);
        } else if (id >= 200 && id <= 202) {
            m_io_view = id - 200;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            // io_view 落盘由 plugin_main 在销毁时统一读走
        } else if (id == 300) {
            PostMessage(m_hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    case WM_CLOSE:
        Destroy();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    default:
        return DefWindowProcW(m_hwnd, msg, wp, lp);
    }
}

void CTopListWindow::SetKind(MetricKind k) {
    if (k == m_kind) return;
    m_kind = k;
    RecalcAndRepaint(true);
}

// ============================================================
// context menu (指标 / IO 视图 / 关闭)
// ============================================================

void CTopListWindow::BuildContextMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    // 指标组（M1：CPU/内存/磁盘 IO）
    struct { UINT id; MetricKind kind; const wchar_t* label; } kinds[] = {
        { 100, MetricKind::Cpu, L"CPU" },
        { 101, MetricKind::Mem, L"内存" },
        { 102, MetricKind::DiskIo, L"磁盘 IO" },
    };
    for (auto& k : kinds) {
        UINT flags = MF_STRING;
        if (k.kind == m_kind) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, k.id, TR(k.label));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (m_kind == MetricKind::DiskIo) {
        // IO 视图子菜单：合计/读/写（方案 §1.2 子视图）
        HMENU sub = CreatePopupMenu();
        const wchar_t* views[] = { L"读+写", L"仅读", L"仅写" };
        for (int i = 0; i < 3; i++) {
            UINT flags = MF_STRING;
            if (m_io_view == i) flags |= MF_CHECKED;
            AppendMenuW(sub, flags, 200 + i, TR(views[i]));
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, TR(L"IO 视图"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, 300, TR(L"关闭"));
    SetForegroundWindow(m_hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                             pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd) {
        // WM_COMMAND 语义复用（TrackPopupMenu TPM_RETURNCMD 不投递）
        HandleMessage(WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
    }
    // 菜单留下的 hover 状态复位（区域可能已变）
    m_hover_close = false;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ============================================================
// backdrop (PNM-inherited)
// ============================================================

void CTopListWindow::ApplyBackdrop(int effect, int opacity) {
    if (!m_hwnd) return;
    if (opacity < 10) opacity = 10;
    if (opacity > 100) opacity = 100;
    if (effect < 0 || effect > 3) effect = 0;
    // SWCA acrylic/blur renders only from Win10 1703: degrade to transparent
    if ((effect == 0 || effect == 2) && !PTM_IsAccentBlurSupported())
        effect = 1;

    bool want_layered = (effect != 3);
    int target = want_layered ? 1 : 0;
    bool dark = IsDarkMode();
    if (target == m_backdrop_mode &&
        (target != 1 || (effect == m_backdrop_effect &&
                         dark == m_acrylic_dark &&
                         opacity == m_backdrop_opacity)))
        return;   // idempotent

    if (m_backdrop_mode == 1 && target != 1) {
        m_acrylic_active = false;
        LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED)
            SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    }

    if (target == 1) {
        SetWindowRgn(m_hwnd, nullptr, FALSE);
        LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        m_noise_acrylic = (effect == 0);
        if (effect == 0 || effect == 2) EnsureBlurLayer();
        else DestroyBlurLayer();
        m_acrylic_active = true;
        m_acrylic_dark = dark;
    } else {
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        HRGN rgn = CreateRoundRectRgn(0, 0, rc.right, rc.bottom,
                                      CORNER_RADIUS * 2, CORNER_RADIUS * 2);
        SetWindowRgn(m_hwnd, rgn, FALSE);
        DestroyBlurLayer();
    }

    m_backdrop_mode = target;
    m_backdrop_effect = effect;
    m_backdrop_opacity = opacity;
    m_last_snap_rect = {};
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CTopListWindow::EnsureBlurLayer() {
    if (m_blur_hwnd) return;
    wchar_t className[64];
    swprintf_s(className, 64, L"PTMBlurTL_%p", m_hinst);
    WNDCLASSEXW wcExist = {};
    wcExist.cbSize = sizeof(wcExist);
    if (!GetClassInfoExW(m_hinst, className, &wcExist)) {
        WNDCLASSEXW wcB = {};
        wcB.cbSize = sizeof(wcB);
        wcB.lpfnWndProc = DefWindowProcW;
        wcB.hInstance = m_hinst;
        wcB.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wcB.lpszClassName = className;
        RegisterClassExW(&wcB);
    }
    m_blur_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,   // click-through
        className, L"", WS_POPUP,
        0, 0, 300, 400, nullptr, nullptr, m_hinst, nullptr);
    if (!m_blur_hwnd) return;

    int corner_pref = 2;   // DWMWCP_ROUND — blur corner must match the ULW shape
    DwmSetWindowAttribute(m_blur_hwnd, 33, &corner_pref, sizeof(corner_pref));
    MARGINS glass = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_blur_hwnd, &glass);

    // No DWMSBT: on plain-black GDI windows it renders tint color only
    // (measured on 26300, PNM). SWCA ACRYLICBLURBEHIND + near-invisible tint
    // is the real blur-behind channel.
    PTM_ACCENT_POLICY policy = {};
    policy.nAccentState = PTM_ACCENT_ENABLE_ACRYLICBLURBEHIND;
    policy.nColor = 0x01000000;   // alpha=1: system blur only, no tint
    PTM_WINDOWCOMPOSITIONATTRIBDATA data = {};
    data.nAttribute = 19;
    data.pData = &policy;
    data.dataSize = sizeof(policy);
    if (PtGetSWCA()) PtGetSWCA()(m_blur_hwnd, &data);
}

void CTopListWindow::DestroyBlurLayer() {
    if (!m_blur_hwnd) return;
    if (!g_shutting_down) PtSafeDestroyWindow(m_blur_hwnd);
    m_blur_hwnd = nullptr;
}

// ============================================================
// ULW surfaces — PNM-inherited verbatim (coverage double-pass compose)
// ============================================================

bool CTopListWindow::EnsureLayeredSurface(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (m_ulw_dc && m_ulw_bmp && m_cov_a_dc && m_cov_c_dc && m_snap_dc &&
        w == m_ulw_w_cache && h == m_ulw_h_cache)
        return true;

    struct { HDC* dc; HBITMAP* bmp; void** bits; int bw, bh; } surfs[4] = {
        { &m_cov_a_dc, &m_cov_a_bmp, &m_cov_a_bits, w, h },
        { &m_cov_c_dc, &m_cov_c_bmp, &m_cov_c_bits, w, h },
        { &m_ulw_dc, &m_ulw_bmp, &m_ulw_bits, w, h },
        { &m_snap_dc, &m_snap_bmp, &m_snap_bits, w, h },
    };
    for (auto& s : surfs) {
        if (*s.dc) { DeleteDC(*s.dc); *s.dc = nullptr; }
        if (*s.bmp) { DeleteObject(*s.bmp); *s.bmp = nullptr; }
        *s.bits = nullptr;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = s.bw;
        bi.bmiHeader.biHeight = -s.bh;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        *s.bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, s.bits, nullptr, 0);
        if (*s.bmp) {
            *s.dc = CreateCompatibleDC(nullptr);
            if (*s.dc) SelectObject(*s.dc, *s.bmp);
        }
        if (!*s.bmp || !*s.dc) {
            // all-or-nothing: partial surfaces with stale bookkeeping would
            // let a later same-size call skip rebuild and read out of bounds
            for (auto& t : surfs) {
                if (*t.dc) { DeleteDC(*t.dc); *t.dc = nullptr; }
                if (*t.bmp) { DeleteObject(*t.bmp); *t.bmp = nullptr; }
                *t.bits = nullptr;
            }
            m_ulw_w_cache = m_ulw_h_cache = 0;
            return false;
        }
    }
    m_ulw_w_cache = w;
    m_ulw_h_cache = h;
    return true;
}

void CTopListWindow::ComposeLayeredSurface(int w, int h) {
    if (!m_cov_a_bits || !m_cov_c_bits || !m_ulw_bits) return;
    unsigned char* pa = (unsigned char*)m_cov_a_bits;
    unsigned char* pc = (unsigned char*)m_cov_c_bits;
    unsigned char* pw = (unsigned char*)m_ulw_bits;

    COLORREF panel = GetBgColor();
    int pb = GetBValue(panel), pg = GetGValue(panel), pr = GetRValue(panel);
    int op = m_backdrop_opacity < 0 ? 100 : m_backdrop_opacity;
    int bg_a = op * 255 / 100;
    int r = std::max(2, CORNER_RADIUS);

    std::vector<int> frow((size_t)w * 3);          // content color [B,G,R]
    std::vector<int> crow((size_t)w);              // cov255
    std::vector<float> grow((size_t)w);            // corner geometry coverage
    std::vector<unsigned char> drow((size_t)w);    // dead-point mask bit0=B 1=G 2=R

    for (int y = 0; y < h; y++) {
        float cy = y + 0.5f;
        for (int x = 0; x < w; x++) {
            float cov = 1.0f;
            float cx = x + 0.5f, ccx = -1.0f, ccy = -1.0f;
            if (x < r && y < r) { ccx = (float)r; ccy = (float)r; }
            else if (x >= w - r && y < r) { ccx = (float)(w - r); ccy = (float)r; }
            else if (x < r && y >= h - r) { ccx = (float)r; ccy = (float)(h - r); }
            else if (x >= w - r && y >= h - r) { ccx = (float)(w - r); ccy = (float)(h - r); }
            if (ccx >= 0.0f) {
                float dx = cx - ccx, dy = cy - ccy;
                float dist = sqrtf(dx * dx + dy * dy);
                cov = (float)r - dist + 0.5f;
                if (cov < 0.0f) cov = 0.0f;
                if (cov > 1.0f) cov = 1.0f;
            }

            size_t i = ((size_t)y * w + x) * 4;
            int da = (pa[i] - pc[i]) + (pa[i + 1] - pc[i + 1]) + (pa[i + 2] - pc[i + 2]);
            int cov255 = 255 - (da + 1) / 3;
            grow[x] = cov;
            crow[x] = cov255;
            drow[x] = 0;
            if (cov255 >= 255) {
                frow[x * 3 + 0] = pc[i];
                frow[x * 3 + 1] = pc[i + 1];
                frow[x * 3 + 2] = pc[i + 2];
            } else if (cov255 > 0) {
                int ab = 255 - pa[i]     + pc[i];
                int ag = 255 - pa[i + 1] + pc[i + 1];
                int ar = 255 - pa[i + 2] + pc[i + 2];
                int fs = (pc[i] + pc[i + 1] + pc[i + 2]) * 255 / (3 * cov255);
                frow[x * 3 + 0] = ab > 1 ? pc[i]     * 255 / ab : fs;
                frow[x * 3 + 1] = ag > 1 ? pc[i + 1] * 255 / ag : fs;
                frow[x * 3 + 2] = ar > 1 ? pc[i + 2] * 255 / ar : fs;
                drow[x] = (unsigned char)((ab <= 1 ? 1 : 0) | (ag <= 1 ? 2 : 0)
                                        | (ar <= 1 ? 4 : 0));
            } else {
                frow[x * 3 + 0] = frow[x * 3 + 1] = frow[x * 3 + 2] = 0;
            }
        }
        // dead-point backfill: two sweeps chain 2px-wide dead bands closed
        for (int rep = 0; rep < 2; rep++) {
            for (int x = 0; x < w; x++) {
                if (!drow[x] || crow[x] <= 0) continue;
                for (int c = 0; c < 3; c++) {
                    int bit = 1 << c;
                    if (!(drow[x] & bit)) continue;
                    bool got = false;
                    int v = 0;
                    if (x > 0 && crow[x - 1] > 0 && !(drow[x - 1] & bit)) {
                        v = frow[(x - 1) * 3 + c]; got = true;
                    }
                    if (x + 1 < w && crow[x + 1] > 0 && !(drow[x + 1] & bit)) {
                        int rv = frow[(x + 1) * 3 + c];
                        v = got ? std::max(v, rv) : rv; got = true;
                    }
                    if (got) {
                        frow[x * 3 + c] = v;
                        drow[x] = (unsigned char)(drow[x] & ~bit);
                    }
                }
            }
        }
        for (int x = 0; x < w; x++) {
            size_t i = ((size_t)y * w + x) * 4;
            float cov = grow[x];
            int cov255 = crow[x];

            int nz = 0;
            if (m_noise_acrylic) {
                nz = ((x * 73856093) ^ (y * 19349663)) & 7;
                nz = nz - 3;
            }

            int a = cov255 + (bg_a * (255 - cov255) + 127) / 255;
            a = (int)(a * cov + 0.5f);
            if (a <= 0) {
                pw[i] = pw[i + 1] = pw[i + 2] = pw[i + 3] = 0;
                continue;
            }
            if (cov255 >= 255) {
                pw[i]     = (unsigned char)std::min(255, (int)(frow[x * 3 + 0] * cov + 0.5f));
                pw[i + 1] = (unsigned char)std::min(255, (int)(frow[x * 3 + 1] * cov + 0.5f));
                pw[i + 2] = (unsigned char)std::min(255, (int)(frow[x * 3 + 2] * cov + 0.5f));
            } else if (cov255 <= 0) {
                int bb = std::max(0, std::min(255, pb + nz)) * bg_a / 255;
                int bgg = std::max(0, std::min(255, pg + nz)) * bg_a / 255;
                int brr = std::max(0, std::min(255, pr + nz)) * bg_a / 255;
                pw[i]     = (unsigned char)std::min(255, (int)(bb * cov + 0.5f));
                pw[i + 1] = (unsigned char)std::min(255, (int)(bgg * cov + 0.5f));
                pw[i + 2] = (unsigned char)std::min(255, (int)(brr * cov + 0.5f));
            } else {
                int inv = 255 - cov255;
                int fr = frow[x * 3 + 2], fg = frow[x * 3 + 1], fb = frow[x * 3 + 0];
                int pnr = std::max(0, pr + nz), png = std::max(0, pg + nz), pnb = std::max(0, pb + nz);
                int er = fr * cov255 / 255 + pnr * bg_a / 255 * inv / 255;
                int eg = fg * cov255 / 255 + png * bg_a / 255 * inv / 255;
                int eb = fb * cov255 / 255 + pnb * bg_a / 255 * inv / 255;
                pw[i]     = (unsigned char)std::max(0, std::min(255, (int)(eb * cov + 0.5f)));
                pw[i + 1] = (unsigned char)std::max(0, std::min(255, (int)(eg * cov + 0.5f)));
                pw[i + 2] = (unsigned char)std::max(0, std::min(255, (int)(er * cov + 0.5f)));
            }
            pw[i + 3] = (unsigned char)a;
        }
    }

    SIZE size = { w, h };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(m_hwnd, screen, nullptr, &size, m_ulw_dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void CTopListWindow::ClearLayeredSurfaceForCapture() {
    if (!m_hwnd || !m_ulw_dc || !m_ulw_bits || m_ulw_w_cache <= 0 || m_ulw_h_cache <= 0)
        return;
    memset(m_ulw_bits, 0, (size_t)m_ulw_w_cache * m_ulw_h_cache * 4);
    SIZE size = { m_ulw_w_cache, m_ulw_h_cache };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(m_hwnd, screen, nullptr, &size, m_ulw_dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

bool CTopListWindow::CaptureSnapNoFlash(const RECT& popup_rect, int w, int h) {
    if (!m_hwnd || !m_snap_dc || !m_snap_bits) return false;
    if (!SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        m_affinity_ok = false;
        return false;
    }
    if (m_blur_hwnd && !SetWindowDisplayAffinity(m_blur_hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        SetWindowDisplayAffinity(m_hwnd, 0);
        m_affinity_ok = false;
        return false;
    }
    HDC scr = GetDC(nullptr);
    BitBlt(m_snap_dc, 0, 0, w, h, scr, popup_rect.left, popup_rect.top,
           SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, scr);
    if (m_blur_hwnd) SetWindowDisplayAffinity(m_blur_hwnd, 0);
    SetWindowDisplayAffinity(m_hwnd, 0);
    return true;
}

// 文字反色判定（PNM-inherited, 简化：无右列 accent 独立判定）
void CTopListWindow::RefreshBackdrop(const RECT& popup_rect) {
    bool prev_adaptive = m_text_adaptive;
    m_text_adaptive = false;
    m_text_dark = m_dark_mode;
    m_bg_lum = -1.0;
    if (!m_acrylic_active) return;

    int w = popup_rect.right - popup_rect.left;
    int h = popup_rect.bottom - popup_rect.top;
    if (w <= 0 || h <= 0) return;

    int op = m_backdrop_opacity < 0 ? 100 : m_backdrop_opacity;
    if (op >= 80) return;   // high opacity: panel color dominates, theme text is fine

    if (!EnsureLayeredSurface(w, h)) return;

    if (!CaptureSnapNoFlash(popup_rect, w, h)) {
        if (m_visible) ClearLayeredSurfaceForCapture();
        HDC scr = GetDC(nullptr);
        BitBlt(m_snap_dc, 0, 0, w, h, scr, popup_rect.left, popup_rect.top,
               SRCCOPY | CAPTUREBLT);
        ReleaseDC(nullptr, scr);
    }

    std::vector<unsigned short> lums;
    lums.reserve((size_t)((h / 4 + 1) * (w / 4 + 1)));
    unsigned char* p8 = (unsigned char*)m_snap_bits;
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            size_t i = ((size_t)y * w + x) * 4;
            lums.push_back((unsigned short)(
                (299 * p8[i + 2] + 587 * p8[i + 1] + 114 * p8[i]) / 1000));
        }
    if (lums.empty()) return;
    size_t mid = lums.size() / 2;
    std::nth_element(lums.begin(), lums.begin() + mid, lums.end());
    double screen_lum = (double)lums[mid];

    COLORREF panel = GetBgColor();
    double panel_lum = (299.0 * GetRValue(panel) + 587.0 * GetGValue(panel)
                        + 114.0 * GetBValue(panel)) / 1000.0;
    double bg_lum = (op / 100.0) * panel_lum + (1.0 - op / 100.0) * screen_lum;
    double text_lum = m_dark_mode ? 240.0 : 30.0;
    // 迟滞带（PNM）：70 翻转 / 85 翻回，吃掉阈值附近的抖动
    bool flip = (fabs(bg_lum - text_lum) < (prev_adaptive ? 85.0 : 70.0));
    if (flip) {
        m_text_dark = !m_dark_mode;
        m_text_adaptive = true;
    }
    m_bg_lum = bg_lum;
}

// ============================================================
// icons (PNM-inherited frame-split extraction)
// ============================================================

namespace {
bool PtIsNetworkPath(const std::wstring& path) {
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') {
        wchar_t root[4] = { path[0], L':', L'\\', 0 };
        return GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    return false;
}
std::wstring PtImageNameFromPath(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}
} // namespace

// exe 路径懒解析：快照行只带 pid+name，路径在 UI 线程按需查（活进程
// OpenProcess 快），miss 负缓存不重试
void CTopListWindow::ResolvePendingPaths() {
    for (auto& r : m_rows) {
        if (!r.pid || !r.exe_path.empty()) continue;   // resolved already
        auto it = m_path_cache.find(r.pid);
        if (it != m_path_cache.end()) {
            r.exe_path = it->second;
            continue;
        }
        // only known-image-name rows resolve; failed lookups go to the
        // negative cache so dead/swept pids don't retry every 200ms
        std::string misskey = PtToUtf8(r.name);
        if (m_path_missed.count(misskey)) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.pid);
        if (!h) {
            m_path_missed.insert(misskey);
            continue;
        }
        wchar_t buf[1024] = L"";
        DWORD n = 1024;
        if (QueryFullProcessImageNameW(h, 0, buf, &n) && n) {
            r.exe_path = buf;
            m_path_cache[r.pid] = r.exe_path;
        } else {
            m_path_missed.insert(misskey);
        }
        CloseHandle(h);
    }
}

HICON CTopListWindow::LookupIcon(const std::wstring& name, DWORD pid) {
    (void)name;
    if (!pid) return nullptr;
    auto it = m_icon_cache.find(pid);
    if (it != m_icon_cache.end()) return it->second;
    // dedupe pending queue
    for (DWORD q : m_icon_pending)
        if (q == pid) return nullptr;
    m_icon_pending.push_back(pid);
    return nullptr;
}

HICON CTopListWindow::ExtractIconFor(const std::wstring& name, DWORD pid) {
    (void)name;
    // resolve path via the row cache (filled by ResolvePendingPaths)
    std::wstring path;
    auto it = m_path_cache.find(pid);
    if (it != m_path_cache.end()) path = it->second;
    if (path.empty()) return nullptr;

    bool net = PtIsNetworkPath(path);
    int want = std::max(16, ICON_SIZE);
    HICON hIcon = nullptr;

    // 首选：Shell 图像工厂按目标尺寸渲染（PNM 实测：大图下缩保 alpha 抗锯齿）
    if (!net) {
        IShellItemImageFactory* factory = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr,
                                                  IID_PPV_ARGS(&factory)))) {
            SIZE sz = { want, want };
            HBITMAP hbm = nullptr;
            if (SUCCEEDED(factory->GetImage(sz, SIIGBF_ICONONLY, &hbm)) && hbm) {
                BITMAP bm = {};
                int iw = 0;
                if (GetObjectW(hbm, sizeof(bm), &bm)) iw = bm.bmWidth;
                if (iw > 0 && iw <= want * 2) {
                    HBITMAP mask = CreateBitmap(iw, iw, 1, 1, nullptr);
                    ICONINFO ii = { TRUE, 0, 0, mask, hbm };
                    hIcon = CreateIconIndirect(&ii);
                    DeleteObject(mask);
                }
                DeleteObject(hbm);
            }
            factory->Release();
        }
    }
    if (!hIcon) {
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES))
            hIcon = sfi.hIcon;
    }
    if (!hIcon && !net) {
        wchar_t path_buf[MAX_PATH] = {};
        wcsncpy_s(path_buf, MAX_PATH, path.c_str(), _TRUNCATE);
        WORD idx = 0;
        hIcon = ExtractAssociatedIconW(m_hinst, path_buf, &idx);
    }
    return hIcon;
}

bool CTopListWindow::PumpIconExtraction(int count) {
    bool produced = false;
    while (count-- > 0 && !m_icon_pending.empty()) {
        DWORD pid = m_icon_pending.front();
        m_icon_pending.erase(m_icon_pending.begin());
        m_icon_cache[pid] = ExtractIconFor(L"", pid);
        produced = true;
    }
    return produced;
}

// ============================================================
// data plumbing + dirty check
// ============================================================

void CTopListWindow::UpdateFromSnapshot(const MetricsSnapshot& snap, MetricKind kind,
                                        int topn, int io_view, bool first_tick) {
    m_kind = kind;
    m_topn = topn;
    m_io_view = io_view;
    m_cpu_total = snap.cpu_total_pct;
    m_mem_total = snap.mem_total;
    m_mem_used = snap.mem_used;
    m_io_read_total = snap.io_read_total_bps;
    m_io_write_total = snap.io_write_total_bps;
    m_io_grade = snap.io_grade;
    m_io_events_lost = snap.io_events_lost;

    const MetricView* view = nullptr;
    switch (m_kind) {
    case MetricKind::Mem: view = &snap.mem; break;
    case MetricKind::DiskIo: {
        view = (m_io_view == 1) ? &snap.io_read
             : (m_io_view == 2) ? &snap.io_write : &snap.io;
        break;
    }
    default: view = &snap.cpu; break;
    }
    m_rows = view->rows;
    m_rows_has_value = view->has_value;
    switch (m_kind) {
    case MetricKind::Mem: m_summary_value = (double)m_mem_used; break;
    case MetricKind::DiskIo:
        m_summary_value = (m_io_view == 1) ? m_io_read_total
                        : (m_io_view == 2) ? m_io_write_total
                        : m_io_read_total + m_io_write_total;
        break;
    default: m_summary_value = snap.cpu_total_pct; break;
    }

    // ---- dirty check: rebuild the (pid,count,value) signature and compare;
    // memory has no delta so its summary also participates (方案 §8.2: an
    // unchanged picture costs zero repaints) ----
    std::vector<std::pair<DWORD, long long>> sig;
    sig.reserve(m_rows.size() + 1);
    sig.push_back({ 0xDEAD, (long long)(m_summary_value * 10.0) });
    for (auto& r : m_rows)
        sig.push_back({ r.pid, (long long)(r.value * 10.0) + r.count * 1000003 });
    bool changed = first_tick || !m_rows_has_value || sig != m_dirty_sig;
    m_dirty_sig = std::move(sig);

    if (!m_visible) {
        RecalcAndRepaint(true);
        return;
    }
    if (changed) {
        RecalcAndRepaint(false);
        return;
    }
    // 静止期反色判定（双档节奏，PNM 同款）
    if (m_acrylic_active && m_affinity_ok) {
        RECT cur;
        GetWindowRect(m_hwnd, &cur);
        if (!EqualRect(&cur, &m_last_snap_rect)) {
            m_last_snap_rect = cur;
            m_last_refresh_tick = GetTickCount64();
            m_steady_probe_fast = true;
        } else if (GetTickCount64() - m_last_refresh_tick
                   >= (m_steady_probe_fast ? 300 : 1000)) {
            bool d0 = m_text_dark, a0 = m_text_adaptive;
            RefreshBackdrop(cur);
            m_last_refresh_tick = GetTickCount64();
            m_steady_probe_fast = false;
            if (m_text_dark != d0 || m_text_adaptive != a0)
                InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
}

void CTopListWindow::RecalcAndRepaint(bool reposition) {
    m_dark_mode = IsDarkMode();
    ApplyBackdrop(m_backdrop_effect, m_backdrop_opacity);   // theme re-check, idempotent
    if (UseTextTheme() != m_pen_dark) RecreateScaledResources();
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    UpdateDpiScale();
    if (reposition) Reposition();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    if (m_acrylic_active) UpdateWindow(m_hwnd);   // compose synchronously, no blank frame
}

void CTopListWindow::Reposition() {
    if (!m_hwnd) return;
    HDC hdc = GetDC(m_hwnd);
    int pw = 0, ph = 0;
    CalcLayout(hdc, pw, ph);
    ReleaseDC(m_hwnd, hdc);

    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    RECT work, mon;
    PTM_GetWorkAreaForRect(rc, work, mon);
    int x = rc.left, y = rc.top;
    int margin = std::max(4, (int)(4 * m_dpi_scale));
    if (x + pw > work.right) x = work.right - pw - margin;
    if (x < work.left) x = work.left + margin;
    if (y + ph > work.bottom) y = work.bottom - ph - margin;
    if (y < work.top) y = work.top + margin;

    // 首显判定：未上屏时截屏天然干净（窗口先摆好位置，再走 OnPaint 的
    // 反色判定路径——与 PNM PositionWindow 首显分支语义一致）
    RECT will = { x, y, x + pw, y + ph };
    if (m_acrylic_active && !m_visible) {
        RefreshBackdrop(will);
        m_last_snap_rect = will;
        m_last_refresh_tick = GetTickCount64();
    }

    if (m_blur_hwnd)
        SetWindowPos(m_blur_hwnd, HWND_TOPMOST, x, y, pw, ph,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, pw, ph,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    m_visible = true;
    if (m_acrylic_active) UpdateWindow(m_hwnd);
}

// ============================================================
// layout & draw
// ============================================================

void CTopListWindow::CalcLayout(HDC hdc, int& out_w, int& out_h) {
    HFONT hOld = (HFONT)SelectObject(hdc, m_font_normal);
    int rows = m_rows_has_value ? (int)m_rows.size() : 1;
    int status_lines = 0;
    if (m_kind == MetricKind::DiskIo && m_io_grade == FileIoGrade::C) status_lines++;
    if (m_io_events_lost > 0) status_lines++;
    SelectObject(hdc, hOld);

    out_w = (int)(BASE_WIDTH * m_dpi_scale);
    out_h = PADDING + TITLE_H + SUMMARY_H
            + status_lines * STATUS_H
            + rows * ROW_HEIGHT
            + PADDING;
    if (out_h < TITLE_H + SUMMARY_H + ROW_HEIGHT + 2 * PADDING)
        out_h = TITLE_H + SUMMARY_H + ROW_HEIGHT + 2 * PADDING;
}

void CTopListWindow::DrawTitleBar(HDC hdc, int w, int y) {
    // 标题：当前指标名（accent 色）+ 副标注（Top N）
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetAccentColor());
    HFONT hOld = (HFONT)SelectObject(hdc, m_font_normal);
    wchar_t title[64];
    swprintf_s(title, 64, L"%s", MetricTitle());
    RECT rc = { PADDING, y, w - PADDING - IDX_W, y + TITLE_H };
    DrawTextW(hdc, title, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    // 关闭按钮 [×]，右侧
    int btn = (int)(22 * m_dpi_scale);
    m_rc_close = { w - PADDING - btn, y + (TITLE_H - btn) / 2, w - PADDING, y + (TITLE_H - btn) / 2 + btn };
    SetTextColor(hdc, m_hover_close ? RGB(255, 120, 120) : GetTextColor());
    RECT rcx = m_rc_close;
    DrawTextW(hdc, L"\x2715", -1, &rcx, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(hdc, hOld);
}

void CTopListWindow::DrawSummary(HDC hdc, int w, int y) {
    wchar_t text[160] = L"";
    wchar_t v[48];
    switch (m_kind) {
    case MetricKind::Mem: {
        wchar_t t[48];
        FormatMem(m_mem_total, t, 48);
        FormatMem(m_mem_used, v, 48);
        swprintf_s(text, 160, L"%s / %s", v, t);
        break;
    }
    case MetricKind::DiskIo:
        if (m_io_view == 0) {
            wchar_t r[32], wr[32];
            FormatSpeed(m_io_read_total, r, 32);
            FormatSpeed(m_io_write_total, wr, 32);
            swprintf_s(text, 160, L"\u2193%s \u2191%s", r, wr);
        } else {
            FormatSpeed(m_summary_value, v, 48);
            swprintf_s(text, 160, L"%s", v);
        }
        break;
    default:
        if (m_summary_value < 0)
            swprintf_s(text, 160, L"--");
        else
            swprintf_s(text, 160, L"%.1f%%", m_summary_value);
        break;
    }
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetTextColor());
    HFONT hOld = (HFONT)SelectObject(hdc, m_font_small);
    RECT rc = { PADDING, y, w - PADDING, y + SUMMARY_H };
    DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(hdc, hOld);
}

void CTopListWindow::DrawStatusLine(HDC hdc, int w, int y) {
    // 黄色警示：档位/丢事件如实标注（口径不冒充，方案 §2.3）
    wchar_t text[256] = L"";
    if (m_kind == MetricKind::DiskIo && m_io_grade == FileIoGrade::C) {
        swprintf_s(text, 256, TR(L"\u26a0 IO 口径：全句柄（含网络）——需要管理员权限获取精确磁盘口径"));
    } else if (m_io_events_lost > 0) {
        swprintf_s(text, 256, TR(L"\u26a0 ETW 缓冲不足，已丢失 %llu 个事件"), m_io_events_lost);
    }
    if (!text[0]) return;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, UseTextTheme() ? RGB(255, 200, 80) : RGB(190, 120, 0));
    HFONT hOld = (HFONT)SelectObject(hdc, m_font_small);
    RECT rc = { PADDING, y, w - PADDING, y + STATUS_H };
    DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(hdc, hOld);
}

void CTopListWindow::DrawRow(HDC hdc, int y, int idx, const TopRow& row) {
    RECT client;
    GetClientRect(m_hwnd, &client);
    int w = client.right;

    int x = PADDING;
    // 1) 序号
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetTextColor());
    HFONT hOld = (HFONT)SelectObject(hdc, m_font_normal);
    wchar_t idxs[8];
    swprintf_s(idxs, 8, L"%d", idx + 1);
    RECT rc_idx = { x, y, x + IDX_W, y + ROW_HEIGHT };
    DrawTextW(hdc, idxs, -1, &rc_idx, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    x += IDX_W;

    // 2) 图标（缓存 miss 画空位，泵提取后重绘）
    HICON hIcon = LookupIcon(row.name, row.pid);
    if (hIcon)
        DrawIconEx(hdc, x, y + (ROW_HEIGHT - ICON_SIZE) / 2, hIcon,
                   ICON_SIZE, ICON_SIZE, 0, nullptr, DI_NORMAL);
    x += ICON_SIZE + (int)(6 * m_dpi_scale);

    // 3) 进程名（.exe 后缀剥除 + 省略号截断；同名合并显示 ×N）
    std::wstring name = row.name;
    if (name.size() > 4) {
        std::wstring ext = name.substr(name.size() - 4);
        if (ext == L".exe" || ext == L".EXE") name = name.substr(0, name.size() - 4);
    }
    if (row.count > 1) {
        wchar_t cnt[16];
        swprintf_s(cnt, 16, L" \u00d7%d", row.count);
        name += cnt;
    }
    int value_w = VALUE_AREA_W;
    int max_name_w = w - PADDING - value_w - x;
    SetTextColor(hdc, GetTextColor());
    SIZE sz;
    GetTextExtentPoint32W(hdc, name.c_str(), (int)name.size(), &sz);
    if (sz.cx > max_name_w && max_name_w > 0) {
        while (name.size() > 3 && sz.cx > max_name_w) {
            name.pop_back();
            name.back() = L'\u2026';
            GetTextExtentPoint32W(hdc, name.c_str(), (int)name.size(), &sz);
        }
    }
    RECT rc_name = { x, y, x + max_name_w, y + ROW_HEIGHT };
    DrawTextW(hdc, name.c_str(), -1, &rc_name, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // 4) 数值（右对齐 accent）
    wchar_t val[64] = L"--";
    if (m_rows_has_value) {
        switch (m_kind) {
        case MetricKind::Mem:
            FormatMem((uint64_t)row.value, val, 64);
            break;
        case MetricKind::DiskIo:
            FormatSpeed(row.value, val, 64);
            break;
        default:
            swprintf_s(val, 64, L"%.1f%%", row.value);
            break;
        }
    }
    SetTextColor(hdc, GetAccentColor());
    RECT rc_val = { w - PADDING - value_w, y, w - PADDING, y + ROW_HEIGHT };
    DrawTextW(hdc, val, -1, &rc_val, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    // 5) 分隔线
    HPEN hOldPen = (HPEN)SelectObject(hdc, m_pen_separator);
    MoveToEx(hdc, PADDING + IDX_W, y + ROW_HEIGHT - 1, nullptr);
    LineTo(hdc, w - PADDING, y + ROW_HEIGHT - 1);
    SelectObject(hdc, hOldPen);

    SelectObject(hdc, hOld);
}

void CTopListWindow::DrawContent(HDC dc, int w, int h, bool content_only) {
    // 边框参与 coverage 双底解算（背景由 ULW 合成提供）
    if (m_acrylic_active) {
        HPEN hPen = CreatePen(PS_SOLID, 1, UseTextTheme() ? RGB(55, 55, 55) : RGB(220, 220, 220));
        HPEN hOldPen = (HPEN)SelectObject(dc, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, w, h);
        SelectObject(dc, hOldBrush);
        SelectObject(dc, hOldPen);
        DeleteObject(hPen);
    } else {
        if (content_only) return;
        HBRUSH hBrush = CreateSolidBrush(GetBgColor());
        HPEN hPen = CreatePen(PS_SOLID, 1, UseTextTheme() ? RGB(55, 55, 55) : RGB(220, 220, 220));
        HBRUSH hOldBrush = (HBRUSH)SelectObject(dc, hBrush);
        HPEN hOldPen = (HPEN)SelectObject(dc, hPen);
        RoundRect(dc, 0, 0, w, h, CORNER_RADIUS * 2, CORNER_RADIUS * 2);
        SelectObject(dc, hOldBrush);
        SelectObject(dc, hOldPen);
        DeleteObject(hBrush);
        DeleteObject(hPen);
    }

    int y = PADDING;
    DrawTitleBar(dc, w, y);
    y += TITLE_H;
    DrawSummary(dc, w, y);
    y += SUMMARY_H;
    if (m_kind == MetricKind::DiskIo && m_io_grade == FileIoGrade::C) {
        DrawStatusLine(dc, w, y);
        y += STATUS_H;
    }
    if (m_io_events_lost > 0 && !(m_kind == MetricKind::DiskIo && m_io_grade == FileIoGrade::C)) {
        DrawStatusLine(dc, w, y);
        y += STATUS_H;
    }

    if (m_rows.empty()) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, UseTextTheme() ? RGB(120, 120, 120) : RGB(160, 160, 160));
        HFONT hOld = (HFONT)SelectObject(dc, m_font_normal);
        const wchar_t* hint = m_rows_has_value ? TR(L"暂无进程") : TR(L"等待首次采样…");
        RECT rc = { PADDING + 2, y, w - PADDING, y + ROW_HEIGHT };
        DrawTextW(dc, hint, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, hOld);
        return;
    }
    for (size_t i = 0; i < m_rows.size(); i++) {
        DrawRow(dc, y, (int)i, m_rows[i]);
        y += ROW_HEIGHT;
    }
}

void CTopListWindow::OnPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);
    RECT client;
    GetClientRect(m_hwnd, &client);
    int w = client.right - client.left;
    int h = client.bottom - client.top;

    if (m_acrylic_active) {
        // ULW：内容在白底/黑底各画一遍解覆盖率，合成后 UpdateLayeredWindow
        if (EnsureLayeredSurface(w, h)) {
            RECT rc_all = { 0, 0, w, h };
            HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(m_cov_a_dc, &rc_all, white);
            DeleteObject(white);
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(m_cov_c_dc, &rc_all, black);
            DeleteObject(black);
            DrawContent(m_cov_a_dc, w, h, true);
            DrawContent(m_cov_c_dc, w, h, true);
            ComposeLayeredSurface(w, h);
        }
        EndPaint(m_hwnd, &ps);
        return;
    }

    // 普通窗口：双缓冲
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
    DrawContent(memDC, w, h, false);
    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    EndPaint(m_hwnd, &ps);
}
