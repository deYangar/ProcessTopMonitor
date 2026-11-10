#pragma once
// ============================================================
// settings.h — settings.json persistence (技术方案 §6.4 schema, M1 fields)
// Same hand-rolled read/write style as PNM: fprintf out, find+atoi in.
// No third-party JSON dependency (零依赖原则).
// M6 schema v3：popup_mode / default_metric / multi_window 三新键，
// 删 dock_edge 与 WindowCfg 的 dock 字段（v1 残留 metric/win_x/win_y
// 迁移源改由加载局部变量承接，不再常驻结构）。
// ============================================================
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#include "top_aggregator.h"   // MetricKind

// 每窗持久化配置（settings.json windows[] 一行；纯数值无字符串——手写
// JSON 解析保持简单，M3-1 方案 §2.1/§5）。
// 注意：kind 存 MetricKind 原始 int 值（枚举含 DiskRead=3/DiskWrite=4
// 占位，Gpu=5、Vram=6。核型拆分曾新增 CpuP=7/CpuE=8，2026-11-09 CPU
// 视图合并后两值退役：CPU(0) 单视图形态自适应（异构+cpu_view=1 →
// 核型双列），Clamp 把配置里的 7/8 一律归 0，枚举值保留作兼容占位）。
// x/y 语义（M6 §3.1）：仅常驻/pin 态使用的「拖过的位置记忆」；
// INT_MIN=未拖过（常驻态用组布局，hover 态忽略）
// w/h 语义（2026-11-10 方案③）：自定义窗口大小记忆（物理像素，DPI 变更
// 由 WM_DPICHANGED 按建议矩形重记）；0=未自定义 → 窗口尺寸走 CalcLayout
// 行数公式。行数菜单点击清 h（显式选行数 = 回行数驱动）
struct WindowCfg {
    int kind = 0;          // MetricKind as int（0=Cpu 1=Mem 2=DiskIo 5=Gpu 6=Vram；7/8 已退役归 0）
    int io_view = 0;       // 0=combined 1=read 2=write（仅 kind==2 有意义）
    int topn = 10;         // 5/10/15
    int x = INT_MIN;       // INT_MIN = 未拖过
    int y = INT_MIN;
    int w = 0;             // 0 = 未自定义（方案③ resize 记忆，物理像素）
    int h = 0;
};

struct PtSettings {
    // ---- 不变 ----
    int refresh_ms = 500;          // 250/500/1000/2000
    int topn = 10;                 // 5/10/15（新窗默认值，M3-1 起每窗独立）
    int memory_basis = 0;          // 0=private commit 1=working set
    int io_view = 0;               // 0=combined 1=read 2=write（新窗默认值）
    int color_mode = 0;            // 0=auto 1=dark 2=light
    int popup_effect = 0;          // 0=acrylic 1=transparent 2=blur 3=opaque
    int popup_opacity = 100;       // 10-100
    std::wstring gpu_engines = L"all";   // "all" 或逗号分隔 engtype 列表
    bool gpu_cap_100 = true;             // 引擎求和 >100% 截断
    int vram_basis = 0;                  // 0=dedicated 1=dedicated+shared
    std::wstring lang = L"auto";   // auto or BCP-47
    bool debug_logs = false;
    bool crash_dumps = true;       // 崩溃自动保存转储（crash.log/crash.dmp+WER），默认开
    bool sensor_summary = true;    // 头部条传感器摘要（M6 决策 3=A，默认 true）

