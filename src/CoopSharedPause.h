// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopTime.h"
#include <array>
#include <cstdint>
#include <cstring>

// Session-scoped reliable desired-state protocol, not a stream of toggles.
// No engine addresses, clocks, sockets or pointer-bearing wire data here.
namespace coop_pause {
constexpr std::uint32_t kMagic=0x5532434e; // NC2U
constexpr std::uint16_t kVersion=1;
constexpr std::uint32_t kRetryMs=200;
constexpr std::uint32_t kInitialPauseWaitMs=5000;
enum class Role : std::uint8_t { None,Host,Client };
enum class Type : std::uint32_t { Request=1,State=2,Ack=3 };
struct Scope {
    std::uint64_t session=0;
    std::uint32_t ticket=0,track=0,epoch=0;
    bool Valid() const {return session && ticket && track && ((track==4000 && !epoch) || (track!=4000 && epoch));}
    bool operator==(const Scope& b) const {return session==b.session && ticket==b.ticket && track==b.track && epoch==b.epoch;}
    bool operator!=(const Scope& b) const {return !(*this==b);}
};
#pragma pack(push,1)
struct Packet {
    std::uint32_t magic=kMagic;
    std::uint16_t version=kVersion,size=52;
    std::uint64_t session=0;
    std::uint32_t ticket=0,track=0,epoch=0;
    Type type=Type::State;
    std::uint32_t paused=0,revision=0,request=0,ackRequest=0,reserved=0;
};
#pragma pack(pop)
static_assert(sizeof(Packet)==52,"Shared pause wire layout changed");
inline bool Newer(std::uint32_t a,std::uint32_t b) {return a!=b && a-b<0x80000000u;}
inline Scope Identity(const Packet& p) {return {p.session,p.ticket,p.track,p.epoch};}
inline bool Valid(const Packet& p) {
    if(p.magic!=kMagic || p.version!=kVersion || p.size!=sizeof(p) || p.reserved ||
        !Identity(p).Valid() || p.paused>1 || !p.revision) return false;
    if(p.type==Type::Request) return p.request && !p.ackRequest;
    if(p.type==Type::State) return !p.request;
    if(p.type==Type::Ack) return !p.request && !p.ackRequest;
    return false;
}
class Core {
public:
    void Bind(Scope scope,Role role,bool active,std::uint32_t now) {
        if(!scope.Valid() || role==Role::None) {Reset();return;}
        if(scope_!=scope || role_!=role) {
            Reset();scope_=scope;role_=role;revision_=1;floorRevision_=1;dirty_=role==Role::Host;lastSend_=now;
        }
        if(role_==Role::Host) ExpireInitialPause(now);
        if(active_!=active) {
            active_=active;pending_=false;paused_=active && role_==Role::Client && wirePaused_;requestDesired_=false;requestRebased_=false;
            // Every loader/frontend boundary fences old pause packets, even if
            // a transition began while already resumed. The identity is kept
            // across garage visits; old RF4/6 state cannot repause a fresh car.
            if(role_==Role::Host) {Next(revision_);floorRevision_=revision_;dirty_=true;awaitAck_=true;}
        }
        if(active_) everActive_=true;
        if(role_==Role::Host && active_ && initialPausePending_) {
            const Packet p=initialPause_;initialPausePending_=false;
            // Activation has already advanced the generation floor. Use the
            // ordinary reject/ACK path so Client rebases against that floor;
            // a deferred packet never acquires a loader-fence exemption.
            HostRequest(p);
        }
    }
    bool Request(bool paused,std::uint32_t now) {
        if(!active_ || role_==Role::None) return false;
        if(role_==Role::Host) {Commit(paused);return true;}
        if(pending_ && requestDesired_==paused) return true;
        Next(requestCounter_);pendingRequest_=requestCounter_;requestBase_=revision_;
        pending_=true;requestDesired_=paused;requestRebased_=false;requestStarted_=now;requestSent_=now-kRetryMs;return true;
    }
    void Receive(const Packet& p,std::uint32_t now) {
        if(!Valid(p) || Identity(p)!=scope_) return;
        if(role_==Role::Host) {
            ExpireInitialPause(now);
            if(p.type==Type::Ack) {if(p.revision==revision_ && p.paused==unsigned(paused_)) awaitAck_=false;return;}
            if(p.type!=Type::Request) return;
            if(initialPausePending_) {
                // Neither duplicate nor reordered older intent extends TTL.
                if(p.request==initialPause_.request) return;
                if(Newer(p.request,initialPause_.request)) initialPausePending_=false;
            }
            if(!active_ && !everActive_ && p.paused && p.revision==revision_ &&
                (!lastPeerRequest_ || Newer(p.request,lastPeerRequest_)) &&
                (!initialPausePending_ || Newer(p.request,initialPause_.request))) {
                // One portable request may arrive before this brand-new Host
                // scope's first UI activation. Do not ACK it as completed yet;
                // loaders keep running and no engine object is touched here.
                initialPause_=p;initialPauseAt_=now;initialPausePending_=true;return;
            }
            HostRequest(p);
        } else if(role_==Role::Client && p.type==Type::State) {
            if(Newer(revision_,p.revision)) return;
            if(p.revision==revision_ && p.paused!=unsigned(wirePaused_)) return;
            revision_=p.revision;wirePaused_=p.paused!=0;paused_=active_ && wirePaused_;
            if(pending_ && p.ackRequest==pendingRequest_) {
                std::uint32_t nextBase=requestBase_;Next(nextBase);
                // Initial Host activation fences revision1 with revision2.
                // A genuine Client pause made before that State arrived must
                // not instantly resume its already-open native menu. Rebase
                // that still-live desired-true intent ONCE, with a fresh ID.
                // Exactly one revision step is intentional: a complete
                // loader departure/reentry advances twice and may never be
                // turned into permission to pause the newly loaded world.
                // Resume, cancelled/inactive intents and a second rejection
                // remain fenced; duplicate ACKs cannot create retry IDs.
                if(active_ && requestDesired_ && !p.paused && !requestRebased_ && p.revision==nextBase &&
                    !InitialPauseExpired(now,requestStarted_)) {
                    Next(requestCounter_);pendingRequest_=requestCounter_;requestBase_=p.revision;
                    requestRebased_=true;requestSent_=now-kRetryMs;
                } else pending_=false;
            }
            ackDirty_=true;lastSend_=now;
        }
    }
    bool Pop(std::uint32_t now,Packet& p) {
        if(role_==Role::None || !scope_.Valid()) return false;
        if(role_==Role::Client) {
            if(ackDirty_) {ackDirty_=false;p=Make(Type::Ack);p.paused=wirePaused_?1u:0u;return true;}
            if(pending_ && coop_time::Elapsed(now,requestSent_)>=kRetryMs) {
                p=Make(Type::Request);p.paused=requestDesired_?1u:0u;p.revision=requestBase_;
                p.request=pendingRequest_;requestSent_=now;return true;
            }
            return false;
        }
        ExpireInitialPause(now);
        if(dirty_ || coop_time::Elapsed(now,lastSend_)>=(awaitAck_?kRetryMs:1000u)) {
            p=Make(Type::State);p.ackRequest=ackRequest_;dirty_=false;lastSend_=now;return true;
        }
        return false;
    }
    bool TargetPaused() const {return active_ && (paused_ || (role_==Role::Client && pending_ && requestDesired_));}
    bool Active() const {return active_;}
    bool Pending() const {return pending_;}
    bool Paused() const {return paused_;}
    std::uint32_t Revision() const {return revision_;}
    Scope CurrentScope() const {return scope_;}
    void Reset() {*this=Core{};}
private:
    static void Next(std::uint32_t& value) {if(!++value) ++value;}
    static bool InitialPauseExpired(std::uint32_t now,std::uint32_t received) {
        // Callers can sample their clock before contending for the Core lock.
        // An older sample is not an instant timeout; DWORD wrap remains valid
        // throughout this bounded five-second lease.
        return static_cast<std::int32_t>(now-received)>=static_cast<std::int32_t>(kInitialPauseWaitMs);
    }
    void HostRequest(const Packet& p,bool accept=true) {
        if(!lastPeerRequest_ || Newer(p.request,lastPeerRequest_)) {
            lastPeerRequest_=p.request;
            // A stale Resume never releases a newer pause, and both loader
            // floor checks are shared by live and deferred request handling.
            if(accept && active_ && !Newer(floorRevision_,p.revision) && !Newer(p.revision,revision_) &&
                (p.paused || p.revision==revision_)) Commit(p.paused!=0);
        }
        ackRequest_=lastPeerRequest_;dirty_=true;awaitAck_=true;
    }
    void ExpireInitialPause(std::uint32_t now) {
        if(!initialPausePending_ || !InitialPauseExpired(now,initialPauseAt_)) return;
        const Packet p=initialPause_;initialPausePending_=false;HostRequest(p,false);
    }
    void Commit(bool paused) {
        if(paused_!=paused) {paused_=paused;Next(revision_);}
        dirty_=true;awaitAck_=true;
    }
    Packet Make(Type type) const {
        Packet p{};p.session=scope_.session;p.ticket=scope_.ticket;p.track=scope_.track;p.epoch=scope_.epoch;
        p.type=type;p.paused=paused_?1u:0u;p.revision=revision_;return p;
    }
    Scope scope_{};Role role_=Role::None;
    std::uint32_t revision_=0,floorRevision_=0,requestCounter_=0,pendingRequest_=0,requestBase_=0,lastPeerRequest_=0;
    std::uint32_t ackRequest_=0,lastSend_=0,requestSent_=0,requestStarted_=0,initialPauseAt_=0;
    bool active_=false,paused_=false,wirePaused_=false,pending_=false,requestDesired_=false,requestRebased_=false;
    bool everActive_=false,initialPausePending_=false;
    Packet initialPause_{};
    bool dirty_=false,awaitAck_=false,ackDirty_=false;
};
}
