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

enum class MetricKind { Cpu, Mem, DiskIo, DiskRead, DiskWrite };

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
    MetricView cpu, mem, io, io_read, io_write;
    // 标题栏摘要
    double cpu_total_pct = -1;
    uint64_t mem_total = 0, mem_used = 0;
    double io_read_total_bps = 0, io_write_total_bps = 0;
    FileIoGrade io_grade = FileIoGrade::C;
    uint64_t io_events_lost = 0;
    ULONGLONG stamp = 0;
};

struct AggregateSettings {
    int topn = 10;
};

class MetricsCollector;

class TopAggregator {
public:
    // Runs on the tick thread. io_delta: this tick's exact ETW increments
    // (valid=true only in grade A/B); when invalid the C-grade all-handle
    // speeds from the snapshot are used and io_grade says so honestly.
    // mc: name-cache owner for short-lived process fallback naming.
    void Build(const CpuMemIoMetrics& m,
               const EtwFileCollector::IoDelta* io_delta,
               double dt,
               const AggregateSettings& cfg,
               MetricsCollector& mc,
               MetricsSnapshot& out);
};
