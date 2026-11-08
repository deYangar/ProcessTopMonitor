// window_manager.cpp — 多实例生命周期 + windows[] 持久化（M3-1）
// + 组显示控制/组布局/锚窗子类化跟随（M6 §4.2，抄 PNM tooltip_popup 机制）
#include "window_manager.h"
#include "plugin_main.h"
#include "utils.h"   // WM_PTM_RECLAIM / DPI helpers
#include <algorithm>
#include <commctrl.h>   // SetWindowSubclass / DefSubclassProc（锚子类化框架）

// ============================================================
// PNM 悬浮窗避让（2026-11-06 green 位反馈二治本）
// PTM 组布局与 PNM PositionWindow 是同款方位公式（上方优先），hover 弹组
// 与 PNM tooltip 弹向完全相同的矩形 → 叠死。检测同进程可见 PNM 窗（类名
// 前缀 PNMTooltip_，含实例指针后缀），rect 相交则 nudge；候选优先级同
// PNM 避让 TM 原生 tooltip 的框架（tooltip_popup.cpp:1700）：副轴平移 >
// 主轴推开 > 锚对面翻面，4 pass 迭代收敛。仅 hover 自动定位路径调用；
// 常驻/pin 用户手放的位置不参与避让。
// ============================================================
namespace {

struct PnmAvoidCtx {
    std::vector<RECT>* out;
    DWORD pid;
};

BOOL CALLBACK CollectPNMPopup(HWND h, LPARAM lp) {
    auto* c = (PnmAvoidCtx*)lp;
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64)) return TRUE;
    if (wcsncmp(cls, L"PNMTooltip_", 11) != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h)) return TRUE;
    RECT r;
    if (GetWindowRect(h, &r) && r.right > r.left && r.bottom > r.top)
        c->out->push_back(r);
    return TRUE;
}

// 找同进程可见 PNM 窗的包围盒（问题④兜底轮询用：PNM 在场判定 +
// 相交检测）。两插件同住 TM UI 线程，枚举期间无并发变化
struct PnmPresence {
    RECT bounds{};
    bool any = false;
};

BOOL CALLBACK CollectPNMPresence(HWND h, LPARAM lp) {
    auto* c = (PnmPresence*)lp;
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64)) return TRUE;
    if (wcsncmp(cls, L"PNMTooltip_", 11) != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    RECT r;
    if (GetWindowRect(h, &r) && r.right > r.left && r.bottom > r.top) {
        if (c->any) UnionRect(&c->bounds, &c->bounds, &r);
        else c->bounds = r;
        c->any = true;
    }
    return TRUE;
}

