// amr_bench — micro benchmarks for the Stage-1 prototype.
// Usage: amr_bench [scale=1]
#include "../tests/testkit.hpp"

#include <chrono>

using namespace amr;
using clk = std::chrono::steady_clock;

static double us(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

int main(int argc, char** argv) {
    const int scale = argc > 1 ? std::atoi(argv[1]) : 1;
    std::printf("| benchmark | value |\n|---|---|\n");

    for (bool fs : {false, true}) {
        auto dir = tk::tmpdir("b-log");
        LogOptions o;
        o.fsync = fs;
        const int n = fs ? 2000 : 200000 * scale;
        auto log = AppendLog::open(dir, o);
        std::string p(128, 'p');
        auto t0 = clk::now();
        for (int i = 0; i < n; ++i)
            log->append(RecType::MemPut, 0, p);
        auto t1 = clk::now();
        std::printf("| log append 128B payload, fsync=%d | %.0f rec/s (%.2f us/rec, n=%d) |\n", fs,
                    n / (us(t0, t1) / 1e6), us(t0, t1) / n, n);
        if (!fs) {
            log.reset();
            LogRecoveryReport rep;
            auto t2 = clk::now();
            auto l2 = AppendLog::open(dir, LogOptions{}, &rep);
            std::uint64_t c = 0;
            l2->for_each(0, [&](const LogRecord&) { ++c; });
            auto t3 = clk::now();
            std::printf("| log open+verify+scan %llu records (%zu segments) | %.1f ms |\n",
                        (unsigned long long)c, rep.segments, us(t2, t3) / 1e3);
        }
        std::filesystem::remove_all(dir);
    }

    {
        auto dir = tk::tmpdir("b-rec");
        const int n = 50000 * scale;
        {
            auto rt = Runtime::open(dir, tk::fast_opts());
            tk::seed_rules(*rt);
            tk::seed_memories(*rt, n);
            rt->checkpoint();
            tk::seed_memories(*rt, n / 10, 50, 3);
        }
        auto o = tk::fast_opts();
        o.use_checkpoint = false;
        auto t0 = clk::now();
        auto full = Runtime::open(dir, o);
        auto t1 = clk::now();
        full.reset();
        auto t2 = clk::now();
        auto ck = Runtime::open(dir, tk::fast_opts());
        auto t3 = clk::now();
        std::printf("| recovery full replay (%d records) | %.1f ms |\n", n + n / 10 + 2, us(t0, t1) / 1e3);
        std::printf("| recovery checkpoint + %llu-record tail | %.1f ms |\n",
                    (unsigned long long)ck->recovery().replayed_records, us(t2, t3) / 1e3);
        std::filesystem::remove_all(dir);
    }

    for (int rules : {10, 100, 1000, 10000}) {
        auto dir = tk::tmpdir("b-wl");
        auto o = tk::fast_opts();
        o.max_active_worldlines = 1 << 20;
        auto rt = Runtime::open(dir, o);
        for (int i = 0; i < rules; ++i)
            rt->define_rule(make_rule("r" + std::to_string(i), Tier::Auto, 1,
                                      "(N" + std::to_string(i) + " (?x))", "(?x)"));
        const int reps = 200;
        double tot_begin = 0, tot_mut = 0;
        for (int r = 0; r < reps; ++r) {
            auto t0 = clk::now();
            WorldlineId w = rt->begin_experiment(rt->snapshot());
            auto t1 = clk::now();
            rt->mutate_rules(w, RuleMutation::upsert(make_rule("r0", Tier::Auto, 2, "(N0 (?x))", "(?x)")));
            auto t2 = clk::now();
            tot_begin += us(t0, t1);
            tot_mut += us(t1, t2);
            rt->abort(w);
        }
        auto t0 = clk::now();
        std::vector<std::string> deep;
        for (int i = 0; i < 20; ++i)
            deep = rt->state().rules.to_text(); // what a deep copy (Aura ast:snapshot style) costs
        auto t1 = clk::now();
        std::printf("| rules=%d: begin_experiment / COW mutate 1 rule / deep text copy | %.1f us / %.1f us / %.1f us |\n",
                    rules, tot_begin / reps, tot_mut / reps, us(t0, t1) / 20);
        std::filesystem::remove_all(dir);
    }

    {
        auto dir = tk::tmpdir("b-q");
        auto rt = Runtime::open(dir, tk::fast_opts());
        tk::seed_rules(*rt);
        tk::seed_memories(*rt, 20000 * scale);
        auto qs = tk::workload(200);
        auto run = [&](const char* label) {
            Metrics m;
            auto t0 = clk::now();
            for (auto& q : qs)
                m += rt->query(q).m;
            auto t1 = clk::now();
            std::printf("| %s: avg cost / avg latency per query | %.0f rows / %.1f us |\n", label,
                        m.cost() / qs.size(), us(t0, t1) / qs.size());
        };
        run("query baseline rules");
        WorldlineId w = rt->begin_experiment(rt->snapshot());
        rt->mutate_rules(w, RuleMutation::upsert(tk::pushdown_rule()));
        rt->mutate_rules(w, RuleMutation::upsert(tk::filter_through_parallel_rule()));
        auto t0 = clk::now();
        rt->run_queries(w, qs);
        auto t1 = clk::now();
        std::printf("| run_queries on worldline fiber (candidate+baseline, %zu intents) | %.1f ms, %llu fiber switches |\n",
                    qs.size(), us(t0, t1) / 1e3, (unsigned long long)rt->worldline_fiber(w).switches());
        auto pr = rt->promote(w);
        std::printf("| promote | %s |\n", pr.ok ? "ok" : pr.reason.c_str());
        run("query after promote (pushdown)");
        Intent in = qs[0];
        OptStats st;
        const int n = 20000;
        auto t2 = clk::now();
        for (int i = 0; i < n; ++i)
            optimize(intent_to_plan(in), rt->state().rules, st);
        auto t3 = clk::now();
        std::printf("| intent_to_plan + optimize (4 rules) | %.2f us |\n", us(t2, t3) / n);
        std::filesystem::remove_all(dir);
    }
    return 0;
}
