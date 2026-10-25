#pragma once
// ============================================================
// settings.h — settings.json persistence (技术方案 §6.4 schema, M1 fields)
// Same hand-rolled read/write style as PNM: fprintf out, find+atoi in.
// No third-party JSON dependency (零依赖原则).
// ============================================================
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#include "top_aggregator.h"   // MetricKind

// 每窗持久化配置（settings.json windows[] 一行；纯数值无字符串——手写
// JSON 解析保持简单，M3-1 方案 §2.1/§5）。docked/dock_target 由停靠模块
// （M3-2）使用，本结构只负责透传持久化。
// 注意：kind 存 MetricKind 原始 int 值（枚举含 DiskRead=3/DiskWrite=4
// 占位，Gpu=5、Vram=6——方案注释「0..4」没算上占位，落地以枚举真值为准）
struct WindowCfg {
    int kind = 0;          // MetricKind as int（0=Cpu 1=Mem 2=DiskIo 5=Gpu 6=Vram）
    int io_view = 0;       // 0=combined 1=read 2=write（仅 kind==2 有意义）
    int topn = 10;         // 5/10/15
    int x = INT_MIN;       // INT_MIN = 默认级联位置
    int y = INT_MIN;
    int docked = 0;        // 0=浮动 1=停靠
    int dock_target = 0;   // 0=auto 1=pnm 2=tm
};

