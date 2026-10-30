// Claim (2): abort / 未 promote 的 worldline 不留痕迹（含崩溃恢复后）。
#include "testkit.hpp"

using namespace amr;

TEST(abort_leaves_main_unchanged) {
    auto dir = tk::tmpdir("abort");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 500);
    const auto d0 = rt->state().digest();
    WorldlineId w = rt->begin_experiment(rt->snapshot());
    rt->mutate_rules(w, RuleMutation::upsert(tk::pushdown_rule()));
    rt->mutate_rules(w, RuleMutation::remove("rank-default-k")); // needs-confirm, allowed to try
    rt->run_queries(w, tk::workload(10));
    rt->abort(w);
    CHECK(rt->state().digest() == d0);
    CHECK(rt->active_worldlines() == 0);
    rt.reset();
    auto rt2 = Runtime::open(dir, tk::fast_opts());
    CHECK(rt2->state().digest() == d0);
    CHECK(rt2->recovery().discarded_worldlines == 0);
    CHECK(Runtime::replay_only(dir).digest() == d0);
}

TEST(crash_with_open_worldline_discards_it) {
    auto dir = tk::tmpdir("crash-open");
    auto o = tk::fast_opts();
    o.log.fsync = true;
    auto rt = Runtime::open(dir, o);
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 200);
    const auto d0 = rt->state().digest();
    WorldlineId w = rt->begin_experiment(rt->snapshot());
    rt->mutate_rules(w, RuleMutation::upsert(tk::pushdown_rule()));
    rt->mutate_rules(w, RuleMutation::upsert(tk::filter_through_parallel_rule()));
    rt->run_queries(w, tk::workload(5));
    rt.reset(); // "crash": the process goes away without promote / abort

    auto rt2 = Runtime::open(dir, o);
    CHECK(rt2->state().digest() == d0);
    CHECK(rt2->recovery().discarded_worldlines == 1);
    CHECK(rt2->recovery().discarded_mutations == 2);
    CHECK(rt2->active_worldlines() == 0);
    CHECK(!rt2->state().rules.find("pushdown-filter-into-retrieve"));
    rt2.reset();
    auto rt3 = Runtime::open(dir, o); // the discard was logged: not counted twice
    CHECK(rt3->recovery().discarded_worldlines == 0);
    CHECK(rt3->state().digest() == d0);
    // new worldline ids never collide with discarded ones
    WorldlineId w2 = rt3->begin_experiment(rt3->snapshot());
    CHECK(w2 > w);
}

TEST(crash_with_open_worldline_after_checkpoint) {
    auto dir = tk::tmpdir("crash-ckpt");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 200);
    const auto d0 = rt->state().digest();
    WorldlineId w = rt->begin_experiment(rt->snapshot());
    rt->mutate_rules(w, RuleMutation::upsert(tk::pushdown_rule()));
    rt->checkpoint(); // worldline is open at checkpoint time
    rt->run_queries(w, tk::workload(3));
    rt.reset();
    auto rt2 = Runtime::open(dir, tk::fast_opts());
    CHECK(rt2->recovery().used_checkpoint);
    CHECK(rt2->recovery().discarded_worldlines == 1);
    CHECK(rt2->recovery().discarded_mutations == 1);
    CHECK(rt2->state().digest() == d0);
}

TEST(crash_in_the_middle_of_promote_is_all_or_nothing) {
    // Cut the WlPromote record at every byte offset: after recovery the main
    // line is either exactly the pre-promote state (torn record truncated)
    // — never a partial merge.
    for (std::uint64_t cut = 1; cut < 200; cut += 7) {
        auto dir = tk::tmpdir("crash-promote");
        auto rt = Runtime::open(dir, tk::fast_opts());
        tk::seed_rules(*rt);
        tk::seed_memories(*rt, 300);
        const auto d0 = rt->state().digest();
        WorldlineId w = rt->begin_experiment(rt->snapshot());
        rt->mutate_rules(w, RuleMutation::upsert(tk::pushdown_rule()));
        rt->mutate_rules(w, RuleMutation::upsert(tk::filter_through_parallel_rule()));
        rt->run_queries(w, tk::workload(10));
        rt->log().inject_crash_after_bytes(cut);
        bool crashed = false;
        try {
            rt->promote(w);
        } catch (const SimulatedCrash&) {
            crashed = true;
        }
        CHECK(crashed);
        rt.reset();
        auto rt2 = Runtime::open(dir, tk::fast_opts());
        CHECK(rt2->state().digest() == d0);
        CHECK(rt2->recovery().log.truncated_bytes == cut);
        CHECK(rt2->recovery().discarded_worldlines == 1);
        CHECK(!rt2->state().rules.find("pushdown-filter-into-retrieve"));
        CHECK(!rt2->state().rules.find("filter-through-parallel"));
        std::filesystem::remove_all(dir);
    }
}

TEST(rejected_promote_keeps_main_unchanged) {
    auto dir = tk::tmpdir("reject");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 500);
    const auto d0 = rt->state().digest();
    WorldlineId w = rt->begin_experiment(rt->snapshot());
    rt->mutate_rules(w, RuleMutation::upsert(tk::bad_drop_ts_rule()));
    auto qs = tk::workload(30);
    rt->run_queries(w, qs);
    PromoteResult pr = rt->promote(w);
    CHECK(!pr.ok);
    CHECK(rt->state().digest() == d0);
    rt->abort(w, pr.reason);
    rt.reset();
    CHECK(Runtime::replay_only(dir).digest() == d0);
}

int main() { return tk::run_all("abort_no_trace"); }
