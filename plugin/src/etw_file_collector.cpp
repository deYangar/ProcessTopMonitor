// etw_file_collector.cpp — FileIo ETW session lifecycle + manual parsing
#include "etw_file_collector.h"
#include "utils.h"
#include <tdh.h>
#include <cstdarg>
#include <cstdio>
#include <cstdarg>

EtwFileCollector* EtwFileCollector::s_self = nullptr;
std::atomic<bool> EtwFileCollector::s_debug_logs{false};

// Kernel-session StartTrace requires SeSystemProfilePrivilege — admin-group
// members hold it DISABLED by default; it must be enabled explicitly
// (logman/xperf do the same). No-op for non-admin callers (fails, and the
// grade machine reports C honestly).
static bool PtEnableSystemProfilePrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;
    BOOL ok = FALSE;
    LUID luid;
    if (LookupPrivilegeValueW(nullptr, SE_SYSTEM_PROFILE_NAME, &luid)) {
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr) &&
             GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(tok);
    return ok != FALSE;
}

// FileIo provider GUID (classic kernel group) — MSDN "FileIo" class docs
static const GUID kFileIoGuid =
    { 0x90cbdc39, 0x4a3e, 0x11d1, { 0x84, 0xf4, 0x00, 0x00, 0xf8, 0x04, 0x64, 0xe3 } };

// FileIo Read/Write opcode — measured on Win11 26300: all bulk file
// read/write events arrive as opcode 76 (0x4C), krabsetw
// KernelEventTypes::FileIoReadWrite; the MSDN table values 67/68 are not
// emitted by this kernel. Direction is distinguished by payload length.
static const uint8_t kFileEvtRead = 67;    // MSDN FileIo table: "File read"
static const uint8_t kFileEvtWrite = 68;   // MSDN FileIo table: "File write"

// Session name for grade A. Independent from NT Kernel Logger so PNM's
// session is never touched.
static const wchar_t* kSessionName = L"PTMFileIoSession";

EtwFileCollector::~EtwFileCollector() { Stop(); }

void EtwFileCollector::LogLine(const wchar_t* fmt, ...) {
    if (!s_debug_logs.load()) return;
    wchar_t dir[MAX_PATH] = L"";
    if (!PTM_GetDebugDir(dir, MAX_PATH)) return;
    wchar_t path[MAX_PATH];
    if (swprintf_s(path, MAX_PATH, L"%s\\etw_file.log", dir) <= 0) return;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t line[512];
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf_s(line, 512, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) {
        DWORD w = 0;
        WriteFile(h, line, (DWORD)wcslen(line) * sizeof(wchar_t), &w, nullptr);
        WriteFile(h, L"\r\n", 4, &w, nullptr);
    }
    CloseHandle(h);
}

void EtwFileCollector::SetStatus(FileIoGrade g, const wchar_t* why) {
    std::lock_guard<std::mutex> lk(m_status_mutex);
    m_grade = g;
    m_grade_reason = why ? why : L"";
}

FileIoGrade EtwFileCollector::Grade() const {
    std::lock_guard<std::mutex> lk(m_status_mutex);
    return m_grade;
}

void EtwFileCollector::GetStatusText(wchar_t* out, size_t cap) const {
    if (!out || cap == 0) return;
    FileIoGrade g;
    std::wstring reason;
    {
        std::lock_guard<std::mutex> lk(m_status_mutex);
        g = m_grade;
        reason = m_grade_reason;
    }
    switch (g) {
    case FileIoGrade::A:
        wcsncpy_s(out, cap, L"A", _TRUNCATE); break;
    case FileIoGrade::B:
        wcsncpy_s(out, cap, L"B", _TRUNCATE); break;
    case FileIoGrade::C:
        wcsncpy_s(out, cap, L"C", _TRUNCATE); break;
    default:
        wcsncpy_s(out, cap, L"off", _TRUNCATE); break;
    }
    if (!reason.empty() && wcslen(out) + 2 < cap) {
        wcscat_s(out, cap, L": ");
        wcscat_s(out, cap, reason.c_str());
    }
}

// Stop a kernel session. The properties buffer must be large enough to
// carry the session name (Wnode.BufferSize covers struct + name) — a bare
// sizeof(EVENT_TRACE_PROPERTIES) fails with ERROR_MORE_DATA (234) and leaks
// the session (measured 2026-10-25). handle!=0 stops by handle, else by name.
static ULONG PtStopSession(TRACEHANDLE handle, const wchar_t* name) {
    size_t name_bytes = ((name ? wcslen(name) : 0) + 1) * sizeof(wchar_t);
    std::vector<BYTE> buf(sizeof(EVENT_TRACE_PROPERTIES) + name_bytes, 0);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    p->Wnode.BufferSize = (ULONG)buf.size();
    if (handle)
        return ControlTraceW(handle, nullptr, p, EVENT_TRACE_CONTROL_STOP);
    return ControlTraceW(0, name, p, EVENT_TRACE_CONTROL_STOP);
}