    // ---- v3 新增（M6 §3.1）----
    int schema = 3;                // schema 版本（写盘固定 3）
    int popup_mode = 0;            // 0=hover（默认） 1=常驻
    int default_metric = 0;        // MetricKind 原值：0=Cpu 1=Mem 2=DiskIo 5=Gpu 6=Vram
    bool multi_window = false;     // false=单窗+切换 true=多窗并排（每窗固定指标）
    // CPU 视图形态（2026-11-09 合并方案 D1/D4）：1=核型分列（异构机上
    // P/E 或 X3D/标准 双列，默认）/0=仅总量（ETW 采样通道不启，tooltip
    // P/E 明细随之消失）。仅异构机器有意义；同构机忽略（形态恒纯 CPU）
    int cpu_view = 1;
    // System(pid 4) 计入核型归因（2026-11-10 方案①）：true（默认）=
    // System 内核线程时间进 P/E 账本，群总与下方加总对得上账；false =
    // 拆分时代行为（System 时间留分母不归因）。仅管 ETW 核型通道——
    // cpu 视图总量口径（NtQSI）System 恒在 ranking，不受此键影响。
    // 缺键 = true（旧配置升级天然兼容）
    bool show_system_procs = true;
    // 常驻悬浮窗置顶开关（2026-11-10 方案二）：false = 窗口落桌面部件层
    // （壁纸之上、桌面图标之下，Rainmeter「On desktop」同款）。仅管常驻/
    // pin 组——hover 弹窗恒置顶（跟随弹出必须置顶）。缺键 = true（现状）
    bool popup_topmost = true;
    // 鼠标穿透开关（2026-11-10 方案四）：true = 悬浮窗内容窗对鼠标完全
    // 「不存在」（WS_EX_TRANSPARENT，点击/滚轮全落下层），纯显示 HUD。
    // 自锁特性：开启后悬浮窗收不到任何输入，取消穿透唯一入口 = 设置面板。
    // 仅常驻/pin 态生效。缺键 = false（现状）
    bool popup_click_through = false;
    std::vector<WindowCfg> windows;   // 单窗模式≤1；多窗 1..6；空=悬浮窗模式退出

    // clamp everything to its legal domain (hand-edited settings.json must
    // not brick the plugin)
    void Clamp() {
        if (refresh_ms != 250 && refresh_ms != 500 && refresh_ms != 1000 && refresh_ms != 2000)
            refresh_ms = 500;
        if (topn != 5 && topn != 10 && topn != 15) topn = 10;
        if (memory_basis != 0 && memory_basis != 1) memory_basis = 0;
        if (io_view < 0 || io_view > 2) io_view = 0;
        if (color_mode < 0 || color_mode > 2) color_mode = 0;
        if (popup_effect < 0 || popup_effect > 3) popup_effect = 0;
        if (popup_opacity < 10) popup_opacity = 10;
        if (popup_opacity > 100) popup_opacity = 100;
        if (vram_basis != 0 && vram_basis != 1) vram_basis = 0;
        if (gpu_engines.empty()) gpu_engines = L"all";
        if (lang.empty()) lang = L"auto";
        // v3 三新键（M6 §3.2）
        if (popup_mode != 0 && popup_mode != 1) popup_mode = 0;
        // cpu_view（合并方案 §4.1）：缺键=默认 1（旧配置升级即核型形态，
        // 正是合并本意）；非法值归 1
        if (cpu_view != 0 && cpu_view != 1) cpu_view = 1;
        // CpuP(7)/CpuE(8) 退役（2026-11-09 CPU 视图合并）：配置里存过的
        // 一律归 0——异构机老核型面板窗迁移为 CPU 窗 + 默认核型形态，
        // 视觉等价；今日的 8→7 归一随之改为 8→0
        if (default_metric == 7 || default_metric == 8) default_metric = 0;
        if (default_metric != 0 && default_metric != 1 && default_metric != 2 &&
            default_metric != 5 && default_metric != 6)
            default_metric = 0;   // 非法（含 3/4 占位）→ CPU
        // windows[]：数量钳到上限，每行字段钳域（手改 settings.json 防砖）。
        // 单窗模式窗数收敛到 ≤1（多窗切回单窗保留第一窗）；空列表合法
        // ——「关闭悬浮窗」后 hover 无弹窗直到 item 重建，不强造窗
        if (windows.size() > 6) windows.resize(6);
        if (!multi_window && windows.size() > 1) windows.resize(1);
        for (auto& c : windows) {
            if (c.kind == 7 || c.kind == 8) c.kind = 0;   // CpuP/CpuE 退役归 0
            if (c.kind != 0 && c.kind != 1 && c.kind != 2 && c.kind != 5 &&
                c.kind != 6)
                c.kind = 0;   // 3/4 是 DiskRead/DiskWrite 占位，不进配置
            if (c.io_view < 0 || c.io_view > 2) c.io_view = 0;
            if (c.topn != 5 && c.topn != 10 && c.topn != 15) c.topn = 10;
            // w/h 防砖（方案③）：荒谬值视为未自定义（0），精确 min 钳由
            // CalcLayout 运行时兜底（GETMINMAXINFO 拖拽期同款公式）
            if (c.w < 50 || c.w > 8192) c.w = 0;
            if (c.h < 50 || c.h > 8192) c.h = 0;
        }
    }

