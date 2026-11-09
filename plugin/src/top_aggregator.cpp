// top_aggregator.cpp — same-name merge + per-metric Top N
#include "top_aggregator.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace {

struct Acc {
    std::wstring name;
    DWORD main_pid = 0;
    int count = 0;
    double cpu = 0;
    uint64_t mem = 0;
    double io_r = 0, io_w = 0;
    double gpu = 0;              // %, scope+clamp applied by gpu_collector
    uint64_t vram_ded = 0, vram_shared = 0;
    // 核型样本占比（§4.3：窗口样本数/全部样本 ×100，全机容量口径）
    double cpu_p = 0, cpu_e = 0;
};

} // namespace

void TopAggregator::Build(const CpuMemIoMetrics& m,
                          const EtwFileCollector::IoDelta* io_delta,
                          const GpuTickData* gpu,
                          const AggregateSettings& cfg,
                          MetricsCollector& mc,
                          MetricsSnapshot& out) {
    out.cpu.rows.clear(); out.mem.rows.clear();
    out.io.rows.clear(); out.io_read.rows.clear(); out.io_write.rows.clear();
    out.gpu.rows.clear(); out.vram.rows.clear();
    out.cpu_p.rows.clear(); out.cpu_e.rows.clear(); out.cpu_pe.rows.clear();
    out.cpu_total_pct = m.cpu_total_pct;
    out.mem_total = m.mem_total;
    out.mem_used = m.mem_used;
    out.stamp = GetTickCount64();

    // 系统级核群占用率透传（§4.5；同构时恒 -1 → 摘要不显示）
    out.cpu_p_total_pct = m.cpu_split_total_pct[1];
    out.cpu_e_total_pct = m.cpu_split_total_pct[0];

    // 核型样本占比（§4.3）：sample_valid = 异构且交付窗口完整。占比 =
    // 该 pid 该类样本 / 全部样本（分母含 Idle/System/unknown = 全机容量
    // 的采样投影；免疫采样间隔差异与均匀丢包）
    bool split_valid = io_delta && io_delta->sample_valid && io_delta->sample_total > 0;
    double pfac = split_valid ? 100.0 / (double)io_delta->sample_total : 0.0;
    out.cpu_split_valid = split_valid;

    // 精确口径速率分母（方案三窗口对齐）：handoff 真实窗口，tick 假 dt
    // 不再参与——首窗几十 ms 的高估/低估、稳态 handoff 时机错位一并根治
    double io_dt = io_delta ? (double)io_delta->window_us / 1e6 : 0.0;

    // ---- merge rows by process name ----
    // Empty name (protected process) -> "<pid>" placeholder, one row each
    // (they are distinct processes even if unnamed). Same-name merge sums
    // memory (independent address spaces — summing is correct) and keeps
    // the largest contributor's pid as the representative (icon + 定位文件).
    std::vector<Acc> merged;
    std::unordered_map<std::wstring, size_t> by_name;
    merged.reserve(m.procs.size());
    bool etw_exact = io_delta && io_delta->valid && io_dt > 0;

    for (const auto& p : m.procs) {
        double io_r = p.io_read_bps, io_w = p.io_write_bps;
        // exact ETW file view (grade A/B) overrides the all-handle counters
        if (etw_exact) {
            io_r = io_w = 0;
            auto it = io_delta->per_pid.find(p.pid);
            if (it != io_delta->per_pid.end()) {
                io_r = (double)it->second.first / io_dt;
                io_w = (double)it->second.second / io_dt;
            }
        }
        double sp_p = 0, sp_e = 0;
        if (split_valid) {
            auto sit = io_delta->sample_pid.find(p.pid);
            if (sit != io_delta->sample_pid.end()) {
                sp_p = (double)sit->second.first * pfac;
                sp_e = (double)sit->second.second * pfac;
            }
        }
        double gpu_pct = 0;
        uint64_t vram_d = 0, vram_s = 0;
        if (gpu) {
            auto gu = gpu->util_by_pid.find(p.pid);
            if (gu != gpu->util_by_pid.end()) gpu_pct = gu->second;
            auto gm = gpu->mem_by_pid.find(p.pid);
            if (gm != gpu->mem_by_pid.end()) {
                vram_d = gm->second.first;
                vram_s = gm->second.second;
            }
        }
        const std::wstring& key = p.name;
        if (key.empty()) continue;   // unnamed protected rows (no name to merge by)
        auto it = by_name.find(key);
        if (it == by_name.end()) {
            by_name[key] = merged.size();
            Acc a;
            a.name = key;
            a.main_pid = p.pid;
            a.count = 1;
            a.cpu = p.cpu_pct;
            a.mem = p.mem;
            a.io_r = io_r;
            a.io_w = io_w;
            a.gpu = gpu_pct;
            a.vram_ded = vram_d;
            a.vram_shared = vram_s;
            a.cpu_p = sp_p;
            a.cpu_e = sp_e;
            merged.push_back(std::move(a));
        } else {
            Acc& a = merged[it->second];
            a.count++;
            a.cpu += p.cpu_pct;
            a.mem += p.mem;
            a.io_r += io_r;
            a.io_w += io_w;
            // GPU 利用率取 max（同名多进程 = 同一应用，任务管理器口径是
            // 「最忙的那个进程/引擎」；相加会把 11 个 Electron 子进程叠成
            // 100%——2026-10-26 用户反馈修复）
            if (gpu_pct > a.gpu) a.gpu = gpu_pct;
            a.vram_ded += vram_d;
            a.vram_shared += vram_s;
            // 核型占比按同名进程累加（分母统一全机样本，可加）
            a.cpu_p += sp_p;
            a.cpu_e += sp_e;
            // representative = biggest live contributor by CPU (stable pick)
            if (p.cpu_pct > 0.01 && a.main_pid == 0) a.main_pid = p.pid;
        }
    }

    // ---- short-lived processes seen by ETW but already gone from the
    // snapshot: name-cache fallback so they still rank (方案 §5.2 第 3 步) —
    // only in the exact view; C-grade counters only see live snapshots.
    // 核型样本同款兜底（§9 短命进程行）：sample 通道与 IO 通道同一窗口，
    // etw_exact 成立时 split_valid 才可能成立，共用本块的 live 检查
    if (etw_exact) {
        std::unordered_set<DWORD> live_pids;
        live_pids.reserve(m.procs.size() + 8);
        for (const auto& p : m.procs) live_pids.insert(p.pid);
        for (const auto& kv : io_delta->per_pid) {
            if (kv.second.first == 0 && kv.second.second == 0) continue;
            if (live_pids.count(kv.first)) continue;
            std::wstring nm = mc.LookupName(kv.first);
            if (nm.empty()) {
                wchar_t tmp[32];
                swprintf_s(tmp, L"<PID %lu>", kv.first);
                nm = tmp;
            }
            double r = (double)kv.second.first / io_dt;
            double w = (double)kv.second.second / io_dt;
            // 该 pid 的核型占比（有则并入同名 Acc）
            double sp_p = 0, sp_e = 0;
            if (split_valid) {
                auto sit = io_delta->sample_pid.find(kv.first);
                if (sit != io_delta->sample_pid.end()) {
                    sp_p = (double)sit->second.first * pfac;
                    sp_e = (double)sit->second.second * pfac;
                }
            }
            auto it = by_name.find(nm);
            if (it == by_name.end()) {
                by_name[nm] = merged.size();
                Acc a;
                a.name = nm;
                a.main_pid = 0;         // gone: no icon / no file location
                a.count = 0;            // C4：死 pid 不计入 count（下同）——
                                       // merged 是全部 7 个视图共享数据源，
                                       // 计入会让 cpu/mem/gpu 视图的
                                       //「×N」合并数含已退出进程
                a.io_r = r;
                a.io_w = w;
                a.cpu_p = sp_p;
                a.cpu_e = sp_e;
                merged.push_back(std::move(a));
            } else {
                Acc& a = merged[it->second];
                a.io_r += r;
                a.io_w += w;
                a.cpu_p += sp_p;
                a.cpu_e += sp_e;
            }
        }
        // 只在核型样本里出现的短命进程（窗口内烧 CPU 后退出、无 IO）：
        // 同名合并进 merged，让 cpu_p/cpu_e 视图也能排上（占比口径自带排序值）
        if (split_valid) {
            for (const auto& kv : io_delta->sample_pid) {
                if (kv.second.first == 0 && kv.second.second == 0) continue;
                if (live_pids.count(kv.first)) continue;
                // 已被 IO 块并入的跳过（by_name 查得到合并过同名 Acc——
                // 简化：IO 块只处理 per_pid 里的 pid，这里查 sample-only）
                std::wstring nm = mc.LookupName(kv.first);
                if (nm.empty()) {
                    wchar_t tmp[32];
                    swprintf_s(tmp, L"<PID %lu>", kv.first);
                    nm = tmp;
                }
                double sp_p = (double)kv.second.first * pfac;
                double sp_e = (double)kv.second.second * pfac;
                auto it = by_name.find(nm);
                if (it == by_name.end()) {
                    by_name[nm] = merged.size();
                    Acc a;
                    a.name = nm;
                    a.main_pid = 0;
                    a.count = 0;         // C4：死 pid 不计入
                    a.cpu_p = sp_p;
                    a.cpu_e = sp_e;
                    merged.push_back(std::move(a));
                } else {
                    Acc& a = merged[it->second];
                    // 同 pid 已在 IO 块加过（per_pid 有它时）——重复加会
                    // 翻倍。防御：只加 IO 块没碰过的 pid（per_pid 查无）
                    if (io_delta->per_pid.find(kv.first) == io_delta->per_pid.end()) {
                        a.cpu_p += sp_p;
                        a.cpu_e += sp_e;
                    }
                }
            }
        }
    }

    // ---- cut Top N per metric ----
    int n = cfg.topn;
    if (n < 3) n = 3;
    if (n > 15) n = 15;

    auto fill = [&](MetricView& v, auto getter, bool has_value, auto aux) {
        v.has_value = has_value;
        if (!has_value) return;
        std::vector<const Acc*> list;
        list.reserve(merged.size());
        for (auto& a : merged) list.push_back(&a);
        std::stable_sort(list.begin(), list.end(),
                         [&](const Acc* x, const Acc* y) {
                             return getter(x) > getter(y);
                         });
        size_t cnt = (size_t)n;
        if (cnt > list.size()) cnt = list.size();
        v.rows.reserve(cnt);
        for (size_t i = 0; i < cnt; i++) {
            TopRow r;
            r.name = list[i]->name;
            r.pid = list[i]->main_pid;
            r.count = list[i]->count;
            r.value = getter(list[i]);
            aux(list[i], r);
            v.rows.push_back(std::move(r));
        }
    };
    auto no_aux = [](const Acc*, TopRow&) {};

    // 核型占比 passthrough（§5 TopRow 先例同款）：cpu/cpu_p/cpu_e 三个视图
    // 的行都带 P/E 明细——CPU 视图 tooltip 取用，核型视图自身亦可显示
    auto split_aux = [](const Acc* a, TopRow& r) {
        r.cpu_p_pct = a->cpu_p;
        r.cpu_e_pct = a->cpu_e;
    };

    bool has_delta = !m.first;
    fill(out.cpu, [](const Acc* a) { return a->cpu; }, has_delta, split_aux);
    fill(out.cpu_p, [](const Acc* a) { return a->cpu_p; }, split_valid, split_aux);
    fill(out.cpu_e, [](const Acc* a) { return a->cpu_e; }, split_valid, split_aux);
    // 核型面板视图（单面板双列）：排序键 max(P,E)——IO 合计 max(r,w)
    // 同款语义（纯 E 群大户不被 P+E 合计里的 P 大户淹没），行内 P/E
    // 双列值由 split_aux passthrough 带出
    fill(out.cpu_pe, [](const Acc* a) { return std::max(a->cpu_p, a->cpu_e); },
         split_valid, split_aux);
    fill(out.mem, [](const Acc* a) { return (double)a->mem; }, true, no_aux);
    // 合计视图（问题③）：排序键 = max(read, write)——纯读/纯写大户不被
    // 双边合计淹没，与「读写分开显示」的意图自洽；r/w 拆分随行带出供
    // 双列显示（仅读/仅写子视图维持单列，排序键照旧单边）
    fill(out.io, [](const Acc* a) { return std::max(a->io_r, a->io_w); },
         has_delta,
         [](const Acc* a, TopRow& r) {
             r.io_read_bps = a->io_r;
             r.io_write_bps = a->io_w;
         });
    fill(out.io_read, [](const Acc* a) { return a->io_r; }, has_delta, no_aux);
    fill(out.io_write, [](const Acc* a) { return a->io_w; }, has_delta, no_aux);
    bool gpu_ok = gpu && gpu->util_valid;
    bool vram_ok = gpu && gpu->mem_valid;
    fill(out.gpu, [](const Acc* a) { return a->gpu; }, gpu_ok, no_aux);
    fill(out.vram, [&](const Acc* a) {
        return (double)(cfg.vram_with_shared ? a->vram_ded + a->vram_shared
                                             : a->vram_ded);
    }, vram_ok, no_aux);

    // totals for the summary line
    if (etw_exact) {
        uint64_t r = 0, w = 0;
        for (const auto& kv : io_delta->per_pid) { r += kv.second.first; w += kv.second.second; }
        out.io_read_total_bps = (double)r / io_dt;
        out.io_write_total_bps = (double)w / io_dt;
    } else {
        double r = 0, w = 0;
        for (const auto& p : m.procs) { r += p.io_read_bps; w += p.io_write_bps; }
        out.io_read_total_bps = r;
        out.io_write_total_bps = w;
    }
    out.io_grade = etw_exact ? FileIoGrade::A : FileIoGrade::C;   // caller overwrites with the true grade (A/B/C)
    out.io_events_lost = io_delta ? io_delta->events_lost : 0;

    // GPU/VRAM summary + grade passthrough (窗口状态行/口径菜单用)
    if (gpu) {
        out.gpu_total_pct = gpu->total_util_pct;
        out.gpu_total_valid = gpu->util_valid;
        out.vram_sys_dedicated = gpu->sys_dedicated;
        out.vram_sys_shared = gpu->sys_shared;
        out.vram_sys_valid = gpu->sys_mem_valid;
        out.gpu_grade = gpu->grade;
        out.gpu_reason = gpu->unsupported_reason;
        out.gpu_engtypes = gpu->known_engtypes;
    } else {
        out.gpu_total_valid = false;
        out.vram_sys_valid = false;
    }
}