FileIoGrade EtwFileCollector::Start() {
    // 串行化点（问题①B）：上一次 StopAsync 的收尾未完成时，先等收尸
    // 线程 join 掉旧消费者（m_running 随之翻 false），再重启——否则旧
    // 消费者还活着，下面的幂等检查会误判「正在运行」直接返回旧档位。
    // 迟停机制下此路径只在 3 分钟空闲真停后再启用 IO 窗时出现，罕见；
    // Start 本身允许 ~1s 的档位等待（现状），此处最多再叠收尸剩余时间
    if (m_reaper.joinable()) {
        try { m_reaper.join(); } catch (...) { m_reaper.detach(); }
    }
    if (m_running.load()) return Grade();
    s_self = this;
    m_stop = false;

    // payload pointer width = kernel bitness (kernel sessions are not
    // wow64-translated — an x86 plugin on x64 sees 8-byte pointers)
    m_ptr_size = 8;
    BOOL wow = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow) && wow) m_ptr_size = 8;
    else {
#ifdef _WIN64
        m_ptr_size = 8;
#else
        m_ptr_size = 4;
#endif
    }

    // grade intent is decided by OS build (A needs the multi-system-logger
    // feature, Win8+); actual outcome is decided inside ConsumerLoop
    SetStatus(FileIoGrade::Off, L"");
    try {
        m_thread = std::thread([this] { ConsumerLoop(); });
    } catch (...) {
        return FileIoGrade::C;
    }
    // Wait (bounded) until the consumer thread reports its first grade so
    // callers see a truthful state right after Start() returns.
    for (int i = 0; i < 50 && Grade() == FileIoGrade::Off; i++) Sleep(20);
    return Grade();
}

void EtwFileCollector::Stop() {
    // 完整停（析构/DLL 卸载/选项对话框）：同步等待消费者退出——此处
    // 允许阻塞。tick 线程的按需停走 StopAsync（问题①B）
    if (!m_running.load() && !m_thread.joinable() && !m_reaper.joinable()) {
        SetStatus(FileIoGrade::Off, L"");
        return;
    }
    m_stop = true;

    // Stopping our own session unblocks ProcessTrace. For an attach-style
    // consumer, CloseTrace is the unblocker (same as PNM etw_capture).
    if (m_own_session_handle) {
        PtStopSession(m_own_session_handle, nullptr);
    } else {
        TRACEHANDLE h = m_cur_consumer.load();
        if (h != INVALID_PROCESSTRACE_HANDLE) CloseTrace(h);
    }
    // 收尸协调（并发 join 同一 std::thread 是 UB）：reaper 在就等它完成
    // （它已把 m_thread join 掉）；没有 reaper 才由本线程直接等+join
    if (m_reaper.joinable()) {
        try { m_reaper.join(); } catch (...) { m_reaper.detach(); }
    } else if (m_thread.joinable()) {
        for (int i = 0; i < 30 && m_running.load(); i++) Sleep(100);
        if (m_thread.joinable()) {
            try { m_thread.join(); } catch (...) { m_thread.detach(); }
        }
    }
    m_running = false;
    SetStatus(FileIoGrade::Off, L"");
}

void EtwFileCollector::StopAsync() {
    // tick 线程的按需停（问题①B）：绝不 join——发停信号 + 解锁
    // ProcessTrace 即返回，join/收尾交给一次性收尸线程。重复调用无害
    // （m_stop 已置位时直接返回，不重叠收尸）
    if (m_stop.load()) return;
    if (!m_running.load() && !m_thread.joinable()) {
        SetStatus(FileIoGrade::Off, L"");
        return;
    }
    m_stop = true;

    // 解锁路径与 Stop() 相同（停止自己的会话 / CloseTrace attach 消费者）
    if (m_own_session_handle) {
        PtStopSession(m_own_session_handle, nullptr);
    } else {
        TRACEHANDLE h = m_cur_consumer.load();
        if (h != INVALID_PROCESSTRACE_HANDLE) CloseTrace(h);
    }

    // 一次性收尸线程：等消费者退出（m_running 由 ConsumerLoop 末尾置
    // false——语义 = 消费者已退出）再 join。Stop()/Start() 里的串行化
    // 点都先 join 本线程，保证对 m_thread 的 join 永远只有一个执行者。
    // 创建失败（资源枯竭）退化为「不 join」：消费者仍会自行退出，
    // 析构 Stop() 的兜底分支会完成等待
    try {
        m_reaper = std::thread([this] {
            for (int i = 0; i < 30 && m_running.load(); i++) Sleep(100);
            if (m_thread.joinable()) {
                try { m_thread.join(); } catch (...) { m_thread.detach(); }
            }
        });
    } catch (...) {
    }
}

