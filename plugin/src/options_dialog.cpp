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
    IDC_EFFECT_CBO = 1060,    // 效果
    IDC_OPACITY_TRK,          // 不透明度滑条
    IDC_OPACITY_VAL,          // 不透明度数值
    IDC_COLOR_CBO,            // 颜色模式
    IDC_SENSOR_CHK,           // 传感器摘要
    IDC_DEBUG_CHK,            // 调试日志
};

// ---- 对话框会话状态（单开，模态期内独占）----
namespace {
struct PtDlgState {
    HWND dlg = nullptr;
    HFONT font = nullptr;
    float dpi = 1.0f;
    int eng_rows = 0;                       // 引擎 checkbox 行数（动态）
    std::vector<std::wstring> engtypes;     // 打开时快照（确定时以此为准）
    std::vector<I18n::LangInfo> langs;      // 语言列表副本（CB_SETITEMDATA 存其元素指针，模态期内稳定）
    // 外观三项打开时快照：取消还原用（方案 §6 定案）
    int snap_color_mode = 0, snap_effect = 0, snap_opacity = 100;
    bool changed = false;
} g_st;

int Sc(int v) { return (int)(v * g_st.dpi); }

void PtApplyAppearanceToAll(int color_mode, int effect, int opacity) {
    WindowManager::Instance().ForEach([color_mode, effect, opacity](CTopListWindow& w, int) {
        w.SetAppearance(color_mode, effect, opacity);
    });
}

// 组框：WS_GROUPBOX 按钮
HWND PtGroup(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                         x, y, w, h, parent, nullptr, nullptr, nullptr);
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
    PtGroup(hwnd, TR(L"磁盘 IO 采集"), M, y, GW, Sc(52));
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
        PtFillIoStatus(hwnd);
    }
    y += Sc(52) + Sc(12);

    // ---- 窗口与外观 ----
    PtGroup(hwnd, TR(L"窗口与外观"), M, y, GW, Sc(118));
    {
        int iy = y + Sc(24);
        PtFont(PtLabel(hwnd, TR(L"效果"), M + Sc(12), iy + Sc(4), Sc(40), Sc(20)));
        HWND c5 = PtCombo(hwnd, IDC_EFFECT_CBO, M + Sc(60), iy, Sc(110));
        const wchar_t* effs[] = { L"亚克力", L"透明", L"模糊", L"不透明" };
        for (int i = 0; i < 4; i++) {
            PtCboAdd(hwnd, IDC_EFFECT_CBO, TR(effs[i]), i);
            if (st.popup_effect == i) SendMessageW(c5, CB_SETCURSEL, i, 0);
        }
        PtFont(c5);
        PtFont(PtLabel(hwnd, TR(L"不透明度"), M + Sc(190), iy + Sc(4), Sc(56), Sc(20)));
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
        HWND s2 = CreateWindowW(L"BUTTON", TR(L"调试日志"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            M + Sc(12), iy2 + Sc(30), Sc(120), Sc(20), hwnd,
            (HMENU)(INT_PTR)IDC_DEBUG_CHK, nullptr, nullptr);
        SendMessageW(s2, BM_SETCHECK, st.debug_logs ? BST_CHECKED : BST_UNCHECKED, 0);
        PtFont(s2);
    }
    y += Sc(118) + Sc(16);

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
    int color = PtCboSel(hwnd, IDC_COLOR_CBO);
    int opacity = (int)SendMessageW(GetDlgItem(hwnd, IDC_OPACITY_TRK), TBM_GETPOS, 0, 0);
    if (effect < 0) effect = g_st.snap_effect;
    if (color < 0) color = g_st.snap_color_mode;
    if (opacity < 10) opacity = g_st.snap_opacity;
    PtApplyAppearanceToAll(color, effect, opacity);
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
    st.sensor_summary = SendMessageW(GetDlgItem(hwnd, IDC_SENSOR_CHK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    st.debug_logs = SendMessageW(GetDlgItem(hwnd, IDC_DEBUG_CHK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    int eff = PtCboSel(hwnd, IDC_EFFECT_CBO);
    if (eff >= 0) st.popup_effect = eff;
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

    // 生效管线：口径 → 定时器 → 语言 → 立即落盘
    plugin.ApplySettingsToCollectors();
    if (!(scope == old_scope)) plugin.ApplyGpuScope(scope);
    if (st.refresh_ms != old_refresh) plugin.RestartRefreshTimer();
    EtwFileCollector::SetDebugLogs(st.debug_logs);
    if (st.lang != old_lang) plugin.ApplyLanguageNow();
    plugin.SaveWindowConfigsNow();   // 设置面改动低频：立即落盘不等节流
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
        if (g_st.font) { DeleteObject(g_st.font); g_st.font = nullptr; }
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

    int dlg_w = Sc(660), dlg_h = Sc(620);
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"ProcessTopMonitorOptionsDlg",
        TR(L"插件设置"),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, dlg_w, dlg_h,
        hParent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        if (g_st.font) DeleteObject(g_st.font);
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
