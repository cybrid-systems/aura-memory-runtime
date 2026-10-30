#include "amr/log.hpp"

#include "amr/store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace amr {

namespace {
std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
        t[i] = c;
    }
    return t;
}
const std::array<std::uint32_t, 256> kCrcTable = make_table();

void put16(char* p, std::uint16_t v) { std::memcpy(p, &v, 2); }
void put32(char* p, std::uint32_t v) { std::memcpy(p, &v, 4); }
void put64(char* p, std::uint64_t v) { std::memcpy(p, &v, 8); }
std::uint16_t get16(const char* p) { std::uint16_t v; std::memcpy(&v, p, 2); return v; }
std::uint32_t get32(const char* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
std::uint64_t get64(const char* p) { std::uint64_t v; std::memcpy(&v, p, 8); return v; }

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void fsync_dir(const std::string& dir) {
    int d = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (d >= 0) {
        ::fsync(d);
        ::close(d);
    }
}

std::string seg_name(std::uint32_t idx) {
    char b[32];
    std::snprintf(b, sizeof b, "seg-%08u.log", idx);
    return b;
}

// Parse one record at buf[off]. Returns bytes consumed, or 0 with `why` set.
std::size_t parse_one(std::string_view buf, std::size_t off, std::uint64_t expect_seq,
                      LogRecord& out, std::string& why) {
    if (buf.size() - off < kHeaderBytes) {
        why = "short header";
        return 0;
    }
    const char* h = buf.data() + off;
    if (get32(h) != kLogMagic) {
        why = "bad magic";
        return 0;
    }
    if (crc32c(h, 28) != get32(h + 28)) {
        why = "header crc mismatch";
        return 0;
    }
    if (get16(h + 4) != kLogVersion) {
        why = "unsupported version";
        return 0;
    }
    std::uint32_t len = get32(h + 8);
    if (len > kMaxPayload) {
        why = "payload too large";
        return 0;
    }
    if (buf.size() - off < kHeaderBytes + len + 4) {
        why = "short payload";
        return 0;
    }
    const char* p = h + kHeaderBytes;
    if (crc32c(p, len) != get32(p + len)) {
        why = "payload crc mismatch";
        return 0;
    }
    std::uint64_t seq = get64(h + 12);
    if (expect_seq != 0 && seq != expect_seq) {
        why = "seq discontinuity";
        return 0;
    }
    out.seq = seq;
    out.worldline = get64(h + 20);
    out.type = static_cast<RecType>(get16(h + 6));
    out.payload.assign(p, len);
    return kHeaderBytes + len + 4;
}
} // namespace

