#include "amr/evo.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <poll.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace amr {
namespace {

constexpr const char* kPushdown = "evo-pushdown-filter";
constexpr const char* kParallel = "evo-filter-through-parallel";
constexpr const char* kDup = "evo-dup-retrieve";
constexpr const char* kDropAssemble = "evo-drop-assemble";

// One rule pool entry. Index i is switched by genome[i] > 0.5.
NodeId build_rule(std::size_t i) {
    switch (i) {
    case 0:
        return make_rule(kPushdown, Tier::Auto, 100,
                         R"((Filter :field "?f" :op "?o" :value "?v" (Retrieve :as "?r" :pfield "~")))",
                         R"((?r :pfield "?f" :pop "?o" :pvalue "?v"))",
                         "push a filter predicate into the storage sink");
    case 1:
        return make_rule(kParallel, Tier::Auto, 90,
                         R"((Filter :as "?f" (Parallel (?a) (?b))))",
                         R"((Parallel (?f (?a)) (?f (?b))))",
                         "distribute a filter over a 2-way Parallel");
    case 2:
        // The :dup marker makes the rule idempotent: the copies no longer
        // match `dup = ~`, so a fixpoint pass cannot double them again.
        return make_rule(kDup, Tier::Auto, 10,
                         R"((Retrieve :as "?r" :pfield "~" :dup "~"))",
                         R"((Parallel (?r :dup "1") (?r :dup "1")))",
                         "negative control: duplicate the sink call (same ids, 2x calls)");
    case 3:
        // Removing Assemble is strictly cheaper (one fewer control-plane
        // operator) and strictly wrong (the context is no longer budgeted),
        // which is the clean "cheaper but invalid" quality-gate probe.
        return make_rule(kDropAssemble, Tier::Auto, 5, R"((Assemble :as "?a" (?x)))",
                         R"((?x))",
                         "negative control: drop Assemble (cheaper, changes answers)");
    default:
        throw std::logic_error("bad rule index");
    }
}

std::string fmt(double v) {
    if (!std::isfinite(v)) return "0";
    return std::format("{:.17g}", v);
}

std::string getenv_or(const char* k, const std::string& d) {
    const char* v = std::getenv(k);
    return v && *v ? std::string(v) : d;
}

std::string json_unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++i >= s.size()) break;
        switch (s[i]) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'u': {
            // This protocol only carries ASCII ids/genomes; keep a placeholder
            // so a stray escape cannot desynchronise the parser.
            if (i + 4 < s.size()) i += 4;
            out.push_back('?');
            break;
        }
        default: out.push_back(s[i]); break;
        }
    }
    return out;
}