void EtwFileCollector::ConsumerLoop() {
    m_running = true;
    bool win8_plus = PTM_IsWin8Plus();
    // PTM_FORCE_KERNEL=1: skip the SYSTEM_LOGGER_MODE session and use the
    // classic single NT Kernel Logger (grade B path) — isolation testing
    // for session-stability issues; harmless if unset in production.
    wchar_t force_b[8] = L"";
    if (GetEnvironmentVariableW(L"PTM_FORCE_KERNEL", force_b, 8) && force_b[0] == L'1')
        win8_plus = false;
    bool can_kernel_session = PtEnableSystemProfilePrivilege();   // one-shot, consumer thread

    while (!m_stop.load()) {
        TRACEHANDLE consumer = INVALID_PROCESSTRACE_HANDLE;
        bool own_session = false;
        TRACEHANDLE session_handle = 0;
        bool use_kernel_logger = false;

        const wchar_t* logger_name = kSessionName;
        DWORD log_file_mode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;

        if (win8_plus) {
            // grade A attempt: private system logger session
        } else {
            // grade B attempt: the one and only NT Kernel Logger
            logger_name = KERNEL_LOGGER_NAMEW;
            log_file_mode = EVENT_TRACE_REAL_TIME_MODE;
            use_kernel_logger = true;
        }

        EVENT_TRACE_LOGFILEW lf;
        memset(&lf, 0, sizeof(lf));
        lf.LoggerName = const_cast<LPWSTR>(logger_name);
        lf.ProcessTraceMode = EVENT_TRACE_REAL_TIME_MODE | PROCESS_TRACE_MODE_EVENT_RECORD;
        lf.EventRecordCallback = &EtwFileCollector::EventCallback;
        lf.BufferCallback = &EtwFileCollector::BufferCallback;

        DWORD name_bytes = (DWORD)((wcslen(logger_name) + 1) * sizeof(wchar_t));
        DWORD buf_size = sizeof(EVENT_TRACE_PROPERTIES) + name_bytes;
        std::vector<BYTE> buf(buf_size, 0);
        auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
        props->Wnode.BufferSize = buf_size;
        props->Wnode.ClientContext = 1;          // QPC timestamps
        props->BufferSize = 256;                 // 256KB buffers (PNM-proven)
        props->MinimumBuffers = 64;
        props->MaximumBuffers = 120;
        props->FlushTimer = 1;
        props->LogFileMode = log_file_mode;
        props->EnableFlags = EVENT_TRACE_FLAG_DISK_FILE_IO | EVENT_TRACE_FLAG_FILE_IO | EVENT_TRACE_FLAG_FILE_IO_INIT;

        TRACEHANDLE session = 0;
        ULONG sr = StartTraceW(&session, logger_name, props);
        if (sr == ERROR_SUCCESS) {
            own_session = true;
            session_handle = session;
            SetStatus(use_kernel_logger ? FileIoGrade::B : FileIoGrade::A, L"");
            LogLine(L"StartTrace OK (%s, maxbuf=%u)",
                    use_kernel_logger ? L"B:NT Kernel Logger" : L"A:PTMFileIoSession",
                    props->MaximumBuffers);
            consumer = OpenTraceW(&lf);
            if (consumer == INVALID_PROCESSTRACE_HANDLE) {
                PtStopSession(session, nullptr);
                own_session = false;
                session_handle = 0;
                SetStatus(FileIoGrade::C, L"OpenTrace failed");
                LogLine(L"OpenTrace FAILED err=%lu", GetLastError());
            }
        } else if (sr == ERROR_ALREADY_EXISTS && use_kernel_logger) {
            // Win7 + someone owns NT Kernel Logger (e.g. PNM's ETW). Do NOT
            // stop-and-takeover: two sibling plugins fighting over the single
            // kernel session endlessly reconnect against each other. C-grade
            // with the honest reason (技术方案 §2.3 档位 B 行).
            SetStatus(FileIoGrade::C, L"NT Kernel Logger busy");
            LogLine(L"NT Kernel Logger busy (grade B unavailable)");
        } else if (sr == ERROR_ACCESS_DENIED) {
            SetStatus(FileIoGrade::C, L"need admin");
            LogLine(L"StartTrace ACCESS_DENIED (grade C)");
        } else if (sr == ERROR_ALREADY_EXISTS) {
            // A-grade session name collision: stale leftover from a previous
            // crash of ours. Safe to stop (it's OUR name) and retry once.
            // Note: newer Windows also reports ALREADY_EXISTS to a NON-elevated
            // StartTrace (do not leak session existence) — if the stop attempt
            // is denied, the real reason is missing admin rights.
            ULONG st = PtStopSession(0, kSessionName);
            LogLine(L"session exists, stop rc=%lu", st);
            if (st == ERROR_SUCCESS) {
                sr = StartTraceW(&session, logger_name, props);
                if (sr == ERROR_SUCCESS) {
                    own_session = true;
                    session_handle = session;
                    SetStatus(FileIoGrade::A, L"");
                    consumer = OpenTraceW(&lf);
                    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
                        PtStopSession(session, nullptr);
                        own_session = false;
                        session_handle = 0;
                        SetStatus(FileIoGrade::C, L"OpenTrace failed");
                    }
                }
            }
            if (!own_session && consumer == INVALID_PROCESSTRACE_HANDLE) {
                if (st != ERROR_SUCCESS)
                    SetStatus(FileIoGrade::C, L"need admin");   // stop denied = no rights, not a real collision
                else
                    SetStatus(FileIoGrade::C, L"session collision");
                LogLine(L"session takeover failed err=%lu", sr);
            }
        } else {
            SetStatus(FileIoGrade::C, L"StartTrace failed");
            LogLine(L"StartTrace failed err=%lu", sr);
        }

        if (consumer == INVALID_PROCESSTRACE_HANDLE) {
            // C grade: stay alive so a later Start() (after TM restarts
            // elevated) can retry — recheck every 10s (运行期档位重协商)
            if (m_stop.load()) break;
            for (int i = 0; i < 100 && !m_stop.load(); i++) Sleep(100);
            if (!m_stop.load() && Grade() == FileIoGrade::C) {
                // retry the session attempt (A/B) once per wait cycle
                continue;
            }
            if (m_stop.load()) break;
            continue;
        }

        m_own_session = own_session;
        m_own_session_handle = session_handle;
        m_using_kernel_logger = use_kernel_logger;
        m_cur_consumer = consumer;
        m_events_lost = 0;
        m_events_lost_last_query = 0;

        ULONG rc = ProcessTrace(&consumer, 1, nullptr, nullptr);
        m_cur_consumer = INVALID_PROCESSTRACE_HANDLE;
        m_own_session = false;
        CloseTrace(consumer);

        if (own_session && session_handle)
            PtStopSession(session_handle, nullptr);
        m_own_session_handle = 0;
        LogLine(L"ProcessTrace rc=%lu, reconnecting in 2s", rc);
        SetStatus(FileIoGrade::C, L"session ended");
        // 2s 重连等待按 100ms 步进查询 m_stop（M6 问题⑤治本）：Stop() 响应
        // 2000ms→≤100ms，TM 退出收尾不再被裸睡拖到最坏 5s（写法同上方 C 档循环）
        if (m_stop.load()) break;
        for (int i = 0; i < 20 && !m_stop.load(); i++) Sleep(100);
        if (m_stop.load()) break;
    }
    m_running = false;
}

