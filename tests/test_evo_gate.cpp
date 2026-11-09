// Claim (#4946): only Pareto-improving rule sets are promoted.
//
// This half needs no Aura process: it drives EvoEngine::evaluate directly with
// explicit genomes, so the worldline path and the gate are covered even when
// no v0.2 aura binary is installed (CI). The ask/tell half lives in
// test_evo_moead.
#include "amr/evo.hpp"
#include "testkit.hpp"

#include <cstdio>

using namespace amr;

namespace {

std::vector<Intent> evo_workload(int n) {
    auto qs = tk::workload(n);
    for (std::size_t i = 0; i < qs.size(); ++i)
        if (i % 2 == 0) qs[i].sources = {"notes", "chat"};
    return qs;
}

struct MainView {
    double cost = 0;
    std::vector<std::vector<std::uint64_t>> ids;
};

MainView main_view(Runtime& rt, const std::vector<Intent>& qs) {
    MainView v;
    for (const auto& q : qs) {
        auto r = rt.query(q);
        v.cost += r.m.cost();
        v.ids.push_back(r.ids);
    }
    return v;
}

} // namespace

TEST(explicit_genome_gate_promotes_pareto_and_aborts_the_rest) {
    auto dir = tk::tmpdir("evo-gate");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 3000);
    auto qs = evo_workload(40);
    const MainView before = main_view(*rt, qs);

    EvoConfig cfg;  // no aura process is opened by evaluate()
    EvoEngine eng(*rt, cfg);

    auto d1 = eng.evaluate({1, 0, 0, 0}, qs, true);
    std::printf("    pushdown: cost %.0f->%.0f tokens %.0f->%.0f sink %.0f->%.0f q=%.3f %s\n",
                d1.baseline.cost, d1.candidate.cost, d1.baseline.tokens, d1.candidate.tokens,
                d1.baseline.sink_calls, d1.candidate.sink_calls, d1.quality, d1.reason.c_str());
    CHECK(d1.promoted);
    CHECK(d1.quality == 1.0);

    auto d2 = eng.evaluate({1, 1, 0, 0}, qs, true);
    std::printf("    +parallel: cost %.0f->%.0f tokens %.0f->%.0f sink %.0f->%.0f q=%.3f %s\n",
                d2.baseline.cost, d2.candidate.cost, d2.baseline.tokens, d2.candidate.tokens,
                d2.baseline.sink_calls, d2.candidate.sink_calls, d2.quality, d2.reason.c_str());
    CHECK(d2.promoted);
    CHECK(d2.candidate.cost < d2.baseline.cost);

    // Negative control 1: same answers, twice the sink calls -> dominated.
    auto d3 = eng.evaluate({0, 0, 1, 0}, qs, true);
    std::printf("    dup-retrieve: cost %.0f->%.0f tokens %.0f->%.0f sink %.0f->%.0f q=%.3f %s\n",
                d3.baseline.cost, d3.candidate.cost, d3.baseline.tokens, d3.candidate.tokens,
                d3.baseline.sink_calls, d3.candidate.sink_calls, d3.quality, d3.reason.c_str());
    CHECK(!d3.promoted);
    CHECK(d3.quality == 1.0);
    CHECK(d3.candidate.sink_calls > d3.baseline.sink_calls);
    CHECK(d3.reason.find("pareto") != std::string::npos);

    // Negative control 2: cheaper, but changes the answers -> quality gate.
    auto d4 = eng.evaluate({0, 0, 0, 1}, qs, true);
    std::printf("    drop-entity: cost %.0f->%.0f tokens %.0f->%.0f sink %.0f->%.0f q=%.3f %s\n",
                d4.baseline.cost, d4.candidate.cost, d4.baseline.tokens, d4.candidate.tokens,
                d4.baseline.sink_calls, d4.candidate.sink_calls, d4.quality, d4.reason.c_str());
    CHECK(!d4.promoted);
    CHECK(d4.quality < 1.0);
    CHECK(d4.reason.find("quality") != std::string::npos);

    const MainView after = main_view(*rt, qs);
    std::printf("    main cost %.0f -> %.0f (%.1fx)\n", before.cost, after.cost,
                before.cost / after.cost);
    CHECK(after.cost < before.cost * 0.5);
    CHECK(after.ids == before.ids);
    CHECK(rt->state().rules.find("evo-pushdown-filter"));
    CHECK(rt->state().rules.find("evo-filter-through-parallel"));
    CHECK(!rt->state().rules.find("evo-dup-retrieve"));
    CHECK(!rt->state().rules.find("evo-drop-entity-filter"));
    CHECK(rt->active_worldlines() == 0);
}

int main() { return tk::run_all("evo_gate"); }