void AvoidPNMPopups(int& x, int& y, int pw, int ph, const RECT& anchor,
                    const RECT& work, float dpi) {
    std::vector<RECT> avoid;
    PnmAvoidCtx ctx{ &avoid, GetCurrentProcessId() };
    EnumWindows(CollectPNMPopup, (LPARAM)&ctx);
    if (avoid.empty()) return;
    int tip_margin = std::max(4, (int)(8 * dpi));
    int margin = std::max(4, (int)(4 * dpi));
    bool at_bottom = anchor.bottom >= work.bottom - 2;
    bool at_top    = anchor.top <= work.top + 2;
    bool at_left   = anchor.left <= work.left + 2;
    bool at_right  = anchor.right >= work.right - 2;
    auto intersects = [&](int cx, int cy, const RECT& arc) {
        return cx < arc.right && cx + pw > arc.left &&
               cy < arc.bottom && cy + ph > arc.top;
    };
    for (int pass = 0; pass < 4; pass++) {
        bool moved = false;
        for (const auto& rc : avoid) {
            RECT arc = { rc.left - tip_margin, rc.top - tip_margin,
                         rc.right + tip_margin, rc.bottom + tip_margin };
            if (!intersects(x, y, arc)) continue;
            // 候选1（副轴平移）：水平弹出→左右让；垂直弹出→上下让
            int c1x = x, c1y = y;
            bool ok = false;
            if (at_bottom || at_top || (!at_left && !at_right)) {
                int right = arc.right + margin;
                int left = arc.left - pw - margin;
                if (right + pw <= work.right - margin) { c1x = right; ok = true; }
                else if (left >= work.left + margin) { c1x = left; ok = true; }
            } else {
                int down = arc.bottom + margin;
                int up = arc.top - ph - margin;
                if (down + ph <= work.bottom - margin) { c1y = down; ok = true; }
                else if (up >= work.top + margin) { c1y = up; ok = true; }
            }
            if (ok && !intersects(c1x, c1y, arc)) { x = c1x; y = c1y; moved = true; continue; }
            // 候选2（主轴推开）：沿弹出方向继续远离锚
            int c2x = x, c2y = y;
            ok = false;
            if (at_bottom || (!at_top && !at_left && !at_right)) {
                int cand = arc.top - ph;
                if (cand >= work.top + margin) { c2y = cand; ok = true; }
            } else if (at_top) {
                int cand = arc.bottom;
                if (cand + ph <= work.bottom - margin) { c2y = cand; ok = true; }
            } else if (at_left) {
                int cand = arc.right;
                if (cand + pw <= work.right - margin) { c2x = cand; ok = true; }
            } else if (at_right) {
                int cand = arc.left - pw;
                if (cand >= work.left + margin) { c2x = cand; ok = true; }
            }
            if (ok && !intersects(c2x, c2y, arc)) { x = c2x; y = c2y; moved = true; continue; }
            // 候选3（翻面）：锚的另一侧，clamp 进工作区
            int c3x = x, c3y = y;
            if (at_bottom || (!at_top && !at_left && !at_right))
                c3y = anchor.bottom + margin;
            else if (at_top)
                c3y = anchor.top - ph - margin;
            else if (at_left)
                c3x = anchor.left - pw - margin;
            else if (at_right)
                c3x = anchor.right + margin;
            int wl = (int)work.left + margin, wr = (int)work.right - pw - margin;
            int wt = (int)work.top + margin, wb = (int)work.bottom - ph - margin;
            if (wr > wl) c3x = std::min(std::max(c3x, wl), wr);
            if (wb > wt) c3y = std::min(std::max(c3y, wt), wb);
            if (!intersects(c3x, c3y, arc)) { x = c3x; y = c3y; moved = true; }
        }
        if (!moved) break;
    }
}

}   // namespace

WindowManager& WindowManager::Instance() {
    static WindowManager s_instance;
    return s_instance;
}

// ============================================================
// 生命周期
// ============================================================

int WindowManager::RestoreAll(const std::vector<WindowCfg>& cfgs) {
    int made = 0;
    for (const auto& c : cfgs) {
        if (AddWindow(c) >= 0) made++;
    }
    return made;
}

int WindowManager::AddWindow(const WindowCfg& cfg) {
    if ((int)m_windows.size() >= CTopListWindow::MAX_WINDOWS) return -1;
    m_ui_tid = GetCurrentThreadId();   // 首窗创建必在 UI 线程（收尸分派依据）
    auto wnd = std::make_unique<CTopListWindow>();

    POINT pos;
    const POINT* suggested = nullptr;
    if (cfg.x != INT_MIN) {
        pos.x = cfg.x;
        pos.y = cfg.y;
        suggested = &pos;
    } else {
        // 级联默认位（仅 Create 初始位；M6 起显示位置由组布局/记忆位置
        // 决定，级联位不回填 cfg——x/y 语义 = 拖过的位置记忆）
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        pos.x = wa.left + (wa.right - wa.left) / 10 + 28 * m_cascade;
        pos.y = wa.top + (wa.bottom - wa.top) / 10 + 28 * m_cascade;
        if (pos.x > wa.right - 320) pos.x = wa.right - 320;
        if (pos.y > wa.bottom - 240) pos.y = wa.top + 40;
        m_cascade = (m_cascade + 1) % 12;
        suggested = &pos;
    }
    if (!wnd->Create(suggested)) return -1;   // 资源枯竭：调用方不崩溃（方案 §7.2）

    wnd->m_index = (int)m_windows.size();
    wnd->ApplyCfg(cfg);
    // 外观/不透明度/颜色模式是全局设置（口径共享，方案偏差表定案）
    auto& st = CProcessTopPlugin::Instance().m_settings;
    wnd->SetAppearance(st.color_mode, st.popup_effect, st.popup_opacity);
    m_windows.push_back(std::move(wnd));
    RecomputeBoundMask();
    // 从「零窗」恢复时重启定时器（全部关闭后定时器已停，方案 §8.3）
    if (Count() == 1) CProcessTopPlugin::Instance().RestartRefreshTimer();
    // 开窗即拍（方案一）：不等下个周期，立即补一拍——新绑维度的采集
    //（GPU 冷启动补拍/ETW 会话建立）当拍启动，首帧数据尽快就位
    CProcessTopPlugin::Instance().KickRefreshTick();
    EnsureHoverTimer();   // hover 状态机 timer 挂组长窗（首建/重组，M6 §4.3.2）
    return (int)m_windows.size() - 1;
}

