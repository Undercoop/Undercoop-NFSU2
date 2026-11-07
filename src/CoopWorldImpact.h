// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopWorldWire.h"
#include "CoopCarCatalog.h"
#include <array>

// Native-contact values only. Never transfer Car/Driver/Witness/body pointers.
namespace coop_world_impact {
constexpr std::uint32_t kMagic = 0x4932434eU; // NC2I
constexpr std::uint16_t kVersion = 2;
constexpr std::int32_t kAuthorityAgeLimitMs = 1500;
enum class Type : std::uint8_t { Request = 1, Ack = 2 };
struct Event {
    coop_world::Identity identity{};
    coop_world::EntityKey key{};
    // observedAt is the timestamp of the Host snapshot that the Client saw,
    // never the Client's uptime. Both it and authorityAt are Host-clock values.
    std::uint32_t sequence = 0, model = 0, authorityAt = 0, observedAt = 0;
    std::uint64_t appearance = 0;
    Type type = Type::Request;
    std::uint8_t accepted = 0;
    float mass = 0, position[3]{}, humanPosition[3]{}, lever[3]{}, impulse[3]{}, angularImpulse[3]{}, deltaVelocity[3]{}, deltaAngular[3]{};
};
inline bool Vector(const float* v, float limit) {
    for (unsigned i=0;i<3;++i) if (!std::isfinite(v[i]) || std::fabs(v[i])>limit) return false;
    return true;
}
inline bool SameContact(const Event& a,const Event& b) {
    return a.identity==b.identity && a.key==b.key && a.sequence==b.sequence &&
        a.model==b.model && a.appearance==b.appearance && a.observedAt==b.observedAt;
}
inline bool AuthoritySampleFresh(std::uint32_t now,std::uint32_t observedAt) {
    const auto age=coop_world::SerialDelta(now,observedAt);
    return age>=0 && age<=kAuthorityAgeLimitMs;
}
inline bool Valid(const Event& e) {
    if (!e.identity.Valid() || !e.key.Valid() || !e.sequence || !coop_car_catalog::PortableModelIndex(e.model) || !e.appearance ||
        (e.type!=Type::Request && e.type!=Type::Ack) || e.accepted>1) return false;
    if (e.type==Type::Ack) return e.accepted ? e.authorityAt!=0 && AuthoritySampleFresh(e.authorityAt,e.observedAt) : e.authorityAt==0;
    if (e.accepted || e.authorityAt || !std::isfinite(e.mass) || e.mass<=0 || e.mass>1000000 ||
        !Vector(e.position,1000000) || !Vector(e.humanPosition,1000000) || !Vector(e.lever,8) ||
        !Vector(e.impulse,e.mass*160) || !Vector(e.angularImpulse,e.mass*1280) || !Vector(e.deltaVelocity,160) || !Vector(e.deltaAngular,100)) return false;
    float strength=0, separation=0;
    for(unsigned i=0;i<3;++i) {
        strength+=e.deltaVelocity[i]*e.deltaVelocity[i]+e.deltaAngular[i]*e.deltaAngular[i];
        const float d=e.position[i]-e.humanPosition[i]; separation+=d*d;
    }
    return strength>1.0e-6f && separation<=20.0f*20.0f;
}
#pragma pack(push,1)
struct Packet {
    coop_world_wire::Header header;
    coop_world_wire::IdentityFields identity;
    coop_world_wire::KeyFields key;
    std::uint32_t sequence, model, authorityAt, observedAt;
    std::uint64_t appearance;
    std::uint8_t accepted, reserved[3];
    std::uint32_t mass, position[3], humanPosition[3], lever[3], impulse[3], angularImpulse[3], deltaVelocity[3], deltaAngular[3];
};
#pragma pack(pop)
static_assert(sizeof(Packet)<=256,"Impact datagrams must stay small");
inline bool Encode(const Event& e,Packet& p) {
    using namespace coop_world_wire;
    if(!Valid(e)) return false;
    p={}; p.header={LE32(kMagic),LE16(kVersion),LE16(sizeof(p)),static_cast<std::uint8_t>(e.type),
        static_cast<std::uint8_t>(e.type==Type::Request?2:1),1,0};
    p.identity={LE64(e.identity.sessionId),LE32(e.identity.trackId),LE32(e.identity.worldEpoch),LE32(e.identity.raceEpoch)};
    p.key={LE64(e.key.id),LE32(e.key.generation)};
    p.sequence=LE32(e.sequence);p.model=LE32(e.model);p.appearance=LE64(e.appearance);p.authorityAt=LE32(e.authorityAt);p.observedAt=LE32(e.observedAt);
    p.accepted=e.accepted;p.mass=FloatBits(e.mass);
    for(unsigned i=0;i<3;++i) {
        p.position[i]=FloatBits(e.position[i]);p.humanPosition[i]=FloatBits(e.humanPosition[i]);p.lever[i]=FloatBits(e.lever[i]);
        p.impulse[i]=FloatBits(e.impulse[i]);p.angularImpulse[i]=FloatBits(e.angularImpulse[i]);p.deltaVelocity[i]=FloatBits(e.deltaVelocity[i]);p.deltaAngular[i]=FloatBits(e.deltaAngular[i]);
    }
    return true;
}
inline bool Decode(const void* bytes,size_t size,Event& e) {
    using namespace coop_world_wire;
    if(!bytes || size!=sizeof(Packet)) return false;
    Packet p{};std::memcpy(&p,bytes,sizeof(p));
    if(LE32(p.header.magic)!=kMagic || LE16(p.header.version)!=kVersion || LE16(p.header.size)!=sizeof(p) ||
        p.header.hostPlayerId!=1 || p.header.reserved || p.reserved[0] || p.reserved[1] || p.reserved[2] ||
        (p.header.kind!=1 && p.header.kind!=2) || p.header.senderPlayerId!=(p.header.kind==1?2:1)) return false;
    Event result{};result.type=static_cast<Type>(p.header.kind);result.accepted=p.accepted;
    result.identity={LE64(p.identity.sessionId),LE32(p.identity.trackId),LE32(p.identity.worldEpoch),LE32(p.identity.raceEpoch)};
    result.key={LE64(p.key.id),LE32(p.key.generation)};
    result.sequence=LE32(p.sequence);result.model=LE32(p.model);result.appearance=LE64(p.appearance);result.authorityAt=LE32(p.authorityAt);result.observedAt=LE32(p.observedAt);
    result.mass=BitsFloat(p.mass);
    for(unsigned i=0;i<3;++i) {
        result.position[i]=BitsFloat(p.position[i]);result.humanPosition[i]=BitsFloat(p.humanPosition[i]);result.lever[i]=BitsFloat(p.lever[i]);
        result.impulse[i]=BitsFloat(p.impulse[i]);result.angularImpulse[i]=BitsFloat(p.angularImpulse[i]);result.deltaVelocity[i]=BitsFloat(p.deltaVelocity[i]);result.deltaAngular[i]=BitsFloat(p.deltaAngular[i]);
    }
    if(!Valid(result)) return false;e=result;return true;
}
// A small selective-repeat window allows reordering but never double-applies
// a retransmitted contact. Scope changes reset it on the game thread.
class Replay {
    std::uint32_t high_=0; std::uint64_t bits_=0;
public:
    void Reset() { high_=0;bits_=0; }
    bool Seen(std::uint32_t sequence) const {
        if(!sequence) return true;
        if(!high_ || coop_world::Newer(sequence,high_)) return false;
        const auto age=static_cast<std::uint32_t>(high_-sequence);
        return age>=64 || (bits_ & (std::uint64_t(1)<<age))!=0;
    }
    void Commit(std::uint32_t sequence) {
        if(!high_) {high_=sequence;bits_=1;return;}
        if(coop_world::Newer(sequence,high_)) {
            const auto shift=static_cast<std::uint32_t>(sequence-high_);
            bits_=shift>=64?1:(bits_<<shift)|1;high_=sequence;
        } else {
            const auto age=static_cast<std::uint32_t>(high_-sequence);
            if(age<64) bits_|=std::uint64_t(1)<<age;
        }
    }
};
inline void Multiply(const float* a,const float* b,float* out) {
    const float q[]{a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
        a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
        a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
    std::memcpy(out,q,sizeof(q));coop_motion::NormalizeQuaternion(out);
}
struct Prediction {
    bool active=false, acked=false, recovering=false, haveRendered=false, native=false;
    coop_world::Identity identity{};coop_world::EntityKey key{};
    std::uint32_t sequence=0,start=0,last=0,authorityAt=0,recoveryStart=0,model=0,observedAt=0;
    std::uint64_t appearance=0;
    float offset[3]{}, velocity[3]{}, angular[3]{}, rotation[4]{0,0,0,1};
    float renderedPosition[3]{}, renderedVelocity[3]{}, renderedAngular[3]{}, renderedRotation[4]{0,0,0,1};
    float Weight(std::uint32_t now) const {
        if(!recovering) return 1;
        const float t=std::clamp(coop_world::SerialDelta(now,recoveryStart)/200.0f,0.0f,1.0f);
        return 1-t*t*(3-2*t);
    }
    void Step(std::uint32_t now) {
        const float dt=std::clamp(coop_world::SerialDelta(now,last)*0.001f,0.0f,0.25f);last=now;
        if(recovering) return;
        for(unsigned i=0;i<3;++i) offset[i]=std::clamp(offset[i]+velocity[i]*dt,-40.0f,40.0f);
        const float speed=std::sqrt(angular[0]*angular[0]+angular[1]*angular[1]+angular[2]*angular[2]);
        if(speed>1.0e-6f) {
            const float half=speed*dt*0.5f,scale=std::sin(half)/speed;
            const float q[]{angular[0]*scale,angular[1]*scale,angular[2]*scale,std::cos(half)};
            Multiply(q,rotation,rotation);
        }
    }
    void Add(const Event& e,std::uint32_t now) {
        if(!active || identity!=e.identity || key!=e.key) { *this={};identity=e.identity;key=e.key;last=now; }
        Step(now);
        // A new contact can arrive while the old one is settling. Retain only
        // the correction currently being displayed, not its original full kick.
        if(recovering) {
            const float weight=Weight(now),neutral[4]{0,0,0,1};float faded[4]{};
            for(unsigned i=0;i<3;++i) {offset[i]*=weight;velocity[i]*=weight;angular[i]*=weight;}
            coop_motion::Slerp(neutral,rotation,weight,faded);std::memcpy(rotation,faded,sizeof(faded));
        }
        active=true;acked=recovering=false;start=now;sequence=e.sequence;
        model=e.model;appearance=e.appearance;observedAt=e.observedAt;
        for(unsigned i=0;i<3;++i) {velocity[i]=std::clamp(velocity[i]+e.deltaVelocity[i],-160.0f,160.0f);
            angular[i]=std::clamp(angular[i]+e.deltaAngular[i],-100.0f,100.0f);}
    }
    void Ack(const Event& e) {
        if(active && e.type==Type::Ack && e.sequence==sequence && e.identity==identity && e.key==key &&
            e.model==model && e.appearance==appearance && e.observedAt==observedAt) {
            acked=true;authorityAt=e.accepted?e.authorityAt:0;
        }
    }
    // A verified contact body has already received the stock solver impulse.
    // While it runs stock substeps, use its complete physical state; adding the
    // wire kick a second time (or writing a pre-impact Host sample) is incorrect.
    void SeedNative(const float* p,const float* q,const float* v,const float* a) {
        native=true;haveRendered=true;
        std::memcpy(renderedPosition,p,12);std::memcpy(renderedRotation,q,16);
        std::memcpy(renderedVelocity,v,12);std::memcpy(renderedAngular,a,12);
    }
    void ApplyNative(std::uint32_t now,std::uint32_t snapshotAt,float* p,float* q,float* v,float* a,
        const float* physicalPosition,const float* physicalRotation,const float* physicalVelocity,const float* physicalAngular) {
        if(!active || !native) return;
        last=now;
        if(!recovering && ((acked && (!authorityAt || coop_world::SerialDelta(snapshotAt,authorityAt)>=0)) ||
            coop_world::SerialDelta(now,start)>700)) {recovering=true;recoveryStart=now;}
        if(recovering && coop_world::SerialDelta(now,recoveryStart)>=200) {*this={};return;}
        const float weight=Weight(now);
        for(unsigned i=0;i<3;++i) {
            p[i]+=(physicalPosition[i]-p[i])*weight;
            v[i]+=(physicalVelocity[i]-v[i])*weight;
            a[i]+=(physicalAngular[i]-a[i])*weight;
        }
        float result[4]{};coop_motion::Slerp(q,physicalRotation,weight,result);std::memcpy(q,result,16);
        std::memcpy(renderedPosition,p,12);std::memcpy(renderedRotation,q,16);
        std::memcpy(renderedVelocity,v,12);std::memcpy(renderedAngular,a,12);haveRendered=true;
    }
    void Apply(std::uint32_t now,std::uint32_t snapshotAt,float* p,float* q,float* v,float* a) {
        if(!active) return;
        const float dt=std::clamp(coop_world::SerialDelta(now,last)*0.001f,0.0f,0.25f);
        Step(now);
        if(!recovering && ((acked && (!authorityAt || coop_world::SerialDelta(snapshotAt,authorityAt)>=0)) ||
            coop_world::SerialDelta(now,start)>700)) {
            // The new Host sample already contains the accepted impact. Adding
            // the full kick again moves the replica twice, then pulls it back.
            // Rebase to the last displayed trajectory and fade only its error.
            if(haveRendered) {
                for(unsigned i=0;i<3;++i) {
                    offset[i]=std::clamp(renderedPosition[i]+renderedVelocity[i]*dt-p[i],-40.0f,40.0f);
                    velocity[i]=std::clamp(renderedVelocity[i]-v[i],-160.0f,160.0f);
                    angular[i]=std::clamp(renderedAngular[i]-a[i],-100.0f,100.0f);
                }
                float desired[4]{};std::memcpy(desired,renderedRotation,sizeof(desired));
                const float speed=std::sqrt(renderedAngular[0]*renderedAngular[0]+renderedAngular[1]*renderedAngular[1]+renderedAngular[2]*renderedAngular[2]);
                if(speed>1.0e-6f) {
                    const float half=speed*dt*0.5f,scale=std::sin(half)/speed;
                    const float delta[]{renderedAngular[0]*scale,renderedAngular[1]*scale,renderedAngular[2]*scale,std::cos(half)};
                    Multiply(delta,desired,desired);
                }
                const float inverse[]{-q[0],-q[1],-q[2],q[3]};Multiply(desired,inverse,rotation);
            }
            recovering=true;recoveryStart=now;
        }
        const float weight=Weight(now);
        if(recovering && coop_world::SerialDelta(now,recoveryStart)>=200) {*this={};return;}
        for(unsigned i=0;i<3;++i) {p[i]+=offset[i]*weight;v[i]+=velocity[i]*weight;a[i]+=angular[i]*weight;}
        // The actual authoritative orientation continues to come from Host.
        float faded[4]{},result[4]{};
        const float neutral[4]{0,0,0,1};
        coop_motion::Slerp(neutral,rotation,weight,faded);
        Multiply(faded,q,result);std::memcpy(q,result,sizeof(result));
        std::memcpy(renderedPosition,p,12);std::memcpy(renderedRotation,q,16);
        std::memcpy(renderedVelocity,v,12);std::memcpy(renderedAngular,a,12);haveRendered=true;
    }
};
}
