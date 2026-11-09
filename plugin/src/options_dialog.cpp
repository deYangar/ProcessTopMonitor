// options_dialog.cpp — 插件选项对话框实现（M4，方案 §9 全量配置项）
// 骨架纪律逐条照搬 PNM：InitCommonControlsEx(trackbar)、RegisterClassW、
// WS_EX_DLGMODALFRAME 模态窗、父窗工作区 clamp 居中、模态消息循环、
// lfMessageFont 全控件统一、打开时外观快照供取消还原。
#include "options_dialog.h"
#include "plugin_main.h"
#include "window_manager.h"
#include "utils.h"
#include "i18n.h"
#include <commctrl.h>
#include <shellapi.h>   // ShellExecuteW：崩溃记录目录查看入口
#include <algorithm>

#pragma comment(lib, "comctl32.lib")

// ---- 控件 ID ----
enum {
    IDC_REFRESH_CBO = 1000,   // 刷新间隔
    IDC_TOPN_CBO,             // 默认行数（新窗默认）
    IDC_LANG_CBO,             // 语言
    IDC_MEM_PRIVATE = 1010,   // 内存口径：专用提交
    IDC_MEM_WS,               // 内存口径：工作集
    IDC_VRAM_DED = 1012,      // 显存口径：专用
    IDC_VRAM_SHARED,          // 显存口径：专用+共享
    IDC_IOVIEW_CBO = 1014,    // IO 默认视图（新窗默认）
    IDC_ENG_CHK0 = 1020,      // 引擎 checkbox 起点（连续 16 个）
    IDC_PRESET_ALL = 1040,    // 预设：全部引擎
    IDC_PRESET_D3D,           // 预设：3D
    IDC_PRESET_COMPUTE,       // 预设：计算
    IDC_IO_STATUS = 1050,     // IO 档位状态行
    IDC_IO_RETRY,             // IO 重试按钮
    IDC_CRASHDUMP_CHK = 1054, // 崩溃自动保存转储开关（2026-11-07 需求）
    IDC_OPENDEBUG_BTN,        // 打开日志目录（调试/崩溃记录共用）
    IDC_EFFECT_CBO = 1060,    // 效果
    IDC_OPACITY_TRK,          // 不透明度滑条
    IDC_OPACITY_VAL,          // 不透明度数值
    IDC_OPACITY_LBL,          // 不透明度标签（不透明档随滑条置灰）
    IDC_COLOR_CBO,            // 颜色模式
    IDC_SENSOR_CHK,           // 传感器摘要
    IDC_DEBUG_CHK,            // 调试日志
    IDC_POPUPMODE_CBO = 1070, // 出现方式（M6 §4.5）
    IDC_DEFMETRIC_CBO,        // 默认指标
    IDC_MULTIWINDOW_CHK,      // 多窗口并排
    IDC_CPUVIEW_CBO,          // CPU 视图形态（合并方案 D1：仅异构机器创建）
};