    // v1 紧凑指标编码（0=cpu 1=mem 2=io 3=gpu 4=vram）→ MetricKind
    static MetricKind KindFromV1(int metric) {
        switch (metric) {
        case 1: return MetricKind::Mem;
        case 2: return MetricKind::DiskIo;
        case 3: return MetricKind::Gpu;
        case 4: return MetricKind::Vram;
        default: return MetricKind::Cpu;
        }
    }

    // v1 迁移（M6 §3.2：v1 字段由 PtLoadSettings 读进局部传入——结构里
    // 已无 v1 残留成员）：无 windows 且有 metric/win_x → 生成 windows[0]
    // （v1 用户原是单窗模式，指标与位置原样搬入）
    static void MigrateV1Into(std::vector<WindowCfg>& windows, int metric,
                              int win_x, int win_y, int io_view, int topn) {
        if (!windows.empty()) return;
        if (metric != 0 || win_x != INT_MIN || win_y != INT_MIN) {
            WindowCfg c;
            c.kind = (int)KindFromV1(metric);   // 紧凑编码 → MetricKind 原值
            c.io_view = io_view; c.topn = topn;
            c.x = win_x; c.y = win_y;
            windows.push_back(c);
        }
    }

    // gpu_engines -> scope config（逗号分隔 engtype 列表 / "all"）
    GpuScopeConfig GpuScope() const {
        GpuScopeConfig c;
        c.cap_100 = gpu_cap_100;
        c.vram_with_shared = (vram_basis == 1);
        c.all_engines = (gpu_engines == L"all");
        if (!c.all_engines) {
            size_t pos = 0;
            while (pos <= gpu_engines.size()) {
                size_t comma = gpu_engines.find(L',', pos);
                if (comma == std::wstring::npos) comma = gpu_engines.size();
                std::wstring one = gpu_engines.substr(pos, comma - pos);
                // trim spaces
                size_t b = one.find_first_not_of(L" \t");
                size_t e = one.find_last_not_of(L" \t");
                if (b != std::wstring::npos)
                    c.engines.push_back(one.substr(b, e - b + 1));
                pos = comma + 1;
            }
            if (c.engines.empty()) c.all_engines = true;
        }
        return c;
    }
};

// B-1 治本（2026-11-10 审查）：写出侧 JSON 字符串转义——gpu_engines/lang
// 值含 " 或 \ 时产出合法 JSON（值域本不含这些字符，纯手改注入防护）；
// 读侧 gets 对应反解
inline std::string PtJsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c;
        }
    }
    return out;
}

// UTF-8 <-> wide helpers (PNM style)
inline std::string PtToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s;
    if (n > 0) {
        s.resize(n);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    }
    return s;
}
inline std::wstring PtFromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w;
    if (n > 0) {
        w.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    }
    return w;
}

