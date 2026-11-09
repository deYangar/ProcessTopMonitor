// core_class_mapper.cpp — 核型分类器实现（方案 §3）
#include "core_class_mapper.h"
#include "utils.h"
#include <cstdio>
#include <cstdarg>
#include <unordered_set>

// 轻量日志：debug\core_class.log（构建一次最多几行；默认总是写——分类
// 结果是「功能为什么隐藏」的第一手证据，量级可忽略）
static void CcLog(const wchar_t* fmt, ...) {
    wchar_t dir[MAX_PATH] = L"";
    if (!PTM_GetDebugDir(dir, MAX_PATH)) return;
    wchar_t path[MAX_PATH];
    if (swprintf_s(path, MAX_PATH, L"%s\\core_class.log", dir) <= 0) return;
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

CoreClassMap ClassifyFromRaw(
    const std::vector<CpuSetEntryRaw>& cpu_sets,
    const std::vector<L3ClusterRaw>& l3_clusters,
    const std::vector<int>& group_active_counts) {

    CoreClassMap m;
    // 组号 → 绝对 LP 起点（group-major；未活跃组计数 0，索引连续无洞）
    std::vector<int> base(group_active_counts.size() + 1, 0);
    int total = 0;
    for (size_t g = 0; g < group_active_counts.size(); g++) {
        base[g] = total;
        total += group_active_counts[g];
    }
    base[group_active_counts.size()] = total;
    m.total_lp = total;
    if (total <= 0) return m;   // 防御：无数据 → 同构

    // ---- 判定 1：EfficiencyClass ≥2 个值 → Intel P/E（§3.3）----
    std::unordered_set<int> classes;
    for (auto& e : cpu_sets) classes.insert((int)e.efficiency_class);

    if (classes.size() >= 2) {
        m.heterogeneous = true;
        m.x3d_style = false;
        m.lp.assign((size_t)total, CoreClass::Homog);
        for (auto& e : cpu_sets) {
            if (e.group >= group_active_counts.size()) continue;   // 防御：组号越界
            int abs = base[e.group] + (int)e.lp_in_group;
            if (abs < 0 || abs >= total) continue;                 // 防御：组内号越界
            if (e.efficiency_class > 0) {
                m.lp[(size_t)abs] = CoreClass::P;
                m.class_lp_count[1]++;
            } else {
                m.lp[(size_t)abs] = CoreClass::E;                  // LP-E 天然并入（§0 拍板）
                m.class_lp_count[0]++;
            }
        }
        // 防御：某核无 cpu_set 条目（理论不发生）→ 保持 Homog，
        // 消费端「越界/Homog 丢弃」语义兜住
        if (!m.class_lp_count[0] || !m.class_lp_count[1]) {
            // 只有单边有效 = 数据不完整，按同构隐藏（不造假）
            m = CoreClassMap{};
            m.total_lp = total;
        }
        return m;
    }

    // ---- 判定 2：全同 class 且 L3 簇 ≥2 种 CacheSize → AMD X3D（§3.3）----
    std::unordered_set<uint64_t> sizes;
    for (auto& c : l3_clusters) sizes.insert(c.cache_size);
    if (sizes.size() >= 2) {
        uint64_t max_size = 0;
        for (auto& c : l3_clusters)
            if (c.cache_size > max_size) max_size = c.cache_size;
        m.heterogeneous = true;
        m.x3d_style = true;
        m.lp.assign((size_t)total, CoreClass::Homog);
        for (auto& c : l3_clusters) {
            CoreClass cls = (c.cache_size == max_size) ? CoreClass::VCache
                                                       : CoreClass::StdCcd;
            for (int abs : c.lp_members) {
                if (abs < 0 || abs >= total) continue;   // 防御：越界成员丢弃
                // 同一 LP 落两个簇（数据异常）：先到先得，不叠加计数
                if (m.lp[(size_t)abs] == CoreClass::Homog) {
                    m.lp[(size_t)abs] = cls;
                    m.class_lp_count[cls == CoreClass::VCache ? 1 : 0]++;
                }
            }
        }
        if (!m.class_lp_count[0] || !m.class_lp_count[1]) {
            m = CoreClassMap{};
            m.total_lp = total;
        }
        return m;
    }

    // ---- 判定 3：同构（含 7950X 等大 L3 双 CCD——无差异可分，隐藏正确）----
    return m;
}

// ============================================================
// 真实 API 构建（BuildCoreClassMap）
// ============================================================

// GetSystemCpuSetInformation 是 Win10 1709+ API——绝不进导入表（Win7/早期
// Win10 上 loader 直接拒载 DLL），GetProcAddress 动态绑定（§3.4）
typedef BOOL(WINAPI* PFN_GetSystemCpuSetInformation)(
    PSYSTEM_CPU_SET_INFORMATION Information, DWORD BufferLength,
    PDWORD ReturnedLength, HANDLE Process, ULONG Flags);

static bool CollectCpuSets(std::vector<CpuSetEntryRaw>& out,
                           std::vector<int>& group_counts) {
    auto fn = (PFN_GetSystemCpuSetInformation)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation");
    if (!fn) return false;   // Win10 <1709：该年代无混架 CPU，同构隐藏

    DWORD need = 0;
    // 长度探测的正确形态 = FALSE + ERROR_INSUFFICIENT_BUFFER + need 填充
    //（本机实测 2026-11-09：ok=0 need=768 gle=122）——只认 need
    fn(nullptr, 0, &need, GetCurrentProcess(), 0);
    if (need == 0) return false;
    std::vector<BYTE> buf(need);
    DWORD got = 0;
    if (!fn((PSYSTEM_CPU_SET_INFORMATION)buf.data(), need, &got,
            GetCurrentProcess(), 0))
        return false;

    WORD max_group = 0;
    out.clear();
    DWORD off = 0;
    while (off + sizeof(DWORD) <= got) {
        auto* e = (PSYSTEM_CPU_SET_INFORMATION)(buf.data() + off);
        if (e->Type == CPU_SET_INFORMATION_TYPE::CpuSetInformation) {
            // 结构布局：现行官方文档与 SDK 26100 一致（平铺 Group WORD +
            // LogicalProcessorIndex BYTE，无 Affinity 字段——2026-11-09 对
            // learn.microsoft.com 核验）；全定长字段，x86 WOW64 同布局
            CpuSetEntryRaw r;
            r.group = e->CpuSet.Group;
            r.lp_in_group = e->CpuSet.LogicalProcessorIndex;
            r.efficiency_class = e->CpuSet.EfficiencyClass;
            out.push_back(r);
            if (r.group > max_group) max_group = r.group;
        }
        if (e->Size < sizeof(DWORD)) break;   // 防御：0 尺寸防死循环
        off += e->Size;
    }
    // group_counts 按组号索引（稀疏组号补 0——cpu_sets 每活跃逻辑核一条，
    // 条目数 = 组内活跃 LP 数，天然是换算基准，§3.2）
    group_counts.assign((size_t)max_group + 1, 0);
    for (auto& r : out) group_counts[r.group]++;
    return !out.empty();
}

static bool CollectL3Clusters(std::vector<L3ClusterRaw>& out,
                              const std::vector<int>& group_counts) {
    // group → 绝对 LP 起点（与 ClassifyFromRaw 同式）
    std::vector<int> base(group_counts.size(), 0);
    int total = 0;
    for (size_t g = 0; g < group_counts.size(); g++) {
        base[g] = total;
        total += group_counts[g];
    }

    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
    if (len == 0) return false;
    std::vector<BYTE> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationCache,
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len))
        return false;

    out.clear();
    DWORD off = 0;
    while (off < len) {
        auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
        if (e->Relationship == RelationCache && e->Cache.Level == 3) {
            L3ClusterRaw c;
            c.cache_size = (uint64_t)e->Cache.CacheSize;
            for (WORD g = 0; g < e->Cache.GroupCount; g++) {
                WORD grp = e->Cache.GroupMasks[g].Group;
                ULONG_PTR mask = e->Cache.GroupMasks[g].Mask;
                if (grp >= group_counts.size()) continue;   // 防御：无活跃核的组
                for (WORD b = 0; b < sizeof(ULONG_PTR) * 8; b++) {
                    if (mask & ((ULONG_PTR)1 << b)) {
                        int abs = base[grp] + (int)b;
                        if (abs < total) c.lp_members.push_back(abs);
                    }
                }
            }
            out.push_back(std::move(c));
        }
        if (e->Size < sizeof(DWORD)) break;
        off += e->Size;
    }
    return true;
}

