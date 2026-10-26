// window_manager.cpp — 多实例生命周期 + windows[] 持久化（M3-1）
#include "window_manager.h"
#include "plugin_main.h"
#include "utils.h"   // WM_PTM_RECLAIM

WindowManager& WindowManager::Instance() {
    static WindowManager s_instance;
    return s_instance;
}

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
        // 级联默认位：工作区 10% 处 + 28px 偏移递增，越界回绕（方案 §3.1）
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
    // 级联位置回填进 cfg：持久化拿到真实值，重启不漂移
    POINT got;
    wnd->GetWindowPos(got);
    if (got.x != INT_MIN) {
        wnd->m_cfg.x = got.x;
        wnd->m_cfg.y = got.y;
    }
    m_windows.push_back(std::move(wnd));
    RecomputeBoundMask();
    // 从「零窗」恢复时重启定时器（全部隐藏/关闭后定时器已停，方案 §8.3）
    if (Count() == 1) CProcessTopPlugin::Instance().RestartRefreshTimer();
    return (int)m_windows.size() - 1;
}

int WindowManager::DestroyAt(int idx) {
    if (idx < 0 || idx >= (int)m_windows.size()) return Count();
    m_windows[idx]->Destroy();
    m_windows.erase(m_windows.begin() + idx);
    for (int i = 0; i < Count(); i++) m_windows[i]->m_index = i;
    if (m_windows.empty()) m_cascade = 0;   // 全灭级联归零（方案 §7.7）
    RecomputeBoundMask();
    return Count();
}

void WindowManager::DestroyAll() {
    while (!m_windows.empty()) DestroyAt(0);
}

bool WindowManager::AnyVisible() const {
    for (auto& w : m_windows)
        if (w->IsVisible()) return true;
    return false;
}

void WindowManager::ShowAll() {
    for (auto& w : m_windows) {
        if (w->GetHwnd()) ShowWindow(w->GetHwnd(), SW_SHOW);
    }
    RecomputeBoundMask();
    // 隐藏期间定时器已停（方案 §8.3），恢复显示必须重新拉起
    CProcessTopPlugin::Instance().RestartRefreshTimer();
    // 隐藏窗恢复后立即灌一次快照（方案 §3.6/边界 5）
    CProcessTopPlugin::Instance().TopListRefreshFromSnapshot();
}

void WindowManager::HideAll() {
    for (auto& w : m_windows) {
        if (w->GetHwnd() && w->IsVisible()) ShowWindow(w->GetHwnd(), SW_HIDE);
    }
    RecomputeBoundMask();   // 隐藏窗不算绑定 → GPU 停采
}

void WindowManager::ToggleAll() {
    if (Count() == 0) {
        // windows 空 = 悬浮窗模式退出；item 点击建默认窗（方案 §3.2）
        auto& st = CProcessTopPlugin::Instance().m_settings;
        WindowCfg def;
        def.kind = 0;   // CPU
        def.topn = st.topn;
        def.io_view = st.io_view;
        AddWindow(def);
        CProcessTopPlugin::Instance().m_settings_dirty = true;
        CProcessTopPlugin::Instance().TopListRefreshFromSnapshot();
        return;
    }
    if (AnyVisible()) HideAll();
    else ShowAll();
}

void WindowManager::RefreshAll(const MetricsSnapshot& snap, bool snap_valid,
                               int default_topn) {
    (void)snap_valid;
    for (auto& w : m_windows) {
        if (!w->GetHwnd() || !IsWindow(w->GetHwnd())) continue;
        // 隐藏窗跳过：UpdateFromSnapshot 的首显路径会 SWP_SHOWWINDOW，
        // 给隐藏窗灌数据等于每拍复活它（方案边界 5）
        if (w->WasShownOnce() && !w->IsVisible()) continue;
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
    // 发给全部活窗：REFRESH 在 UpdateFromSnapshot 里对隐藏窗早退（省开销），
    // LANG_CHANGED 必须也达隐藏窗——否则重显后文案停留旧语言
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
    int mask = 0;
    for (auto& w : m_windows) {
        if (w->IsVisible()) mask |= PtKindMask(w->GetKind());
    }
    m_bound_mask.store(mask, std::memory_order_relaxed);
}

void WindowManager::OnWindowMoveEnded(int idx) {
    (void)idx;   // cfg 已由窗口在 WM_EXITSIZEMOVE 里写好，这里全量落盘
    CProcessTopPlugin::Instance().SaveWindowConfigsNow();
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
}
