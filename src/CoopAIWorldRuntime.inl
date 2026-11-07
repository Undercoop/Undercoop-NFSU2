// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Native AI integration is deliberately opt-in while world/traffic adapters
// are validated. No native objects are inspected by the network thread.
#include "CoopWorldWire.h"
#include "CoopTime.h"
#include <atomic>
namespace CoopAIWorldRuntime {
// GetTickCount commonly advances only once per 10--16ms. Using it for both
// >64Hz capture and rendering repeatedly reset the sender-time buffer and
// stepped otherwise smooth native motion. Keep all three motion clocks
// (capture, render, actual enqueue) on this monotonic QPC timebase instead.
static DWORD PreciseMotionNow() {
    struct Clock {
        LARGE_INTEGER frequency{}, origin{};
        DWORD coarseOrigin = GetTickCount();
        Clock() {
            if(!QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0 || !QueryPerformanceCounter(&origin)) frequency.QuadPart=0;
        }
    };
    static const Clock clock;
    LARGE_INTEGER current{};
    if(!clock.frequency.QuadPart || !QueryPerformanceCounter(&current) || current.QuadPart<clock.origin.QuadPart) return GetTickCount();
    const auto elapsed=current.QuadPart-clock.origin.QuadPart;
    // Split before multiplying, preserving DWORD wrap without long-uptime
    // overflow in the high-frequency counter-to-millisecond conversion.
    const auto milliseconds=(elapsed/clock.frequency.QuadPart)*1000+
        ((elapsed%clock.frequency.QuadPart)*1000)/clock.frequency.QuadPart;
    return clock.coarseOrigin+static_cast<DWORD>(milliseconds);
}
static DWORD (*motionNow)()=PreciseMotionNow; // Private fixtures inject their clock.
static DWORD MotionNow() { return motionNow ? motionNow() : PreciseMotionNow(); }
struct Binding {
    uintptr_t car=0,parameter=0;
    uint32_t ordinal=0,model=0,lastReceive=0,generation=1;
    coop_world::Descriptor descriptor{};
    coop_world::EntityState latest{};
    bool haveLatest=false;
};
static Binding bindings[4]{};
static uint32_t generations[6]{};
static coop_world::World<22> world;
static coop_world::Identity identity{};
static coop::Role ownerRole=coop::Role::Idle;
static uint32_t sourceSequence=0,publication=0,lastSent=0,applied=0,refused=0;
static uint32_t captureRefused=0,descriptorRefused=0,descriptorMismatch=0;
static uint32_t wheelApplied=0,wheelRefused=0;
static std::atomic<uint32_t> inboxDropped{0};
struct Received { coop_world::Event event{}; DWORD received=0; };
static SRWLOCK lock=SRWLOCK_INIT;
static std::array<Received,64> inbox{};
static size_t inboxCount=0;
static std::array<coop_world_wire::EventPacket,4> outgoing{};
static size_t outgoingCount=0;
static DWORD outgoingAt=0;
static uint64_t outgoingSession=0;
static coop::Endpoint outgoingPeer{};
static constexpr uint64_t kRaceAIEntityBase=0x100000000ULL;
static constexpr size_t kRaceAIOrdinalLimit=6;
static constexpr DWORD kFinishedRefreshMs=500;

// The network thread owns this pacing state. The first terminal frame is
// reliable, then refreshed in case a connection generation was retired.
// Repeating one immutable finish on every frame would refill the control lane.
struct FinishedPublication {
    coop_world::Identity identity{};
    uint32_t membership=0,ticket=0,generation=0;
    uint32_t ticks=0;
    uint16_t nativeStatus=0;
    uint8_t status=0,rank=0;
    DWORD sentAt=0;
};
static bool ShouldPublishFrame(const coop_world_wire::EventPacket& frame,const coop::SessionSnapshot& session,DWORD now) {
    if(frame.motion.finishStatus==coop_world::kFinishRacing)return true;
    const auto key=coop_world_wire::DecodeKey(frame.key);
    if(key.id<=kRaceAIEntityBase||key.id>=kRaceAIEntityBase+kRaceAIOrdinalLimit)return false;
    static std::array<FinishedPublication,kRaceAIOrdinalLimit> lastFinished;
    auto& previous=lastFinished[size_t(key.id-kRaceAIEntityBase)];
    const auto frameIdentity=coop_world_wire::DecodeIdentity(frame.identity);
    const auto& motion=frame.motion;
    const bool sameResult=previous.identity==frameIdentity&&previous.membership==session.membershipNonce&&
        previous.ticket==session.ticket&&previous.generation==key.generation&&
        previous.ticks==motion.finishTicks&&previous.nativeStatus==motion.nativeFinishStatus&&
        previous.status==motion.finishStatus&&previous.rank==motion.finishRank;
    if(sameResult&&coop_time::Elapsed(now,previous.sentAt)<kFinishedRefreshMs)return false;
    previous={frameIdentity,session.membershipNonce,session.ticket,key.generation,motion.finishTicks,
        motion.nativeFinishStatus,motion.finishStatus,motion.finishRank,now};
    return true;
}
static uint32_t NativeRaceMode() {
    uint32_t mode=0;
    // UI gameMode labels are normalized differently on the client. The native
    // race parameter bytes, however, are transferred without enum coercion.
    std::memcpy(&mode,gPendingRaceEvent.raceParams+0x10,sizeof(mode));return mode;
}

static bool Scope(const coop::SessionSnapshot& session,uint32_t track,coop_world::Identity& result) {
    if(!gCoopAIEnabled || !IsCoopGameplayActive() || !session.joined ||
        (session.role!=coop::Role::Host && session.role!=coop::Role::Client) ||
        !track || track==4000 || gPendingRaceEvent.sessionId!=session.sessionId ||
        gPendingRaceEvent.trackId!=track || !gPendingRaceEvent.seed ||
        !CoopAIRosterRaceCount()) return false;
    result={session.sessionId,track,gPendingRaceEvent.seed,gPendingRaceEvent.seed};
    return true;
}
static uint32_t NameHash(const char* text) {
    uint32_t hash=2166136261u;
    for(size_t i=0;i<16 && text[i];++i) { hash^=static_cast<uint8_t>(text[i]);hash*=16777619u; }
    return hash?hash:1u;
}
static bool Descriptor(uintptr_t parameter,uint32_t model,coop_world::Descriptor& result) {
    uintptr_t table=0;
    uint8_t portable[0x940]{};
    if(!IsSupportedCarModel(model) || !ReadGameMemory(0x008A1CCC,&table,sizeof(table)) ||
        !IsValidUserPtr(table) || !ReadGameMemory(table+model*0x890,result.carModel,16) ||
        !std::memchr(result.carModel,0,16) || !ReadNativeAppearancePortable(parameter+0x10,portable)) return false;
    result.modelId=model;result.modelHash=NameHash(result.carModel);
    result.appearanceHash=ComputePeerAppearanceFingerprint(portable);
    // Handling is generated from the authoritative event manifest, not a raw
    // process-local tuning/physics pointer. Both processes validate that event.
    result.handlingHash=gPendingRaceEvent.careerEventHash;
    return coop_world::ValidDescriptor(result);
}
static void Reset() {
    identity={};ownerRole=coop::Role::Idle;for(auto& binding:bindings) binding=Binding{};
    for(auto& generation:generations) generation=0;
    AcquireSRWLockExclusive(&lock);
    outgoingCount=0;inboxCount=0;outgoingSession=0;++publication;
    ReleaseSRWLockExclusive(&lock);
}
static Binding* Find(uintptr_t car) {
    for(auto& binding:bindings) if(binding.car==car && car) return &binding;
    return nullptr;
}
static uintptr_t driftFlagAddress=0x89E7D9;
static bool DriftMode() {uint8_t flag=0;return ReadGameMemory(driftFlagAddress,&flag,1) && flag!=0;}
static bool DriftBank(uintptr_t car,uintptr_t& bank,float& value) {
    uintptr_t racer=0,owner=0,controller=0;
    return IsValidUserPtr(car) && ReadGameMemory(car+0x1C,&racer,4) && IsValidUserPtr(racer) &&
        ReadGameMemory(racer,&owner,4) && owner==car && ReadGameMemory(racer+0x1D4,&controller,4) && IsValidUserPtr(controller) &&
        ReadGameMemory(controller,&bank,4) && IsValidUserPtr(bank) && ReadGameMemory(bank+0x2C,&value,4) &&
        std::isfinite(value) && value>=0 && value<=1.0e9f;
}
static bool Matches(Binding& binding) {
    uintptr_t parameter=0,ride=0;
    uint32_t ordinal=0,model=0;
    return IsValidUserPtr(binding.car) &&
        ReadGameMemory(binding.car+0x14,&parameter,sizeof(parameter)) && parameter==binding.parameter &&
        ReadGameMemory(binding.car+0x10,&ride,sizeof(ride)) && ride==parameter+0x10 &&
        ReadCoopAIRosterParam(parameter,ordinal,model) && ordinal==binding.ordinal && model==binding.model;
}
static bool Capture(Binding& binding,coop_world::Motion& motion,DWORD now) {
    ReadCoopWorldWheelState(binding.car,motion.wheels);
    float position[3]{},matrix[16]{};
    uintptr_t racer=0,owner=0;
    uint16_t status=0;uint8_t rank=0;uint32_t ticks=0;
    if(!Matches(binding) ||
        !ReadCoopWorldPhysicalMotion(binding.car,position,motion.orientation,motion.linearVelocity,motion.angularVelocity) ||
        !coop_motion::QuaternionToMatrix(motion.orientation,matrix) ||
        !ReadGameMemory(binding.car+0x1C,&racer,sizeof(racer)) || !IsValidUserPtr(racer) ||
        !ReadGameMemory(racer,&owner,sizeof(owner)) || owner!=binding.car ||
        !ReadGameMemory(racer+0x20,&motion.totalProgress,4) ||
        !ReadGameMemory(binding.car+0x8FC,&motion.splineDistance,4) ||
        !ReadGameMemory(racer+8,&motion.lap,1) || !ReadGameMemory(racer+9,&motion.completedLaps,1)) return false;
    // Verified mode-seven native comparator reads +24 as a float. Do not
    // reinterpret that field for an unverified mode/layout.
    if(NativeRaceMode()==7 && !ReadGameMemory(racer+0x24,&motion.nativeRaceScore,4)) return false;
    if(DriftMode()) {uintptr_t bank=0;if(!DriftBank(binding.car,bank,motion.nativeRaceScore))return false;}
    motion.x=position[0];motion.y=position[1];motion.z=position[2];
    motion.forwardX=matrix[0];motion.forwardY=matrix[1];motion.forwardZ=matrix[2];
    motion.headingDeg=std::atan2(matrix[1],matrix[0])*57.295779513f;
    if(motion.headingDeg<0) motion.headingDeg+=360.0f;
    const auto* v=motion.linearVelocity;
    motion.speedKmh=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2])*3.6f;
    ReadGameMemory(binding.car+0x40E,&motion.steerAngle,2);
    ReadGameMemory(binding.car+0x4E8,motion.wheelPhaseRad,sizeof(motion.wheelPhaseRad));
    ReadGameMemory(binding.car+0x4F8,motion.wheelSpeedRad,sizeof(motion.wheelSpeedRad));
    for(unsigned wheel=0;wheel<4;++wheel) {
        float rotation[16]{};
        if(ReadGameMemory(binding.car+0x130+wheel*0x40,rotation,sizeof(rotation)) && coop_motion::IsRotation(rotation)) {
            for(unsigned row=0;row<3;++row) for(unsigned column=0;column<3;++column)
                motion.wheelRotation[wheel][row*3+column]=rotation[row*4+column];
            motion.wheelRotationValidMask|=1u<<wheel;
        }
    }
    if(ReadGameMemory(racer+0x10,&status,2) && status && status<=11 &&
        ReadGameMemory(racer+0x0C,&rank,1) && rank>0 && rank<=CoopAIRosterRaceCount() &&
        ReadGameMemory(racer+0x198,&ticks,4) && ticks) {
        motion.finishStatus=(status==1 || status==6)?coop_world::kFinishComplete:coop_world::kFinishDnf;
        motion.nativeFinishStatus=status;motion.finishRank=rank;
        motion.finishTicks=ticks;motion.aiFrozen=1;
    }
    motion.sequence=++sourceSequence;motion.sentAtMs=now;motion.sessionId=identity.sessionId;
    motion.trackId=identity.trackId;motion.worldEpoch=identity.worldEpoch;
    std::memcpy(motion.carModel,binding.descriptor.carModel,16);
    return coop_world::ValidMotion(motion);
}
static bool ApplyWheelOrientation(uintptr_t car,unsigned wheel,const float* rotation) {
    if(wheel>=4 || !rotation) return false;
    uintptr_t ride=0;uint32_t kind=0;
    if(!ReadManagedCoopAI(car,ride,kind) || (kind!=2 && kind!=3)) return false;
    float matrix[16]{};
    if(!ReadGameMemory(car+0x130+wheel*0x40,matrix,sizeof(matrix))) return false;
    // Native tire matrices also carry renderer translation/homogeneous data;
    // the wire only carries their 3x3 orientation. Preserve ALL seven other
    // values and every hub matrix at +230. Zeroing a full 64-byte matrix made
    // the client tires disappear even though body application succeeded.
    constexpr unsigned preserved[]{3,7,11,12,13,14,15};
    for(const auto index:preserved) if(!std::isfinite(matrix[index])) return false;
    for(unsigned row=0;row<3;++row) for(unsigned column=0;column<3;++column)
        matrix[row*4+column]=rotation[row*3+column];
    if(!coop_motion::IsRotation(matrix)) return false;
    for(unsigned row=0;row<3;++row)
        if(!SafeWriteBytes(car+0x130+wheel*0x40+row*0x10,matrix+row*4,3*sizeof(float))) return false;
    return true;
}
static bool SyncProgress(Binding& binding,const coop_world::Motion& motion) {
    uintptr_t racer=0,owner=0;float length=0;
    uintptr_t authorizedRide=0;uint32_t authorizedKind=0;
    // This helper can be reached separately from TickCoopAIWorld. Never rely on
    // an earlier role snapshot to authorize progress/finish writes on a Host.
    if(!ReadManagedCoopAI(binding.car,authorizedRide,authorizedKind) || authorizedKind!=2 ||
        authorizedRide!=binding.parameter+0x10 || !Matches(binding) ||
        !ReadGameMemory(binding.car+0x1C,&racer,sizeof(racer)) || !IsValidUserPtr(racer) ||
        !ReadGameMemory(racer,&owner,sizeof(owner)) || owner!=binding.car ||
        !ReadGameMemory(gRaceProgressAddresses.routeLength,&length,4) || !std::isfinite(length) ||
        length<=100 || length>1000000 || motion.splineDistance<0 || motion.splineDistance>length+1 ||
        motion.totalProgress< -1.1f || motion.totalProgress>22.1f || motion.lap>22 || motion.completedLaps>22) return false;
    const float fraction=std::clamp(motion.splineDistance/length,0.0f,1.0f);
    if(!SafeWriteBytes(binding.car+0x8FC,&motion.splineDistance,4) ||
        !SafeWriteBytes(binding.car+0x8F8,&fraction,4) || !SafeWriteBytes(racer+8,&motion.lap,1) ||
        !SafeWriteBytes(racer+9,&motion.completedLaps,1) ||
        !SafeWriteBytes(racer+0x14,&motion.totalProgress,4) || !SafeWriteBytes(racer+0x20,&motion.totalProgress,4)) return false;
    if(NativeRaceMode()==7 && !SafeWriteBytes(racer+0x24,&motion.nativeRaceScore,4)) return false;
    if(DriftMode()) {uintptr_t bank=0;float previous=0;
        if(!std::isfinite(motion.nativeRaceScore) || motion.nativeRaceScore<0 || motion.nativeRaceScore>1.0e9f ||
            !DriftBank(binding.car,bank,previous) || !SafeWriteBytes(bank+0x2C,&motion.nativeRaceScore,4))return false;}
    if(motion.finishStatus && motion.finishRank && motion.finishRank<=CoopAIRosterRaceCount()) {
        const uint16_t status=motion.nativeFinishStatus;
        if(!status || status>11) return false;
        return SafeWriteBytes(racer+0x0A,&motion.finishRank,1) && SafeWriteBytes(racer+0x0C,&motion.finishRank,1) &&
            SafeWriteBytes(racer+0x10,&status,2) && SafeWriteBytes(racer+0x198,&motion.finishTicks,4);
    }
    return true;
}
} // namespace CoopAIWorldRuntime

