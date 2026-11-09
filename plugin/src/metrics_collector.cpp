// metrics_collector.cpp — snapshot collection via NtQuerySystemInformation
#include "metrics_collector.h"
#include "core_class_mapper.h"
#include <cstdio>
#include <pdh.h>
#include <powerbase.h>

#pragma comment(lib, "powrprof.lib")

typedef LONG(NTAPI* PFN_NtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG SystemProcessInformation = 5;
constexpr ULONG SystemProcessorPerformanceInformation = 8;   // §4.5 每核 Idle

// PROCESSOR_POWER_INFORMATION：SDK 的 powrprof.h 不导出该结构（TM 同款手抄）
typedef struct _PTM_PROCESSOR_POWER_INFORMATION {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} PTM_PROCESSOR_POWER_INFORMATION;

int MetricsCollector::QueryCpuFreqMhz() {
    if (!m_freq_query) {
        PDH_HQUERY q = nullptr;
        if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return 0;
        PDH_HCOUNTER c = nullptr;
        // English counter path：免本地化（与 gpu_collector 同款约定）
        if (PdhAddEnglishCounterW(q, L"\\Processor Information(_Total)\\% Processor Performance",
                                  0, &c) != ERROR_SUCCESS) {
            PdhCloseQuery(q);
            return 0;
        }
        m_freq_query = q;
        m_freq_counter = c;

        SYSTEM_INFO si = {};
        GetSystemInfo(&si);
        ULONG n = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
        std::vector<PTM_PROCESSOR_POWER_INFORMATION> pp(n);
        if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, pp.data(),
                                   (ULONG)(pp.size() * sizeof(pp[0]))) == 0) {
            ULONG max_mhz = 0;
            for (auto& p : pp) if (p.MaxMhz > max_mhz) max_mhz = p.MaxMhz;
            m_freq_max_ghz = max_mhz / 1000.0;
        }
        if (m_freq_max_ghz <= 0.1) {
            // 兜底：注册表基准频率（无 turbo，仅保证不显示 0）
            HKEY hk = nullptr;
            DWORD mhz = 0, sz = sizeof(mhz);
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                              L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                              0, KEY_READ, &hk) == ERROR_SUCCESS) {
                RegQueryValueExW(hk, L"~MHz", nullptr, nullptr, (LPBYTE)&mhz, &sz);
                RegCloseKey(hk);
            }
            m_freq_max_ghz = mhz / 1000.0;
        }
    }
    if (PdhCollectQueryData((PDH_HQUERY)m_freq_query) != ERROR_SUCCESS)
        return m_freq_mhz;
    if (++m_freq_collections < 2) return 0;   // 首拍无 delta
    PDH_FMT_COUNTERVALUE v = {};
    if (PdhGetFormattedCounterValue((PDH_HCOUNTER)m_freq_counter, PDH_FMT_DOUBLE,
                                    nullptr, &v) == ERROR_SUCCESS &&
        v.CStatus == 0 /* PDH_CSTATUS_VALID_DATA */) {
        double ghz = v.doubleValue / 100.0 * m_freq_max_ghz;
        if (ghz > 0.1 && ghz < 20.0) m_freq_mhz = (int)(ghz * 1000.0 + 0.5);
    }
    return m_freq_mhz;
}

void MetricsCollector::Remember(DWORD pid, const std::wstring& name) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(m_name_mutex);
    auto& slot = m_names[pid];
    if (slot.empty() || slot != name) slot = name;
}

std::wstring MetricsCollector::LookupName(DWORD pid) {
    {
        std::lock_guard<std::mutex> lk(m_name_mutex);
        auto it = m_names.find(pid);
        if (it != m_names.end()) return it->second;
    }
    return L"";
}