int WindowManager::DestroyAt(int idx) {
    if (idx < 0 || idx >= (int)m_windows.size()) return Count();
    m_windows[idx]->Destroy();
    m_windows.erase(m_windows.begin() + idx);
    for (int i = 0; i < Count(); i++) m_windows[i]->m_index = i;
    if (m_windows.empty()) m_cascade = 0;   // 全灭级联归零（方案 §7.7）
    RecomputeBoundMask();
    EnsureHoverTimer();   // 组长可能已换（销毁的若是组长，timer 随窗消失）
    return Count();
}

void WindowManager::DestroyAll() {
    while (!m_windows.empty()) DestroyAt(0);
}

// ============================================================
// 组显示控制（M6 §4.2）
// ============================================================

bool WindowManager::GroupVisible() const {
    for (auto& w : m_windows)
        if (w->IsVisible()) return true;
    return false;
}

void WindowManager::ShowGroupAt(const RECT& anchor, HWND follow_wnd) {
    LayoutGroup(anchor);
    // 弹回即新：摆位上屏后立即灌一次快照（隐藏期数据已保鲜）
    CProcessTopPlugin::Instance().TopListRefreshFromSnapshot();
    // 开窗即拍（方案一）：弹回时刻立即补一拍——真停后弹回的冷启动
    //（m_gpu_last 已清空、ETW 会话已停）在用户看到的这一刻就开始，
    // 不等下个周期（refresh_ms 最长 2s）
    CProcessTopPlugin::Instance().KickRefreshTick();
    if (follow_wnd) SetAnchorFollow(follow_wnd);
}

void WindowManager::HideGroup() {
    for (auto& w : m_windows) w->Hide();
    // 事件跟随即时卸载（锚子类化只在 hover 可见且未 pin 期间存在；
    // HoverTick 的 reconcile 每 tick 也会收敛，M6 §4.2.3）
    DetachAnchorTracker();
}