std::uint32_t crc32c(const void* data, std::size_t n, std::uint32_t crc) {
    const auto* p = static_cast<const unsigned char*>(data);
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i)
        crc = kCrcTable[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

std::string encode_record(const LogRecord& r) {
    std::string b(kHeaderBytes + r.payload.size() + 4, '\0');
    char* h = b.data();
    put32(h, kLogMagic);
    put16(h + 4, kLogVersion);
    put16(h + 6, static_cast<std::uint16_t>(r.type));
    put32(h + 8, static_cast<std::uint32_t>(r.payload.size()));
    put64(h + 12, r.seq);
    put64(h + 20, r.worldline);
    put32(h + 28, crc32c(h, 28));
    std::memcpy(h + kHeaderBytes, r.payload.data(), r.payload.size());
    put32(h + kHeaderBytes + r.payload.size(), crc32c(r.payload.data(), r.payload.size()));
    return b;
}

std::unique_ptr<AppendLog> AppendLog::open(const std::string& dir, LogOptions opts,
                                           LogRecoveryReport* report) {
    fs::create_directories(dir);
    std::unique_ptr<AppendLog> log(new AppendLog());
    log->dir_ = dir;
    log->opts_ = opts;
    LogRecoveryReport rep;

    std::vector<std::pair<std::uint32_t, std::string>> files;
    for (const auto& e : fs::directory_iterator(dir)) {
        auto name = e.path().filename().string();
        unsigned idx = 0;
        if (std::sscanf(name.c_str(), "seg-%08u.log", &idx) == 1 && name.size() == 16)
            files.emplace_back(idx, e.path().string());
    }
    std::sort(files.begin(), files.end());
    rep.segments = files.size();

    std::uint64_t expect = 1;
    for (std::size_t fi = 0; fi < files.size(); ++fi) {
        const bool last = fi + 1 == files.size();
        std::string buf = read_file(files[fi].second);
        Seg seg;
        seg.path = files[fi].second;
        seg.index = files[fi].first;
        std::size_t off = 0;
        std::uint32_t running = 0;
        std::uint64_t nrec = 0;
        std::string why;
        for (;;) {
            if (off == buf.size())
                break;
            LogRecord r;
            std::size_t used = parse_one(buf, off, expect, r, why);
            if (used == 0)
                break;
            if (r.type == RecType::Seal) {
                Reader rd{r.payload};
                std::uint64_t cnt = rd.u64();
                std::uint32_t scrc = rd.u32();
                if (cnt != nrec || scrc != running) {
                    why = "seal checksum mismatch";
                    break;
                }
                seg.sealed = true;
                ++expect;
                off += used;
                if (off != buf.size())
                    why = "bytes after seal";
                break;
            }
            if (seg.first_seq == 0)
                seg.first_seq = r.seq;
            seg.last_seq = r.seq;
            running = crc32c(buf.data() + off, used, running);
            ++nrec;
            ++rep.records;
            ++expect;
            off += used;
        }
        if (!last) {
            if (!seg.sealed || !why.empty())
                throw LogCorruption("sealed segment " + seg.path + " is corrupt: " +
                                    (why.empty() ? std::string("missing seal") : why));
            ++rep.sealed_segments;
        } else if (seg.sealed) {
            if (!why.empty())
                throw LogCorruption("sealed segment " + seg.path + ": " + why);
            ++rep.sealed_segments;
        } else if (off != buf.size()) {
            // torn / corrupt tail of the active segment: truncate to the last
            // valid record. Everything before `off` is the confirmed prefix.
            rep.truncated_bytes = buf.size() - off;
            rep.truncate_reason = why;
            if (opts.read_only) {
                // leave the file alone; readers stop at valid_bytes
            } else if (::truncate(seg.path.c_str(), static_cast<off_t>(off)) != 0)
                throw std::runtime_error("truncate failed: " + std::string(std::strerror(errno)));
            if (!opts.read_only) {
                int fd = ::open(seg.path.c_str(), O_WRONLY);
                if (fd >= 0) {
                    ::fsync(fd);
                    ::close(fd);
                }
            }
        }
        seg.valid_bytes = off;
        if (last && !seg.sealed) {
            log->active_bytes_ = off;
            log->active_records_ = nrec;
            log->active_crc_ = running;
        }
        log->segs_.push_back(seg);
    }
    log->last_seq_ = expect - 1;
    rep.last_seq = log->last_seq_;

    if (opts.read_only) {
        // no writer state
    } else if (log->segs_.empty() || log->segs_.back().sealed) {
        log->open_new_segment();
    } else {
        log->fd_ = ::open(log->segs_.back().path.c_str(), O_WRONLY | O_APPEND);
        if (log->fd_ < 0)
            throw std::runtime_error("open segment failed");
    }
    if (report)
        *report = rep;
    return log;
}

AppendLog::~AppendLog() {
    if (fd_ >= 0)
        ::close(fd_);
}

void AppendLog::open_new_segment() {
    if (fd_ >= 0)
        ::close(fd_);
    Seg s;
    s.index = segs_.empty() ? 1 : segs_.back().index + 1;
    s.path = (fs::path(dir_) / seg_name(s.index)).string();
    fd_ = ::open(s.path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_TRUNC, 0644);
    if (fd_ < 0)
        throw std::runtime_error("create segment failed");
    fsync_dir(dir_);
    segs_.push_back(s);
    active_bytes_ = 0;
    active_records_ = 0;
    active_crc_ = 0;
}

void AppendLog::write_all(const std::string& buf) {
    std::size_t n = buf.size();
    bool crash = false;
    if (crash_budget_ >= 0 && static_cast<std::uint64_t>(crash_budget_) < n) {
        n = static_cast<std::size_t>(crash_budget_);
        crash = true;
    }
    std::size_t done = 0;
    while (done < n) {
        ssize_t w = ::write(fd_, buf.data() + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            dead_ = true;
            throw std::runtime_error("log write failed: " + std::string(std::strerror(errno)));
        }
        done += static_cast<std::size_t>(w);
    }
    if (crash) {
        dead_ = true;
        ::fsync(fd_); // make the torn bytes durable: worst case for recovery
        throw SimulatedCrash("simulated crash after " + std::to_string(n) + " bytes");
    }
    if (crash_budget_ >= 0)
        crash_budget_ -= static_cast<std::int64_t>(n);
    if (opts_.fsync)
        ::fdatasync(fd_);
}

std::uint64_t AppendLog::append(RecType type, std::uint64_t worldline, std::string_view payload) {
    if (dead_)
        throw std::runtime_error("log is dead (crashed)");
    if (opts_.read_only)
        throw std::runtime_error("log opened read-only");
    if (payload.size() > kMaxPayload)
        throw std::runtime_error("payload too large");
    LogRecord r{last_seq_ + 1, worldline, type, std::string(payload)};
    std::string buf = encode_record(r);
    write_all(buf);
    active_crc_ = crc32c(buf.data(), buf.size(), active_crc_);
    active_bytes_ += buf.size();
    ++active_records_;
    last_seq_ = r.seq;
    auto& s = segs_.back();
    if (s.first_seq == 0)
        s.first_seq = r.seq;
    s.last_seq = r.seq;
    s.valid_bytes = active_bytes_;
    if (active_bytes_ >= opts_.segment_bytes)
        seal();
    return r.seq;
}

void AppendLog::seal() {
    if (dead_)
        throw std::runtime_error("log is dead (crashed)");
    Writer w;
    w.u64(active_records_);
    w.u32(active_crc_);
    LogRecord r{last_seq_ + 1, 0, RecType::Seal, w.buf};
    std::string buf = encode_record(r);
    write_all(buf);
    ::fsync(fd_);
    last_seq_ = r.seq;
    segs_.back().sealed = true;
    open_new_segment();
}

void AppendLog::for_each(std::uint64_t after_seq,
                         const std::function<void(const LogRecord&)>& fn) const {
    for (const auto& s : segs_) {
        if (s.first_seq == 0 || (s.last_seq != 0 && s.last_seq <= after_seq))
            continue;
        std::string buf = read_file(s.path);
        if (buf.size() > s.valid_bytes)
            buf.resize(s.valid_bytes);
        std::size_t off = 0;
        std::string why;
        while (off < buf.size()) {
            LogRecord r;
            std::size_t used = parse_one(buf, off, 0, r, why);
            if (used == 0)
                throw LogCorruption("record became unreadable in " + s.path + ": " + why);
            off += used;
            if (r.type == RecType::Seal || r.seq <= after_seq)
                continue;
            fn(r);
        }
    }
}

void AppendLog::inject_crash_after_bytes(std::uint64_t bytes) {
    crash_budget_ = static_cast<std::int64_t>(bytes);
}

} // namespace amr
