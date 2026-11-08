#pragma once
// ============================================================
// top_list_window — per-metric Top-N ranking popup window
// (技术方案 §3.2/§6.1: CTooltipPopup 视觉管线改造)
//
// Inherited from PNM's CTooltipPopup (proven ULW pipeline):
//   - ULW per-pixel channel: coverage double-pass (white/black) -> premultiplied
//     ARGB compose -> UpdateLayeredWindow. Acrylic/transparent/blur/opaque.
//   - SWCA blur-behind effect layer underneath (system real-time blur).
//   - Text flip judgment (affinity no-flash capture, hysteresis band).
//   - Frame-split icon extraction (draw path is cache-only, <=2 disk
//     extractions per paint).
// Changed for PTM:
//   - Multi-instance: HWND -> CTopListWindow* registry (PNM was a singleton).
//   - Rows: one metric, N ranked rows (no upload/download sections).
//   - Close button removed (M6): window teardown goes through the context
//     menu / programmatic Destroy(); Destroy() tears down the blur layer
//     first so no acrylic ghost survives the content window.
// ============================================================
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "top_aggregator.h"
#include "settings.h"

// Set during DLL_PROCESS_DETACH: window teardown skips DestroyWindow
// (issue #7 class of host crashes, PNM-inherited discipline)
extern volatile bool g_shutting_down;

// Destroy a window with IME detached and SEH-guarded (C2712-safe standalone)
void PtSafeDestroyWindow(HWND hwnd);

class CTopListWindow {
public:
    static const int MAX_WINDOWS = 6;   // 方案 §6.3 上限

    CTopListWindow();
    ~CTopListWindow();

    // Register the window class once per DLL (idempotent). Call before Create.
    static bool EnsureClassRegistered(HINSTANCE hInst);

    // UI thread. suggested: screen pos hint (falls back to cascading default)
    bool Create(const POINT* suggested);
    void Destroy();                    // public UI-thread teardown

    // Backdrop effect (0 acrylic / 1 transparent / 2 blur / 3 opaque) +
    // opacity 10-100. Idempotent; call before/while showing.
    void ApplyBackdrop(int effect, int opacity);

    // Push a fresh snapshot (already copied under the plugin data lock by
    // the caller) and refresh the display (dirty-check gated repaint).
    void UpdateFromSnapshot(const MetricsSnapshot& snap, MetricKind kind,
                            int topn, int io_view, bool first_tick);

    // 切换指标：只换读取的快照子集，零额外采集（方案 §6.2）
    void SetKind(MetricKind k);
    MetricKind GetKind() const { return m_kind; }

    // 组显示入口（M6 §4.1.5，WindowManager 组布局逐窗调用）：
    // ShowAt = 指定位置上屏（blur 先内容后成对 SetWindowPos，含首显
    // 干净截屏反色判定）；Hide = 双窗 SW_HIDE 成对 + 状态复位
    void ShowAt(int x, int y);
    void Hide();

    bool IsVisible() const { return m_hwnd && IsWindowVisible(m_hwnd) != FALSE; }
    // 首显判定（内部 ULW 首显干净截屏路径用；隐藏≠未首显）
    bool WasShownOnce() const { return m_visible; }
    // hover 状态机信号（TrackMouseEvent leave 追踪，M6 §4.3.2 over_group）
    bool IsHovering() const { return m_hovering; }
    HWND GetHwnd() const { return m_hwnd; }
    void GetWindowPos(POINT& pt) const;   // settings persistence

    // 每窗持久化配置（UI 线程读写；创建时由 WindowManager 注入，
    // 菜单命令就地修改，CollectConfigs 收走落盘——配置本体就在这）
    WindowCfg m_cfg;
    void ApplyCfg(const WindowCfg& c);    // kind/io_view/topn 变更入口
    int  Index() const { return m_index; }   // manager 槽位（菜单命令用）

    // settings pass-through for draw-time reads (effect/opacity/color mode)
    void SetAppearance(int color_mode, int effect, int opacity) {
        m_color_mode = color_mode;
        ApplyBackdrop(effect, opacity);
    }

    int GetIoView() const { return m_io_view; }   // plugin syncs it into settings

    // 预览实例入口（选项对话框外观反馈，PNM COpacityPreview 的 PTM
    // 形态）：灌固定示例数据并置预览模式（交互全短路），随后 ShowAt
    // 上屏。不注册进 WindowManager——不占槽位/不进组布局/收不到刷新
    // 广播/不被收尸，生命周期归创建方（options_dialog）管
    void LoadDemoSnapshot(int kind);
    // 预览双底缓存置脏（内容相关输入变化时由驱动方调；仅预览实例读）
    void MarkContentDirty() { m_cov_dirty = true; }
    // 预览 2s 自动收起 timer id（WM_TIMER wp；options_dialog 续期用）
    static constexpr UINT_PTR PREVIEW_AUTOHIDE_TIMER = 3;

private:
    // 快照 → 本窗显示数据的全套灌装（kind/io_view/topn 取本窗当前状态，
    // 问题②治本）：UpdateFromSnapshot 每拍走这里；SetKind/io_view/行数
    // 切换的同拍重灌也走这里——切换帧不再出现「旧指标数值 × 新格式模板」
    void ApplySnapshotForCurrentView(const MetricsSnapshot& snap);

