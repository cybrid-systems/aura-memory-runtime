// Tiers (auto / needs-confirm / never-auto), worldline cap, conflicts (Aura overlay).
#include "testkit.hpp"

using namespace amr;

TEST(never_auto_rules_cannot_be_touched_by_experiments) {
    auto dir = tk::tmpdir("tier");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    WorldlineId w = rt->begin_experiment(rt->snapshot());
    CHECK_THROWS(rt->mutate_rules(w, RuleMutation::remove("safety-redact-secrets")), TierViolation);
    CHECK_THROWS(rt->mutate_rules(w, RuleMutation::upsert(make_rule("safety-redact-secrets", Tier::Auto, 1,
                                                                    "(X)", "(Y)"))), TierViolation);
    CHECK_THROWS(rt->mutate_rules(w, RuleMutation::upsert(make_rule("new-never", Tier::NeverAuto, 1,
                                                                    "(X)", "(Y)"))), TierViolation);
    CHECK(rt->worldline_rules(w).get("safety-redact-secrets")->tier == Tier::NeverAuto);
    rt->abort(w);
}

TEST(needs_confirm_requires_human_confirmation) {
    auto dir = tk::tmpdir("confirm");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 2000);
    WorldlineId w = rt->begin_experiment(rt->snapshot());
    rt->mutate_rules(w, RuleMutation::upsert(tk::pushdown_rule()));
    // touching a needs-confirm rule marks the whole worldline
    rt->mutate_rules(w, RuleMutation::upsert(make_rule("rank-default-k", Tier::NeedsConfirm, 50,
                                                       R"((Rank :as "?r" :k "~" (?x)))", R"((?r :k "10"))")));
    rt->run_queries(w, tk::workload(20));
    PromoteResult p1 = rt->promote(w);
    CHECK(!p1.ok && !p1.aborted);
    CHECK(p1.reason.find("confirm") != std::string::npos);
    PromotePolicy pol;
    pol.confirmed = true;
    PromoteResult p2 = rt->promote(w, pol);
    CHECK(p2.ok);
    CHECK(*node(view_rule(rt->state().rules.get("rank-default-k")->id).rewrite).attr("k") == "10");
}

TEST(active_worldline_cap) {
    auto dir = tk::tmpdir("cap");
    auto o = tk::fast_opts();
    o.max_active_worldlines = 3;
    auto rt = Runtime::open(dir, o);
    std::vector<WorldlineId> ws;
    for (int i = 0; i < 3; ++i)
        ws.push_back(rt->begin_experiment(rt->snapshot()));
    CHECK_THROWS(rt->begin_experiment(rt->snapshot()), WorldlineLimit);
    rt->abort(ws[0]);
    CHECK(rt->active_worldlines() == 2);
    ws.push_back(rt->begin_experiment(rt->snapshot()));
    CHECK(rt->active_worldlines() == 3);
}

TEST(two_worldlines_same_rule_first_committer_wins) {
    auto dir = tk::tmpdir("conflict");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 2000);
    auto qs = tk::workload(20);
    Snapshot s = rt->snapshot();
    WorldlineId a = rt->begin_experiment(s);
    WorldlineId b = rt->begin_experiment(s);
    rt->mutate_rules(a, RuleMutation::upsert(tk::pushdown_rule()));
    rt->mutate_rules(b, RuleMutation::upsert(tk::pushdown_rule()));
    rt->run_queries(a, qs);
    rt->run_queries(b, qs);
    PromoteResult pa = rt->promote(a);
    PromoteResult pb = rt->promote(b);
    CHECK(pa.ok);
    CHECK(!pb.ok && pb.aborted);
    std::printf("    loser reason: %s\n", pb.reason.c_str());
    CHECK(rt->active_worldlines() == 0);
    rt.reset();
    auto rt2 = Runtime::open(dir, tk::fast_opts());
    CHECK(rt2->recovery().discarded_worldlines == 0);
    CHECK(rt2->state().main_version == 3); // 2 seed rules + 1 promote
}

TEST(main_line_change_after_snapshot_conflicts) {
    auto dir = tk::tmpdir("stale");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 2000);
    WorldlineId w = rt->begin_experiment(rt->snapshot());
    rt->mutate_rules(w, RuleMutation::upsert(tk::pushdown_rule()));
    rt->run_queries(w, tk::workload(10));
    rt->define_rule(tk::pushdown_rule()); // admin lands the same rule name on main first
    PromoteResult pr = rt->promote(w);
    CHECK(!pr.ok && pr.aborted);
    // a disjoint main-line change does NOT conflict
    WorldlineId w2 = rt->begin_experiment(rt->snapshot());
    rt->mutate_rules(w2, RuleMutation::upsert(tk::filter_through_parallel_rule()));
    rt->run_queries(w2, tk::workload(10));
    rt->define_rule(make_rule("unrelated", Tier::Auto, 1, "(Nope)", "(Nope :x \"1\")"));
    Intent multi;
    multi.entity = "e1";
    multi.sources = {"notes", "chat"};
    rt->run_queries(w2, {multi});
    PromoteResult p2 = rt->promote(w2);
    std::printf("    disjoint promote: ok=%d reason=%s\n", p2.ok ? 1 : 0, p2.reason.c_str());
    CHECK(!p2.aborted);
}

TEST(future_snapshot_rejected) {
    auto dir = tk::tmpdir("future");
    auto rt = Runtime::open(dir, tk::fast_opts());
    Snapshot s = rt->snapshot();
    s.main_version += 5;
    CHECK_THROWS(rt->begin_experiment(s), std::exception);
}

int main() { return tk::run_all("tiers_and_limits"); }
