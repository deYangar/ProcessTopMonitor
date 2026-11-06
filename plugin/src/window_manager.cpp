// window_manager.cpp — 多实例生命周期 + windows[] 持久化（M3-1）
// + 组显示控制/组布局/锚窗子类化跟随（M6 §4.2，抄 PNM tooltip_popup 机制）
#include "window_manager.h"
#include "plugin_main.h"
#include "utils.h"   // WM_PTM_RECLAIM / DPI helpers

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
    if (follow_wnd) SetAnchorFollow(follow_wnd);
}

void WindowManager::HideGroup() {
    for (auto& w : m_windows) w->Hide();
    // 事件跟随即时卸载（锚子类化只在 hover 可见且未 pin 期间存在；
    // HoverTick 的 reconcile 每 tick 也会收敛，M6 §4.2.3）
    DetachAnchorTracker();
}

void WindowManager::LayoutGroup(const RECT& anchor) {
    // M6 §4.2.2 组布局：
    // 1) 组长位置 = PNM PositionWindow 方位公式（锚贴工作区哪条边就往
    //    反方向弹；悬浮主窗不贴边 → 优先上方、放不下 → 下方；水平对锚
    //    居中；margin=max(4, 4×DPI)；工作区取 MonitorFromRect(anchor)）
    // 2) 组内排布：按 windows[] 顺序横排，间距 8×DPI；总宽超出工作区 →
    //    换行（行间距 8×DPI，第二行从组长 x 起）；行高不同的窗按行对齐
    // 3) 有记忆位置（x/y != INT_MIN）且当前为常驻/pin 态的窗：用记忆
    //    位置，不参与排布
    // 4) 逐窗 ShowAt()
    if (m_windows.empty()) return;
    auto& plugin = CProcessTopPlugin::Instance();
    bool resident = (plugin.m_settings.popup_mode == 1) || plugin.GroupPinned();

    int n = Count();
    std::vector<SIZE> sz(n);
    std::vector<bool> use_mem(n, false);
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
        int x = x0, y = y0, row_h = 0, max_w = 0;
        for (int i = 0; i < n; i++) {
            if (use_mem[i]) continue;
            if (x > x0 && x + sz[i].cx > work.right - margin) {   // 换行
                x = x0;
                y += row_h + gap;
                row_h = 0;
            }
            max_w = std::max(max_w, (int)(x + sz[i].cx - x0));
            x += (int)sz[i].cx + gap;
            row_h = std::max(row_h, (int)sz[i].cy);
        }
        total = { max_w, (row_h > 0 || y > y0) ? (y + row_h - y0) : 0 };
    };
    SIZE total{};
    flow_bounds(work.left + margin, work.top + margin, total);
    int pw = total.cx, ph = total.cy;

    // 组长位置：PNM PositionWindow 方位公式（tooltip_popup.cpp:1644-1690）
    int aw = anchor.right - anchor.left;
    int ah = anchor.bottom - anchor.top;
    bool at_bottom = anchor.bottom >= work.bottom - 2;
    bool at_top    = anchor.top <= work.top + 2;
    bool at_left   = anchor.left <= work.left + 2;
    bool at_right  = anchor.right >= work.right - 2;

    int x, y;
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
// 锚窗子类化跟随五件套（M6 §4.2.3，抄 PNM tooltip_popup.cpp:60-260）
// ============================================================

WNDPROC WindowManager::s_orphan_anchor_orig = nullptr;
HWND WindowManager::s_orphan_anchor_hwnd = nullptr;

LRESULT CALLBACK WindowManager::AnchorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WindowManager* self = &Instance();
    WNDPROC orig = nullptr;
    bool tracked = (self->m_anchor_wnd == hwnd && self->m_anchor_orig_proc);
    if (tracked) {
        orig = self->m_anchor_orig_proc;
    } else if (s_orphan_anchor_hwnd == hwnd && s_orphan_anchor_orig) {
        orig = s_orphan_anchor_orig;   // 已 detach 的残留链：纯透传
    }
    if (!orig) return DefWindowProcW(hwnd, msg, wp, lp);

    if (tracked) {
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
            // 锚窗销毁：链随窗口一起消失，清状态即可（无需还原）
            self->m_anchor_wnd = nullptr;
            self->m_anchor_orig_proc = nullptr;
            self->m_have_last_seen_anchor = false;
            if (s_orphan_anchor_hwnd == hwnd) {
                s_orphan_anchor_hwnd = nullptr;
                s_orphan_anchor_orig = nullptr;
            }
            break;
        }
    }
    return CallWindowProcW(orig, hwnd, msg, wp, lp);
}

void WindowManager::SetAnchorFollow(HWND anchor) {
    if (anchor == m_anchor_wnd) return;   // 幂等：HoverTick 每 tick 调
    DetachAnchorTracker();
    if (!anchor || !IsWindow(anchor)) return;
    // 仅同线程窗口（弹窗与 TM 主窗同住主线程；跨线程改 WNDPROC 不安全）。
    // 注意 GetWindowThreadProcessId 的返回值才是线程 ID，out 参数是进程
    // ID——PNM 曾把两者读反导致锚窗跟踪永远装不上，照抄注释防重蹈
    DWORD pid_anchor = 0;
    DWORD tid_anchor = GetWindowThreadProcessId(anchor, &pid_anchor);
    if (tid_anchor != GetCurrentThreadId()) return;
    m_anchor_orig_proc = (WNDPROC)SetWindowLongPtrW(anchor, GWLP_WNDPROC,
                                                    (LONG_PTR)AnchorProc);
    if (!m_anchor_orig_proc) return;
    m_anchor_wnd = anchor;
    m_anchor_in_sizemove = false;
    m_have_last_seen_anchor = false;   // 首个位置事件只建基准、不跟随
    if (s_orphan_anchor_hwnd == anchor) {
        // 同窗重挂：旧残留链失去透传职责（新链自己链对 orig），作废
        s_orphan_anchor_hwnd = nullptr;
        s_orphan_anchor_orig = nullptr;
    }
}

void WindowManager::DetachAnchorTracker() {
    if (!m_anchor_wnd) return;
    HWND hwnd = m_anchor_wnd;
    WNDPROC orig = m_anchor_orig_proc;
    m_anchor_wnd = nullptr;
    m_anchor_orig_proc = nullptr;
    m_have_last_seen_anchor = false;
    m_anchor_in_sizemove = false;
    if (!IsWindow(hwnd) || !orig) return;
    if ((WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == AnchorProc) {
        // 我们在链头：安全还原
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)orig);
    } else {
        // 有后来者叠在上方：留透传链（见 window_manager.h 注释）
        s_orphan_anchor_hwnd = hwnd;
        s_orphan_anchor_orig = orig;
    }
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