void EtwFileCollector::DoHandoffLocked() {
    // consumer thread only. m_acc -> m_out swap (O(1)); the previous out
    // content was already consumed by the tick thread's move.
    m_out.swap(m_acc);
    m_acc.clear();   // moved-in old out: ensure clean
    m_take_req.store(false, std::memory_order_release);
    m_take_seq.fetch_add(1, std::memory_order_release);
}

void WINAPI EtwFileCollector::EventCallback(PEVENT_RECORD rec) {
    if (s_self) s_self->OnEvent(rec);
}

void EtwFileCollector::OnEvent(PEVENT_RECORD rec) {
    // handoff check: one relaxed load on the hot path (~1ns)
    if (m_take_req.load(std::memory_order_relaxed)) DoHandoffLocked();
    m_ev_seen++;

    const auto& h = rec->EventHeader;
    // ProviderId IS FileIoGuid for these kernel events (measured with the
    // diag histogram: 100% of bulk events carry 90cbdc39). Session enables
    // FILE_IO alone, so this is also the foreign-session bleed-through guard.
    if (memcmp(&h.ProviderId, &kFileIoGuid, sizeof(GUID)) != 0) {
        m_ev_short++;
        return;
    }

    unsigned op = h.EventDescriptor.Opcode;
    // Definitive opcode/payload map (Win11 26300, TDH-verified 2026-10-25,
    // full flag set DISK_FILE_IO|FILE_IO|FILE_IO_INIT):
    //   op67 = FileIo/Read, op68 = FileIo/Write — the MSDN table is correct;
    //   the earlier "rare stragglers" reading came from sessions whose
    //   EnableFlags lacked EVENT_TRACE_FLAG_DISK_FILE_IO (metadata-only).
    //   op64/65/66 are Create/Cleanup/Close metadata; op76 is the per-IO
    //   OpEnd status event; none of them carry transfer bytes.
    if (op != kFileEvtRead && op != kFileEvtWrite)
        return;

    // FileIo Read/Write payload (TDH EVENT_PROPERTY_INFO array, ver=3):
    //   Offset[8] IrpPtr[ptr] FileObject[ptr] FileKey[ptr] TTID[4]
    //   IoSize[4] IoFlags[4] — pointer width = kernel bitness, so IoSize
    //   sits at 8 + 3*ptr + 4 (36 on x64 kernels, 24 on x86).
    const UINT32 ud_off = (UINT32)(8 + 3 * m_ptr_size + 4);
    USHORT ulen = rec->UserDataLength;
    if (ulen < ud_off + 4) { m_ev_short++; return; }

    uint32_t size;
    memcpy(&size, (BYTE*)rec->UserData + ud_off, 4);

    DWORD pid = h.ProcessId;
    auto& a = m_acc[pid];
    if (op == kFileEvtRead) a.read += size;
    else a.write += size;
    m_ev_kept++;
}

