// log.hpp — single-partition append-only log: the source of truth.
//
// Record layout (little endian):
//   off  size  field
//   0    4     magic        'AMRL' (0x4C524D41)
//   4    2     version      1
//   6    2     type         RecType
//   8    4     payload_len
//   12   8     seq          strictly +1 per record
//   20   8     worldline    0 = main line
//   28   4     header_crc   CRC32C over bytes [0,28)
//   32   N     payload
//   32+N 4     payload_crc  CRC32C over payload
// Segments: seg-00000001.log ... A segment is sealed by a Seal record whose
// payload = {u64 record_count, u32 segment_crc(CRC32C of all bytes before the
// seal record)}; a new segment is then opened. Only the LAST segment may be
// unsealed; recovery scans it and truncates at the first invalid record.
// A corrupt SEALED segment is a media error: open() fails closed.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace amr {

std::uint32_t crc32c(const void* data, std::size_t n, std::uint32_t crc = 0);

enum class RecType : std::uint16_t {
    MemPut = 1,
    MemDel = 2,
    RuleUpsert = 3,   // main-line rule change (human/admin)
    RuleRemove = 4,
    WlBegin = 5,      // worldline begin (payload: base main_version)
    WlMutate = 6,     // worldline rule mutation (audit; never applied on replay)
    WlPromote = 7,    // self-contained: carries every mutation being merged
    WlAbort = 8,
    Feedback = 9,     // per-worldline metrics (audit)
    Seal = 15,
};

inline constexpr std::uint32_t kLogMagic = 0x4C524D41u; // "AMRL"
inline constexpr std::uint16_t kLogVersion = 1;
inline constexpr std::size_t kHeaderBytes = 32;
inline constexpr std::size_t kMaxPayload = 64u << 20;

struct LogRecord {
    std::uint64_t seq = 0;
    std::uint64_t worldline = 0;
    RecType type = RecType::MemPut;
    std::string payload;
};

struct LogOptions {
    std::size_t segment_bytes = 4u << 20;
    bool fsync = true;
    bool read_only = false; // no truncation, no new segment, no appends
};

struct LogRecoveryReport {
    std::size_t segments = 0;
    std::size_t sealed_segments = 0;
    std::uint64_t records = 0;        // valid non-seal records
    std::uint64_t last_seq = 0;
    std::uint64_t truncated_bytes = 0;
    std::string truncate_reason;      // empty if the tail was clean
};

struct LogCorruption : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// Thrown by the fault injector: the process "crashed" mid-write.
struct SimulatedCrash : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string encode_record(const LogRecord& r);

class AppendLog {
public:
    static std::unique_ptr<AppendLog> open(const std::string& dir, LogOptions opts,
                                           LogRecoveryReport* report = nullptr);
    ~AppendLog();
    AppendLog(const AppendLog&) = delete;
    AppendLog& operator=(const AppendLog&) = delete;

    // Appends and (if opts.fsync) fdatasync()s. Returns the record's seq.
    // A record is CONFIRMED once append() returns.
    std::uint64_t append(RecType type, std::uint64_t worldline, std::string_view payload);
    void seal(); // seal the active segment and roll to a new one
    // Visit every record with seq > after_seq, in order (Seal records skipped).
    void for_each(std::uint64_t after_seq, const std::function<void(const LogRecord&)>& fn) const;
    [[nodiscard]] std::uint64_t last_seq() const { return last_seq_; }
    [[nodiscard]] std::size_t segment_count() const { return segs_.size(); }
    [[nodiscard]] const std::string& dir() const { return dir_; }

    // Fault injection: after `bytes` more bytes are written the write is cut
    // short and SimulatedCrash is thrown; the log object is dead afterwards.
    void inject_crash_after_bytes(std::uint64_t bytes);

private:
    AppendLog() = default;
    struct Seg {
        std::string path;
        std::uint32_t index = 0;
        std::uint64_t first_seq = 0; // 0 = empty
        std::uint64_t last_seq = 0;
        bool sealed = false;
        std::uint64_t valid_bytes = 0;
    };
    void open_new_segment();
    void write_all(const std::string& buf);

    std::string dir_;
    LogOptions opts_;
    std::vector<Seg> segs_;
    int fd_ = -1;
    std::uint64_t active_bytes_ = 0;
    std::uint64_t active_records_ = 0;
    std::uint32_t active_crc_ = 0;
    std::uint64_t last_seq_ = 0;
    bool dead_ = false;
    std::int64_t crash_budget_ = -1;
};

} // namespace amr
