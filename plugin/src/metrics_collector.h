#pragma once
// ============================================================
// metrics_collector — CPU / memory / fallback-IO snapshot collector
// (技术方案 §2.1/§2.2)
//
// Single NtQuerySystemInformation(SystemProcessInformation) call per tick
// returns every process with CPU times, memory sizes and IO counters —
// the same data source as Task Manager's process list.
//
//  - CPU%  : (dKernel+dUser) / (dWall * LogicalProcessorCount) * 100
//  - Memory: PrivatePageCount (default) or WorkingSetSize
//  - IO    : Read/WriteTransferCount deltas — ALL-handle bytes (file +
//            network socket + device ioctl mixed). This is the C-grade
//            fallback only; the exact file-IO view comes from
//            etw_file_collector (A/B grades). 口径不冒充：调用方必须按
//            ETW 档位选择数据源并如实标注（技术方案 §2.3）。
//
// WOW64 safety: the struct below uses ULONG_PTR-sized fields and is stepped
// by NextEntryOffset — the wow64 layer returns the caller-bitness view, the
// Process Hacker approach (技术方案 §2.1 风险与对策).
// ============================================================
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <cstdint>

struct CoreClassMap;   // core_class_mapper.h（弱引用注入，避免头依赖）

struct ProcMetric {
    DWORD pid = 0;
    std::wstring name;                 // may be empty (protected processes)
    double cpu_pct = 0;                // valid after the 2nd tick
    uint64_t mem = 0;                  // basis selected by settings
    uint64_t mem_private = 0;
    uint64_t mem_ws = 0;
    double io_read_bps = 0;            // C-grade (all handles incl. network)
    double io_write_bps = 0;
};

struct CpuMemIoMetrics {
    // system summary (标题栏摘要)
    double cpu_total_pct = -1;         // -1 = first tick, no delta yet
    uint64_t mem_total = 0;
    uint64_t mem_used = 0;
    bool first = true;
    // 当前 CPU 频率（MHz，整数；PDH % Processor Performance × MaxMhz）。
    // 宿主 MonitorInfo.cpu_freq 只有整数 GHz（float 截断），拿不到小数位，
    // 摘要行要 "4.9GHz" 必须自测（M5 用户反馈修复）。
    int cpu_freq_mhz = 0;
    // 系统级核群占用率（核型拆分方案 §4.5，一期）：各核群自身容量口径
    //（「P 38」读作 P 核群忙 38%）。-1 = 同构/首拍/NtQSI 失败。
    // [1] = P/VCache 群，[0] = E/StdCcd 群
    double cpu_split_total_pct[2] = { -1, -1 };
    // 核群 LP 数与全机 LP 数（2026-11-10 方案②其余合计行用：群总 ×
    // LP_g/total_lp 换算到全核口径，才能与行值（分母=全部核）同量纲对
    // 账）。异构时随 split 计算填写；同构恒 0（下游 share=0 → rest 不显示）
    int cpu_split_lp[2] = { 0, 0 };   // [1]=P/VCache 群 [0]=E/StdCcd 群
    int cpu_lp_total = 0;             // 全机 LP 数（含防御性 Homog 核）
    std::vector<ProcMetric> procs;     // idle excluded; System(pid4) in ranking
};

class MetricsCollector {
public:
    // Runs on the timer-queue tick thread. Cheap (1-5ms per call).
    void Tick(CpuMemIoMetrics& out);

    void SetMemBasisWorkingSet(bool ws) { m_mem_ws = ws; }
    bool MemBasisWorkingSet() const { return m_mem_ws; }

    // 核型分类表注入（Start 时一次，tick 线程调用）。异构时 Tick 内跑
    // 第二次 NtQSI(SystemProcessorPerformanceInformation) 出核群占用率
    //（核型拆分方案 §4.5）；nullptr/同构 = 不跑，cpu_split_total_pct 恒 -1
    void SetCoreClassMap(const struct CoreClassMap* ccm) { m_ccm = ccm; }

    // 本拍从 SPI 尾部线程数组构建的 tid→pid 表（核型样本归因数据源，
    // §4.2 探针定案路线；tick 线程调 Take 转交 ETW collector，空 = 未构建）
    std::shared_ptr<const std::unordered_map<DWORD, DWORD>> TakeThreadTable() {
        return m_thread_table;
    }

