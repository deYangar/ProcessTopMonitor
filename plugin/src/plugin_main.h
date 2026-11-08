#pragma once
#include <windows.h>
#include "PluginInterface.h"
#include "metrics_collector.h"
#include "etw_file_collector.h"
#include "top_aggregator.h"
#include "top_list_window.h"
#include "window_manager.h"
#include "settings.h"
#include "i18n.h"
#include <atomic>
#include <memory>

// 任务栏 item：CPU Top1 进程名 + 占用（方案 §1.1.5，PNM CProcessNetItem 缩编）
// 自绘两段式：进程名左对齐 + 占用右对齐，点击切换排行悬浮窗显隐
class CTopItem : public IPluginItem {
public:
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;
    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;

    bool IsCustomDraw() const override { return true; }
    int GetItemWidth() const override;
    int GetItemWidthEx(void* hDC) const override;
    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override;

    // tick 线程写（m_data_lock 内），TM 主线程读 —— PNM 同款静态缓冲
    static wchar_t s_top_name[48];
    static wchar_t s_top_value[32];
    static bool s_show_item;      // master switch（M1 恒 true，留 schema 位）
    static COLORREF s_value_color;
    static bool s_has_value_color;
};

class CProcessTopPlugin : public ITMPlugin {
public:
    CProcessTopPlugin();
    static CProcessTopPlugin& Instance();

    IPluginItem* GetItem(int index) override;
    void DataRequired() override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    const wchar_t* GetTooltipInfo() override;
    OptionReturn ShowOptionsDialog(void* hParent) override;
    void OnInitialize(ITrafficMonitor* pApp) override;
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;
    void OnMonitorInfo(const MonitorInfo& monitor_info) override;
    ~CProcessTopPlugin();

    // DllMain DLL_PROCESS_DETACH: stop the timer + ETW consumer thread
    // before unload (private members; this wrapper is the public seam)
    void ShutdownForDllUnload() {
        StopRefreshTimer();
        StopKickTimer(true);
        // A4：DllMain 路径走「尽力而为停」——ExitProcess 已杀其它线程，
        // 完整 Stop 的 SetStatus/join 可能等一把被弃的锁（TM 退出挂死）
        m_etw.StopForUnload();
        m_gpu.EnsureStopped();
        // 锚窗子类化还原：TM 主窗通常活得比本 DLL 久，留着 AnchorProc
        // 会在 DLL 卸载后炸宿主（PNM 同款纪律）
        WindowManager::Instance().DetachAnchorTracker();
    }

    // ---- UI thread entry points (called from the top-list window) ----
    void TopListRefreshFromSnapshot();   // WM_PTM_REFRESH handler
    void SaveSettingsIfDirty();
    void ApplySettingsToCollectors();    // memory basis / GPU scope / debug logs
    void SaveWindowConfigsNow();         // 拖动结束等即时落盘路径（缺陷①修复）
    // watchdog TICK_SYNC 处理器调用（UI 线程）：全部窗口销毁 → 停刷新
    // 定时器。异步删除（不等待正跑的 tick 回调），与回调内自停同语义
    void StopRefreshTimerFromUi() { StopRefreshTimer(false); }
    // M4 选项对话框生效管线用的接缝（刷新率重启/语言切换/立即落盘）
    void RestartRefreshTimer();
    void ApplyLanguageNow();
    // M6 §4.5：popup_mode/multi_window/default_metric 三项的生效函数
    //（选项对话框「确定」管线集中调用，UI 线程）
    void ApplyWindowMode();

    // 方案一「开窗即拍」（2026-11-08 IO/GPU 预热提速）：在「用户刚要看」
    // 的时刻（AddWindow / 指标切换 / ShowGroupAt 弹回）用 one-shot
    // timer（due=0）立即补一拍 RefreshTick——首帧到首拍的 0~refresh_ms
    // 间隙由此消除，GPU「采样预热中」可见时长 500ms → ≤50ms（冷启动
    // 补拍在触发的这一拍内完成）。UI 线程调用；与周期 timer 的并发投递
    // 由 RefreshTick 的 m_refresh_busy CAS 防重入兜住
    void KickRefreshTick();

    // GPU 口径（窗口口径菜单读写，均 UI 线程）
    GpuScopeConfig GpuScope() const { return m_settings.GpuScope(); }
    void ApplyGpuScope(const GpuScopeConfig& cfg);

    // ---- M4 选项对话框接缝（对话框只经这些通道访问插件内部）----
    GpuGrade GpuCollectorGrade() const { return m_gpu.Grade(); }
    const wchar_t* GpuGradeReason() const { return m_gpu.Reason(); }
    void CopySnapshot(MetricsSnapshot& out, bool& valid);          // 锁内拷贝
    std::vector<std::wstring> EnumerateGpuEnginesForDialog();      // 运行中取快照，否则临时查询
    FileIoGrade RetryFileIoSession() { return m_etw.Start(); }     // 幂等重试
    void GetIoStatusForDialog(wchar_t* out, size_t cap);           // 档位文案 + 丢事件
    // 崩溃诊断开关（设置页）：旗标 + WER LocalDumps 注册/撤销即时生效
    void SetCrashDumps(bool on);
    bool CrashDumpsEnabled();

