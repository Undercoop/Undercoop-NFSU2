// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// This is an installed-data bound, not permission to make an unpatched engine
// load more cars. Native loader support and matching peer assets are separate.
namespace coop_car_catalog {
constexpr std::uint32_t kStockCount = 46, kMaxModels = 256;
constexpr std::size_t kRecordBytes = 0x890, kWireNameBytes = 16;
constexpr std::size_t kMaxFileBytes = 64u * 1024u * 1024u;
enum class Error { None, FileSize, ChunkBounds, ChunkLimit, Nesting, Missing,
    DuplicateChunk, RecordSize, Count, Index, Name, LongName, DuplicateName };
struct Model { char name[kWireNameBytes]{}; std::uint32_t usage = 0; };
struct Catalog {
    std::array<Model, kMaxModels> models{};
    std::uint32_t count = 0;
    std::uint64_t fingerprint = 0;
    bool Contains(std::uint32_t index) const { return fingerprint && index < count; }
};
inline std::atomic<std::uint32_t> portableCount{kStockCount};
inline std::uint32_t PortableCount() { return portableCount.load(std::memory_order_acquire); }
inline bool PortableModelIndex(std::uint32_t model) { return model < PortableCount(); }
inline void PublishCount(std::uint32_t count) {
    portableCount.store(count >= kStockCount && count <= kMaxModels ? count : 0, std::memory_order_release);
}
inline std::uint32_t U32(const std::uint8_t* bytes) {
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
        (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}
inline void Mix(std::uint64_t& hash, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) { hash = (hash ^ std::uint8_t(value)) * 1099511628211ULL; value >>= 8; }
}
inline bool ReadIdentity(const std::uint8_t* record, Model& out, Error& error) {
    const void* end = std::memchr(record, 0, 32);
    if (!end || end == record) { error = Error::Name; return false; }
    const auto length = static_cast<const std::uint8_t*>(end) - record;
    if (length >= kWireNameBytes) { error = Error::LongName; return false; }
    for (std::ptrdiff_t i = 0; i < length; ++i)
        if (record[i] < 33 || record[i] > 126 || record[i] == '/' || record[i] == '\\') {
            error = Error::Name; return false;
        }
    out = {}; std::memcpy(out.name, record, static_cast<std::size_t>(length));
    out.usage = U32(record + 0x844);
    return true;
}
namespace detail {
struct Parser {
    const std::uint8_t* bytes; Catalog result{}; Error error = Error::None;
    std::size_t chunks = 0; bool found = false;
    bool Records(std::size_t at, std::size_t payloadEnd) {
        if (found) { error = Error::DuplicateChunk; return false; }
        found = true;
        // Native 636BF1 aligns the first CarTypeInfo after the chunk header.
        const std::size_t first = (at + 0x17) & ~std::size_t(15);
        if (first > payloadEnd || (payloadEnd - first) % kRecordBytes) { error = Error::RecordSize; return false; }
        const std::size_t count = (payloadEnd - first) / kRecordBytes;
        if (count < kStockCount || count > kMaxModels) { error = Error::Count; return false; }
        result.count = static_cast<std::uint32_t>(count);
        std::uint64_t hash = 14695981039346656037ULL; Mix(hash, result.count);
        for (std::uint32_t i = 0; i < result.count; ++i) {
            const auto* row = bytes + first + i * kRecordBytes;
            if (U32(row + 0x840) != i) { error = Error::Index; return false; }
            auto& model = result.models[i];
            if (!ReadIdentity(row, model, error)) return false;
            for (std::uint32_t j = 0; j < i; ++j)
                if (!std::strcmp(model.name, result.models[j].name)) { error = Error::DuplicateName; return false; }
            Mix(hash, i); Mix(hash, model.usage);
            for (char c : model.name) hash = (hash ^ std::uint8_t(c)) * 1099511628211ULL;
        }
        result.fingerprint = hash ? hash : 1; return true;
    }
    bool Walk(std::size_t begin, std::size_t end, unsigned depth) {
        if (depth > 8) { error = Error::Nesting; return false; }
        for (std::size_t at = begin; at < end;) {
            if (end - at < 8) { error = Error::ChunkBounds; return false; }
            if (++chunks > 65536) { error = Error::ChunkLimit; return false; }
            const auto id = U32(bytes + at), size = U32(bytes + at + 4);
            if (size > end - at - 8) { error = Error::ChunkBounds; return false; }
            const std::size_t next = at + 8 + size;
            if (id == 0x34600 && !Records(at, next)) return false;
            if ((id & 0x80000000u) && !Walk(at + 8, next, depth + 1)) return false;
            at = next;
        }
        return true;
    }
};
}
inline bool Parse(const void* bytes, std::size_t size, Catalog& output, Error& error) {
    output = {}; error = Error::None;
    if (!bytes || size < 8 || size > kMaxFileBytes) { error = Error::FileSize; return false; }
    detail::Parser parser{static_cast<const std::uint8_t*>(bytes)};
    if (!parser.Walk(0, size, 0)) { error = parser.error; return false; }
    if (!parser.found) { error = Error::Missing; return false; }
    output = parser.result; return true;
}
inline bool SameMapping(const Catalog& a, const Catalog& b) {
    if (!a.fingerprint || a.fingerprint != b.fingerprint || a.count != b.count) return false;
    for (std::uint32_t i = 0; i < a.count; ++i)
        if (a.models[i].usage != b.models[i].usage || std::strcmp(a.models[i].name, b.models[i].name)) return false;
    return true;
}
inline const char* ErrorText(Error error) {
    switch (error) {
    case Error::None: return "ready";
    case Error::FileSize: return "car catalog file size is unsupported";
    case Error::ChunkBounds: return "car catalog has a truncated chunk";
    case Error::ChunkLimit: return "car catalog has too many chunks";
    case Error::Nesting: return "car catalog nesting is unsupported";
    case Error::Missing: return "car catalog chunk is missing";
    case Error::DuplicateChunk: return "car catalog chunk is duplicated";
    case Error::RecordSize: return "car catalog record layout is unsupported";
    case Error::Count: return "car catalog must contain 46 to 256 models";
    case Error::Index: return "car catalog index does not match its record";
    case Error::Name: return "car catalog contains an invalid model identity";
    case Error::LongName: return "car model identities longer than 15 characters are unsupported";
    case Error::DuplicateName: return "car catalog contains duplicate model identities";
    }
    return "car catalog is unsupported";
}
}
