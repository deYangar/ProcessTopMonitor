#pragma once
// ============================================================
// gpu_collector — per-process GPU utilization + video memory
// (技术方案 §2.4/§2.5, M2)
//
// Grade state machine (方案 §2.4 三级降级链):
//   G1 (Win10 1709+, build 16299+): PDH "GPU Engine" / "GPU Process Memory"
//        / "GPU Adapter Memory" counters — the Task-Manager-grade main line.
//   G2 (Win8 ~ Win10 1607): D3DKMTQueryStatistics (gdi32 dynamic, WDK DDI);
//        no engtype naming -> engine scope degrades to "all nodes summed",
//        honestly annotated (implemented in the G2 path below).
//   G3 (Win7 / no GPU / RDP without counters): dimension hidden; the
//        title-bar summary falls back to the host TM's whole-GPU value.
//
// PDH counter paths are added via PdhAddEnglishCounterW (locale-proof).
// Instances:
//   GPU Engine:          pid_1234_luid_0x..._phys_0_eng_0_engtype_3D
//   GPU Process Memory:  pid_1234_luid_0x..._phys_0
//   GPU Adapter Memory:  luid_0x..._phys_0 (system-wide, summary line)
// Parsing is loose (pid_/engtype_ anchors, rest ignored — 方案 §10 风险 8).
//
// Scope (方案 §2.4 已拍板): engines selectable by engtype (dynamic list from
// live instances, "all" = sum of everything); sum across engines/instances
// may exceed 100% -> optional clamp (gpu_cap_100, default on).
//
// Threads: PDH query handles live on the tick thread only (EnsureStarted /
// Tick / EnsureStopped all called there). Configure() may be called from the
// UI thread (options/menu) — guarded by a critical section.
// ============================================================
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <cstdint>

enum class GpuGrade { G1, G2, G3 };

// 口径配置（settings 映射，方案 §2.4 引擎类型多选 + §2.5 显存口径）
struct GpuScopeConfig {
    bool all_engines = true;
    std::vector<std::wstring> engines;   // engtype list when !all_engines
    bool cap_100 = true;                 // >100% 截断开关（默认开）
    bool vram_with_shared = false;       // 显存口径：0 专用 / 1 专用+共享

    bool operator==(const GpuScopeConfig& o) const {
        return all_engines == o.all_engines && cap_100 == o.cap_100 &&
               vram_with_shared == o.vram_with_shared && engines == o.engines;
    }
};

// One collection result (tick thread -> snapshot via the aggregator).
struct GpuTickData {
    bool util_valid = false;             // needs 2 collections (range counter)
    bool mem_valid = false;              // raw counter: valid from the 1st collection
    bool sys_mem_valid = false;          // adapter-memory counters present
    // per-pid values with the scope + clamp already applied:
    std::unordered_map<DWORD, double> util_by_pid;                       // %
    std::unordered_map<DWORD, std::pair<uint64_t, uint64_t>> mem_by_pid; // (dedicated, shared)
    double total_util_pct = 0;           // system-wide, same scope, clamped
    uint64_t sys_dedicated = 0;          // adapter memory (summary line)
    uint64_t sys_shared = 0;
    std::vector<std::wstring> known_engtypes;   // dynamic enumeration (sorted)
    GpuGrade grade = GpuGrade::G3;
    std::wstring unsupported_reason;     // G3 display reason
};

class MetricsCollector;
struct ProcMetric;

class GpuCollector {
public:
    GpuCollector() = default;
    ~GpuCollector();

    // UI thread (menu/options). Takes effect on the next Tick.
    void Configure(const GpuScopeConfig& cfg);
    GpuScopeConfig Scope() const;

    // OS-version gate only (no PDH resources): run once at init so the
    // metric menu can gray the dimension on G3 systems before any GPU view.
    void ProbeGrade() { std::wstring why; DetectGrade(why); m_grade_reason = why; }
    const wchar_t* Reason() const { return m_grade_reason.c_str(); }

