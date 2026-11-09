// etw_file_collector.cpp — 内核会话生命周期 + 手工解析（FileIo + 核型样本）
#include "etw_file_collector.h"
#include "core_class_mapper.h"
#include "utils.h"
#include <tdh.h>
#include <cstdarg>
#include <cstdio>

EtwFileCollector* EtwFileCollector::s_self = nullptr;
std::atomic<bool> EtwFileCollector::s_debug_logs{false};

// defined in top_list_window.cpp (DLL shutdown flag, set in DllMain first)
extern volatile bool g_shutting_down;

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

// ---- 核型样本通道事件识别（方案 §4.2，本机实测口径 2026-11-09）----
// SampledProfile：Win11 26300 上 PerfInfo 事件 manifest 化，provider 换为
// ce1dbfb4…（文档版 3d6fa8d1 一条未出现；老系统仍按文档 GUID 兜底识别）
static const GUID kPerfInfoNewGuid =
    { 0xce1dbfb4, 0x137e, 0x4da6, { 0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc } };
// CSwitch 仍走文档版 PerfInfo GUID（WPR ETL 实测 3d6fa8d1 op36，v5 探针）
static const GUID kPerfInfoOldGuid =
    { 0x3d6fa8d1, 0xfe05, 0x11d0, { 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c } };
static const uint8_t kOpSampleProfile = 46;   // PERFINFO_LOG_TYPE_SAMPLE_INTERRUPT
static const uint8_t kOpCSwitch = 36;         // PERFINFO_LOG_TYPE_CSWITCH

// FileIo Read/Write opcode — measured on Win11 26300: all bulk file
// read/write events arrive as opcode 76 (0x4C), krabsetw
// KernelEventTypes::FileIoReadWrite; the MSDN table values 67/68 are not
// emitted by this kernel. Direction is distinguished by payload length.
static const uint8_t kFileEvtRead = 67;    // MSDN FileIo table: "File read"
static const uint8_t kFileEvtWrite = 68;   // MSDN FileIo table: "File write"

// Session name for grade A. Independent from NT Kernel Logger so PNM's
// session is never touched.
static const wchar_t* kSessionName = L"PTMFileIoSession";

// ---- CSwitch 的 groupmask 开关（探针 v7-v10 定案，2026-11-09）----
// 经典 EnableFlags 没有 CSwitch 位（DISPATCHER=ReadyThread 而非 CSwitch，
// v7 四形态矩阵实证）；WPR/krabsetw 走 PERFINFO_GROUPMASK：
// StartTrace 后 NtQuerySystemInformation(SystemPerformanceTraceInformation)
// 读回本会话 groupmask → OR PERF_CONTEXT_SWITCH(0x20000004, Masks[1]) →
// NtSetSystemInformation 写回（krabsetw kt.hpp enable_providers 同款）。
typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* PFN_NtQSI2)(ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS(NTAPI* PFN_NtSSI)(ULONG, PVOID, ULONG);
static const ULONG kSysPerfTraceInfo = 0x1f;   // SystemPerformanceTraceInformation
static constexpr ULONG kPerfMaskIndex = 0xe0000000;
static constexpr size_t kPerfNumMasks = 8;
static constexpr ULONG kPerfContextSwitch = 0x20000004;   // PERF_CONTEXT_SWITCH

struct PtGroupMaskInfo {
    ULONG InformationClass;      // 1 = EventTraceGroupMaskInformation
    TRACEHANDLE TraceHandle;
    ULONG Masks[kPerfNumMasks];
};

// 会话建立后调（消费者线程，StartTrace 成功分支）。失败不致命：样本通道
// 无 CSwitch 流 → 归因恒 no-ctx → sample 列不可用（C 档语义，§6），IO 不受影响
static void PtEnableCSwitchGroupMask(TRACEHANDLE session) {
    auto ntqsi = (PFN_NtQSI2)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                   "NtQuerySystemInformation");
    auto ntssi = (PFN_NtSSI)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                  "NtSetSystemInformation");
    if (!ntqsi || !ntssi) return;   // 理论不发生（ntdll 永在）
    PtGroupMaskInfo gmi = {};
    gmi.InformationClass = 1;
    gmi.TraceHandle = session;
    // 读回：把 EnableFlags 已生效的组带出来（探针 v10 实测形态：
    // Masks[0]=01000800 Masks[1]=00000202——只 OR 不覆盖，别组不动）
    if (ntqsi(kSysPerfTraceInfo, &gmi, sizeof(gmi), nullptr) != 0) return;
    ULONG idx = (kPerfContextSwitch & kPerfMaskIndex) >> 29;
    gmi.Masks[idx] |= (kPerfContextSwitch & ~kPerfMaskIndex);
    ntssi(kSysPerfTraceInfo, &gmi, sizeof(gmi));
}

