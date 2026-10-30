#include "amr/rules.hpp"

#include <algorithm>
#include <stdexcept>

namespace amr {

const char* tier_name(Tier t) {
    switch (t) {
    case Tier::Auto:
        return "auto";
    case Tier::NeedsConfirm:
        return "needs-confirm";
    case Tier::NeverAuto:
        return "never-auto";
    }
    return "?";
}

Tier parse_tier(std::string_view s) {
    if (s == "auto")
        return Tier::Auto;
    if (s == "needs-confirm")
        return Tier::NeedsConfirm;
    if (s == "never-auto")
        return Tier::NeverAuto;
    throw std::runtime_error("bad tier: " + std::string(s));
}

RuleView view_rule(NodeId rule) {
    const Node& n = node(rule);
    if (n.kind != "Rule" || n.kids.size() != 2)
        throw std::runtime_error("malformed rule: " + to_sexpr(rule));
    const Node& p = node(n.kids[0]);
    const Node& r = node(n.kids[1]);
    if (p.kind != "Pattern" || p.kids.size() != 1 || r.kind != "Rewrite" || r.kids.size() != 1)
        throw std::runtime_error("malformed rule body: " + to_sexpr(rule));
    RuleView v;
    v.id = rule;
    const std::string* nm = n.attr("name");
    if (!nm || nm->empty())
        throw std::runtime_error("rule without :name");
    v.name = *nm;
    const std::string* t = n.attr("tier");
    v.tier = t ? parse_tier(*t) : Tier::Auto;
    const std::string* pr = n.attr("prio");
    v.prio = pr ? std::stoi(*pr) : 0;
    v.pattern = p.kids[0];
    v.rewrite = r.kids[0];
    return v;
}

NodeId make_rule(std::string name, Tier tier, int prio, std::string_view pattern_sexpr,
                 std::string_view rewrite_sexpr, std::string doc) {
    std::vector<Attr> attrs{{"name", std::move(name)},
                            {"tier", tier_name(tier)},
                            {"prio", std::to_string(prio)}};
    if (!doc.empty())
        attrs.push_back({"doc", std::move(doc)});
    NodeId pat = mk("Pattern", {}, {parse_sexpr(pattern_sexpr)});
    NodeId rw = mk("Rewrite", {}, {parse_sexpr(rewrite_sexpr)});
    NodeId r = mk("Rule", std::move(attrs), {pat, rw});
    (void)view_rule(r);
    return r;
}

static bool is_var(std::string_view s) { return s.size() > 1 && s[0] == '?'; }

bool match(NodeId pattern, NodeId target, Bindings& b) {
    const Node& p = node(pattern);
    if (is_var(p.kind)) {
        auto it = b.nodes.find(p.kind);
        if (it != b.nodes.end())
            return deep_equal(it->second, target);
        b.nodes.emplace(p.kind, target);
        return true;
    }
    const Node& t = node(target);
    if (p.kind != "_" && p.kind != t.kind)
        return false;
    for (const auto& a : p.attrs) {
        if (a.key == "as") {
            if (!is_var(a.value))
                return false;
            b.nodes[a.value] = target;
            continue;
        }
        const std::string* tv = t.attr(a.key);
        if (a.value == "~") {
            if (tv)
                return false;
        } else if (a.value == "_") {
            if (!tv)
                return false;
        } else if (is_var(a.value)) {
            if (!tv)
                return false;
            auto it = b.strs.find(a.value);
            if (it != b.strs.end()) {
                if (it->second != *tv)
                    return false;
            } else {
                b.strs.emplace(a.value, *tv);
            }
        } else if (!tv || *tv != a.value) {
            return false;
        }
    }
    if (p.kids.size() > 0) {
        if (p.kids.size() != t.kids.size())
            return false;
        for (std::size_t i = 0; i < p.kids.size(); ++i)
            if (!match(p.kids[i], t.kids[i], b))
                return false;
    }
    return true;
}

static std::string subst(const std::string& v, const Bindings& b) {
    if (!is_var(v))
        return v;
    auto it = b.strs.find(v);
    if (it == b.strs.end())
        throw std::runtime_error("unbound template var " + v);
    return it->second;
}

NodeId instantiate(NodeId tmpl, const Bindings& b) {
    const Node& t = node(tmpl);
    std::vector<Attr> attrs;
    attrs.reserve(t.attrs.size());
    for (const auto& a : t.attrs)
        attrs.push_back({a.key, subst(a.value, b)});
    std::vector<NodeId> kids;
    kids.reserve(t.kids.size());
    for (std::size_t i = 0; i < t.kids.size(); ++i)
        kids.push_back(instantiate(t.kids[i], b));
    if (is_var(t.kind)) {
        auto it = b.nodes.find(t.kind);
        if (it == b.nodes.end())
            throw std::runtime_error("unbound template node " + t.kind);
        if (attrs.empty() && kids.empty())
            return it->second; // structural sharing
        NodeId base = attrs.empty() ? it->second : with_attrs(it->second, attrs);
        if (kids.empty())
            return base;
        const Node& bn = node(base);
        return mk(bn.kind, bn.attrs, std::move(kids));
    }
    return mk(t.kind, std::move(attrs), std::move(kids));
}

std::optional<std::size_t> RuleSet::find(std::string_view name) const {
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const std::string* nm = node(rules[i]).attr("name");
        if (nm && *nm == name)
            return i;
    }
    return std::nullopt;
}

