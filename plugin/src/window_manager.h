#pragma once
// ============================================================
// window_manager — 排行悬浮窗实例生命周期 + windows[] 配置持久化
// (技术方案 §6.3/§6.4，M3-1)
// 单例（与插件单例同生命周期）。全部方法只在 TM 主线程调用，仅两处例外：
//   - BoundMask()：tick 线程原子读（GPU 按需启停判定）
//   - ReclaimDead()：tick 线程调用，但收尸动作会转交 UI 线程（见实现）
// ============================================================
#include "top_list_window.h"
#include <memory>
#include <vector>
#include <functional>
#include <atomic>

class WindowManager {
public:
    static WindowManager& Instance();

    // ---- 生命周期 ----
    // 启动时从 cfgs 恢复全部窗口（空则不建窗）；返回实际建窗数。
    // 某窗 Create 失败（资源枯竭）跳过该行不中断。
    int RestoreAll(const std::vector<WindowCfg>& cfgs);
    // 新建一窗（菜单「新建悬浮窗」/item 点击建默认窗）。cfg.x==INT_MIN
    // 时取工作区级联位置；返回 index，失败 -1。
    int AddWindow(const WindowCfg& cfg);
    // 销毁第 idx 窗并从列表移除（配置随行删除），返回剩余窗数
    int DestroyAt(int idx);
    // 全部销毁（窗口随进程走，DLL 卸载期不调用；此方法给测试用）
    void DestroyAll();

    // ---- 显示状态 ----
    bool AnyVisible() const;
    void ShowAll();    // item 点击：显示全部（恢复隐藏窗 + 立即灌快照）
    void HideAll();    // item 点击：隐藏全部（配置保留）
    void ToggleAll();  // = AnyVisible ? HideAll : (空列表则建默认窗) ShowAll

    // ---- 数据/通知广播（UI 线程：RefreshTick 投递后窗口拉取）----
    void RefreshAll(const MetricsSnapshot& snap, bool snap_valid,
                    int default_topn);   // 每窗按各自绑定维度灌数据
    void PostToAll(UINT msg, WPARAM wp, LPARAM lp);   // WM_PTM_REFRESH / LANG_CHANGED

    // ---- 配置 ----
    void ForEach(std::function<void(CTopListWindow&, int idx)> fn);  // UI 线程遍历
    CTopListWindow* At(int idx);
    int Count() const { return (int)m_windows.size(); }
    std::vector<WindowCfg> CollectConfigs() const;   // 持久化时读（含隐藏窗）
    // 指标绑定位集（bit per MetricKind）：遍历可见窗口求并集——
    // HideAll 后 GPU 停采，ShowAll 恢复（方案 §3.3 语义）
    void RecomputeBoundMask();                       // 增删窗/SetKind/显隐后调
    int  BoundMask() const { return m_bound_mask.load(std::memory_order_relaxed); }

    // ---- 缺陷①修复（M5/A1）：窗口拖动结束回调 → 立即落盘 ----
    void OnWindowMoveEnded(int idx);

    // ---- 死窗回收（tick 线程调；× 钮销毁 HWND 后由这里清对象）----
    void ReclaimDead();

private:
    WindowManager() = default;
    std::vector<std::unique_ptr<CTopListWindow>> m_windows;
    std::atomic<int> m_bound_mask{0};
    int m_cascade = 0;   // 级联偏移计数（新窗位置 = 工作区 10% 处 + 28*n）
    DWORD m_ui_tid = 0;  // 首窗创建线程（= UI 线程），收尸分派判定用
};
