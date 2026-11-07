// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopCheckpoint.h"

// Fleet-only bounded go-back-N. Receiver still commits the complete digest and
// caller-validated native manifest; save/checkpoint wire and sender are unchanged.
// Its ACK index proves a contiguous prefix: the existing Receiver never ACKs
// a future block across a gap. Coalescing ACKs therefore cannot lose progress.
namespace coop_fleet_transfer {
constexpr uint16_t kWindow = 16;
constexpr uint32_t kRetryMs = 150;
class Sender {
public:
    bool Start(uint64_t session,uint32_t transfer,uint32_t ticket,const char* name,
        const std::vector<uint8_t>& file,uint32_t now,uint32_t phase=0) {
        using namespace coop_checkpoint;
        if(!session || !transfer || !SafeName(name) || file.empty() || file.size()>kMaxFile) return false;
        Reset(); header_=Packet{};header_.session=session;header_.transfer=transfer;header_.ticket=ticket;
        header_.total=static_cast<uint32_t>(file.size());header_.digest=Digest(file);header_.launchKind=phase;
        header_.count=static_cast<uint16_t>((file.size()+kBlock-1)/kBlock);
        std::memcpy(header_.name,name,std::strlen(name)+1);file_=file;
        started_=lastActivity_=lastProgress_=now;lastCommit_=now-kRetryMs;active_=true;return true;
    }
    bool Tick(uint32_t now,coop_checkpoint::Packet& packet) {
        using namespace coop_checkpoint;
        if(!active_) return false;
        if(coop::SessionElapsed(now,lastProgress_)>=kInactiveTimeoutMs ||
           coop::SessionElapsed(now,started_)>=kMaxDurationMs) {active_=false;return false;}
        if(acknowledged_==header_.count) {
            if(coop::SessionElapsed(now,lastCommit_)<kRetryMs) return false;
            packet=header_;packet.index=header_.count;packet.type=Type::Commit;packet.bytes=0;
            lastCommit_=now;return true;
        }
        if(retrying_) {
            if(retryIndex_<acknowledged_) retryIndex_=acknowledged_;
            if(retryIndex_<sent_) {Fill(retryIndex_++,packet);return true;}
            retrying_=false;lastActivity_=now;
        }
        if(sent_<header_.count && sent_-acknowledged_<kWindow) {
            Fill(sent_++,packet);lastActivity_=now;return true;
        }
        if(coop::SessionElapsed(now,lastActivity_)<kRetryMs) return false;
        retryIndex_=acknowledged_;retrying_=true;
        Fill(retryIndex_++,packet);return true;
    }
    bool Ack(const coop_checkpoint::Packet& packet,uint32_t now) {
        using namespace coop_checkpoint;
        if(!active_ || !Valid(packet) || packet.type!=Type::Ack || !Matches(packet)) return false;
        if(packet.index==kComplete) {
            if(acknowledged_!=header_.count) return false;
            active_=false;done_=true;return true;
        }
        // Never credit unsent blocks or let delayed ACKs rewind the prefix.
        if(packet.index>=sent_ || packet.index>=header_.count || packet.index<acknowledged_) return false;
        acknowledged_=static_cast<uint16_t>(packet.index+1);lastActivity_=lastProgress_=now;
        if(acknowledged_==header_.count) {retrying_=false;lastCommit_=now-kRetryMs;}
        return false;
    }
    bool Active() const {return active_;}
    bool Done() const {return done_;}
    uint32_t Ticket() const {return header_.ticket;}
    void Reset() {
        file_.clear();header_=coop_checkpoint::Packet{};
        acknowledged_=sent_=retryIndex_=0;started_=lastActivity_=lastCommit_=lastProgress_=0;
        active_=done_=retrying_=false;
    }
private:
    bool Matches(const coop_checkpoint::Packet& packet) const {
        return packet.session==header_.session && packet.transfer==header_.transfer && packet.ticket==header_.ticket &&
            packet.total==header_.total && packet.digest==header_.digest && packet.count==header_.count &&
            packet.launchKind==header_.launchKind && !std::strcmp(packet.name,header_.name);
    }
    void Fill(uint16_t index,coop_checkpoint::Packet& packet) const {
        packet=header_;packet.index=index;
        const size_t offset=static_cast<size_t>(index)*coop_checkpoint::kBlock;
        packet.bytes=static_cast<uint16_t>((std::min)(coop_checkpoint::kBlock,file_.size()-offset));
        std::memcpy(packet.data,file_.data()+offset,packet.bytes);
    }
    coop_checkpoint::Packet header_{};std::vector<uint8_t> file_;
    uint16_t acknowledged_=0,sent_=0,retryIndex_=0;
    uint32_t started_=0,lastActivity_=0,lastCommit_=0,lastProgress_=0;
    bool active_=false,done_=false,retrying_=false;
};
}
