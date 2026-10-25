#pragma once
// ============================================================
// dock_manager — 停靠跟随：锚窗枚举 + WinEventHook 事件驱动 + 三段式跟随
// (技术方案 §7，M3-2)。单例；全部方法/回调都在 TM 主线程——hook 为
// OUTOFCONTEXT，回调经宿主消息泵在安装线程派发（即 UI 线程，SetWindowPos
// 安全）。原则（方案 §7.2）：全程不主动，停靠只由显式菜单动作触发。
// ============================================================
#include <windows.h>
#include <vector>

#include "top_list_window.h"

enum class DockTarget { Auto = 0, PnmDetail = 1, TmMain = 2 };
enum class DockState  { Floating, Docked, Detached };

// 找锚窗：按 target 枚举（PNM 详情窗优先 / TM 主窗）；返回 HWND 或 nullptr
HWND DockFindAnchor(DockTarget target);

class DockManager {
public:
    static DockManager& Instance();

    // ---- 启停（UI 线程）----
    // 窗口请求停靠：查找锚 → Docked + 立即布局 + 装 hook（首个 Docked 时）。
    // 返回 false = 锚不在（调用方提示「未找到可停靠窗口」）
    bool Dock(CTopListWindow* wnd, DockTarget target);
    // 解除：回 Floating（用户菜单「解除停靠」），窗口留在当前位置
    void Undock(CTopListWindow* wnd);
    // 启动恢复：RestoreAll 后按 cfg.docked==1 逐窗尝试 Dock（锚不在 →
    // 落 Floating 且 cfg.docked=0，与「锚消失不自动吸回」一致）；结束时落盘一次
    void RestoreFromConfigs();

    // ---- 状态查询 ----
    DockState StateOf(const CTopListWindow* wnd) const;
    HWND Anchor() const { return m_anchor; }

    // ---- 布局（公开给换行重排/锚尺寸变化）----
    void RelayoutAll();          // 全部 Docked 窗按方案 §4 排布

    // ---- 供 CTopListWindow 调 ----
    void OnWindowDestroyed(CTopListWindow* wnd);   // Docked 窗被 ×：摘除+重排
    void OnWindowDragged(CTopListWindow* wnd);     // Docked 窗被拖：距锚>48px → Detached

    // DLL 卸载清理（FMW 场景 hook 先于进程回收，防止回调进已卸载代码）
    void RemoveHooks();

private:
    void InstallHooks();         // 首个 Docked
    static VOID CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                      LONG id_object, LONG id_child,
                                      DWORD thread, DWORD time);
    void OnAnchorLocationChange();          // 轻路径 delta（方案 §6.2）
    void OnAnchorMoveEnd();                 // 终判全量重定位
    void OnAnchorGone();                    // IsWindow(anchor)==false
    static void CALLBACK PollTimerProc(HWND, UINT, UINT_PTR, DWORD);  // 500ms 兜底
    // 启动恢复延迟重试：锚窗（尤其 TM 主窗）可能晚于插件 InitOnce 出现
    static void CALLBACK RestoreRetryProc(HWND, UINT, UINT_PTR, DWORD);

    HWND m_anchor = nullptr;
    DWORD m_anchor_pid = 0;
    HMONITOR m_anchor_monitor = nullptr;
    DockTarget m_target = DockTarget::Auto;
    HWINEVENTHOOK m_hook_loc = nullptr;     // EVENT_OBJECT_LOCATIONCHANGE
    HWINEVENTHOOK m_hook_end = nullptr;     // EVENT_SYSTEM_MOVESIZEEND
    RECT m_anchor_last = {};
    ULONGLONG m_last_relayout = 0;          // 全量重定位节流（150ms）
    UINT_PTR m_poll_timer = 0;              // 500ms 兜底轮询
    // Docked 窗集合按 WindowManager 槽位序（布局级联用）
    std::vector<CTopListWindow*> m_docked;
    std::vector<CTopListWindow*> m_detached;
    // 启动恢复待重试队列（500ms × 60 上限，见 RestoreRetryProc）
    std::vector<CTopListWindow*> m_pending_restore;
    UINT_PTR m_restore_timer = 0;
    int m_restore_retry = 0;
};
