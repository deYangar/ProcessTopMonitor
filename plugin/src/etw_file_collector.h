#pragma once
// ============================================================
// etw_file_collector — 内核会话采集器：FileIo 字节 + CPU 核型样本
// （FileIo 见下；核型样本通道 = 核型拆分方案 §4，2026-11-09）
//
// Grade state machine:
//   A (Win8+):  own EVENT_TRACE_SYSTEM_LOGGER_MODE session
//               "PTMFileIoSession" with EVENT_TRACE_FLAG_FILE_IO only —
//               coexists with PNM's NT Kernel Logger (multiple system-logger
//               sessions are a Win8+ feature).
//   B (Win7):   the single NT Kernel Logger, used only when idle.
//               When occupied (e.g. PNM's ETW) -> C, NO stop-and-takeover
//               (a session tug-of-war between sibling plugins is worse).
//   C:          no session. Caller must fall back to the all-handle IO
//               counters from metrics_collector and show the yellow
//               warning (口径如实标注, never silently impersonate A/B).
//
// Event parsing is manual fixed-offset (NO TDH): FileIo Read/Write is one
// of the most stable classic events (MSDN FileIo_ReadWrite):
//     IrpPtr [ptr] ByteOffset [8] FileObject [ptr] TransferSize [4]
// Pointer width follows the KERNEL's bitness (kernel sessions are not
// wow64-translated), so an x86 plugin on x64 parses with 8-byte pointers.
// Read = event type 67, Write = 68 (MSDN FileIo class table).
//
// ---- 核型样本通道（方案 §4.2，探针实证后的定案形态）----
// Win11 26300 实测（tools/cpuprofile_probe v1-v6，2026-11-09）：
//   · SampledProfile = PerfInfo 新 GUID ce1dbfb4… + Opcode 46（文档 GUID
//     3d6fa8d1 一条未出现；连 WPR 官方 ETL 亦然）
//   · 事件头 ProcessId/ThreadId 对 SampledProfile 恒为 0xFFFFFFFF（无归因）
//     ——方案 §4.2 主路线（Header.ProcessId）与预埋回退（header ThreadId
//     → pid 表）双双实证死亡；唯一可行 = CSwitch 流重建线程上下文
//     （WPA 同机制）：DISPATCHER flag 的 CSwitch（旧 GUID 3d6fa8d1 op36，
//     payload 头 8B = NewTid/OldTid）维护每核 current_tid，sample 到达
//     查表 → tid→pid 表（metrics_collector 每 tick 解析 SPI 线程数组供给）
//   · 归因失败样本（unknown tid / Idle / System）保留在 sample_total 分母
//
// Handoff: the ProcessTrace callback thread owns a private accumulator map
// (lock-free in the callback). The tick thread requests a handoff via an
// atomic flag; the consumer thread (event callback or the periodic buffer
// callback) ACCUMULATES into the out map and bumps a sequence — single-
// writer handoff, zero locks on the hot path (技术方案 §5.1).
// 2026-11-08 方案三改造：handoff 带窗口记账（QPC），交付 = 字节+窗口
// 累计进 m_out；tick 侧「完整窗口才接管」——窗口 < 半个 tick 间隔的交付
// 不置 valid（caller 继续全口径 fallback），字节留在 m_out 与后续窗口
// 合并，速率永远除真实窗口而不是 tick 假 dt。核型样本同一班车（§4.4：
// 两通道窗口状态天然共享，占比分子分母同窗自洽）。
// ============================================================
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>

struct CoreClassMap;   // core_class_mapper.h（弱引用，Start 前注入）

enum class FileIoGrade { A, B, C, Off };

