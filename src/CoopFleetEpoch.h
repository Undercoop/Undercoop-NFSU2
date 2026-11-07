// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopWorldWire.h"
#include "CoopSession.h"
#include "CoopCheckpoint.h"

// Pure loading handshake. The Client's per-load nonce is echoed by the Host;
// delayed offers from a prior same-track load cannot authorize a new pool.
// Local native generations and all object addresses stay off the wire.
namespace coop_fleet_load {
constexpr std::uint32_t kMagic = 0x4c46324eU; // N2FL; distinct from save checkpoints.
// ReceiveCoopSessionPacket runs first and consumes recognized checkpoint magic
// even when its packet size is wrong. A collision silently eats every load offer.
static_assert(kMagic != coop_checkpoint::kMagic && kMagic != coop::kSessionMagic &&
    kMagic != coop_world_wire::kEventMagic && kMagic != coop_world_wire::kLeaseMagic,
    "Fleet loading packets must reach their own network dispatcher");
enum class Type : std::uint8_t { Request = 1, Offer = 2 };
struct Message {
    Type type = Type::Request;
    std::uint8_t sender = 0;
    coop_world::Identity identity{};
    std::uint64_t nonce = 0;
    bool freeRoam = false, trafficAllowed = false;
    std::uint32_t flowState = 0;
    std::uint32_t presenceSequence = 0;
    std::uint32_t ticket = 0, membership = 0;
};
#pragma pack(push, 1)
struct Packet {
    std::uint32_t magic;
    std::uint16_t version, size;
    std::uint8_t type, sender, freeRoam, trafficAllowed;
    coop_world_wire::IdentityFields identity;
    std::uint64_t nonce;
    std::uint32_t flowState;
    std::uint32_t presenceSequence;
    std::uint32_t ticket, membership;
};
#pragma pack(pop)
static_assert(sizeof(Packet) == 56, "Fleet loading wire changed");
inline bool Valid(const Message& message) {
    return message.identity.sessionId && message.identity.trackId && message.nonce && message.flowState <= 6 &&
        (message.freeRoam == (message.identity.raceEpoch == 0)) &&
        ((message.type == Type::Request && message.sender == 2 && !message.identity.worldEpoch) ||
         (message.type == Type::Offer && message.sender == 1 && message.identity.worldEpoch));
}
inline bool Encode(const Message& message, Packet& packet) {
    if (!Valid(message)) return false;
    using namespace coop_world_wire;
    packet = {}; packet.magic = LE32(kMagic); packet.version = LE16(2); packet.size = LE16(sizeof(packet));
    packet.type = static_cast<std::uint8_t>(message.type); packet.sender = message.sender;
    packet.freeRoam = message.freeRoam; packet.trafficAllowed = message.trafficAllowed;
    packet.identity = {LE64(message.identity.sessionId), LE32(message.identity.trackId),
        LE32(message.identity.worldEpoch), LE32(message.identity.raceEpoch)};
    packet.nonce = LE64(message.nonce); packet.flowState = LE32(message.flowState);
    packet.presenceSequence = LE32(message.presenceSequence);
    packet.ticket=LE32(message.ticket);packet.membership=LE32(message.membership);return true;
}
inline bool Decode(const void* bytes, std::size_t size, Message& message) {
    if (!bytes || size != sizeof(Packet)) return false;
    Packet packet{}; std::memcpy(&packet, bytes, size);
    using namespace coop_world_wire;
    if (LE32(packet.magic) != kMagic || LE16(packet.version) != 2 || LE16(packet.size) != size ||
        packet.freeRoam > 1 || packet.trafficAllowed > 1) return false;
    message = {}; message.type = static_cast<Type>(packet.type); message.sender = packet.sender;
    message.identity = {LE64(packet.identity.sessionId), LE32(packet.identity.trackId),
        LE32(packet.identity.worldEpoch), LE32(packet.identity.raceEpoch)};
    message.nonce = LE64(packet.nonce); message.freeRoam = packet.freeRoam != 0;
    message.trafficAllowed = packet.trafficAllowed != 0; message.flowState = LE32(packet.flowState);
    message.presenceSequence = LE32(packet.presenceSequence);
    message.ticket=LE32(packet.ticket);message.membership=LE32(packet.membership);return Valid(message);
}
inline bool SameCampaign(const char* a,const char* b) {
    if(!a||!b||!a[0]||!b[0])return false;
    for(unsigned i=0;i<32;++i) {
        auto x=static_cast<unsigned char>(a[i]),y=static_cast<unsigned char>(b[i]);
        if(x>='a'&&x<='z')x-=32;if(y>='a'&&y<='z')y-=32;
        if(x!=y)return false;if(!x)return true;
    }
    return false;
}
class State {
public:
    bool Begin(const coop::SessionSnapshot& session, std::uint32_t track, std::uint32_t race,
        bool freeRoam, std::uint64_t generation, bool trafficAllowed, bool retainLiveFreeRoam = false) {
        if (!session.joined || !session.peer.Valid() || !session.sessionId || !track || !generation ||
            (session.role != coop::Role::Host && session.role != coop::Role::Client) || freeRoam != (race == 0)) return false;
        if (session.sessionId == identity_.sessionId && session.role == role_ && generation == generation_ &&
            track == identity_.trackId && race == identity_.raceEpoch && freeRoam == freeRoam_ &&
            session.ticket==ticket_&&session.membershipNonce==membership_) return true;
        // Only an authenticated wrapper may request this after the initial
        // joint barrier, while the authenticated peer still owns or is
        // reloading the SAME established FreeRoam.
        // Local native generations change; authority identity/policy do not.
        const bool retainedTicket=session.fresh&&session.careerLaunched&&
            (session.localRetainsNative||session.peerRetainsNative)&&SameCampaign(session.campaignName,campaign_);
        if (retainLiveFreeRoam && (session.initialBarrierComplete||retainedTicket) && session.role == coop::Role::Host &&
            role_ == coop::Role::Host && Ready() && session.sessionId == identity_.sessionId &&
            freeRoam && freeRoam_ && !race && track == identity_.trackId) {
            generation_ = generation; identitySealed_ = false;Bind(session,true);
            return true;
        }
        identitySealed_ = false;
        role_ = session.role; generation_ = generation; freeRoam_ = freeRoam; traffic_ = trafficAllowed;
        Bind(session,false);
        identity_ = {session.sessionId, track, 0, race}; peerNonce_ = 0;
        ++nonceCounter_; if (!nonceCounter_) ++nonceCounter_; nonce_ = nonceCounter_;
        if (role_ == coop::Role::Host) { ++epochCounter_; if (!epochCounter_) ++epochCounter_; identity_.worldEpoch = epochCounter_; }
        return true;
    }
    // A game-thread proof that this side still owns the already-loaded world.
    // Refresh transport scope only; do not rotate authority or native assets.
    bool RebindRetained(const coop::SessionSnapshot& session,std::uint64_t generation) {
        if(!Ready()||!freeRoam_||identity_.raceEpoch||!generation||generation!=generation_||
            !session.joined||!session.fresh||!session.peer.Valid()||!session.localRetainsNative||
            !session.careerLaunched||session.role!=role_||session.sessionId!=identity_.sessionId||
            !SameCampaign(session.campaignName,campaign_))return false;
        const bool changed=session.ticket!=ticket_||session.membershipNonce!=membership_;
        Bind(session,true);
        if(changed&&role_==coop::Role::Client){++nonceCounter_;if(!nonceCounter_)++nonceCounter_;nonce_=nonceCounter_;}
        return true;
    }
    bool Receive(const Message& message) {
        if (!Valid(message) || !identity_.sessionId || message.identity.sessionId != identity_.sessionId ||
            message.identity.trackId != identity_.trackId || message.identity.raceEpoch != identity_.raceEpoch ||
            message.freeRoam != freeRoam_ || message.ticket!=ticket_ || message.membership!=membership_) return false;
        if (role_ == coop::Role::Host && message.type == Type::Request) {
            if (peerNonce_ && coop_world::NewerRequest(peerNonce_, message.nonce)) return false;
            peerNonce_ = message.nonce; return true;
        }
        if (role_ != coop::Role::Client || message.type != Type::Offer || message.nonce != nonce_ ||
            (identitySealed_ && (message.identity != identity_ || message.trafficAllowed != traffic_)) ||
            (identity_.worldEpoch && message.identity.worldEpoch != identity_.worldEpoch &&
             !coop_world::Newer(message.identity.worldEpoch, identity_.worldEpoch))) return false;
        // An offer received after this pool has committed needs an explicit
        // local reload; the wrapper never applies it over a live generation.
        identity_ = message.identity; traffic_ = message.trafficAllowed; return true;
    }
    // The Stage3 owner calls this only after all current-generation assets and
    // human selections are finalized, immediately before native streaming.
    // Offers may converge while Stage3 waits, but cannot retarget Stage5/6.
    bool SealIdentity(std::uint64_t generation) {
        if (!Ready() || !generation || generation != generation_) return false;
        identitySealed_ = true; return true;
    }
    bool IdentitySealed() const { return identitySealed_; }
    bool Outgoing(Message& message) const {
        if (!identity_.sessionId) return false;
        message = {}; message.identity = identity_; message.freeRoam = freeRoam_; message.trafficAllowed = traffic_;
        message.ticket=ticket_;message.membership=membership_;
        if (role_ == coop::Role::Host) {
            if (!peerNonce_) return false;
            message.type = Type::Offer; message.sender = 1; message.nonce = peerNonce_;
        } else { message.type = Type::Request; message.sender = 2; message.nonce = nonce_; message.identity.worldEpoch = 0; }
        return Valid(message);
    }
    bool Ready() const { return identity_.Valid(); }
    const coop_world::Identity& Identity() const { return identity_; }
    std::uint64_t Generation() const { return generation_; }
    bool FreeRoam() const { return freeRoam_; }
    bool TrafficAllowed() const { return traffic_; }
    coop::Role Role() const { return role_; }
    bool Matches(const coop::SessionSnapshot& session) const {return ticket_==session.ticket&&membership_==session.membershipNonce;}
    const char* Campaign() const {return campaign_;}
    void Reset() { identity_ = {}; role_ = coop::Role::Idle; generation_ = nonce_ = peerNonce_ = 0; identitySealed_ = false;ticket_=membership_=0;campaign_[0]=0; }
private:
    void Bind(const coop::SessionSnapshot& session,bool preserve) {
        if(preserve&&(ticket_!=session.ticket||membership_!=session.membershipNonce))peerNonce_=0;
        ticket_=session.ticket;membership_=session.membershipNonce;
        std::memcpy(campaign_,session.campaignName,sizeof(campaign_));campaign_[31]=0;
    }
    coop_world::Identity identity_{};
    coop::Role role_ = coop::Role::Idle;
    std::uint64_t generation_ = 0, nonce_ = 0, peerNonce_ = 0, nonceCounter_ = 0;
    std::uint32_t epochCounter_ = 0;
    std::uint32_t ticket_=0,membership_=0;
    char campaign_[32]{};
    bool freeRoam_ = false, traffic_ = false, identitySealed_ = false;
};
}
