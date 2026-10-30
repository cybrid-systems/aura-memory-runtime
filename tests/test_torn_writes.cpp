// Claim (3): 半截写入被截断，已确认前缀不丢（故障注入）。
#include "testkit.hpp"

#include <csignal>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>

using namespace amr;
namespace fs = std::filesystem;

static std::string payload_for(std::uint64_t i) {
    return "payload-" + std::to_string(i) + std::string(i % 37, 'x');
}

static std::string seg1(const std::string& dir) { return dir + "/seg-00000001.log"; }

static std::vector<std::size_t> record_ends(const std::string& dir, int n) {
    // byte offset after each record (records are written back to back)
    std::vector<std::size_t> ends;
    std::size_t off = 0;
    for (int i = 1; i <= n; ++i) {
        off += kHeaderBytes + payload_for(i).size() + 4;
        ends.push_back(off);
    }
    (void)dir;
    return ends;
}

static void write_n(const std::string& dir, int n, LogOptions o = {}) {
    auto log = AppendLog::open(dir, o);
    for (int i = 1; i <= n; ++i)
        log->append(RecType::MemPut, 0, payload_for(i));
}

static void copy_dir(const std::string& a, const std::string& b) {
    fs::remove_all(b);
    fs::copy(a, b, fs::copy_options::recursive);
}

static std::vector<LogRecord> read_all(const std::string& dir, LogRecoveryReport* rep) {
    auto log = AppendLog::open(dir, LogOptions{}, rep);
    std::vector<LogRecord> v;
    log->for_each(0, [&](const LogRecord& r) { v.push_back(r); });
    return v;
}

TEST(truncate_at_every_byte_of_the_tail) {
    const int n = 12;
    auto base = tk::tmpdir("torn-base");
    write_n(base, n);
    auto ends = record_ends(base, n);
    CHECK(fs::file_size(seg1(base)) == ends.back());
    // every cut inside the last 3 records
    for (std::size_t cut = ends[n - 4] + 1; cut < ends.back(); ++cut) {
        auto d = base + "-c";
        copy_dir(base, d);
        fs::resize_file(seg1(d), cut);
        LogRecoveryReport rep;
        auto recs = read_all(d, &rep);
        std::size_t complete = 0;
        while (complete < ends.size() && ends[complete] <= cut)
            ++complete;
        CHECK(recs.size() == complete);
        CHECK(rep.truncated_bytes == cut - (complete ? ends[complete - 1] : 0));
        for (std::size_t i = 0; i < recs.size(); ++i)
            CHECK(recs[i].seq == i + 1 && recs[i].payload == payload_for(i + 1));
        // the log is writable again and seq continues without a gap
        {
            auto log = AppendLog::open(d, LogOptions{});
            CHECK(log->append(RecType::MemPut, 0, "after") == complete + 1);
        }
        CHECK(read_all(d, nullptr).size() == complete + 1);
    }
}

TEST(bit_flip_in_header_or_payload_of_tail_record) {
    const int n = 6;
    auto base = tk::tmpdir("flip-base");
    write_n(base, n);
    auto ends = record_ends(base, n);
    for (std::size_t pos = ends[n - 2]; pos < ends[n - 1]; ++pos) {
        auto d = base + "-f";
        copy_dir(base, d);
        {
            std::fstream f(seg1(d), std::ios::in | std::ios::out | std::ios::binary);
            f.seekg(static_cast<std::streamoff>(pos));
            char c;
            f.read(&c, 1);
            c ^= 0x5a;
            f.seekp(static_cast<std::streamoff>(pos));
            f.write(&c, 1);
        }
        LogRecoveryReport rep;
        auto recs = read_all(d, &rep);
        CHECK(recs.size() == static_cast<std::size_t>(n - 1));
        CHECK(!rep.truncate_reason.empty());
    }
}

