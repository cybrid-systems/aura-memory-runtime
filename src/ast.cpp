#include "amr/ast.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace amr {

const std::string* Node::attr(std::string_view k) const noexcept {
    for (const auto& a : attrs)
        if (a.key == k)
            return &a.value;
    return nullptr;
}

NodePool& NodePool::global() {
    static NodePool pool;
    return pool;
}

NodeId NodePool::add(Node n) {
    std::sort(n.attrs.begin(), n.attrs.end(),
              [](const Attr& a, const Attr& b) { return a.key < b.key; });
    std::unique_lock lk(mu_);
    if (nodes_.size() >= kNoNode)
        throw std::runtime_error("NodePool exhausted");
    nodes_.push_back(std::make_unique<Node>(std::move(n)));
    return static_cast<NodeId>(nodes_.size() - 1);
}

const Node& NodePool::get(NodeId id) const {
    std::shared_lock lk(mu_);
    if (id >= nodes_.size())
        throw std::out_of_range("bad NodeId");
    return *nodes_[id];
}

std::size_t NodePool::size() const {
    std::shared_lock lk(mu_);
    return nodes_.size();
}

std::size_t NodePool::node_bytes(NodeId id) const {
    const Node& n = get(id);
    std::size_t b = sizeof(Node) + n.kind.capacity();
    for (const auto& a : n.attrs)
        b += sizeof(Attr) + a.key.capacity() + a.value.capacity();
    b += n.kids.size() * sizeof(NodeId);
    return b;
}

NodeId mk(std::string kind, std::vector<Attr> attrs, std::vector<NodeId> kids) {
    Node n;
    n.kind = std::move(kind);
    n.attrs = std::move(attrs);
    n.kids = Kids(kids.begin(), kids.end());
    return NodePool::global().add(std::move(n));
}

NodeId mk_kids(std::string kind, std::vector<Attr> attrs, Kids kids) {
    Node n;
    n.kind = std::move(kind);
    n.attrs = std::move(attrs);
    n.kids = std::move(kids);
    return NodePool::global().add(std::move(n));
}

NodeId with_attrs(NodeId base, const std::vector<Attr>& overrides) {
    const Node& b = node(base);
    Node n;
    n.kind = b.kind;
    n.attrs = b.attrs;
    for (const auto& o : overrides) {
        auto it = std::find_if(n.attrs.begin(), n.attrs.end(),
                               [&](const Attr& a) { return a.key == o.key; });
        if (it != n.attrs.end())
            it->value = o.value;
        else
            n.attrs.push_back(o);
    }
    n.kids = b.kids; // shared storage (refcount bump only)
    return NodePool::global().add(std::move(n));
}

NodeId with_child(NodeId base, std::size_t i, NodeId child) {
    const Node& b = node(base);
    Node n;
    n.kind = b.kind;
    n.attrs = b.attrs;
    Kids shared = b.kids;            // use_count >= 2 here, so with_set COWs
    n.kids = shared.with_set(i, child);
    return NodePool::global().add(std::move(n));
}

bool deep_equal(NodeId a, NodeId b) {
    if (a == b)
        return true;
    const Node& x = node(a);
    const Node& y = node(b);
    if (x.kind != y.kind || x.attrs != y.attrs || x.kids.size() != y.kids.size())
        return false;
    for (std::size_t i = 0; i < x.kids.size(); ++i)
        if (!deep_equal(x.kids[i], y.kids[i]))
            return false;
    return true;
}

static std::uint64_t fnv(std::uint64_t h, std::string_view s) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    h ^= 0xff;
    h *= 1099511628211ull;
    return h;
}

std::uint64_t deep_hash(NodeId a) {
    const Node& x = node(a);
    std::uint64_t h = fnv(1469598103934665603ull, x.kind);
    for (const auto& at : x.attrs)
        h = fnv(fnv(h, at.key), at.value);
    for (std::size_t i = 0; i < x.kids.size(); ++i)
        h = (h ^ deep_hash(x.kids[i])) * 1099511628211ull;
    return h;
}

std::size_t count_nodes(NodeId root) {
    const Node& n = node(root);
    std::size_t c = 1;
    for (std::size_t i = 0; i < n.kids.size(); ++i)
        c += count_nodes(n.kids[i]);
    return c;
}

void collect_nodes(NodeId root, std::unordered_set<NodeId>& out) {
    if (!out.insert(root).second)
        return;
    const Node& n = node(root);
    for (std::size_t i = 0; i < n.kids.size(); ++i)
        collect_nodes(n.kids[i], out);
}

NodeId find_kind(NodeId root, std::string_view kind) {
    const Node& n = node(root);
    if (n.kind == kind)
        return root;
    for (std::size_t i = 0; i < n.kids.size(); ++i) {
        NodeId r = find_kind(n.kids[i], kind);
        if (r != kNoNode)
            return r;
    }
    return kNoNode;
}

// ── s-expression printer / parser ───────────────────────────
static void quote(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out.push_back(c);
    }
    out.push_back('"');
}

static void print(std::string& out, NodeId id) {
    const Node& n = node(id);
    out.push_back('(');
    out += n.kind;
    for (const auto& a : n.attrs) {
        out += " :";
        out += a.key;
        out.push_back(' ');
        quote(out, a.value);
    }
    for (std::size_t i = 0; i < n.kids.size(); ++i) {
        out.push_back(' ');
        print(out, n.kids[i]);
    }
    out.push_back(')');
}

std::string to_sexpr(NodeId id) {
    std::string s;
    print(s, id);
    return s;
}

namespace {
struct Parser {
    std::string_view s;
    std::size_t i = 0;
    [[noreturn]] void fail(const char* what) const {
        throw std::runtime_error(std::string("sexpr parse error at ") + std::to_string(i) + ": " +
                                 what);
    }
    void ws() {
        while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ';')) {
            if (s[i] == ';')
                while (i < s.size() && s[i] != '\n')
                    ++i;
            else
                ++i;
        }
    }
    std::string atom() {
        ws();
        if (i >= s.size())
            fail("eof");
        if (s[i] == '"') {
            ++i;
            std::string out;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    ++i;
                    out.push_back(s[i] == 'n' ? '\n' : s[i]);
                } else {
                    out.push_back(s[i]);
                }
                ++i;
            }
            if (i >= s.size())
                fail("unterminated string");
            ++i;
            return out;
        }
        std::size_t b = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '(' &&
               s[i] != ')')
            ++i;
        if (b == i)
            fail("empty atom");
        return std::string(s.substr(b, i - b));
    }
    NodeId parse_node() {
        ws();
        if (i >= s.size() || s[i] != '(')
            fail("expected (");
        ++i;
        std::string kind = atom();
        std::vector<Attr> attrs;
        std::vector<NodeId> kids;
        for (;;) {
            ws();
            if (i >= s.size())
                fail("unterminated node");
            if (s[i] == ')') {
                ++i;
                break;
            }
            if (s[i] == ':') {
                ++i;
                std::string k = atom();
                std::string v = atom();
                attrs.push_back({std::move(k), std::move(v)});
            } else if (s[i] == '(') {
                kids.push_back(parse_node());
            } else {
                fail("unexpected token");
            }
        }
        return mk(std::move(kind), std::move(attrs), std::move(kids));
    }
};
} // namespace

NodeId parse_sexpr(std::string_view text) {
    Parser p{text};
    NodeId n = p.parse_node();
    p.ws();
    if (p.i != text.size())
        p.fail("trailing input");
    return n;
}

} // namespace amr
