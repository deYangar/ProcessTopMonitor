#pragma once
// ============================================================
// etw_file_collector — exact file read/write bytes per process
// (技术方案 §2.3 主线，M1 已拍板首版即 ETW 精确口径)
//
// Grade state machine:
//   A (Win8+):  own EVENT_TRACE_SYSTEM_LOGGER_MODE session
//               "PTMFileIoSession" with EVENT_TRACE_FLAG_FILE_IO only —
//               coexists with PNM's NT Kernel Logger (multiple system
//               logger sessions are a Win8+ feature).
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
// Handoff: the ProcessTrace callback thread owns a private accumulator map
// (lock-free in the callback). The tick thread requests a handoff via an
// atomic flag; the consumer thread (event callback or the periodic buffer
// callback) moves the accumulator into the out map and bumps a sequence —
// single-writer handoff, zero locks on the hot path (技术方案 §5.1).
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

enum class FileIoGrade { A, B, C, Off };

class EtwFileCollector {
public:
    // IoDelta: per-pid exact file read/write BYTES accumulated since the
    // previous successful TakeDelta, plus the session's cumulative
    // EventsLost (queried at a low rate).
    struct IoDelta {
        std::unordered_map<DWORD, std::pair<uint64_t, uint64_t>> per_pid; // pid -> (read,write)
        uint64_t events_lost = 0;
        bool valid = false;    // false: consumer had nothing to hand over yet
    };

    EtwFileCollector() = default;
    ~EtwFileCollector();

    // Attempt A (Win8+) or B (Win7). Returns the achieved grade; never
    // blocks the caller for long (consumer thread does the session work).
    FileIoGrade Start();
    void Stop();
    bool IsRunning() const { return m_running.load(); }
    FileIoGrade Grade() const;

    // Human-readable one-line status for the options dialog / status row.
    // Thread-safe.
    void GetStatusText(wchar_t* out, size_t cap) const;

    // Tick thread. Bounded wait (~100ms worst case); returns false when the
    // consumer handed nothing over (keep last rates, do not zero them out).
    void TakeDelta(IoDelta& out);

    // Debug-log master switch (default OFF). etw_file.log generation only.
    static void SetDebugLogs(bool on) { s_debug_logs.store(on); }
    static bool DebugLogs() { return s_debug_logs.load(); }
    static std::atomic<bool> s_debug_logs;

private:
    struct Acc {
        uint64_t read = 0;
        uint64_t write = 0;
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

    static EtwFileCollector* s_self;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;

    // ---- handoff protocol (see header comment) ----
    std::atomic<bool> m_take_req{false};
    std::atomic<uint64_t> m_take_seq{0};
    std::unordered_map<DWORD, Acc> m_acc;      // consumer thread private
    std::unordered_map<DWORD, Acc> m_out;      // handed off; tick reads after seq bump

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

    // diagnostics
    uint64_t m_ev_seen = 0, m_ev_kept = 0, m_ev_short = 0;
    ULONGLONG m_last_log_tick = 0;
};
