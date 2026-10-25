// dock_manager.cpp — 停靠跟随实现（M3-2，方案 §7）
#include "dock_manager.h"
#include "window_manager.h"
#include "plugin_main.h"
#include "utils.h"
#include <algorithm>

// ============================================================
// 锚窗检测（方案 §7.1 / §3.5 选项 A：类名前缀 + 属主校验 + 标题核对）
// ============================================================

// 属主校验：窗口必须属 TrafficMonitor.exe（防第三方同名窗口）
static bool IsTrafficMonitorProcess(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return true;   // 权限失败放行：宁可多核一次标题，不误杀本进程锚
    wchar_t exe[MAX_PATH] = L"";
    DWORD n = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(h, 0, exe, &n);
    CloseHandle(h);
    if (!ok || !n) return true;
    const wchar_t* base = exe + n;
    while (base > exe && base[-1] != L'\\') base--;
    return _wcsicmp(base, L"TrafficMonitor.exe") == 0;
}

namespace {
struct AnchorEnumCtx {
    DockTarget want;
    HWND found_pnm = nullptr;
    HWND found_tm = nullptr;
};

BOOL CALLBACK AnchorEnumProc(HWND hwnd, LPARAM lp) {
    AnchorEnumCtx* ctx = (AnchorEnumCtx*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t cls[64] = {};
    if (!GetClassNameW(hwnd, cls, 64) || !cls[0]) return TRUE;
    // 类名前缀匹配（PNM 类名带实例指针后缀，前缀稳定——方案 §3.5 选项 A）
    bool is_pnm = wcsncmp(cls, L"PNMDetail_", 10) == 0;
    bool is_tm = wcsncmp(cls, L"TrafficMonitor", 14) == 0;   // TM 主悬浮窗
    if (!is_pnm && !is_tm) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!IsTrafficMonitorProcess(pid)) return TRUE;
    if (is_pnm) {
        // PNM 详情窗再核标题 == "ProcessNetMonitor"（标题稳定可 FindWindow）
        wchar_t title[64] = {};
        GetWindowTextW(hwnd, title, 64);
        if (wcscmp(title, L"ProcessNetMonitor") == 0 && !ctx->found_pnm)
            ctx->found_pnm = hwnd;
    } else if (!ctx->found_tm) {
        ctx->found_tm = hwnd;
    }
    return TRUE;
}
} // namespace

HWND DockFindAnchor(DockTarget target) {
    AnchorEnumCtx ctx;
    ctx.want = target;
    EnumWindows(AnchorEnumProc, (LPARAM)&ctx);
    if (target == DockTarget::PnmDetail) return ctx.found_pnm;
    if (target == DockTarget::TmMain) return ctx.found_tm;
    return ctx.found_pnm ? ctx.found_pnm : ctx.found_tm;   // Auto：PNM 优先
}

// ============================================================
// DockManager
// ============================================================

DockManager& DockManager::Instance() {
    static DockManager s_instance;
    return s_instance;
}

DockState DockManager::StateOf(const CTopListWindow* wnd) const {
    for (auto* w : m_docked) if (w == wnd) return DockState::Docked;
    for (auto* w : m_detached) if (w == wnd) return DockState::Detached;
    return DockState::Floating;
}

bool DockManager::Dock(CTopListWindow* wnd, DockTarget target) {
    if (!wnd || !wnd->GetHwnd()) return false;
    HWND anchor = DockFindAnchor(target);
    if (!anchor) return false;   // 锚不在：保持 Floating，调用方如实提示

    // 换锚 / 换目标：先摘干净旧关系
    bool was_docked = StateOf(wnd) == DockState::Docked;
    if (was_docked && m_anchor == anchor) {
        RelayoutAll();
        return true;
    }
    m_docked.erase(std::remove(m_docked.begin(), m_docked.end(), wnd), m_docked.end());
    m_detached.erase(std::remove(m_detached.begin(), m_detached.end(), wnd), m_detached.end());
    if (m_docked.empty() && m_anchor && m_anchor != anchor) RemoveHooks();

    m_target = target;
    m_anchor = anchor;
    DWORD pid = 0;
    GetWindowThreadProcessId(anchor, &pid);
    m_anchor_pid = pid;
    m_anchor_monitor = MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST);
    GetWindowRect(anchor, &m_anchor_last);

    wnd->m_cfg.docked = 1;
    wnd->m_cfg.dock_target = (int)target;
    // 按 WindowManager 槽位序插入（布局级联稳定）
    int idx = wnd->Index();
    size_t at = m_docked.size();
    for (size_t i = 0; i < m_docked.size(); i++) {
        if (m_docked[i]->Index() > idx) { at = i; break; }
    }
    m_docked.insert(m_docked.begin() + at, wnd);

    InstallHooks();
    RelayoutAll();
    return true;
}