std::wstring MetricsCollector::LookupPath(DWORD pid) {
    {
        std::lock_guard<std::mutex> lk(m_name_mutex);
        auto it = m_paths.find(pid);
        if (it != m_paths.end()) return it->second;
    }
    std::wstring got;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t buf[1024] = L"";
        DWORD n = 1024;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) got = buf;
        CloseHandle(h);
    }
    std::lock_guard<std::mutex> lk(m_name_mutex);
    m_paths[pid] = got;   // negative cache too: dead pids won't retry-spam
    return got;
}

bool MetricsCollector::Snapshot() {
    if (!m_ntqsi_tried) {
        m_ntqsi_tried = true;
        m_ntqsi = (void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                        "NtQuerySystemInformation");
    }
    if (!m_ntqsi) return false;
    auto fn = (PFN_NtQuerySystemInformation)m_ntqsi;

    // 缓冲区按需增长后复用不释放（技术方案 §8.1）。初始 512KB，
    // StatusInfoLengthMismatch 时翻倍重试。
    if (m_buf.size() < 512 * 1024) m_buf.resize(512 * 1024);
    ULONG need = 0;
    for (int attempt = 0; attempt < 6; attempt++) {
        LONG st = fn(SystemProcessInformation, m_buf.data(),
                     (ULONG)m_buf.size(), &need);
        if (st == 0) return true;
        if (st != 0xC0000004 /*STATUS_INFO_LENGTHMismatch*/) return false;
        size_t grow = m_buf.size() * 2;
        if (need > grow) grow = need;
        if (grow > 64u * 1024 * 1024) return false;
        m_buf.resize(grow);
    }
    return false;
}