TEST(sealed_segments_are_verified_and_fail_closed) {
    auto d = tk::tmpdir("seal");
    LogOptions o;
    o.segment_bytes = 512;
    write_n(d, 60, o);
    LogRecoveryReport rep;
    auto recs = read_all(d, &rep);
    CHECK(recs.size() == 60);
    CHECK(rep.segments > 3);
    CHECK(rep.sealed_segments == rep.segments - 1 || rep.sealed_segments == rep.segments);
    // corrupt one byte in the first (sealed) segment -> open must refuse
    {
        std::fstream f(seg1(d), std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(40);
        f.put('Z');
    }
    CHECK_THROWS(AppendLog::open(d, LogOptions{}), LogCorruption);
}

TEST(sigkill_during_appends_preserves_every_acknowledged_record) {
    for (int round = 0; round < 3; ++round) {
        auto d = tk::tmpdir("kill");
        int fds[2];
        CHECK(::pipe(fds) == 0);
        pid_t pid = ::fork();
        if (pid == 0) {
            ::close(fds[0]);
            LogOptions o;
            o.fsync = true;
            o.segment_bytes = 64 * 1024;
            auto log = AppendLog::open(d, o);
            for (std::uint64_t i = 1;; ++i) {
                std::uint64_t seq = log->append(RecType::MemPut, 0, payload_for(i));
                if (::write(fds[1], &seq, sizeof seq) != sizeof seq)
                    ::_exit(2);
            }
        }
        ::close(fds[1]);
        std::uint64_t acked = 0, seq = 0;
        const std::uint64_t kill_at = 300 + 250 * round;
        while (::read(fds[0], &seq, sizeof seq) == sizeof seq) {
            acked = seq;
            if (acked >= kill_at) {
                ::kill(pid, SIGKILL);
                break;
            }
        }
        // drain whatever was acked before the kill landed
        while (::read(fds[0], &seq, sizeof seq) == sizeof seq)
            acked = seq;
        ::close(fds[0]);
        int st = 0;
        ::waitpid(pid, &st, 0);
        // count only seals-free data records: seq numbering includes seal records
        LogRecoveryReport rep;
        auto log = AppendLog::open(d, LogOptions{}, &rep);
        CHECK(rep.last_seq >= acked);
        std::uint64_t data_i = 0, max_seq = 0;
        bool ok = true;
        log->for_each(0, [&](const LogRecord& r) {
            ++data_i;
            ok = ok && r.payload == payload_for(data_i);
            max_seq = r.seq;
        });
        CHECK(ok);
        CHECK(max_seq >= acked);
        std::printf("    round %d: acked seq %llu, recovered last seq %llu, truncated %llu B\n",
                    round, (unsigned long long)acked, (unsigned long long)rep.last_seq,
                    (unsigned long long)rep.truncated_bytes);
    }
}

TEST(runtime_put_memory_crash_at_any_byte) {
    auto base = tk::tmpdir("rt-put");
    std::uint64_t d0 = 0;
    std::size_t n0 = 0;
    {
        auto rt = Runtime::open(base, tk::fast_opts());
        tk::seed_rules(*rt);
        tk::seed_memories(*rt, 50);
        d0 = rt->state().digest();
        n0 = rt->state().memories.size();
    }
    Memory m;
    m.entity = "e1";
    m.source = "notes";
    m.text = "the new memory being written";
    const std::size_t rec = kHeaderBytes + encode_memory(Memory{51, "e1", "notes", "", 0, m.text}).size() + 4;
    for (std::size_t cut = 1; cut < rec; ++cut) {
        auto d = base + "-x";
        copy_dir(base, d);
        {
            auto rt = Runtime::open(d, tk::fast_opts());
            rt->log().inject_crash_after_bytes(cut);
            CHECK_THROWS(rt->put_memory(m), SimulatedCrash);
        }
        auto rt = Runtime::open(d, tk::fast_opts());
        CHECK(rt->state().memories.size() == n0);
        CHECK(rt->state().digest() == d0);
        CHECK(rt->recovery().log.truncated_bytes == cut);
    }
}

int main() { return tk::run_all("torn_writes"); }