void WindowManager::CalcGroupLayout(const RECT& anchor, bool resident,
                                     std::vector<SIZE>& sz,
                                     std::vector<bool>& use_mem,
                                     int& x, int& y, int& pw, int& ph,
                                     bool avoid_pnm) {
    // M6 §4.2.2 组布局纯计算（LayoutGroup 摆窗前段；问题④抽出供
    // PnmOrderReconcile 的「理论落点 vs 实际位置」比对共用）：
    // 1) 各窗尺寸 + 记忆位窗标记（常驻/pin 且有记忆位的窗不参与排布）
    // 2) 横排 wrap 预演算 → 组包围盒
    // 3) 组长位置 = PNM PositionWindow 方位公式
    // 4) 工作区 clamp
    // 5) 可选 PNM 避让（avoid_pnm；理论落点计算传 false）
    int n = Count();
    sz.assign(n, SIZE{ 0, 0 });
    use_mem.assign(n, false);
    HDC hdc = m_windows[0]->GetHwnd() ? GetDC(m_windows[0]->GetHwnd())
                                      : GetDC(nullptr);
    for (int i = 0; i < n; i++) {
        int w = 0, h = 0;
        m_windows[i]->CalcLayout(hdc, w, h);
        sz[i] = { w, h };
        use_mem[i] = resident && m_windows[i]->m_cfg.x != INT_MIN;
    }
    if (m_windows[0]->GetHwnd() && hdc)
        ReleaseDC(m_windows[0]->GetHwnd(), hdc);
    else if (hdc)
        ReleaseDC(nullptr, hdc);

    RECT work, mon;
    PTM_GetWorkAreaForRect(anchor, work, mon);
    float dpi = PTM_GetDpiScaleForRect(anchor);
    int gap = (int)(8 * dpi);
    int margin = std::max(4, (int)(4 * dpi));

    // 排布窗横排 wrap 预演算（x0 取工作区左界近似；求包围盒供方位公式）
    auto flow_bounds = [&](int x0, int y0, SIZE& total) {
        int fx = x0, fy = y0, row_h = 0, max_w = 0;
        for (int i = 0; i < n; i++) {
            if (use_mem[i]) continue;
            if (fx > x0 && fx + sz[i].cx > work.right - margin) {   // 换行
                fx = x0;
                fy += row_h + gap;
                row_h = 0;
            }
            max_w = std::max(max_w, (int)(fx + sz[i].cx - x0));
            fx += (int)sz[i].cx + gap;
            row_h = std::max(row_h, (int)sz[i].cy);
        }
        total = { max_w, (row_h > 0 || fy > y0) ? (fy + row_h - y0) : 0 };
    };
    SIZE total{};
    flow_bounds(work.left + margin, work.top + margin, total);
    pw = total.cx;
    ph = total.cy;

    // 组长位置：PNM PositionWindow 方位公式（tooltip_popup.cpp:1644-1690）
    int aw = anchor.right - anchor.left;
    int ah = anchor.bottom - anchor.top;
    bool at_bottom = anchor.bottom >= work.bottom - 2;
    bool at_top    = anchor.top <= work.top + 2;
    bool at_left   = anchor.left <= work.left + 2;
    bool at_right  = anchor.right >= work.right - 2;

    if (at_bottom) {          // 任务栏在下 → 上方
        x = anchor.left + (aw - pw) / 2;
        y = anchor.top - ph - margin;
    } else if (at_top) {      // 任务栏在上 → 下方
        x = anchor.left + (aw - pw) / 2;
        y = anchor.bottom + margin;
    } else if (at_left) {     // 左侧 → 右侧
        x = anchor.right + margin;
        y = anchor.top + (ah - ph) / 2;
    } else if (at_right) {    // 右侧 → 左侧
        x = anchor.left - pw - margin;
        y = anchor.top + (ah - ph) / 2;
    } else {
        // 悬浮主窗不贴边：优先上方，放不下 → 下方
        x = anchor.left + (aw - pw) / 2;
        if (anchor.top - ph - margin >= work.top)
            y = anchor.top - ph - margin;
        else
            y = anchor.bottom + margin;
    }
    if (x + pw > work.right) x = work.right - pw - margin;
    if (x < work.left) x = work.left + margin;
    if (y + ph > work.bottom) y = work.bottom - ph - margin;
    if (y < work.top) y = work.top + margin;

    // PNM 悬浮窗避让：仅 hover 自动定位路径（常驻/pin = 用户手放，尊重）
    if (avoid_pnm && !resident)
        AvoidPNMPopups(x, y, pw, ph, anchor, work, dpi);
}

void WindowManager::LayoutGroup(const RECT& anchor) {
    // M6 §4.2.2 组布局（计算见 CalcGroupLayout）：
    // 3) 有记忆位置（x/y != INT_MIN）且当前为常驻/pin 态的窗：用记忆
    //    位置，不参与排布
    // 4) 逐窗 ShowAt()
    if (m_windows.empty()) return;
    auto& plugin = CProcessTopPlugin::Instance();
    bool resident = (plugin.m_settings.popup_mode == 1) || plugin.GroupPinned();

    std::vector<SIZE> sz;
    std::vector<bool> use_mem;
    int x, y, pw, ph;
    CalcGroupLayout(anchor, resident, sz, use_mem, x, y, pw, ph, true);

    int n = Count();
    RECT work, mon;
    PTM_GetWorkAreaForRect(anchor, work, mon);
    float dpi = PTM_GetDpiScaleForRect(anchor);
    int gap = (int)(8 * dpi);
    int margin = std::max(4, (int)(4 * dpi));

    // 真排布（wrap 判定用组长真实 x）
    int cx = x, cy = y, row_h = 0;
    for (int i = 0; i < n; i++) {
        if (use_mem[i]) {
            m_windows[i]->ShowAt(m_windows[i]->m_cfg.x, m_windows[i]->m_cfg.y);
            continue;
        }
        if (cx > x && cx + sz[i].cx > work.right - margin) {   // 换行
            cx = x;
            cy += row_h + gap;
            row_h = 0;
        }
        m_windows[i]->ShowAt(cx, cy);
        cx += (int)sz[i].cx + gap;
        row_h = std::max(row_h, (int)sz[i].cy);
    }
    m_popup_anchor = anchor;
}