EtwFileCollector::~EtwFileCollector() {
    // A4：静态析构（DLL 卸载链）在 ExitProcess 杀线程之后跑——完整
    // Stop() 的 SetStatus/join 可能踩被弃的锁；卸载路径走尽力而为停
    if (g_shutting_down) StopForUnload();
    else Stop();
}

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
    // A4 加固：TryLock + 放弃——所有调用点都是「尽力更新档位状态」语义
    //（消费者线程下一轮循环会重写），持锁方忙/锁被弃时放弃本次更新，
    // 杜绝任何路径（含进程退出收尾）在这把锁上阻塞
    if (!m_status_mutex.try_lock()) return;
    m_grade = g;
    m_grade_reason = why ? why : L"";
    m_status_mutex.unlock();
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
    // A5（2026-11-08 审查）：reaper 创建失败的漏网路径——StopAsync 里
    // std::thread 构造抛出被 catch(...) 吞掉且消费者已自行退出
    //（m_running=false、m_thread 仍 joinable）时，下面 m_thread =
    // std::thread(...) 对 joinable 对象 move-assign = std::terminate 直接
    // 杀宿主。先 join 再赋值，三行治本
    if (m_thread.joinable()) {
        try { m_thread.join(); } catch (...) { m_thread.detach(); }
    }
    if (m_running.load()) return Grade();
    s_self = this;
    m_stop = false;
    // 窗口记账（方案三）：QPC 频率取一次；窗口起点由首个会话的
    // 「首拍就绪 handoff」推齐（见 ConsumerLoop）
    if (!m_qpc_freq) {
        LARGE_INTEGER f;
        if (QueryPerformanceFrequency(&f) && f.QuadPart > 0)
            m_qpc_freq = f.QuadPart;
        else
            m_qpc_freq = 1;   // 防御：QPC 不可用（实践中不发生），窗口退化为 0
    }
    m_window_open_qpc = 0;

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