inline bool PtSaveSettings(const std::wstring& config_dir, const PtSettings& st) {
    if (config_dir.empty()) return false;
    CreateDirectoryW(config_dir.c_str(), nullptr);
    wchar_t path[MAX_PATH];
    if (swprintf_s(path, MAX_PATH, L"%s\\settings.json", config_dir.c_str()) <= 0) return false;
    // 原子写（A2 治本，「活配置原子写」纪律落实到插件自身代码）：先写
    // 同卷 tmp，fclose 落盘后 MoveFileEx(REPLACE_EXISTING) 原子替换——
    // 任何时刻外部读 settings.json 都是完整旧版或完整新版，杜绝截断
    // 半写形态（2026-11-08 部署事故同族失效）；写失败时原文件保持原样
    wchar_t tmp[MAX_PATH];
    if (swprintf_s(tmp, MAX_PATH, L"%s\\settings.json.tmp", config_dir.c_str()) <= 0) return false;
    FILE* f = _wfopen(tmp, L"w");
    if (!f) return false;
    // M6 §3.2：schema 固定写 3；新增三键；windows[] 每行只写
    // {kind, io_view, topn, x, y, w, h}（w/h=自定义大小记忆，方案③）；
    // dock_edge/docked/dock_target 与 v1 的 metric/win_x/win_y 不再写出
    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": 3,\n");
    fprintf(f, "  \"refresh_ms\": %d,\n", st.refresh_ms);
    fprintf(f, "  \"topn\": %d,\n", st.topn);
    fprintf(f, "  \"memory_basis\": %d,\n", st.memory_basis);
    fprintf(f, "  \"io_view\": %d,\n", st.io_view);
    fprintf(f, "  \"color_mode\": %d,\n", st.color_mode);
    fprintf(f, "  \"popup_effect\": %d,\n", st.popup_effect);
    fprintf(f, "  \"popup_opacity\": %d,\n", st.popup_opacity);
    fprintf(f, "  \"gpu_engines\": \"%s\",\n", PtJsonEscape(PtToUtf8(st.gpu_engines)).c_str());
    fprintf(f, "  \"gpu_cap_100\": %s,\n", st.gpu_cap_100 ? "true" : "false");
    fprintf(f, "  \"vram_basis\": %d,\n", st.vram_basis);
    fprintf(f, "  \"debug_logs\": %s,\n", st.debug_logs ? "true" : "false");
    fprintf(f, "  \"crash_dumps\": %s,\n", st.crash_dumps ? "true" : "false");
    fprintf(f, "  \"lang\": \"%s\",\n", PtJsonEscape(PtToUtf8(st.lang)).c_str());
    fprintf(f, "  \"sensor_summary\": %s,\n", st.sensor_summary ? "true" : "false");
    fprintf(f, "  \"popup_mode\": %d,\n", st.popup_mode);
    fprintf(f, "  \"default_metric\": %d,\n", st.default_metric);
    fprintf(f, "  \"multi_window\": %s,\n", st.multi_window ? "true" : "false");
    fprintf(f, "  \"cpu_view\": %d,\n", st.cpu_view);
    fprintf(f, "  \"show_system_procs\": %s,\n", st.show_system_procs ? "true" : "false");
    fprintf(f, "  \"popup_topmost\": %s,\n", st.popup_topmost ? "true" : "false");
    fprintf(f, "  \"popup_click_through\": %s,\n", st.popup_click_through ? "true" : "false");
    fprintf(f, "  \"windows\": [\n");
    for (size_t i = 0; i < st.windows.size(); i++) {
        const WindowCfg& c = st.windows[i];
        fprintf(f, "    {\"kind\":%d,\"io_view\":%d,\"topn\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}%s\n",
                c.kind, c.io_view, c.topn, c.x, c.y, c.w, c.h,
                (i + 1 < st.windows.size()) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    if (fclose(f) != 0) {
        DeleteFileW(tmp);   // 写失败（磁盘满等）：原文件保持原样
        return false;
    }
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        return false;
    }
    return true;
}