    // ---- ULW pipeline (PNM-inherited) ----
    bool EnsureLayeredSurface(int w, int h);
    void ComposeLayeredSurface(int w, int h);
    void EnsureBlurLayer();
    void DestroyBlurLayer();
    void RefreshBackdrop(const RECT& popup_rect);
    bool CaptureSnapNoFlash(const RECT& popup_rect, int w, int h);
    void ClearLayeredSurfaceForCapture();

    // ---- layout & rendering ----
    void CalcLayout(HDC hdc, int& out_w, int& out_h);
    void DrawContent(HDC dc, int w, int h, bool content_only);
    void DrawRow(HDC hdc, int y, int idx, const TopRow& row);
    void DrawHeader(HDC hdc, int w, int y);   // M6 头部条：‹指标名› + 传感器摘要
    void DrawSummary(HDC hdc, int w, int y);
    // 指标循环切换（滚轮/头部点击共用；单窗模式，M6 §4.1.2/§4.1.3）
    void CycleKind(int step);
    enum StatusTone {
        ToneIoGrade,      // 黄：IO C 档警示
        ToneEventsLost,   // 黄：ETW 丢事件
        ToneGpuG2,        // 灰：G2 档无引擎命名说明
        ToneGpuHidden,    // 黄：GPU 维度不可用原因
    };
    void DrawStatusLine(HDC hdc, int w, int y, StatusTone tone);
    // 当前实际显示的状态行清单（布局/绘制/菜单判定共用，保持一致）
    void ActiveStatusLines(StatusTone out[4], int& count) const;
    // 值区宽（问题③）：磁盘 IO 合计视图双列（读/写文字前缀）= 2×单列 +
    // 8×DPI 列间距；其余视图单列。DrawRow 名字列缩减与 CalcLayout 窗宽联动
    // 都以此为单一事实源
    int ValueAreaWidth() const;
    void Reposition();                       // clamp to work area, show
    void OnPaint();

    // ---- helpers ----
    bool IsDarkMode();
    bool UseTextTheme() const { return m_text_adaptive ? m_text_dark : m_dark_mode; }
    COLORREF GetBgColor();
    COLORREF GetTextColor();
    // 指标 accent 色板单变体提取（翻转判定要算两变体中点用）
    COLORREF AccentVariant(bool light);
    COLORREF GetAccentColor();
    void UpdateDpiScale();
    void RecreateScaledResources();
    HICON LookupIcon(const std::wstring& name, DWORD pid);
    HICON ExtractIconFor(const std::wstring& name, DWORD pid);   // not "ExtractIcon": shellapi macro clash
    bool PumpIconExtraction(int count);
    void ResolvePendingPaths();
    const wchar_t* MetricTitle() const;
    void BuildContextMenu(POINT pt);
    void RecalcAndRepaint(bool reposition);

    // ---- message handling ----
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static CTopListWindow* FromHwnd(HWND hwnd);
    void UnregisterDispatch();   // UI 线程：HWND 销毁后立即摘除派发表项

    friend class WindowManager;   // m_index 槽位注入
    int m_index = -1;

    // ---- state ----
    HWND m_hwnd = nullptr;
    HINSTANCE m_hinst = nullptr;
    bool m_visible = false;
    bool m_dark_mode = true;
    bool m_dark_mode_cached = true;
    ULONGLONG m_dark_mode_tick = 0;

    // current data
    MetricKind m_kind = MetricKind::Cpu;
    std::vector<TopRow> m_rows;
    bool m_rows_has_value = false;
    double m_summary_value = 0;       // per-metric system total
    double m_cpu_total = -1;
    uint64_t m_mem_total = 0, m_mem_used = 0;
    double m_io_read_total = 0, m_io_write_total = 0;
    FileIoGrade m_io_grade = FileIoGrade::C;
    uint64_t m_io_events_lost = 0;
    // GPU / VRAM
    double m_gpu_total = 0;
    bool m_gpu_total_valid = false;
    uint64_t m_vram_sys_ded = 0, m_vram_sys_shared = 0;
    bool m_vram_sys_valid = false;
    GpuGrade m_gpu_grade = GpuGrade::G3;
    std::wstring m_gpu_reason;
    std::vector<std::wstring> m_gpu_engtypes;   // 动态引擎列表（口径菜单）
    int m_tm_gpu_usage = -1;                    // 宿主整机 GPU（G3 摘要兜底）
    // 宿主传感器（M3-3 L0）：0 = TM 未采集 → 摘要行传感器段整体隐藏
    int m_tm_cpu_temp = 0, m_tm_gpu_temp = 0, m_tm_cpu_freq = 0;
    int m_topn = 10;
    int m_io_view = 0;
    int m_color_mode = 0;

