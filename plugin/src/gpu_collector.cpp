// gpu_collector.cpp — PDH GPU Engine / Process Memory / Adapter Memory (G1)
#include "gpu_collector.h"
#include "utils.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cinttypes>

#pragma comment(lib, "pdh.lib")

// ============================================================
// PDH wrappers (raw fn-typedefs not needed: pdh.h + pdh.lib direct link,
// PdhAddEnglishCounterW is Win7+ — same floor as the plugin itself)
// ============================================================

namespace {

// \GPU Engine(*)\Utilization Percentage etc. — English paths added through
// PdhAddEnglishCounterW so locale never matters (中文系统计数器名不同).
constexpr wchar_t kPathUtil[] = L"\\GPU Engine(*)\\Utilization Percentage";
constexpr wchar_t kPathMemDed[] = L"\\GPU Process Memory(*)\\Dedicated Usage";
constexpr wchar_t kPathMemLocal[] = L"\\GPU Process Memory(*)\\Local Usage";
constexpr wchar_t kPathMemShared[] = L"\\GPU Process Memory(*)\\Shared Usage";
constexpr wchar_t kPathAdaptDed[] = L"\\GPU Adapter Memory(*)\\DedicatedUsage";
constexpr wchar_t kPathAdaptShared[] = L"\\GPU Adapter Memory(*)\\SharedUsage";

bool AddCounter(void* query, const wchar_t* path, void** out) {
    PDH_HCOUNTER h = nullptr;
    PDH_STATUS st = PdhAddEnglishCounterW((PDH_HQUERY)query, path, 0, &h);
    if (st != ERROR_SUCCESS) { *out = nullptr; return false; }
    *out = (void*)h;
    return true;
}

// Formatted array fetch. Returns false on hard failure; PDH_MORE_DATA grows
// the shared buffer. Items: name + cooked value (CStatus checked by caller).
bool GetFormattedArray(void* counter, std::vector<BYTE>& buf,
                       std::vector<PDH_FMT_COUNTERVALUE_ITEM_W>& items) {
    if (!counter) return false;
    DWORD size = 0, count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(
        (PDH_HCOUNTER)counter, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (st != PDH_MORE_DATA || size == 0) return false;
    if (buf.size() < size) buf.resize(size);
    st = PdhGetFormattedCounterArrayW((PDH_HCOUNTER)counter, PDH_FMT_DOUBLE,
                                      &size, &count,
                                      (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data());
    if (st != ERROR_SUCCESS) return false;
    items.assign((PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data(),
                 (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data() + count);
    return true;
}

bool ValueOk(const PDH_FMT_COUNTERVALUE& v) {
    return v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA;
}

// 宽松解析（方案 §10 风险 8）: pid_1234_luid_..._phys_0_eng_0_engtype_3D —
// 只锚定 pid_ / engtype_，其余任意。返回是否拿到了 pid。
bool ParseEngineInstance(const std::wstring& inst, DWORD& pid, std::wstring& engtype) {
    size_t p = inst.find(L"pid_");
    if (p == std::wstring::npos) return false;
    pid = (DWORD)wcstoul(inst.c_str() + p + 4, nullptr, 10);
    if (!pid) return false;
    engtype.clear();
    size_t e = inst.find(L"engtype_");
    if (e != std::wstring::npos) engtype = inst.substr(e + 8);
    return true;
}

bool ParsePidInstance(const std::wstring& inst, DWORD& pid) {
    size_t p = inst.find(L"pid_");
    if (p == std::wstring::npos) return false;
    pid = (DWORD)wcstoul(inst.c_str() + p + 4, nullptr, 10);
    return pid != 0;
}

} // namespace

// ============================================================
// lifecycle
// ============================================================

GpuCollector::~GpuCollector() { CloseQuery(); }

void GpuCollector::Configure(const GpuScopeConfig& cfg) {
    std::lock_guard<std::mutex> lk(m_cfg_mutex);
    m_cfg = cfg;
}

GpuScopeConfig GpuCollector::Scope() const {
    std::lock_guard<std::mutex> lk(m_cfg_mutex);
    return m_cfg;
}

void GpuCollector::DetectGrade(std::wstring& reason) {
    DWORD build = 0;
    if (!PTM_GetOSBuild(build)) {
        m_grade = GpuGrade::G3;
        reason = L"OS version unavailable";
        return;
    }
    if (build < 9200) {
        // G3: Win7 has no reliable per-process GPU path (方案 §2.4 G3)
        m_grade = GpuGrade::G3;
        reason = L"Windows 7";
        return;
    }
    // 测试开关：PTM_GPU_FORCE_G2=1 在现代 build 上强制走 G2 路径——
    // 26300 实测系统拒收 Win8+ 时代查询类型，恰好验证「查询被拒 → 如实
    // 空态」的回退行为（不伪造数据）。
    wchar_t force[4] = L"";
    bool forced = GetEnvironmentVariableW(L"PTM_GPU_FORCE_G2", force, 4) > 0;
    if (build < 16299 || forced) {
        // G2 range (Win8 ~ Win10 1607): D3DKMTQueryStatistics path.
        m_grade = GpuGrade::G2;
        reason.clear();
        return;
    }
    // G1: PDH GPU Engine exists since 1709; counter add failure below is
    // the RDP / no-GPU detector.
    m_grade = GpuGrade::G1;
    reason.clear();
}

bool GpuCollector::EnsureStarted() {
    if (m_running.load()) return true;
    DetectGrade(m_grade_reason);
    if (m_grade == GpuGrade::G1) {
        if (!OpenQuery()) {
            m_grade = GpuGrade::G3;
            m_grade_reason = L"GPU counters unavailable (RDP / no GPU?)";
            m_running.store(false);
            return false;
        }
        m_collections = 0;
        m_running.store(true);
        return true;
    }
    if (m_grade == GpuGrade::G2) {
        if (!InitG2()) {
            m_grade = GpuGrade::G3;
            m_grade_reason = L"D3DKMT adapters unavailable";
            m_running.store(false);
            return false;
        }
        m_g2_last_wall_us = 0;
        m_running.store(true);
        return true;
    }
    m_running.store(false);
    return false;
}

void GpuCollector::EnsureStopped() {
    if (!m_running.exchange(false)) return;
    CloseQuery();
    StopG2();
    m_eng_cache.clear();
    m_pid_cache.clear();
    m_engtypes.clear();
    m_fmt_buf.clear();
    m_fmt_buf.shrink_to_fit();
    m_collections = 0;
}

bool GpuCollector::OpenQuery() {
    CloseQuery();
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return false;
    m_query = (void*)q;
    bool have_util = AddCounter(m_query, kPathUtil, &m_cnt_util);
    // Dedicated Usage on newer builds; Local Usage is the pre-~19H1 name —
    // try both, keep which one worked (方案 §2.5 两种命名兼容).
    bool have_mem = AddCounter(m_query, kPathMemDed, &m_cnt_mem_ded);
    m_mem_ded_is_local = false;
    if (!have_mem) {
        have_mem = AddCounter(m_query, kPathMemLocal, &m_cnt_mem_ded);
        m_mem_ded_is_local = have_mem;
    }
    // optional counters (missing on some builds): fine to run without
    AddCounter(m_query, kPathMemShared, &m_cnt_mem_shared);
    AddCounter(m_query, kPathAdaptDed, &m_cnt_adapt_ded);
    AddCounter(m_query, kPathAdaptShared, &m_cnt_adapt_shared);
    // the dimension is real only with the utilization counter present
    if (!have_util) {
        CloseQuery();
        return false;
    }
    return true;
}

void GpuCollector::CloseQuery() {
    if (m_query) {
        PdhCloseQuery((PDH_HQUERY)m_query);
        m_query = nullptr;
    }
    m_cnt_util = m_cnt_mem_ded = m_cnt_mem_shared = nullptr;
    m_cnt_adapt_ded = m_cnt_adapt_shared = nullptr;
}

// ============================================================
// G2 — D3DKMTQueryStatistics (Win8 ~ Win10 1607)
// ============================================================

namespace {

// ---- 最小手抄结构（成员顺序 = 本机 SDK d3dkmthk.h，探针已验证偏移）----

struct PtD3dkmtAdapterInfo {
    ULONG hAdapter;
    LUID  AdapterLuid;
    ULONG NumOfSources;
    BOOL  bPrecisePresentRegionsPreferred;
};
struct PtD3dkmtEnumAdapters {
    ULONG NumAdapters;
    PtD3dkmtAdapterInfo Adapters[16];
};

// 查询结构：头部字段与官方逐一对应（Type@0 Luid@4 hProcess@ptr对齐），
// 结果 union 用 1KB 冗余缓冲替代（任何 per-process 结果 << 1KB），尾部
// query-param union 只有一个 ULONG 大小——用 Param 收尾保持布局。
struct PtD3dkmtQuery {
    ULONG  Type;
    LUID   AdapterLuid;
    HANDLE hProcess;
    UCHAR  Result[1024];
    ULONG  Param;
};
enum PtQType {
    PtQAdapter = 0, PtQProcess = 1, PtQSegment = 3, PtQProcessSegment = 4,
    PtQProcessNode = 6, PtQProcessSegmentGroup = 9,
};

// 结果头部读取（Win8+ 布局；只读已知字段，尾部不碰）
struct PtSegHead {            // PtQSegment
    ULONGLONG CommitLimit, BytesCommitted, BytesResident;
    ULONGLONG TotalBytesEvicted; ULONG AllocsCommitted, AllocsResident;
    ULONG Aperture;
};

// WIN32_LEAN_AND_MEAN 不含 ntdef.h（26100 SDK 实测：NTSTATUS 未定义，
// C2059 事故源头）——这里等价用 LONG
typedef LONG(APIENTRY* PFN_D3dkmtQuery)(const void*);
typedef LONG(APIENTRY* PFN_D3dkmtEnum)(void*);

} // namespace

bool GpuCollector::InitG2() {
    if (m_pfn_d3dkmt_query) return true;
    m_gdi32 = (void*)LoadLibraryW(L"gdi32.dll");
    if (!m_gdi32) return false;
    m_pfn_d3dkmt_query = (void*)GetProcAddress((HMODULE)m_gdi32,
                                               "D3DKMTQueryStatistics");
    m_pfn_d3dkmt_enum = (void*)GetProcAddress((HMODULE)m_gdi32,
                                              "D3DKMTEnumAdapters");
    if (!m_pfn_d3dkmt_query || !m_pfn_d3dkmt_enum) {
        FreeLibrary((HMODULE)m_gdi32);
        m_gdi32 = nullptr;
        return false;
    }
    auto pEnum = (PFN_D3dkmtEnum)m_pfn_d3dkmt_enum;
    auto pQuery = (PFN_D3dkmtQuery)m_pfn_d3dkmt_query;

    PtD3dkmtEnumAdapters ea = {};
    ea.NumAdapters = 16;
    if (pEnum(&ea) != 0 || ea.NumAdapters == 0) {
        FreeLibrary((HMODULE)m_gdi32);
        m_gdi32 = nullptr;
        m_pfn_d3dkmt_query = m_pfn_d3dkmt_enum = nullptr;
        return false;
    }
    m_g2_adapters.clear();
    for (ULONG i = 0; i < ea.NumAdapters; i++) {
        G2Adapter a;
        a.luid = ea.Adapters[i].AdapterLuid;
        // 适配器统计：NodeCount/NbSegments（无 hProcess，Win7 时代类型）
        PtD3dkmtQuery q = {};
        q.Type = PtQAdapter;
        q.AdapterLuid = a.luid;
        if (pQuery(&q) != 0) continue;
        a.nb_segments = *(ULONG*)(q.Result + 0);
        a.node_count = *(ULONG*)(q.Result + 4);
        if (!a.node_count || !a.nb_segments) continue;
        // 段属性（Aperture）：WDDM2.1+ 用 SEGMENT_GROUP，不需要；老系统
        // 专用/共享拆分依赖它——探针一次，存档
        a.seg_aperture.assign(a.nb_segments > 32 ? 32 : a.nb_segments, 0xFF);
        for (ULONG s = 0; s < a.seg_aperture.size(); s++) {
            PtD3dkmtQuery qs = {};
            qs.Type = PtQSegment;
            qs.AdapterLuid = a.luid;
            qs.Param = s;
            if (pQuery(&qs) == 0) {
                a.seg_aperture[s] = ((PtSegHead*)qs.Result)->Aperture;
            }
        }
        // SEGMENT_GROUP 类型可用性（WDDM2.1+ = Win10 1607+）：空 hProcess
        // 的探测调用，接受即走新路径（Win8~1511 落到逐段求和）
        PtD3dkmtQuery qg = {};
        qg.Type = PtQProcessSegmentGroup;
        qg.AdapterLuid = a.luid;
        qg.Param = 0;   // LOCAL
        LONG rcg = pQuery(&qg);
        a.seg_group_ok = (rcg == 0);
        m_g2_adapters.push_back(a);
    }
    if (m_g2_adapters.empty()) {
        FreeLibrary((HMODULE)m_gdi32);
        m_gdi32 = nullptr;
        m_pfn_d3dkmt_query = m_pfn_d3dkmt_enum = nullptr;
        return false;
    }
    return true;
}

void GpuCollector::StopG2() {
    m_g2_adapters.clear();
    m_g2_last_rt.clear();
    m_g2_last_wall_us = 0;
    if (m_gdi32) {
        FreeLibrary((HMODULE)m_gdi32);
        m_gdi32 = nullptr;
    }
    m_pfn_d3dkmt_query = m_pfn_d3dkmt_enum = nullptr;
}

// G2 一拍：候选 pid × 适配器 × 节点查询 + delta 换算（方案 §2.4 G2 行）
bool GpuCollector::TickG2(GpuTickData& out,
                          const std::vector<DWORD>& pid_candidates) {
    auto pQuery = (PFN_D3dkmtQuery)m_pfn_d3dkmt_query;
    if (!pQuery) return false;

    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    LONGLONG wall_us = (LONGLONG)(t.QuadPart * 1000000.0 / f.QuadPart);
    double dt_us = m_g2_last_wall_us ? (double)(wall_us - m_g2_last_wall_us) : 0.0;
    m_g2_last_wall_us = wall_us;

    // per-pid 逐适配器采集
    std::unordered_set<DWORD> seen;
    bool util_any = false, mem_any = false;
    for (DWORD pid : pid_candidates) {
        if (!pid || seen.count(pid)) continue;
        HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!hp) continue;    // 受保护进程：如实缺席
        seen.insert(pid);
        auto& state = m_g2_last_rt[pid];   // [adapter][node]

        double util_pct = 0;
        uint64_t ded = 0, shr = 0;
        bool pid_util_ok = false, pid_mem_ok = false;
        if (state.size() < m_g2_adapters.size())
            state.resize(m_g2_adapters.size());

        for (size_t ai = 0; ai < m_g2_adapters.size(); ai++) {
            G2Adapter& a = m_g2_adapters[ai];
            if (a.seg_group_ok) {
                // ---- 显存：SEGMENT_GROUP LOCAL/NON_LOCAL（WDDM2.1+）----
                for (int g = 0; g < 2; g++) {
                    PtD3dkmtQuery qg = {};
                    qg.Type = PtQProcessSegmentGroup;
                    qg.AdapterLuid = a.luid;
                    qg.hProcess = hp;
                    qg.Param = (ULONG)g;
                    if (pQuery(&qg) == 0) {
                        ULONGLONG usage = *(ULONGLONG*)(qg.Result + 16);
                        if (g == 0) { ded += usage; pid_mem_ok = true; }
                        else shr += usage;
                    }
                }
            } else {
                // ---- 老路径：PROCESS_SEGMENT 逐段求和（Win8~1511）----
                for (ULONG s = 0; s < a.seg_aperture.size(); s++) {
                    if (a.seg_aperture[s] == 0xFF) continue;
                    PtD3dkmtQuery qp = {};
                    qp.Type = PtQProcessSegment;
                    qp.AdapterLuid = a.luid;
                    qp.hProcess = hp;
                    qp.Param = s;
                    if (pQuery(&qp) == 0) {
                        ULONGLONG committed = *(ULONGLONG*)(qp.Result + 0);
                        if (a.seg_aperture[s]) shr += committed;
                        else { ded += committed; pid_mem_ok = true; }
                    }
                }
            }

            // ---- 利用率：PROCESS_NODE 逐节点 RunningTime delta ----
            if (dt_us > 0 && state[ai].size() < a.node_count)
                state[ai].resize(a.node_count, -1);
            for (ULONG n = 0; n < a.node_count && dt_us > 0; n++) {
                PtD3dkmtQuery qn = {};
                qn.Type = PtQProcessNode;
                qn.AdapterLuid = a.luid;
                qn.hProcess = hp;
                qn.Param = n;
                if (pQuery(&qn) != 0) { state[ai][n] = -1; continue; }
                LONGLONG rt = ((LARGE_INTEGER*)qn.Result)->QuadPart;
                // 实测定案：RunningTime 单位是 100ns（NT 时间惯例），WDK 头里
                // "micro-second" 注释是错的——强制 G2 实测 G2 值恰为 G1(PDH)
                // 的 10 倍，除以 10 后逐进程吻合
                LONGLONG& prev = state[ai][n];
                if (prev >= 0 && rt >= prev) {
                    // util% = ΣΔ_100ns / (wall_100ns × node_count) × 100（每适配器独立归一）
                    double wall_100ns = dt_us * 10.0;
                    util_pct += (double)(rt - prev) / (wall_100ns * (double)a.node_count) * 100.0;
                    pid_util_ok = true;
                }
                prev = rt;
            }
        }
        if (pid_util_ok) {
            if (util_pct < 0) util_pct = 0;
            out.util_by_pid[pid] += util_pct;
            out.total_util_pct += util_pct;
            util_any = true;
        }
        if (pid_mem_ok) {
            out.mem_by_pid[pid] = { ded, shr };
            out.sys_dedicated += ded;
            out.sys_shared += shr;
            mem_any = true;
        }
        CloseHandle(hp);
    }

    // 退场 pid 的基线清理
    for (auto it = m_g2_last_rt.begin(); it != m_g2_last_rt.end();) {
        if (!seen.count(it->first)) it = m_g2_last_rt.erase(it);
        else ++it;
    }

    // 截断 + valid 位
    GpuScopeConfig cfg = Scope();
    auto cap = [&](double v) { return (cfg.cap_100 && v > 100.0) ? 100.0 : v; };
    for (auto& kv : out.util_by_pid) kv.second = cap(kv.second);
    out.total_util_pct = cap(out.total_util_pct);
    out.util_valid = util_any;
    out.mem_valid = mem_any;
    out.sys_mem_valid = mem_any;
    return util_any || mem_any;
}



// ============================================================
// collection — G1 PDH main path (G2 dispatched inside)
// ============================================================

void GpuCollector::Tick(GpuTickData& out,
                        const std::vector<DWORD>& pid_candidates) {
    out.grade = m_grade;
    out.unsupported_reason = m_grade_reason;
    out.util_valid = out.mem_valid = out.sys_mem_valid = false;
    if (!m_running.load()) return;

    if (m_grade == GpuGrade::G2) {
        TickG2(out, pid_candidates);
        return;
    }
    if (!m_query) return;

    if (PdhCollectQueryData((PDH_HQUERY)m_query) != ERROR_SUCCESS)
        return;
    m_collections++;
    out.mem_valid = true;                 // raw counters: 1st collection suffices
    if (m_collections < 2) return;        // utilization needs 2 collections
    out.util_valid = true;

    GpuScopeConfig cfg = Scope();

    // known engtypes (dynamic enumeration, sorted) for the scope UI
    out.known_engtypes.assign(m_engtypes.begin(), m_engtypes.end());
    std::sort(out.known_engtypes.begin(), out.known_engtypes.end());

    auto in_scope = [&](const std::wstring& eng) {
        if (cfg.all_engines || eng.empty()) return true;
        for (auto& e : cfg.engines)
            if (e == eng) return true;
        return false;
    };
    auto cap = [&](double v) {
        return (cfg.cap_100 && v > 100.0) ? 100.0 : v;
    };

    // ---- GPU Engine utilization: per (pid × engtype) instances ----
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> items;
    if (GetFormattedArray(m_cnt_util, m_fmt_buf, items)) {
        for (auto& it : items) {
            if (!ValueOk(it.FmtValue)) continue;
            DWORD pid;
            std::wstring eng;
            std::wstring inst = it.szName ? it.szName : L"";
            auto cached = m_eng_cache.find(inst);
            if (cached != m_eng_cache.end()) {
                pid = cached->second.first;
                eng = cached->second.second;
            } else {
                if (!ParseEngineInstance(inst, pid, eng)) {
                    m_eng_cache[inst] = { 0, L"" };   // negative cache
                    continue;
                }
                m_eng_cache[inst] = { pid, eng };
                if (!eng.empty()) m_engtypes.insert(eng);
            }
            if (!in_scope(eng)) continue;
            double v = it.FmtValue.doubleValue;
            if (v < 0) v = 0;
            out.util_by_pid[pid] += v;
            out.total_util_pct += v;
        }
        for (auto& kv : out.util_by_pid) kv.second = cap(kv.second);
        out.total_util_pct = cap(out.total_util_pct);
    }

    // ---- GPU Process Memory: per-pid dedicated (+shared) bytes ----
    auto collect_mem = [&](void* counter,
                           std::unordered_map<DWORD, uint64_t>& dst) {
        if (!counter) return false;
        if (!GetFormattedArray(counter, m_fmt_buf, items)) return false;
        for (auto& it : items) {
            if (!ValueOk(it.FmtValue)) continue;
            DWORD pid = 0;
            std::wstring inst = it.szName ? it.szName : L"";
            auto cached = m_pid_cache.find(inst);
            if (cached != m_pid_cache.end()) pid = cached->second;
            else {
                if (!ParsePidInstance(inst, pid)) {
                    m_pid_cache[inst] = 0;
                    continue;
                }
                m_pid_cache[inst] = pid;
            }
            double v = it.FmtValue.doubleValue;
            if (v < 0) v = 0;
            dst[pid] += (uint64_t)v;
        }
        return true;
    };
    std::unordered_map<DWORD, uint64_t> ded, shr;
    collect_mem(m_cnt_mem_ded, ded);
    if (m_cnt_mem_shared) collect_mem(m_cnt_mem_shared, shr);
    for (auto& kv : ded) {
        uint64_t s = 0;
        auto it = shr.find(kv.first);
        if (it != shr.end()) s = it->second;
        out.mem_by_pid[kv.first] = { kv.second, s };
    }
    // shared-only pids (rare but legal)
    for (auto& kv : shr) {
        if (out.mem_by_pid.count(kv.first)) continue;
        out.mem_by_pid[kv.first] = { 0, kv.second };
    }

    // ---- GPU Adapter Memory (system-wide, summary line) ----
    std::unordered_map<DWORD, uint64_t> adapt_ded, adapt_shr;
    bool adapt_ok = collect_mem(m_cnt_adapt_ded, adapt_ded);
    if (m_cnt_adapt_shared) collect_mem(m_cnt_adapt_shared, adapt_shr);
    if (adapt_ok) {
        out.sys_mem_valid = true;
        for (auto& kv : adapt_ded) out.sys_dedicated += kv.second;
        for (auto& kv : adapt_shr) out.sys_shared += kv.second;
    } else {
        // fallback: sum the per-process view (still an honest system total)
        for (auto& kv : ded) out.sys_dedicated += kv.second;
        for (auto& kv : shr) out.sys_shared += kv.second;
        out.sys_mem_valid = out.mem_valid;
    }
}
