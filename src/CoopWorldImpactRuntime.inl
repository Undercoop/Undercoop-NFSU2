// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include "CoopWorldImpact.h"
#include "CoopWorldImpactContactNative.inl"
namespace CoopWorldImpactRuntime {
using Event=coop_world_impact::Event;
using Packet=coop_world_impact::Packet;
struct Binding {uintptr_t car=0;coop_world::Identity identity{};coop_world::EntityState state{};uint32_t received=0;};
struct Queued {Event event{};coop::Endpoint peer{};uint32_t received=0;};
struct Outgoing {Event event{};uint32_t start=0,last=0;};
struct Acknowledged {Event event{};};
static SRWLOCK lock=SRWLOCK_INIT;
static std::array<Queued,64> inbox{};static size_t inboxCount=0;
static std::array<Outgoing,64> outgoing{};static size_t outgoingCount=0;
static std::array<Acknowledged,64> acknowledgements{};static size_t ackCursor=0;
static std::array<coop_world_impact::Prediction,22> predictions{};
static std::array<uintptr_t,22> predictedBodies{};
static coop_world_impact::Replay replay;
static uint64_t sessionId=0;static coop::Endpoint peer{};static coop::Role role=coop::Role::Idle;
static uint32_t sequence=0,accepted=0,rejected=0,reported=0,duplicate=0,lastTrace=0,reportRefused=0,ackAccepted=0,ackRejected=0,nativeTouches=0;
static std::atomic<uint32_t> dropped{0},expiredRequests{0};
static uint32_t staleAuthority=0;
enum class BindRefusal : size_t { Session, RaceScope, NoLatest, Fleet, Descriptor, Stale, Identity, Frozen, Visible, Track, Count };
static std::array<uint32_t,static_cast<size_t>(BindRefusal::Count)> bindRefused{};

static bool Session(const coop::SessionSnapshot& s) {
    return s.joined && s.fresh && s.sessionId && s.peer.Valid() &&
        (s.role==coop::Role::Host || s.role==coop::Role::Client) && IsCoopGameplayActive();
}
static bool Resolve(uintptr_t car,Binding& result,bool diagnose=false) {
    const auto s=CoopSnapshot();const auto now=CoopAIWorldRuntime::MotionNow();
    const auto fail=[diagnose](BindRefusal stage) {if(diagnose) ++bindRefused[static_cast<size_t>(stage)];return false;};
    if(!car || car==CoopRaceCar(false) || car==CoopRaceCar(true)) return false;
    if(!Session(s)) return fail(BindRefusal::Session);
    Binding b{};b.car=car;
    if(auto* race=CoopAIWorldRuntime::Find(car)) {
        if(CoopAIWorldRuntime::ownerRole!=s.role || CoopAIWorldRuntime::identity.sessionId!=s.sessionId ||
            !CoopAIWorldRuntime::Matches(*race)) return fail(BindRefusal::RaceScope);
        b.identity=CoopAIWorldRuntime::identity;
        if(s.role==coop::Role::Client) {
            if(!race->haveLatest) return fail(BindRefusal::NoLatest);
            if(coop_world::SerialDelta(now,race->lastReceive)>1000) return fail(BindRefusal::Stale);
            b.state=race->latest;b.received=race->lastReceive;
        } else {
            b.state.key={0x100000000ULL+race->ordinal,race->generation};
            b.state.kind=coop_world::Kind::RaceRacer;b.state.descriptor=race->descriptor;
        }
    } else {
        uintptr_t ride=0;uint32_t kind=0,model=0;uint64_t appearance=0;
        if(!CoopFleetWorldRuntime::ready || !CoopFleetWorldRuntime::NativeIdentity(car,ride,kind,nullptr,&model,&appearance) ||
            !CoopFleetWorldRuntime::runtime.NativeState(car,b.state,b.received)) return fail(BindRefusal::Fleet);
        if(b.state.descriptor.modelId!=model || b.state.descriptor.appearanceHash!=appearance) return fail(BindRefusal::Descriptor);
        if(s.role==coop::Role::Client && coop_world::SerialDelta(now,b.received)>1000) return fail(BindRefusal::Stale);
        b.identity=CoopFleetWorldRuntime::loading.Identity();
    }
    uint8_t visible=0;uint32_t track=0;
    if(!b.identity.Valid() || b.identity.sessionId!=s.sessionId || !b.state.key.Valid()) return fail(BindRefusal::Identity);
    if(b.state.motion.aiFrozen) return fail(BindRefusal::Frozen);
    if(!ReadGameMemory(car+0x550,&visible,1) || visible!=1) return fail(BindRefusal::Visible);
    if(!ReadGameMemory(gRaceContextAddresses.track,&track,4) || track!=b.identity.trackId) return fail(BindRefusal::Track);
    result=b;return true;
}
static bool Authorize(uintptr_t car,uintptr_t human,bool& suppress) {
    Binding b{};const auto s=CoopSnapshot();
    if(s.role==coop::Role::Client?human!=CoopRaceCar(false):s.role==coop::Role::Host?human!=CoopRaceCar(true):true) return false;
    if(!Resolve(car,b,true)) return false;
    if(s.role==coop::Role::Client) {
        uintptr_t ride=0;uint32_t kind=0;
        suppress=false;return human==CoopRaceCar(false) && ReadManagedCoopAI(car,ride,kind);
    }
    // The owning human solver sends this precise contact. Applying the Host
    // remote-human proxy impulse too would give the AI a double collision.
    suppress=human==CoopRaceCar(true);return suppress;
}
static void Publish(const Event& e,uint32_t now) {
    AcquireSRWLockExclusive(&lock);
    if(outgoingCount<outgoing.size()) outgoing[outgoingCount++]={e,now,0};else ++dropped;
    ReleaseSRWLockExclusive(&lock);
}
static coop_world_impact::Prediction* Predict(const coop_world::Identity& id,coop_world::EntityKey key,bool create) {
    for(auto& p:predictions) if(p.active && p.identity==id && p.key==key) return &p;
    if(create) for(auto& p:predictions) if(!p.active) return &p;
    return nullptr;
}
static bool PhysicalState(const CoopWorldImpactContactNative::Body& body,float* p,float* q,float* v,float* a) {
    float matrix[16]{};
    return ReadGameMemory(body.body+0x20,p,12) && CoopPhysicalMotionNative::FiniteVector(p,1000000) &&
        ReadGameMemory(body.body+0x30,matrix,64) && coop_motion::MatrixToQuaternion(matrix,q) &&
        ReadGameMemory(body.body+0xA0,v,12) && CoopPhysicalMotionNative::FiniteVector(v,500) &&
        ReadGameMemory(body.body+0xC0,a,12) && CoopPhysicalMotionNative::FiniteVector(a,100);
}
static bool NativeIntegration(uintptr_t car) {
    if(CoopSnapshot().role!=coop::Role::Client) return false;
    Binding b{};if(!Resolve(car,b)) return false;
    auto* p=Predict(b.identity,b.state.key,false);if(!p || !p->native) return false;
    const auto age=coop_world::SerialDelta(CoopAIWorldRuntime::MotionNow(),p->start);
    CoopWorldImpactContactNative::Body body{};
    return age>=0 && age<=1000 && CoopWorldImpactContactNative::ReadBody(car,body) &&
        body.body==predictedBodies[static_cast<size_t>(p-predictions.data())];
}
static void Touch(uintptr_t car,uintptr_t human) {
    const auto s=CoopSnapshot();if(s.role!=coop::Role::Client || human!=CoopRaceCar(false)) return;
    Binding b{};CoopWorldImpactContactNative::Body body{};
    float position[3]{},rotation[4]{},velocity[3]{},angular[3]{};
    if(!Resolve(car,b) || !CoopWorldImpactContactNative::ReadBody(car,body) ||
        !PhysicalState(body,position,rotation,velocity,angular)) return;
    if(auto* p=Predict(b.identity,b.state.key,true)) {
        const auto slot=static_cast<size_t>(p-predictions.data());
        if(!p->active || (p->native && predictedBodies[slot]!=body.body)) {*p={};p->identity=b.identity;p->key=b.state.key;}
        // A verified native pair may separate bodies without producing an
        // explicit impulse. Do not invent an impulse or send a position to Host.
        // Keep stock local physical response until contact stops, then reconcile.
        // Retain an already matched impulse acknowledgement. Repeated genuine
        // contact postpones the blend, not the acknowledgement; when separation
        // ends, the accepted Host timeline can settle without a 700ms retry wait.
        p->active=true;p->recovering=false;
        p->start=p->last=CoopAIWorldRuntime::MotionNow();p->SeedNative(position,rotation,velocity,angular);
        predictedBodies[slot]=body.body;++nativeTouches;
    }
}
static void Report(const CoopWorldImpactContactNative::Contact& c) {
    Binding b{};if(CoopSnapshot().role!=coop::Role::Client || !Resolve(c.car,b)) {++reportRefused;return;}
    Event e{};e.identity=b.identity;e.key=b.state.key;e.model=b.state.descriptor.modelId;e.appearance=b.state.descriptor.appearanceHash;
    e.sequence=++sequence;if(!e.sequence) e.sequence=++sequence;e.mass=c.mass;e.observedAt=b.state.motion.sentAtMs;
    std::memcpy(e.position,c.position,12);std::memcpy(e.humanPosition,c.humanPosition,12);std::memcpy(e.lever,c.lever,12);
    std::memcpy(e.impulse,c.impulse,12);std::memcpy(e.angularImpulse,c.angularImpulse,12);
    std::memcpy(e.deltaVelocity,c.velocity,12);std::memcpy(e.deltaAngular,c.angular,12);
    if(!coop_world_impact::Valid(e)) {++reportRefused;return;}
    const auto now=CoopAIWorldRuntime::MotionNow();
    if(auto* p=Predict(e.identity,e.key,true)) {
        p->Add(e,now);
        CoopWorldImpactContactNative::Body body{};float position[3]{},rotation[4]{},velocity[3]{},angular[3]{};
        if(CoopWorldImpactContactNative::ReadBody(c.car,body) && PhysicalState(body,position,rotation,velocity,angular)) {
            // Witness::Impulse runs before the native pair's velocity-cache
            // refresh. This is only a seed, never a body write: the next game
            // thread Apply/stock substep reads the actual post-pair body state.
            p->SeedNative(position,rotation,velocity,angular);
            predictedBodies[static_cast<size_t>(p-predictions.data())]=body.body;
        }
    }
    Publish(e,now);++reported;
}
static uintptr_t FindTarget(const Event& e) {
    uintptr_t cars[kMaxVehicleSlots]{};if(!ReadGameMemory(kCarArray,cars,sizeof(cars))) return 0;
    for(const auto car:cars) {Binding b{};if(Resolve(car,b) && b.identity==e.identity && b.state.key==e.key &&
        b.state.descriptor.modelId==e.model && b.state.descriptor.appearanceHash==e.appearance) return car;}
    return 0;
}
// Only real, fresh local-human contacts may reach this Host writer. Preserve
// native momentum and angular momentum; never set AI coordinates from a client.
static bool SameMass(float native,float reported) {
    // Stock vehicle mass is tonne-scale (e.g. 1.22). A minimum tolerance of
    // 1.0 would admit differences of an entire tonne between the two bodies.
    return std::isfinite(native) && std::isfinite(reported) && native>0 && reported>0 &&
        std::fabs(native-reported)<=std::max(0.0001f,native*0.01f);
}
static bool (*writeMomentumCache)(uintptr_t,const void*,size_t)=&SafeWriteBytes;
static bool CommitMomentum(uintptr_t car,const CoopWorldImpactContactNative::Body& body,
    const float* momentum,const float* spin,const float* velocity,const float* angular) {
    struct Write {uintptr_t address;const float* next;float prior[3]{};};
    Write fields[]{{body.body+0x70,momentum},{body.body+0x80,spin},
        {body.body+0xA0,velocity},{body.body+0xC0,angular},{car+0x70,velocity},{car+0x110,angular}};
    // Validate and snapshot every cache before changing any. A rejected write
    // must not leave half an impulse installed, then answer "not applied".
    for(auto& field:fields) if(!CoopPhysicalMotionNative::WritableRange(field.address,12) ||
        !ReadGameMemory(field.address,field.prior,12)) return false;
    if(!writeMomentumCache) return false;
    for(size_t i=0;i<std::size(fields);++i) if(!writeMomentumCache(fields[i].address,fields[i].next,12)) {
        bool restored=true;
        for(size_t j=i+1;j>0;--j) restored=writeMomentumCache(fields[j-1].address,fields[j-1].prior,12)&&restored;
        if(!restored) LogMessage("[AI_IMPACT] Native cache rollback failed; contact was not acknowledged as applied.\r\n");
        return false;
    }
    return true;
}
static bool ApplyHost(const Event& e,uint32_t received,uint32_t now) {
    using namespace CoopWorldImpactContactNative;
    const auto s=CoopSnapshot();if(s.role!=coop::Role::Host || !Session(s) ||
        coop_world::SerialDelta(now,received)>250 || coop_world::SerialDelta(now,received)<0) return false;
    if(!coop_world_impact::AuthoritySampleFresh(now,e.observedAt)) {++staleAuthority;return false;}
    const uintptr_t car=FindTarget(e);if(!car) return false;
    RemotePlayerState remote{};
    if(!GetRemotePlayerState(remote,false) || remote.current.flowState!=6 || remote.current.sessionId!=s.sessionId ||
        remote.current.trackId!=e.identity.trackId) return false;
    float position[3]{};if(!ReadGameMemory(car+0x60,position,12)) return false;
    float targetDifference=0,humanDifference=0;
    const float human[3]{remote.current.x,remote.current.y,remote.current.z};
    for(unsigned i=0;i<3;++i) {const float t=position[i]-e.position[i],h=human[i]-e.humanPosition[i];
        targetDifference+=t*t;humanDifference+=h*h;}
    if(targetDifference>80*80 || humanDifference>80*80) return false;
    Body body{};
    if(!ReadBody(car,body)) {
        // Native TrafficMover has no body. A verified current traffic pool
        // gains the same native Mellow body as an ordinary stock car impact.
        uintptr_t ride=0;uint32_t kind=0,mode=0,wrapper=0,vt=0,owner=0,geometry=0,base=0;
        uint8_t terrain=0;float mass=0;
        if(!CoopFleetWorldRuntime::NativeIdentity(car,ride,kind) || kind!=3 ||
            !ReadGameMemory(car+0x28,&mode,4) || mode!=2 || !ReadGameMemory(car+0x34,&wrapper,4) ||
            !ReadGameMemory(wrapper,&vt,4) || vt!=CoopWorldPhysicalMotionNative::kTrafficMoverVtable ||
            !ReadGameMemory(wrapper+8,&owner,4) || owner!=car ||
            !ReadGameMemory(car+0x20,&geometry,4) || geometry!=ride+0x40 ||
            !ReadGameMemory(car+0x40,&base,4) || base!=geometry ||
            !ReadGameMemory(geometry+0x110,&mass,4) || !std::isfinite(mass) || mass<=0 ||
            !SameMass(mass,e.mass) ||
            !ReadGameMemory(car+0x5C0,&terrain,1) || !terrain ||
            !CoopPhysicalMotionNative::WritableRange(geometry,0x2A0)) return false;
        float previousPosition[3]{},previousQuaternion[4]{},previousVelocity[3]{},previousAngular[3]{},previousMatrix[16]{};
        if(!ReadCoopWorldPhysicalMotion(car,previousPosition,previousQuaternion,previousVelocity,previousAngular) ||
            !coop_motion::QuaternionToMatrix(previousQuaternion,previousMatrix)) return false;
        CoopWorldContactNative::setMode(reinterpret_cast<void*>(car),3);
        if(!ReadBody(car,body)) return false;
        CoopPhysicalMotionNative::setBodyState(reinterpret_cast<void*>(body.body),previousPosition,previousMatrix,previousVelocity,previousAngular,nullptr);
        if(!SafeWriteBytes(car+0x60,previousPosition,12) || !SafeWriteBytes(car+0x90,previousMatrix,64) ||
            !SafeWriteBytes(car+0x70,previousVelocity,12) || !SafeWriteBytes(car+0x110,previousAngular,12)) return false;
    }
    if(!SameMass(body.mass,e.mass)) return false;
    float p[3]{},spin[3]{},v[3]{},a[3]{},dv[3]{},da[3]{};
    if(!ReadGameMemory(body.body+0x70,p,12) || !ReadGameMemory(body.body+0x80,spin,12) ||
        !ReadGameMemory(body.body+0xA0,v,12) || !ReadGameMemory(body.body+0xC0,a,12)) return false;
    for(unsigned i=0;i<3;++i) {p[i]+=e.impulse[i];spin[i]+=e.angularImpulse[i];v[i]=p[i]*body.inverseMass;}
    AngularDelta(body,spin,a);
    if(!CoopPhysicalMotionNative::FiniteVector(v,500) || !CoopPhysicalMotionNative::FiniteVector(a,100) ||
        !coop_world_impact::Vector(p,body.mass*1000) || !coop_world_impact::Vector(spin,body.mass*10000)) return false;
    Binding rechecked{};Body recaptured{};
    if(!Resolve(car,rechecked) || rechecked.identity!=e.identity || rechecked.state.key!=e.key ||
        rechecked.state.descriptor.modelId!=e.model || rechecked.state.descriptor.appearanceHash!=e.appearance ||
        !ReadBody(car,recaptured) || recaptured.wrapper!=body.wrapper || recaptured.body!=body.body ||
        recaptured.witness!=body.witness || !SameMass(recaptured.mass,e.mass)) return false;
    return CommitMomentum(car,recaptured,p,spin,v,a);
}
static void Reset() {
    replay.Reset();acknowledgements={};predictions={};predictedBodies={};ackCursor=0;
    AcquireSRWLockExclusive(&lock);inboxCount=outgoingCount=0;ReleaseSRWLockExclusive(&lock);
}
}
void ApplyCoopWorldImpactPrediction(uintptr_t car,coop_world::Motion& motion) {
    using namespace CoopWorldImpactRuntime;
    if(CoopSnapshot().role!=coop::Role::Client || motion.aiFrozen) return;
    Binding b{};if(!Resolve(car,b)) return;
    if(auto* p=Predict(b.identity,b.state.key,false)) {
        float position[]{motion.x,motion.y,motion.z};
        const auto now=CoopAIWorldRuntime::MotionNow();
        if(p->native) {
            CoopWorldImpactContactNative::Body body{};float physicalPosition[3]{},rotation[4]{},velocity[3]{},angular[3]{};
            if(!CoopWorldImpactContactNative::ReadBody(car,body) ||
                body.body!=predictedBodies[static_cast<size_t>(p-predictions.data())] ||
                !PhysicalState(body,physicalPosition,rotation,velocity,angular)) {*p={};return;}
            p->ApplyNative(now,motion.sentAtMs,position,motion.orientation,motion.linearVelocity,motion.angularVelocity,
                physicalPosition,rotation,velocity,angular);
        } else p->Apply(now,motion.sentAtMs,position,motion.orientation,motion.linearVelocity,motion.angularVelocity);
        motion.x=position[0];motion.y=position[1];motion.z=position[2];
    }
}
void TickCoopWorldImpact(DWORD flow) {
    using namespace CoopWorldImpactRuntime;
    const auto s=CoopSnapshot();const auto now=CoopAIWorldRuntime::MotionNow();
    if(flow!=6 || !Session(s)) {if(sessionId) {Reset();sessionId=0;}return;}
    if(s.sessionId!=sessionId || s.peer!=peer || s.role!=role) {Reset();sessionId=s.sessionId;peer=s.peer;role=s.role;}
    std::array<Queued,64> messages{};size_t count=0;
    AcquireSRWLockExclusive(&lock);count=inboxCount;std::copy_n(inbox.begin(),count,messages.begin());inboxCount=0;ReleaseSRWLockExclusive(&lock);
    for(size_t i=0;i<count;++i) {
        const auto& q=messages[i];const auto& e=q.event;
        if(q.peer!=s.peer || e.identity.sessionId!=s.sessionId) continue;
        if(s.role==coop::Role::Client && e.type==coop_world_impact::Type::Ack) {
            if(auto* p=Predict(e.identity,e.key,false)) p->Ack(e);
            AcquireSRWLockExclusive(&lock);
            for(size_t j=0;j<outgoingCount;++j) if(coop_world_impact::SameContact(outgoing[j].event,e)) {
                if(e.accepted) ++ackAccepted;else ++ackRejected;
                outgoing[j]=outgoing[--outgoingCount];break;
            }
            ReleaseSRWLockExclusive(&lock);continue;
        }
        if(s.role!=coop::Role::Host || e.type!=coop_world_impact::Type::Request) continue;
        if(replay.Seen(e.sequence)) {
            for(const auto& a:acknowledgements) if(coop_world_impact::SameContact(a.event,e)) {
                Publish(a.event,now);break;
            }
            ++duplicate;continue;
        }
        Event ack{};ack.identity=e.identity;ack.key=e.key;ack.model=e.model;ack.appearance=e.appearance;
        ack.sequence=e.sequence;ack.observedAt=e.observedAt;ack.type=coop_world_impact::Type::Ack;
        ack.accepted=ApplyHost(e,q.received,now)?1:0;ack.authorityAt=ack.accepted?(now?now:1):0;
        replay.Commit(e.sequence);acknowledgements[ackCursor++%acknowledgements.size()].event=ack;
        if(ack.accepted) ++accepted;else ++rejected;Publish(ack,now);
    }
    // Keep correction slots bounded across despawn/reload/menu transitions.
    for(auto& p:predictions) if(p.active && (coop_world::SerialDelta(now,p.start)>1100 ||
        p.identity.sessionId!=s.sessionId)) p={};
    if(coop_world::SerialDelta(now,lastTrace)>=2000) {
        lastTrace=now;char line[512]{};
        std::snprintf(line,sizeof(line),"[AI_IMPACT] role=%s sent=%u accepted=%u rejected=%u duplicates=%u dropped=%u contacts=%u suppressed=%u dispatches=%u paired=%u authorized=%u body_refused=%u invalid=%u report_refused=%u ack=%u/%u wheels=%u/%u/%u native_touches=%u stale_authority=%u unacknowledged_expired=%u\r\n",
            s.role==coop::Role::Host?"Host":"Client",reported,accepted,rejected,duplicate,dropped.load(),
            CoopWorldImpactContactNative::captured,CoopWorldImpactContactNative::suppressed,
            CoopWorldImpactContactNative::dispatches,CoopWorldImpactContactNative::paired,CoopWorldImpactContactNative::authorized,CoopWorldImpactContactNative::refused,
            CoopWorldImpactContactNative::invalid,reportRefused,ackAccepted,ackRejected,
            CoopWorldWheelNative::captured,CoopWorldWheelNative::applied,CoopWorldWheelNative::refused,nativeTouches,staleAuthority,expiredRequests.load());LogMessage(line);
        std::snprintf(line,sizeof(line),"[AI_IMPACT_BIND] session=%u race_scope=%u no_latest=%u fleet=%u descriptor=%u stale=%u identity=%u frozen=%u visible=%u track=%u\r\n",
            bindRefused[0],bindRefused[1],bindRefused[2],bindRefused[3],bindRefused[4],bindRefused[5],bindRefused[6],bindRefused[7],bindRefused[8],bindRefused[9]);LogMessage(line);
    }
}
bool ReceiveCoopWorldImpactPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD) {
    using namespace CoopWorldImpactRuntime;
    uint32_t magic=0;if(bytes<4 || !data) return false;std::memcpy(&magic,data,4);
    if(coop_world_wire::LE32(magic)!=coop_world_impact::kMagic) return false;
    Event e{};const auto s=CoopSnapshot();const coop::Endpoint source{sender.sin_addr.s_addr,ntohs(sender.sin_port)};
    if(!s.joined || source!=s.peer || !coop_world_impact::Decode(data,bytes,e) || e.identity.sessionId!=s.sessionId ||
        (s.role==coop::Role::Host?e.type!=coop_world_impact::Type::Request:
         s.role==coop::Role::Client?e.type!=coop_world_impact::Type::Ack:true)) return true;
    AcquireSRWLockExclusive(&lock);
    if(inboxCount<inbox.size()) inbox[inboxCount++]={e,source,CoopAIWorldRuntime::MotionNow()};else ++dropped;
    ReleaseSRWLockExclusive(&lock);return true;
}
void TickCoopWorldImpactNetwork(DWORD) {
    using namespace CoopWorldImpactRuntime;
    const auto s=CoopSnapshot();if(!s.joined || !s.fresh) return;
    const auto now=CoopAIWorldRuntime::MotionNow();std::array<Packet,64> packets{};size_t count=0;
    AcquireSRWLockExclusive(&lock);
    for(size_t i=0;i<outgoingCount;) {
        auto& q=outgoing[i];const bool ack=q.event.type==coop_world_impact::Type::Ack;
        if(q.event.identity.sessionId!=s.sessionId || coop_world::SerialDelta(now,q.start)>700) {
            if(!ack && q.event.identity.sessionId==s.sessionId) ++expiredRequests;
            q=outgoing[--outgoingCount];continue;
        }
        if(!q.last || coop_world::SerialDelta(now,q.last)>=60) {
            if(coop_world_impact::Encode(q.event,packets[count])) ++count;q.last=now;
            if(ack) {q=outgoing[--outgoingCount];continue;}
        }
        ++i;
    }
    ReleaseSRWLockExclusive(&lock);
    for(size_t i=0;i<count;++i) SendSessionBytes(&packets[i],sizeof(Packet),s.peer);
}
static bool InstallCoopWorldImpact() {
    CoopAINativePlacementHooks::localImpactIntegration=&CoopWorldImpactRuntime::NativeIntegration;
    CoopWorldImpactContactNative::touch=&CoopWorldImpactRuntime::Touch;
    CoopWorldImpactContactNative::authorize=&CoopWorldImpactRuntime::Authorize;
    CoopWorldImpactContactNative::report=&CoopWorldImpactRuntime::Report;
    return CoopWorldImpactContactNative::Install();
}