class EtwFileCollector {
public:
    // IoDelta: 内核会话增量（FileIo 字节 + 核型样本），自上次被接管的
    // TakeDelta 起累计，外加会话累计 EventsLost（低频查询）。
    struct IoDelta {
        std::unordered_map<DWORD, std::pair<uint64_t, uint64_t>> per_pid; // pid -> (read,write)
        // ---- 核型样本通道（方案 §4.4，与 IO 同一窗口、同一 handoff 序列）----
        std::unordered_map<DWORD, std::pair<uint64_t, uint64_t>> sample_pid; // pid -> (P样本,E样本)
        uint64_t sample_total = 0;      // 全部样本（校准分母，含 Idle/System/unknown）
        bool sample_valid = false;      // 异构且交付窗口完整才置 true
        uint64_t events_lost = 0;
        // 本次交付覆盖的真实窗口长度（上次被消费的 handoff → 本次 handoff，
        // µs；方案三窗口记账）——Build 的速率分母，不再除 tick 假 dt
        uint64_t window_us = 0;
        bool valid = false;    // false: consumer had nothing to hand over yet,
                               // or the window was incomplete (< min_window_us)
    };

    EtwFileCollector() = default;
    ~EtwFileCollector();

    // Attempt A (Win8+) or B (Win7). Returns the achieved grade; never
    // blocks the caller for long (consumer thread does the session work).
    FileIoGrade Start();
    void Stop();
    // DllMain PROCESS_DETACH 专用「尽力而为停」（A4，2026-11-08 审查）：
    // ExitProcess 先杀其它线程再进 DETACH——消费者/reaper 若死在持
    // m_status_mutex 的半路，完整 Stop() 的 SetStatus/join 会在 DllMain
    // 里永久等一把被弃的锁（TM 退出挂死）或被 3s 等待拖住。此路径只
    // 发停信号 + 解锁 ProcessTrace（停会话/CloseTrace），线程对象 detach
    // 后随进程退出整体回收，全程零锁零等待
    void StopForUnload();
    // Async stop (tick thread, 问题①B)：只发停信号 + 解锁 ProcessTrace，
    // join/收尾交给一次性收尸线程——tick 线程绝不同步等 3s（旧 Stop 的
    // join 会卡住 m_refresh_busy 门闩、全部维度数据停更）。m_running 仍由
    // ConsumerLoop 末尾置 false（语义 = 消费者已退出），TakeDelta 的
    // IsRunning 判定天然兼容。下一次 Start() 开头串行化等收尸完成。
    void StopAsync();
    bool IsRunning() const { return m_running.load(); }
    FileIoGrade Grade() const;

    // Human-readable one-line status for the options dialog / status row.
    // Thread-safe.
    void GetStatusText(wchar_t* out, size_t cap) const;

    // Tick thread. Bounded wait (~100ms worst case); valid=false when the
    // consumer handed nothing over OR the handed window is shorter than
    // min_window_us (incomplete window — caller falls back to the all-handle
    // counters; the bytes are NOT lost, they stay accrued for the next
    // handoff. 方案三「完整窗口才接管」).
    void TakeDelta(IoDelta& out, uint64_t min_window_us);

    // Debug-log master switch (default OFF). etw_file.log generation only.
    static void SetDebugLogs(bool on) { s_debug_logs.store(on); }
    static bool DebugLogs() { return s_debug_logs.load(); }
    static std::atomic<bool> s_debug_logs;

    // ---- 核型样本通道（方案 §4）----
    // 分类表注入（Start 前一次；nullptr/同构 = 通道静默，仅 FileIo）
    void SetCoreClassMap(const CoreClassMap* ccm) { m_ccm = ccm; }
    // tid→pid 表供给（tick 线程每拍，metrics_collector 解析 SPI 线程数组）。
    // 消费者线程在 dirty 标记下低频换表（~500ms 一次，mutex 瞬持）
    using ThreadTable = std::unordered_map<DWORD, DWORD>;
    void UpdateThreadTable(std::shared_ptr<const ThreadTable> tbl);

private:
    struct Acc {
        uint64_t read = 0;
        uint64_t write = 0;
    };
    struct SampleAcc {
        uint64_t p = 0;    // P/VCache 群样本
        uint64_t e = 0;    // E/StdCcd 群样本
    };

