#include "amr/runtime.hpp"

#include "compiler/fiber_cow_overlay.hh" // Aura per-fiber COW overlay (commit / rollback / conflict)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>
#include <unordered_set>

namespace fs = std::filesystem;

namespace amr {

namespace cow = aura::compiler::cow;
// The overlay value is the rule NodeId written by a worldline (kNoNode = removal).
using RuleOverlay = cow::OverlayRegistry<NodeId>;
static RuleOverlay& overlay() { return cow::registry<NodeId>(); }
static constexpr std::uint64_t kMainFiber = 0;

RuleMutation RuleMutation::upsert(NodeId rule) {
    RuleMutation m;
    m.op = Op::Upsert;
    m.rule = rule;
    m.name = view_rule(rule).name;
    return m;
}
RuleMutation RuleMutation::remove(std::string name) {
    RuleMutation m;
    m.op = Op::Remove;
    m.name = std::move(name);
    return m;
}

static void encode_mutation(Writer& w, const RuleMutation& m) {
    w.u8(static_cast<std::uint8_t>(m.op));
    w.str(m.name);
    w.str(m.op == RuleMutation::Op::Upsert ? to_sexpr(m.rule) : std::string());
}
static RuleMutation decode_mutation(Reader& r) {
    RuleMutation m;
    m.op = static_cast<RuleMutation::Op>(r.u8());
    m.name = r.str();
    std::string text = r.str();
    if (m.op == RuleMutation::Op::Upsert)
        m.rule = parse_sexpr(text);
    return m;
}

std::string encode_mutations(std::uint64_t base_version, const std::vector<RuleMutation>& ms) {
    Writer w;
    w.u64(base_version);
    w.u32(static_cast<std::uint32_t>(ms.size()));
    for (const auto& m : ms)
        encode_mutation(w, m);
    return w.buf;
}

static void apply_mutation(HotState& st, const RuleMutation& m) {
    st.rules = m.op == RuleMutation::Op::Upsert ? st.rules.with_upsert(m.rule)
                                                 : st.rules.with_remove(m.name);
    st.rule_modified_at[m.name] = st.main_version;
}

void apply_record(HotState& st, const LogRecord& r, ReplayCtx* ctx) {
    switch (r.type) {
    case RecType::MemPut:
        st.memories.put(decode_memory(r.payload));
        break;
    case RecType::MemDel: {
        Reader rd{r.payload};
        st.memories.erase(rd.u64());
        break;
    }
    case RecType::RuleUpsert: {
        ++st.main_version;
        apply_mutation(st, RuleMutation::upsert(parse_sexpr(r.payload)));
        break;
    }
    case RecType::RuleRemove:
        ++st.main_version;
        apply_mutation(st, RuleMutation::remove(r.payload));
        break;
    case RecType::WlBegin:
        if (ctx)
            ctx->open[r.worldline] = 0;
        break;
    case RecType::WlMutate:
        // Audit only. A worldline's mutations reach the main line exclusively
        // through its (self-contained) WlPromote record.
        if (ctx)
            ++ctx->open[r.worldline];
        break;
    case RecType::WlPromote: {
        Reader rd{r.payload};
        (void)rd.u64(); // base version (audit)
        std::uint32_t n = rd.u32();
        ++st.main_version;
        for (std::uint32_t i = 0; i < n; ++i)
            apply_mutation(st, decode_mutation(rd));
        if (ctx)
            ctx->open.erase(r.worldline);
        break;
    }
    case RecType::WlAbort:
        if (ctx)
            ctx->open.erase(r.worldline);
        break;
    case RecType::Feedback:
    case RecType::Seal:
        break;
    }
    st.applied_seq = r.seq;
}

std::uint64_t HotState::digest() const {
    std::uint64_t h = memories.digest();
    h = (h ^ rules.digest()) * 1099511628211ull;
    h = (h ^ main_version) * 1099511628211ull;
    for (const auto& [k, v] : rule_modified_at) {
        for (char c : k)
            h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

double result_f1(const std::vector<std::uint64_t>& got, const std::vector<std::uint64_t>& want) {
    if (got.empty() && want.empty())
        return 1.0;
    std::unordered_set<std::uint64_t> w(want.begin(), want.end());
    std::size_t tp = 0;
    for (auto g : got)
        tp += w.count(g);
    if (tp == 0)
        return 0.0;
    double p = double(tp) / got.size(), r = double(tp) / want.size();
    return 2 * p * r / (p + r);
}

// ── worldline ──────────────────────────────────────────────
struct Runtime::Worldline {
    WorldlineId id = 0;
    Snapshot base;
    RuleSet rules; // COW copy of base.rules, mutated privately
    std::vector<RuleMutation> muts;
    Feedback fb;
    std::unique_ptr<Fiber> fiber;
    bool needs_confirm = false;
};

// ── checkpoint (hot state + log position) ─────────────────
static constexpr char kCkptMagic[8] = {'A', 'M', 'R', 'C', 'K', 'P', 'T', '1'};

static std::string ckpt_name(std::uint64_t seq) {
    char b[48];
    std::snprintf(b, sizeof b, "ckpt-%016llu.ckpt", static_cast<unsigned long long>(seq));
    return b;
}

static std::string encode_checkpoint(const HotState& st, const std::map<WorldlineId, std::uint64_t>& open) {
    Writer w;
    w.buf.append(kCkptMagic, 8);
    w.u64(st.applied_seq);
    w.u64(st.main_version);
    w.u64(st.memories.next_id());
    auto rules = st.rules.to_text();
    w.u32(static_cast<std::uint32_t>(rules.size()));
    for (const auto& r : rules)
        w.str(r);
    w.u32(static_cast<std::uint32_t>(st.rule_modified_at.size()));
    for (const auto& [k, v] : st.rule_modified_at) {
        w.str(k);
        w.u64(v);
    }
    w.u64(st.memories.size());
    for (const auto& [id, m] : st.memories.all())
        w.str(encode_memory(m));
    w.u32(static_cast<std::uint32_t>(open.size()));
    for (const auto& [id, n] : open) {
        w.u64(id);
        w.u64(n);
    }
    w.u32(crc32c(w.buf.data(), w.buf.size()));
    return w.buf;
}

static bool decode_checkpoint(const std::string& buf, HotState& st,
                              std::map<WorldlineId, std::uint64_t>& open) {
    if (buf.size() < 12 || std::memcmp(buf.data(), kCkptMagic, 8) != 0)
        return false;
    Reader tail{std::string_view(buf).substr(buf.size() - 4)};
    if (tail.u32() != crc32c(buf.data(), buf.size() - 4))
        return false;
    try {
        Reader r{std::string_view(buf).substr(8, buf.size() - 12)};
        HotState s;
        s.applied_seq = r.u64();
        s.main_version = r.u64();
        std::uint64_t next_id = r.u64();
        std::uint32_t nr = r.u32();
        for (std::uint32_t i = 0; i < nr; ++i)
            s.rules = s.rules.with_upsert(parse_sexpr(r.str()));
        std::uint32_t nm = r.u32();
        for (std::uint32_t i = 0; i < nm; ++i) {
            std::string k = r.str();
            s.rule_modified_at[k] = r.u64();
        }
        std::uint64_t mems = r.u64();
        for (std::uint64_t i = 0; i < mems; ++i)
            s.memories.put(decode_memory(r.str()));
        s.memories.set_next_id(next_id);
        std::uint32_t no = r.u32();
        std::map<WorldlineId, std::uint64_t> o;
        for (std::uint32_t i = 0; i < no; ++i) {
            WorldlineId id = r.u64();
            o[id] = r.u64();
        }
        if (!r.done())
            return false;
        st = std::move(s);
        open = std::move(o);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// ── Runtime ────────────────────────────────────────────────
std::unique_ptr<Runtime> Runtime::open(const std::string& dir, RuntimeOptions opts) {
    std::unique_ptr<Runtime> rt(new Runtime());
    rt->dir_ = dir;
    rt->opts_ = opts;
    fs::create_directories(dir);
    rt->log_ = AppendLog::open((fs::path(dir) / "log").string(), opts.log, &rt->recovery_.log);
    ReplayCtx ctx;
    if (opts.use_checkpoint) {
        std::vector<std::string> ck;
        for (const auto& e : fs::directory_iterator(dir))
            if (e.path().extension() == ".ckpt")
                ck.push_back(e.path().string());
        std::sort(ck.rbegin(), ck.rend());
        for (const auto& p : ck) {
            std::ifstream f(p, std::ios::binary);
            std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            HotState s;
            std::map<WorldlineId, std::uint64_t> open;
            if (decode_checkpoint(buf, s, open) && s.applied_seq <= rt->log_->last_seq()) {
                rt->st_ = std::move(s);
                ctx.open = std::move(open);
                rt->recovery_.used_checkpoint = true;
                rt->recovery_.checkpoint_seq = rt->st_.applied_seq;
                break;
            }
        }
    }
    rt->log_->for_each(rt->st_.applied_seq, [&](const LogRecord& r) {
        apply_record(rt->st_, r, &ctx);
        ++rt->recovery_.replayed_records;
    });
    // Unfinished worldlines are discarded: nothing of theirs ever reached the
    // main line. Record the discard so the decision itself is auditable.
    for (const auto& [id, n] : ctx.open) {
        ++rt->recovery_.discarded_worldlines;
        rt->recovery_.discarded_mutations += n;
        rt->append(RecType::WlAbort, id, "discarded-at-recovery");
    }
    return rt;
}

Runtime::~Runtime() { overlay().purge(this); }

std::uint64_t Runtime::append(RecType t, std::uint64_t wl, const std::string& payload) {
    // Log first, then derive hot state with the same function recovery uses.
    std::uint64_t seq = log_->append(t, wl, payload);
    apply_record(st_, LogRecord{seq, wl, t, payload}, nullptr);
    return seq;
}

std::uint64_t Runtime::put_memory(Memory m) {
    std::lock_guard lk(mu_);
    if (m.id == 0)
        m.id = st_.memories.next_id();
    append(RecType::MemPut, 0, encode_memory(m));
    return m.id;
}

bool Runtime::delete_memory(std::uint64_t id) {
    std::lock_guard lk(mu_);
    if (!st_.memories.get(id))
        return false;
    Writer w;
    w.u64(id);
    append(RecType::MemDel, 0, w.buf);
    return true;
}

void Runtime::define_rule(NodeId rule) {
    std::lock_guard lk(mu_);
    RuleView v = view_rule(rule);
    // Main-line changes also go through the Aura overlay as fiber 0 so that
    // a worldline begun earlier that touches the same rule conflicts.
    overlay().begin(this, kMainFiber, st_.main_version);
    overlay().write(this, kMainFiber, v.name, rule, st_.main_version);
    overlay().commit(this, kMainFiber, st_.main_version, [](const std::string&, const NodeId&) {});
    append(RecType::RuleUpsert, 0, to_sexpr(rule));
}

void Runtime::remove_rule(const std::string& name) {
    std::lock_guard lk(mu_);
    overlay().begin(this, kMainFiber, st_.main_version);
    overlay().write(this, kMainFiber, name, kNoNode, st_.main_version);
    overlay().commit(this, kMainFiber, st_.main_version, [](const std::string&, const NodeId&) {});
    append(RecType::RuleRemove, 0, name);
}

QueryResult Runtime::query(const Intent& in) {
    std::lock_guard lk(mu_);
    QueryResult q;
    q.plan = optimize(intent_to_plan(in), st_.rules, q.opt);
    ExecResult e = execute(q.plan, st_.memories);
    q.ids = std::move(e.ids);
    q.m = e.m;
    return q;
}

Snapshot Runtime::snapshot() const {
    std::lock_guard lk(mu_);
    return Snapshot{st_.main_version, log_->last_seq(), st_.rules};
}

Runtime::Worldline& Runtime::wl_ref(WorldlineId id) {
    auto it = wls_.find(id);
    if (it == wls_.end())
        throw std::runtime_error("no active worldline " + std::to_string(id));
    return *it->second;
}
const Runtime::Worldline& Runtime::wl_ref(WorldlineId id) const {
    auto it = wls_.find(id);
    if (it == wls_.end())
        throw std::runtime_error("no active worldline " + std::to_string(id));
    return *it->second;
}

WorldlineId Runtime::begin_experiment(const Snapshot& base) {
    std::lock_guard lk(mu_);
    if (wls_.size() >= opts_.max_active_worldlines)
        throw WorldlineLimit("active worldline cap reached (" +
                             std::to_string(opts_.max_active_worldlines) + ")");
    if (base.main_version > st_.main_version)
        throw std::runtime_error("snapshot from the future");
    WorldlineId id = log_->last_seq() + 1;
    Writer w;
    w.u64(base.main_version);
    std::uint64_t seq = append(RecType::WlBegin, id, w.buf);
    if (seq != id)
        throw std::logic_error("worldline id / seq mismatch");
    auto wl = std::make_unique<Worldline>();
    wl->id = id;
    wl->base = base;
    wl->rules = base.rules; // O(1): shares the PCV storage and every rule AST
    wl->fiber = std::make_unique<Fiber>(id);
    overlay().begin(this, id, base.main_version);
    wls_.emplace(id, std::move(wl));
    return id;
}

void Runtime::mutate_rules(WorldlineId id, const RuleMutation& m) {
    std::lock_guard lk(mu_);
    Worldline& wl = wl_ref(id);
    auto check_existing = [&](const RuleSet& rs) {
        if (auto r = rs.get(m.name)) {
            if (r->tier == Tier::NeverAuto)
                throw TierViolation("rule '" + m.name + "' is never-auto: experiments may not touch it");
            if (r->tier == Tier::NeedsConfirm)
                wl.needs_confirm = true;
        }
    };
    check_existing(st_.rules);
    check_existing(wl.base.rules);
    check_existing(wl.rules);
    if (m.op == RuleMutation::Op::Upsert) {
        Tier t = view_rule(m.rule).tier;
        if (t == Tier::NeverAuto)
            throw TierViolation("experiments may not create never-auto rules");
        if (t == Tier::NeedsConfirm)
            wl.needs_confirm = true;
        wl.rules = wl.rules.with_upsert(m.rule);
    } else {
        wl.rules = wl.rules.with_remove(m.name);
    }
    wl.muts.push_back(m);
    overlay().write(this, id, m.name, m.op == RuleMutation::Op::Upsert ? m.rule : kNoNode,
                    wl.base.main_version);
    Writer w;
    encode_mutation(w, m);
    append(RecType::WlMutate, id, w.buf);
}

std::vector<QueryResult> Runtime::run_queries(WorldlineId id, const std::vector<Intent>& intents) {
    std::lock_guard lk(mu_);
    Worldline& wl = wl_ref(id);
    std::vector<QueryResult> out;
    Metrics cand_sum, base_sum;
    // The experiment runs on the worldline's own fiber: its own stack and its
    // own Aura execution Context. It only READS the main-line store.
    wl.fiber->run([&] {
        for (const auto& in : intents) {
            NodeId plan = intent_to_plan(in);
            QueryResult c;
            c.plan = optimize(plan, wl.rules, c.opt);
            ExecResult ce = execute(c.plan, st_.memories);
            c.ids = ce.ids;
            c.m = ce.m;
            OptStats bo;
            NodeId bplan = optimize(plan, wl.base.rules, bo);
            ExecResult be = execute(bplan, st_.memories);
            ++wl.fb.samples;
            wl.fb.candidate += c.m;
            wl.fb.baseline += be.m;
            wl.fb.quality_sum += result_f1(c.ids, be.ids);
            for (const auto& [k, v] : c.opt.hits)
                wl.fb.rule_hits[k] += v;
            cand_sum += c.m;
            base_sum += be.m;
            out.push_back(std::move(c));
            Fiber::yield(); // cooperative: one query per slice (shadow / low priority)
        }
    });
    Writer w;
    w.u64(intents.size());
    w.u64(static_cast<std::uint64_t>(cand_sum.cost()));
    w.u64(static_cast<std::uint64_t>(base_sum.cost()));
    append(RecType::Feedback, id, w.buf);
    return out;
}

const Feedback& Runtime::feedback(WorldlineId id) const {
    std::lock_guard lk(mu_);
    return wl_ref(id).fb;
}

RuleSet Runtime::worldline_rules(WorldlineId id) const {
    std::lock_guard lk(mu_);
    return wl_ref(id).rules;
}

Fiber& Runtime::worldline_fiber(WorldlineId id) {
    std::lock_guard lk(mu_);
    return *wl_ref(id).fiber;
}

std::size_t Runtime::active_worldlines() const {
    std::lock_guard lk(mu_);
    return wls_.size();
}

PromoteResult Runtime::promote(WorldlineId id, const PromotePolicy& pol) {
    std::lock_guard lk(mu_);
    Worldline& wl = wl_ref(id);
    PromoteResult res;
    if (wl.muts.empty()) {
        res.reason = "nothing to promote";
        return res;
    }
    if (wl.fb.samples < pol.min_samples) {
        res.reason = "insufficient samples";
        return res;
    }
    if (wl.needs_confirm && !pol.confirmed) {
        res.reason = "needs human confirmation";
        return res;
    }
    if (wl.fb.avg_quality() < pol.min_quality) {
        res.reason = "quality regression";
        return res;
    }
    if (pol.require_cost_improvement && !(wl.fb.avg_candidate_cost() < wl.fb.avg_baseline_cost())) {
        res.reason = "no cost improvement";
        return res;
    }
    // Stale-snapshot guard: a rule changed on the main line after the base
    // snapshot was taken (Aura's overlay base is "now", not the snapshot).
    for (const auto& m : wl.muts) {
        auto it = st_.rule_modified_at.find(m.name);
        if (it != st_.rule_modified_at.end() && it->second > wl.base.main_version) {
            overlay().rollback(this, id);
            res.aborted = true;
            res.reason = "conflict: '" + m.name + "' changed on main after the base snapshot";
            append(RecType::WlAbort, id, res.reason);
            wls_.erase(id);
            return res;
        }
    }
    // Aura overlay commit: first-committer-wins, no partial write set.
    auto cr = overlay().commit(this, id, st_.main_version, [](const std::string&, const NodeId&) {});
    if (!cr.ok) {
        res.aborted = true;
        res.reason = "conflict: " + (cr.conflict.empty() ? std::string("overlay missing") : cr.conflict);
        append(RecType::WlAbort, id, res.reason);
        wls_.erase(id);
        return res;
    }
    append(RecType::WlPromote, id, encode_mutations(wl.base.main_version, wl.muts));
    wls_.erase(id);
    res.ok = true;
    res.reason = "promoted";
    return res;
}

void Runtime::abort(WorldlineId id, const std::string& reason) {
    std::lock_guard lk(mu_);
    (void)wl_ref(id);
    overlay().rollback(this, id);
    append(RecType::WlAbort, id, reason);
    wls_.erase(id);
}

void Runtime::checkpoint() {
    std::lock_guard lk(mu_);
    std::map<WorldlineId, std::uint64_t> open;
    for (const auto& [id, wl] : wls_)
        open[id] = wl->muts.size();
    std::string buf = encode_checkpoint(st_, open);
    std::string final_path = (fs::path(dir_) / ckpt_name(st_.applied_seq)).string();
    std::string tmp = final_path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        throw std::runtime_error("checkpoint open failed");
    std::size_t done = 0;
    while (done < buf.size()) {
        ssize_t w = ::write(fd, buf.data() + done, buf.size() - done);
        if (w <= 0) {
            ::close(fd);
            throw std::runtime_error("checkpoint write failed");
        }
        done += static_cast<std::size_t>(w);
    }
    ::fsync(fd);
    ::close(fd);
    fs::rename(tmp, final_path);
    int d = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY);
    if (d >= 0) {
        ::fsync(d);
        ::close(d);
    }
    // keep the newest two checkpoints
    std::vector<std::string> ck;
    for (const auto& e : fs::directory_iterator(dir_))
        if (e.path().extension() == ".ckpt")
            ck.push_back(e.path().string());
    std::sort(ck.rbegin(), ck.rend());
    for (std::size_t i = 2; i < ck.size(); ++i)
        fs::remove(ck[i]);
}

HotState Runtime::replay_only(const std::string& dir) {
    LogOptions lo;
    lo.read_only = true;
    auto log = AppendLog::open((fs::path(dir) / "log").string(), lo);
    HotState st;
    ReplayCtx ctx;
    log->for_each(0, [&](const LogRecord& r) { apply_record(st, r, &ctx); });
    return st;
}

} // namespace amr