void DockManager::Undock(CTopListWindow* wnd) {
    m_docked.erase(std::remove(m_docked.begin(), m_docked.end(), wnd), m_docked.end());
    m_detached.erase(std::remove(m_detached.begin(), m_detached.end(), wnd), m_detached.end());
    if (wnd) {
        wnd->m_cfg.docked = 0;
        // 解除后窗口留在当前位置：把位置刷进 cfg（浮动语义从此接手）
        RECT rc;
        if (wnd->GetHwnd() && GetWindowRect(wnd->GetHwnd(), &rc)) {
            wnd->m_cfg.x = rc.left;
            wnd->m_cfg.y = rc.top;
        }
    }
    if (m_docked.empty()) RemoveHooks();
    else RelayoutAll();   // 其余 Docked 窗补位
}

void DockManager::RestoreFromConfigs() {
    bool restored_any = false;
    WindowManager& mgr = WindowManager::Instance();
    for (int i = 0; i < mgr.Count(); i++) {
        CTopListWindow* w = mgr.At(i);
        if (!w || w->m_cfg.docked != 1) continue;
        if (Dock(w, (DockTarget)w->m_cfg.dock_target)) {
            restored_any = true;
        } else {
            // 锚此刻不在：不立即放弃——TM 主窗作为锚必然晚于插件 InitOnce
            // 创建（实测 2026-10-26：恢复瞬间锚不存在 → 停靠态全灭）。
            // 保持 docked=1 进待恢复队列，500ms 重试至 30s；超时才按
            // 「锚消失不吸回」定案落 Floating
            m_pending_restore.push_back(w);
        }
    }
    if (restored_any) RelayoutAll();
    if (!m_pending_restore.empty() && !m_restore_timer) {
        m_restore_retry = 0;
        m_restore_timer = SetTimer(nullptr, 2, 500, RestoreRetryProc);
    }
    // 恢复流程结束统一落盘一次（方案 §5 定案；待恢复窗落盘等重试收敛后）
    if (m_pending_restore.empty()) CProcessTopPlugin::Instance().SaveWindowConfigsNow();
}

void CALLBACK DockManager::RestoreRetryProc(HWND, UINT, UINT_PTR id, DWORD) {
    (void)id;
    DockManager& self = Instance();
    if (self.m_pending_restore.empty()) {
        if (self.m_restore_timer) { KillTimer(nullptr, self.m_restore_timer); self.m_restore_timer = 0; }
        return;
    }
    bool any_left = false;
    bool any_docked = false;
    for (CTopListWindow* w : self.m_pending_restore) {
        if (!w || !w->GetHwnd() || !IsWindow(w->GetHwnd())) continue;   // 窗口被 ×：随它去
        if (w->m_cfg.docked != 1) continue;                             // 用户已手动改变状态
        if (self.Dock(w, (DockTarget)w->m_cfg.dock_target)) any_docked = true;
        else any_left = true;
    }
    if (any_docked) self.RelayoutAll();
    if (any_left && ++self.m_restore_retry < 60) return;   // 500ms × 60 = 30s
    // 收敛（全部贴上）或超时：清队、落盘一次
    for (CTopListWindow* w : self.m_pending_restore) {
        if (w && w->m_cfg.docked == 1 &&
            self.StateOf(w) != DockState::Docked) w->m_cfg.docked = 0;   // 超时仍浮着 → 置 0
    }
    self.m_pending_restore.clear();
    if (self.m_restore_timer) { KillTimer(nullptr, self.m_restore_timer); self.m_restore_timer = 0; }
    CProcessTopPlugin::Instance().SaveWindowConfigsNow();
}

void DockManager::OnWindowDestroyed(CTopListWindow* wnd) {
    bool was = StateOf(wnd) != DockState::Floating;
    m_docked.erase(std::remove(m_docked.begin(), m_docked.end(), wnd), m_docked.end());
    m_detached.erase(std::remove(m_detached.begin(), m_detached.end(), wnd), m_detached.end());
    if (!was) return;
    if (m_docked.empty()) RemoveHooks();
    else RelayoutAll();   // 其余 Docked 窗补位重排
}