    // 脏检查基线：序号+量化值指纹（方案 §5.3/§8.2 静止画面零重绘）
    std::vector<std::pair<DWORD, long long>> m_dirty_sig;

    // backdrop state (PNM-inherited semantics)
    int m_backdrop_mode = 0;          // 0=not-yet 1=layered
    int m_backdrop_effect = 0;
    int m_backdrop_opacity = 100;
    bool m_acrylic_active = false;
    bool m_acrylic_dark = true;
    bool m_noise_acrylic = false;
    HWND m_blur_hwnd = nullptr;
    bool m_text_dark = false;
    bool m_text_adaptive = false;
    double m_bg_lum = -1.0;
    // 数值列独立翻转 + accent 对比保障（PNM m_speed_flip/m_speed_bg_lum
    // 2026-10 顺修的 PTM 适配）：右值区独立采样中位亮度 sbg → 与本窗
    // 指标 accent 两变体中点比较独立翻转（数值列背景与左列不同，透明/
    // 低浓度档透出的桌面内容左右各异）；推离基准优先右值区 sbg，无
    // 独立判定数据时退整窗 m_bg_lum
    bool m_accent_flip = false;
    double m_accent_bg_lum = -1.0;

    // ULW surfaces
    HDC m_ulw_dc = nullptr;  HBITMAP m_ulw_bmp = nullptr;  void* m_ulw_bits = nullptr;
    HDC m_cov_a_dc = nullptr; HBITMAP m_cov_a_bmp = nullptr; void* m_cov_a_bits = nullptr;
    HDC m_cov_c_dc = nullptr; HBITMAP m_cov_c_bmp = nullptr; void* m_cov_c_bits = nullptr;
    HDC m_snap_dc = nullptr;  HBITMAP m_snap_bmp = nullptr;  void* m_snap_bits = nullptr;
    int m_ulw_w_cache = 0, m_ulw_h_cache = 0;
    RECT m_last_snap_rect = {};
    ULONGLONG m_last_refresh_tick = 0;
    bool m_affinity_ok = true;
    bool m_steady_probe_fast = false;

    // icons: exe 路径（小写归一）-> HICON or nullptr（负缓存；B1 治本——
    // 图标语义由 exe 路径决定，同 exe 的换新 pid（浏览器/Electron 子进程
    // 持续生灭）天然共享一份 HICON，不再按 pid 无界重建逼向 10000 GDI
    // 句柄上限）。pending 仍按 pid 挂（每 pid 一次提取动作）
    std::unordered_map<std::wstring, HICON> m_icon_cache;
    std::vector<DWORD> m_icon_pending;
    std::unordered_map<DWORD, std::wstring> m_path_cache;   // pid -> exe path
    // 解析失败的负缓存（C3）：键=pid——同映像名的其它 pid 不连坐，
    // 瞬时失败（进程创建/退出窗口期）只影响该 pid 本身
    std::unordered_set<DWORD> m_path_missed;

    // cached GDI objects
    HFONT m_font_normal = nullptr;
    HFONT m_font_small = nullptr;
    HPEN m_pen_separator = nullptr;
    bool m_pen_dark = true;

    // hover state for header bar (repaint on flip only)
    bool m_tracking = false;
    bool m_hovering = false;           // 光标在窗内（HoverTick over_group 信号）
    bool m_preview = false;            // 预览实例（LoadDemoSnapshot 置位）：交互全短路
    bool m_cov_dirty = true;           // 预览双底缓存脏标志：内容恒定，仅主题/翻转/效果档切换置脏
    RECT m_rc_header = {};             // 头部条（拖动柄 + 命中测试）
    RECT m_rc_metric = {};             // 指标区（点击切换；单窗模式可点）

    // layout constants (96-DPI base, DPI-scaled members)
    int PADDING = 12, ROW_HEIGHT = 30, ICON_SIZE = 20, CORNER_RADIUS = 8;
    int HEADER_H = 26, SUMMARY_H = 26, STATUS_H = 20;
    int VALUE_AREA_W = 90, IDX_W = 22;
    static constexpr int BASE_PADDING = 12, BASE_ROW_HEIGHT = 30, BASE_ICON_SIZE = 20;
    static constexpr int BASE_CORNER_RADIUS = 8;
    static constexpr int BASE_HEADER_H = 26, BASE_SUMMARY_H = 26, BASE_STATUS_H = 20;
    static constexpr int BASE_VALUE_AREA_W = 90, BASE_IDX_W = 22;
    int BASE_WIDTH = 300;
    float m_dpi_scale = 1.0f;
    bool m_dpi_inited = false;
};
