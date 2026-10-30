// Claim (1): 主线永远只观察到已 promote 的规则版本。
#include "testkit.hpp"

#include "core/fiber_context.hh"

using namespace amr;

static bool plan_has_pushdown(NodeId plan) {
    std::unordered_set<NodeId> all;
    collect_nodes(plan, all);
    for (NodeId n : all)
        if (node(n).attr("pfield"))
            return true;
    return false;
}

TEST(main_sees_only_promoted_rules) {
    auto dir = tk::tmpdir("iso");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 2000);
    auto qs = tk::workload(20);

    std::vector<std::vector<std::uint64_t>> before;
    for (auto& q : qs)
        before.push_back(rt->query(q).ids);
    const auto main_digest = rt->state().rules.digest();
    const auto main_version = rt->state().main_version;

    Snapshot s = rt->snapshot();
    WorldlineId a = rt->begin_experiment(s);
    WorldlineId b = rt->begin_experiment(s);
    rt->mutate_rules(a, RuleMutation::upsert(tk::pushdown_rule()));

    // worldline A sees its rule; B (same base) and main do not
    auto ra = rt->run_queries(a, qs);
    auto rb = rt->run_queries(b, qs);
    CHECK(plan_has_pushdown(ra[0].plan));
    CHECK(!plan_has_pushdown(rb[0].plan));
    for (std::size_t i = 0; i < qs.size(); ++i) {
        QueryResult m = rt->query(qs[i]);
        CHECK(!plan_has_pushdown(m.plan));
        CHECK(m.ids == before[i]);
    }
    CHECK(rt->state().rules.digest() == main_digest);
    CHECK(rt->state().main_version == main_version);
    CHECK(!rt->state().rules.find("pushdown-filter-into-retrieve"));

    // promote A: now main sees it, B's view is still its own snapshot
    PromoteResult pr = rt->promote(a);
    CHECK(pr.ok);
    CHECK(rt->state().main_version == main_version + 1);
    CHECK(plan_has_pushdown(rt->query(qs[0]).plan));
    CHECK(!rt->worldline_rules(b).find("pushdown-filter-into-retrieve"));
    auto rb2 = rt->run_queries(b, qs);
    CHECK(!plan_has_pushdown(rb2[0].plan));
    // results are equivalent, only cheaper
    for (std::size_t i = 0; i < qs.size(); ++i)
        CHECK(rt->query(qs[i]).ids == before[i]);
    rt->abort(b);
}

TEST(worldline_runs_on_its_own_aura_fiber_context) {
    auto dir = tk::tmpdir("fib");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    WorldlineId a = rt->begin_experiment(rt->snapshot());
    WorldlineId b = rt->begin_experiment(rt->snapshot());
    std::uint64_t seen_a = 0, seen_b = 0, seen_plain = 0;
    rt->worldline_fiber(a).run([&] { seen_a = aura::core::fiber_ctx::active().owner_fiber; });
    rt->worldline_fiber(b).run([&] { seen_b = aura::core::fiber_ctx::active().owner_fiber; });
    seen_plain = aura::core::fiber_ctx::active().owner_fiber;
    CHECK(seen_a == a);
    CHECK(seen_b == b);
    CHECK(seen_plain == 0); // plain thread fallback after detach
    // fiber-local state does not leak between worldlines
    rt->worldline_fiber(a).run([&] { aura::core::fiber_ctx::active().try_body_depth = 7; });
    int db = -1;
    rt->worldline_fiber(b).run([&] { db = aura::core::fiber_ctx::active().try_body_depth; });
    CHECK(db == 0);
    int da = -1;
    rt->worldline_fiber(a).run([&] { da = aura::core::fiber_ctx::active().try_body_depth; });
    CHECK(da == 7);
    // run_queries yields between queries; switches > queries
    tk::seed_memories(*rt, 100);
    rt->run_queries(a, tk::workload(5));
    CHECK(rt->worldline_fiber(a).switches() >= 5);
    rt->abort(a);
    rt->abort(b);
}

int main() { return tk::run_all("main_line_isolation"); }
