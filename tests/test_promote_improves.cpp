// Claim (6): 规则改写在实验中改善成本指标并被 promote。
#include "testkit.hpp"

using namespace amr;

static double main_cost(Runtime& rt, const std::vector<Intent>& qs, std::vector<std::vector<std::uint64_t>>* ids) {
    double c = 0;
    for (auto& q : qs) {
        auto r = rt.query(q);
        c += r.m.cost();
        if (ids)
            ids->push_back(r.ids);
    }
    return c;
}

TEST(pushdown_experiment_lowers_cost_and_is_promoted) {
    auto dir = tk::tmpdir("promote");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 5000);
    auto qs = tk::workload(60);
    std::vector<std::vector<std::uint64_t>> before_ids;
    const double before = main_cost(*rt, qs, &before_ids);

    WorldlineId w = rt->begin_experiment(rt->snapshot());
    rt->mutate_rules(w, RuleMutation::upsert(tk::pushdown_rule()));
    rt->mutate_rules(w, RuleMutation::upsert(tk::filter_through_parallel_rule()));
    rt->run_queries(w, qs);
    const Feedback& fb = rt->feedback(w);
    std::printf("    samples=%llu avg cost baseline=%.1f candidate=%.1f (%.1fx) quality=%.3f\n",
                (unsigned long long)fb.samples, fb.avg_baseline_cost(), fb.avg_candidate_cost(),
                fb.avg_baseline_cost() / fb.avg_candidate_cost(), fb.avg_quality());
    for (auto& [name, n] : fb.rule_hits)
        std::printf("    rule hit %s x%llu\n", name.c_str(), (unsigned long long)n);
    CHECK(fb.samples == qs.size());
    CHECK(fb.avg_candidate_cost() < fb.avg_baseline_cost() * 0.5);
    CHECK(fb.avg_quality() == 1.0);
    CHECK(fb.rule_hits.count("pushdown-filter-into-retrieve"));

    PromoteResult pr = rt->promote(w);
    CHECK(pr.ok);
    std::vector<std::vector<std::uint64_t>> after_ids;
    const double after = main_cost(*rt, qs, &after_ids);
    std::printf("    main-line total cost before=%.0f after promote=%.0f\n", before, after);
    CHECK(after < before * 0.5);
    CHECK(after_ids == before_ids);
    CHECK(rt->state().rules.find("pushdown-filter-into-retrieve"));

    // the promotion is durable: survives restart via the WlPromote event
    rt.reset();
    auto rt2 = Runtime::open(dir, tk::fast_opts());
    CHECK(rt2->state().rules.find("pushdown-filter-into-retrieve"));
    CHECK(main_cost(*rt2, qs, nullptr) == after);
}

TEST(quality_regression_is_rejected) {
    auto dir = tk::tmpdir("bad");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 3000);
    // a recent-window intent: dropping the ts filter lets older rows leak in.
    // (with a wide window the filter is redundant under Rank-by-recency and
    // the "bad" rule is harmless -- see README finding F9)
    auto qs = tk::workload(60);
    for (auto& q : qs)
        q.since = 1000 + 3000 - 150;
    WorldlineId w = rt->begin_experiment(rt->snapshot());
    rt->mutate_rules(w, RuleMutation::upsert(tk::bad_drop_ts_rule()));
    rt->run_queries(w, qs);
    const Feedback& fb = rt->feedback(w);
    std::printf("    bad rule: quality=%.3f cost %.1f -> %.1f\n", fb.avg_quality(),
                fb.avg_baseline_cost(), fb.avg_candidate_cost());
    CHECK(fb.avg_quality() < 1.0);
    PromoteResult pr = rt->promote(w);
    CHECK(!pr.ok);
    CHECK(pr.reason.find("quality") != std::string::npos);
    CHECK(!rt->state().rules.find("drop-ts-filter"));
    rt->abort(w);
}

TEST(no_improvement_is_rejected) {
    auto dir = tk::tmpdir("noop");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 500);
    WorldlineId w = rt->begin_experiment(rt->snapshot());
    rt->mutate_rules(w, RuleMutation::upsert(make_rule("never-matches", Tier::Auto, 1,
                                                       "(NoSuchNode (?x))", "(?x)")));
    PromoteResult p0 = rt->promote(w);
    CHECK(!p0.ok); // no samples yet
    rt->run_queries(w, tk::workload(10));
    PromoteResult pr = rt->promote(w);
    CHECK(!pr.ok);
    CHECK(pr.reason.find("cost") != std::string::npos);
    rt->abort(w);
}

int main() { return tk::run_all("promote_improves"); }
