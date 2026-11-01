// Production soak: many worldline lifecycles (begin -> mutate -> run -> promote
// or abort) with periodic checkpoints. Checks that (1) no worldline slot leaks
// (WorldlineLimit would throw), (2) RSS does not grow without bound, (3) the
// state after the soak is exactly what log replay and a fresh open rebuild.
#include "testkit.hpp"

#include <fstream>

using namespace amr;

static long rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string k;
    while (f >> k) {
        if (k == "VmRSS:") {
            long v = 0;
            f >> v;
            return v;
        }
    }
    return 0;
}

TEST(soak_worldline_lifecycles_bounded_and_replayable) {
    auto dir = tk::tmpdir("soak");
    std::uint64_t digest = 0;
    long rss_warm = 0, rss_end = 0;
    std::size_t nodes_warm = 0, nodes_end = 0;
    int promoted = 0, aborted = 0;
    constexpr int kIters = 1500;
    {
        auto rt = Runtime::open(dir, tk::fast_opts());
        tk::seed_rules(*rt);
        tk::seed_memories(*rt, 500);
        auto qs = tk::workload(4);
        PromotePolicy pol;
        pol.require_cost_improvement = false;
        pol.min_quality = 0.0;
        for (int i = 0; i < kIters; ++i) {
            Snapshot s = rt->snapshot();
            WorldlineId wl = rt->begin_experiment(s);
            // a fresh candidate rule each round: names cycle so the rule set
            // stays bounded while every round is a real mutation.
            const std::string name = "soak-rule-" + std::to_string(i % 8);
            rt->mutate_rules(wl, RuleMutation::upsert(make_rule(
                                     name, Tier::Auto, 10 + (i % 5),
                                     R"((Filter :field "?f" :op "?o" :value "?v" (Retrieve :as "?r" :pfield "~")))",
                                     R"((?r :pfield "?f" :pop "?o" :pvalue "?v"))", "soak candidate")));
            (void)rt->run_queries(wl, qs);
            if (i % 3 == 0) {
                auto r = rt->promote(wl, pol);
                if (r.ok)
                    ++promoted;
                else if (!r.aborted)
                    rt->abort(wl, "soak: not promoted");
            } else {
                rt->abort(wl, "soak");
                ++aborted;
            }
            if (i % 250 == 249)
                rt->checkpoint();
            if (i == 300) {
                rss_warm = rss_kb();
                nodes_warm = NodePool::global().size();
            }
        }
        rss_end = rss_kb();
        nodes_end = NodePool::global().size();
        digest = rt->state().digest();
        std::printf("    soak: %d iters, promoted=%d aborted=%d, rules=%zu, RSS warm=%ld kB end=%ld kB (+%ld kB)\n",
                    kIters, promoted, aborted, rt->state().rules.rules.size(), rss_warm, rss_end,
                    rss_end - rss_warm);
        // Known Stage-1 limit (README R2): the node pool is append-only, so
        // every lifecycle's rule + plan nodes stay resident. Report the rate.
        std::printf("    soak: node pool warm=%zu end=%zu (+%zu nodes, %.1f per lifecycle), %.0f B RSS per lifecycle\n",
                    nodes_warm, nodes_end, nodes_end - nodes_warm,
                    double(nodes_end - nodes_warm) / (kIters - 301),
                    1024.0 * double(rss_end - rss_warm) / (kIters - 301));
        CHECK(promoted > 0);
        CHECK(rt->state().rules.rules.size() <= 2 + 8);
    }
    // ~1200 lifecycles after warm-up: anything that keeps per-worldline state
    // forever (nodes, overlays, plans) shows up here.
    CHECK(rss_end - rss_warm < 64 * 1024);
    CHECK(Runtime::replay_only(dir).digest() == digest);
    auto rt2 = Runtime::open(dir, tk::fast_opts());
    CHECK(rt2->state().digest() == digest);
}

int main() { return tk::run_all("soak_worldlines"); }
