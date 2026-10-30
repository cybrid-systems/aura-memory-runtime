// Claim (4): 热状态 == 日志重放（随机负载，含 checkpoint、多段、实验 promote/abort）。
#include "testkit.hpp"

#include <fstream>

using namespace amr;
namespace fs = std::filesystem;

static std::uint64_t run_random(const std::string& dir, std::uint64_t seed, int ops,
                                std::vector<std::vector<std::uint64_t>>* answers) {
    auto o = tk::fast_opts();
    o.log.segment_bytes = 16 * 1024; // force many sealed segments
    auto rt = Runtime::open(dir, o);
    tk::seed_rules(*rt);
    std::mt19937_64 rng(seed);
    std::vector<NodeId> candidates = {tk::pushdown_rule(), tk::filter_through_parallel_rule(),
                                      tk::bad_drop_ts_rule()};
    std::vector<std::uint64_t> ids;
    std::map<WorldlineId, int> open;
    for (int i = 0; i < ops; ++i) {
        int k = static_cast<int>(rng() % 100);
        if (k < 55) {
            Memory m;
            m.entity = "e" + std::to_string(rng() % 20);
            m.source = (rng() % 2) ? "notes" : "chat";
            m.tag = (rng() % 4 == 0) ? "secret" : "fact";
            m.ts = 1000 + i;
            m.text = "t" + std::to_string(rng() % 1000) + " u" + std::to_string(i);
            ids.push_back(rt->put_memory(m));
        } else if (k < 65 && !ids.empty()) {
            std::size_t j = rng() % ids.size();
            rt->delete_memory(ids[j]);
            ids.erase(ids.begin() + static_cast<long>(j));
        } else if (k < 72 && open.size() < 3) {
            open[rt->begin_experiment(rt->snapshot())] = 0;
        } else if (k < 82 && !open.empty()) {
            auto it = std::next(open.begin(), static_cast<long>(rng() % open.size()));
            rt->mutate_rules(it->first, RuleMutation::upsert(candidates[rng() % candidates.size()]));
            rt->run_queries(it->first, tk::workload(4, 20, rng()));
            ++it->second;
        } else if (k < 90 && !open.empty()) {
            auto it = std::next(open.begin(), static_cast<long>(rng() % open.size()));
            PromoteResult pr = rt->promote(it->first);
            if (!pr.ok && !pr.aborted)
                rt->abort(it->first, pr.reason);
            open.erase(it);
        } else if (k < 93) {
            // admin-side main-line rule churn
            if (rng() % 2)
                rt->define_rule(tk::rank_default_rule());
            else if (rt->state().rules.find("pushdown-filter-into-retrieve"))
                rt->remove_rule("pushdown-filter-into-retrieve");
        } else if (k < 95) {
            rt->checkpoint();
        }
    }
    // leave a couple of worldlines open on purpose (they must vanish on replay)
    if (answers)
        for (auto& q : tk::workload(25, 20, seed + 1))
            answers->push_back(rt->query(q).ids);
    return rt->state().digest();
}

TEST(live_equals_replay_equals_checkpoint_plus_tail) {
    for (std::uint64_t seed : {1ull, 2ull, 3ull, 42ull}) {
        auto dir = tk::tmpdir("replay");
        std::vector<std::vector<std::uint64_t>> live_answers;
        // open worldlines left at the end change nothing in main, so the live digest
        // is the main-line digest that recovery must reproduce
        const std::uint64_t live = run_random(dir, seed, 1500, &live_answers);
        const std::uint64_t pure = Runtime::replay_only(dir).digest();
        CHECK(pure == live);

        auto with_ckpt = Runtime::open(dir, tk::fast_opts());
        CHECK(with_ckpt->state().digest() == live);
        const bool used = with_ckpt->recovery().used_checkpoint;
        std::vector<std::vector<std::uint64_t>> answers;
        for (auto& q : tk::workload(25, 20, seed + 1))
            answers.push_back(with_ckpt->query(q).ids);
        CHECK(answers == live_answers);
        with_ckpt.reset();

        auto o = tk::fast_opts();
        o.use_checkpoint = false;
        auto full = Runtime::open(dir, o);
        CHECK(full->state().digest() == live);
        CHECK(!full->recovery().used_checkpoint);
        std::printf("    seed %llu: digest %016llx, checkpoint used=%d, segments=%zu\n",
                    (unsigned long long)seed, (unsigned long long)live, used ? 1 : 0,
                    full->log().segment_count());
    }
}

TEST(corrupt_checkpoint_falls_back_to_older_or_full_replay) {
    auto dir = tk::tmpdir("ckpt-bad");
    const std::uint64_t live = run_random(dir, 9, 800, nullptr);
    {
        auto rt = Runtime::open(dir, tk::fast_opts());
        rt->checkpoint();
    }
    std::vector<fs::path> ck;
    for (auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".ckpt")
            ck.push_back(e.path());
    std::sort(ck.begin(), ck.end());
    CHECK(!ck.empty());
    for (auto& p : ck) { // corrupt every checkpoint, one at a time newest-first
        {
            std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(static_cast<std::streamoff>(fs::file_size(p) / 2));
            f.put('\x7f');
        }
        auto rt = Runtime::open(dir, tk::fast_opts());
        CHECK(rt->state().digest() == live);
    }
}

TEST(checkpoint_ahead_of_log_is_ignored) {
    // a checkpoint whose seq is beyond the (truncated) log tail must not be trusted
    auto dir = tk::tmpdir("ckpt-ahead");
    std::uint64_t d_before = 0;
    {
        auto rt = Runtime::open(dir, tk::fast_opts());
        tk::seed_rules(*rt);
        tk::seed_memories(*rt, 100);
        d_before = rt->state().digest();
        tk::seed_memories(*rt, 1, 50, 99);
        rt->checkpoint();
    }
    // drop the last record from the log -> checkpoint now claims a seq the log lacks
    auto seg = dir + "/log/seg-00000001.log";
    std::uint64_t last_len = 0;
    {
        LogOptions ro;
        ro.read_only = true;
        auto log = AppendLog::open(dir + "/log", ro);
        log->for_each(0, [&](const LogRecord& r) { last_len = kHeaderBytes + r.payload.size() + 4; });
    }
    fs::resize_file(seg, fs::file_size(seg) - last_len);
    auto rt = Runtime::open(dir, tk::fast_opts());
    CHECK(!rt->recovery().used_checkpoint);
    CHECK(rt->state().digest() == d_before);
}

int main() { return tk::run_all("hot_state_replay"); }