void MetricsCollector::Tick(CpuMemIoMetrics& out) {
    out.procs.clear();
    out.first = !m_have_prev;

    if (m_cpu_cores <= 0) {
        // logical processor count (cached; PSIZE re-queries are not free)
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        if (len > 0) {
            std::vector<BYTE> buf(len);
            if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                 (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) {
                m_cpu_cores = 0;
                DWORD off = 0;
                while (off < len) {
                    auto* p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
                    if (p->Relationship == RelationProcessorCore) {
                        // count logical processors in the group masks
                        for (int g = 0; g < p->Processor.GroupCount; g++)
                            for (WORD m = 0; m < sizeof(ULONG_PTR) * 8; m++)
                                if (p->Processor.GroupMask[g].Mask & ((ULONG_PTR)1 << m)) m_cpu_cores++;
                    }
                    off += p->Size;
                }
            }
        }
        if (m_cpu_cores <= 0) {
            SYSTEM_INFO si = {};
            GetSystemInfo(&si);
            m_cpu_cores = si.dwNumberOfProcessors ? (int)si.dwNumberOfProcessors : 1;
        }
    }

    ULONGLONG now = GetTickCount64();
    double dt = m_have_prev ? (double)(now - m_prev_wall) / 1000.0 : 0.0;
    if (dt < 0.05) dt = 0.05;

    if (!Snapshot()) {
        // 采集失败：保持上拍基线不动（下一拍仍可产出 delta），本拍无数据
        out.first = !m_have_prev;
        out.cpu_total_pct = -1;
        return;
    }

    MEMORYSTATUSEX ms = { sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) {
        out.mem_total = ms.ullTotalPhys;
        out.mem_used = ms.ullTotalPhys - ms.ullAvailPhys;
    }

    // CPU 当前频率：每 2 拍（≈1s）更新一次，PDH 需要两拍 delta
    if ((m_freq_div++ & 1) == 0) out.cpu_freq_mhz = QueryCpuFreqMhz();
    else out.cpu_freq_mhz = m_freq_mhz;

    // 当前拍每进程原始值
    struct Raw {
        DWORD pid;
        std::wstring name;
        uint64_t cpu_time;
        uint64_t ws, priv;
        uint64_t io_read, io_write;
    };
    std::vector<Raw> raws;
    uint64_t idle_time = 0;
    // tid→pid 表（核型样本归因数据源，§4.2：SPI 尾部线程数组就地解析，
    // 零额外系统调用；tick 线程构建，TakeThreadTable 转交 ETW collector）
    auto tid_tbl = std::make_shared<std::unordered_map<DWORD, DWORD>>();

    auto* base = (SPI*)m_buf.data();
    for (ULONG off = 0;;) {
        SPI* e = (SPI*)(m_buf.data() + off);
        DWORD pid = (DWORD)(ULONG_PTR)e->UniqueProcessId;
        uint64_t cpu = (uint64_t)e->KernelTime.QuadPart + (uint64_t)e->UserTime.QuadPart;
        auto* th = (SysThreadInfo*)(e + 1);   // 紧随 SPI 的线程数组
        for (ULONG t = 0; t < e->NumberOfThreads; t++)
            (*tid_tbl)[(DWORD)(ULONG_PTR)th[t].UniqueThread] = pid;
        if (pid == 0) {
            // Idle / "System Idle Process": excluded from ranking, counted
            // for the system-wide CPU summary
            idle_time = cpu;
        } else if (pid == 4) {
            // System: keep in ranking (it does real work — lazy writer etc.)
            Raw r{ pid, L"System", cpu,
                   (uint64_t)e->WorkingSetSize,
                   (uint64_t)e->CommitCharge,
                   e->ReadTransferCount, e->WriteTransferCount };
            raws.push_back(std::move(r));
        } else {
            std::wstring name;
            if (e->ImageName.Buffer && e->ImageName.Length) {
                // Buffer points INTO our own buffer (same call) — copy before
                // the next tick overwrites it.
                name.assign(e->ImageName.Buffer,
                            e->ImageName.Length / sizeof(wchar_t));
            }
            Remember(pid, name);
            Raw r{ pid, name, cpu,
                   (uint64_t)e->WorkingSetSize,
                   (uint64_t)e->CommitCharge,
                   e->ReadTransferCount, e->WriteTransferCount };
            raws.push_back(std::move(r));
        }
        if (e->NextEntryOffset == 0) break;
        off += e->NextEntryOffset;
    }

    // 内存口径：PrivatePageCount 默认（更能反映进程真实负担）
    // WorkingSetSize 可选（技术方案 §2.2）
    out.procs.reserve(raws.size());
    for (auto& r : raws) {
        ProcMetric m;
        m.pid = r.pid;
        m.name = r.name;
        m.mem_ws = r.ws;
        m.mem_private = r.priv;
        m.mem = m_mem_ws ? r.ws : r.priv;
        auto it = m_prev.find(r.pid);
        if (it != m_prev.end() && it->second.seen) {
            // CPU time is cumulative since boot — immune to sampling jitter.
            // Counter resets (pid reuse) clamp at 0 (方案 §8.1 delta 钳制).
            uint64_t dc = r.cpu_time >= it->second.cpu_time
                          ? r.cpu_time - it->second.cpu_time : 0;
            uint64_t dr = r.io_read >= it->second.io_read
                          ? r.io_read - it->second.io_read : 0;
            uint64_t dw = r.io_write >= it->second.io_write
                          ? r.io_write - it->second.io_write : 0;
            // 100ns units -> seconds: dc/1e7 over (dt*cores) -> percent
            double denom = dt * (double)m_cpu_cores;
            if (denom > 0) m.cpu_pct = (dc / 1e7) / denom * 100.0;
            m.io_read_bps = (double)dr / dt;
            m.io_write_bps = (double)dw / dt;
        }
        out.procs.push_back(std::move(m));
    }

    // system CPU%: 100 - idle% (idle counted against ALL logical cores,
    // 100ns units -> seconds: di/1e7 over dt*cores)
    if (m_have_prev) {
        uint64_t di = idle_time >= m_prev_idle_time
                      ? idle_time - m_prev_idle_time : 0;
        double idle_pct = (di / 1e7) / (dt * (double)m_cpu_cores) * 100.0;
        if (idle_pct < 0) idle_pct = 0;
        if (idle_pct > 100) idle_pct = 100;
        out.cpu_total_pct = 100.0 - idle_pct;
    } else {
        out.cpu_total_pct = -1;
    }

    // store this tick as the new baseline
    m_prev.clear();
    m_prev.reserve(raws.size() + 8);
    for (auto& r : raws)
        m_prev[r.pid] = Prev{ r.cpu_time, r.io_read, r.io_write, true };
    m_prev_idle_time = idle_time;
    m_prev_wall = now;
    m_have_prev = true;

    // tid→pid 表交付（§4.2：本拍构建完毕）
    m_thread_table = std::move(tid_tbl);

    // 系统级核群占用率（§4.5，一期拍板不延后）：异构时才跑（每拍 ~0.1ms）
    TickCoreClassTotals(out, dt);
}

