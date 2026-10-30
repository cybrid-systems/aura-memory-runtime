// ast.hpp — EDSL plan / rule AST.
//
// 节点采用 Aura FlatAST 同款布局：节点存放在只追加的 NodePool 中，用 32 位
// NodeId 引用；每个节点的 children 是 Aura 的
// aura::ast::PersistentChildVector<NodeId>（third_party/aura/src/core/
// persistent_child_vector.hh，直接复用，未拷贝）。节点一旦创建即不可变，
// 任何"修改"都会创建新节点并通过 PCV with_set 共享未改动的子节点，
// 这就是规则集 / 计划上的 COW。
#pragma once

#include "core/persistent_child_vector.hh" // Aura COW children vector

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace amr {

using NodeId = std::uint32_t;
inline constexpr NodeId kNoNode = 0xffffffffu;
using Kids = aura::ast::PersistentChildVector<NodeId>;

struct Attr {
    std::string key;
    std::string value;
    bool operator==(const Attr&) const = default;
};

struct Node {
    std::string kind;
    std::vector<Attr> attrs; // kept sorted by key
    Kids kids;
    [[nodiscard]] const std::string* attr(std::string_view k) const noexcept;
};

// Process-wide, append-only, immutable node pool (Stage 1: no node GC).
class NodePool {
public:
    static NodePool& global();
    NodeId add(Node n);
    [[nodiscard]] const Node& get(NodeId id) const;
    [[nodiscard]] std::size_t size() const;
    // Rough heap footprint of one node (strings + attr vector + kids storage).
    [[nodiscard]] std::size_t node_bytes(NodeId id) const;

private:
    mutable std::shared_mutex mu_;
    std::vector<std::unique_ptr<Node>> nodes_;
};

inline const Node& node(NodeId id) { return NodePool::global().get(id); }

NodeId mk(std::string kind, std::vector<Attr> attrs = {}, std::vector<NodeId> kids = {});
NodeId mk_kids(std::string kind, std::vector<Attr> attrs, Kids kids);
// Copy of `base` with attrs overridden / added (children storage shared).
NodeId with_attrs(NodeId base, const std::vector<Attr>& overrides);
// Copy of `base` whose child i is replaced (PCV with_set => COW).
NodeId with_child(NodeId base, std::size_t i, NodeId child);

bool deep_equal(NodeId a, NodeId b);
std::uint64_t deep_hash(NodeId a);
std::size_t count_nodes(NodeId root);
void collect_nodes(NodeId root, std::unordered_set<NodeId>& out);

// S-expression text form:  (Kind :key "value" :k2 "v2" child child ...)
std::string to_sexpr(NodeId id);
NodeId parse_sexpr(std::string_view text); // throws std::runtime_error

// Find the first node (pre-order) of a given kind; kNoNode if none.
NodeId find_kind(NodeId root, std::string_view kind);

} // namespace amr
