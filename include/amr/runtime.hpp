// runtime.hpp — Query & Memory Runtime, Stage 1 (single machine loop).
//
//   main line      : RuleSet (Aura PCV, COW) + MemoryStore, derived from the log
//   worldline      : COW RuleSet copy + Aura fiber-overlay (conflict ledger)
//                    + its own Fiber (Aura fiber Context) + isolated feedback
//   log            : AppendLog is the source of truth; hot state = replay
//   checkpoint     : hot-state snapshot + log position; recovery = ckpt + tail
#pragma once

#include "amr/ast.hpp"
#include "amr/fiber.hpp"
#include "amr/log.hpp"
#include "amr/plan.hpp"
#include "amr/rules.hpp"
#include "amr/store.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace amr {

using WorldlineId = std::uint64_t; // == seq of its WlBegin record (unique forever)

struct RuntimeOptions {
    LogOptions log;
    std::size_t max_active_worldlines = 4;
    bool use_checkpoint = true; // recovery: newest valid checkpoint + log tail
};

struct Snapshot {
    std::uint64_t main_version = 0;
    std::uint64_t log_seq = 0;
    RuleSet rules; // O(1) COW copy of the main rule set
};

struct RuleMutation {
    enum class Op : std::uint8_t { Upsert = 1, Remove = 2 } op = Op::Upsert;
    NodeId rule = kNoNode;  // Upsert
    std::string name;       // rule name (both ops)
    static RuleMutation upsert(NodeId rule);
    static RuleMutation remove(std::string name);
};

struct QueryResult {
    std::vector<std::uint64_t> ids;
    Metrics m;
    NodeId plan = kNoNode;      // optimised plan that ran
    OptStats opt;
};

struct Feedback {
    std::uint64_t samples = 0;
    Metrics candidate;          // summed over samples, worldline rules
    Metrics baseline;           // same queries, base-snapshot rules
    double quality_sum = 0;     // per query: F1 of result ids vs baseline
    std::map<std::string, std::uint64_t> rule_hits;
    [[nodiscard]] double avg_candidate_cost() const { return samples ? candidate.cost() / samples : 0; }
    [[nodiscard]] double avg_baseline_cost() const { return samples ? baseline.cost() / samples : 0; }
    [[nodiscard]] double avg_quality() const { return samples ? quality_sum / samples : 0; }
};

struct PromotePolicy {
    std::uint64_t min_samples = 1;
    bool require_cost_improvement = true;
    double min_quality = 1.0;
    bool confirmed = false;  // human confirmation for needs-confirm rules
};

struct PromoteResult {
    bool ok = false;
    bool aborted = false;     // worldline was dropped (conflict)
    std::string reason;
};

struct RecoveryReport {
    LogRecoveryReport log;
    bool used_checkpoint = false;
    std::uint64_t checkpoint_seq = 0;
    std::uint64_t replayed_records = 0;
    std::uint64_t discarded_worldlines = 0;  // begun, never promoted/aborted
    std::uint64_t discarded_mutations = 0;   // WlMutate records not applied
};

struct TierViolation : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct WorldlineLimit : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Derived state. Everything here is a pure function of the log.
struct HotState {
    MemoryStore memories;
    RuleSet rules;
    std::uint64_t main_version = 0;
    std::uint64_t applied_seq = 0;
    std::map<std::string, std::uint64_t> rule_modified_at; // name -> main_version
    [[nodiscard]] std::uint64_t digest() const;
};

class Runtime {
public:
    static std::unique_ptr<Runtime> open(const std::string& dir, RuntimeOptions opts = {});
    ~Runtime();

    // ── main line ──
    std::uint64_t put_memory(Memory m); // returns id (assigned if m.id == 0)
    bool delete_memory(std::uint64_t id);
    void define_rule(NodeId rule);       // human/admin change, any tier
    void remove_rule(const std::string& name);
    QueryResult query(const Intent& in);
    [[nodiscard]] Snapshot snapshot() const;

    // ── worldlines ──
    WorldlineId begin_experiment(const Snapshot& base);
    void mutate_rules(WorldlineId wl, const RuleMutation& m);
    std::vector<QueryResult> run_queries(WorldlineId wl, const std::vector<Intent>& intents);
    [[nodiscard]] const Feedback& feedback(WorldlineId wl) const;
    [[nodiscard]] RuleSet worldline_rules(WorldlineId wl) const;
    PromoteResult promote(WorldlineId wl, const PromotePolicy& pol = {});
    void abort(WorldlineId wl, const std::string& reason = "aborted");
    [[nodiscard]] std::size_t active_worldlines() const;
    [[nodiscard]] Fiber& worldline_fiber(WorldlineId wl);

    // ── durability ──
    void checkpoint();
    [[nodiscard]] const RecoveryReport& recovery() const { return recovery_; }
    [[nodiscard]] const HotState& state() const { return st_; }
    [[nodiscard]] AppendLog& log() { return *log_; }

    // Rebuild hot state from the log alone (no checkpoint): test oracle.
    static HotState replay_only(const std::string& dir);

private:
    Runtime() = default;
    struct Worldline;
    Worldline& wl_ref(WorldlineId id);
    const Worldline& wl_ref(WorldlineId id) const;
    std::uint64_t append(RecType t, std::uint64_t wl, const std::string& payload);

    std::string dir_;
    RuntimeOptions opts_;
    std::unique_ptr<AppendLog> log_;
    HotState st_;
    RecoveryReport recovery_;
    std::map<WorldlineId, std::unique_ptr<Worldline>> wls_;
    mutable std::recursive_mutex mu_;
};

// Worldlines seen during replay that were begun but not (yet) closed.
struct ReplayCtx {
    std::map<WorldlineId, std::uint64_t> open; // wl -> #WlMutate records
};
// Applies one log record to hot state — the ONLY state transition function,
// used identically by the live path (after append) and by recovery.
void apply_record(HotState& st, const LogRecord& r, ReplayCtx* ctx = nullptr);
std::string encode_mutations(std::uint64_t base_version, const std::vector<RuleMutation>& ms);

// Result-set quality: F1 of `got` vs `want` (1.0 if both empty).
double result_f1(const std::vector<std::uint64_t>& got, const std::vector<std::uint64_t>& want);

} // namespace amr
