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
};

} // namespace

void TopAggregator::Build(const CpuMemIoMetrics& m,
                          const EtwFileCollector::IoDelta* io_delta,
                          double dt,
                          const AggregateSettings& cfg,
                          MetricsCollector& mc,
                          MetricsSnapshot& out) {
    out.cpu.rows.clear(); out.mem.rows.clear();
    out.io.rows.clear(); out.io_read.rows.clear(); out.io_write.rows.clear();
    out.cpu_total_pct = m.cpu_total_pct;
    out.mem_total = m.mem_total;
    out.mem_used = m.mem_used;
    out.stamp = GetTickCount64();

    // ---- merge rows by process name ----
    // Empty name (protected process) -> "<pid>" placeholder, one row each
    // (they are distinct processes even if unnamed). Same-name merge sums
    // memory (independent address spaces — summing is correct) and keeps
    // the largest contributor's pid as the representative (icon + 定位文件).
    std::vector<Acc> merged;
    std::unordered_map<std::wstring, size_t> by_name;
    merged.reserve(m.procs.size());
    bool etw_exact = io_delta && io_delta->valid && dt > 0;

    for (const auto& p : m.procs) {
        double io_r = p.io_read_bps, io_w = p.io_write_bps;
        // exact ETW file view (grade A/B) overrides the all-handle counters
        if (etw_exact) {
            io_r = io_w = 0;
            auto it = io_delta->per_pid.find(p.pid);
            if (it != io_delta->per_pid.end()) {
                io_r = (double)it->second.first / dt;
                io_w = (double)it->second.second / dt;
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
            merged.push_back(std::move(a));
        } else {
            Acc& a = merged[it->second];
            a.count++;
            a.cpu += p.cpu_pct;
            a.mem += p.mem;
            a.io_r += io_r;
            a.io_w += io_w;
            // representative = biggest live contributor by CPU (stable pick)
            if (p.cpu_pct > 0.01 && a.main_pid == 0) a.main_pid = p.pid;
        }
    }

    // ---- short-lived processes seen by ETW but already gone from the
    // snapshot: name-cache fallback so they still rank (方案 §5.2 第 3 步) —
    // only in the exact view; C-grade counters only see live snapshots.
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
            double r = (double)kv.second.first / dt;
            double w = (double)kv.second.second / dt;
            auto it = by_name.find(nm);
            if (it == by_name.end()) {
                by_name[nm] = merged.size();
                Acc a;
                a.name = nm;
                a.main_pid = 0;         // gone: no icon / no file location
                a.count = 1;
                a.io_r = r;
                a.io_w = w;
                merged.push_back(std::move(a));
            } else {
                Acc& a = merged[it->second];
                a.count++;
                a.io_r += r;
                a.io_w += w;
            }
        }
    }

    // ---- cut Top N per metric ----
    int n = cfg.topn;
    if (n < 3) n = 3;
    if (n > 15) n = 15;

    auto fill = [&](MetricView& v, auto getter, bool has_value) {
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
            v.rows.push_back(std::move(r));
        }
    };

    bool has_delta = !m.first;
    fill(out.cpu, [](const Acc* a) { return a->cpu; }, has_delta);
    fill(out.mem, [](const Acc* a) { return (double)a->mem; }, true);
    fill(out.io, [](const Acc* a) { return a->io_r + a->io_w; }, has_delta);
    fill(out.io_read, [](const Acc* a) { return a->io_r; }, has_delta);
    fill(out.io_write, [](const Acc* a) { return a->io_w; }, has_delta);

    // totals for the summary line
    if (etw_exact) {
        uint64_t r = 0, w = 0;
        for (const auto& kv : io_delta->per_pid) { r += kv.second.first; w += kv.second.second; }
        out.io_read_total_bps = (double)r / dt;
        out.io_write_total_bps = (double)w / dt;
    } else {
        double r = 0, w = 0;
        for (const auto& p : m.procs) { r += p.io_read_bps; w += p.io_write_bps; }
        out.io_read_total_bps = r;
        out.io_write_total_bps = w;
    }
    out.io_grade = etw_exact ? FileIoGrade::A : FileIoGrade::C;   // caller overwrites with the true grade (A/B/C)
    out.io_events_lost = io_delta ? io_delta->events_lost : 0;
}