CoreClassMap BuildCoreClassMap() {
    std::vector<CpuSetEntryRaw> sets;
    std::vector<int> group_counts;
    if (!CollectCpuSets(sets, group_counts)) {
        CcLog(L"[core_class] GetSystemCpuSetInformation unavailable -> Homog");
        return {};
    }
    std::vector<L3ClusterRaw> l3;
    CollectL3Clusters(l3, group_counts);   // L3 拿不到 = 无 X3D 判定输入，Intel 路径不受影响

    CoreClassMap m = ClassifyFromRaw(sets, l3, group_counts);
    if (m.heterogeneous) {
        // 分类证据一行留档（标签风格 / 两群核数 / L3 簇形态）
        wchar_t l3info[128] = L"";
        if (!l3.empty()) {
            std::unordered_set<uint64_t> sz;
            for (auto& c : l3) sz.insert(c.cache_size);
            swprintf_s(l3info, 128, L" l3_clusters=%zu distinct_sizes=%zu",
                       l3.size(), sz.size());
        }
        CcLog(L"[core_class] heterogeneous: style=%s p/vcache=%d e/std=%d total=%d%s",
              m.x3d_style ? L"x3d" : L"p/e",
              m.class_lp_count[1], m.class_lp_count[0], m.total_lp, l3info);
    } else {
        CcLog(L"[core_class] homogeneous (total_lp=%d) -> hidden", m.total_lp);
    }
    return m;
}
