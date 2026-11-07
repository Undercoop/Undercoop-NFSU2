// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <array>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include "CoopSession.h"

namespace coop_checkpoint {
constexpr uint32_t kMagic = 0x4632434e; // NC2F
constexpr size_t kBlock = 768, kMaxFile = 128 * 1024;
constexpr uint16_t kComplete = 0xffff;
constexpr uint16_t kRejected = 0xfffe;
constexpr uint32_t kRestoreCareer = 4;
constexpr uint32_t kInactiveTimeoutMs=60000,kMaxDurationMs=10*60*1000;
enum class Type : uint16_t { Data = 1, Ack = 2, Commit = 3 };
#pragma pack(push, 1)
struct Packet {
    uint32_t magic = kMagic;
    uint16_t version = 1, size = sizeof(Packet);
    uint64_t session = 0;
    uint32_t transfer = 0, ticket = 0, total = 0, digest = 0, launchKind = 0;
    Type type = Type::Data;
    uint16_t index = 0, count = 0, bytes = 0;
    char name[32]{};
    uint8_t data[kBlock]{};
};
#pragma pack(pop)
inline uint32_t Digest(const std::vector<uint8_t>& file) {
    uint32_t value = 2166136261u;
    for (uint8_t b : file) { value ^= b; value *= 16777619u; }
    return value;
}
inline bool SafeName(const char* name) {
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < 32; ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (!c) return i > 0 && name[i - 1] != '.' && name[i - 1] != ' ';
        if (c < 32 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '<' || c == '>' || c == '|' || c == '"') return false;
    }
    return false;
}
inline bool Valid(const Packet& p) {
    const bool header = p.magic == kMagic && p.version == 1 && p.size == sizeof(Packet) && p.session &&
        p.transfer && p.total > 0 && p.total <= kMaxFile && p.count == (p.total + kBlock - 1) / kBlock &&
        SafeName(p.name) && (p.type == Type::Data || p.type == Type::Ack || p.type == Type::Commit);
    if (!header) return false;
    if (p.type == Type::Data) {
        if (p.index >= p.count) return false;
        const size_t offset = size_t(p.index) * kBlock;
        return p.bytes == (std::min)(kBlock, size_t(p.total) - offset);
    }
    if (p.bytes) return false;
    // 0xFFFE is the existing Fleet application's manifest-rejected ACK.
    // Generic checkpoint senders do not credit it as data or completion.
    if (p.type == Type::Ack && p.index >= p.count && p.index != kComplete && p.index != 0xFFFE) return false;
    if (p.type == Type::Commit && p.index != p.count) return false;
    for (uint8_t byte : p.data) if (byte) return false;
    return true;
}
struct Completed { uint64_t session = 0; uint32_t transfer = 0, ticket = 0, launchKind = 0; char name[32]{}; std::vector<uint8_t> file; };