void EtwFileCollector::StopForUnload() {
    // DllMain PROCESS_DETACH / 静态析构路径（A4）：零锁零 join。ExitProcess
    // 已终止其它线程——消费者/reaper 的执行体已死，joinable 的
    // std::thread 对象若原样析构会 std::terminate（joinable 析构=杀宿主），
    // 对已死线程 detach 立即返回、对象进入安全态。m_own_session_handle/
    // m_cur_consumer 是尽力读（消费者线程写，无同步）——读到哪个分支都
    // 只是「发一次停信号/关一次句柄」，进程退出语境下足够
    m_stop = true;
    if (m_own_session_handle) {
        PtStopSession(m_own_session_handle, nullptr);
    } else {
        TRACEHANDLE h = m_cur_consumer.load();
        if (h != INVALID_PROCESSTRACE_HANDLE) CloseTrace(h);
    }
    if (m_reaper.joinable()) m_reaper.detach();
    if (m_thread.joinable()) m_thread.detach();
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

    // C 档快重试配额（方案二两级退避）：落 C 后先 250ms×4（1s 内重试
    // 四次），仍未成功回 10s 慢档——「回到 A 档」的可见延迟从 10s+ 压到
    // ≤1.5s；need admin 这类真不可恢复场景行为不变（只是恢复探测更勤），
    // 会话成功重建后配额重置（下次落 C 重新快重试）
    int c_fast_left = 4;

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
        // FileIo 三件套 + 核型样本通道两件套（§4.1：flags 集中一处，A/B 档
        // 同一生效处）。DISPATCHER = CSwitch 流（核型归因的线程上下文重建
        // 数据源，探针 v6 实证口径）；PROFILE = SampledProfile 采样本身
        props->EnableFlags = EVENT_TRACE_FLAG_DISK_FILE_IO | EVENT_TRACE_FLAG_FILE_IO
                           | EVENT_TRACE_FLAG_FILE_IO_INIT
                           | EVENT_TRACE_FLAG_PROFILE | EVENT_TRACE_FLAG_DISPATCHER;

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
            // crash of ours. Safe to stop (it's OUR name) and retry.
            // ControlTrace STOP 是异步下线：旧会话 buffers flush 需要时间，
            // 二次 StartTrace 容易再撞 ALREADY_EXISTS（残留会话场景的主要
            // 形态，旧「只重试一次」撞上即落 C 档）——方案二：撞上睡 150ms
            // 等下线再试第三次，把残留场景在 Start() 内一次治好。
            // Note: newer Windows also reports ALREADY_EXISTS to a NON-elevated
            // StartTrace (do not leak session existence) — if the stop attempt
            // is denied, the real reason is missing admin rights.
            ULONG st = PtStopSession(0, kSessionName);
            LogLine(L"session exists, stop rc=%lu", st);
            if (st == ERROR_SUCCESS) {
                for (int attempt = 0; attempt < 2 && sr == ERROR_ALREADY_EXISTS; attempt++) {
                    if (attempt) Sleep(150);   // 第三次尝试前等异步下线
                    sr = StartTraceW(&session, logger_name, props);
                }
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
            // elevated) can retry（运行期档位重协商）——两级退避（方案二）
            if (m_stop.load()) break;
            if (c_fast_left > 0) {
                c_fast_left--;
                for (int i = 0; i < 25 && !m_stop.load(); i++) Sleep(10);   // 250ms 快档
            } else {
                for (int i = 0; i < 100 && !m_stop.load(); i++) Sleep(100); // 10s 慢档
            }
            if (m_stop.load()) break;
            continue;   // retry the session attempt (A/B)
        }

        m_own_session = own_session;
        m_own_session_handle = session_handle;
        m_using_kernel_logger = use_kernel_logger;
        m_cur_consumer = consumer;
        m_events_lost = 0;
        m_events_lost_last_query = 0;
        c_fast_left = 4;   // 会话成功重建：C 档快重试配额重置（方案二）
        m_log_was_valid = false;   // 新会话：接管转换日志从头记

        // CSwitch groupmask（探针 v10 定案）：仅 A 档设（Win8+；B 档 Win7
        // 无混架 CPU，核型本就同构隐藏）。失败静默——样本通道进入 no-ctx
        // 形态（列不可用），FileIo 不受影响
        if (win8_plus && session_handle)
            PtEnableCSwitchGroupMask(session_handle);

        // 首拍就绪信号（方案三第 3 条）：进 ProcessTrace 前主动 handoff 一次
        //（窗口=0，配合完整窗口规则天然不接管）——把 handoff 时钟起点推齐
        // 到「会话已就绪」时刻，否则起点要等第一批事件或 1s FlushTimer，
        // 首个完整窗口迟迟不闭合；同时服务 tick 可能挂着的 pending 请求
        //（valid=false → 该拍继续 fallback 全口径，数据连续不闪 0）。
        // 上个会话残留的未交付 acc 属旧窗口语义，随新会话起点一并作废
        //（核型样本同款：旧会话样本/重建态随新会话清零——cur_tid 重建态
        // 与旧会话的 CSwitch 流不再连续，保留只会错归因）
        m_acc.clear();
        m_acc_sample.clear();
        m_sample_total_acc = 0;
        if (m_ccm && m_ccm->heterogeneous && m_ccm->total_lp > 0) {
            m_cur_tid.assign((size_t)m_ccm->total_lp, 0);
            m_last_sw_t.assign((size_t)m_ccm->total_lp, 0);   // 0 = 无区间起点
        } else {
            m_cur_tid.clear();
            m_last_sw_t.clear();
        }
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            m_window_open_qpc = now.QuadPart;
        }
        LogLine(L"session ready, window clock aligned (ready handoff)");
        DoHandoffLocked();

        ULONG rc = ProcessTrace(&consumer, 1, nullptr, nullptr);
        m_cur_consumer = INVALID_PROCESSTRACE_HANDLE;
        m_own_session = false;
        CloseTrace(consumer);

        if (own_session && session_handle)
            PtStopSession(session_handle, nullptr);
        m_own_session_handle = 0;
        LogLine(L"ProcessTrace rc=%lu, reconnecting in 500ms", rc);
        SetStatus(FileIoGrade::C, L"session ended");
        // 重连窗 2s→500ms（方案二）：意外断流的恢复可见延迟减半再减半；
        // 等待按 100ms 步进查询 m_stop（M6 问题⑤治本）：Stop() 响应
        // ≤100ms，TM 退出收尾不被裸睡拖住
        if (m_stop.load()) break;
        for (int i = 0; i < 5 && !m_stop.load(); i++) Sleep(100);
        if (m_stop.load()) break;
    }
    m_running = false;
}