void WindowManager::PnmOrderReconcile() {
    // 问题④兜底轮询（消息丢失/时序错乱的收敛网，HoverTick 每 ≥500ms
    // 调）：不对称秩序——PNM 恒占方位公式正位（PNM 侧已移除对 PTM 的
    // 避让），PTM 以 PNM 矩形为参照让到旁边。判定用「组长是否在理论
    // 落点」而不是裸「是否相交」：让开之后即使包围盒仍与 PNM 相邻近
    // 也不重排，杜绝抖动循环。pin/常驻 = 用户手放位置，永不参与。
    // PNM 收起时本函数不归位（2026-11-06 反馈）：归位会让组在鼠标
    // 半路上跳走——去点让位处的 PTM 时鼠标离开 TM → PNM 先消失 → 组
    // 若跳回正位就脱离光标 → 300ms 宽限后连锁收起。让位处就地不动，
    // 收起后下次弹出 LayoutGroup 自然落回正位（PNM 不在场无窗可避）
    if (m_windows.empty()) return;
    auto& plugin = CProcessTopPlugin::Instance();
    if (plugin.m_settings.popup_mode == 1 || plugin.GroupPinned()) return;
    if (!GroupVisible()) return;

    PnmPresence pnm;
    EnumWindows(CollectPNMPresence, (LPARAM)&pnm);

    // PTM 组包围盒（可见窗 union）
    RECT group{};
    bool any_vis = false;
    for (auto& w : m_windows) {
        if (!w->IsVisible()) continue;
        HWND h = w->GetHwnd();
        RECT r;
        if (!h || !GetWindowRect(h, &r)) continue;
        if (any_vis) UnionRect(&group, &group, &r);
        else { group = r; any_vis = true; }
    }
    if (!any_vis) return;

    // 理论落点 = 无避让的方位公式输出（与 LayoutGroup 同一份公式，
    // CalcGroupLayout 单一事实源）；组长实际位与之重合 ±2px 视为「在
    // 正位」——AvoidPNMPopups 的让位位移以整窗宽计，远超此容差
    std::vector<SIZE> sz;
    std::vector<bool> use_mem;
    int tx, ty, tw, th;
    CalcGroupLayout(m_popup_anchor, false, sz, use_mem, tx, ty, tw, th, false);
    HWND leader = m_windows[0]->GetHwnd();
    RECT lr;
    if (!leader || !GetWindowRect(leader, &lr)) return;
    bool at_theoretical = (abs(lr.left - tx) <= 2 && abs(lr.top - ty) <= 2);

    if (pnm.any) {
        RECT inter;
        if (IntersectRect(&inter, &group, &pnm.bounds) && at_theoretical) {
            // PNM 压到组上且 PTM 还在正位：重排让位（LayoutGroup 内
            // AvoidPNMPopups 枚举到 PNM 窗自然让开；幂等）
            LayoutGroup(m_popup_anchor);
        }
    }
    // PNM 全部收起：不归位（见函数头 2026-11-06 注释），本次显示会话
    // 停在让位处；组收起后下次弹出走理论落点自然回正位
}

