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
    if (build < 16299) {
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
    if (m_grade != GpuGrade::G1) {
        // G2 path (D3DKMT) lands with the G2 implementation; until then
        // only G1 systems collect. G3 = hidden dimension.
        m_running.store(false);
        return false;
    }
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

void GpuCollector::EnsureStopped() {
    if (!m_running.exchange(false)) return;
    CloseQuery();
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
// collection
// ============================================================

void GpuCollector::Tick(GpuTickData& out) {
    out.grade = m_grade;
    out.unsupported_reason = m_grade_reason;
    out.util_valid = out.mem_valid = out.sys_mem_valid = false;
    if (!m_running.load() || !m_query) return;

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