void EtwFileCollector::DoHandoffLocked() {
    // consumer thread only. 窗口记账（方案三）：每次 handoff 记 QPC，
    // window = 本次 handoff − 上次 handoff（首次 = 会话启动时刻起算，
    // m_window_open_qpc 由首拍就绪信号推齐）。m_out 是累计式：字节与窗口
    // 一起 += 进 m_out，tick 消费（TakeDelta 读走清零）前不清——「窗口不
    // 完整不接管」的字节跨窗口累计，永不断账（旧 swap 实现在 tick 超时
    // 后 handoff 迟到的场景会丢一窗数据）。空闲时 m_acc 为空、循环零成本
    if (m_window_open_qpc && m_qpc_freq > 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        int64_t delta = now.QuadPart - m_window_open_qpc;
        if (delta > 0)
            m_out_window_us += (uint64_t)(delta * 1000000 / m_qpc_freq);
        m_window_open_qpc = now.QuadPart;
    }
    if (!m_acc.empty()) {
        if (m_out.empty()) {
            m_out.swap(m_acc);          // 快路径：空 out 直接 swap（O(1)）
        } else {
            for (auto& kv : m_acc) {     // 累计路径：并入未消费的 out
                auto& o = m_out[kv.first];
                o.read += kv.second.read;
                o.write += kv.second.write;
            }
            m_acc.clear();
        }
    }
    // 核型样本同一班车（§4.4）：sample 通道与 IO 共用窗口状态，累计式
    // 合并语义完全一致——占比分子分母同步累计，跨窗口合并后依旧自洽
    if (!m_acc_sample.empty()) {
        if (m_out_sample.empty()) {
            m_out_sample.swap(m_acc_sample);
        } else {
            for (auto& kv : m_acc_sample) {
                auto& o = m_out_sample[kv.first];
                o.p += kv.second.p;
                o.e += kv.second.e;
            }
            m_acc_sample.clear();
        }
    }
    m_sample_total_out += m_sample_total_acc;
    m_sample_total_acc = 0;
    m_take_req.store(false, std::memory_order_release);
    m_take_seq.fetch_add(1, std::memory_order_release);
}