// Stop-and-wait, bounded retries; all mutable access belongs to the caller's session lock.
class Sender {
public:
    bool Start(uint64_t session, uint32_t transfer, uint32_t ticket, const char* name,
               const std::vector<uint8_t>& file, uint32_t now, uint32_t launchKind = 0) {
        if (!session || !transfer || !SafeName(name) || file.empty() || file.size() > kMaxFile) return false;
        header_ = Packet{}; header_.session = session; header_.transfer = transfer; header_.ticket = ticket;
        header_.total = static_cast<uint32_t>(file.size()); header_.digest = Digest(file);
        header_.launchKind = launchKind;
        header_.count = static_cast<uint16_t>((file.size() + kBlock - 1) / kBlock);
        std::memcpy(header_.name, name, std::strlen(name) + 1);
        file_ = file; index_ = 0; active_ = true; done_ = sentCurrent_ = false;
        lastSend_ = now - 500; started_ = lastProgress_ = now;
        return true;
    }
    bool Tick(uint32_t now, Packet& p) {
        if (!active_) return false;
        if (coop::SessionElapsed(now,lastProgress_) >= kInactiveTimeoutMs ||
            coop::SessionElapsed(now,started_) >= kMaxDurationMs) { active_ = false; return false; }
        if (coop::SessionElapsed(now,lastSend_) < 300) return false;
        p = header_; p.index = index_;
        if (index_ == header_.count) { p.type = Type::Commit; p.bytes = 0; }
        else {
            size_t offset = index_ * kBlock;
            p.bytes = static_cast<uint16_t>((std::min)(kBlock, file_.size() - offset));
            std::memcpy(p.data, file_.data() + offset, p.bytes);
        }
        sentCurrent_=true;lastSend_ = now; return true;
    }
    bool Ack(const Packet& p, uint32_t now) {
        if (!active_ || !Valid(p) || p.type != Type::Ack || !Matches(p)) return false;
        if (p.index == kComplete && index_ == header_.count) { active_ = false; done_ = true; return true; }
        if (p.index == index_ && index_ < header_.count && sentCurrent_) {
            ++index_;sentCurrent_=false;lastSend_=now-500;lastProgress_=now;
        }
        return false;
    }
    bool Done() const { return done_; }
    bool Active() const { return active_; }
    uint32_t Ticket() const { return header_.ticket; }
    bool RejectRestore(const Packet& p) {
        if(!active_ || header_.launchKind!=kRestoreCareer || index_!=header_.count || !sentCurrent_ ||
            !Valid(p) || p.type!=Type::Ack || p.index!=kRejected || !Matches(p))return false;
        Reset();return true;
    }
    void Reset() { file_.clear(); active_ = done_ = sentCurrent_ = false; header_ = Packet{}; }
private:
    bool Matches(const Packet& p) const {
        return p.session == header_.session && p.transfer == header_.transfer && p.ticket == header_.ticket &&
            p.total == header_.total && p.digest == header_.digest && p.count == header_.count &&
            p.launchKind == header_.launchKind && std::strcmp(p.name, header_.name) == 0;
    }
    Packet header_{}; std::vector<uint8_t> file_; uint16_t index_ = 0;
    uint32_t lastSend_ = 0, started_ = 0,lastProgress_=0; bool active_ = false, done_ = false,sentCurrent_=false;
};
class Receiver {
public:
    bool Receive(const Packet& p, Packet& ack) {
        if (!Valid(p) || p.type == Type::Ack) return false;
        if(rejected_ && Matches(p) && p.type==Type::Commit) {
            ack=header_;ack.type=Type::Ack;ack.index=kRejected;ack.bytes=0;
            std::memset(ack.data,0,sizeof(ack.data));return true;
        }
        if (committed_ && Matches(p)) {
            ack = p; ack.type = Type::Ack; ack.index = p.type == Type::Data ? p.index : kComplete; ack.bytes = 0;
            std::memset(ack.data, 0, sizeof(ack.data)); return true;
        }
        if((committed_ || rejected_) && p.session==header_.session && p.ticket==header_.ticket &&
            static_cast<int32_t>(p.transfer-header_.transfer)<=0) return false;
        // A newer complete transfer may replace an abandoned partial one.
        if(active_ && p.type == Type::Data && p.index == 0 && p.session == header_.session &&
            p.ticket == header_.ticket && p.launchKind == header_.launchKind &&
            static_cast<int32_t>(p.transfer-header_.transfer)>0) Reset();
        if (!active_) {
            if (p.type != Type::Data || p.index != 0) return false;
            header_ = p; active_ = true; committed_ = ready_ = taken_ = rejected_ = false;
            next_ = 0; file_.assign(p.total, 0);
        }
        if (!Matches(p)) return false;
        if (p.type == Type::Commit) {
            // Final ACK is withheld until the caller validates and atomically commits.
            if (committed_) {
                ack = p; ack.type = Type::Ack; ack.index = kComplete; ack.bytes = 0;
                std::memset(ack.data, 0, sizeof(ack.data)); return true;
            }
            return false;
        }
        if (p.index >= p.count || p.index > next_) return false;
        size_t offset = p.index * kBlock;
        if (p.bytes != (std::min)(kBlock, static_cast<size_t>(p.total) - offset)) return false;
        if (p.index == next_) {
            std::memcpy(file_.data() + offset, p.data, p.bytes); ++next_;
            if (next_ == p.count) {
                if (Digest(file_) != p.digest) { Reset(); return false; }
                ready_ = true;
            }
        }
        ack = p; ack.type = Type::Ack; ack.bytes = 0;
        std::memset(ack.data, 0, sizeof(ack.data)); return true;
    }
    bool Take(Completed& out) {
        if (!ready_ || taken_) return false;
        out.session = header_.session; out.transfer = header_.transfer; out.ticket = header_.ticket;
        out.launchKind = header_.launchKind;
        std::memcpy(out.name, header_.name, 32); out.file = file_; taken_ = true; return true;
    }
    bool Commit(bool success, Packet& ack) {
        if (!taken_ || !ready_) return false;
        if (!success) { Reset(); return false; }
        committed_ = true; active_ = false; ready_ = false; file_.clear();
        ack = header_; ack.type = Type::Ack; ack.index = kComplete; ack.bytes = 0;
        std::memset(ack.data, 0, sizeof(ack.data)); return true;
    }
    bool RejectRestore(Packet& ack) {
        if(!taken_ || !ready_ || header_.launchKind!=kRestoreCareer)return false;
        rejected_=true;active_=ready_=taken_=false;file_.clear();
        ack=header_;ack.type=Type::Ack;ack.index=kRejected;ack.bytes=0;
        std::memset(ack.data,0,sizeof(ack.data));return true;
    }
    void Reset() { file_.clear(); active_ = committed_ = ready_ = taken_ = rejected_ = false; next_ = 0; header_ = Packet{}; }
private:
    bool Matches(const Packet& p) const {
        return p.session == header_.session && p.transfer == header_.transfer && p.ticket == header_.ticket &&
            p.total == header_.total && p.digest == header_.digest && p.count == header_.count &&
            p.launchKind == header_.launchKind && std::strcmp(p.name, header_.name) == 0;
    }
    Packet header_{}; std::vector<uint8_t> file_; uint16_t next_ = 0;
    bool active_ = false, ready_ = false, taken_ = false, committed_ = false, rejected_ = false;
};
}
