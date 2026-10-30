// store.hpp — hot state: memories + secondary indexes (Stage 1: in-memory
// ordered maps; Stage 2 would put this behind an LSM).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace amr {

struct Memory {
    std::uint64_t id = 0;
    std::string entity;
    std::string source;
    std::string tag;
    std::int64_t ts = 0;
    std::string text;
    bool operator==(const Memory&) const = default;
};

std::string encode_memory(const Memory& m);
Memory decode_memory(std::string_view s);

class MemoryStore {
public:
    void put(const Memory& m);
    bool erase(std::uint64_t id);
    [[nodiscard]] const Memory* get(std::uint64_t id) const;
    [[nodiscard]] std::size_t size() const { return by_id_.size(); }
    [[nodiscard]] std::uint64_t next_id() const { return next_id_; }
    void set_next_id(std::uint64_t v) { next_id_ = v; }
    // Full scan of one source ("*" = all), in id order.
    void scan_source(const std::string& source,
                     const std::function<void(const Memory&)>& fn) const;
    // Entity index lookup restricted to a source ("*" = all).
    void lookup_entity(const std::string& source, const std::string& entity,
                       const std::function<void(const Memory&)>& fn) const;
    [[nodiscard]] std::uint64_t digest() const;
    [[nodiscard]] const std::map<std::uint64_t, Memory>& all() const { return by_id_; }

private:
    std::map<std::uint64_t, Memory> by_id_;
    std::unordered_map<std::string, std::set<std::uint64_t>> by_source_;
    std::unordered_map<std::string, std::set<std::uint64_t>> by_entity_;
    std::uint64_t next_id_ = 1;
};

// Tiny binary codec helpers shared by log payloads and checkpoints.
struct Writer {
    std::string buf;
    void u8(std::uint8_t v) { buf.push_back(static_cast<char>(v)); }
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void str(std::string_view s);
};
struct Reader {
    std::string_view s;
    std::size_t i = 0;
    std::uint8_t u8();
    std::uint32_t u32();
    std::uint64_t u64();
    std::string str();
    [[nodiscard]] bool done() const { return i == s.size(); }
};

} // namespace amr