void MetricsCollector::TickCoreClassTotals(CpuMemIoMetrics& out, double dt) {
    out.cpu_split_total_pct[0] = -1;
    out.cpu_split_total_pct[1] = -1;
    if (!m_ccm || !m_ccm->heterogeneous || !m_ccm->total_lp) return;

    auto fn = (PFN_NtQuerySystemInformation)m_ntqsi;
    if (!m_ntqsi_tried) {
        // Snapshot() 尚未被调过（首拍顺序）：此处自行解析一次
        m_ntqsi_tried = true;
        m_ntqsi = (void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                        "NtQuerySystemInformation");
        fn = (PFN_NtQuerySystemInformation)m_ntqsi;
    }
    if (!fn) return;

    size_t need_records = (size_t)m_ccm->total_lp;
    size_t want = need_records * sizeof(SPPI) + 64;
    if (m_sppi_buf.size() < want) m_sppi_buf.resize(want);
    ULONG got = 0;
    LONG st = fn(SystemProcessorPerformanceInformation, m_sppi_buf.data(),
                 (ULONG)m_sppi_buf.size(), &got);
    if (st != 0) return;
    size_t n_records = got / sizeof(SPPI);
    if (n_records < need_records) return;   // 记录数不足：如实无效，不造假

    // 首拍只建基线（无 delta），下拍出值
    if (!m_have_prev_idle_core || m_prev_idle_core.size() != n_records) {
        m_prev_idle_core.assign(n_records, 0);
        for (size_t i = 0; i < n_records; i++)
            m_prev_idle_core[i] = ((SPPI*)(m_sppi_buf.data() + i * sizeof(SPPI)))
                                      ->IdleTime.QuadPart;
        m_have_prev_idle_core = true;
        return;
    }

    // 每核 Idle 差分 → 核群和（100ns 单位）。绝对 LP 号 = 数组下标（§3.2）
    uint64_t idle_sum[2] = { 0, 0 };
    for (size_t i = 0; i < n_records; i++) {
        uint64_t idle = ((SPPI*)(m_sppi_buf.data() + i * sizeof(SPPI)))
                            ->IdleTime.QuadPart;
        uint64_t d = idle >= m_prev_idle_core[i] ? idle - m_prev_idle_core[i] : 0;
        m_prev_idle_core[i] = idle;
        // P/VCache 群 = [1]，E/StdCcd 群 = [0]；Homog（防御性未知核）不进任何群
        if (m_ccm->lp[i] == CoreClass::P || m_ccm->lp[i] == CoreClass::VCache)
            idle_sum[1] += d;
        else if (m_ccm->lp[i] == CoreClass::E || m_ccm->lp[i] == CoreClass::StdCcd)
            idle_sum[0] += d;
    }
    // 核群容量占用率：100 − ΣIdle增量 / (dt × 核群核数)（§4.5 口径）
    for (int g = 0; g < 2; g++) {
        int n = m_ccm->class_lp_count[g];
        if (n <= 0 || dt <= 0) continue;
        double idle_pct = ((double)idle_sum[g] / 1e7) / (dt * (double)n) * 100.0;
        if (idle_pct < 0) idle_pct = 0;
        if (idle_pct > 100) idle_pct = 100;
        out.cpu_split_total_pct[g] = 100.0 - idle_pct;
    }
}