bool ReadManagedCoopAI(uintptr_t car,uintptr_t& ride,uint32_t& kind) {
    if(ReadManagedCoopFleet(car,ride,kind)) return true;
    using namespace CoopAIWorldRuntime;
    const auto session=CoopSnapshot();coop_world::Identity expected{};
    uint32_t flow=0,track=0;
    if(!ReadGameMemory(kGameFlowState,&flow,4) || flow!=6 ||
        !ReadGameMemory(gRaceContextAddresses.track,&track,4) || !Scope(session,track,expected) ||
        session.role!=coop::Role::Client || ownerRole!=coop::Role::Client || expected!=identity ||
        car==CoopRaceCar(false) || car==CoopRaceCar(true)) return false;
    Binding* binding=Find(car);
    // Ownership survives packet loss. Stale motion is not applied below, but
    // resuming a second independent AI simulation would split race results.
    if(!binding || !Matches(*binding)) return false;
    ride=binding->parameter+0x10;kind=2;return true;
}

void TickCoopAIWorld(DWORD flow) {
    using namespace CoopAIWorldRuntime;
    const auto session=CoopSnapshot();const DWORD now=MotionNow();
    uint32_t track=0;coop_world::Identity scope{};
    if(flow!=6 || !ReadGameMemory(gRaceContextAddresses.track,&track,4) || !Scope(session,track,scope)) {
        if(identity.Valid()) Reset();return;
    }
    const bool host=session.role==coop::Role::Host;
    if(scope!=identity || ownerRole!=session.role) {
        Reset();identity=scope;ownerRole=session.role;
        coop_world::RacePolicy policy{};policy.raceId=scope.raceEpoch;
        policy.vehicleCapacity=static_cast<uint16_t>(CoopAIRosterRaceCount());
        // Native event traffic remains independent of the grid budget.
        policy.trafficAllowed=true;
        if(!world.Begin(scope,1,host?1:2,policy,now)) { Reset();return; }
    }
    uintptr_t cars[kMaxVehicleSlots]{};
    if(!ReadGameMemory(kCarArray,cars,sizeof(cars))) return;
    for(auto& binding:bindings) if(binding.car && !Matches(binding)) binding=Binding{};
    for(uintptr_t car:cars) {
        uintptr_t parameter=0,ride=0;uint32_t ordinal=0,model=0;
        if(!IsValidUserPtr(car) || car==CoopRaceCar(false) || car==CoopRaceCar(true) ||
            !ReadGameMemory(car+0x14,&parameter,sizeof(parameter)) ||
            !ReadGameMemory(car+0x10,&ride,sizeof(ride)) || ride!=parameter+0x10 ||
            !ReadCoopAIRosterParam(parameter,ordinal,model) || !ordinal || ordinal>=kRaceAIOrdinalLimit) continue;
        Binding* existing=Find(car);if(existing) continue;
        bool duplicateOrdinal=false;
        for(const auto& bound:bindings) if(bound.car && bound.ordinal==ordinal) duplicateOrdinal=true;
        if(duplicateOrdinal) continue;
        Binding* slot=nullptr;
        for(auto& candidate:bindings) if(!candidate.car) { slot=&candidate;break; }
        if(!slot) continue;
        coop_world::Descriptor descriptor{};
        if(!Descriptor(parameter,model,descriptor)) { ++descriptorRefused;continue; }
        *slot=Binding{};slot->car=car;slot->parameter=parameter;slot->ordinal=ordinal;slot->model=model;slot->descriptor=descriptor;
        slot->generation=++generations[ordinal];if(!slot->generation) slot->generation=++generations[ordinal];
    }
    if(host) {
        std::array<coop_world_wire::EventPacket,4> frames{};size_t count=0;
        for(auto& binding:bindings) {
            if(!binding.car) continue;
            coop_world::Event event{};event.identity=identity;event.action=coop_world::Action::Spawn;
            event.state.key={kRaceAIEntityBase+binding.ordinal,binding.generation};event.state.kind=coop_world::Kind::RaceRacer;
            event.state.raceMembershipId=identity.raceEpoch;event.state.descriptor=binding.descriptor;
            if(!Capture(binding,event.state.motion,now)) { ++captureRefused;continue; }
            event.sequence=event.state.motion.sequence;
            if(coop_world_wire::EncodeEvent(event,1,frames[count])) ++count;
        }
        AcquireSRWLockExclusive(&lock);outgoing=frames;outgoingCount=count;outgoingAt=now;
        outgoingSession=session.sessionId;outgoingPeer=session.peer;++publication;ReleaseSRWLockExclusive(&lock);
    } else {
        std::array<Received,64> received{};size_t count=0;
        AcquireSRWLockExclusive(&lock);count=inboxCount;std::copy_n(inbox.begin(),count,received.begin());
        inboxCount=0;ReleaseSRWLockExclusive(&lock);
        for(size_t i=0;i<count;++i) {
            const auto& message=received[i];if(message.event.identity!=identity) continue;
            for(auto& binding:bindings) {
                if(!binding.car || message.event.state.key.id!=kRaceAIEntityBase+binding.ordinal) continue;
                if(!(message.event.state.descriptor==binding.descriptor)) { ++descriptorMismatch;continue; }
                if(world.Apply(message.event,1,message.received)!=coop_world::ApplyResult::Applied) break;
                // Read back the normalized immutable finish, not the raw UDP
                // snapshot which may still describe a pre-finish frame.
                const auto* normalized=world.Find(message.event.state.key);
                if(!normalized) break;
                binding.latest=*normalized;binding.haveLatest=true;binding.lastReceive=message.received;break;
            }
        }
        const uintptr_t local=CoopRaceCar(false);
        for(auto& binding:bindings) {
            if(!binding.haveLatest) continue;
            coop_world::EntityState rendered{};
            if(!world.Sample(binding.latest.key,now,rendered)) continue;
            auto& motion=rendered.motion;
            if(coop_world::SerialDelta(now,binding.lastReceive)>1000) {
                // Freeze the last bounded visual sample on lost authority,
                // including any native body: never let cached velocity keep
                // integrating or start an independent client AI simulation.
                for(unsigned axis=0;axis<3;++axis) motion.linearVelocity[axis]=motion.angularVelocity[axis]=0;
                for(auto& speed:motion.wheelSpeedRad) speed=0;
                motion.speedKmh=0;
            }
            ApplyCoopWorldImpactPrediction(binding.car,motion);
            const float position[3]={motion.x,motion.y,motion.z};
            if(ApplyCoopWorldPhysicalMotion(binding.car,local,position,motion.orientation,motion.linearVelocity,motion.angularVelocity)) {
                ++applied;SafeWriteBytes(binding.car+0x40E,&motion.steerAngle,2);
                SetCoopAIReplicaContacts(binding.car,local,
                    !motion.aiFrozen && coop_world::SerialDelta(now,binding.lastReceive)<=1000);
                if(motion.wheels.validMask) ApplyCoopWorldWheelState(binding.car,local,motion.wheels);
                for(unsigned wheel=0;wheel<4;++wheel) if(motion.wheelRotationValidMask&(1u<<wheel)) {
                    if(ApplyWheelOrientation(binding.car,wheel,motion.wheelRotation[wheel])) ++wheelApplied;
                    else ++wheelRefused;
                }
            } else ++refused;
            SyncProgress(binding,binding.latest.motion);
        }
    }
}

