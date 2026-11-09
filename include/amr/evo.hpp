// evo.hpp — rule-set evolution driven by Aura's std/evo-core + std/moead.
//
// The search is NOT implemented here. Aura owns the MOEA/D state, the
// Das-Dennis weights, the Tchebycheff scalarisation and the neighbourhood
// update; this file only carries the ask/tell boundary and the rule-set
// encoding. Aura exposes no evaluator ABI (docs/embedding.md: only
// aura::core / aura::fiber_rt are linkable, neither contains the Evaluator),
// so the boundary is Aura's compiler-session protocol:
//
//     aura --serve   (one JSON reply per line, session state persists)
//
// The session runs the module's own prelude:
//     (require "std/list" all:) (require "std/math" all:)
//     (require "std/moead" all:)
//     (define amr-evo (moead:make ...))
//
// Every candidate is evaluated in an existing worldline: COW RuleSet copy +
// Aura fiber_ctx + COW overlay, exactly the path test_main_line_isolation /
// test_promote_improves cover. A candidate is promoted only when it is a
// Pareto improvement on (cost, tokens, sink_calls) over the live rule set
// and its result quality (F1 vs the live answers) meets the gate.
#pragma once

#include "amr/runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace amr {

// Three minimised objectives. Cost = rows_scanned + rows_processed.
struct ObjectiveVector {
    double cost = 0.0;
    double tokens = 0.0;
    double sink_calls = 0.0;

    bool operator==(const ObjectiveVector&) const = default;
    // No objective worse than `o`.
    [[nodiscard]] bool weakly_dominates(const ObjectiveVector& o) const;
    // No objective worse and at least one strictly better.
    [[nodiscard]] bool pareto_improves(const ObjectiveVector& o) const;
};

struct EvoConfig {
    std::string aura_bin;      // empty: AMR_AURA_BIN, else "aura" in PATH
    std::uint64_t seed = 42;
    int H = 4;                 // Das-Dennis divisions -> 15 weight vectors
    int T = 5;                 // neighbourhood size
    int dimensions = 4;        // genome length (rule toggles)
    int candidates_per_gen = 5;
    int generations = 4;
    double min_quality = 1.0;  // F1 gate, checked before any objective gate
};

struct EvoCandidate {
    std::string id;
    std::vector<double> genome;
    int gen = 0;
    int slot = 0;
};

// One evaluated rule set. `baseline` is the live rule set's vector on the
// same workload; `candidate` is the worldline's vector.
struct EvoDecision {
    std::string id;
    std::vector<double> genome;
    ObjectiveVector baseline;
    ObjectiveVector candidate;
    double quality = 0.0;
    bool promoted = false;
    std::string reason;
};

struct EvoRound {
    int gen = 0;
    std::vector<EvoDecision> decisions;
};

struct EvoFrontPoint {
    std::string id;
    ObjectiveVector obj;
};

struct EvoRunReport {
    std::vector<EvoRound> rounds;
    // Non-dominated feasible evaluations (quality gate passed) seen during
    // the run, recomputed here so the report does not depend on Aura's
    // internal population order.
    std::vector<EvoFrontPoint> front;
    int promoted = 0;
    int rejected = 0;
    std::vector<std::string> final_rules;
    std::uint64_t final_rules_digest = 0;
    // Stable serialisation used by the determinism test.
    [[nodiscard]] std::string transcript() const;
};

// A long-lived `aura --serve` session running std/moead. ask/tell never
// reimplement the algorithm: they send one S-expression and read one JSON
// reply.
class AuraEvoSession {
public:
    AuraEvoSession(std::string bin, const EvoConfig& cfg);
    ~AuraEvoSession();
    AuraEvoSession(const AuraEvoSession&) = delete;
    AuraEvoSession& operator=(const AuraEvoSession&) = delete;

    [[nodiscard]] std::vector<EvoCandidate> ask(int k);
    void tell(const std::vector<EvoCandidate>& cands, const std::vector<ObjectiveVector>& objs,
              const std::vector<double>& quality_violation);

private:
    std::string command(const std::string& expr);  // returns the reply value
    void send(const std::string& expr);
    std::string read_line(int timeout_ms);

    std::string bin_;
    int in_fd_ = -1;
    int out_fd_ = -1;
    long pid_ = -1;
    std::string read_buf_;
};

// Encodes one genome as a rule set and evaluates/promotes it through
// Runtime's existing worldline API.
class EvoEngine {
public:
    EvoEngine(Runtime& rt, EvoConfig cfg);

    // Rules that a genome switches on. Exposed for the coverage test.
    static std::vector<NodeId> rules_for(const std::vector<double>& genome);
    static std::vector<std::string> rule_names();

    // Begin a worldline from the current live snapshot, install the genome's
    // rules, run the workload, and apply the quality + Pareto gate.
    EvoDecision evaluate(const std::vector<double>& genome, const std::vector<Intent>& workload,
                         bool allow_promote);

    // ask -> evaluate -> tell until the generation budget is spent.
    EvoRunReport run(const std::vector<Intent>& workload);

    const EvoConfig& config() const { return cfg_; }

private:
    void install_rules(WorldlineId wl, const std::vector<double>& genome);
    Runtime& rt_;
    EvoConfig cfg_;
};

} // namespace amr