// ============================================================
// 布局（方案 §7.2 / M3-2 §4）
// ============================================================

void DockManager::RelayoutAll() {
    if (!m_anchor || !IsWindow(m_anchor) || m_docked.empty()) return;
    RECT A;
    GetWindowRect(m_anchor, &A);
    m_anchor_last = A;
    m_anchor_monitor = MonitorFromWindow(m_anchor, MONITOR_DEFAULTTONEAREST);

    // 工作区：锚窗所在显示器（多显示器正确；PNM PNM_GetWorkAreaForRect 同款）
    MONITORINFO mi = { sizeof(mi) };
    HMONITOR mon = MonitorFromWindow(m_anchor, MONITOR_DEFAULTTONEAREST);
    RECT work;
    if (mon && GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    float dpi = PTM_GetDpiScaleForMonitor(mon);
    int gap = (int)(8 * dpi);

    // edge 求值：auto → 默认 right；右侧放不下首窗 → left
    int edge = CProcessTopPlugin::Instance().m_settings.dock_edge;   // 0=auto 1=right 2=left
    int first_w = 0;
    if (m_docked[0] && m_docked[0]->GetHwnd()) {
        RECT r;
        if (GetWindowRect(m_docked[0]->GetHwnd(), &r)) first_w = r.right - r.left;
    }
    if (edge == 0) {
        edge = 1;
        if (A.right + gap + first_w + gap > work.right) edge = 2;
    }

    int x0 = (edge == 1) ? A.right + gap : A.left - gap;
    int x = x0, y = A.top, row_max_h = 0;
    for (auto* w : m_docked) {
        if (!w->GetHwnd()) continue;
        RECT r;
        if (!GetWindowRect(w->GetHwnd(), &r)) continue;
        int ww = r.right - r.left, wh = r.bottom - r.top;
        int wx = (edge == 1) ? x : x - ww;   // 左停靠每窗按自身宽反排
        // 一行满（右停靠判右溢出；左停靠判左溢出）→ 换行到锚窗下方
        bool overflow = (edge == 1) ? (wx + ww > work.right) : (wx < work.left);
        if (overflow && x != x0) {
            x = x0;
            y += row_max_h + gap;
            row_max_h = 0;
            wx = (edge == 1) ? x : x - ww;
        }
        SetWindowPos(w->GetHwnd(), HWND_TOPMOST, wx, y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        x += ww + gap;
        row_max_h = std::max(row_max_h, wh);
    }
}

// ============================================================
// WinEventHook 跟随（方案 §7.3 三段式）
// ============================================================

void DockManager::InstallHooks() {
    if (m_hook_loc || !m_anchor) return;
    // 按 PID 过滤（idProcess）：不挂全系统 LOCATIONCHANGE 洪流，只看锚进程
    m_hook_loc = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                 nullptr, WinEventProc, m_anchor_pid, 0, WINEVENT_OUTOFCONTEXT);
    // MOVESIZEEND(0x000B) 与 LOCATIONCHANGE(0x800B) 数值不连续，两个 hook
    m_hook_end = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND,
                                 nullptr, WinEventProc, m_anchor_pid, 0, WINEVENT_OUTOFCONTEXT);
    m_last_relayout = GetTickCount64();
    if (!m_poll_timer) m_poll_timer = SetTimer(nullptr, 0, 500, PollTimerProc);
}

void DockManager::RemoveHooks() {
    if (m_hook_loc) { UnhookWinEvent(m_hook_loc); m_hook_loc = nullptr; }
    if (m_hook_end) { UnhookWinEvent(m_hook_end); m_hook_end = nullptr; }
    if (m_poll_timer) { KillTimer(nullptr, m_poll_timer); m_poll_timer = 0; }
    if (m_restore_timer) { KillTimer(nullptr, m_restore_timer); m_restore_timer = 0; }
    m_pending_restore.clear();
}

VOID CALLBACK DockManager::WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                        LONG id_object, LONG id_child,
                                        DWORD thread, DWORD time) {
    (void)hook; (void)thread; (void)time;
    DockManager& self = Instance();
    // LOCATIONCHANGE 对滚动条/子对象也触发：只认锚窗本体
    if (id_object != OBJID_WINDOW || hwnd != self.m_anchor) return;
    if (event == EVENT_OBJECT_LOCATIONCHANGE) self.OnAnchorLocationChange();
    else if (event == EVENT_SYSTEM_MOVESIZEEND) self.OnAnchorMoveEnd();
}