void EtwFileCollector::OnSampleOrSwitch(PEVENT_RECORD rec) {
    // consumer thread only（核型时间账通道，§4.2 探针定案 + 2026-11-09
    // 实测口径修正）。归因状态机沿 §4.2 定案：CSwitch 流重建每核
    // current_tid → tid→pid 表；计量从「SampledProfile 计数」改为
    // 「CSwitch 区间时间账」：switch(t, core, new_tid) 结算
    // [上次switch, t) 时长给旧 current_tid 对应 pid——调度器时间账
    // （NtQSI 同源），忙闲核无采样密度失真（实测样本计数口径下
    // pin_load 占比 95% vs 时钟口径 25%，闲核深 C-state timer 压缩）。
    const auto& h = rec->EventHeader;
    // SampledProfile（新 GUID op46）：不再计量（时间账由 CSwitch 全权），
    // 识别保留——flags 里的 PROFILE 事件到此静默丢弃（摘 flag 列为后续
    // 优化，避免本窗口动会话参数）
    if (h.EventDescriptor.Opcode == kOpSampleProfile) return;
    if (h.EventDescriptor.Opcode != kOpCSwitch
        || h.ProviderId.Data1 != kPerfInfoOldGuid.Data1)
        return;

    uint32_t core = rec->BufferContext.ProcessorIndex;
    // >255 LP：EVENT_HEADER_EXT_TYPE_PROCESSOR_INDEX 扩展项防御分支
    //（消费级远不及；UCHAR 越界即丢弃，§9 风险表）
    if (core >= m_cur_tid.size()) { m_ev_short++; return; }
    // CSwitch payload: NewThreadId[4] OldThreadId[4] …（TDH schema 固定头）
    if (rec->UserDataLength < 8) { m_ev_short++; return; }

    // 结算旧区间 [m_last_sw_t[core], t) → 旧 current_tid 的核群时间账
    uint64_t t = (uint64_t)h.TimeStamp.QuadPart;   // 100ns ticks，单调
    uint64_t last = m_last_sw_t[core];
    if (last && t > last) {
        uint64_t dt = t - last;
        m_sample_total_acc += dt;               // 全部核时间进分母（含 Idle/System/unknown）
        uint32_t tid = m_cur_tid[core];
        if (tid && tid != 0xFFFFFFFF) {
            // 低频换表（tick 每 ~500ms 供新表；dirty 检查 ~1ns/事件）
            if (m_table_dirty.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(m_table_mutex);
                m_table_local = m_table_new;
                m_table_dirty.store(false, std::memory_order_release);
            }
            if (m_table_local && core < m_ccm->lp.size()) {
                auto it = m_table_local->find(tid);
                if (it != m_table_local->end()) {
                    DWORD pid = it->second;
                    if (pid != 0 && pid != 4) {  // Idle/System：留分母不进 per-pid
                        switch (m_ccm->lp[core]) {
                        case CoreClass::P:
                        case CoreClass::VCache: m_acc_sample[pid].p += dt; break;
                        case CoreClass::E:
                        case CoreClass::StdCcd: m_acc_sample[pid].e += dt; break;
                        default: break;          // Homog/未知核：留分母
                        }
                    }
                }
                // unknown tid（新建线程）：时间留分母（≤1 tick 归因延迟的
                // 时间版——首个 switch 前的区间记不到新线程头上，量级同）
            }
        }
    }

    // 推进状态机：新区间从本事件起算，归属 new_tid
    uint32_t nt;
    memcpy(&nt, (BYTE*)rec->UserData, 4);
    m_cur_tid[core] = nt;   // 状态机自愈：old != current（丢包/中途起会话）时直接覆盖
    m_last_sw_t[core] = t;
}

void WINAPI EtwFileCollector::EventCallback(PEVENT_RECORD rec) {
    if (s_self) s_self->OnEvent(rec);
}

