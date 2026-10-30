#include "amr/store.hpp"

#include <cstring>
#include <stdexcept>

namespace amr {

void Writer::u32(std::uint32_t v) {
    for (int k = 0; k < 4; ++k)
        buf.push_back(static_cast<char>((v >> (8 * k)) & 0xff));
}
void Writer::u64(std::uint64_t v) {
    for (int k = 0; k < 8; ++k)
        buf.push_back(static_cast<char>((v >> (8 * k)) & 0xff));
}
void Writer::str(std::string_view s) {
    u32(static_cast<std::uint32_t>(s.size()));
    buf.append(s);
}
std::uint8_t Reader::u8() {
    if (i + 1 > s.size())
        throw std::runtime_error("decode: short u8");
    return static_cast<std::uint8_t>(s[i++]);
}
std::uint32_t Reader::u32() {
    if (i + 4 > s.size())
        throw std::runtime_error("decode: short u32");
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k)
        v |= std::uint32_t(static_cast<std::uint8_t>(s[i + k])) << (8 * k);
    i += 4;
    return v;
}
std::uint64_t Reader::u64() {
    if (i + 8 > s.size())
        throw std::runtime_error("decode: short u64");
    std::uint64_t v = 0;
    for (int k = 0; k < 8; ++k)
        v |= std::uint64_t(static_cast<std::uint8_t>(s[i + k])) << (8 * k);
    i += 8;
    return v;
}
std::string Reader::str() {
    std::uint32_t n = u32();
    if (i + n > s.size())
        throw std::runtime_error("decode: short str");
    std::string out(s.substr(i, n));
    i += n;
    return out;
}

std::string encode_memory(const Memory& m) {
    Writer w;
    w.u64(m.id);
    w.str(m.entity);
    w.str(m.source);
    w.str(m.tag);
    w.u64(static_cast<std::uint64_t>(m.ts));
    w.str(m.text);
    return w.buf;
}

Memory decode_memory(std::string_view s) {
    Reader r{s};
    Memory m;
    m.id = r.u64();
    m.entity = r.str();
    m.source = r.str();
    m.tag = r.str();
    m.ts = static_cast<std::int64_t>(r.u64());
    m.text = r.str();
    return m;
}

void MemoryStore::put(const Memory& m) {
    erase(m.id);
    by_id_[m.id] = m;
    by_source_[m.source].insert(m.id);
    by_entity_[m.entity].insert(m.id);
    if (m.id >= next_id_)
        next_id_ = m.id + 1;
}

bool MemoryStore::erase(std::uint64_t id) {
    auto it = by_id_.find(id);
    if (it == by_id_.end())
        return false;
    by_source_[it->second.source].erase(id);
    by_entity_[it->second.entity].erase(id);
    by_id_.erase(it);
    return true;
}

const Memory* MemoryStore::get(std::uint64_t id) const {
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &it->second;
}

void MemoryStore::scan_source(const std::string& source,
                              const std::function<void(const Memory&)>& fn) const {
    if (source == "*") {
        for (const auto& [id, m] : by_id_)
            fn(m);
        return;
    }
    auto it = by_source_.find(source);
    if (it == by_source_.end())
        return;
    for (auto id : it->second)
        fn(by_id_.at(id));
}

void MemoryStore::lookup_entity(const std::string& source, const std::string& entity,
                                const std::function<void(const Memory&)>& fn) const {
    auto it = by_entity_.find(entity);
    if (it == by_entity_.end())
        return;
    for (auto id : it->second) {
        const Memory& m = by_id_.at(id);
        if (source == "*" || m.source == source)
            fn(m);
    }
}

std::uint64_t MemoryStore::digest() const {
    std::uint64_t h = 1469598103934665603ull ^ next_id_;
    for (const auto& [id, m] : by_id_) {
        for (char c : encode_memory(m)) {
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ull;
        }
    }
    return h;
}

} // namespace amr
