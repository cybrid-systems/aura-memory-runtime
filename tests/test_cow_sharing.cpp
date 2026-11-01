// Claim (5): COW 共享未修改的规则节点（实测）。
#include "testkit.hpp"

#include <chrono>

using namespace amr;

static NodeId numbered_rule(int i) {
    std::string n = std::to_string(i);
    return make_rule("r" + n, Tier::Auto, i % 10,
                     "(Filter :field \"f" + n + "\" :op \"=\" :value \"?v\" (Retrieve :as \"?r\"))",
                     "(?r :pfield \"f" + n + "\" :pop \"=\" :pvalue \"?v\")", "generated rule " + n);
}

TEST(worldline_shares_all_unmodified_rule_nodes) {
    auto dir = tk::tmpdir("cow");
    auto rt = Runtime::open(dir, tk::fast_opts());
    const int N = 1000;
    for (int i = 0; i < N; ++i)
        rt->define_rule(numbered_rule(i));
    CHECK(rt->state().rules.size() == static_cast<std::size_t>(N));

    auto& pcv = aura::ast::g_pcv_hotpath_metrics();
    const auto cow0 = pcv.with_set_cow_total.load();

    auto t0 = std::chrono::steady_clock::now();
    WorldlineId w = rt->begin_experiment(rt->snapshot());
    auto t1 = std::chrono::steady_clock::now();

    // replace ONE rule (r500) in the worldline
    NodeId changed = make_rule("r500", Tier::Auto, 5,
                               "(Filter :field \"f500\" :op \"=\" :value \"?v\" (Retrieve :as \"?r\"))",
                               "(?r :pfield \"f500\" :pop \"=\" :pvalue \"?v\" :hint \"x\")");
    rt->mutate_rules(w, RuleMutation::upsert(changed));
    const auto cow1 = pcv.with_set_cow_total.load();

    RuleSet main = rt->state().rules;
    RuleSet wl = rt->worldline_rules(w);
    std::unordered_set<NodeId> a, b;
    for (std::size_t i = 0; i < main.rules.size(); ++i)
        collect_nodes(main.rules[i], a);
    for (std::size_t i = 0; i < wl.rules.size(); ++i)
        collect_nodes(wl.rules[i], b);
    std::size_t shared = 0, wl_only = 0, shared_bytes = 0, wl_only_bytes = 0;
    for (NodeId n : b) {
        if (a.count(n)) {
            ++shared;
            shared_bytes += NodePool::global().node_bytes(n);
        } else {
            ++wl_only;
            wl_only_bytes += NodePool::global().node_bytes(n);
        }
    }
    std::size_t top_shared = 0;
    for (std::size_t i = 0; i < wl.rules.size(); ++i)
        top_shared += (wl.rules[i] == main.rules[i]);

    std::printf("    rules=%d, worldline nodes=%zu, shared=%zu (%.2f%%), new=%zu\n", N, b.size(),
                shared, 100.0 * shared / b.size(), wl_only);
    std::printf("    bytes shared=%zu, new=%zu; top-level rule ids shared=%zu/%d\n", shared_bytes,
                wl_only_bytes, top_shared, N);
    std::printf("    begin_experiment=%lld ns; PCV with_set_cow_total delta=%llu\n",
                (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count(),
                (unsigned long long)(cow1 - cow0));

    CHECK(top_shared == static_cast<std::size_t>(N - 1));
    CHECK(wl_only == count_nodes(changed) || wl_only < count_nodes(changed) + 1);
    CHECK(wl_only <= count_nodes(changed));
    CHECK(shared >= b.size() - count_nodes(changed));
    CHECK(cow1 - cow0 >= 1); // Aura PCV detected the shared buffer and copied (COW)
    CHECK(rt->state().rules.get("r500")->rewrite != view_rule(changed).rewrite);
    rt->abort(w);
}

TEST(optimizer_rewrite_shares_untouched_subtrees) {
    Intent in;
    in.entity = "e1";
    in.sources = {"notes"};
    in.since = 1200;
    in.relate_source = "tools"; // a second branch the rule does not touch
    NodeId plan = intent_to_plan(in);
    RuleSet rs;
    rs = rs.with_upsert(tk::pushdown_rule());
    OptStats st;
    NodeId out = optimize(plan, rs, st);
    std::unordered_set<NodeId> a, b;
    collect_nodes(plan, a);
    collect_nodes(out, b);
    std::size_t shared = 0;
    for (NodeId n : b)
        shared += a.count(n);
    std::printf("    plan nodes before=%zu after=%zu shared=%zu\n", a.size(), b.size(), shared);
    CHECK(shared > 0);
    CHECK(!deep_equal(plan, out));
    CHECK(count_nodes(plan) == a.size()); // original plan is intact (persistent)
}

TEST(aura_pcv_with_set_leaves_unique_receiver_untouched) {
    // Finding F2 (cybrid-systems/aura#4770, fixed upstream): PCV `with_set()
    // const` used to take an in-place fast path on a uniquely owned buffer
    // (Issue #2140) and change the *receiver*. On the current pin the
    // receiver must stay untouched even when it is the only owner.
    Kids k;
    k = k.with_push_back(1).with_push_back(2).with_push_back(3);
    Kids k2 = k.with_set(0, 9);
    const bool in_place = (k[0] == 9);
    std::printf("    PCV with_set on unique buffer mutated receiver: %s\n", in_place ? "YES" : "no");
    CHECK(!in_place);
    CHECK(k[0] == 1);
    CHECK(k2[0] == 9);

    RuleSet rs;
    rs = rs.with_upsert(tk::pushdown_rule());
    const NodeId before = rs.rules[0];
    RuleSet rs2 = rs.with_upsert(make_rule("pushdown-filter-into-retrieve", Tier::Auto, 1,
                                           "(Filter (?x))", "(?x)"));
    CHECK(rs.rules[0] == before); // receiver untouched even though it was unique
    CHECK(rs2.rules[0] != before);
}

int main() { return tk::run_all("cow_sharing"); }