ULONG WINAPI EtwFileCollector::BufferCallback(PEVENT_TRACE_LOGFILEW logfile) {
    // Called when buffers flush (FlushTimer=1s) even with zero events —
    // the idle-path handoff point so the tick baseline stays fresh and
    // idle->active deltas are not diluted (see header comment).
    // MUST return TRUE: FALSE tells ETW to stop consuming (ProcessTrace
    // returns) — measured 2026-10-25: returning 0 killed the session at
    // every flush tick (A<->C flapping, "ProcessTrace rc=0" every ~1-2s).
    (void)logfile;
    if (s_self && s_self->m_take_req.load(std::memory_order_relaxed))
        s_self->DoHandoffLocked();
    return 1;   // TRUE — keep consuming
}

void EtwFileCollector::QueryEventsLost() {
    // low-rate ControlTrace QUERY for the cumulative EventsLost counter
    if (!m_own_session_handle && !m_using_kernel_logger) return;
    ULONGLONG now = GetTickCount64();
    if (m_last_lost_query && now - m_last_lost_query < 5000) return;
    m_last_lost_query = now;
    BYTE qbuf[sizeof(EVENT_TRACE_PROPERTIES) + 64] = {};
    auto* p = (EVENT_TRACE_PROPERTIES*)qbuf;
    p->Wnode.BufferSize = sizeof(qbuf);
    ULONG rc;
    if (m_own_session_handle)
        rc = ControlTraceW(m_own_session_handle, nullptr, p, EVENT_TRACE_CONTROL_QUERY);
    else
        rc = ControlTraceW(0, KERNEL_LOGGER_NAMEW, p, EVENT_TRACE_CONTROL_QUERY);
    if (rc == ERROR_SUCCESS) m_events_lost = p->EventsLost;
}

void EtwFileCollector::TakeDelta(IoDelta& out) {
    out.per_pid.clear();
    out.valid = false;
    if (!m_running.load()) return;

    uint64_t seq0 = m_take_seq.load(std::memory_order_acquire);
    m_take_req.store(true, std::memory_order_release);

    // bounded wait for the consumer's handoff (event or buffer callback).
    // ~100ms worst case; the tick cadence is >=250ms so this never overlaps.
    for (int i = 0; i < 100; i++) {
        if (m_take_seq.load(std::memory_order_acquire) != seq0) {
            out.per_pid.reserve(m_out.size());
            for (auto& kv : m_out) {
                if (kv.second.read || kv.second.write)
                    out.per_pid[kv.first] = { kv.second.read, kv.second.write };
            }
            m_out.clear();
            out.valid = true;
            QueryEventsLost();
            out.events_lost = m_events_lost;
            return;
        }
        Sleep(1);
    }
    // timeout: consumer silent (idle system, buffer callback not yet fired).
    // Do NOT read m_out (the handoff may still land concurrently) — caller
    // keeps its previous rates; the pending request is served next tick.
}
