// plan.hpp — intent -> logical plan (EDSL AST) and the plan interpreter.
//
// EDSL node kinds:
//   (Retrieve :source s [:pfield f :pop o :pvalue v])   ; p* = pushed-down predicate
//   (Filter   :field f :op o :value v  child)
//   (Join     :key "entity" left right)                  ; Relate by entity
//   (Rank     :by "recency" [:k K]  child)
//   (Assemble :budget B [:safe "1"] child)
//   (CostHint :max_tokens B :max_calls C child)
//   (Parallel child child ...)
// Retrieve with a pushed predicate is the "kernel-sink" fragment: it runs
// against the hot-state indexes directly instead of materialising the source.
#pragma once

#include "amr/ast.hpp"
#include "amr/store.hpp"

#include <string>
#include <vector>

namespace amr {

struct Intent {
    std::string entity;                         // about whom / what
    std::vector<std::string> sources{"notes"};  // where to look
    std::int64_t since = 0;                     // time lower bound
    std::string tag;                            // optional tag filter
    std::string relate_source;                  // optional Join/Relate source
    int top_k = 8;
    int budget_tokens = 256;
};

NodeId intent_to_plan(const Intent& in);

struct Metrics {
    std::uint64_t rows_scanned = 0;   // rows touched by sink/storage
    std::uint64_t rows_processed = 0; // rows flowing through control-plane operators
    std::uint64_t tokens = 0;         // assembled context tokens
    std::uint64_t sink_calls = 0;     // Retrieve invocations
    std::uint64_t hint_violations = 0;
    std::uint64_t latency_ns = 0;
    [[nodiscard]] double cost() const { return double(rows_scanned + rows_processed); }
    Metrics& operator+=(const Metrics& o);
};

struct ExecResult {
    std::vector<std::uint64_t> ids; // assembled context, in order
    Metrics m;
};

ExecResult execute(NodeId plan, const MemoryStore& store);

} // namespace amr
