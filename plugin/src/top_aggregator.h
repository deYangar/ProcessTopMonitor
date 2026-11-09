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

enum class MetricKind { Cpu, Mem, DiskIo, DiskRead, DiskWrite, Gpu, Vram, CpuP, CpuE };

// 指标绑定位集（bit per MetricKind，M3-1 方案 §2.2）：多窗模式下 GPU 按
// 需启停的判定基础——mask 含 Gpu/Vram 任一位才保采集。
inline int PtKindMask(MetricKind k) { return 1 << (int)k; }

struct TopRow {
    std::wstring name;
    std::wstring exe_path;   // resolved lazily by the window (icon lookup)
    DWORD pid = 0;           // representative pid (largest contributor)
    int count = 1;           // merged process count (chrome.exe × 12 etc.)
    double value = 0;
    // DiskIo 行的读/写速率拆分（问题③合计视图双列显示用；数据层
    // per_pid 本就 {read,write} 独立累计，这里只是 passthrough。
    // 其他维度恒 0；io 合计视图 value = max(read,write)（排序键））
    double io_read_bps = 0;
    double io_write_bps = 0;
    // 核型拆分（§4.3）：进程核型占比 passthrough（CPU 视图行 tooltip 用，
    // io_read_bps 先例同款）——全部样本口径百分比，其他维度恒 0
    double cpu_p_pct = 0;
    double cpu_e_pct = 0;
};

struct MetricView {
    std::vector<TopRow> rows;
    bool has_value = false;  // false on the first tick (needs a delta)
};

struct MetricsSnapshot {
    MetricView cpu, mem, io, io_read, io_write, gpu, vram;
    // 核型拆分两视图（§5 top_aggregator.h 行）：异构机器上随快照构建，
    // 同构机器恒 has_value=false（窗口列表不含 = 不可选）
    MetricView cpu_p, cpu_e;
    // 标题栏摘要
    double cpu_total_pct = -1;
    uint64_t mem_total = 0, mem_used = 0;
    double io_read_total_bps = 0, io_write_total_bps = 0;
    // 系统级核群占用率（§4.5，核群自身容量口径；-1 = 无效/同构）
    double cpu_p_total_pct = -1, cpu_e_total_pct = -1;
    // 核型列可用性（§6：会话 C 档/窗口不完整 = false → 列不可用态）与
    // 标签风格（true: X3D/标准；false: P核/E核）
    bool cpu_split_valid = false;
    bool cpu_split_x3d = false;
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
    // 宿主传感器（M3-3 L0 白拿；0 = TM 未启用该项监控 → 摘要行自动隐藏）
    int tm_cpu_temp = 0;
    int tm_gpu_temp = 0;
    int tm_cpu_freq = 0;                 // MHz
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
    // (valid=true only in grade A/B AND the handoff window is complete,
    // 方案三); when invalid the C-grade all-handle speeds from the snapshot
    // are used and io_grade says so honestly. 精确口径速率分母 =
    // io_delta->window_us（handoff 真实窗口），不再接受 tick 假 dt——
    // 窗口错位（首窗几十 ms / 稳态 handoff ≠ tick 间隔）的低估由此根治。
    // gpu: GPU/VRAM collection (nullptr = dimension off/not bound this tick).
    // mc: name-cache owner for short-lived process fallback naming.
    void Build(const CpuMemIoMetrics& m,
               const EtwFileCollector::IoDelta* io_delta,
               const GpuTickData* gpu,
               const AggregateSettings& cfg,
               MetricsCollector& mc,
               MetricsSnapshot& out);
};