inline bool PtLoadSettings(const std::wstring& config_dir, PtSettings& st) {
    if (config_dir.empty()) return false;
    wchar_t path[MAX_PATH];
    if (swprintf_s(path, MAX_PATH, L"%s\\settings.json", config_dir.c_str()) <= 0) return false;
    FILE* f = _wfopen(path, L"r");
    if (!f) return false;
    // B-1 治本（2026-11-10 审查）：读循环到文件尾（上限 64KB）——旧固定
    // 4096 字节缓冲静默截断，6 窗配置 + 超长引擎表/手改注释时 windows[]
    // 尾部行丢失（解析不崩，表现为末尾窗配置回落默认）
    std::string json;
    {
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            json.append(chunk, n);
            if (json.size() >= 64 * 1024) break;   // 上限：更大按坏文件处理
        }
    }
    fclose(f);

    auto geti = [&](const char* key, int& out) {
        size_t pos = json.find(key);
        if (pos == std::string::npos) return;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return;
        int v;
        if (sscanf(json.c_str() + pos + 1, " %d", &v) == 1) out = v;
    };
    auto getb = [&](const char* key, bool& out) {
        size_t pos = json.find(key);
        if (pos == std::string::npos) return;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return;
        size_t v = pos + 1;
        while (v < json.size() && (json[v] == ' ' || json[v] == '\t')) v++;
        out = (json.compare(v, 4, "true") == 0);
    };
    auto gets = [&](const char* key, std::wstring& out) {
        size_t pos = json.find(key);
        if (pos == std::string::npos) return;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return;
        pos = json.find('"', pos);
        if (pos == std::string::npos) return;
        // B-1：转义反解（对应 PtJsonEscape）——扫描到未转义的闭引号为止
        std::string raw;
        for (size_t e = pos + 1; e < json.size(); e++) {
            char c = json[e];
            if (c == '"' && (raw.empty() || raw.back() != '\\')) break;
            if (c == '\\' && e + 1 < json.size()) {
                char nx = json[e + 1];
                if (nx == '"' || nx == '\\') { raw += nx; e++; continue; }
                if (nx == 'n') { raw += '\n'; e++; continue; }
                if (nx == 'r') { raw += '\r'; e++; continue; }
                if (nx == 't') { raw += '\t'; e++; continue; }
            }
            raw += c;
        }
        out = PtFromUtf8(raw);
    };

    // v1 迁移源（局部承接，M6 §3.2：结构里已无 v1 残留成员）
    int v1_metric = 0, v1_win_x = INT_MIN, v1_win_y = INT_MIN;
    geti("\"refresh_ms\"", st.refresh_ms);
    geti("\"topn\"", st.topn);
    geti("\"memory_basis\"", st.memory_basis);
    geti("\"io_view\"", st.io_view);
    geti("\"metric\"", v1_metric);
    geti("\"win_x\"", v1_win_x);
    geti("\"win_y\"", v1_win_y);
    geti("\"color_mode\"", st.color_mode);
    geti("\"popup_effect\"", st.popup_effect);
    geti("\"popup_opacity\"", st.popup_opacity);
    gets("\"gpu_engines\"", st.gpu_engines);
    getb("\"gpu_cap_100\"", st.gpu_cap_100);
    geti("\"vram_basis\"", st.vram_basis);
    getb("\"debug_logs\"", st.debug_logs);
    getb("\"crash_dumps\"", st.crash_dumps);   // 无键 = 默认开（旧配置升级天然兼容）
    gets("\"lang\"", st.lang);
    getb("\"sensor_summary\"", st.sensor_summary);
    geti("\"popup_mode\"", st.popup_mode);
    geti("\"default_metric\"", st.default_metric);
    getb("\"multi_window\"", st.multi_window);
    geti("\"cpu_view\"", st.cpu_view);   // 缺键 = 默认 1（Clamp 兜底非法值）
    getb("\"show_system_procs\"", st.show_system_procs);   // 缺键 = 默认 true
    getb("\"popup_topmost\"", st.popup_topmost);           // 缺键 = 默认 true
    getb("\"popup_click_through\"", st.popup_click_through);   // 缺键 = 默认 false
    int schema_read = 0;   // 无 schema 键 = v1/v2 旧文件，走迁移
    geti("\"schema\"", schema_read);

    // windows[] 段解析：定位 "windows" → [ → 逐 {…} 行按 key 读 5 个 int
    // （v3 起无 dock 字段）。行内无嵌套对象/转义引号，find('}') 即行界。
    size_t wpos = json.find("\"windows\"");
    if (wpos != std::string::npos) {
        size_t lb = json.find('[', wpos);
        if (lb != std::string::npos) {
            size_t cur = lb;
            for (;;) {
                size_t ob = json.find('{', cur);
                if (ob == std::string::npos) break;
                size_t cb = json.find('}', ob);
                if (cb == std::string::npos) break;
                std::string row = json.substr(ob, cb - ob + 1);
                auto rowi = [&](const char* key, int def) {
                    size_t p = row.find(key);
                    if (p == std::string::npos) return def;
                    p = row.find(':', p);
                    if (p == std::string::npos) return def;
                    int v;
                    if (sscanf(row.c_str() + p + 1, " %d", &v) == 1) return v;
                    return def;
                };
                WindowCfg c;
                c.kind = rowi("\"kind\"", 0);
                c.io_view = rowi("\"io_view\"", 0);
                c.topn = rowi("\"topn\"", 10);
                c.x = rowi("\"x\"", INT_MIN);
                c.y = rowi("\"y\"", INT_MIN);
                c.w = rowi("\"w\"", 0);   // 缺键 = 0（未自定义，方案③）
                c.h = rowi("\"h\"", 0);
                st.windows.push_back(c);
                cur = cb + 1;
            }
        }
    } else {
        // 无 windows 键 = v1 配置 → 迁移（v1 → v2 原逻辑保留，产物再走
        // 下方 v2→v3 推导，M6 §3.2 链式）
        PtSettings::MigrateV1Into(st.windows, v1_metric, v1_win_x, v1_win_y,
                                  st.io_view, st.topn);
    }
    // v2 → v3 迁移（读到 schema<=2 时，M6 §3.2）：
    // 1. popup_mode=0（hover 是默认形态）、multi_window=(windows.size()>1)、
    //    default_metric=windows[0].kind（空则 0）
    // 2. windows[] 的 kind/io_view/topn/x/y 原样保留（x/y 作常驻位置记忆）
    if (schema_read <= 2) {
        st.popup_mode = 0;
        st.multi_window = st.windows.size() > 1;
        st.default_metric = st.windows.empty() ? 0 : st.windows[0].kind;
    }
    st.Clamp();
    return true;
}