std::optional<RuleView> RuleSet::get(std::string_view name) const {
    auto i = find(name);
    if (!i)
        return std::nullopt;
    return view_rule(rules[*i]);
}

RuleSet RuleSet::with_upsert(NodeId rule) const {
    RuleView v = view_rule(rule);
    RuleSet out;
    auto i = find(v.name);
    // NOTE: `rules` is const here, but Aura's const with_set() writes IN
    // PLACE when the storage is uniquely owned (Issue #2140 fast path). We
    // take a local copy first so use_count() >= 2 and the receiver is never
    // mutated (see tests/test_cow_sharing.cpp: pcv_const_with_set_hazard).
    Kids local = rules;
    out.rules = i ? local.with_set(*i, rule) : local.with_push_back(rule);
    return out;
}

RuleSet RuleSet::with_remove(std::string_view name) const {
    RuleSet out;
    auto i = find(name);
    Kids local = rules;
    out.rules = i ? local.with_erase(*i) : local;
    return out;
}

std::vector<NodeId> RuleSet::query(NodeId pattern) const {
    std::vector<NodeId> out;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        Bindings b;
        if (match(pattern, rules[i], b))
            out.push_back(rules[i]);
    }
    return out;
}

std::uint64_t RuleSet::digest() const {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < rules.size(); ++i)
        h = (h ^ deep_hash(rules[i])) * 1099511628211ull;
    return h;
}

std::vector<std::string> RuleSet::to_text() const {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < rules.size(); ++i)
        out.push_back(to_sexpr(rules[i]));
    return out;
}

namespace {
struct Rewriter {
    std::vector<RuleView> rules;
    OptStats& stats;
    bool changed = false;

    NodeId rewrite(NodeId id) {
        // children first (bottom-up); COW parent only if a child changed
        NodeId cur = id;
        const std::size_t nk = node(id).kids.size();
        for (std::size_t i = 0; i < nk; ++i) {
            NodeId c = node(cur).kids[i];
            NodeId nc = rewrite(c);
            if (nc != c)
                cur = with_child(cur, i, nc);
        }
        for (const auto& r : rules) {
            Bindings b;
            if (match(r.pattern, cur, b)) {
                NodeId out = instantiate(r.rewrite, b);
                if (out != cur && !deep_equal(out, cur)) {
                    ++stats.hits[r.name];
                    changed = true;
                    return out;
                }
            }
        }
        return cur;
    }
};
} // namespace

NodeId optimize(NodeId plan, const RuleSet& rs, OptStats& stats, int max_passes) {
    Rewriter w{{}, stats};
    for (std::size_t i = 0; i < rs.rules.size(); ++i)
        w.rules.push_back(view_rule(rs.rules[i]));
    std::stable_sort(w.rules.begin(), w.rules.end(),
                     [](const RuleView& a, const RuleView& b) { return a.prio > b.prio; });
    NodeId cur = plan;
    for (int p = 0; p < max_passes; ++p) {
        w.changed = false;
        cur = w.rewrite(cur);
        ++stats.passes;
        if (!w.changed)
            break;
    }
    return cur;
}

} // namespace amr
