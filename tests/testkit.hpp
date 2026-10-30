// testkit.hpp — tiny test harness + shared fixtures.
#pragma once

#include "amr/runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace tk {

inline int g_fail = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++tk::g_checks;                                                                            \
        if (!(cond)) {                                                                             \
            ++tk::g_fail;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                                          \
    } while (0)

#define CHECK_THROWS(expr, Type)                                                                   \
    do {                                                                                           \
        ++tk::g_checks;                                                                            \
        bool thrown_ = false;                                                                      \
        try {                                                                                      \
            expr;                                                                                  \
        } catch (const Type&) {                                                                    \
            thrown_ = true;                                                                        \
        }                                                                                          \
        if (!thrown_) {                                                                            \
            ++tk::g_fail;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: expected %s from %s\n", __FILE__, __LINE__, #Type, \
                         #expr);                                                                   \
        }                                                                                          \
    } while (0)

struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); }
};
#define TEST(name)                                                                                 \
    static void name();                                                                            \
    static tk::Reg reg_##name(#name, name);                                                        \
    static void name()

inline int run_all(const char* suite) {
    for (auto& c : cases()) {
        int before = g_fail;
        try {
            c.fn();
        } catch (const std::exception& e) {
            ++g_fail;
            std::fprintf(stderr, "  FAIL %s: uncaught exception: %s\n", c.name, e.what());
        }
        std::printf("[%s] %s.%s\n", g_fail == before ? "PASS" : "FAIL", suite, c.name);
    }
    std::printf("%s: %zu cases, %d checks, %d failures\n", suite, cases().size(), g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}

inline std::string tmpdir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto p = std::filesystem::temp_directory_path() /
             ("amr-" + tag + "-" + std::to_string(rng() % 1000000000));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}

inline amr::RuntimeOptions fast_opts() {
    amr::RuntimeOptions o;
    o.log.fsync = false; // tests that need durability turn it back on
    return o;
}

// Baseline main-line rules (human-authored).
inline amr::NodeId safety_rule() {
    return amr::make_rule("safety-redact-secrets", amr::Tier::NeverAuto, 100,
                          R"((Assemble :as "?a" :safe "~" (?x)))",
                          R"((?a :safe "1" (Filter :field "tag" :op "!=" :value "secret" (?x))))",
                          "never let tag=secret into the context");
}
inline amr::NodeId rank_default_rule() {
    return amr::make_rule("rank-default-k", amr::Tier::NeedsConfirm, 50,
                          R"((Rank :as "?r" :k "~" (?x)))", R"((?r :k "20"))",
                          "bound Rank output when no k is given");
}
// Candidate optimisation rules (what an experiment proposes).
inline amr::NodeId pushdown_rule() {
    return amr::make_rule(
        "pushdown-filter-into-retrieve", amr::Tier::Auto, 10,
        R"((Filter :field "?f" :op "?o" :value "?v" (Retrieve :as "?r" :pfield "~")))",
        R"((?r :pfield "?f" :pop "?o" :pvalue "?v"))",
        "push a filter into the storage sink (index lookup for entity =)");
}
inline amr::NodeId filter_through_parallel_rule() {
    return amr::make_rule("filter-through-parallel", amr::Tier::Auto, 20,
                          R"((Filter :as "?f" (Parallel (?a) (?b))))",
                          R"((Parallel (?f (?a)) (?f (?b))))",
                          "distribute a filter over a 2-way Parallel");
}
inline amr::NodeId bad_drop_ts_rule() {
    return amr::make_rule("drop-ts-filter", amr::Tier::Auto, 30,
                          R"((Filter :field "ts" (?x)))", R"((?x))",
                          "WRONG: drops the time filter (changes results)");
}

inline void seed_rules(amr::Runtime& rt) {
    rt.define_rule(safety_rule());
    rt.define_rule(rank_default_rule());
}

inline void seed_memories(amr::Runtime& rt, int n, int entities = 50, std::uint64_t seed = 7) {
    std::mt19937_64 rng(seed);
    const char* sources[] = {"notes", "chat", "tools"};
    const char* tags[] = {"pref", "fact", "todo", "secret"};
    for (int i = 0; i < n; ++i) {
        amr::Memory m;
        m.entity = "e" + std::to_string(rng() % entities);
        m.source = sources[rng() % 3];
        m.tag = tags[rng() % 4];
        m.ts = 1000 + static_cast<std::int64_t>(i);
        int words = 3 + static_cast<int>(rng() % 12);
        for (int w = 0; w < words; ++w)
            m.text += (w ? " " : "") + std::string("w") + std::to_string(rng() % 100);
        rt.put_memory(m);
    }
}

inline std::vector<amr::Intent> workload(int n, int entities = 50, std::uint64_t seed = 11) {
    std::mt19937_64 rng(seed);
    std::vector<amr::Intent> v;
    for (int i = 0; i < n; ++i) {
        amr::Intent in;
        in.entity = "e" + std::to_string(rng() % entities);
        in.sources = {(rng() % 2) ? "notes" : "chat"};
        if (rng() % 3 == 0)
            in.since = 1000 + static_cast<std::int64_t>(rng() % 500);
        in.top_k = 5 + static_cast<int>(rng() % 10);
        in.budget_tokens = 64 + static_cast<int>(rng() % 200);
        v.push_back(in);
    }
    return v;
}

} // namespace tk
