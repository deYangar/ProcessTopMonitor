#pragma once
#include <windows.h>
#include "PluginInterface.h"
#include "metrics_collector.h"
#include "etw_file_collector.h"
#include "top_aggregator.h"
#include "top_list_window.h"
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
    ~CProcessTopPlugin();

    // DllMain DLL_PROCESS_DETACH: stop the timer + ETW consumer thread
    // before unload (private members; this wrapper is the public seam)
    void ShutdownForDllUnload() {
        StopRefreshTimer();
        m_etw.Stop();
    }

    // ---- UI thread entry points (called from the top-list window) ----
    void TopListRefreshFromSnapshot();   // WM_PTM_REFRESH handler
    void ToggleTopListWindow(HWND parent);
    void SaveSettingsIfDirty();
    void ApplySettingsToCollectors();    // memory basis / debug logs

    // settings live here; the window reads a copy on push
    PtSettings m_settings;
    std::wstring m_config_dir;           // <plugins>\ProcessTopMonitor
    bool m_settings_dirty = false;

private:
    CTopItem m_item;
    ITrafficMonitor* m_app = nullptr;

    MetricsCollector m_metrics;
    EtwFileCollector m_etw;
    TopAggregator m_aggregator;
    MetricsSnapshot m_snapshot;          // guarded by m_data_lock
    bool m_snapshot_valid = false;

    std::unique_ptr<CTopListWindow> m_wnd;
    bool m_wnd_shown = false;            // 用户 intent（点击 item 切换）

    CRITICAL_SECTION m_data_lock;
    bool m_lock_inited = false;
    std::atomic<bool> m_refresh_busy{false};
    bool m_inited = false;
    HANDLE m_refresh_timer = nullptr;
    bool m_refresh_timer_ok = false;
    int m_tick_count = 0;
    ULONGLONG m_last_save_tick = 0;

    std::wstring m_tm_config_dir;

    void InitOnce(const wchar_t* cfg_base);
    void EnsureInitialized();
    void InitCrashHandlers();
    void RefreshTick();
    static void CALLBACK RefreshTimerProc(void* param, BOOLEAN timer_or_wait);
    void StartRefreshTimer();
    void StopRefreshTimer();
    void CheckLanguageChange();
    void CreateTopListWindow();
    void PollWindowAlive();              // 关闭钮销毁窗口后回收对象
    void BuildTooltip();                 // 双缓冲 tooltip 文本
    wchar_t m_tooltip[2][2048] = { L"", L"" };
    std::atomic<int> m_tooltip_cur{ 0 };
    wchar_t* TipWriteBuf() { return m_tooltip[1 - m_tooltip_cur.load(std::memory_order_acquire)]; }
    void PublishTooltip() { m_tooltip_cur.store(1 - m_tooltip_cur.load(std::memory_order_acquire), std::memory_order_release); }

    static CProcessTopPlugin s_instance;
};