void SyncCoopAIRaceProgress() {
    using namespace CoopAIWorldRuntime;
    if(!gCoopAIEnabled || CoopSnapshot().role!=coop::Role::Client) return;
    for(auto& binding:bindings) {
        uintptr_t ride=0;uint32_t kind=0;
        if(binding.haveLatest && ReadManagedCoopAI(binding.car,ride,kind))
            SyncProgress(binding,binding.latest.motion);
    }
}
bool CoopFinishIsManagedAIRacer(uintptr_t racer) {
    using namespace CoopAIWorldRuntime;
    uintptr_t car=0,back=0,ride=0;uint32_t kind=0;
    return ReadGameMemory(racer,&car,sizeof(car)) && ReadManagedCoopAI(car,ride,kind) &&
        ReadGameMemory(car+0x1C,&back,sizeof(back)) && back==racer;
}
bool CoopFinishReadManagedAI(uintptr_t racer,uint8_t& rank,uint32_t& ticks,uint16_t& status) {
    using namespace CoopAIWorldRuntime;
    if(!CoopFinishIsManagedAIRacer(racer)) return false;
    uintptr_t car=0;
    if(!ReadGameMemory(racer,&car,sizeof(car))) return false;
    const Binding* binding=Find(car);
    if(!binding || !binding->haveLatest) return false;
    const auto& motion=binding->latest.motion;
    if(!motion.finishStatus || !motion.finishRank || motion.finishRank>CoopAIRosterRaceCount() ||
        !motion.nativeFinishStatus || motion.nativeFinishStatus>11) return false;
    rank=motion.finishRank;ticks=motion.finishTicks;status=motion.nativeFinishStatus;return true;
}

bool ReceiveCoopAIWorldPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now) {
    using namespace CoopAIWorldRuntime;
    uint32_t magic=0;if(!data || bytes<4) return false;std::memcpy(&magic,data,4);
    if(magic!=coop_world_wire::kEventMagic) return false;
    if(!gCoopAIEnabled) return true;
    const auto session=CoopSnapshot();const coop::Endpoint endpoint{sender.sin_addr.s_addr,ntohs(sender.sin_port)};
    coop_world::Event event{};uint32_t player=0;
    if(session.role!=coop::Role::Client || !session.joined || !session.fresh || sender.sin_family!=AF_INET ||
        endpoint!=session.peer || !coop_world_wire::DecodeEvent(data,bytes,event,player,1) ||
        event.identity.sessionId!=session.sessionId || event.state.kind!=coop_world::Kind::RaceRacer) return true;
    (void)now; // Outer socket-drain tick can predate a whole burst of packets.
    const DWORD receivedAt=MotionNow();
    AcquireSRWLockExclusive(&lock);
    if(inboxCount<inbox.size()) inbox[inboxCount++]={event,receivedAt};
    else ++inboxDropped;
    ReleaseSRWLockExclusive(&lock);return true;
}
void TickCoopAIWorldNetwork(DWORD now) {
    using namespace CoopAIWorldRuntime;
    if(!gCoopAIEnabled) return;
    (void)now; // Publication freshness must use the same QPC anchor as capture.
    const DWORD motionAt=MotionNow();
    const auto session=CoopSnapshot();std::array<coop_world_wire::EventPacket,4> frames{};size_t count=0;
    if(session.role!=coop::Role::Host || !session.joined || !session.fresh) return;
    AcquireSRWLockExclusive(&lock);
    if(lastSent!=publication && outgoingSession==session.sessionId && outgoingPeer==session.peer && motionAt-outgoingAt<=250) {
        frames=outgoing;count=outgoingCount;lastSent=publication;
    }
    ReleaseSRWLockExclusive(&lock);
    for(size_t i=0;i<count;++i) {
        if(ShouldPublishFrame(frames[i],session,motionAt))SendSessionBytes(&frames[i],sizeof(frames[i]),session.peer);
    }
}