    void ConsumerLoop();
    static void WINAPI EventCallback(PEVENT_RECORD rec);
    static ULONG WINAPI BufferCallback(PEVENT_TRACE_LOGFILEW logfile);
    void OnEvent(PEVENT_RECORD rec);
    // move accumulator -> out map; bump handoff sequence. consumer thread only.
    void DoHandoffLocked();
    void SetStatus(FileIoGrade g, const wchar_t* why);
    void LogLine(const wchar_t* fmt, ...);
    void QueryEventsLost();
    void OnSampleOrSwitch(PEVENT_RECORD rec);   // 核型样本/CSwitch 通道（§4.2）

    static EtwFileCollector* s_self;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::thread m_reaper;    // StopAsync 的一次性收尸线程：join 消费者

    // ---- handoff protocol (see header comment) ----
    std::atomic<bool> m_take_req{false};
    std::atomic<uint64_t> m_take_seq{0};
    std::unordered_map<DWORD, Acc> m_acc;      // consumer thread private
    std::unordered_map<DWORD, Acc> m_out;      // handed off; tick reads after seq bump
    // ---- 核型样本通道（§4.4：与 IO 同一班车，同一窗口记账）----
    std::unordered_map<DWORD, SampleAcc> m_acc_sample;   // consumer private
    std::unordered_map<DWORD, SampleAcc> m_out_sample;   // 累计式 handed-off
    uint64_t m_sample_total_acc = 0;    // 消费者私有：窗口内全部样本（含 Idle）
    uint64_t m_sample_total_out = 0;    // 累计式（窗口不完整时跨窗合并）
    // 窗口记账（方案三）：m_out 改累计式——tick 不消费（超时/窗口不完整）
    // 时字节与窗口都留在 m_out 继续累计，跨短窗口合并成完整窗口交付，
    // 字节永不断账。m_out_window_us 由 consumer +=、tick 消费时清零
    //（并发安全同 m_out：tick 只在 seq bump 后读，读期间无新 handoff）
    uint64_t m_out_window_us = 0;
    int64_t m_window_open_qpc = 0;   // 当前窗口起点 QPC（consumer 线程私有；
                                     // 0 = 无窗口，首次 handoff 从会话启动起算）
    int64_t m_qpc_freq = 0;          // QPC 频率（Start 时取一次）

    // ---- grade status (written on consumer thread, read anywhere) ----
    mutable std::mutex m_status_mutex;
    FileIoGrade m_grade = FileIoGrade::Off;
    std::wstring m_grade_reason;

    // session bookkeeping
    std::atomic<TRACEHANDLE> m_cur_consumer{INVALID_PROCESSTRACE_HANDLE};
    bool m_own_session = false;         // consumer thread
    TRACEHANDLE m_own_session_handle = 0;
    bool m_using_kernel_logger = false; // B grade: NT Kernel Logger
    uint64_t m_events_lost = 0;
    uint64_t m_events_lost_last_query = 0;
    ULONGLONG m_last_lost_query = 0;

    // payload pointer width: kernel bitness, resolved once at Start()
    int m_ptr_size = 8;

    // ---- 核型样本通道（§4.2/§4.4）----
    const CoreClassMap* m_ccm = nullptr;   // Start 前注入（弱引用）
    std::vector<uint32_t> m_cur_tid;       // 每核 current_tid（消费者私有，
                                           // CSwitch 重建态；会话重建时清零）
    // tid→pid 表：tick 写 / 消费者低频换入（dirty + mutex 瞬持，热路径零锁）
    std::mutex m_table_mutex;
    std::shared_ptr<const ThreadTable> m_table_new;    // tick 最新供给
    std::shared_ptr<const ThreadTable> m_table_local;  // 消费者局部持有
    std::atomic<bool> m_table_dirty{false};

    // diagnostics
    uint64_t m_ev_seen = 0, m_ev_kept = 0, m_ev_short = 0;
    bool m_log_was_valid = false;   // 上次 TakeDelta 的接管态（转换点日志用，
                                    // tick 线程私有）；会话重启时重置
};
