// rules.hpp — declarative pattern-match + rewrite rules, stored as data.
//
// A rule is itself an AST node:
//   (Rule :name "pushdown" :tier "auto" :prio "10" :doc "..."
//         (Pattern <pattern-node>) (Rewrite <template-node>))
//
// Pattern language (on plan ASTs):
//   (?x)                 node variable: matches any subtree, binds ?x
//   (Kind ...)           kind must equal ("_" = any kind)
//   :key "lit"           attr must equal literal
//   :key "?v"            attr must exist, binds string ?v
//   :key "_"             attr must exist
//   :key "~"             attr must be ABSENT (used for idempotence guards)
//   :as  "?n"            binds the matched node itself to ?n
//   children             if the pattern has children, counts must match and
//                        each child pattern must match
// Template language:
//   (?n :k "v" kids...)  copy of bound node ?n with attr overrides; children
//                        replaced only if the template lists children. With
//                        no overrides and no children the bound NodeId is
//                        reused as-is (structural sharing).
//   (Kind :k "?v" ...)   fresh node, "?v" substituted from string bindings
#pragma once

#include "amr/ast.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace amr {

enum class Tier { Auto = 0, NeedsConfirm = 1, NeverAuto = 2 };
const char* tier_name(Tier t);
Tier parse_tier(std::string_view s);

struct RuleView {
    NodeId id = kNoNode;
    std::string name;
    Tier tier = Tier::Auto;
    int prio = 0;
    NodeId pattern = kNoNode;
    NodeId rewrite = kNoNode;
};
// Validates shape; throws std::runtime_error on malformed rules.
RuleView view_rule(NodeId rule);
NodeId make_rule(std::string name, Tier tier, int prio, std::string_view pattern_sexpr,
                 std::string_view rewrite_sexpr, std::string doc = {});

struct Bindings {
    std::unordered_map<std::string, NodeId> nodes;
    std::unordered_map<std::string, std::string> strs;
};
bool match(NodeId pattern, NodeId target, Bindings& b);
NodeId instantiate(NodeId tmpl, const Bindings& b);

// A rule set is an Aura PersistentChildVector<NodeId> of Rule nodes.
// Copying a RuleSet is O(1) (refcount bump); with_upsert / with_remove
// return a new set and copy only the pointer array — rule ASTs are shared.
struct RuleSet {
    Kids rules;
    [[nodiscard]] std::size_t size() const { return rules.size(); }
    [[nodiscard]] std::optional<std::size_t> find(std::string_view name) const;
    [[nodiscard]] std::optional<RuleView> get(std::string_view name) const;
    [[nodiscard]] RuleSet with_upsert(NodeId rule) const;
    [[nodiscard]] RuleSet with_remove(std::string_view name) const;
    // Rules are data: query them with the same pattern language,
    // e.g. parse_sexpr("(Rule :tier \"auto\")").
    [[nodiscard]] std::vector<NodeId> query(NodeId pattern) const;
    [[nodiscard]] std::uint64_t digest() const;
    [[nodiscard]] std::vector<std::string> to_text() const;
};

struct OptStats {
    std::map<std::string, std::uint64_t> hits;
    int passes = 0;
};
// Rewrite to fixpoint (bottom-up, rules by descending priority, max passes).
NodeId optimize(NodeId plan, const RuleSet& rules, OptStats& stats, int max_passes = 16);

} // namespace amr
