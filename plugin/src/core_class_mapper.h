#pragma once
// ============================================================
// core_class_mapper — 混架 CPU 核型分类器
// （方案 docs/plans/2026-11-09-混架核型拆分方案.md §3）
//
// 启动时一次构建：GetSystemCpuSetInformation（EfficiencyClass，Intel P/E
// 判定）+ GetLogicalProcessorInformationEx(RelationCache)（L3 簇 CacheSize，
// AMD X3D 判定）→ CoreClassMap（绝对 LP 号 → 核型）。拓扑运行期视为
// 不变，插件重启重算（§3.4）。
//
// 索引统一（§3.2）：绝对 LP 号 = Σ_{g'<g} group_active_counts[g'] + 组内号
// （group-major）。三个数据源（CPU set / RelationCache mask / ETW
// ProcessorIndex / NtQSI 每核数组）全部折算到该空间。
//
// 判定树（§3.3）：
//   1. EfficiencyClass 出现 ≥2 个值 → Intel 口径：class>0 = P，class 0 = E
//      （LP-E 天然并入 E）
//   2. 全同 class 且 L3 簇存在 ≥2 种 CacheSize → AMD X3D 口径：
//      最大簇群 = VCache，其余 = StdCcd
//   3. 否则同构（含 7950X 等大 L3 双 CCD）——功能整体隐藏
// ============================================================
#include <windows.h>
#include <vector>
#include <cstdint>

enum class CoreClass { Homog, P, E, VCache, StdCcd };

struct CoreClassMap {
    bool heterogeneous = false;   // false = 同构，功能整体隐藏
    bool x3d_style = false;       // true: VCache/StdCcd 标签；false: P/E 标签
    int total_lp = 0;             // 全机逻辑核数
    int class_lp_count[2] = {0,0};// [0]=E/StdCcd 群, [1]=P/VCache 群
    std::vector<CoreClass> lp;    // 绝对 LP 号 → 类型，size == total_lp
};

// ---- 纯函数单测入口（假拓扑 → 分类结果，不碰真实 API）----
// cpu_sets：GetSystemCpuSetInformation 每逻辑核一条（group + 组内号 + class）
// l3_clusters：L3 簇（成员绝对 LP 集 + CacheSize 字节）
// group_active_counts：每组活跃 LP 数（绝对号换算基准）
struct CpuSetEntryRaw {
    WORD group = 0;
    WORD lp_in_group = 0;
    BYTE efficiency_class = 0;
};
struct L3ClusterRaw {
    std::vector<int> lp_members;   // 绝对 LP 号
    uint64_t cache_size = 0;       // bytes
};

CoreClassMap ClassifyFromRaw(
    const std::vector<CpuSetEntryRaw>& cpu_sets,
    const std::vector<L3ClusterRaw>& l3_clusters,
    const std::vector<int>& group_active_counts);

// 真实 API 构建（Start 时 tick 线程调用一次）。失败形态一律同构 + 日志
// 一行（§3.4：不崩溃不造假）——Win10 <1709 无 API / 权限 / 意外错误。
CoreClassMap BuildCoreClassMap();