    // Tick thread. Starts the PDH query (grade detection happens here:
    // OS build gate + counter presence). Returns false when the dimension
    // is unavailable (G3).
    bool EnsureStarted();
    // Tick thread. Zero-residue shutdown when no window binds GPU/VRAM
    // (方案 §4 设计决策 2: PDH 全关).
    void EnsureStopped();
    bool IsRunning() const { return m_running.load(); }
    GpuGrade Grade() const { return m_grade; }

    // Tick thread, every 2nd tick (~1s). Fills out; safe to call when not
    // running (returns immediately with grade=G3).
    // procs: current snapshot pid list — G2 (D3DKMT) needs candidate pids;
    // G1 (PDH) ignores it.
    void Tick(GpuTickData& out, const std::vector<DWORD>& pid_candidates);

private:
    bool OpenQuery();                  // tick thread (G1)
    void CloseQuery();                 // tick thread (G1)
    bool InitG2();                     // tick thread (G2): gdi32 + adapters
    void StopG2();
    void DetectGrade(std::wstring& reason);   // tick thread
    bool TickG2(GpuTickData& out, const std::vector<DWORD>& pid_candidates);

    mutable std::mutex m_cfg_mutex;
    GpuScopeConfig m_cfg;

    std::atomic<bool> m_running{ false };
    GpuGrade m_grade = GpuGrade::G3;
    std::wstring m_grade_reason;

    void* m_query = nullptr;                     // PDH_HQUERY
    void* m_cnt_util = nullptr;                  // GPU Engine Utilization Percentage
    void* m_cnt_mem_ded = nullptr;               // GPU Process Memory Dedicated/Local Usage
    void* m_cnt_mem_shared = nullptr;            // GPU Process Memory Shared Usage (optional)
    void* m_cnt_adapt_ded = nullptr;             // GPU Adapter Memory DedicatedUsage (optional)
    void* m_cnt_adapt_shared = nullptr;          // GPU Adapter Memory SharedUsage (optional)
    bool m_mem_ded_is_local = false;             // Local Usage fallback (older builds)
    int m_collections = 0;

    // instance-name parse caches (tick thread only): instance -> pid/engtype
    std::unordered_map<std::wstring, std::pair<DWORD, std::wstring>> m_eng_cache;
    std::unordered_map<std::wstring, DWORD> m_pid_cache;
    std::unordered_set<std::wstring> m_engtypes;

    std::vector<BYTE> m_fmt_buf;                 // reused formatted-array buffer

    // ---- G2 (D3DKMTQueryStatistics, Win8~Win10 1607) ----
    // 按本机 SDK d3dkmthk.h 转录的最小查询布局：成员顺序与官方一致，Result
    // 用冗余缓冲替代结果 union（内核只写不超缓冲，我们只读已知头部字段）。
    // 实测定案（2026-10-25 探针+插件双路验证）：PROCESS_NODE/SEGMENT_GROUP
    // 在 26300 上可用，但要求 hProcess 为 PROCESS_QUERY_INFORMATION 权限
    // （LIMITED 被拒 C000000D），且进程在该适配器有 GPU 统计（无上下文进程
    // 同样拒收）——失败按无数据处理，受保护进程（dwm 等）如实缺席。
    struct G2Adapter {
        LUID luid;
        ULONG node_count = 0;
        ULONG nb_segments = 0;
        std::vector<ULONG> seg_aperture;         // per-segment (0xFF=unknown)
        bool seg_group_ok = false;               // PROCESS_SEGMENT_GROUP 可用(WDDM2.1+)
    };
    std::vector<G2Adapter> m_g2_adapters;
    // per-pid delta 基线：adapter idx -> per-node RunningTime（µs，WDK 注释）
    std::unordered_map<DWORD, std::vector<std::vector<LONGLONG>>> m_g2_last_rt;
    LONGLONG m_g2_last_wall_us = 0;
    void* m_gdi32 = nullptr;
    void* m_pfn_d3dkmt_query = nullptr;          // D3DKMTQueryStatistics
    void* m_pfn_d3dkmt_enum = nullptr;           // D3DKMTEnumAdapters
};