    // ---- M6 hover 状态机 + pin（UI 线程；§4.3.2 状态字段照 PNM 命名）----
    bool GroupPinned() const { return m_group_pinned; }
    // 常驻/pin 态弹窗可拖（§4.1.3：hover 态恒 HTCLIENT 跟锚不可拖）
    bool GroupDraggable() const { return m_settings.popup_mode == 1 || m_group_pinned; }
    HWND m_hover_target = nullptr;       // 当前 hover 的 TM 窗（主窗句柄）
    bool m_hover_is_taskbar = false;     // 目标是否任务栏上下文（锚带跟随）
    ULONGLONG m_hover_start_tick = 0;    // hover 计时起点（400ms 阈值）
    ULONGLONG m_hover_leave_tick = 0;    // 离开宽限起点（300ms）
    bool m_group_pinned = false;         // pin：hover/常驻间临时切换
    void HoverTick();                    // 100ms 状态机（组长窗 timer id=2 驱动）
    void PinGroup(const RECT* anchor_for_show);   // pin 翻转（item/菜单 360 共用）
    void RebuildDefaultWindow();         // windows 空时 item 点击重建默认窗
    // 常驻启动/pin 恢复的布局锚：TM 主窗 rect（枚举查找），找不到用
    // 主屏工作区兜底（M6 §4.2.4）
    RECT GetStartupAnchor();

    // settings live here; the window reads a copy on push
    PtSettings m_settings;
    std::wstring m_config_dir;           // <plugins>\ProcessTopMonitor
    bool m_settings_dirty = false;

private:
    CTopItem m_item;
    ITrafficMonitor* m_app = nullptr;

    MetricsCollector m_metrics;
    EtwFileCollector m_etw;
    GpuCollector m_gpu;
    GpuTickData m_gpu_last;              // GPU 上一次采集结果（tick 线程私有）
    TopAggregator m_aggregator;
    MetricsSnapshot m_snapshot;          // guarded by m_data_lock
    bool m_snapshot_valid = false;

    // 宿主整机 GPU 利用率（OnMonitorInfo 回调缓存；G3 摘要兜底）
    std::atomic<int> m_tm_gpu_usage{ -1 };
    // 宿主传感器缓存（M3-3 L0 白拿：宿主线程写，tick/任意线程读；0 = TM 未采集）
    std::atomic<int> m_tm_cpu_temp{ 0 };
    std::atomic<int> m_tm_gpu_temp{ 0 };
    std::atomic<int> m_tm_cpu_freq{ 0 };
    // 窗口实例集合（多窗模式，M3-1）：WindowManager 单例持有，plugin 经
    // WindowManager::Instance() 访问——窗口侧也要直达，成员+单例二选一
    // 取单例（方案 §2.1 Instance 与 §6 m_wmgr 的矛盾在此定案：单例）

    CRITICAL_SECTION m_data_lock;
    bool m_lock_inited = false;
    std::atomic<bool> m_refresh_busy{false};
    bool m_inited = false;
    HANDLE m_refresh_timer = nullptr;
    bool m_refresh_timer_ok = false;
    HANDLE m_kick_timer = nullptr;   // 方案一：one-shot 补拍 timer（复用式，
                                     // 仅 UI 线程触达；Shutdown/析构统一删除）
    int m_tick_count = 0;
    int m_gpu_tick_div = 0;              // GPU 采集分频（每 2 拍 ≈1s）
    // 迟停计时（问题①A，tick 线程私有）：0 = 绑定中/未启动；非 0 = 空闲
    // 起点打点。连续 kXxxIdleStopMs 无该维度窗口才真停（GPU 冷启动 1-2s、
    // ETW 冷启动 ~1s 且伴口径闪变——切换循环全部落在冷启动窗内）
    ULONGLONG m_gpu_idle_since = 0;
    ULONGLONG m_etw_idle_since = 0;
    // PNM 秩序兜底轮询节流（问题④，UI 线程）：0 = 从未检查
    ULONGLONG m_pnm_poll_tick = 0;
    ULONGLONG m_last_save_tick = 0;

    std::wstring m_tm_config_dir;

    void InitOnce(const wchar_t* cfg_base);
    void EnsureInitialized();
    void InitCrashHandlers();
    void RefreshTick();
    static void CALLBACK RefreshTimerProc(void* param, BOOLEAN timer_or_wait);
    void StartRefreshTimer();
    void StopRefreshTimer(bool wait = true);   // wait=false：定时器回调内自停（防自死锁）
    // A3：wait=true（默认，DllMain/析构路径）传 INVALID_HANDLE_VALUE 同步
    // 等待回调收尾——异步删除在 DLL 卸载场景下回调可能跑已解除映射的
    // 代码（kick 是 one-shot，等待上限一拍内）；wait 语义与
    // StopRefreshTimer 对齐
    void StopKickTimer(bool wait = true);
    void CheckLanguageChange();
    void BuildTooltip();                 // 双缓冲 tooltip 文本
    wchar_t m_tooltip[2][2048] = { L"", L"" };
    std::atomic<int> m_tooltip_cur{ 0 };
    wchar_t* TipWriteBuf() { return m_tooltip[1 - m_tooltip_cur.load(std::memory_order_acquire)]; }
    void PublishTooltip() { m_tooltip_cur.store(1 - m_tooltip_cur.load(std::memory_order_acquire), std::memory_order_release); }

    static CProcessTopPlugin s_instance;
};