// Extracts the JSON string body for a key. Empty when the key is absent.
std::string json_string_field(const std::string& line, std::string_view key) {
    auto k = line.find(key);
    if (k == std::string::npos) return {};
    auto q = line.find('"', k + key.size());
    if (q == std::string::npos) return {};
    std::string raw;
    bool esc = false;
    for (std::size_t i = q + 1; i < line.size(); ++i) {
        char c = line[i];
        if (esc) {
            raw.push_back('\\');
            raw.push_back(c);
            esc = false;
            continue;
        }
        if (c == '\\') {
            esc = true;
            continue;
        }
        if (c == '"') return json_unescape(raw);
        raw.push_back(c);
    }
    return {};
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto p = s.find(sep, start);
        if (p == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

} // namespace

bool ObjectiveVector::weakly_dominates(const ObjectiveVector& o) const {
    return cost <= o.cost && tokens <= o.tokens && sink_calls <= o.sink_calls;
}

bool ObjectiveVector::pareto_improves(const ObjectiveVector& o) const {
    return weakly_dominates(o) &&
           (cost < o.cost || tokens < o.tokens || sink_calls < o.sink_calls);
}

// AuraEvoSession

AuraEvoSession::AuraEvoSession(std::string bin, const EvoConfig& cfg) {
    bin_ = !bin.empty() ? std::move(bin) : getenv_or("AMR_AURA_BIN", "");
    if (bin_.empty()) bin_ = "aura";
    if (::access(bin_.c_str(), X_OK) != 0)
        throw std::runtime_error("aura binary not found/executable: " + bin_ +
                                 " (set AMR_AURA_BIN or pass EvoConfig::aura_bin)");

    int in_pipe[2], out_pipe[2];
    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0)
        throw std::runtime_error("aura session: pipe() failed");
    pid_ = ::fork();
    if (pid_ < 0) throw std::runtime_error("aura session: fork() failed");
    if (pid_ == 0) {
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) ::dup2(devnull, STDERR_FILENO);
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        // Aura's serve path needs the sandbox decision and the non-strict
        // pipeline fallback; the evo log is an audit file this host owns, so
        // the harness turns it off (AURA_EVO_LOG empty).
        ::setenv("AURA_SANDBOX", "off", 1);
        ::setenv("AURA_PIPELINE_STRICT", "0", 1);
        ::setenv("AURA_EVO_LOG", "", 1);
        if (::access("/usr/bin/stdbuf", X_OK) == 0)
            ::execl("/usr/bin/stdbuf", "stdbuf", "-oL", "-eL", bin_.c_str(), "--serve",
                    static_cast<char*>(nullptr));
        ::execlp(bin_.c_str(), bin_.c_str(), "--serve", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    in_fd_ = in_pipe[1];
    out_fd_ = out_pipe[0];

    // Module prelude. std/list and std/math are not on the serve prelude, and
    // std/moead uses sort/expt. The helpers only format candidate values for
    // this boundary; the search itself stays in std/moead.
    command(R"((require "std/list" all:))");
    command(R"((require "std/math" all:))");
    command(R"((require "std/moead" all:))");
    command(
        R"((define (amr-genome-str g) (if (null? g) "" (if (null? (cdr g)) (number->string (car g)) (string-append (number->string (car g)) "," (amr-genome-str (cdr g)))))))");
    command(
        R"((define (amr-cand-str c) (string-append (evo:candidate-id c) "|" (number->string (evo:candidate-gen c)) "|" (number->string (evo:candidate-slot c)) "|" (amr-genome-str (evo:candidate-genome c)) ";")))");
    std::string make = std::format(
        R"((define amr-evo (moead:make (hash "n-obj" 3 "H" {} "T" {} "dim" {} "seed" {} "operator" "de" "objectives" (list "cost" "tokens" "sink_calls") "dirs" (list "min" "min" "min")))))",
        cfg.H, cfg.T, cfg.dimensions, cfg.seed);
    command(make);
}

AuraEvoSession::~AuraEvoSession() {
    if (in_fd_ >= 0) ::close(in_fd_);
    if (out_fd_ >= 0) ::close(out_fd_);
    if (pid_ > 0) {
        int st = 0;
        ::waitpid(pid_, &st, 0);
    }
}

void AuraEvoSession::send(const std::string& expr) {
    std::string line = expr;
    line.push_back('\n');
    std::size_t off = 0;
    while (off < line.size()) {
        ssize_t n = ::write(in_fd_, line.data() + off, line.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("aura session: write failed");
        }
        off += static_cast<std::size_t>(n);
    }
}

std::string AuraEvoSession::read_line(int timeout_ms) {
    auto& buf = read_buf_;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        auto p = buf.find('\n');
        if (p != std::string::npos) {
            std::string line = buf.substr(0, p);
            buf.erase(0, p + 1);
            return line;
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now())
                        .count();
        if (left <= 0) throw std::runtime_error("aura session: reply timeout");
        pollfd pfd{out_fd_, POLLIN, 0};
        int pr = ::poll(&pfd, 1, static_cast<int>(left));
        if (pr < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("aura session: poll failed");
        }
        if (pr == 0) throw std::runtime_error("aura session: reply timeout");
        char tmp[4096];
        ssize_t n = ::read(out_fd_, tmp, sizeof(tmp));
        if (n <= 0) throw std::runtime_error("aura session: aura exited");
        buf.append(tmp, static_cast<std::size_t>(n));
    }
}

std::string AuraEvoSession::command(const std::string& expr) {
    send(expr);
    std::string line = read_line(30000);
    auto status = json_string_field(line, "\"status\":");
    if (status != "ok") {
        auto msg = json_string_field(line, "\"msg\":");
        throw std::runtime_error("aura session error for '" + expr.substr(0, 80) + "': " +
                                 (msg.empty() ? line : msg));
    }
    return json_string_field(line, "\"value\":");
}

