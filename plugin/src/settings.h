#pragma once
// ============================================================
// settings.h — settings.json persistence (技术方案 §6.4 schema, M1 fields)
// Same hand-rolled read/write style as PNM: fprintf out, find+atoi in.
// No third-party JSON dependency (零依赖原则).
// ============================================================
#include <windows.h>
#include <cstdio>
#include <string>

#include "top_aggregator.h"   // MetricKind

struct PtSettings {
    int refresh_ms = 500;          // 250/500/1000/2000
    int topn = 10;                 // 5/10/15
    int memory_basis = 0;          // 0=private commit 1=working set
    int io_view = 0;               // 0=combined 1=read 2=write
    int color_mode = 0;            // 0=auto 1=dark 2=light
    int popup_effect = 0;          // 0=acrylic 1=transparent 2=blur 3=opaque
    int popup_opacity = 100;       // 10-100
    int metric = 0;                // single-window bound metric (MetricKind)
    int win_x = INT_MIN, win_y = INT_MIN;   // single-window position
    std::wstring lang = L"auto";   // auto or BCP-47
    bool debug_logs = false;

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
        if (metric < 0 || metric > 2) metric = 0;   // M1: cpu/mem/io
        if (lang.empty()) lang = L"auto";
    }

    MetricKind Kind() const {
        switch (metric) {
        case 1: return MetricKind::Mem;
        case 2: return MetricKind::DiskIo;
        default: return MetricKind::Cpu;
        }
    }
    void SetKind(MetricKind k) {
        switch (k) {
        case MetricKind::Mem: metric = 1; break;
        case MetricKind::DiskIo: metric = 2; break;
        default: metric = 0; break;
        }
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
    fprintf(f, "  \"refresh_ms\": %d,\n", st.refresh_ms);
    fprintf(f, "  \"topn\": %d,\n", st.topn);
    fprintf(f, "  \"memory_basis\": %d,\n", st.memory_basis);
    fprintf(f, "  \"io_view\": %d,\n", st.io_view);
    fprintf(f, "  \"metric\": %d,\n", st.metric);
    fprintf(f, "  \"win_x\": %d,\n", st.win_x);
    fprintf(f, "  \"win_y\": %d,\n", st.win_y);
    fprintf(f, "  \"color_mode\": %d,\n", st.color_mode);
    fprintf(f, "  \"popup_effect\": %d,\n", st.popup_effect);
    fprintf(f, "  \"popup_opacity\": %d,\n", st.popup_opacity);
    fprintf(f, "  \"debug_logs\": %s,\n", st.debug_logs ? "true" : "false");
    fprintf(f, "  \"lang\": \"%s\"\n", PtToUtf8(st.lang).c_str());
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
        out = (json.compare(pos + 1, 4, "true") == 0);
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
    geti("\"metric\"", st.metric);
    geti("\"win_x\"", st.win_x);
    geti("\"win_y\"", st.win_y);
    geti("\"color_mode\"", st.color_mode);
    geti("\"popup_effect\"", st.popup_effect);
    geti("\"popup_opacity\"", st.popup_opacity);
    getb("\"debug_logs\"", st.debug_logs);
    gets("\"lang\"", st.lang);
    st.Clamp();
    return true;
}
