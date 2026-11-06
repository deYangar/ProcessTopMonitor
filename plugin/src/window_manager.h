#pragma once
// ============================================================
// window_manager — 排行悬浮窗实例生命周期 + windows[] 配置持久化 +
// 组显示控制（组布局 / 锚窗子类化跟随，M6 §4.2）
// 单例（与插件单例同生命周期）。全部方法只在 TM 主线程调用，仅两处例外：
//   - BoundMask()：tick 线程原子读（GPU 按需启停判定）
//   - ReclaimDead()：tick 线程调用，但收尸动作会转交 UI 线程（见实现）
// M6 职责变化：新增组显示控制 + 组布局 + 锚窗子类化；删除
// ShowAll/HideAll/ToggleAll（被 plugin_main hover 状态机取代）与
// OnWindowMoveEnded（移入窗口自身 WM_EXITSIZEMOVE）。
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
    // 新建一窗（菜单「添加窗口」/item 点击建默认窗）。返回 index，失败 -1。
    int AddWindow(const WindowCfg& cfg);
    // 销毁第 idx 窗并从列表移除（配置随行删除），返回剩余窗数
    int DestroyAt(int idx);
    // 全部销毁（窗口随进程走，DLL 卸载期不调用；此方法给测试用）
    void DestroyAll();

    // ---- 组显示控制（M6 §4.2.4，UI 线程）----
    // LayoutGroup + 可选挂锚跟随（follow_wnd=nullptr：pin/常驻不跟锚）。
    // 锚 rect：TM 主窗 rect 或任务栏光标 ±60px 锚带
    void ShowGroupAt(const RECT& anchor, HWND follow_wnd = nullptr);
    void HideGroup();                       // 逐窗 Hide() + 卸锚子类化
    bool GroupVisible() const;              // 任一窗可见
    // 组布局（M6 §4.2.2）：组长位置 = PNM PositionWindow 方位公式；
    // 组内横排 wrap；有记忆位置且常驻/pin 态的窗用记忆位置不参与排布；
    // 逐窗 ShowAt()。只在出现/锚全量重排/尺寸变化时跑，纯跟随走 delta 路径
    void LayoutGroup(const RECT& anchor);
    // 锚纯位移路径（M6 §4.2.3）：整组每窗 blur+内容同参数连续
    // SetWindowPos + 工作区 clamp（无累积漂移）
    void GroupDeltaMove(int dx, int dy);

    // ---- 锚窗子类化跟随五件套（M6 §4.2.3，抄 PNM tooltip_popup 机制）----
    void SetAnchorFollow(HWND anchor);      // 幂等；仅同线程窗口
    void DetachAnchorTracker();             // 链头在则还原；被叠留 orphan 透传链
    void OnAnchorPosChanged(const RECT& new_anchor, bool sized);
    void AnchorFullPass(const RECT& anchor_rect);   // sized/跨屏/松手全量重排
    RECT AnchorRect() const { return m_popup_anchor; }   // HoverTick 兜底比对

    // ---- 数据/通知广播（UI 线程：RefreshTick 投递后窗口拉取）----
    void RefreshAll(const MetricsSnapshot& snap, bool snap_valid,
                    int default_topn);   // 每窗按各自绑定维度灌数据
    void PostToAll(UINT msg, WPARAM wp, LPARAM lp);   // WM_PTM_REFRESH / LANG_CHANGED

    // ---- 配置 ----
    void ForEach(std::function<void(CTopListWindow&, int idx)> fn);  // UI 线程遍历
    CTopListWindow* At(int idx);
    int Count() const { return (int)m_windows.size(); }
    std::vector<WindowCfg> CollectConfigs() const;   // 持久化时读（含隐藏窗）
    // 指标绑定位集（bit per MetricKind）：M6 §4.2.5 语义修正——「存在」
    // （配置内）窗口并集，hover 隐藏 ≠ 关闭，采集保持预热
    void RecomputeBoundMask();                       // 增删窗/SetKind 后调
    int  BoundMask() const { return m_bound_mask.load(std::memory_order_relaxed); }

    // ---- hover 状态机 timer（100ms，挂组长窗，M6 §4.3.2）----
    // 组隐藏时窗口 SW_HIDE 不停 timer；组长变化（首建/销毁）后重挂
    void EnsureHoverTimer();

    // ---- PNM 秩序兜底（问题④不对称秩序，UI 线程）----
    // HoverTick 每 ≥500ms 调：检测同进程 PNMTooltip_ 窗与 PTM 组的
    // 关系——PNM 压住正位且 PTM 还在理论矩形 → 重排让位；PNM 全部消失
    // 且 PTM 已让开 → 重排归位。判定用「组长是否在理论落点」而非裸
    // 相交，避免让开后的抖动循环；pin/常驻手放位置不参与
    void PnmOrderReconcile();

    // ---- 死窗回收（tick 线程调；× 钮销毁 HWND 后由这里清对象）----
    void ReclaimDead();

private:
    WindowManager() = default;
    // 组布局纯计算（LayoutGroup 的 1-5 段：各窗尺寸 → wrap 包围盒 →
    // 方位公式 → 工作区 clamp → 可选 PNM 避让）。LayoutGroup 摆窗与
    // PnmOrderReconcile 的理论落点比对共用同一份公式，杜绝第二份布局
    // 逻辑漂移（问题④）
    void CalcGroupLayout(const RECT& anchor, bool resident,
                         std::vector<SIZE>& sz, std::vector<bool>& use_mem,
                         int& x, int& y, int& pw, int& ph, bool avoid_pnm);
    // 锚窗子类化走 comctl32 SetWindowSubclass（2026-11-06 双环崩溃根治）：
    // master_proc 中性地址挂在 GWLP_WNDPROC 链头，本方 proc 地址不被链上
    // 任何一方当 orig 持有——跨插件叠挂/摘除不可能造出自指/互指环
    static LRESULT CALLBACK AnchorProc(HWND, UINT, WPARAM, LPARAM,
                                       UINT_PTR id, DWORD_PTR ref);
    // 子类化看门狗（M6 §4.3.4，照抄 PNM tooltip_popup.cpp:572-587+:87-95）：
    // message-only 窗 + 原生注册 proc + 1s timer，发现组长窗 wndproc 被
    // 外部注入器剥掉 → 重装（防 hover timer 静默死亡导致弹窗永不消失）
    static LRESULT CALLBACK WatchdogProc(HWND, UINT, WPARAM, LPARAM);
    void EnsureWatchdog();
    HWND m_watchdog_hwnd = nullptr;
    std::vector<std::unique_ptr<CTopListWindow>> m_windows;
    std::atomic<int> m_bound_mask{0};
    int m_cascade = 0;   // 级联偏移计数（新窗位置 = 工作区 10% 处 + 28*n）
    DWORD m_ui_tid = 0;  // 首窗创建线程（= UI 线程），收尸分派判定用

    // ---- 锚跟随状态（全部 UI 线程）----
    HWND m_anchor_wnd = nullptr;          // 子类化中的锚窗
    bool m_anchor_in_sizemove = false;
    RECT m_last_seen_anchor = {};         // delta 基准
    bool m_have_last_seen_anchor = false; // 首事件只建基准不跟随
    HMONITOR m_last_seen_mon = nullptr;
    ULONGLONG m_last_full_pass_tick = 0;  // 全量重排节流（100ms）
    RECT m_popup_anchor = {};             // 锚 rect（供 HoverTick 轮询兜底比对）
};