std::vector<EvoCandidate> AuraEvoSession::ask(int k) {
    command(std::format("(define amr-batch (moead:ask amr-evo {}))", k));
    std::string raw = command("(apply string-append (map amr-cand-str amr-batch))");
    // Aura's fmt_val prints a string value with its quotes; the protocol only
    // quotes it once more for JSON.
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
        raw = raw.substr(1, raw.size() - 2);
    std::vector<EvoCandidate> out;
    for (const auto& item : split(raw, ';')) {
        if (item.empty()) continue;
        auto f = split(item, '|');
        if (f.size() != 4)
            throw std::runtime_error("aura session: bad candidate record [" + item +
                                     "] in [" + raw + "]");
        EvoCandidate c;
        c.id = f[0];
        c.gen = std::stoi(f[1]);
        c.slot = std::stoi(f[2]);
        for (const auto& g : split(f[3], ','))
            if (!g.empty()) c.genome.push_back(std::stod(g));
        out.push_back(std::move(c));
    }
    return out;
}

void AuraEvoSession::tell(const std::vector<EvoCandidate>& cands,
                          const std::vector<ObjectiveVector>& objs,
                          const std::vector<double>& quality_violation) {
    std::string expr = "(moead:tell! amr-evo amr-batch (list ";
    for (std::size_t i = 0; i < objs.size(); ++i) {
        double q = i < quality_violation.size() ? quality_violation[i] : 0.0;
        expr += std::format(
            R"((hash "objectives" (hash "cost" {} "tokens" {} "sink_calls" {}) "constraints" (hash "quality" {})))",
            fmt(objs[i].cost), fmt(objs[i].tokens), fmt(objs[i].sink_calls), fmt(q));
        if (i + 1 < objs.size()) expr.push_back(' ');
    }
    expr += "))";
    (void)cands;
    auto r = command(expr);
    if (r != "#t") throw std::runtime_error("aura session: tell! did not return #t");
}

// EvoEngine

EvoEngine::EvoEngine(Runtime& rt, EvoConfig cfg) : rt_(rt), cfg_(std::move(cfg)) {}

std::vector<std::string> EvoEngine::rule_names() {
    return {kPushdown, kParallel, kDup, kDropAssemble};
}

std::vector<NodeId> EvoEngine::rules_for(const std::vector<double>& genome) {
    std::vector<NodeId> out(4, kNoNode);
    for (std::size_t i = 0; i < out.size(); ++i)
        if (i < genome.size() && genome[i] > 0.5) out[i] = build_rule(i);
    return out;
}

void EvoEngine::install_rules(WorldlineId wl, const std::vector<double>& genome) {
    auto nodes = rules_for(genome);
    auto names = rule_names();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i] != kNoNode) {
            rt_.mutate_rules(wl, RuleMutation::upsert(nodes[i]));
        } else if (rt_.worldline_rules(wl).find(names[i])) {
            rt_.mutate_rules(wl, RuleMutation::remove(names[i]));
        }
    }
}

EvoDecision EvoEngine::evaluate(const std::vector<double>& genome,
                                const std::vector<Intent>& workload, bool allow_promote) {
    EvoDecision d;
    d.genome = genome;
    WorldlineId wl = rt_.begin_experiment(rt_.snapshot());
    install_rules(wl, genome);
    rt_.run_queries(wl, workload);
    const Feedback& fb = rt_.feedback(wl);
    const double n = fb.samples ? static_cast<double>(fb.samples) : 1.0;
    d.baseline = {fb.baseline.cost() / n, static_cast<double>(fb.baseline.tokens) / n,
                  static_cast<double>(fb.baseline.sink_calls) / n};
    d.candidate = {fb.candidate.cost() / n, static_cast<double>(fb.candidate.tokens) / n,
                   static_cast<double>(fb.candidate.sink_calls) / n};
    d.quality = fb.avg_quality();

    if (d.quality + 1e-12 < cfg_.min_quality) {
        d.reason = std::format("quality regression: F1={:.4f} < {:.4f}", d.quality,
                               cfg_.min_quality);
    } else if (!d.candidate.pareto_improves(d.baseline)) {
        d.reason = std::format(
            "not a pareto improvement: cost {:.1f}->{:.1f}, tokens {:.1f}->{:.1f}, "
            "sink_calls {:.1f}->{:.1f}",
            d.baseline.cost, d.candidate.cost, d.baseline.tokens, d.candidate.tokens,
            d.baseline.sink_calls, d.candidate.sink_calls);
    }

    if (allow_promote && d.reason.empty()) {
        PromotePolicy pol;
        pol.min_quality = cfg_.min_quality;
        // The gate above is the Pareto gate; promote() still checks quality.
        pol.require_cost_improvement = false;
        auto pr = rt_.promote(wl, pol);
        d.promoted = pr.ok;
        if (!pr.ok) {
            d.reason = pr.reason;
            if (!pr.aborted) rt_.abort(wl, "promote refused: " + pr.reason);
        } else {
            d.reason = "promoted";
        }
    } else {
        if (d.reason.empty()) d.reason = "not selected";
        rt_.abort(wl, d.reason);
    }
    return d;
}

