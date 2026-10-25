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
#include <cstdint>

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
    std::vector<ProcMetric> procs;     // idle/system excluded, all others
};

class MetricsCollector {
public:
    // Runs on the timer-queue tick thread. Cheap (1-5ms per call).
    void Tick(CpuMemIoMetrics& out);

    void SetMemBasisWorkingSet(bool ws) { m_mem_ws = ws; }
    bool MemBasisWorkingSet() const { return m_mem_ws; }

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
#pragma pack(push, 8)
    struct PtUnicodeString {
        USHORT Length;
        USHORT MaximumLength;
        wchar_t* Buffer;   // x64: 2+2+pad4+8 = 16, x86: 2+2+4 = 8 — NT layout
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

    // name/path caches, guarded by m_name_mutex (LookupName: any thread)
    mutable std::mutex m_name_mutex;
    std::unordered_map<DWORD, std::wstring> m_names;
    std::unordered_map<DWORD, std::wstring> m_paths;
};
