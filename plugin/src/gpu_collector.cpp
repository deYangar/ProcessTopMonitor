// gpu_collector.cpp — PDH GPU Engine / Process Memory / Adapter Memory (G1)
// 2026-11-10 显存口径修复：per-pid 显存数值源切 D3DKMTQueryStatistics
// （任务管理器性能页同款 VidMM 会计）；PDH GPU Process Memory 的 cooked
// 值是只增不减的累计假账（Starward 实测虚报 660 倍且每秒 +750MB，
// 微软 known-issue 文档在案），只保留其实例名作 (pid×LUID) 枚举。
#include "gpu_collector.h"
#include "utils.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cinttypes>

// SDK 官方 D3DKMT 查询布局（shared 头）。WDDM2.1 起 PROCESS_SEGMENT_GROUP
// 存在且尾部参数 union 偏移恒定（WDDM2.1/3.1 编译布局一致：tail@800，
// 探针实测 26300）。宏必须显式定义——否则条件编译全走最老分支。
// winternl.h 先行：LEAN_AND_MEAN 下 NTSTATUS 未定义（shared 头 typedef
// 全线 C2059——旧手抄版废弃注释里的 C2059 事故同源）
#define DXGKDDI_INTERFACE_VERSION 0x6003
#include <winternl.h>
#include <d3dkmthk.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "gdi32.lib")

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
// 2026-11-10 修复：注册表（Perflib\009）注册名带空格，无空格的
// "DedicatedUsage" 自上线起 PdhAddEnglishCounterW 恒失败，系统级汇总
// 从未生效、恒走「进程加总」fallback——假账直接顶到标题栏
constexpr wchar_t kPathAdaptDed[] = L"\\GPU Adapter Memory(*)\\Dedicated Usage";
constexpr wchar_t kPathAdaptShared[] = L"\\GPU Adapter Memory(*)\\Shared Usage";

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

// pid_1234_luid_0x00000000_0x00014ab5_phys_0 → (pid, LUID)。luid 段缺失时
// luid 置零（调用方按「仅 pid 汇总」防御）。返回是否拿到了 pid。
bool ParsePidInstance(const std::wstring& inst, DWORD& pid, LUID& luid) {
    luid.LowPart = luid.HighPart = 0;
    size_t p = inst.find(L"pid_");
    if (p == std::wstring::npos) return false;
    pid = (DWORD)wcstoul(inst.c_str() + p + 4, nullptr, 10);
    if (!pid) return false;
    size_t l = inst.find(L"luid_0x", p);
    if (l != std::wstring::npos)
        swscanf_s(inst.c_str() + l, L"luid_0x%x_0x%x",
                  &luid.HighPart, &luid.LowPart);
    return true;
}

// SeDebugPrivilege 一次性启用（提权宿主内可用；失败=普通进程口径，
// 受保护进程如实缺席）。返回是否成功。token 权限是进程级的，启用后
// 一直有效，EnsureStarted 调一次即可。
bool EnableSeDebug() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;
    LUID lid;
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &lid)) {
        TOKEN_PRIVILEGES tp;
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = lid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        ok = GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(tok);
    return ok;
}