void EtwFileCollector::OnEvent(PEVENT_RECORD rec) {
    // handoff check: one relaxed load on the hot path (~1ns)
    if (m_take_req.load(std::memory_order_relaxed)) DoHandoffLocked();
    m_ev_seen++;

    const auto& h = rec->EventHeader;
    // ---- 核型样本通道（§4.2）：异构机器上 PROFILE|DISPATCHER 事件分流。
    // CSwitch 是最高频事件，先判（Data1 快筛 + memcmp 确认）
    if (m_ccm && m_ccm->heterogeneous && !m_cur_tid.empty()) {
        DWORD d1 = h.ProviderId.Data1;
        if ((d1 == kPerfInfoOldGuid.Data1
             && memcmp(&h.ProviderId, &kPerfInfoOldGuid, sizeof(GUID)) == 0)
            || (d1 == kPerfInfoNewGuid.Data1
                && memcmp(&h.ProviderId, &kPerfInfoNewGuid, sizeof(GUID)) == 0)) {
            OnSampleOrSwitch(rec);
            return;
        }
    }
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

void EtwFileCollector::UpdateThreadTable(std::shared_ptr<const ThreadTable> tbl) {
    // tick 线程（每拍）：换新表 + 置 dirty。消费者在下一事件回调的低频
    // 分支里换入（mutex 瞬持，~500ms 一次，热路径零锁）
    {
        std::lock_guard<std::mutex> lk(m_table_mutex);
        m_table_new = std::move(tbl);
    }
    m_table_dirty.store(true, std::memory_order_release);
}

void EtwFileCollector::TakeDelta(IoDelta& out, uint64_t min_window_us) {
    out.per_pid.clear();
    out.sample_pid.clear();
    out.valid = false;
    out.sample_valid = false;
    out.sample_total = 0;
    out.window_us = 0;
    if (!m_running.load()) return;

    uint64_t seq0 = m_take_seq.load(std::memory_order_acquire);
    m_take_req.store(true, std::memory_order_release);

    // bounded wait for the consumer's handoff (event or buffer callback).
    // ~100ms worst case; the tick cadence is >=250ms so this never overlaps.
    for (int i = 0; i < 100; i++) {
        if (m_take_seq.load(std::memory_order_acquire) != seq0) {
            out.window_us = m_out_window_us;
            out.per_pid.reserve(m_out.size());
            for (auto& kv : m_out) {
                if (kv.second.read || kv.second.write)
                    out.per_pid[kv.first] = { kv.second.read, kv.second.write };
            }
            // 核型样本通道同班车交付（§4.4）
            out.sample_total = m_sample_total_out;
            for (auto& kv : m_out_sample) {
                if (kv.second.p || kv.second.e)
                    out.sample_pid[kv.first] = { kv.second.p, kv.second.e };
            }
            // 完整窗口才接管（方案三）：窗口短于阈值（tick 间隔的一半）
            // = 数据还没覆盖一个正常采样窗——不完整的窗口冒充精确口径
            // 正是 C→A 切换瞬间全 0 闪烁的根子（per_pid 空窗被当成真零）。
            // 不接管时字节/窗口**不清零**，留在 m_out 累计到下个窗口合并
            // 交付，速率按合并后的真实窗口计算，永不断账
            out.valid = (out.window_us >= min_window_us);
            if (out.valid) {
                m_out.clear();
                m_out_window_us = 0;
                m_out_sample.clear();
                m_sample_total_out = 0;
            }
            // 采样通道的可用性：异构分类注入且本窗口完整（与 IO 同一
            // 窗口判定；占比口径无速率分母，窗口完整性即数据覆盖性）
            out.sample_valid = out.valid && m_ccm && m_ccm->heterogeneous
                               && out.sample_total > 0;
            // 接管转换点日志（方案三验证证据，仅 debug_logs 开时落盘）：
            // 让位（窗口不完整→fallback）与接管（首个完整窗口）的切换
            // 各打一条，稳态静默
            if (s_debug_logs.load() && out.valid != m_log_was_valid) {
                LogLine(L"window %s: win=%lluus pids=%u (threshold %lluus)",
                        out.valid ? L"TAKEOVER" : L"YIELD",
                        (unsigned long long)out.window_us,
                        (unsigned)out.per_pid.size(),
                        (unsigned long long)min_window_us);
                m_log_was_valid = out.valid;
            }
            // 核型时间账通道节流日志（§4.4 验证证据，debug_logs 开时每 5s
            // 一条）：全量核时间 + 占比 ≥0.5% 的 pid 明细（≤5 条）——实机
            // 对账/远期排查用，锁屏等无屏场景也能数据层核账（us = ticks/10）
            if (s_debug_logs.load() && out.sample_total > 0) {
                ULONGLONG now = GetTickCount64();
                if (now - m_log_sample_tick >= 5000) {
                    m_log_sample_tick = now;
                    LogLine(L"coretime window: total=%lluticks(%.2f core-seconds) pids=%u valid=%d",
                            (unsigned long long)out.sample_total,
                            (double)out.sample_total / 10000000.0,
                            (unsigned)out.sample_pid.size(),
                            (int)out.sample_valid);
                    int n = 0;
                    for (auto& kv : out.sample_pid) {
                        if (kv.second.first + kv.second.second < out.sample_total / 200)
                            continue;   // <0.5% 的杂项不打
                        LogLine(L"  coretime pid=%u p=%llu e=%llu (%.1f%% of total)",
                                kv.first,
                                (unsigned long long)kv.second.first,
                                (unsigned long long)kv.second.second,
                                100.0 * (double)(kv.second.first + kv.second.second)
                                    / (double)out.sample_total);
                        if (++n >= 5) break;
                    }
                }
            }
            QueryEventsLost();
            out.events_lost = m_events_lost;
            return;
        }
        Sleep(1);
    }
    // timeout: consumer silent (idle system, buffer callback not yet fired).
    // Do NOT read m_out (the handoff may still land concurrently) — caller
    // keeps its previous rates; the pending request is served next tick.
    //（累计式 handoff 下本拍等待期的字节与窗口自动并入下次交付，无丢失）
}
