// Claim (#4946): Aura std/moead's ask/tell drives rule-set evolution.
//
// Determinism: a fixed seed must produce the same candidates, the same
// evaluations, the same promote/abort sequence and the same final rule set.
// The gate itself is covered without Aura in test_evo_gate.
#include "amr/evo.hpp"
#include "testkit.hpp"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

using namespace amr;

namespace {

std::string aura_bin() {
    if (const char* b = std::getenv("AMR_AURA_BIN"); b && *b) return b;
    return "aura";
}

bool have_aura(const std::string& b) { return ::access(b.c_str(), X_OK) == 0; }

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

EvoConfig cfg_for(const std::string& bin, int gens, int k) {
    EvoConfig cfg;
    cfg.aura_bin = bin;
    cfg.seed = 42;
    cfg.H = 4;
    cfg.T = 5;
    cfg.generations = gens;
    cfg.candidates_per_gen = k;
    return cfg;
}

} // namespace

TEST(moead_ask_tell_is_deterministic_for_a_fixed_seed) {
    const std::string bin = aura_bin();
    if (!have_aura(bin)) {
        std::printf("    SKIP: no aura binary (set AMR_AURA_BIN)\n");
        return;
    }
    auto run_once = [&](const char* tag) {
        auto dir = tk::tmpdir(tag);
        auto rt = Runtime::open(dir, tk::fast_opts());
        tk::seed_rules(*rt);
        tk::seed_memories(*rt, 2000);
        auto qs = evo_workload(24);
        EvoEngine eng(*rt, cfg_for(bin, 3, 4));
        auto rep = eng.run(qs);
        CHECK(rep.rounds.size() == 3);
        CHECK(rep.promoted + rep.rejected == 12);
        return rep.transcript();
    };
    const std::string a = run_once("evo-det-a");
    const std::string b = run_once("evo-det-b");
    CHECK(a == b);
    std::printf("    transcript bytes=%zu, identical=%s\n", a.size(), a == b ? "yes" : "no");
}

TEST(moead_run_promotes_only_pareto_improvements) {
    const std::string bin = aura_bin();
    if (!have_aura(bin)) {
        std::printf("    SKIP: no aura binary (set AMR_AURA_BIN)\n");
        return;
    }
    auto dir = tk::tmpdir("evo-run");
    auto rt = Runtime::open(dir, tk::fast_opts());
    tk::seed_rules(*rt);
    tk::seed_memories(*rt, 2500);
    auto qs = evo_workload(32);
    const MainView before = main_view(*rt, qs);

    EvoEngine eng(*rt, cfg_for(bin, 4, 5));
    auto rep = eng.run(qs);
    CHECK(rep.rounds.size() == 4);
    CHECK(rep.promoted >= 1);
    CHECK(rep.rejected >= 1);

    int saw_infeasible = 0;
    int saw_sink_worse = 0;
    for (const auto& round : rep.rounds) {
        for (const auto& d : round.decisions) {
            if (d.promoted) {
                CHECK(d.candidate.pareto_improves(d.baseline));
                CHECK(d.quality >= 1.0);
            } else {
                CHECK(!d.reason.empty());
                CHECK(d.reason != "promoted");
            }
            if (d.quality < 1.0) ++saw_infeasible;
            if (d.candidate.sink_calls > d.baseline.sink_calls) ++saw_sink_worse;
        }
    }
    const MainView after = main_view(*rt, qs);
    std::printf("    promoted=%d rejected=%d main cost %.0f -> %.0f; infeasible=%d sink-worse=%d\n",
                rep.promoted, rep.rejected, before.cost, after.cost, saw_infeasible, saw_sink_worse);
    CHECK(after.cost < before.cost);
    CHECK(after.ids == before.ids);
    CHECK(rt->active_worldlines() == 0);
    CHECK(!rep.front.empty());
    for (std::size_t i = 0; i < rep.front.size(); ++i) {
        for (std::size_t j = 0; j < rep.front.size(); ++j) {
            if (i == j) continue;
            CHECK(!rep.front[j].obj.pareto_improves(rep.front[i].obj));
        }
    }
    std::printf("    front:");
    for (const auto& p : rep.front)
        std::printf(" (%s cost=%.0f tok=%.0f sink=%.0f)", p.id.substr(0, 8).c_str(),
                    p.obj.cost, p.obj.tokens, p.obj.sink_calls);
    std::printf("\n");

    // Rules are data: the final set is queryable and still has the safety tier.
    auto rules = rt->state().rules.to_text();
    CHECK(!rules.empty());
    std::printf("    final rules=%zu digest=%llu\n", rules.size(),
                (unsigned long long)rep.final_rules_digest);
    for (const auto& line : rules)
        if (line.find("safety-redact-secrets") != std::string::npos)
            CHECK(line.find("never-auto") != std::string::npos);
}

int main() { return tk::run_all("evo_moead"); }