void DockManager::OnAnchorLocationChange() {
    if (!m_anchor || !IsWindow(m_anchor)) return;
    RECT now;
    if (!GetWindowRect(m_anchor, &now)) return;
    int dx = now.left - m_anchor_last.left;
    int dy = now.top - m_anchor_last.top;
    if (!dx && !dy) return;
    bool same_size = (now.right - now.left == m_anchor_last.right - m_anchor_last.left &&
                      now.bottom - now.top == m_anchor_last.bottom - m_anchor_last.top);
    HMONITOR mon = MonitorFromWindow(m_anchor, MONITOR_DEFAULTTONEAREST);
    if (same_size && mon == m_anchor_monitor) {
        // 轻路径：纯位移 → 全部 Docked 窗同 delta 平移
        for (auto* w : m_docked) {
            if (!w->GetHwnd()) continue;
            RECT r;
            if (!GetWindowRect(w->GetHwnd(), &r)) continue;
            SetWindowPos(w->GetHwnd(), nullptr, r.left + dx, r.top + dy, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        }
    } else {
        // 尺寸变/跨屏：150ms 节流全量重定位（拖动中锚被 resize 的高频场景）
        ULONGLONG t = GetTickCount64();
        if (t - m_last_relayout >= 150) {
            m_last_relayout = t;
            RelayoutAll();
            return;
        }
    }
    m_anchor_last = now;
    m_anchor_monitor = mon;
}

void DockManager::OnAnchorMoveEnd() {
    // 终判：无节流全量重定位，拖动过程 delta 累计误差一次消除
    RelayoutAll();
}

void CALLBACK DockManager::PollTimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    (void)id;
    DockManager& self = Instance();
    if (self.m_docked.empty()) return;
    if (!self.m_anchor || !IsWindow(self.m_anchor)) {
        self.OnAnchorGone();
        return;
    }
    RECT now;
    if (GetWindowRect(self.m_anchor, &now) &&
        !EqualRect(&now, &self.m_anchor_last)) {
        self.RelayoutAll();
    }
}

void DockManager::OnAnchorGone() {
    // 全部 Docked 窗 → Floating 冻结原位（cfg.docked=0，锚重现不自动吸回）
    for (auto* w : m_docked) {
        if (w) w->m_cfg.docked = 0;
    }
    m_docked.clear();
    m_detached.clear();
    RemoveHooks();
    m_anchor = nullptr;
    CProcessTopPlugin::Instance().m_settings_dirty = true;
}

// ============================================================
// 拖离判定（方案 §5：落点距锚边缘 >48px → Detached；否则吸回布局位）
// ============================================================

void DockManager::OnWindowDragged(CTopListWindow* wnd) {
    if (StateOf(wnd) != DockState::Docked) return;
    if (!wnd->GetHwnd() || !m_anchor || !IsWindow(m_anchor)) return;
    RECT W, A;
    if (!GetWindowRect(wnd->GetHwnd(), &W) || !GetWindowRect(m_anchor, &A)) return;
    // 矩形间距离（0 = 相邻/重叠）
    int dx = std::max(0, std::max((int)(A.left - W.right), (int)(W.left - A.right)));
    int dy = std::max(0, std::max((int)(A.top - W.bottom), (int)(W.top - A.bottom)));
    double dist = sqrt((double)(dx * dx + dy * dy));
    float dpi = PTM_GetDpiScaleForMonitor(MonitorFromWindow(m_anchor, MONITOR_DEFAULTTONEAREST));
    double thresh = 48.0 * dpi;
    if (dist > thresh) {
        // 拖离：Detached，其余 Docked 窗补位（cfg 位置已是拖后位置）
        m_docked.erase(std::remove(m_docked.begin(), m_docked.end(), wnd), m_docked.end());
        m_detached.erase(std::remove(m_detached.begin(), m_detached.end(), wnd), m_detached.end());
        m_detached.push_back(wnd);
        wnd->m_cfg.docked = 0;
        if (m_docked.empty()) RemoveHooks();
        else RelayoutAll();
    } else {
        // 微调：仍 Docked，吸回布局位
        RelayoutAll();
    }
}