void WindowManager::GroupDeltaMove(int dx, int dy) {
    // 纯位移轻量路径（M6 §4.2.3）：不算布局，每窗 blur+内容同参数连续
    // SetWindowPos + 工作区 clamp——基准按 anchor 推进，无累积漂移
    if (!dx && !dy) return;
    for (auto& w : m_windows) {
        HWND h = w->GetHwnd();
        if (!h || !IsWindow(h) || !w->IsVisible()) continue;
        RECT cur;
        if (!GetWindowRect(h, &cur)) continue;
        int wd = cur.right - cur.left, ht = cur.bottom - cur.top;
        int x = cur.left + dx, y = cur.top + dy;
        RECT work, monr;
        PTM_GetWorkAreaForRect(m_last_seen_anchor, work, monr);
        int margin = std::max(4, (int)(4 * w->m_dpi_scale));
        if (x + wd > work.right) x = work.right - wd - margin;
        if (x < work.left) x = work.left + margin;
        if (y + ht > work.bottom) y = work.bottom - ht - margin;
        if (y < work.top) y = work.top + margin;
        if (w->m_blur_hwnd)
            SetWindowPos(w->m_blur_hwnd, HWND_TOP, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(h, HWND_TOP, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// ============================================================
// 锚窗子类化跟随（M6 §4.2.3；2026-11-06 根治改 SetWindowSubclass）
//
// 手搓 SetWindowLongPtrW 链在跨插件叠挂下有两种必崩环形态（14:04 自指环
// / 14:31 互指环，均 c00000fd 栈溢出，dump 实证见方案 §1.2/§1.3-v2）：
// detach 方留 orphan 残链时本方 proc 地址被对方当 orig 持有，此后任一方
// re-attach 即成环。治本 = comctl32 SetWindowSubclass：GWLP_WNDPROC 链头
// 挂的是中性 master_proc，子类节点登记在 master 内部链表，本方 proc 地
// 址**不被任何一方持有**——摘除（RemoveWindowSubclass）从表里删节点，
// 表空自动还原原 proc，环在物理上不可构造。
// ============================================================

namespace {
// 子类 id（进程内本插件唯一即可；comctl32 按 (pfn, id) 寻址）
constexpr UINT_PTR kAnchorSubclassId = 0x50544D41;   // 'PTMA'
}

LRESULT CALLBACK WindowManager::AnchorProc(HWND hwnd, UINT msg, WPARAM wp,
                                           LPARAM lp, UINT_PTR /*id*/,
                                           DWORD_PTR /*ref*/) {
    WindowManager* self = &Instance();
    if (self->m_anchor_wnd == hwnd) {
        switch (msg) {
        case WM_ENTERSIZEMOVE:
            self->m_anchor_in_sizemove = true;
            break;
        case WM_EXITSIZEMOVE:
            // 松手：全量重定位（布局公式重算，M6 §4.2.3）
            self->m_anchor_in_sizemove = false;
            self->AnchorFullPass(self->m_last_seen_anchor);
            break;
        case WM_WINDOWPOSCHANGED: {
            WINDOWPOS* p = (WINDOWPOS*)lp;
            if (p && !(p->flags & SWP_NOMOVE)) {
                RECT na = { p->x, p->y, p->x + p->cx, p->y + p->cy };
                self->OnAnchorPosChanged(na, !(p->flags & SWP_NOSIZE));
            }
            break;
        }
        case WM_NCDESTROY:
            // 锚窗销毁：comctl32 随窗口清理子类表，这里只清状态
            self->m_anchor_wnd = nullptr;
            self->m_have_last_seen_anchor = false;
            break;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void WindowManager::SetAnchorFollow(HWND anchor) {
    if (anchor == m_anchor_wnd) return;   // 幂等：HoverTick 每 tick 调
    DetachAnchorTracker();
    if (!anchor || !IsWindow(anchor)) return;
    // 仅同线程窗口（弹窗与 TM 主窗同住主线程；跨线程子类化不安全）。
    // 注意 GetWindowThreadProcessId 的返回值才是线程 ID，out 参数是进程
    // ID——PNM 曾把两者读反导致锚窗跟踪永远装不上，照抄注释防重蹈
    DWORD pid_anchor = 0;
    DWORD tid_anchor = GetWindowThreadProcessId(anchor, &pid_anchor);
    if (tid_anchor != GetCurrentThreadId()) return;
    if (!SetWindowSubclass(anchor, AnchorProc, kAnchorSubclassId, 0)) return;
    m_anchor_wnd = anchor;
    m_anchor_in_sizemove = false;
    m_have_last_seen_anchor = false;   // 首个位置事件只建基准、不跟随
}

void WindowManager::DetachAnchorTracker() {
    if (!m_anchor_wnd) return;
    HWND hwnd = m_anchor_wnd;
    m_anchor_wnd = nullptr;
    m_have_last_seen_anchor = false;
    m_anchor_in_sizemove = false;
    // 从 master 链表摘本节点（被叠场景同样干净）；表空 comctl32 自动还原
    if (IsWindow(hwnd))
        RemoveWindowSubclass(hwnd, AnchorProc, kAnchorSubclassId);
}

void WindowManager::OnAnchorPosChanged(const RECT& new_anchor, bool sized) {
    if (!GroupVisible() || !m_anchor_wnd) return;
    HMONITOR mon_now = MonitorFromRect(&new_anchor, MONITOR_DEFAULTTONEAREST);
    if (!m_have_last_seen_anchor) {
        m_last_seen_anchor = new_anchor;
        m_last_seen_mon = mon_now;
        m_have_last_seen_anchor = true;
        return;   // 首事件只建基准、不跟随
    }
    int dx = new_anchor.left - m_last_seen_anchor.left;
    int dy = new_anchor.top - m_last_seen_anchor.top;
    bool mon_changed = (mon_now != m_last_seen_mon);
    if (sized || mon_changed) {
        // 尺寸/显示器变化：布局公式的输入变了，节流 100ms 转全量
        if (GetTickCount64() - m_last_full_pass_tick >= 100)
            AnchorFullPass(new_anchor);
    } else if (dx || dy) {
        GroupDeltaMove(dx, dy);
    }
    m_last_seen_anchor = new_anchor;
    m_last_seen_mon = mon_now;
    // 锚 rect 变化回写：供 HoverTick 轮询兜底比对（PNM reconcile 语义）
    m_popup_anchor = new_anchor;
}

void WindowManager::AnchorFullPass(const RECT& anchor_rect) {
    if (!GroupVisible()) return;
    RECT r = anchor_rect;
    if (r.right <= r.left || r.bottom <= r.top) {
        RECT wr;
        if (m_anchor_wnd && GetWindowRect(m_anchor_wnd, &wr)) r = wr;
        else return;
    }
    m_last_full_pass_tick = GetTickCount64();
    LayoutGroup(r);
}

// ============================================================
// hover timer（组长窗，M6 §4.3.2）
// ============================================================

void WindowManager::EnsureHoverTimer() {
    if (m_windows.empty()) return;
    HWND h = m_windows[0]->GetHwnd();
    if (h && IsWindow(h)) SetTimer(h, 2, 100, nullptr);   // 幂等：同 id 重设
    EnsureWatchdog();
}

// ---- 子类化看门狗（M6 §4.3.4，照抄 PNM :572-587 + :87-95）----

void WindowManager::EnsureWatchdog() {
    // message-only 窗（HWND_MESSAGE）+ 原生注册 proc（不经 GWLP_WNDPROC
    // 安装，EnumWindows 注入器扫描不可见）+ 1s timer。与组长窗同线程
    // 创建：WM_TIMER 及修复都跑在属主线程——从属主线程修复子类化是
    // 文档安全路径
    if (m_watchdog_hwnd) return;
    HINSTANCE inst = s_dll_hinst ? s_dll_hinst : (HINSTANCE)GetModuleHandleW(nullptr);
    wchar_t cls[64];
    swprintf_s(cls, 64, L"PTMWatchdog_%p", inst);
    WNDCLASSEXW wcExist = {};
    wcExist.cbSize = sizeof(wcExist);
    if (!GetClassInfoExW(inst, cls, &wcExist)) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WatchdogProc;
        wc.hInstance = inst;
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
    }
    m_watchdog_hwnd = CreateWindowExW(0, cls, L"", WS_OVERLAPPED,
                                      0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (m_watchdog_hwnd) SetTimer(m_watchdog_hwnd, 3, 1000, nullptr);
}

LRESULT CALLBACK WindowManager::WatchdogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == 3) {
        if (g_shutting_down) return 0;   // DLL 卸载期跳过修复
        auto& mgr = Instance();
        if (!mgr.m_windows.empty()) {
            HWND leader = mgr.m_windows[0]->GetHwnd();
            if (leader && IsWindow(leader) &&
                GetWindowLongPtrW(leader, GWLP_WNDPROC) !=
                    (LONG_PTR)CTopListWindow::StaticWndProc) {
                // 子类化被外部注入器剥掉：hover WM_TIMER 正被派发到
                // DefWindowProc（或注入器 proc），HoverTick 已死——重装
                // proc + timer（弹窗永不消失的隐患治本）
                SetWindowLongPtrW(leader, GWLP_WNDPROC,
                                  (LONG_PTR)CTopListWindow::StaticWndProc);
                SetTimer(leader, 2, 100, nullptr);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// 数据广播 / 配置 / 回收
// ============================================================

void WindowManager::RefreshAll(const MetricsSnapshot& snap, bool snap_valid,
                               int default_topn) {
    (void)snap_valid;
    for (auto& w : m_windows) {
        if (!w->GetHwnd() || !IsWindow(w->GetHwnd())) continue;
        // 隐藏窗照灌数据（M6：hover 隐藏 ≠ 关闭，弹回即新；显示只由
        // ShowAt/组显示驱动——UpdateFromSnapshot 对 !m_visible 早退）
        // first_tick 语义 = 该窗绑定维度尚无有效数据（缺陷②修复：不再以
        // CPU 口径一刀切强制重绘，mem/vram 首拍即有值）
        const MetricView* view = nullptr;
        switch (w->GetKind()) {
        case MetricKind::Mem: view = &snap.mem; break;
        case MetricKind::DiskIo:
            view = (w->m_cfg.io_view == 1) ? &snap.io_read
                 : (w->m_cfg.io_view == 2) ? &snap.io_write : &snap.io;
            break;
        case MetricKind::Gpu: view = &snap.gpu; break;
        case MetricKind::Vram: view = &snap.vram; break;
        default: view = &snap.cpu; break;
        }
        bool first_tick = !view || !view->has_value;
        int topn = w->m_cfg.topn;
        if (topn != 5 && topn != 10 && topn != 15) topn = default_topn;
        w->UpdateFromSnapshot(snap, w->GetKind(), topn, w->m_cfg.io_view, first_tick);
    }
}

void WindowManager::PostToAll(UINT msg, WPARAM wp, LPARAM lp) {
    // 发给全部活窗（含隐藏窗）：REFRESH 在 UpdateFromSnapshot 里对隐藏窗
    // 早退（省重绘），LANG_CHANGED 必须也达隐藏窗——否则重显后文案停留
    // 旧语言
    for (auto& w : m_windows) {
        if (w->GetHwnd() && IsWindow(w->GetHwnd()))
            PostMessage(w->GetHwnd(), msg, wp, lp);
    }
}

void WindowManager::ForEach(std::function<void(CTopListWindow&, int idx)> fn) {
    for (int i = 0; i < Count(); i++) fn(*m_windows[i], i);
}

CTopListWindow* WindowManager::At(int idx) {
    if (idx < 0 || idx >= Count()) return nullptr;
    return m_windows[idx].get();
}

std::vector<WindowCfg> WindowManager::CollectConfigs() const {
    std::vector<WindowCfg> out;
    out.reserve(m_windows.size());
    for (auto& w : m_windows) out.push_back(w->m_cfg);
    return out;
}

void WindowManager::RecomputeBoundMask() {
    // M6 §4.2.5 语义修正：「存在」（配置内）窗口并集——hover 隐藏 ≠
    // 关闭，窗随时弹回，ETW/GPU 冷启动要预热、体验差；PNM 亦常采。
    // mask==0 ⇔ windows 清空（RefreshTick 的采集停转判定随之改看窗数）
    int mask = 0;
    for (auto& w : m_windows) {
        mask |= PtKindMask(w->GetKind());
    }
    m_bound_mask.store(mask, std::memory_order_relaxed);
}

void WindowManager::ReclaimDead() {
    bool any_dead = false;
    for (auto& w : m_windows) {
        if (w->GetHwnd() && !IsWindow(w->GetHwnd())) { any_dead = true; break; }
    }
    if (!any_dead) return;
    // erase/reindex 会与 UI 线程的菜单遍历并发：tick 线程调进来时先转交
    // UI 线程（WM_APP 段消息），由 WM_PTM_RECLAIM 处理器再入本函数真删；
    // 已在 UI 线程（或无活窗可转交且无并发方）则直接清
    if (GetCurrentThreadId() != m_ui_tid) {
        for (auto& w : m_windows) {
            if (w->GetHwnd() && IsWindow(w->GetHwnd())) {
                PostMessage(w->GetHwnd(), WM_PTM_RECLAIM, 0, 0);
                return;
            }
        }
    }
    for (size_t i = 0; i < m_windows.size();) {
        if (m_windows[i]->GetHwnd() && !IsWindow(m_windows[i]->GetHwnd()))
            m_windows.erase(m_windows.begin() + i);
        else i++;
    }
    for (int i = 0; i < Count(); i++) m_windows[i]->m_index = i;
    if (m_windows.empty()) m_cascade = 0;
    RecomputeBoundMask();
    EnsureHoverTimer();   // 组长可能已换
}
