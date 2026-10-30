#include "amr/plan.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <unordered_set>

namespace amr {

Metrics& Metrics::operator+=(const Metrics& o) {
    rows_scanned += o.rows_scanned;
    rows_processed += o.rows_processed;
    tokens += o.tokens;
    sink_calls += o.sink_calls;
    hint_violations += o.hint_violations;
    latency_ns += o.latency_ns;
    return *this;
}

NodeId intent_to_plan(const Intent& in) {
    std::vector<NodeId> leaves;
    for (const auto& s : in.sources)
        leaves.push_back(mk("Retrieve", {{"source", s}}));
    NodeId x = leaves.size() == 1 ? leaves[0] : mk("Parallel", {}, leaves);
    if (!in.entity.empty())
        x = mk("Filter", {{"field", "entity"}, {"op", "="}, {"value", in.entity}}, {x});
    if (in.since > 0)
        x = mk("Filter", {{"field", "ts"}, {"op", ">="}, {"value", std::to_string(in.since)}}, {x});
    if (!in.tag.empty())
        x = mk("Filter", {{"field", "tag"}, {"op", "="}, {"value", in.tag}}, {x});
    if (!in.relate_source.empty())
        x = mk("Join", {{"key", "entity"}}, {x, mk("Retrieve", {{"source", in.relate_source}})});
    x = mk("Rank", {{"by", "recency"}, {"k", std::to_string(in.top_k)}}, {x});
    x = mk("Assemble", {{"budget", std::to_string(in.budget_tokens)}}, {x});
    x = mk("CostHint",
           {{"max_tokens", std::to_string(in.budget_tokens)}, {"max_calls", "8"}}, {x});
    return x;
}

namespace {
using Rows = std::vector<const Memory*>;

bool pred(const Memory& m, const std::string& f, const std::string& op, const std::string& v) {
    if (f == "ts") {
        std::int64_t x = m.ts, y = std::stoll(v);
        if (op == ">=") return x >= y;
        if (op == "<=") return x <= y;
        if (op == ">") return x > y;
        if (op == "<") return x < y;
        if (op == "=") return x == y;
        if (op == "!=") return x != y;
        throw std::runtime_error("bad ts op " + op);
    }
    const std::string* s = nullptr;
    if (f == "entity") s = &m.entity;
    else if (f == "source") s = &m.source;
    else if (f == "tag") s = &m.tag;
    else if (f == "text") s = &m.text;
    else throw std::runtime_error("bad field " + f);
    if (op == "=") return *s == v;
    if (op == "!=") return *s != v;
    if (op == "contains") return s->find(v) != std::string::npos;
    throw std::runtime_error("bad op " + op);
}

std::uint64_t tokens_of(const std::string& t) {
    if (t.empty()) return 0;
    return 1 + static_cast<std::uint64_t>(std::count(t.begin(), t.end(), ' '));
}

const std::string& need(const Node& n, const char* k) {
    const std::string* v = n.attr(k);
    if (!v) throw std::runtime_error(n.kind + " missing :" + k);
    return *v;
}

struct Exec {
    const MemoryStore& st;
    Metrics m;

    Rows run(NodeId id) {
        const Node& n = node(id);
        if (n.kind == "Retrieve") {
            ++m.sink_calls;
            Rows out;
            const std::string& src = need(n, "source");
            const std::string* pf = n.attr("pfield");
            if (pf) {
                const std::string& op = need(n, "pop");
                const std::string& v = need(n, "pvalue");
                if (*pf == "entity" && op == "=") {
                    st.lookup_entity(src, v, [&](const Memory& r) {
                        ++m.rows_scanned;
                        out.push_back(&r);
                    });
                } else {
                    st.scan_source(src, [&](const Memory& r) {
                        ++m.rows_scanned;
                        if (pred(r, *pf, op, v)) out.push_back(&r);
                    });
                }
            } else {
                st.scan_source(src, [&](const Memory& r) {
                    ++m.rows_scanned;
                    out.push_back(&r);
                });
            }
            return out;
        }
        if (n.kind == "Filter") {
            Rows in = run(n.kids.at(0));
            m.rows_processed += in.size();
            Rows out;
            const auto &f = need(n, "field"), &op = need(n, "op"), &v = need(n, "value");
            for (auto* r : in)
                if (pred(*r, f, op, v)) out.push_back(r);
            return out;
        }
        if (n.kind == "Parallel") {
            Rows out;
            std::unordered_set<std::uint64_t> seen;
            for (std::size_t i = 0; i < n.kids.size(); ++i)
                for (auto* r : run(n.kids[i]))
                    if (seen.insert(r->id).second) out.push_back(r);
            return out;
        }
        if (n.kind == "Join") {
            Rows l = run(n.kids.at(0)), r = run(n.kids.at(1));
            m.rows_processed += l.size() + r.size();
            std::unordered_set<std::string> le, re;
            for (auto* x : l) le.insert(x->entity);
            for (auto* x : r) re.insert(x->entity);
            Rows out;
            std::unordered_set<std::uint64_t> seen;
            for (auto* x : l)
                if (re.count(x->entity) && seen.insert(x->id).second) out.push_back(x);
            for (auto* x : r)
                if (le.count(x->entity) && seen.insert(x->id).second) out.push_back(x);
            return out;
        }
        if (n.kind == "Rank") {
            Rows in = run(n.kids.at(0));
            m.rows_processed += in.size();
            std::sort(in.begin(), in.end(), [](const Memory* a, const Memory* b) {
                return a->ts != b->ts ? a->ts > b->ts : a->id < b->id;
            });
            if (const std::string* k = n.attr("k")) {
                std::size_t kk = std::stoul(*k);
                if (in.size() > kk) in.resize(kk);
            }
            return in;
        }
        if (n.kind == "Assemble") {
            Rows in = run(n.kids.at(0));
            m.rows_processed += in.size();
            std::uint64_t budget = std::stoull(need(n, "budget"));
            Rows out;
            std::uint64_t used = 0;
            for (auto* r : in) {
                std::uint64_t t = tokens_of(r->text);
                if (used + t > budget) break;
                used += t;
                out.push_back(r);
            }
            m.tokens += used;
            return out;
        }
        if (n.kind == "CostHint") {
            Metrics before = m;
            Rows out = run(n.kids.at(0));
            if (const std::string* mt = n.attr("max_tokens"))
                if (m.tokens - before.tokens > std::stoull(*mt)) ++m.hint_violations;
            if (const std::string* mc = n.attr("max_calls"))
                if (m.sink_calls - before.sink_calls > std::stoull(*mc)) ++m.hint_violations;
            return out;
        }
        throw std::runtime_error("unknown plan node " + n.kind);
    }
};
} // namespace

ExecResult execute(NodeId plan, const MemoryStore& store) {
    auto t0 = std::chrono::steady_clock::now();
    Exec e{store, {}};
    Rows rows = e.run(plan);
    ExecResult r;
    r.m = e.m;
    r.ids.reserve(rows.size());
    for (auto* x : rows) r.ids.push_back(x->id);
    r.m.latency_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    return r;
}

} // namespace amr
