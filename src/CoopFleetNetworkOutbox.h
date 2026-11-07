// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopFleetEpoch.h"
#include <array>

// Portable, bounded game-thread -> network-thread mailbox. Caller owns locking.
// Source publications refresh latest values; only successful drains advance the
// pacing clock. An empty/newer game frame cannot erase an unsent publication.
namespace coop_fleet_net {
constexpr size_t kEntityLimit=64,kControlLimit=128,kBurstLimit=8;
constexpr uint32_t kActiveMs=50,kDormantMs=500,kRemoveMs=200,kLeaseMs=250,kPresenceMs=250,kFreshMs=200;
struct Datagram {uint16_t size=0;uint8_t bytes[sizeof(coop_world_wire::EventPacket)]{};};
struct Statistics {
    uint64_t published=0,coalesced=0,datagrams=0,bytes=0,expired=0,rejected=0,capacityRefused=0;
    uint32_t maxControls=0;
};
class Outbox {
    struct Value {
        Datagram wire{};uint32_t produced=0,sent=0;bool used=false,haveSent=false,critical=false;
    };
    struct Entity:Value {coop_world::Event event{};};
    struct Lease:Value {coop_world_wire::LeaseEnvelope state{};};
    struct Control {Datagram wire{};uint32_t produced=0;};
public:
    void Reset(){
        // Clear one bounded entry at a time: avoid a large temporary Outbox on
        // the native game thread's stack during a loading/epoch transition.
        for(auto& row:entities_)row=Entity{};for(auto& row:leases_)row=Lease{};for(auto& row:controls_)row=Control{};
        session_=0;peer_={};identity_={};controlHead_=controlCount_=entityCursor_=leaseCursor_=0;
        presence_={};presenceMessage_={};stats_={};normalLeaseTurn_=false;
    }
    void Bind(uint64_t session,coop::Endpoint peer,const coop_world::Identity& identity) {
        if(session_==session&&peer_==peer&&identity_==identity)return;
        const auto stats=stats_;Reset();stats_=stats;session_=session;peer_=peer;identity_=identity;
    }
    bool Matches(uint64_t session,coop::Endpoint peer)const{return session_==session&&peer_==peer&&session;}
    size_t ControlSpace()const{return kControlLimit-controlCount_;}
    size_t ControlCount()const{return controlCount_;}
    const Statistics& Stats()const{return stats_;}
    bool Push(const void* data,size_t size,uint32_t now) {
        if(!session_||!data||size<4||size>sizeof(Datagram::bytes))return Reject();
        uint32_t magic=0;std::memcpy(&magic,data,4);magic=coop_world_wire::LE32(magic);
        if(magic==coop_fleet_load::kMagic) {
            coop_fleet_load::Message message;
            if(!coop_fleet_load::Decode(data,size,message)||message.identity.sessionId!=session_||
                message.identity.trackId!=identity_.trackId||message.identity.raceEpoch!=identity_.raceEpoch||
                (message.type==coop_fleet_load::Type::Offer&&message.identity!=identity_))return Reject();
            const bool changed=!presence_.used||message.nonce!=presenceMessage_.nonce||message.type!=presenceMessage_.type||
                message.flowState!=presenceMessage_.flowState||message.freeRoam!=presenceMessage_.freeRoam||
                message.trafficAllowed!=presenceMessage_.trafficAllowed;
            presenceMessage_=message;Store(presence_,data,size,now,changed);return true;
        }
        if(magic==coop_world_wire::kEventMagic) {
            coop_world::Event event;uint32_t sender=0;
            if(!coop_world_wire::DecodeEvent(data,size,event,sender,1)||event.identity!=identity_||
                event.state.kind==coop_world::Kind::RaceRacer)return Reject();
            Entity* slot=Find(entities_,event.state.key.id);
            if(!slot)return Capacity();
            if(slot->used) {
                const auto& old=slot->event;
                if(event.state.key.generation!=old.state.key.generation) {
                    if(!coop_world::Newer(event.state.key.generation,old.state.key.generation))return Reject();
                } else if(event.sequence!=old.sequence&&!coop_world::Newer(event.sequence,old.sequence))return Reject();
                else if(old.action==coop_world::Action::Remove&&event.action!=coop_world::Action::Remove)return Reject();
            }
            const bool changed=!slot->used||event.state.key!=slot->event.state.key||
                !(event.state.descriptor==slot->event.state.descriptor)||
                (event.action==coop_world::Action::Remove)!=(slot->event.action==coop_world::Action::Remove)||
                event.state.motion.aiFrozen!=slot->event.state.motion.aiFrozen;
            slot->event=event;Store(*slot,data,size,now,changed);
            for(auto& lease:leases_)if(lease.used&&lease.state.state.key.id==event.state.key.id&&
                (lease.state.state.key!=event.state.key||event.action==coop_world::Action::Remove))lease=Lease{};
            return true;
        }
        if(magic==coop_world_wire::kLeaseMagic) {
            coop_world_wire::LeaseEnvelope envelope;
            if(!coop_world_wire::DecodeLease(data,size,envelope,1)||envelope.state.identity!=identity_)return Reject();
            if(envelope.kind!=coop_world_wire::LeaseMessageKind::AuthorityState||
                envelope.decision!=coop_world_wire::LeaseDecision::State)return QueueControl(data,size,now,envelope);
            Lease* slot=Find(leases_,envelope.state.key.id);if(!slot)return Capacity();
            if(slot->used) {
                const auto& old=slot->state.state;
                if(envelope.state.key.generation!=old.key.generation) {
                    if(!coop_world::Newer(envelope.state.key.generation,old.key.generation))return Reject();
                } else if(envelope.state.sequence!=old.sequence&&!coop_world::Newer(envelope.state.sequence,old.sequence))return Reject();
            }
            // Remaining duration counts down every source frame. Only a new
            // lease serial/owner/claim is a state change needing immediate TX.
            const bool changed=!slot->used||envelope.state.key!=slot->state.state.key||
                envelope.state.sequence!=slot->state.state.sequence||envelope.state.ownerPlayerId!=slot->state.state.ownerPlayerId||
                envelope.state.requestId!=slot->state.state.requestId;
            slot->state=envelope;Store(*slot,data,size,now,changed);return true;
        }
        return Reject();
    }
    size_t PopDue(Datagram* output,size_t capacity,uint32_t now) {
        if(!output||!capacity)return 0;capacity=std::min(capacity,kBurstLimit);size_t count=0;
        // User challenge transactions precede periodic world snapshots. The
        // producer reserves enough FIFO capacity before draining Runtime.
        while(controlCount_&&count<capacity) {
            const auto next=controls_[controlHead_];controlHead_=(controlHead_+1)%kControlLimit;--controlCount_;
            if(coop_world::SerialDelta(now,next.produced)>2000){++stats_.expired;continue;}
            Emit(next.wire,output[count++]);
        }
        if(count<capacity&&Due(presence_,now,kPresenceMs))Take(presence_,output[count++],now);
        DrainEntities(output,capacity,count,now,true);
        DrainLeases(output,capacity,count,now,true);
        // Alternate the first normal slot even with a one-datagram consumer;
        // then reserve at most one lease slot before the remaining motion.
        if(count<capacity) {
            if(normalLeaseTurn_)DrainLeases(output,std::min(capacity,count+1),count,now,false);
            else DrainEntities(output,std::min(capacity,count+1),count,now,false);
            normalLeaseTurn_=!normalLeaseTurn_;
            DrainLeases(output,std::min(capacity,count+1),count,now,false);
            DrainEntities(output,capacity,count,now,false);
        }
        return count;
    }
private:
    uint64_t session_=0;coop::Endpoint peer_{};coop_world::Identity identity_{};
    std::array<Entity,kEntityLimit> entities_{};std::array<Lease,kEntityLimit> leases_{};
    std::array<Control,kControlLimit> controls_{};size_t controlHead_=0,controlCount_=0,entityCursor_=0,leaseCursor_=0;
    Value presence_{};coop_fleet_load::Message presenceMessage_{};Statistics stats_{};
    bool normalLeaseTurn_=false;
    bool Reject(){++stats_.rejected;return false;}bool Capacity(){++stats_.capacityRefused;return false;}
    template<class T,size_t N>static T* Find(std::array<T,N>& rows,uint64_t id) {
        T* empty=nullptr;for(auto& row:rows) {
            if(!row.used){if(!empty)empty=&row;continue;}
            if(Id(row)==id)return &row;
        }return empty;
    }
    static uint64_t Id(const Entity& row){return row.event.state.key.id;}
    static uint64_t Id(const Lease& row){return row.state.state.key.id;}
    void Store(Value& value,const void* data,size_t size,uint32_t now,bool critical) {
        ++stats_.published;if(value.used)++stats_.coalesced;
        value.wire.size=uint16_t(size);std::memcpy(value.wire.bytes,data,size);
        value.used=true;value.produced=now;value.critical=value.critical||critical;
    }
    bool QueueControl(const void* data,size_t size,uint32_t now,const coop_world_wire::LeaseEnvelope& envelope) {
        // Exact repeated transaction retries refresh the existing FIFO entry;
        // different claim/release event IDs remain separate ordered messages.
        for(size_t i=0;i<controlCount_;++i) {
            auto& item=controls_[(controlHead_+i)%kControlLimit];coop_world_wire::LeaseEnvelope old;
            if(coop_world_wire::DecodeLease(item.wire.bytes,item.wire.size,old,1)&&old.kind==envelope.kind&&
                old.eventId==envelope.eventId&&old.state.key==envelope.state.key) {
                item.wire.size=uint16_t(size);std::memcpy(item.wire.bytes,data,size);item.produced=now;++stats_.coalesced;return true;
            }
        }
        if(!ControlSpace())return Capacity();auto& item=controls_[(controlHead_+controlCount_)%kControlLimit];
        item.wire.size=uint16_t(size);std::memcpy(item.wire.bytes,data,size);item.produced=now;++controlCount_;++stats_.published;
        stats_.maxControls=std::max(stats_.maxControls,uint32_t(controlCount_));return true;
    }
    bool Due(Value& value,uint32_t now,uint32_t interval) {
        if(!value.used)return false;
        if(coop_world::SerialDelta(now,value.produced)>int32_t(kFreshMs)) {
            // Keep ordering high-water marks but never replay an old pose.
            if(value.critical){value.critical=false;++stats_.expired;}return false;
        }
        return value.critical||!value.haveSent||coop_world::SerialDelta(now,value.sent)>=int32_t(interval);
    }
    void Emit(const Datagram& wire,Datagram& output){output=wire;++stats_.datagrams;stats_.bytes+=wire.size;}
    void Take(Value& value,Datagram& output,uint32_t now){Emit(value.wire,output);value.sent=now;value.haveSent=true;value.critical=false;}
    void DrainEntities(Datagram* output,size_t capacity,size_t& count,uint32_t now,bool critical) {
        const size_t start=entityCursor_;
        for(size_t n=0;n<entities_.size()&&count<capacity;++n) {
            const size_t i=(start+n)%entities_.size();auto& row=entities_[i];
            const uint32_t interval=row.event.action==coop_world::Action::Remove?kRemoveMs:row.event.state.motion.aiFrozen?kDormantMs:kActiveMs;
            if(row.critical!=critical||!Due(row,now,interval))continue;
            Take(row,output[count++],now);entityCursor_=(i+1)%entities_.size();
        }
    }
    void DrainLeases(Datagram* output,size_t capacity,size_t& count,uint32_t now,bool critical) {
        const size_t start=leaseCursor_;
        for(size_t n=0;n<leases_.size()&&count<capacity;++n) {
            const size_t i=(start+n)%leases_.size();auto& row=leases_[i];
            if(row.critical!=critical||!Due(row,now,kLeaseMs))continue;
            Take(row,output[count++],now);leaseCursor_=(i+1)%leases_.size();
        }
    }
};
} // namespace coop_fleet_net