// per-pid per-adapter 的 LOCAL/NON_LOCAL Usage（VidMM 会计，任务管理器
// 性能页同款）。hProcess 需 PROCESS_QUERY_INFORMATION；该 (pid,LUID) 无
// GPU 上下文时内核拒收（C000000D），返回 false 按无数据处理。
bool QueryProcessSegmentUsage(HANDLE hp, LUID luid,
                              uint64_t& local, uint64_t& non_local) {
    local = non_local = 0;
    bool any = false;
    for (int g = 0; g < 2; g++) {
        D3DKMT_QUERYSTATISTICS q = {};
        q.Type = D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP;
        q.AdapterLuid = luid;
        q.hProcess = hp;
        q.QueryProcessSegmentGroup =
            (D3DKMT_MEMORY_SEGMENT_GROUP)g;   // 0=LOCAL 1=NON_LOCAL
        if (D3DKMTQueryStatistics(&q) != 0) return false;
        uint64_t usage = q.QueryResult.ProcessSegmentGroupInformation.Usage;
        if (g == 0) local = usage; else non_local = usage;
        any = true;
    }
    return any;
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

void GpuCollector::SetGradeLocked(GpuGrade g, const wchar_t* why) {
    // C-7：档位状态唯一写入通道（m_grade_mutex 内）
    std::lock_guard<std::mutex> lk(m_grade_mutex);
    m_grade = g;
    m_grade_reason = why ? why : L"";
}

GpuGrade GpuCollector::Grade() const {
    // C-7：UI 线程（设置页/菜单）与 tick 线程并发读——锁内取
    std::lock_guard<std::mutex> lk(m_grade_mutex);
    return m_grade;
}

void GpuCollector::Reason(wchar_t* out, size_t cap) const {
    // C-7：锁内拷入调用方缓冲（tick 侧重探测并发时读者拿到完整串）
    if (!out || cap == 0) return;
    std::lock_guard<std::mutex> lk(m_grade_mutex);
    wcsncpy_s(out, cap, m_grade_reason.c_str(), _TRUNCATE);
}

void GpuCollector::DetectGrade(std::wstring& reason) {
    // 局部变量结算，结尾一次锁内提交（C-7）——函数体不再直写成员，
    // 调用方（EnsureStarted/ProbeGrade）拿返回的 reason 做后续分支
    GpuGrade g = GpuGrade::G3;
    DWORD build = 0;
    if (!PTM_GetOSBuild(build)) {
        reason = L"OS version unavailable";
        SetGradeLocked(g, reason.c_str());
        return;
    }
    if (build < 9200) {
        // G3: Win7 has no reliable per-process GPU path (方案 §2.4 G3)
        reason = L"Windows 7";
        SetGradeLocked(g, reason.c_str());
        return;
    }
    // 测试开关：PTM_GPU_FORCE_G2=1 在现代 build 上强制走 G2 路径——
    // 26300 实测系统拒收 Win8+ 时代查询类型，恰好验证「查询被拒 → 如实
    // 空态」的回退行为（不伪造数据）。
    wchar_t force[4] = L"";
    bool forced = GetEnvironmentVariableW(L"PTM_GPU_FORCE_G2", force, 4) > 0;
    if (build < 16299 || forced) {
        // G2 range (Win8 ~ Win10 1607): D3DKMTQueryStatistics path.
        g = GpuGrade::G2;
        reason.clear();
        SetGradeLocked(g, reason.c_str());
        return;
    }
    // G1: PDH GPU Engine exists since 1709; counter add failure below is
    // the RDP / no-GPU detector.
    g = GpuGrade::G1;
    reason.clear();
    SetGradeLocked(g, reason.c_str());
}

bool GpuCollector::EnsureStarted() {
    if (m_running.load()) return true;
    std::wstring why;
    DetectGrade(why);
    GpuGrade g = Grade();
    // 2026-11-10：per-pid 显存走 D3DKMT（PROCESS_QUERY_INFORMATION），
    // SeDebug 一次启用（提权宿主内成功；非提权=dwm 等受保护进程缺席）
    if (!m_se_debug) m_se_debug = EnableSeDebug();
    if (g == GpuGrade::G1) {
        if (!OpenQuery()) {
            // C-7：失败分支经锁内写入通道
            SetGradeLocked(GpuGrade::G3, L"GPU counters unavailable (RDP / no GPU?)");
            m_running.store(false);
            return false;
        }
        // 启动预热（问题①C）：Utilization Percentage 是 cooked 计数器，
        // 需要 2 次采样才有值。这里采基准拍（m_collections=1）；第 2 次
        // 由 plugin_main RefreshTick 冷启动分支跨时间戳（+50ms Sleep）补
        // 齐——探针实测 0ms 间隔第二次 collect 不拒但值全 0。首拍出值
        // 全链路见 RefreshTick 2b 段注释
        m_collections = (PdhCollectQueryData((PDH_HQUERY)m_query) == ERROR_SUCCESS)
                            ? 1 : 0;
        m_running.store(true);
        return true;
    }
    if (g == GpuGrade::G2) {
        if (!InitG2()) {
            // C-7：失败分支经锁内写入通道
            SetGradeLocked(GpuGrade::G3, L"D3DKMT adapters unavailable");
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
// 2026-11-10：手抄最小布局废弃（尾部参数 union 实际偏移 800，
// 手抄版 Param@1048 内核读不到，group/node/segment 参数全部失效），
// 改用文件头 include 的 SDK 官方 d3dkmthk.h 类型直调（gdi32 静态链接，
// D3DKMTQueryStatistics/D3DKMTEnumAdapters 自 Vista 起 gdi32 导出）。
// ============================================================

bool GpuCollector::InitG2() {
    if (!m_g2_adapters.empty()) return true;
    D3DKMT_ENUMADAPTERS ea = {};
    ea.NumAdapters = MAX_ENUM_ADAPTERS;
    if (D3DKMTEnumAdapters(&ea) != 0 || ea.NumAdapters == 0) return false;
    m_g2_adapters.clear();
    for (ULONG i = 0; i < ea.NumAdapters; i++) {
        G2Adapter a;
        a.luid = ea.Adapters[i].AdapterLuid;
        // 适配器统计：NbSegments/NodeCount（无 hProcess）
        D3DKMT_QUERYSTATISTICS q = {};
        q.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
        q.AdapterLuid = a.luid;
        if (D3DKMTQueryStatistics(&q) != 0) continue;
        a.nb_segments = q.QueryResult.AdapterInformation.NbSegments;
        a.node_count = q.QueryResult.AdapterInformation.NodeCount;
        if (!a.node_count || !a.nb_segments) continue;
        // 段属性（Aperture）：WDDM2.1+ 用 SEGMENT_GROUP，不需要；老系统
        // 专用/共享拆分依赖它——探针一次，存档（0xFF=unknown）
        a.seg_aperture.assign(a.nb_segments > 32 ? 32 : a.nb_segments, 0xFF);
        for (ULONG s = 0; s < a.seg_aperture.size(); s++) {
            D3DKMT_QUERYSTATISTICS qs = {};
            qs.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
            qs.AdapterLuid = a.luid;
            qs.QuerySegment.SegmentId = s;
            if (D3DKMTQueryStatistics(&qs) == 0)
                a.seg_aperture[s] =
                    qs.QueryResult.SegmentInformation.Aperture ? 1 : 0;
        }
        // SEGMENT_GROUP 类型可用性（WDDM2.1+ = Win10 1607+）：空 hProcess
        // 的探测调用，接受即走新路径（Win8~1511 落到逐段求和）
        D3DKMT_QUERYSTATISTICS qg = {};
        qg.Type = D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP;
        qg.AdapterLuid = a.luid;
        qg.QueryProcessSegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
        a.seg_group_ok = (D3DKMTQueryStatistics(&qg) == 0);
        m_g2_adapters.push_back(a);
    }
    return !m_g2_adapters.empty();
}

void GpuCollector::StopG2() {
    m_g2_adapters.clear();
    m_g2_last_rt.clear();
    m_g2_last_wall_us = 0;
}

// G2 一拍：候选 pid × 适配器 × 节点查询 + delta 换算（方案 §2.4 G2 行）
bool GpuCollector::TickG2(GpuTickData& out,
                          const std::vector<DWORD>& pid_candidates) {
    if (m_g2_adapters.empty()) return false;

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
                    D3DKMT_QUERYSTATISTICS qg = {};
                    qg.Type = D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP;
                    qg.AdapterLuid = a.luid;
                    qg.hProcess = hp;
                    qg.QueryProcessSegmentGroup =
                        (D3DKMT_MEMORY_SEGMENT_GROUP)g;
                    if (D3DKMTQueryStatistics(&qg) == 0) {
                        ULONGLONG usage =
                            qg.QueryResult.ProcessSegmentGroupInformation.Usage;
                        if (g == 0) { ded += usage; pid_mem_ok = true; }
                        else shr += usage;
                    }
                }
            } else {
                // ---- 老路径：PROCESS_SEGMENT 逐段求和（Win8~1511）----
                for (ULONG s = 0; s < a.seg_aperture.size(); s++) {
                    if (a.seg_aperture[s] == 0xFF) continue;
                    D3DKMT_QUERYSTATISTICS qp = {};
                    qp.Type = D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT;
                    qp.AdapterLuid = a.luid;
                    qp.hProcess = hp;
                    qp.QueryProcessSegment.SegmentId = s;
                    if (D3DKMTQueryStatistics(&qp) == 0) {
                        ULONGLONG committed =
                            qp.QueryResult.ProcessSegmentInformation.BytesCommitted;
                        if (a.seg_aperture[s]) shr += committed;
                        else { ded += committed; pid_mem_ok = true; }
                    }
                }
            }

            // ---- 利用率：PROCESS_NODE 逐节点 RunningTime delta ----
            if (dt_us > 0 && state[ai].size() < a.node_count)
                state[ai].resize(a.node_count, -1);
            for (ULONG n = 0; n < a.node_count && dt_us > 0; n++) {
                D3DKMT_QUERYSTATISTICS qn = {};
                qn.Type = D3DKMT_QUERYSTATISTICS_PROCESS_NODE;
                qn.AdapterLuid = a.luid;
                qn.hProcess = hp;
                qn.QueryProcessNode.NodeId = n;
                if (D3DKMTQueryStatistics(&qn) != 0) { state[ai][n] = -1; continue; }
                LONGLONG rt =
                    qn.QueryResult.ProcessNodeInformation.RunningTime.QuadPart;
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
            // 与 G1 同口径：多适配器/多节点取 max，不做跨适配器相加
            double& slot = out.util_by_pid[pid];
            if (util_pct > slot) slot = util_pct;
            if (util_pct > out.total_util_pct) out.total_util_pct = util_pct;
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
    // 关键：out 是调用方复用的 m_gpu_last。每拍必须先清零，否则
    // util_by_pid/sys_dedicated 会跨拍累加（实测症状：GPU 占用率逐拍
    // 涨到 100% 封顶、显存总量无上限上涨——2026-10-26 用户反馈修复）
    out.util_by_pid.clear();
    out.mem_by_pid.clear();
    out.total_util_pct = 0;
    out.sys_dedicated = 0;
    out.sys_shared = 0;
    out.known_engtypes.clear();
    out.grade = m_grade;              // tick 线程自读（写者同线程，C-7 无竞争）
    out.unsupported_reason = m_grade_reason;
    out.util_valid = out.mem_valid = out.sys_mem_valid = false;
    if (!m_running.load()) return;

    if (m_grade == GpuGrade::G2) {
        TickG2(out, pid_candidates);
        return;
    }
    if (!m_query) return;

    // P-1 治本（2026-11-10 审查）：实例名解析缓存只增不减（GPU 视图
    // 常开时随进程生灭月级累积）——本拍出现的 pid 集作清理依据，Tick
    // 末尾淘汰已消失 pid 的实例键
    std::unordered_set<DWORD> seen_pids;

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
            if (pid) seen_pids.insert(pid);
            if (!in_scope(eng)) continue;
            double v = it.FmtValue.doubleValue;
            if (v < 0) v = 0;
            // 口径（2026-10-26 用户反馈修正）：任务管理器 GPU 列是「该进程
            // 最忙引擎」的利用率，不是各引擎相加。求和会把 3D+Copy+Video
            // 并发引擎叠成 >100%，再被截断成 100%——WorkBuddy 等 Electron
            // 应用因此长期顶格。改为逐进程取 max；系统总量同样取 max。
            double& slot = out.util_by_pid[pid];
            if (v > slot) slot = v;
            if (v > out.total_util_pct) out.total_util_pct = v;
        }
        for (auto& kv : out.util_by_pid) kv.second = cap(kv.second);
        out.total_util_pct = cap(out.total_util_pct);
    }

    // ---- GPU Process Memory: (pid×LUID) 枚举 + D3DKMT 真值 ----
    // 2026-11-10 口径修复：PDH 这组 per-pid 计数器的 cooked 值是只增不减
    // 的累计假账（Starward 实测虚报 660 倍、每秒 +750MB；微软 known-issue
    // 文档在案——任务管理器详情页同源同病，性能页口径才是真值）。PDH 实例
    // 名保留：它是内核记账的 (pid×LUID) 视图清单，天然只含活跃视图（镜像
    // LUID 不会重复出现，DXGI 全枚举会双计）；数值改走 D3DKMT
    // PROCESS_SEGMENT_GROUP 的 Usage（VidMM 会计，与任务管理器性能页同源）。
    std::vector<std::pair<DWORD, LUID>> gpu_pairs;
    auto collect_mem = [&](void* counter) {
        if (!counter) return false;
        if (!GetFormattedArray(counter, m_fmt_buf, items)) return false;
        for (auto& it : items) {
            if (!ValueOk(it.FmtValue)) continue;
            std::wstring inst = it.szName ? it.szName : L"";
            DWORD pid = 0;
            LUID luid = {};
            auto cached = m_pid_cache.find(inst);
            if (cached != m_pid_cache.end()) {
                pid = cached->second.first;
                luid = cached->second.second;
            } else {
                if (!ParsePidInstance(inst, pid, luid)) {
                    m_pid_cache[inst] = { 0, LUID{} };   // negative cache
                    continue;
                }
                m_pid_cache[inst] = { pid, luid };
            }
            if (pid) seen_pids.insert(pid);
            if (!pid) continue;
            bool dup = false;
            for (auto& x : gpu_pairs)
                if (x.first == pid && x.second.LowPart == luid.LowPart &&
                    x.second.HighPart == luid.HighPart) { dup = true; break; }
            if (!dup) gpu_pairs.push_back({ pid, luid });
        }
        return true;
    };
    bool mem_enum_any = collect_mem(m_cnt_mem_ded);
    if (!mem_enum_any && m_cnt_mem_shared)
        mem_enum_any = collect_mem(m_cnt_mem_shared);
    // 无 LUID 实例（ded 与 shared 计数器都缺席）也照常走 D3DKMT 吗——
    // 不：无清单则无 per-pid 维度，按无数据（mem_valid 维持 false 语义
    // 由调用方判定，这里仅不产数据）
    std::unordered_map<DWORD, std::pair<uint64_t, uint64_t>> mem;
    for (auto& pl : gpu_pairs) {
        // per tick open/close：几十次内核调用，微秒级；SeDebug 未启用时
        // 受保护进程（dwm 等）拒开，如实缺席
        HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pl.first);
        if (!hp) continue;
        uint64_t lo = 0, nl = 0;
        if (QueryProcessSegmentUsage(hp, pl.second, lo, nl)) {
            auto& slot = mem[pl.first];
            slot.first += lo;    // 跨 LUID 求和（进程在多 GPU 各有记账）
            slot.second += nl;
        }
        CloseHandle(hp);
    }
    for (auto& kv : mem)
        out.mem_by_pid[kv.first] = { kv.second.first, kv.second.second };

    // ---- GPU Adapter Memory (system-wide, summary line) ----
    // adapter 级计数器是 gauge 真值（与任务管理器性能页一致，实测无假账）。
    // 实例名 luid_*_phys_N 无 pid_ 前缀——不走 pid 解析，全实例直加。
    auto collect_adapter = [&](void* counter, uint64_t& dst) {
        if (!counter) return false;
        if (!GetFormattedArray(counter, m_fmt_buf, items)) return false;
        for (auto& it : items) {
            if (!ValueOk(it.FmtValue)) continue;
            double v = it.FmtValue.doubleValue;
            if (v > 0) dst += (uint64_t)v;
        }
        return true;
    };
    uint64_t adapt_ded = 0, adapt_shr = 0;
    bool adapt_ok = collect_adapter(m_cnt_adapt_ded, adapt_ded);
    if (m_cnt_adapt_shared) collect_adapter(m_cnt_adapt_shared, adapt_shr);
    if (adapt_ok) {
        out.sys_mem_valid = true;
        out.sys_dedicated += adapt_ded;
        out.sys_shared += adapt_shr;
    } else {
        // fallback: sum the per-process view (still an honest system total)
        for (auto& kv : mem) {
            out.sys_dedicated += kv.second.first;
            out.sys_shared += kv.second.second;
        }
        out.sys_mem_valid = out.mem_valid;
    }

    // P-1 治本收尾：清掉本拍未出现的 pid 实例键（PDH 实例随进程退出即
    // 消失，键继续挂着只会积灰）；负缓存项（pid=0，解析失败，如适配器
    // 级实例名）保留——清了每拍重新解析失败再负缓存，纯抖动
    for (auto it = m_eng_cache.begin(); it != m_eng_cache.end();)
        it = (it->second.first && !seen_pids.count(it->second.first))
                 ? m_eng_cache.erase(it) : ++it;
    for (auto it = m_pid_cache.begin(); it != m_pid_cache.end();)
        it = (it->second.first && !seen_pids.count(it->second.first))
                 ? m_pid_cache.erase(it) : ++it;
}

// ============================================================
// EnumerateEngines — one-shot engtype list for the options dialog (M4)
// Independent throwaway query: safe to run from the UI thread while the
// tick-thread query is live (PDH supports concurrent queries).
// ============================================================
std::vector<std::wstring> GpuCollector::EnumerateEngines() {
    std::vector<std::wstring> out;
    DWORD build = 0;
    if (!PTM_GetOSBuild(build) || build < 16299) return out;   // G2/G3：无引擎命名
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return out;
    PDH_HCOUNTER c = nullptr;
    if (AddCounter(q, kPathUtil, (void**)&c)) {
        PdhCollectQueryData(q);   // 实例名首拍即现；利用率数值不取
        std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> items;
        std::vector<BYTE> buf;
        if (GetFormattedArray(c, buf, items)) {
            std::unordered_set<std::wstring> set;
            for (auto& it : items) {
                std::wstring inst = it.szName ? it.szName : L"";
                DWORD pid = 0;
                std::wstring eng;
                if (ParseEngineInstance(inst, pid, eng) && !eng.empty())
                    set.insert(eng);
            }
            out.assign(set.begin(), set.end());
            std::sort(out.begin(), out.end());
        }
    }
    if (q) PdhCloseQuery(q);
    return out;
}