// ---- 对话框会话状态（单开，模态期内独占）----
namespace {
struct PtDlgState {
    HWND dlg = nullptr;
    HFONT font = nullptr;
    HFONT font_title = nullptr;   // 组框标题：微软雅黑 13px（2026-11-07 拍板）
    float dpi = 1.0f;
    int eng_rows = 0;                       // 引擎 checkbox 行数（动态）
    std::vector<std::wstring> engtypes;     // 打开时快照（确定时以此为准）
    std::vector<I18n::LangInfo> langs;      // 语言列表副本（CB_SETITEMDATA 存其元素指针，模态期内稳定）
    // 外观三项打开时快照：取消还原用（方案 §6 定案）
    int snap_color_mode = 0, snap_effect = 0, snap_opacity = 100;
    int preview_color = -1;   // 预览实例当前颜色模式（变化时置双底脏）
    bool changed = false;
} g_st;

int Sc(int v) { return (int)(v * g_st.dpi); }

// 低系统保存/预览校正（PNM OptionsClampEffectForOS 同款）：亚克力/模糊
// 在 <Win10 1703 上 SWCA 磨砂不渲染，统一落透明档——与 ApplyBackdrop
// 生效降级同规则，配置与观感永远一致
int PtClampEffectForOS(int eff) {
    if ((eff == 0 || eff == 2) && !PTM_IsAccentBlurSupported()) return 1;
    return eff;
}

// 不透明档无浓度概念：滑条行整体置灰（PNM OptionsUpdateOpacityEnabled
// 同款——拖了无效的控件不如禁用，状态可见）
void PtUpdateOpacityEnabled(HWND hwnd, int eff) {
    BOOL enable = (eff != 3);
    EnableWindow(GetDlgItem(hwnd, IDC_OPACITY_TRK), enable);
    EnableWindow(GetDlgItem(hwnd, IDC_OPACITY_VAL), enable);
    EnableWindow(GetDlgItem(hwnd, IDC_OPACITY_LBL), enable);
}

// ---- 预览实例（无可见悬浮窗时的外观反馈，PNM COpacityPreview 的 PTM
// 形态）：SWCA/ULW 是窗口级效果，预览必须自立门户；但呈现代码不二
// 份——直接用 CTopListWindow 本类画固定示例数据（LoadDemoSnapshot）。
// 已有可见悬浮窗时真窗口预览更直观，走 PtApplyAppearanceToAll ----
CTopListWindow* g_preview = nullptr;

void PtPreviewHide() {
    delete g_preview;   // 析构含 blur 层成对销毁（shutdown 短路纪律同款）
    g_preview = nullptr;
}

void PtPreviewEnsure(HWND hDlg, int color_mode, int effect, int opacity) {
    CProcessTopPlugin& plugin = CProcessTopPlugin::Instance();
    if (!g_preview) {
        g_preview = new CTopListWindow();
        if (!g_preview->Create(nullptr)) {
            delete g_preview;
            g_preview = nullptr;
            return;
        }
        g_preview->LoadDemoSnapshot(plugin.m_settings.default_metric);
    }
    // 颜色模式变化 → 面板/文字色变 = 双底内容变化，置脏（浓度/效果档
    // 由 ApplyBackdrop 自己置）
    if (g_st.preview_color != color_mode) {
        g_st.preview_color = color_mode;
        g_preview->MarkContentDirty();
    }
    g_preview->SetAppearance(color_mode, effect, opacity);
    if (!g_preview->IsVisible()) {
        // 定位（PNM 预览同款）：对话框右侧、与滑条行同水平线；右侧放不
        // 下退左侧，越界兜底交给 ShowAt 工作区 clamp。仅本次显示会话首
        // 帧定位——2s 自灭后重建重新定位，会话内（连续滑动/被拖走）不
        // 重定位，用户拖放的位置保持到本会话结束
        RECT rd, rt;
        GetWindowRect(hDlg, &rd);
        int line_cy = rd.top + (rd.bottom - rd.top) / 2;
        HWND hTrack = GetDlgItem(hDlg, IDC_OPACITY_TRK);
        if (hTrack && GetWindowRect(hTrack, &rt))
            line_cy = (rt.top + rt.bottom) / 2;
        int pw = (int)(300 * g_st.dpi);   // 粗算宽（IO 双列略宽，clamp 兜住）
        int ph = (int)(316 * g_st.dpi);   // demo 8 行固定高的 96-DPI 基准
        int gap = Sc(12);
        int px = rd.right + gap;
        int py = line_cy - ph / 2;
        HMONITOR mon = MonitorFromWindow(hDlg, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(mon, &mi)) {
            const RECT& wa = mi.rcWork;
            if (px + pw > wa.right) px = rd.left - gap - pw;
            if (py + ph > wa.bottom) py = wa.bottom - ph;
            if (py < wa.top) py = wa.top;
            if (px < wa.left) px = wa.left;   // 两侧都放不下：贴左，宽交给 ShowAt clamp
        }
        g_preview->ShowAt(px, py);
    }
    // 每次滑动续 2s 自动收起（PNM TIMER_AUTOHIDE 同款语义；收起动作由
    // 预览窗自己的 WM_TIMER 执行，拖动期间 KillTimer 也在窗口侧）
    SetTimer(g_preview->GetHwnd(), CTopListWindow::PREVIEW_AUTOHIDE_TIMER,
             2000, NULL);
}

void PtApplyAppearanceToAll(int color_mode, int effect, int opacity) {
    WindowManager::Instance().ForEach([color_mode, effect, opacity](CTopListWindow& w, int) {
        w.SetAppearance(color_mode, effect, opacity);
    });
}

// 组框：WS_GROUPBOX 按钮。标题挂 13px 雅黑标题字体（此前组框漏设字体，
// 板块名称显示为老式 System 字体，与其他控件不协调）
HWND PtGroup(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    HWND gb = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                            x, y, w, h, parent, nullptr, nullptr, nullptr);
    if (gb && g_st.font_title) SendMessageW(gb, WM_SETFONT, (WPARAM)g_st.font_title, TRUE);
    return gb;
}
HWND PtLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                         x, y, w, h, parent, nullptr, nullptr, nullptr);
}
HWND PtCombo(HWND parent, int id, int x, int y, int w) {
    return CreateWindowW(L"COMBOBOX", L"",
                         WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                         x, y, w, Sc(24) + 4, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
}
void PtFont(HWND ctrl) {
    if (ctrl && g_st.font) SendMessageW(ctrl, WM_SETFONT, (WPARAM)g_st.font, TRUE);
}
int PtCboSel(HWND dlg, int id) { return (int)SendMessageW(GetDlgItem(dlg, id), CB_GETCURSEL, 0, 0); }
void PtCboAdd(HWND dlg, int id, const wchar_t* text, int data) {
    HWND c = GetDlgItem(dlg, id);
    int at = (int)SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)text);
    SendMessageW(c, CB_SETITEMDATA, at, (LPARAM)data);
}

// ---- IO 状态行文案（方案 §5）----
void PtFillIoStatus(HWND dlg) {
    CProcessTopPlugin& plugin = CProcessTopPlugin::Instance();
    wchar_t text[256];
    plugin.GetIoStatusForDialog(text, 256);
    SetWindowTextW(GetDlgItem(dlg, IDC_IO_STATUS), text);
}