struct PtSettings {
    int refresh_ms = 500;          // 250/500/1000/2000
    int topn = 10;                 // 5/10/15（新窗默认值，M3-1 起每窗独立）
    int memory_basis = 0;          // 0=private commit 1=working set
    int io_view = 0;               // 0=combined 1=read 2=write（新窗默认值）
    int color_mode = 0;            // 0=auto 1=dark 2=light
    int popup_effect = 0;          // 0=acrylic 1=transparent 2=blur 3=opaque
    int popup_opacity = 100;       // 10-100
    int metric = 0;                // v1 迁移源（schema 2 不再作为运行配置）
    int win_x = INT_MIN, win_y = INT_MIN;   // v1 迁移源
    int schema = 2;                // schema 版本（写盘固定 2）
    std::vector<WindowCfg> windows;   // 空 = 悬浮窗模式关闭（item 可重开默认窗）
    // GPU 口径（方案 §2.4 引擎类型多选 + 截断；§2.5 显存口径）
    std::wstring gpu_engines = L"all";   // "all" 或逗号分隔 engtype 列表
    bool gpu_cap_100 = true;             // 引擎求和 >100% 截断
    int vram_basis = 0;                  // 0=dedicated 1=dedicated+shared
    std::wstring lang = L"auto";   // auto or BCP-47
    bool debug_logs = false;
    int dock_edge = 0;              // 停靠边偏好（M3-2 方案 §7.2）：0=auto 1=right 2=left
    bool sensor_summary = false;    // 标题栏传感器摘要（M3-3 L0，方案 §9 默认关）

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
        if (metric < 0 || metric > 4) metric = 0;   // cpu/mem/io/gpu/vram
        if (vram_basis != 0 && vram_basis != 1) vram_basis = 0;
        if (gpu_engines.empty()) gpu_engines = L"all";
        if (lang.empty()) lang = L"auto";
        if (dock_edge < 0 || dock_edge > 2) dock_edge = 0;
        // windows[]：数量钳到上限，每行字段钳域（手改 settings.json 防砖）。
        // kind 域 = MetricKind 枚举全集 [0,6]（3/4 是 DiskRead/DiskWrite 占位）
        if (windows.size() > 6) windows.resize(6);
        for (auto& c : windows) {
            if (c.kind < 0 || c.kind > 6) c.kind = 0;
            if (c.io_view < 0 || c.io_view > 2) c.io_view = 0;
            if (c.topn != 5 && c.topn != 10 && c.topn != 15) c.topn = 10;
            if (c.docked != 0 && c.docked != 1) c.docked = 0;
            if (c.dock_target < 0 || c.dock_target > 2) c.dock_target = 0;
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

    // v1 迁移：加载后若无 windows 且有 metric/win_x → 生成 windows[0]
    // （v1 用户原是单窗模式，指标与位置原样搬入）
    void MigrateV1() {
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
    FILE* f = _wfopen(path, L"w");
    if (!f) return false;
    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": %d,\n", st.schema);
    fprintf(f, "  \"refresh_ms\": %d,\n", st.refresh_ms);
    fprintf(f, "  \"topn\": %d,\n", st.topn);
    fprintf(f, "  \"memory_basis\": %d,\n", st.memory_basis);
    fprintf(f, "  \"io_view\": %d,\n", st.io_view);
    fprintf(f, "  \"color_mode\": %d,\n", st.color_mode);
    fprintf(f, "  \"popup_effect\": %d,\n", st.popup_effect);
    fprintf(f, "  \"popup_opacity\": %d,\n", st.popup_opacity);
    fprintf(f, "  \"gpu_engines\": \"%s\",\n", PtToUtf8(st.gpu_engines).c_str());
    fprintf(f, "  \"gpu_cap_100\": %s,\n", st.gpu_cap_100 ? "true" : "false");
    fprintf(f, "  \"vram_basis\": %d,\n", st.vram_basis);
    fprintf(f, "  \"debug_logs\": %s,\n", st.debug_logs ? "true" : "false");
    fprintf(f, "  \"lang\": \"%s\",\n", PtToUtf8(st.lang).c_str());
    fprintf(f, "  \"dock_edge\": %d,\n", st.dock_edge);
    fprintf(f, "  \"sensor_summary\": %s,\n", st.sensor_summary ? "true" : "false");
    // windows[]（schema 2 核心字段；v1 的 metric/win_x/win_y 停止写出）
    fprintf(f, "  \"windows\": [\n");
    for (size_t i = 0; i < st.windows.size(); i++) {
        const WindowCfg& c = st.windows[i];
        fprintf(f, "    {\"kind\":%d,\"io_view\":%d,\"topn\":%d,\"x\":%d,\"y\":%d,"
                   "\"docked\":%d,\"dock_target\":%d}%s\n",
                c.kind, c.io_view, c.topn, c.x, c.y, c.docked, c.dock_target,
                (i + 1 < st.windows.size()) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
    return true;
}

inline bool PtLoadSettings(const std::wstring& config_dir, PtSettings& st) {
    if (config_dir.empty()) return false;
    wchar_t path[MAX_PATH];
    if (swprintf_s(path, MAX_PATH, L"%s\\settings.json", config_dir.c_str()) <= 0) return false;
    FILE* f = _wfopen(path, L"r");
    if (!f) return false;
    char buf[4096] = {};
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    std::string json(buf, len);

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
        size_t e = json.find('"', pos + 1);
        if (e == std::string::npos) return;
        out = PtFromUtf8(json.substr(pos + 1, e - pos - 1));
    };

    geti("\"refresh_ms\"", st.refresh_ms);
    geti("\"topn\"", st.topn);
    geti("\"memory_basis\"", st.memory_basis);
    geti("\"io_view\"", st.io_view);
    geti("\"metric\"", st.metric);          // v1 迁移源
    geti("\"win_x\"", st.win_x);            // v1 迁移源
    geti("\"win_y\"", st.win_y);            // v1 迁移源
    geti("\"color_mode\"", st.color_mode);
    geti("\"popup_effect\"", st.popup_effect);
    geti("\"popup_opacity\"", st.popup_opacity);
    gets("\"gpu_engines\"", st.gpu_engines);
    getb("\"gpu_cap_100\"", st.gpu_cap_100);
    geti("\"vram_basis\"", st.vram_basis);
    getb("\"debug_logs\"", st.debug_logs);
    gets("\"lang\"", st.lang);
    geti("\"dock_edge\"", st.dock_edge);
    getb("\"sensor_summary\"", st.sensor_summary);

    // windows[] 段解析：定位 "windows" → [ → 逐 {…} 行按 key 读 7 个 int。
    // 行内无嵌套对象/转义引号，find('}') 即行界（方案 §5.2）。
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
                c.docked = rowi("\"docked\"", 0);
                c.dock_target = rowi("\"dock_target\"", 0);
                st.windows.push_back(c);
                cur = cb + 1;
            }
        }
    } else {
        st.MigrateV1();   // 无 windows 键 = v1 配置 → 迁移
    }
    st.Clamp();
    return true;
}