// Read-only live-world inventory to establish the actual pooled traffic/model
// lifecycle. It never creates/despawns cars, redirects profiles or changes AI.
void TraceCoopAIWorld(DWORD flow) {
    static DWORD last=0;const DWORD now=GetTickCount();
    if(!IsCoopGameplayActive() || flow!=6 || now-last<5000) return;last=now;
    const DWORD motionAt=CoopAIWorldRuntime::MotionNow();
    uint32_t track=0,params=0,types[4]{};uintptr_t cars[kMaxVehicleSlots]{};
    ReadGameMemory(gRaceContextAddresses.track,&track,4);ReadGameMemory(0x89E814,&params,4);
    if(!ReadGameMemory(kCarArray,cars,sizeof(cars))) return;
    char fleet[400]{};size_t written=0;uintptr_t seen[kMaxVehicleSlots]{};unsigned seenCount=0;
    for(uintptr_t car:cars) {
        if(!IsValidUserPtr(car)) continue;bool duplicate=false;
        for(unsigned i=0;i<seenCount;++i) if(seen[i]==car) duplicate=true;
        if(duplicate) continue;seen[seenCount++]=car;
        uintptr_t parameter=0;uint32_t type=0,model=0,mode=0;uint8_t active=0;
        if(!ReadGameMemory(car+0x14,&parameter,sizeof(parameter)) || !IsValidUserPtr(parameter) ||
            !ReadGameMemory(parameter+4,&type,4) || type>3 || !type ||
            !ReadGameMemory(car+0x10,&parameter,sizeof(parameter)) || !ReadGameMemory(parameter,&model,4)) continue;
        ++types[type];ReadGameMemory(car+0x28,&mode,4);ReadGameMemory(car+0x550,&active,1);
        if(written+35<sizeof(fleet)) written+=std::snprintf(fleet+written,sizeof(fleet)-written," %u:%u/%u/%u",type,model,mode,active);
    }
    char line[680];std::snprintf(line,sizeof(line),
        "[AI_WORLD] track=%u native_params=%u human=%u racer=%u traffic=%u replica_applied=%u refused=%u%s\r\n",
        track,params,types[1],types[2],types[3],CoopAIWorldRuntime::applied,CoopAIWorldRuntime::refused,fleet);
    LogMessage(line);
    unsigned boundCount=0;
    for(const auto& binding:CoopAIWorldRuntime::bindings) if(binding.car) ++boundCount;
    std::snprintf(line,sizeof(line),
        "[AI_REPLICA] enabled=%u joined=%u role=%u bindings=%u race_count=%u capture_refused=%u descriptor_refused=%u descriptor_mismatch=%u capture_cache=%u capture_body=%u capture_native_refused=%u\r\n",
        gCoopAIEnabled?1:0,CoopSnapshot().joined?1:0,static_cast<unsigned>(CoopSnapshot().role),boundCount,
        CoopAIRosterRaceCount(),CoopAIWorldRuntime::captureRefused,CoopAIWorldRuntime::descriptorRefused,CoopAIWorldRuntime::descriptorMismatch,
        CoopWorldPhysicalMotionNative::captureFromCache,CoopWorldPhysicalMotionNative::captureFromBody,
        CoopWorldPhysicalMotionNative::captureRefused);
    LogMessage(line);
    for(const auto& binding:CoopAIWorldRuntime::bindings) if(binding.car) {
        std::snprintf(line,sizeof(line),
            "[AI_REPLICA_CAR] ordinal=%u model=%u car=%08llX param=%08llX generation=%u appearance=%016llX latest=%u age=%d finish=%u rank=%u ticks=%u\r\n",
            binding.ordinal,binding.model,static_cast<unsigned long long>(binding.car),static_cast<unsigned long long>(binding.parameter),
            binding.generation,static_cast<unsigned long long>(binding.descriptor.appearanceHash),binding.haveLatest?1:0,
            binding.haveLatest?coop_world::SerialDelta(motionAt,binding.lastReceive): -1,
            binding.latest.motion.nativeFinishStatus,binding.latest.motion.finishRank,binding.latest.motion.finishTicks);
        LogMessage(line);
        coop_world::MotionDiagnostics diagnostic{};
        if(binding.haveLatest && CoopAIWorldRuntime::world.ReadMotionDiagnostics(binding.latest.key,diagnostic)) {
            std::snprintf(line,sizeof(line),
                "[AI_BUFFER] ordinal=%u history=%u delay_ms=%u extrap_ms=%u resets=%u same_source_tick=%u source_step_ms=%d render_step_ms=%d sampled_snapshot_lag_ms=%d enqueue_age_ms=%d wheel_applied=%u wheel_refused=%u inbox_dropped=%u\r\n",
                binding.ordinal,static_cast<unsigned>(diagnostic.historyCount),diagnostic.delayMs,diagnostic.extrapolationMs,
                diagnostic.resetCount,diagnostic.coalescedTimes,diagnostic.sourceStepMs,diagnostic.renderStepMs,
                coop_world::SerialDelta(diagnostic.sourceAtMs,diagnostic.sampledSnapshotAtMs),
                coop_world::SerialDelta(CoopAIWorldRuntime::MotionNow(),diagnostic.receiveAtMs),
                CoopAIWorldRuntime::wheelApplied,CoopAIWorldRuntime::wheelRefused,CoopAIWorldRuntime::inboxDropped.load(std::memory_order_relaxed));
            LogMessage(line);
        }
    }
}