// ---- GPU 引擎区：可用性 + 勾选还原（方案 §4）----
void PtFillEngines(HWND dlg, bool enable) {
    CProcessTopPlugin& plugin = CProcessTopPlugin::Instance();
    GpuScopeConfig scope = plugin.GpuScope();
    for (int i = 0; i < g_st.eng_rows; i++) {
        HWND chk = GetDlgItem(dlg, IDC_ENG_CHK0 + i);
        if (!chk) continue;
        EnableWindow(chk, enable);
        bool on = false;
        for (auto& e : scope.engines)
            if (e == g_st.engtypes[i]) { on = true; break; }
        SendMessageW(chk, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    HWND buttons[] = { GetDlgItem(dlg, IDC_PRESET_ALL), GetDlgItem(dlg, IDC_PRESET_D3D),
                       GetDlgItem(dlg, IDC_PRESET_COMPUTE) };
    for (HWND b : buttons) if (b) EnableWindow(b, enable);
}

void PtPresetEngines(HWND dlg, int preset) {
    // 0=全部（回 all 态：清空多选）1=D3D（engtype 含 3D 的项）2=计算（Compute/Cuda）
    for (int i = 0; i < g_st.eng_rows; i++) {
        HWND chk = GetDlgItem(dlg, IDC_ENG_CHK0 + i);
        if (!chk) continue;
        bool on;
        if (preset == 0) on = false;
        else {
            const std::wstring& e = g_st.engtypes[i];
            if (preset == 1)
                on = (e.find(L"3D") != std::wstring::npos);
            else
                on = (e.find(L"Compute") != std::wstring::npos ||
                      e.find(L"Cuda") != std::wstring::npos);
        }
        SendMessageW(chk, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

// ---- 布局与控件创建 ----
void PtCreateControls(HWND hwnd) {
    CProcessTopPlugin& plugin = CProcessTopPlugin::Instance();
    PtSettings& st = plugin.m_settings;

    int M = Sc(16);            // 边距
    int GW = Sc(660) - 2 * M;  // 组宽
    int y = Sc(12);

    // ---- 通用 ----
    PtGroup(hwnd, TR(L"通用"), M, y, GW, Sc(92));
    {
        int iy = y + Sc(24);
        PtFont(PtLabel(hwnd, TR(L"刷新间隔"), M + Sc(12), iy + Sc(4), Sc(60), Sc(20)));
        HWND c1 = PtCombo(hwnd, IDC_REFRESH_CBO, M + Sc(80), iy, Sc(110));
        const int rms[] = { 250, 500, 1000, 2000 };
        const wchar_t* rmt[] = { L"0.25 秒", L"0.5 秒", L"1 秒", L"2 秒" };
        for (int i = 0; i < 4; i++) {
            PtCboAdd(hwnd, IDC_REFRESH_CBO, TR(rmt[i]), rms[i]);
            if (st.refresh_ms == rms[i])
                SendMessageW(c1, CB_SETCURSEL, i, 0);
        }
        PtFont(c1);
        PtFont(PtLabel(hwnd, TR(L"默认行数"), M + Sc(210), iy + Sc(4), Sc(60), Sc(20)));
        HWND c2 = PtCombo(hwnd, IDC_TOPN_CBO, M + Sc(280), iy, Sc(80));
        const int tns[] = { 5, 10, 15 };
        for (int i = 0; i < 3; i++) {
            wchar_t t[8];
            swprintf_s(t, 8, L"%d", tns[i]);
            PtCboAdd(hwnd, IDC_TOPN_CBO, t, tns[i]);
            if (st.topn == tns[i]) SendMessageW(c2, CB_SETCURSEL, i, 0);
        }
        PtFont(c2);
        PtFont(PtLabel(hwnd, TR(L"语言"), M + Sc(390), iy + Sc(4), Sc(40), Sc(20)));
        HWND c3 = PtCombo(hwnd, IDC_LANG_CBO, M + Sc(440), iy, Sc(160));
        int auto_at = (int)SendMessageW(c3, CB_ADDSTRING, 0, (LPARAM)TR(L"跟随 TrafficMonitor"));
        SendMessageW(c3, CB_SETITEMDATA, auto_at, 0);
        int cur = 0;
        g_st.langs = I18n::GetLangList();
        for (auto& li : g_st.langs) {
            int at = (int)SendMessageW(c3, CB_ADDSTRING, 0, (LPARAM)li.display_name.c_str());
            SendMessageW(c3, CB_SETITEMDATA, at, (LPARAM) & li);
        }
        // 选中：手动模式精确匹配 BCP-47，否则 auto
        if (st.lang != L"auto") {
            for (size_t i = 0; i < g_st.langs.size(); i++)
                if (g_st.langs[i].bcp47 == st.lang) cur = 1 + (int)i;
        }
        SendMessageW(c3, CB_SETCURSEL, cur, 0);
        PtFont(c3);
    }
    y += Sc(92) + Sc(12);

    // ---- 指标口径 ----
    PtGroup(hwnd, TR(L"指标口径"), M, y, GW, Sc(104));
    {
        int iy = y + Sc(24);
        HWND r1 = CreateWindowW(L"BUTTON", TR(L"内存口径：专用提交"),
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
            M + Sc(12), iy, Sc(220), Sc(20), hwnd, (HMENU)(INT_PTR)IDC_MEM_PRIVATE, nullptr, nullptr);
        HWND r2 = CreateWindowW(L"BUTTON", TR(L"工作集"),
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            M + Sc(240), iy, Sc(110), Sc(20), hwnd, (HMENU)(INT_PTR)IDC_MEM_WS, nullptr, nullptr);
        SendMessageW(r1, BM_SETCHECK, st.memory_basis == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(r2, BM_SETCHECK, st.memory_basis == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
        PtFont(r1); PtFont(r2);
        int iy2 = iy + Sc(30);
        HWND r3 = CreateWindowW(L"BUTTON", TR(L"显存口径：专用"),
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
            M + Sc(12), iy2, Sc(220), Sc(20), hwnd, (HMENU)(INT_PTR)IDC_VRAM_DED, nullptr, nullptr);
        HWND r4 = CreateWindowW(L"BUTTON", TR(L"专用+共享"),
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            M + Sc(240), iy2, Sc(110), Sc(20), hwnd, (HMENU)(INT_PTR)IDC_VRAM_SHARED, nullptr, nullptr);
        SendMessageW(r3, BM_SETCHECK, st.vram_basis == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(r4, BM_SETCHECK, st.vram_basis == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
        PtFont(r3); PtFont(r4);
        PtFont(PtLabel(hwnd, TR(L"IO 默认视图"), M + Sc(390), iy2 + Sc(4), Sc(80), Sc(20)));
        HWND c4 = PtCombo(hwnd, IDC_IOVIEW_CBO, M + Sc(470), iy2, Sc(130));
        const wchar_t* iov[] = { L"读+写", L"仅读", L"仅写" };
        for (int i = 0; i < 3; i++) {
            PtCboAdd(hwnd, IDC_IOVIEW_CBO, TR(iov[i]), i);
            if (st.io_view == i) SendMessageW(c4, CB_SETCURSEL, i, 0);
        }
        PtFont(c4);
    }
    y += Sc(104) + Sc(12);

    // ---- GPU（引擎列表动态高度）----
    GpuGrade ggrade = plugin.GpuCollectorGrade();
    // 引擎列表来源（方案 §4）：接缝内部 = 运行中取快照，否则一次性枚举
    if (ggrade != GpuGrade::G3) g_st.engtypes = plugin.EnumerateGpuEnginesForDialog();
    if (g_st.engtypes.size() > 16) g_st.engtypes.resize(16);
    g_st.eng_rows = (int)g_st.engtypes.size();
    int eng_area = std::max(1, g_st.eng_rows) * Sc(20) + (ggrade == GpuGrade::G3 ? Sc(20) : 0);
    int gpu_h = Sc(24) + eng_area + Sc(30);
    PtGroup(hwnd, TR(L"GPU"), M, y, GW, gpu_h);
    {
        int iy = y + Sc(24);
        bool gpu_ok = (ggrade != GpuGrade::G3);
        if (!gpu_ok) {
            // G3：整组灰显 + 原因一行（快照同源文本）
            wchar_t reason[160];
            swprintf_s(reason, 160, TR(L"GPU 排行不可用：%s"), plugin.GpuGradeReason());
            HWND rl = PtLabel(hwnd, reason, M + Sc(12), iy, GW - Sc(24), Sc(20));
            PtFont(rl);
        } else {
            for (int i = 0; i < g_st.eng_rows; i++) {
                HWND chk = CreateWindowW(L"BUTTON", g_st.engtypes[i].c_str(),
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    M + Sc(12), iy, Sc(300), Sc(20), hwnd,
                    (HMENU)(INT_PTR)(IDC_ENG_CHK0 + i), nullptr, nullptr);
                PtFont(chk);
                iy += Sc(20);
            }
            if (ggrade == GpuGrade::G2) {
                // G2 无引擎命名：列表灰显 + 说明（口径为全部节点求和）
                HWND gl = PtLabel(hwnd, TR(L"本系统无引擎类型细分（口径：全部节点求和）"),
                                  M + Sc(12), iy, GW - Sc(24), Sc(20));
                PtFont(gl);
                iy += Sc(20);
            }
            // 预设按钮（右上）
            HWND p1 = CreateWindowW(L"BUTTON", TR(L"全部"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                M + GW - Sc(300), y + Sc(24), Sc(84), Sc(24), hwnd,
                (HMENU)(INT_PTR)IDC_PRESET_ALL, nullptr, nullptr);
            HWND p2 = CreateWindowW(L"BUTTON", TR(L"3D"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                M + GW - Sc(210), y + Sc(24), Sc(84), Sc(24), hwnd,
                (HMENU)(INT_PTR)IDC_PRESET_D3D, nullptr, nullptr);
            HWND p3 = CreateWindowW(L"BUTTON", TR(L"计算"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                M + GW - Sc(120), y + Sc(24), Sc(84), Sc(24), hwnd,
                (HMENU)(INT_PTR)IDC_PRESET_COMPUTE, nullptr, nullptr);
            PtFont(p1); PtFont(p2); PtFont(p3);
            // 口径说明（2026-10-26 用户反馈修正：求和会叠成 100%，改为
            // 每进程取最忙引擎，与任务管理器一致；不再提供截断开关）
            HWND cap = CreateWindowW(L"STATIC", TR(L"口径：每个进程取最忙引擎（与任务管理器一致）"),
                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                M + Sc(12), y + gpu_h - Sc(28), Sc(380), Sc(20), hwnd,
                nullptr, nullptr, nullptr);
            PtFont(cap);
            PtFillEngines(hwnd, ggrade != GpuGrade::G2);
        }
    }
    y += gpu_h + Sc(12);

    // ---- 磁盘 IO 采集 ----
    // 2026-11-07 反馈：状态行原样暴露 A/B/C 档位与 ETW 会话名，用户
    // 看不懂也看不出「这是什么设置」。改法：文案口语化（见
    // GetIoStatusForDialog）+ 板块内补一行口径说明，讲清「统计什么、
    // 什么时候降级、重试干什么」
    PtGroup(hwnd, TR(L"磁盘 IO 采集"), M, y, GW, Sc(72));
    {
        int iy = y + Sc(22);
        HWND stc = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                 M + Sc(12), iy, GW - Sc(140), Sc(20), hwnd,
                                 (HMENU)(INT_PTR)IDC_IO_STATUS, nullptr, nullptr);
        PtFont(stc);
        HWND rb = CreateWindowW(L"BUTTON", TR(L"重试"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            M + GW - Sc(110), iy - Sc(4), Sc(84), Sc(24), hwnd,
            (HMENU)(INT_PTR)IDC_IO_RETRY, nullptr, nullptr);
        PtFont(rb);
        HWND cap = CreateWindowW(L"STATIC",
            TR(L"说明：按文件读写精确统计（不含网络）；系统不允许时自动降级为全口径（含网络），点「重试」恢复"),
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            M + Sc(12), iy + Sc(26), GW - Sc(24), Sc(20), hwnd,
            nullptr, nullptr, nullptr);
        PtFont(cap);
        PtFillIoStatus(hwnd);
    }
    y += Sc(72) + Sc(12);

    // ---- 日志与诊断（2026-11-08 合并：调试日志开关并入，一个目录入口管全部）----
    PtGroup(hwnd, TR(L"日志与诊断"), M, y, GW, Sc(100));
    {
        int iy = y + Sc(24);
        HWND chk = CreateWindowW(L"BUTTON",
            TR(L"崩溃时自动保存转储（用于定位崩溃原因，推荐开启）"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            M + Sc(12), iy, Sc(420), Sc(20), hwnd,
            (HMENU)(INT_PTR)IDC_CRASHDUMP_CHK, nullptr, nullptr);
        SendMessageW(chk, BM_SETCHECK, st.crash_dumps ? BST_CHECKED : BST_UNCHECKED, 0);
        PtFont(chk);
        HWND dchk = CreateWindowW(L"BUTTON", TR(L"调试日志"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            M + Sc(12), iy + Sc(24), Sc(420), Sc(20), hwnd,
            (HMENU)(INT_PTR)IDC_DEBUG_CHK, nullptr, nullptr);
        SendMessageW(dchk, BM_SETCHECK, st.debug_logs ? BST_CHECKED : BST_UNCHECKED, 0);
        PtFont(dchk);
        wchar_t dbg_dir[MAX_PATH] = L"";
        if (PTM_GetDebugDir(dbg_dir, MAX_PATH) && dbg_dir[0]) {
            HWND pl = CreateWindowW(L"STATIC", dbg_dir,
                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                M + Sc(110), iy + Sc(52), GW - Sc(230), Sc(20), hwnd,
                nullptr, nullptr, nullptr);
            PtFont(pl);
        }
        HWND plbl = CreateWindowW(L"STATIC", TR(L"记录保存在："),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            M + Sc(12), iy + Sc(52), Sc(96), Sc(20), hwnd,
            nullptr, nullptr, nullptr);
        PtFont(plbl);
        HWND ob = CreateWindowW(L"BUTTON", TR(L"打开目录"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            M + GW - Sc(100), iy + Sc(48), Sc(84), Sc(24), hwnd,
            (HMENU)(INT_PTR)IDC_OPENDEBUG_BTN, nullptr, nullptr);
        PtFont(ob);
    }
    y += Sc(100) + Sc(12);

    // ---- 显示（M6 §4.5：出现方式/默认指标/多窗口并排三行）----
    PtGroup(hwnd, TR(L"显示"), M, y, GW, Sc(88));
    {
        int iy = y + Sc(26);
        PtFont(PtLabel(hwnd, TR(L"出现方式"), M + Sc(12), iy + Sc(4), Sc(60), Sc(20)));
        HWND c8 = PtCombo(hwnd, IDC_POPUPMODE_CBO, M + Sc(80), iy, Sc(110));
        const wchar_t* pms[] = { L"悬停显示", L"常驻显示" };
        for (int i = 0; i < 2; i++) {
            PtCboAdd(hwnd, IDC_POPUPMODE_CBO, TR(pms[i]), i);
            if (st.popup_mode == i) SendMessageW(c8, CB_SETCURSEL, i, 0);
        }
        PtFont(c8);
        PtFont(PtLabel(hwnd, TR(L"默认指标"), M + Sc(220), iy + Sc(4), Sc(60), Sc(20)));
        HWND c9 = PtCombo(hwnd, IDC_DEFMETRIC_CBO, M + Sc(290), iy, Sc(110));
        const int dmk[] = { 0, 1, 2, 5, 6 };   // MetricKind 原值（itemdata）
        const wchar_t* dmt[] = { L"CPU", L"内存", L"磁盘 IO", L"GPU", L"显存" };
        // 核型分列并入 CPU 视图形态自适应（2026-11-09 合并方案）：
        // 「核型」项退役，形态走下方 CPU 视图 combo
        for (int i = 0; i < 5; i++) {
            PtCboAdd(hwnd, IDC_DEFMETRIC_CBO, TR(dmt[i]), dmk[i]);
            if (st.default_metric == dmk[i]) SendMessageW(c9, CB_SETCURSEL, i, 0);
        }
        PtFont(c9);
        // CPU 视图形态（合并方案 D1）：核型分列（默认）/仅总量，仅异构
        // 机器创建——同构机形态恒纯总量且无数据，选项整行缺席（零变化
        // 原则）；保存路径按控件存在性判定，缺席不覆写
        if (plugin.CoreClass().heterogeneous) {
            PtFont(PtLabel(hwnd, TR(L"CPU 视图"), M + Sc(430), iy + Sc(4),
                           Sc(60), Sc(20)));
            HWND cv = PtCombo(hwnd, IDC_CPUVIEW_CBO, M + Sc(500), iy, Sc(110));
            const int cvv[] = { 1, 0 };   // itemdata = PtSettings.cpu_view 值
            const wchar_t* cvt[] = { L"核型分列", L"仅总量" };
            for (int i = 0; i < 2; i++) {
                PtCboAdd(hwnd, IDC_CPUVIEW_CBO, TR(cvt[i]), cvv[i]);
                if (st.cpu_view == cvv[i]) SendMessageW(cv, CB_SETCURSEL, i, 0);
            }
            PtFont(cv);
        }
        HWND mw = CreateWindowW(L"BUTTON",
            TR(L"多窗口并排（每窗固定指标，增删走右键菜单）"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            M + Sc(12), iy + Sc(34), Sc(420), Sc(20), hwnd,
            (HMENU)(INT_PTR)IDC_MULTIWINDOW_CHK, nullptr, nullptr);
        SendMessageW(mw, BM_SETCHECK, st.multi_window ? BST_CHECKED : BST_UNCHECKED, 0);
        PtFont(mw);
    }
    y += Sc(88) + Sc(12);

    // ---- 窗口与外观 ----
    PtGroup(hwnd, TR(L"窗口与外观"), M, y, GW, Sc(88));
    {
        int iy = y + Sc(24);
        PtFont(PtLabel(hwnd, TR(L"效果"), M + Sc(12), iy + Sc(4), Sc(40), Sc(20)));
        HWND c5 = PtCombo(hwnd, IDC_EFFECT_CBO, M + Sc(60), iy, Sc(110));
        // 低系统（<Win10 1703，SWCA 磨砂不渲染）给亚克力/模糊两项加需求
        // 标注——仍可选择，预览与保存处统一降级透明档（PNM 同款）
        bool blur_ok = PTM_IsAccentBlurSupported();
        const wchar_t* effs[] = {
            blur_ok ? L"亚克力" : L"亚克力（需 Win10 1703+）",
            L"透明",
            blur_ok ? L"模糊" : L"模糊（需 Win10 1703+）",
            L"不透明",
        };
        for (int i = 0; i < 4; i++) {
            PtCboAdd(hwnd, IDC_EFFECT_CBO, TR(effs[i]), i);
            if (st.popup_effect == i) SendMessageW(c5, CB_SETCURSEL, i, 0);
        }
        PtFont(c5);
        HWND olbl = PtLabel(hwnd, TR(L"不透明度"), M + Sc(190), iy + Sc(4), Sc(56), Sc(20));
        SetWindowLongPtrW(olbl, GWL_ID, IDC_OPACITY_LBL);   // 不透明档随滑条置灰
        HWND trk = CreateWindowW(TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
            M + Sc(250), iy - Sc(2), Sc(160), Sc(26), hwnd,
            (HMENU)(INT_PTR)IDC_OPACITY_TRK, nullptr, nullptr);
        SendMessageW(trk, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
        SendMessageW(trk, TBM_SETPOS, TRUE, st.popup_opacity);
        PtFont(trk);
        wchar_t ov[8];
        swprintf_s(ov, 8, L"%d%%", st.popup_opacity);
        HWND ovl = CreateWindowW(L"STATIC", ov, WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                 M + Sc(416), iy + Sc(4), Sc(44), Sc(20), hwnd,
                                 (HMENU)(INT_PTR)IDC_OPACITY_VAL, nullptr, nullptr);
        PtFont(ovl);
        PtFont(PtLabel(hwnd, TR(L"颜色模式"), M + Sc(470), iy + Sc(4), Sc(56), Sc(20)));
        HWND c6 = PtCombo(hwnd, IDC_COLOR_CBO, M + GW - Sc(100), iy, Sc(100));
        const wchar_t* cols[] = { L"自动", L"深色", L"浅色" };
        for (int i = 0; i < 3; i++) {
            PtCboAdd(hwnd, IDC_COLOR_CBO, TR(cols[i]), i);
            if (st.color_mode == i) SendMessageW(c6, CB_SETCURSEL, i, 0);
        }
        PtFont(c6);
        int iy2 = iy + Sc(34);
        // 停靠边已随 dock_manager 移除（M6 §4.4）；传感器摘要挪头部条
        HWND s1 = CreateWindowW(L"BUTTON", TR(L"标题栏显示温度/频率（来自 TrafficMonitor）"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            M + Sc(190), iy2, Sc(340), Sc(20), hwnd,
            (HMENU)(INT_PTR)IDC_SENSOR_CHK, nullptr, nullptr);
        SendMessageW(s1, BM_SETCHECK, st.sensor_summary ? BST_CHECKED : BST_UNCHECKED, 0);
        PtFont(s1);
    }
    y += Sc(88) + Sc(16);

    // 初始滑条行状态：不透明档无浓度概念，整行置灰（PNM 同款）
    PtUpdateOpacityEnabled(hwnd, st.popup_effect);

    // ---- 确定 / 取消 ----
    HWND bok = CreateWindowW(L"BUTTON", TR(L"确定"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        M + GW - Sc(190), y, Sc(84), Sc(26), hwnd, (HMENU)IDOK, nullptr, nullptr);
    HWND bcan = CreateWindowW(L"BUTTON", TR(L"取消"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        M + GW - Sc(96), y, Sc(84), Sc(26), hwnd, (HMENU)IDCANCEL, nullptr, nullptr);
    PtFont(bok); PtFont(bcan);
}

// ---- 外观即时预览（效果/不透明度/颜色：控件态 → 全部窗口，方案 §3）----
void PtPreviewAppearance(HWND hwnd) {
    int effect = PtCboSel(hwnd, IDC_EFFECT_CBO);
    if (effect >= 0) {
        int clamped = PtClampEffectForOS(effect);
        if (clamped != effect)   // 低系统降级：下拉同步跳到透明档，让降级可见（PNM 同款）
            SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_CBO), CB_SETCURSEL, clamped, 0);
        effect = clamped;
    }
    int color = PtCboSel(hwnd, IDC_COLOR_CBO);
    int opacity = (int)SendMessageW(GetDlgItem(hwnd, IDC_OPACITY_TRK), TBM_GETPOS, 0, 0);
    if (effect < 0) effect = g_st.snap_effect;
    if (color < 0) color = g_st.snap_color_mode;
    if (opacity < 10) opacity = g_st.snap_opacity;
    // 不透明档：滑条行置灰 + 预览收起（无浓度可预览，PNM 同款）
    PtUpdateOpacityEnabled(hwnd, effect);
    if (effect == 3) {
        if (g_preview) PtPreviewHide();
        PtApplyAppearanceToAll(color, effect, opacity);   // 真窗口照常热切
        return;
    }
    if (WindowManager::Instance().GroupVisible()) {
        PtApplyAppearanceToAll(color, effect, opacity);
        if (g_preview) PtPreviewHide();   // 真窗口接管反馈，预览实例退场
    } else {
        PtPreviewEnsure(hwnd, color, effect, opacity);   // hover 隐藏期：预览反馈
    }
}

// ---- 确定：写回 + 生效管线（方案 §6）----
void PtOnOk(HWND hwnd) {
    CProcessTopPlugin& plugin = CProcessTopPlugin::Instance();
    PtSettings& st = plugin.m_settings;
    int old_refresh = st.refresh_ms;
    std::wstring old_lang = st.lang;
    GpuScopeConfig old_scope = plugin.GpuScope();

    int rms = PtCboSel(hwnd, IDC_REFRESH_CBO);
    HWND rc = GetDlgItem(hwnd, IDC_REFRESH_CBO);
    if (rms >= 0) st.refresh_ms = (int)SendMessageW(rc, CB_GETITEMDATA, rms, 0);
    int tns = PtCboSel(hwnd, IDC_TOPN_CBO);
    HWND tc = GetDlgItem(hwnd, IDC_TOPN_CBO);
    if (tns >= 0) st.topn = (int)SendMessageW(tc, CB_GETITEMDATA, tns, 0);
    int li = PtCboSel(hwnd, IDC_LANG_CBO);
    if (li >= 0) {
        if (li == 0) st.lang = L"auto";
        else {
            LPARAM pd = SendMessageW(GetDlgItem(hwnd, IDC_LANG_CBO), CB_GETITEMDATA, li, 0);
            if (pd) st.lang = ((I18n::LangInfo*)pd)->bcp47;
        }
    }
    st.memory_basis = (SendMessageW(GetDlgItem(hwnd, IDC_MEM_WS), BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    st.vram_basis = (SendMessageW(GetDlgItem(hwnd, IDC_VRAM_SHARED), BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    int iov = PtCboSel(hwnd, IDC_IOVIEW_CBO);
    if (iov >= 0) st.io_view = iov;
    // M6 显示三行：出现方式 / 默认指标 / 多窗口并排
    int pms = PtCboSel(hwnd, IDC_POPUPMODE_CBO);
    if (pms >= 0) st.popup_mode = pms;
    int dmi = PtCboSel(hwnd, IDC_DEFMETRIC_CBO);
    if (dmi >= 0) {
        LPARAM dm = SendMessageW(GetDlgItem(hwnd, IDC_DEFMETRIC_CBO),
                                 CB_GETITEMDATA, dmi, 0);
        if (dm) st.default_metric = (int)dm;
    }
    // CPU 视图形态（合并方案 D1）：同构机不创建控件（GetDlgItem=NULL 时
    // CB_GETCURSEL 也返 0，必须先判存在再读——缺席不覆写存量值）
    if (IsWindow(GetDlgItem(hwnd, IDC_CPUVIEW_CBO))) {
        int cvi = PtCboSel(hwnd, IDC_CPUVIEW_CBO);
        if (cvi >= 0) {
            LPARAM cvv = SendMessageW(GetDlgItem(hwnd, IDC_CPUVIEW_CBO),
                                      CB_GETITEMDATA, cvi, 0);
            st.cpu_view = (int)cvv;   // itemdata 即 0/1，无选中返 CB_ERR=-1 不进
        }
    }
    st.multi_window = SendMessageW(GetDlgItem(hwnd, IDC_MULTIWINDOW_CHK),
                                   BM_GETCHECK, 0, 0) == BST_CHECKED;
    st.sensor_summary = SendMessageW(GetDlgItem(hwnd, IDC_SENSOR_CHK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    st.debug_logs = SendMessageW(GetDlgItem(hwnd, IDC_DEBUG_CHK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    // 崩溃转储开关：读回 + 即时生效（旗标/WER 注册当场翻转，不等重启）
    st.crash_dumps = SendMessageW(GetDlgItem(hwnd, IDC_CRASHDUMP_CHK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    plugin.SetCrashDumps(st.crash_dumps);
    int eff = PtCboSel(hwnd, IDC_EFFECT_CBO);
    if (eff >= 0) st.popup_effect = PtClampEffectForOS(eff);   // 低系统存档降级透明档
    int col = PtCboSel(hwnd, IDC_COLOR_CBO);
    if (col >= 0) st.color_mode = col;
    int opa = (int)SendMessageW(GetDlgItem(hwnd, IDC_OPACITY_TRK), TBM_GETPOS, 0, 0);
    if (opa >= 10) st.popup_opacity = opa;
    st.Clamp();

    // GPU scope：勾选集 → gpu_engines（空集回 all，方案 §4）
    GpuScopeConfig scope = st.GpuScope();
    scope.vram_with_shared = (st.vram_basis == 1);
    {
        std::vector<std::wstring> sel;
        for (int i = 0; i < g_st.eng_rows; i++) {
            if (SendMessageW(GetDlgItem(hwnd, IDC_ENG_CHK0 + i), BM_GETCHECK, 0, 0) == BST_CHECKED)
                sel.push_back(g_st.engtypes[i]);
        }
        if (sel.empty()) {
            scope.all_engines = true;
            scope.engines.clear();
        } else {
            scope.all_engines = false;
            scope.engines = sel;
        }
    }

    // 生效管线：口径 → 窗口形态（M6）→ 定时器 → 语言 → 立即落盘
    plugin.ApplySettingsToCollectors();
    plugin.ApplyWindowMode();   // popup_mode/multi_window/default_metric 生效
    if (!(scope == old_scope)) plugin.ApplyGpuScope(scope);
    if (st.refresh_ms != old_refresh) plugin.RestartRefreshTimer();
    EtwFileCollector::SetDebugLogs(st.debug_logs);
    if (st.lang != old_lang) plugin.ApplyLanguageNow();
    plugin.SaveWindowConfigsNow();   // 设置面改动低频：立即落盘不等节流
                                       //（ApplyWindowMode 内已置 dirty）
    g_st.changed = true;
    DestroyWindow(hwnd);
}

// ---- 取消：外观三项还原打开时快照（方案 §6 定案）----
void PtOnCancel(HWND hwnd) {
    PtApplyAppearanceToAll(g_st.snap_color_mode, g_st.snap_effect, g_st.snap_opacity);
    DestroyWindow(hwnd);
}

LRESULT CALLBACK PtOptionsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        PtCreateControls(hwnd);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == IDOK) { PtOnOk(hwnd); return 0; }
        if (id == IDCANCEL) { PtOnCancel(hwnd); return 0; }
        if (id == IDC_IO_RETRY) {
            CProcessTopPlugin::Instance().RetryFileIoSession();
            PtFillIoStatus(hwnd);
            return 0;
        }
        if (id == IDC_OPENDEBUG_BTN) {
            // 日志查看入口：资源管理器打开 debug 目录（etw_file.log /
            // crash.log / crash.dmp / werdumps 都在这里），选中方便用户定位
            wchar_t dbg_dir[MAX_PATH] = L"";
            if (PTM_GetDebugDir(dbg_dir, MAX_PATH) && dbg_dir[0])
                ShellExecuteW(hwnd, L"open", dbg_dir, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (id == IDC_PRESET_ALL || id == IDC_PRESET_D3D || id == IDC_PRESET_COMPUTE) {
            PtPresetEngines(hwnd, id - IDC_PRESET_ALL);
            return 0;
        }
        // 效果/颜色 combo：即时预览
        if ((id == IDC_EFFECT_CBO || id == IDC_COLOR_CBO) && code == CBN_SELCHANGE) {
            PtPreviewAppearance(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_HSCROLL: {
        if ((HWND)lp == GetDlgItem(hwnd, IDC_OPACITY_TRK)) {
            int v = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
            wchar_t ov[8];
            swprintf_s(ov, 8, L"%d%%", v);
            SetWindowTextW(GetDlgItem(hwnd, IDC_OPACITY_VAL), ov);
            PtPreviewAppearance(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { PtOnOk(hwnd); return 0; }
        if (wp == VK_ESCAPE) { PtOnCancel(hwnd); return 0; }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_CLOSE:
        PtOnCancel(hwnd);
        return 0;
    case WM_DESTROY:
        PtPreviewHide();   // 预览实例随对话框销毁（OK/取消/ESC/关闭全路径）
        if (g_st.font) { DeleteObject(g_st.font); g_st.font = nullptr; }
        if (g_st.font_title) { DeleteObject(g_st.font_title); g_st.font_title = nullptr; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

ITMPlugin::OptionReturn PtShowOptionsDialog(HWND hParent) {
    static bool registered = false;
    if (!registered) {
        // trackbar 属 comctl32 通用控件，首用前显式注册（幂等）
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = PtOptionsWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ProcessTopMonitorOptionsDlg";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        registered = true;
    }

    CProcessTopPlugin& plugin = CProcessTopPlugin::Instance();
    g_st = PtDlgState{};
    g_st.snap_color_mode = plugin.m_settings.color_mode;
    g_st.snap_effect = plugin.m_settings.popup_effect;
    g_st.snap_opacity = plugin.m_settings.popup_opacity;

    // DPI：屏幕 DC 口径（96 基准）；布局常量全部经 Sc() 缩放
    HDC sdc = GetDC(nullptr);
    g_st.dpi = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, sdc);
    if (g_st.dpi < 1.0f || g_st.dpi > 3.0f) g_st.dpi = 1.0f;

    // 字体：lfMessageFont 全控件统一（WM_CREATE 前建好）
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_st.font = CreateFontIndirectW(&ncm.lfMessageFont);
    // 组框标题字体：微软雅黑 13px，随 DPI 缩放（96 基准，与其他控件 9pt
    // 正文拉开层级；找不到雅黑时系统自动 fallback 近似字体）
    g_st.font_title = CreateFontW(-(int)(13 * g_st.dpi + 0.5f), 0, 0, 0, FW_NORMAL,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                  L"Microsoft YaHei UI");

    int dlg_w = Sc(660), dlg_h = Sc(700);
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"ProcessTopMonitorOptionsDlg",
        TR(L"插件设置"),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, dlg_w, dlg_h,
        hParent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        if (g_st.font) DeleteObject(g_st.font);
        if (g_st.font_title) DeleteObject(g_st.font_title);
        return ITMPlugin::OR_OPTION_NOT_PROVIDED;
    }

    // 高度自适应：引擎行数动态 → 按控件实际底缘重设（±Sc 余量）
    {
        HWND last = GetDlgItem(hwnd, IDOK);
        if (last) {
            RECT rc;
            GetWindowRect(last, &rc);
            MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&rc, 2);
            int need = rc.bottom + Sc(14);
            if (need != dlg_h) {
                RECT adj = { 0, 0, dlg_w, need };
                DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
                DWORD ex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                AdjustWindowRectEx(&adj, style, FALSE, ex);
                SetWindowPos(hwnd, nullptr, 0, 0, adj.right - adj.left,
                             adj.bottom - adj.top, SWP_NOMOVE | SWP_NOZORDER);
            }
        }
    }

    // 父窗工作区 clamp 居中（PNM 同款：父窗在屏幕角落也不出屏）
    if (hParent) {
        RECT rcParent, rcDlg;
        GetWindowRect(hParent, &rcParent);
        GetWindowRect(hwnd, &rcDlg);
        int w = rcDlg.right - rcDlg.left, h = rcDlg.bottom - rcDlg.top;
        int x = rcParent.left + ((rcParent.right - rcParent.left) - w) / 2;
        int y = rcParent.top + ((rcParent.bottom - rcParent.top) - h) / 2;
        HMONITOR hMon = MonitorFromWindow(hParent, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) {
            const RECT& wa = mi.rcWork;
            if (x > wa.right - w) x = wa.right - w;
            if (y > wa.bottom - h) y = wa.bottom - h;
            if (x < wa.left) x = wa.left;
            if (y < wa.top) y = wa.top;
        }
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // 模态循环（PNM 同款）
    HWND hPar = hParent;
    if (hPar) EnableWindow(hPar, FALSE);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsWindow(hwnd)) break;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (hPar) {
        EnableWindow(hPar, TRUE);
        SetForegroundWindow(hPar);
    }
    return g_st.changed ? ITMPlugin::OR_OPTION_CHANGED : ITMPlugin::OR_OPTION_UNCHANGED;
}