    // Process-name cache, "name never expires" policy: pid -> last known
    // name/path survives process exit (short-lived processes keep a name in
    // the ETW view). Thread-safe (tick + UI threads).
    std::wstring LookupName(DWORD pid);
    std::wstring LookupPath(DWORD pid);

private:
    // 手工定义的 SYSTEM_PROCESS_INFORMATION（Winternl 布局，Process Hacker
    // 同款）。尾部线程数组不解析——按 NextEntryOffset 步进。
    // UNICODE_STRING 手工定义（WIN32_LEAN_AND_MEAN 排除了 winternl.h，
    // 自带声明会与宿主环境可能的 winternl 引用冲突，取独立名字）
    // 2026-11-09 核型拆分：尾部线程数组开始解析（tid→pid 表，§4.2）
#pragma pack(push, 8)
    struct PtUnicodeString {
        USHORT Length;
        USHORT MaximumLength;
        wchar_t* Buffer;   // x64: 2+2+pad4+8 = 16, x86: 2+2+4 = 8 — NT layout
    };
    // SYSTEM_THREAD_INFORMATION（SPI 之后紧跟的数组元素，NT 布局）
    struct SysThreadInfo {
        LARGE_INTEGER KernelTime;
        LARGE_INTEGER UserTime;
        LARGE_INTEGER CreateTime;
        ULONG WaitTime;                 // +4 pad on x64
        void* StartAddress;
        HANDLE UniqueProcess;           // CLIENT_ID
        HANDLE UniqueThread;
        LONG Priority;
        LONG BasePriority;
        ULONG ContextSwitches;
        ULONG ThreadState;
        ULONG WaitReason;               // 尾部 pad to 8（x64: 80B / x86: 64B）
    };
    struct SPI {
        ULONG NextEntryOffset;
        ULONG NumberOfThreads;
        LARGE_INTEGER WorkingSetPrivateSize;
        ULONG HardFaultCount;
        ULONG NumberOfThreadsHighWatermark;
        ULONGLONG CycleTime;
        LARGE_INTEGER CreateTime;
        LARGE_INTEGER UserTime;
        LARGE_INTEGER KernelTime;
        PtUnicodeString ImageName;
        LONG BasePriority;
        HANDLE UniqueProcessId;
        HANDLE InheritedFromUniqueProcessId;
        ULONG HandleCount;
        ULONG SessionId;
        ULONG_PTR UniqueProcessKey;
        SIZE_T PeakVirtualSize;
        SIZE_T VirtualSize;
        ULONG PageFaultCount;
        SIZE_T PeakWorkingSetSize;
        SIZE_T WorkingSetSize;
        SIZE_T QuotaPeakPagedPoolUsage;
        SIZE_T QuotaPagedPoolUsage;
        SIZE_T QuotaPeakNonPagedPoolUsage;
        SIZE_T QuotaNonPagedPoolUsage;
        SIZE_T CommitCharge;            // == PrivatePageCount (PH alias)
        SIZE_T PeakCommitCharge;
        SIZE_T ReadOperationCount;
        SIZE_T WriteOperationCount;
        SIZE_T OtherOperationCount;
        SIZE_T ReadTransferCount;
        SIZE_T WriteTransferCount;
        SIZE_T OtherTransferCount;
        // 尾部对齐（2026-11-09 实测）：官方 sizeof(SYSTEM_PROCESS_INFORMATION)
        // x64 = 256，上面字段到 248；线程数组紧跟结构（起点 = 结构+256）。
        // 没有这 8 字节时尾部线程数组的 tid 解析整体错位（归因全 unknown，
        // tools/cpuprofile_probe/diag_ioffset.cpp 实测定案）。字段偏移本身
        // 与旧手写完全一致——生产 CPU/内存/IO 读数不受影响
        uint64_t TailPadding = 0;
    };
#pragma pack(pop)

    struct Prev {
        uint64_t cpu_time = 0;         // kernel+user, 100ns units
        uint64_t io_read = 0;
        uint64_t io_write = 0;
        bool seen = false;
    };

    bool Snapshot();                   // fills m_raw / returns false on failure
    void Remember(DWORD pid, const std::wstring& name);
    int QueryCpuFreqMhz();             // tick thread; PDH 当前频率（MHz）
    void TickCoreClassTotals(CpuMemIoMetrics& out, double dt);  // §4.5 系统级 P/E

    // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION（info class 8）手写布局：
    // 按编译位宽步进（x64 sizeof 48 / x86 sizeof 44，WOW64 thunk 返回
    // 匹配位宽的视图——与 SPI 同款约定）。数组每核一条，跨组 group-major
    // 连续 = 绝对 LP 号空间（§3.2）
#pragma pack(push, 8)
    struct SPPI {
        LARGE_INTEGER IdleTime;
        LARGE_INTEGER KernelTime;
        LARGE_INTEGER UserTime;
        LARGE_INTEGER DpcTime;
        LARGE_INTEGER InterruptTime;
        ULONG InterruptCount;
    };
#pragma pack(pop)

    // CPU 频率自测（tick thread only）
    void* m_freq_query = nullptr;      // PDH_HQUERY
    void* m_freq_counter = nullptr;    // PDH_HCOUNTER
    double m_freq_max_ghz = 0;         // MaxMhz / 1000
    int m_freq_collections = 0;
    int m_freq_div = 0;
    int m_freq_mhz = 0;

    // NtQuerySystemInformation, resolved once
    void* m_ntqsi = nullptr;
    bool m_ntqsi_tried = false;

    std::vector<BYTE> m_buf;           // grows as needed, reused across ticks
    int m_cpu_cores = 0;

    // per-pid delta state (tick thread only)
    std::unordered_map<DWORD, Prev> m_prev;
    std::unordered_map<DWORD, uint64_t> m_prev_total_cpu; // idle accounting
    uint64_t m_prev_idle_time = 0;
    bool m_have_prev = false;
    ULONGLONG m_prev_wall = 0;
    bool m_mem_ws = false;

    // ---- 核型拆分（§4.5）----
    const struct CoreClassMap* m_ccm = nullptr;   // Start 时注入（弱引用）
    std::vector<BYTE> m_sppi_buf;                 // NtQSI class 8 缓冲（复用）
    std::vector<uint64_t> m_prev_idle_core;       // 每核 Idle 基线（绝对 LP 号索引）
    bool m_have_prev_idle_core = false;
    // 本拍 tid→pid 表（tick 线程构建；TakeThreadTable 弱引用转交）
    std::shared_ptr<const std::unordered_map<DWORD, DWORD>> m_thread_table;

    // name/path caches, guarded by m_name_mutex (LookupName: any thread)
    mutable std::mutex m_name_mutex;
    std::unordered_map<DWORD, std::wstring> m_names;
    std::unordered_map<DWORD, std::wstring> m_paths;
};