EvoRunReport EvoEngine::run(const std::vector<Intent>& workload) {
    AuraEvoSession session(cfg_.aura_bin, cfg_);
    EvoRunReport report;
    for (int g = 0; g < cfg_.generations; ++g) {
        auto cands = session.ask(cfg_.candidates_per_gen);
        if (cands.empty()) break;
        EvoRound round;
        round.gen = cands.front().gen;
        std::vector<ObjectiveVector> objs;
        std::vector<double> viol;
        for (const auto& c : cands) {
            auto d = evaluate(c.genome, workload, /*allow_promote=*/true);
            d.id = c.id;
            if (d.promoted) ++report.promoted; else ++report.rejected;
            objs.push_back(d.candidate);
            viol.push_back(std::max(0.0, 1.0 - d.quality));
            round.decisions.push_back(std::move(d));
        }
        session.tell(cands, objs, viol);
        report.rounds.push_back(std::move(round));
    }
    // Feasible non-dominated set over all evaluations. Duplicate vectors are
    // collapsed to their first (lowest) id so the report is stable.
    std::vector<EvoFrontPoint> feasible;
    for (const auto& round : report.rounds)
        for (const auto& d : round.decisions)
            if (d.quality + 1e-12 >= cfg_.min_quality)
                feasible.push_back({d.id, d.candidate});
    for (std::size_t i = 0; i < feasible.size(); ++i) {
        bool dominated = false;
        for (std::size_t j = 0; j < feasible.size(); ++j) {
            if (i == j) continue;
            const auto& p = feasible[i];
            const auto& q = feasible[j];
            if (q.obj.pareto_improves(p.obj) ||
                (q.obj == p.obj && (q.id < p.id || (q.id == p.id && j < i)))) {
                dominated = true;
                break;
            }
        }
        if (!dominated) report.front.push_back(feasible[i]);
    }
    report.final_rules = rt_.state().rules.to_text();
    report.final_rules_digest = rt_.state().rules.digest();
    return report;
}

std::string EvoRunReport::transcript() const {
    std::string out;
    for (const auto& r : rounds) {
        out += std::format("gen {}\n", r.gen);
        for (const auto& d : r.decisions) {
            out += d.id;
            out += " g=";
            for (double x : d.genome) out += fmt(x) + ",";
            out += std::format(
                " base=({},{},{}) cand=({},{},{}) q={} promoted={} reason={}\n",
                fmt(d.baseline.cost), fmt(d.baseline.tokens), fmt(d.baseline.sink_calls),
                fmt(d.candidate.cost), fmt(d.candidate.tokens), fmt(d.candidate.sink_calls),
                fmt(d.quality), d.promoted ? 1 : 0, d.reason);
        }
    }
    out += std::format("promoted={} rejected={} digest={}\n", promoted, rejected,
                       final_rules_digest);
    out += std::format("front={}\n", front.size());
    for (const auto& p : front)
        out += std::format("front {} ({},{},{})\n", p.id, fmt(p.obj.cost), fmt(p.obj.tokens),
                           fmt(p.obj.sink_calls));
    for (const auto& r : final_rules) out += "rule " + r + "\n";
    return out;
}

} // namespace amr
