#pragma once
// ============================================================
// top_aggregator — merge same-name processes, sort per metric, cut Top N
// (技术方案 §3.4). Pure data plumbing; no windows, no locks.
// ============================================================
#include <windows.h>
#include <string>
#include <vector>
#include "metrics_collector.h"
#include "etw_file_collector.h"
#include "gpu_collector.h"

enum class MetricKind { Cpu, Mem, DiskIo, DiskRead, DiskWrite, Gpu, Vram };

// 指标绑定位集（bit per MetricKind，M3-1 方案 §2.2）：多窗模式下 GPU 按
// 需启停的判定基础——mask 含 Gpu/Vram 任一位才保采集。
inline int PtKindMask(MetricKind k) { return 1 << (int)k; }

struct TopRow {
    std::wstring name;
    std::wstring exe_path;   // resolved lazily by the window (icon lookup)
    DWORD pid = 0;           // representative pid (largest contributor)
    int count = 1;           // merged process count (chrome.exe × 12 etc.)
    double value = 0;
};

struct MetricView {
    std::vector<TopRow> rows;
    bool has_value = false;  // false on the first tick (needs a delta)
};

struct MetricsSnapshot {
    MetricView cpu, mem, io, io_read, io_write, gpu, vram;
    // 标题栏摘要
    double cpu_total_pct = -1;
    uint64_t mem_total = 0, mem_used = 0;
    double io_read_total_bps = 0, io_write_total_bps = 0;
    // GPU/显存摘要
    double gpu_total_pct = 0;            // selected scope, clamped (G1/G2)
    bool gpu_total_valid = false;
    uint64_t vram_sys_dedicated = 0;     // adapter memory (system-wide)
    uint64_t vram_sys_shared = 0;
    bool vram_sys_valid = false;
    GpuGrade gpu_grade = GpuGrade::G3;
    std::wstring gpu_reason;             // G3 display reason
    std::vector<std::wstring> gpu_engtypes;   // dynamic engine list (menu)
    int tm_gpu_usage = -1;               // host TM whole-GPU value (G3 summary fallback)
    FileIoGrade io_grade = FileIoGrade::C;
    uint64_t io_events_lost = 0;
    ULONGLONG stamp = 0;
};

struct AggregateSettings {
    int topn = 10;
    bool vram_with_shared = false;       // 显存口径：专用 / 专用+共享（方案 §2.5）
};

class MetricsCollector;

class TopAggregator {
public:
    // Runs on the tick thread. io_delta: this tick's exact ETW increments
    // (valid=true only in grade A/B); when invalid the C-grade all-handle
    // speeds from the snapshot are used and io_grade says so honestly.
    // gpu: GPU/VRAM collection (nullptr = dimension off/not bound this tick).
    // mc: name-cache owner for short-lived process fallback naming.
    void Build(const CpuMemIoMetrics& m,
               const EtwFileCollector::IoDelta* io_delta,
               const GpuTickData* gpu,
               double dt,
               const AggregateSettings& cfg,
               MetricsCollector& mc,
               MetricsSnapshot& out);
};
