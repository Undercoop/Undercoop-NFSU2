// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Game-thread loading adapter. Network callbacks only access gNetwork under
// gLock: they never inspect a native parameter, palette, loader or event.
#include "CoopCheckpoint.h"
#include "CoopCarCatalog.h"
#include "CoopFleetTransfer.h"
#include "CoopWorldSync.h"
#include "CoopFleetEpoch.h"

bool CoopFleetWaitForPeerAssets();
uint64_t CoopFleetLocalGeneration();
bool ApplyCoopFleetClientSelection(const coop_world::Identity&);

namespace CoopFleetAssetsNative {
constexpr uint32_t kMagic = 0x4146324E; // N2FA, not the race roster protocol.
constexpr uint32_t kRosterMagic = 0x52504643, kPaletteMagic = 0x50465043;
constexpr size_t kStride = 0x940, kTraffic = 16, kAmbient = 3, kPalette = 46;
constexpr uint16_t kRejected = 0xFFFE;
enum Phase : uint8_t { RosterPhase = 1, PalettePhase = 2 };
#pragma pack(push, 1)
struct WireIdentity {
    uint64_t session = 0;
    uint32_t track = 0, world = 0, race = 0;
};
struct Entry {
    uint8_t id = 0, flags = 0, skill = 0, style = 0;
    uint32_t type = 0;
    char name[16]{};
    uint8_t portable[kStride]{};
};
struct Roster {
    uint32_t magic = kRosterMagic;
    uint16_t version = 1;
    uint32_t size = sizeof(Roster);
    WireIdentity identity{};
    uint8_t freeRoam = 0, trafficDisabled = 0, ambientCount = 0, trafficCount = 0;
    Entry entries[kAmbient + kTraffic]{}; // ambient first, then traffic.
};
struct PaletteEntry {
    uint32_t model = 0, selected = 0;
    uint8_t portable[kStride]{};
};
struct Selection {
    uint8_t id = 0;
    uint32_t model = 0;
};
struct Palette {
    uint32_t magic = kPaletteMagic;
    uint16_t version = 1;
    uint32_t size = sizeof(Palette);
    WireIdentity identity{};
    uint8_t paletteCount = 0, trafficCount = 0;
    Selection traffic[kTraffic]{};
    PaletteEntry entries[kPalette]{};
};
struct Envelope {
    uint32_t magic = kMagic;
    uint16_t version = 2, size = sizeof(Envelope);
    WireIdentity identity{};
    uint32_t ticket=0,membership=0;
    uint8_t phase = RosterPhase, senderPlayer = 0;
    coop_checkpoint::Packet block{};
};
#pragma pack(pop)
static_assert(sizeof(Envelope) <= 1200, "Fleet packets must not fragment");
static_assert(sizeof(Palette) <= coop_checkpoint::kMaxFile, "Palette transfer is bounded");
static_assert(sizeof(Roster) <= coop_checkpoint::kMaxFile, "Fleet transfer is bounded");
static WireIdentity Wire(const coop_world::Identity& id) { return {id.sessionId,id.trackId,id.worldEpoch,id.raceEpoch}; }
static bool Same(const WireIdentity& a, const WireIdentity& b) {
    return a.session==b.session && a.track==b.track && a.world==b.world && a.race==b.race;
}
static bool Valid(const WireIdentity& id) { return id.session && id.track && id.world; }
static bool Empty(const void* input, size_t count) {
    const auto* bytes=static_cast<const uint8_t*>(input);
    for(size_t i=0;i<count;++i) if(bytes[i]) return false;
    return true;
}
template<class T> static T Value(const uint8_t* input, size_t offset) {
    T result{}; std::memcpy(&result,input+offset,sizeof(result)); return result;
}
template<class T> static void Put(uint8_t* output, size_t offset, T value) {
    std::memcpy(output+offset,&value,sizeof(value));
}
struct Addresses {
    uintptr_t array=0x89E810, count=0x89E814, parameters=0x89E7A0;
    uintptr_t participants=0x89E7C0, disabled=0x89E7E4;
    uintptr_t palette=0x81E810, loader=0x8A3288, table=0x8A1CCC, parts=0x8A2B68;
    uintptr_t flow=0x8654A4;
};
static Addresses gAddresses{};
static void NativeCtor(uint8_t* ride) { reinterpret_cast<void*(__thiscall*)(void*)>(0x40C910)(ride); }
static void NativeCopy(uint8_t* to,const uint8_t* from) {
    reinterpret_cast<void(__thiscall*)(void*,const void*)>(0x5FC130)(to,from);
}
static void NativeCopyRide(uint8_t* to,const uint8_t* from) {
    reinterpret_cast<void(__thiscall*)(void*,const void*)>(0x413440)(to,from);
}
static void NativeInit(uint8_t* ride,uint32_t model,bool traffic) {
    reinterpret_cast<void(__thiscall*)(void*,int,int,char,char)>(0x610270)(ride,static_cast<int>(model),traffic?-1:0,0,0);
}
static void NativeStock(uint8_t* ride) { reinterpret_cast<void(__thiscall*)(void*,int)>(0x637040)(ride,0); }
static void* NativePart(uint32_t model,uint32_t slot,uint32_t hash) {
    return reinterpret_cast<void*(__thiscall*)(void*,uint32_t,uint32_t,uint32_t,void*,int)>(0x61B980)(
        reinterpret_cast<void*>(gAddresses.parts),model,slot,hash,nullptr,-1);
}
static void NativeParts(uint8_t* ride) { reinterpret_cast<void(__thiscall*)(void*)>(0x61BCD0)(ride); }
static void NativeTexture(uint8_t* ride,uint32_t slot) {
    reinterpret_cast<void(__thiscall*)(void*,int)>(0x61C280)(ride,static_cast<int>(slot));
}
static void NativeAdd(uint8_t* param) {
    reinterpret_cast<void(__thiscall*)(void*,void*)>(&TrampolineRaceParametersAddVehicle)(
        reinterpret_cast<void*>(gAddresses.parameters),param);
}
static bool NativeSend(const Envelope& packet,const coop::Endpoint& endpoint) {
    return SendMeasuredCoopDatagram(&packet,sizeof(packet),endpoint)==sizeof(packet);
}
struct Api {
    void (*ctor)(uint8_t*)=NativeCtor;
    void (*copy)(uint8_t*,const uint8_t*)=NativeCopy;
    void (*copyRide)(uint8_t*,const uint8_t*)=NativeCopyRide;
    void (*init)(uint8_t*,uint32_t,bool)=NativeInit;
    void (*stock)(uint8_t*)=NativeStock;
    void* (*part)(uint32_t,uint32_t,uint32_t)=NativePart;
    void (*parts)(uint8_t*)=NativeParts;
    void (*texture)(uint8_t*,uint32_t)=NativeTexture;
    void (*add)(uint8_t*)=NativeAdd;
    bool (*send)(const Envelope&,const coop::Endpoint&)=NativeSend;
};
static Api gApi{}; // Private fixtures override calls, not validation.
struct Signature { uintptr_t address; uint8_t bytes[8]; uint8_t length; };
static Signature gSignatures[] = {
    {0x40C910,{0x6A,0xFF,0x68,0xEB,0x13,0x77,0x00},7},
    {0x5FC130,{0x55,0x56,0x8B,0xF1,0x57,0x8B,0x7C,0x24},8},
    {0x413440,{0x53,0x8B,0xD9,0x55,0x8B,0x6C,0x24,0x0C},8},
    {0x610270,{0x8B,0x44,0x24,0x04,0x8B,0xD1},6},
    {0x637040,{0x83,0xEC,0x10,0x53,0x55,0x56,0x57},7},
    {0x61B980,{0x51,0x8B,0x44,0x24,0x08,0x56},6},
    {0x61BCD0,{0x81,0xEC,0x88,0x00,0x00,0x00},6},
    {0x61C280,{0x83,0xEC,0x40,0x56,0x57},5}
};
static bool VerifyNative() {
    static uintptr_t lastFailure=0;
    for(const auto& signature:gSignatures) {
        uint8_t bytes[8]{};
        const bool readable=ReadGameMemory(signature.address,bytes,signature.length);
        if(!readable || std::memcmp(bytes,signature.bytes,signature.length)) {
            // Unlimiter's audited replacement has the same RideInfo ABI and
            // part/visibility layout. Preserve its entry; never skip a hook or
            // treat arbitrary JMPs as compatible with the native adapter.
            if(readable&&signature.address==0x61BCD0&&
                coop_unlimiter_ride::Approved(signature.address,bytes,signature.length)) {
                static bool reported=false;
                if(!reported) {
                    LogMessage("[FLEET_ASSETS] Verified Unlimiter 3.1.1.1337 RideInfo parts adapter (SHA-256, PE identity, owned target and body). Existing mod hook preserved.\r\n");
                    reported=true;
                }
                continue;
            }
            if(lastFailure!=signature.address) {
                char message[160]{};
                _snprintf_s(message,sizeof(message),_TRUNCATE,
                    "[FLEET_ASSETS] Native signature %08X %s: %02X %02X %02X %02X %02X %02X %02X %02X; unsupported adapter, streaming held.\r\n",
                    static_cast<unsigned>(signature.address),readable?"differs":"unreadable",
                    bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7]);
                LogMessage(message);lastFailure=signature.address;
            }
            return false;
        }
    }
    lastFailure=0;
    return true;
}
struct Channel {
    coop_fleet_transfer::Sender sender;
    coop_checkpoint::Receiver receiver;
    Envelope ack{},finalAck{};
    bool ackPending=false,finalAckPending=false,expected=false,complete=false;
    uint32_t digest=0;
    uint32_t sent=0,received=0;
    uint16_t sentIndex=0,receivedIndex=0,chunks=0;
};
struct Network {
    WireIdentity identity{};
    coop::Endpoint peer{};
    bool host=false;
    uint32_t ticket=0,membership=0;
    // Game-thread-validated immutable bytes. UDP may confirm an exact replay,
    // but never constructs assets or reads a native/game object.
    std::vector<uint8_t> cachedProof[2];
    DWORD diagnosticAt=0;
    uint32_t rejected=0,sendFailures=0;
    uint8_t rejectReason=0; // 1=wire metadata, 2=current epoch/direction, 3=block order/digest.
    Channel channels[2];
};
static SRWLOCK gLock=SRWLOCK_INIT;
static Network gNetwork;
struct Game {
    WireIdentity identity{};
    uintptr_t array=0;
    uint32_t committedCount=0;
    uint64_t generation=0,localGeneration=0;
    bool host=false, prepared=false, committed=false, palettePrepared=false;
    bool paletteApplied=false, assetsReady=false, holdLogged=false, replay=false;
    char campaign[32]{};
    bool retainedProof=false;
    uint32_t retainedTicket=0,retainedMembership=0,retainedCount=0;
    uintptr_t retainedArray=0;
    Roster roster{};
    Palette palette{};
    alignas(16) uint8_t staged[kAmbient+kTraffic][kStride]{};
    alignas(16) uint8_t palettes[kPalette][kStride]{};
};
static Game gGame;
// Cache only validated pointer-free authority data. Native staging, resource
// leases and array/commit latches are rebuilt for EVERY local load generation.
// A different world, endpoint or role discards the cache; it never authorizes
// replacement of a peer's active models or carries native handles across loads.
struct PortableCache {
    WireIdentity identity{};
    coop::Endpoint peer{};
    bool host=false,rosterValid=false,paletteValid=false;
    Roster roster{};
    Palette palette{};
};
static PortableCache gCache;
static bool Hold(const char* message) {
    if(!gGame.holdLogged) {LogMessage(message);gGame.holdLogged=true;}return false;
}
static bool Scope(const WireIdentity& identity) {
    const auto session=CoopSnapshot();
    return IsCoopGameplayActive() && Valid(identity) && session.joined && session.peer.Valid() &&
        session.sessionId==identity.session && (session.role==coop::Role::Host || session.role==coop::Role::Client) &&
        gGame.identity.session==session.sessionId && gGame.host==(session.role==coop::Role::Host) &&
        Same(identity,gGame.identity) && Same(identity,gCache.identity) && gCache.peer==session.peer &&
        gCache.host==gGame.host && gGame.localGeneration && gGame.localGeneration==CoopFleetLocalGeneration();
}
static bool Publish(const coop_world::Identity& id) {
    const auto session=CoopSnapshot();const auto identity=Wire(id);
    if(!IsCoopGameplayActive() || !Valid(identity) || !session.joined || !session.fresh || !session.peer.Valid() ||
        session.sessionId!=identity.session || (session.role!=coop::Role::Host && session.role!=coop::Role::Client)) return false;
    const bool host=session.role==coop::Role::Host;
    const uint64_t generation=CoopFleetLocalGeneration();if(!generation) return false;
    const bool sameCache=Same(gCache.identity,identity) && gCache.peer==session.peer && gCache.host==host;
    if(sameCache && Same(gGame.identity,identity) && gGame.localGeneration && generation!=gGame.localGeneration &&
        !coop_world::NewerRequest(generation,gGame.localGeneration)) return false;
    if(!sameCache) {
        gCache.~PortableCache();::new (static_cast<void*>(&gCache)) PortableCache{};
        gCache.identity=identity;gCache.peer=session.peer;gCache.host=host;
    }
    if(!Same(gGame.identity,identity) || gGame.host!=host || gGame.localGeneration!=generation || !sameCache) {
        gGame.~Game();::new (static_cast<void*>(&gGame)) Game{};
        gGame.identity=identity;gGame.host=host;gGame.localGeneration=generation;
        std::memcpy(gGame.campaign,session.campaignName,sizeof(gGame.campaign));gGame.campaign[31]=0;
        gGame.replay=sameCache && gCache.rosterValid && gCache.paletteValid;
    }
    AcquireSRWLockExclusive(&gLock);
    if(!Same(gNetwork.identity,identity) || gNetwork.peer!=session.peer || gNetwork.host!=host ||
       gNetwork.ticket!=session.ticket||gNetwork.membership!=session.membershipNonce) {
        gNetwork=Network{};gNetwork.identity=identity;gNetwork.peer=session.peer;gNetwork.host=host;
        gNetwork.ticket=session.ticket;gNetwork.membership=session.membershipNonce;
        gNetwork.diagnosticAt=GetTickCount();gNetwork.channels[RosterPhase-1].expected=true;
    }
    if(!host) {
        if(gCache.rosterValid&&gNetwork.cachedProof[RosterPhase-1].empty()) {
            auto& bytes=gNetwork.cachedProof[RosterPhase-1];bytes.resize(sizeof(Roster));std::memcpy(bytes.data(),&gCache.roster,bytes.size());
        }
        if(gCache.paletteValid&&gNetwork.cachedProof[PalettePhase-1].empty()) {
            auto& bytes=gNetwork.cachedProof[PalettePhase-1];bytes.resize(sizeof(Palette));std::memcpy(bytes.data(),&gCache.palette,bytes.size());
        }
    }
    ReleaseSRWLockExclusive(&gLock);return true;
}
static const char* Name(unsigned phase) {return phase==RosterPhase?"fleet-roster":"fleet-palette";}
static size_t Size(unsigned phase) {return phase==RosterPhase?sizeof(Roster):sizeof(Palette);}
static Envelope Wrap(const coop_checkpoint::Packet& block,unsigned phase,bool host) {
    Envelope packet;packet.identity=gNetwork.identity;packet.phase=static_cast<uint8_t>(phase);
    packet.ticket=gNetwork.ticket;packet.membership=gNetwork.membership;
    packet.senderPlayer=host?1:2;packet.block=block;return packet;
}
static void QueueAck(Channel& channel,const coop_checkpoint::Packet& ack,unsigned phase) {
    const auto packet=Wrap(ack,phase,false);
    // Sender must acknowledge the last Data block before it can accept the
    // final Commit ACK. UI validation must not overwrite that pending ACK.
    if(ack.index==coop_checkpoint::kComplete && channel.ackPending && channel.ack.block.index!=coop_checkpoint::kComplete) {
        channel.finalAck=packet;channel.finalAckPending=true;
    } else if(ack.index!=coop_checkpoint::kComplete && channel.ackPending &&
        channel.ack.block.index!=coop_checkpoint::kComplete && ack.index<channel.ack.block.index) {
        // ACKs represent the validated contiguous prefix; a delayed duplicate
        // must not replace a higher pending ACK from this exact transfer.
    } else {channel.ack=packet;channel.ackPending=true;}
}
static void RejectWire(const coop::Endpoint& endpoint,uint8_t reason) {
    AcquireSRWLockExclusive(&gLock);
    if(endpoint==gNetwork.peer && Valid(gNetwork.identity)) {++gNetwork.rejected;gNetwork.rejectReason=reason;}
    ReleaseSRWLockExclusive(&gLock);
}
static bool SendManifest(unsigned phase,const void* input) {
    // Do not serialize/hash a 100KB palette on every held UI frame once its
    // immutable current-world transfer has started or already completed.
    AcquireSRWLockExclusive(&gLock);auto& existing=gNetwork.channels[phase-1];
    const bool current=Same(gGame.identity,gNetwork.identity) && gNetwork.host;
    if(current) existing.expected=true;
    if(current && (existing.sender.Active() || existing.sender.Done())) {
        const bool complete=existing.sender.Done();ReleaseSRWLockExclusive(&gLock);return complete;
    }
    ReleaseSRWLockExclusive(&gLock);
    std::vector<uint8_t> file(Size(phase));std::memcpy(file.data(),input,file.size());
    AcquireSRWLockExclusive(&gLock);auto& channel=gNetwork.channels[phase-1];
    bool result=Same(gGame.identity,gNetwork.identity) && gNetwork.host;
    if(result) channel.expected=true;
    if(result && !channel.sender.Active() && !channel.sender.Done()) {
        result=channel.sender.Start(gNetwork.identity.session,gNetwork.identity.world,gNetwork.identity.world,
            Name(phase),file,GetTickCount(),phase);
        if(result) channel.digest=coop_checkpoint::Digest(file);
    }
    const bool done=result && channel.sender.Done();ReleaseSRWLockExclusive(&gLock);return done;
}
static bool TakeManifest(unsigned phase,void* output) {
    coop_checkpoint::Completed completed;
    AcquireSRWLockExclusive(&gLock);auto& channel=gNetwork.channels[phase-1];channel.expected=true;
    const bool result=channel.receiver.Take(completed);
    ReleaseSRWLockExclusive(&gLock);
    if(!result || completed.file.size()!=Size(phase)) return false;
    std::memcpy(output,completed.file.data(),completed.file.size());return true;
}
static void AcceptManifest(unsigned phase,bool valid,const void* input) {
    AcquireSRWLockExclusive(&gLock);auto& channel=gNetwork.channels[phase-1];coop_checkpoint::Packet ack;
    if(channel.receiver.Commit(valid,ack)) {QueueAck(channel,ack,phase);channel.complete=true;}
    else if(!valid) {
        std::vector<uint8_t> file(Size(phase));std::memcpy(file.data(),input,file.size());
        ack.session=gNetwork.identity.session;ack.transfer=ack.ticket=gNetwork.identity.world;
        ack.total=static_cast<uint32_t>(file.size());ack.count=static_cast<uint16_t>((file.size()+coop_checkpoint::kBlock-1)/coop_checkpoint::kBlock);
        ack.launchKind=phase;ack.type=coop_checkpoint::Type::Ack;ack.index=kRejected;ack.digest=coop_checkpoint::Digest(file);
        std::memcpy(ack.name,Name(phase),std::strlen(Name(phase))+1);
        channel.ack=Wrap(ack,phase,false);channel.ackPending=true;
    }
    ReleaseSRWLockExclusive(&gLock);
}
static bool ReadArray(uintptr_t& array,uint32_t& count) {
    return ReadGameMemory(gAddresses.array,&array,sizeof(array)) && IsValidUserPtr(array) &&
        ReadGameMemory(gAddresses.count,&count,4) && count>=1 && count<=22;
}
static bool Human(uintptr_t parameter,bool remote) {
    uint32_t type=0;uint8_t id=0,flag=0;
    return ReadGameMemory(parameter+4,&type,4) && type==1 && ReadGameMemory(parameter+1,&id,1) && id==(remote?1:0) &&
        ReadGameMemory(parameter+0x900,&flag,1) && flag==(remote?1:0);
}
static bool TrafficModel(uint32_t model) {
    uintptr_t table=0;uint32_t type=0,index=0;
    return IsSupportedCarModel(model) && ReadGameMemory(gAddresses.table,&table,sizeof(table)) && IsValidUserPtr(table) &&
        ReadGameMemory(table+model*0x890+0x840,&index,4) && index==model &&
        ReadGameMemory(table+model*0x890+0x844,&type,4) && type==2;
}
static bool Portable(const uint8_t* portable) {
    return Empty(portable,0x10) && Empty(portable+0x10+0x38C,8) && Empty(portable+0x10+0x3C0,4) &&
        Empty(portable+0x10+0x57C,4) && Empty(portable+0x10+0x584,12) && Empty(portable+0x8F2,kStride-0x8F2) &&
        coop_car_catalog::PortableModelIndex(Value<uint32_t>(portable,0x10));
}
static bool CapturePortable(uintptr_t parameter,uint8_t* portable) {
    uint8_t probe[kStride]{};
    if(!ReadGameMemory(parameter,probe,sizeof(probe))) return false;
    SerializeVehicleParamPortable(portable,reinterpret_cast<const uint8_t*>(parameter));
    std::memset(portable+0x10+0x3C0,0,4);
    return Portable(portable);
}
static bool BuildRide(uint8_t* output,const uint8_t* input,bool traffic) {
    const uint32_t model=Value<uint32_t>(input,0x10);
    if(!Portable(input) || !IsSupportedCarModel(model) || (traffic && !TrafficModel(model))) return false;
    std::memset(output,0,kStride);auto* ride=output+0x10;const auto* source=input+0x10;
    gApi.ctor(ride);gApi.init(ride,model,traffic);gApi.stock(ride);
    const uint32_t cache=Value<uint32_t>(ride,0x3C0);
    std::memcpy(ride,source,0x38C);std::memcpy(ride+0x394,source+0x394,0x1E8);
    Put(ride,0x3C0,cache);std::memcpy(ride+0x580,source+0x580,4);
    for(uint32_t slot=0;slot<170;++slot) {
        const uint32_t hash=Value<uint32_t>(source,0x590+slot*4);
        if(!hash) {Put<uint32_t>(ride,0x590+slot*4,0);continue;}
        void* part=gApi.part(model,slot,hash);uint32_t actual=0;
        if(!part || !ReadGameMemory(reinterpret_cast<uintptr_t>(part),&actual,4) || actual!=hash) return false;
        std::memcpy(ride+0x590+slot*4,&part,4);
    }
    gApi.parts(ride);
    // 61BCD0 normalizes stock visibility. Restore finalized host visibility
    // afterwards, so hidden/installed wheel and body parts remain identical.
    std::memcpy(ride+0x838,source+0x838,0xAA);
    gApi.copy(output,output); // native embedded-curve/self-pointer repair.
    return true;
}
static bool ValidRoster(const Roster& roster) {
    if(roster.magic!=kRosterMagic || roster.version!=1 || roster.size!=sizeof(roster) ||
        !Same(roster.identity,gGame.identity) || roster.freeRoam>1 || roster.trafficDisabled>1 ||
        roster.ambientCount>kAmbient || roster.trafficCount>kTraffic || (!roster.freeRoam && roster.ambientCount) ||
        (roster.trafficDisabled && roster.trafficCount)) return false;
    for(size_t i=0;i<kAmbient+kTraffic;++i) {
        const auto& entry=roster.entries[i];
        if(i>=static_cast<size_t>(roster.ambientCount)+roster.trafficCount) {
            if(!Empty(&entry,sizeof(entry))) return false;continue;
        }
        const bool ambient=i<roster.ambientCount;
        const uint8_t expected=static_cast<uint8_t>(ambient?100+i:50+i-roster.ambientCount);
        // Native4FE9F0 passes the empty string at783FE5 for traffic drivers.
        // Ambient racers have names, but traffic is not a standings row.
        if(entry.id!=expected || entry.type!=(ambient?2u:3u) || (ambient && !entry.name[0]) || !std::memchr(entry.name,0,16) ||
            !Portable(entry.portable) || !IsSupportedCarModel(Value<uint32_t>(entry.portable,0x10)) ||
            (!ambient && !TrafficModel(Value<uint32_t>(entry.portable,0x10)))) return false;
    }
    return true;
}
static bool NativeAmbientBudget(uint32_t& budget) {
    // Native FreeRoam 5404A0/540780 sets this before 53F000 builds its
    // opponents. Do not accept a front-end count clamp as an empty world.
    return ReadGameMemory(gAddresses.participants+4,&budget,sizeof(budget)) && budget<=kAmbient;
}
static bool CaptureRoster(bool freeRoam) {
    uintptr_t array=0;uint32_t count=0;uint8_t disabled=0;
    if(!ReadArray(array,count) || !Human(array,false) || !ReadGameMemory(gAddresses.disabled,&disabled,1) || disabled>1) return false;
    Roster roster;roster.identity=gGame.identity;roster.freeRoam=freeRoam?1:0;roster.trafficDisabled=disabled;
    // Native factories produce ascending IDs. Sort by ID; array order is not
    // assumed and race opponents never enter this FreeRoam-only ambient list.
    uintptr_t ambient[kAmbient]{},traffic[kTraffic]{};
    for(uint32_t slot=1;slot<count;++slot) {
        uint32_t type=0;uint8_t id=0;
        const auto parameter=array+slot*kStride;
        if(!ReadGameMemory(parameter+4,&type,4) || !ReadGameMemory(parameter+1,&id,1)) return false;
        if(type==1 && !freeRoam && slot==1 && gPendingRaceEvent.parentEpoch && Human(parameter,true)) {
            // Restart/message9 retains the verified second human until Stage4
            // reconstructs the list. It is neither ambient nor native traffic.
            continue;
        } else if(type==2) {
            if(!freeRoam) continue;
            if(id<100 || id>=100+kAmbient || ambient[id-100]) return false;
            ambient[id-100]=parameter;++roster.ambientCount;
        } else if(type==3) {
            if(id<50 || id>=50+kTraffic || traffic[id-50]) return false;
            traffic[id-50]=parameter;++roster.trafficCount;
        } else return false;
    }
    if(freeRoam) {
        uint32_t expected=0;
        if(!NativeAmbientBudget(expected)) return false;
        if(roster.ambientCount!=expected) {
            char message[224]{};_snprintf_s(message,sizeof(message),_TRUNCATE,
                "[FLEET_ASSETS] Native FreeRoam roster incomplete: parameters=%u ambient=%u expected=%u traffic=%u disabled=%u; streaming held.\r\n",
                count,static_cast<unsigned>(roster.ambientCount),expected,static_cast<unsigned>(roster.trafficCount),static_cast<unsigned>(disabled));
            return Hold(message);
        }
    }
    size_t index=0;
    for(unsigned kind=0;kind<2;++kind) {
        const auto total=kind?roster.trafficCount:roster.ambientCount;
        for(unsigned i=0;i<total;++i,++index) {
            const uintptr_t source=kind?traffic[i]:ambient[i];uint8_t native[kStride]{};
            if(!source || !ReadGameMemory(source,native,sizeof(native)) || native[0x900] || Value<uint32_t>(native,0x904)) return false;
            auto& entry=roster.entries[index];entry.id=native[1];entry.flags=native[2];entry.skill=native[0x901];entry.style=native[0x902];
            entry.type=Value<uint32_t>(native,4);std::memcpy(entry.name,native+0x90C,16);
            if(!CapturePortable(source,entry.portable)) return false;
            gApi.ctor(gGame.staged[index]+0x10);gApi.copy(gGame.staged[index],reinterpret_cast<const uint8_t*>(source));
        }
    }
    if(!ValidRoster(roster)) return false;
    gGame.roster=roster;gGame.array=array;return true;
}
static bool StageRoster(const Roster& roster,bool host,bool rebuild=false) {
    uintptr_t array=0;uint32_t count=0;uint8_t disabled=0;
    if(!ValidRoster(roster) || !ReadArray(array,count) || !Human(array,false) ||
        !ReadGameMemory(gAddresses.disabled,&disabled,1) || disabled!=roster.trafficDisabled) return false;
    if(roster.freeRoam) {
        uint32_t expected=0;if(!NativeAmbientBudget(expected) || roster.ambientCount!=expected) return false;
    }
    for(size_t i=0;i<static_cast<size_t>(roster.ambientCount)+roster.trafficCount;++i) {
        const auto& entry=roster.entries[i];const bool traffic=entry.type==3;
        auto* output=gGame.staged[i];
        if((!host || rebuild) && !BuildRide(output,entry.portable,traffic)) return false;
        output[0]=0;output[1]=entry.id;output[2]=entry.flags;
        output[3]=traffic?0:static_cast<uint8_t>(3+i);
        Put(output,4,entry.type);Put<uint32_t>(output,0x8F8,0);
        output[0x900]=!host && !traffic?1:0;output[0x901]=entry.skill;output[0x902]=entry.style;
        Put<uint32_t>(output,0x904,0);Put(output,0x908,Value<uint32_t>(entry.portable,0x10));
        std::memcpy(output+0x90C,entry.name,16);output[0x91C]=0;
        if(!traffic) gApi.texture(output+0x10,static_cast<uint32_t>(3+i));
        gApi.copy(output,output);
    }
    gGame.array=array;return true;
}
struct Catalog {uintptr_t nodes[kPalette]{};uint32_t models[kPalette]{};size_t count=0;};
static bool ReadCatalog(Catalog& catalog) {
    catalog=Catalog{};const auto head=gAddresses.palette+0x900;
    uintptr_t node=0,previous=head,tail=0;uint32_t expected=0;
    if(!ReadGameMemory(head,&node,4) || !ReadGameMemory(head+4,&tail,4) ||
        !ReadGameMemory(gAddresses.palette+0x908,&expected,4) || expected>kPalette) return false;
    while(node!=head) {
        uintptr_t next=0,back=0;uint32_t model=0;
        if(catalog.count>=kPalette || !IsValidUserPtr(node) ||
            !ReadGameMemory(node,&next,4) || !ReadGameMemory(node+4,&back,4) || back!=previous ||
            !ReadGameMemory(node+0x10,&model,4) || !TrafficModel(model)) return false;
        for(size_t i=0;i<catalog.count;++i) if(catalog.nodes[i]==node || catalog.models[i]==model) return false;
        catalog.nodes[catalog.count]=node;catalog.models[catalog.count++]=model;previous=node;node=next;
    }
    return catalog.count==expected && previous==tail;
}
static uintptr_t Node(const Catalog& catalog,uint32_t model) {
    for(size_t i=0;i<catalog.count;++i) if(catalog.models[i]==model) return catalog.nodes[i];return 0;
}
static bool Unleased(uintptr_t node,bool selectedAllowed) {
    uint32_t resource=0,refs=0,active=0,selected=0;
    return ReadGameMemory(node+0x8F8,&resource,4) && !resource && ReadGameMemory(node+0x908,&refs,4) && !refs &&
        ReadGameMemory(node+0x90C,&active,4) && !active && ReadGameMemory(node+0x910,&selected,4) && (selectedAllowed || !selected);
}
static bool ValidPalette(const Palette& palette) {
    if(palette.magic!=kPaletteMagic || palette.version!=1 || palette.size!=sizeof(palette) ||
        !Same(palette.identity,gGame.identity) || palette.paletteCount>kPalette ||
        palette.trafficCount!=gGame.roster.trafficCount) return false;
    uint32_t selected[kPalette]{};
    for(size_t i=0;i<kPalette;++i) {
        const auto& entry=palette.entries[i];
        if(i>=palette.paletteCount) {if(!Empty(&entry,sizeof(entry))) return false;continue;}
        if(!TrafficModel(entry.model) || Value<uint32_t>(entry.portable,0x10)!=entry.model || !Portable(entry.portable)) return false;
        for(size_t j=0;j<i;++j) if(palette.entries[j].model==entry.model) return false;
    }
    for(size_t i=0;i<kTraffic;++i) {
        const auto& choice=palette.traffic[i];
        if(i>=palette.trafficCount) {if(!Empty(&choice,sizeof(choice))) return false;continue;}
        if(choice.id!=50+i) return false;
        bool found=false;
        for(size_t j=0;j<palette.paletteCount;++j) if(palette.entries[j].model==choice.model) {++selected[j];found=true;break;}
        if(!found) return false;
    }
    unsigned unique=0;
    for(size_t i=0;i<palette.paletteCount;++i) {
        if(palette.entries[i].selected!=selected[i]) return false;if(selected[i]) ++unique;
    }
    return unique<=7 && (!palette.trafficCount || palette.paletteCount);
}
static uintptr_t FindParameter(uint32_t type,uint8_t id) {
    uintptr_t array=0;uint32_t count=0;uintptr_t result=0;
    if(!gGame.committed || !ReadArray(array,count) || array!=gGame.array || count!=gGame.committedCount) return 0;
    for(uint32_t i=2;i<count;++i) {
        uint32_t actual=0;uint8_t driver=0;const auto parameter=array+i*kStride;
        if(!ReadGameMemory(parameter+4,&actual,4) || !ReadGameMemory(parameter+1,&driver,1)) return 0;
        if(actual==type && driver==id) {if(result) return 0;result=parameter;}
    }
    return result;
}
static bool CapturePalette() {
    Catalog catalog;if(!ReadCatalog(catalog)) return false;
    Palette palette;palette.identity=gGame.identity;palette.paletteCount=static_cast<uint8_t>(catalog.count);
    palette.trafficCount=gGame.roster.trafficCount;
    for(size_t i=0;i<catalog.count;++i) {
        auto& entry=palette.entries[i];entry.model=catalog.models[i];
        if(!Unleased(catalog.nodes[i],true) || !ReadGameMemory(catalog.nodes[i]+0x910,&entry.selected,4) ||
            !CapturePortable(catalog.nodes[i],entry.portable)) return false;
    }
    for(size_t i=0;i<palette.trafficCount;++i) {
        auto& choice=palette.traffic[i];choice.id=static_cast<uint8_t>(50+i);
        const auto parameter=FindParameter(3,choice.id);
        if(!parameter || !ReadGameMemory(parameter+0x908,&choice.model,4)) return false;
    }
    if(!ValidPalette(palette)) return false;gGame.palette=palette;return true;
}
static bool StagePalette(const Palette& palette) {
    if(!ValidPalette(palette)) return false;
    for(size_t i=0;i<palette.paletteCount;++i) if(!BuildRide(gGame.palettes[i],palette.entries[i].portable,true)) return false;
    return true;
}
static const uint8_t* Expected(uint32_t type,uint8_t id,bool finalAppearance) {
    if(type==2 && id>=100 && id<100+gGame.roster.ambientCount)
        return gGame.roster.entries[id-100].portable;
    if(type!=3 || id<50 || id>=50+gGame.roster.trafficCount) return nullptr;
    if(finalAppearance && gGame.palettePrepared) {
        const auto model=gGame.palette.traffic[id-50].model;
        for(size_t i=0;i<gGame.palette.paletteCount;++i) if(gGame.palette.entries[i].model==model) return gGame.palette.entries[i].portable;
        return nullptr;
    }
    return gGame.roster.entries[gGame.roster.ambientCount+id-50].portable;
}
static bool Matches(uintptr_t parameter,const uint8_t* expected) {
    uint8_t portable[kStride]{};
    return expected && CapturePortable(parameter,portable) &&
        Value<uint32_t>(portable,0x10)==Value<uint32_t>(expected,0x10) &&
        ComputePeerAppearanceFingerprint(portable)==ComputePeerAppearanceFingerprint(expected);
}
static bool LoadedResource(uint32_t resource,const uint8_t* expected) {
    if(!resource) return false;
    const auto head=gAddresses.loader+0x58;uintptr_t node=0,previous=head,tail=0;
    if(!ReadGameMemory(head,&node,4) || !ReadGameMemory(head+4,&tail,4)) return false;
    uintptr_t seen[128]{};size_t count=0;bool found=false;
    while(node!=head) {
        uintptr_t next=0,back=0;uint32_t id=0,state=0;
        if(count>=128 || !IsValidUserPtr(node)) return false;
        for(size_t i=0;i<count;++i) if(seen[i]==node) return false;seen[count++]=node;
        if(!ReadGameMemory(node,&next,4) || !ReadGameMemory(node+4,&back,4) || back!=previous ||
            !ReadGameMemory(node+0x123C,&id,4) || !ReadGameMemory(node+0xC,&state,4)) return false;
        if(id==resource) {
            if(found || state!=2 || !Matches(node+0x10,expected)) return false;
            found=true;
        }
        previous=node;node=next;
    }
    return found && previous==tail;
}
static const uint8_t* LoadedTrafficAppearance(uintptr_t parameter) {
    if(!gGame.paletteApplied || !gGame.assetsReady) return nullptr;
    uint32_t model=0,resource=0;
    if(!ReadGameMemory(parameter+0x10,&model,4) || !ReadGameMemory(parameter+0x8F8,&resource,4)) return nullptr;
    Catalog catalog;if(!ReadCatalog(catalog)) return nullptr;
    for(size_t i=0;i<gGame.palette.paletteCount;++i) {
        const auto& entry=gGame.palette.entries[i];
        if(entry.model!=model) continue;
        const uintptr_t node=Node(catalog,model);uint32_t paletteResource=0;
        return node && Matches(parameter,entry.portable) && Matches(node,entry.portable) &&
            ReadGameMemory(node+0x8F8,&paletteResource,4) && paletteResource==resource &&
            LoadedResource(resource,entry.portable)?entry.portable:nullptr;
    }
    return nullptr;
}
static bool RetainedNativeProof(const coop::SessionSnapshot& session,const WireIdentity& identity) {
    uintptr_t array=0;uint32_t count=0,flow=0;
    if(!IsCoopGameplayActive()||!session.joined||!session.fresh||!session.localRetainsNative||!session.careerLaunched||
       !session.sessionId||session.sessionId!=identity.session||!session.peer.Valid()||
       (session.role!=coop::Role::Host&&session.role!=coop::Role::Client)||
       !coop_fleet_load::SameCampaign(session.campaignName,gGame.campaign)||
       !Same(identity,gGame.identity)||!Same(identity,gCache.identity)||!gGame.roster.freeRoam||identity.race||
       gGame.host!=(session.role==coop::Role::Host)||gCache.host!=gGame.host||!gCache.rosterValid||!gCache.paletteValid||
       !gGame.prepared||!gGame.committed||!gGame.palettePrepared||!gGame.paletteApplied||!gGame.assetsReady||
       !gGame.generation||gGame.generation!=gGame.localGeneration||gGame.localGeneration!=CoopFleetLocalGeneration()||
       !ReadGameMemory(gAddresses.flow,&flow,4)||flow!=6||!ReadArray(array,count)||array!=gGame.array||count!=gGame.committedCount) {
        gGame.retainedProof=false;return false;
    }
    if(gGame.retainedProof&&gGame.retainedTicket==session.ticket&&gGame.retainedMembership==session.membershipNonce&&
       gGame.retainedArray==array&&gGame.retainedCount==count)return true;
    gGame.retainedProof=false;
    if(std::memcmp(&gGame.roster,&gCache.roster,sizeof(Roster))||std::memcmp(&gGame.palette,&gCache.palette,sizeof(Palette)))return false;
    for(size_t i=0;i<size_t(gGame.roster.ambientCount)+gGame.roster.trafficCount;++i) {
        const auto& row=gGame.roster.entries[i];const auto parameter=FindParameter(row.type,row.id);
        const auto* expected=row.type==3?LoadedTrafficAppearance(parameter):Expected(row.type,row.id,true);
        if(!parameter||!expected||!Matches(parameter,expected))return false;
    }
    gGame.retainedProof=true;gGame.retainedTicket=session.ticket;gGame.retainedMembership=session.membershipNonce;
    gGame.retainedArray=array;gGame.retainedCount=count;return true;
}
} // namespace CoopFleetAssetsNative

// The retained side has no new Stage3 callback. Re-publish only its validated
// immutable manifests; never rebuild/commit a live native roster or palette.
bool PrepareCoopRetainedWorldReentry() {
    using namespace CoopFleetAssetsNative;const auto session=CoopSnapshot();coop_world::Identity id{};
    if(!CurrentCoopFleetIdentity(id)||!RetainedNativeProof(session,Wire(id)))return false;
    gCache.peer=session.peer; // Explicit same-world native proof permits a new membership endpoint.
    if(!Publish(id))return false;
    if(gGame.host){SendManifest(RosterPhase,&gGame.roster);SendManifest(PalettePhase,&gGame.palette);}
    return true;
}

void TickCoopFleetAssetsNetwork(DWORD /*sampledBeforeLock*/) {
    using namespace CoopFleetAssetsNative;
    const auto session=CoopSnapshot();std::array<Envelope,coop_fleet_transfer::kWindow> packets{};
    coop::Endpoint peer{};size_t outgoing=0;
    AcquireSRWLockExclusive(&gLock);
    // UI Start() can run after the UDP loop sampled its timestamp, but before
    // this lock. Unsigned Sender timeout arithmetic would expire that NEW
    // transfer immediately. Sample on the same serialized side as Start().
    const DWORD now=GetTickCount();
    if(Valid(gNetwork.identity) && session.joined && session.fresh && session.sessionId==gNetwork.identity.session &&
        session.peer==gNetwork.peer && session.role==(gNetwork.host?coop::Role::Host:coop::Role::Client)&&
        session.ticket==gNetwork.ticket&&session.membershipNonce==gNetwork.membership) {
        peer=gNetwork.peer;
        for(unsigned phase=RosterPhase;phase<=PalettePhase && outgoing<packets.size();++phase) {
            auto& channel=gNetwork.channels[phase-1];
            for(;outgoing<packets.size();) {
                Envelope packet{};bool havePacket=false;
                if(channel.ackPending) {packet=channel.ack;channel.ackPending=false;havePacket=true;}
                else if(channel.finalAckPending) {packet=channel.finalAck;channel.finalAckPending=false;havePacket=true;}
                else if(gNetwork.host) {coop_checkpoint::Packet block;
                    if(channel.sender.Tick(now,block)) {packet=Wrap(block,phase,true);havePacket=true;}}
                if(!havePacket) break;
                ++channel.sent;channel.sentIndex=packet.block.index;channel.chunks=packet.block.count;
                packets[outgoing++]=packet;
            }
        }
        if(now-gNetwork.diagnosticAt>=10000) {
            bool pending=false;for(const auto& channel:gNetwork.channels)
                pending|=channel.expected && !(gNetwork.host?channel.sender.Done():channel.complete);
            if(pending) {
                const auto& roster=gNetwork.channels[RosterPhase-1];const auto& palette=gNetwork.channels[PalettePhase-1];char message[384]{};
                _snprintf_s(message,sizeof(message),_TRUNCATE,
                    "[FLEET_ASSETS] Pending %s world=%u track=%u race=%u; roster active=%u done=%u tx=%u chunk=%u/%u rx=%u chunk=%u; palette active=%u done=%u tx=%u chunk=%u/%u rx=%u chunk=%u; rejects=%u reason=%u sendFail=%u.\r\n",
                    gNetwork.host?"Host":"Client",gNetwork.identity.world,gNetwork.identity.track,gNetwork.identity.race,
                    roster.sender.Active()?1u:0u,(gNetwork.host?roster.sender.Done():roster.complete)?1u:0u,
                    roster.sent,static_cast<unsigned>(roster.sentIndex),static_cast<unsigned>(roster.chunks),roster.received,static_cast<unsigned>(roster.receivedIndex),
                    palette.sender.Active()?1u:0u,(gNetwork.host?palette.sender.Done():palette.complete)?1u:0u,
                    palette.sent,static_cast<unsigned>(palette.sentIndex),static_cast<unsigned>(palette.chunks),palette.received,static_cast<unsigned>(palette.receivedIndex),
                    gNetwork.rejected,static_cast<unsigned>(gNetwork.rejectReason),gNetwork.sendFailures);LogMessage(message);
            }
            gNetwork.diagnosticAt=now;
        }
    }
    ReleaseSRWLockExclusive(&gLock);
    for(size_t i=0;i<outgoing;++i) if(!gApi.send(packets[i],peer)) {
        AcquireSRWLockExclusive(&gLock);
        if(Same(packets[i].identity,gNetwork.identity) && peer==gNetwork.peer) ++gNetwork.sendFailures;
        ReleaseSRWLockExclusive(&gLock);
    }
}
bool ReceiveCoopFleetAssetsPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD /*sampledBeforeLock*/) {
    using namespace CoopFleetAssetsNative;
    uint32_t magic=0;if(!data || bytes<4) return false;std::memcpy(&magic,data,4);if(magic!=kMagic) return false;
    if(bytes!=sizeof(Envelope)) return true;
    Envelope packet;std::memcpy(&packet,data,sizeof(packet));const auto session=CoopSnapshot();
    const coop::Endpoint endpoint{sender.sin_addr.s_addr,ntohs(sender.sin_port)};
    if(sender.sin_family!=AF_INET || !session.joined || !session.fresh || endpoint!=session.peer ||
        packet.version!=2 || packet.size!=sizeof(packet) || packet.phase<RosterPhase || packet.phase>PalettePhase ||
        packet.ticket!=session.ticket||packet.membership!=session.membershipNonce||
        !Valid(packet.identity) || packet.identity.session!=session.sessionId || !coop_checkpoint::Valid(packet.block) ||
        packet.block.session!=packet.identity.session || packet.block.transfer!=packet.identity.world ||
        packet.block.ticket!=packet.identity.world || packet.block.launchKind!=packet.phase ||
        packet.block.total!=Size(packet.phase) || std::strcmp(packet.block.name,Name(packet.phase))) {
        RejectWire(endpoint,1);return true;
    }
    AcquireSRWLockExclusive(&gLock);
    const DWORD now=GetTickCount();
    if(Same(packet.identity,gNetwork.identity) && endpoint==gNetwork.peer && packet.senderPlayer==(gNetwork.host?2:1)&&
       packet.ticket==gNetwork.ticket&&packet.membership==gNetwork.membership) {
        auto& channel=gNetwork.channels[packet.phase-1];
        ++channel.received;channel.receivedIndex=packet.block.index;channel.chunks=packet.block.count;
        if(gNetwork.host && packet.block.type==coop_checkpoint::Type::Ack) {
            if(packet.block.index==kRejected && packet.block.digest==channel.digest && channel.sender.Active()) channel.sender.Reset();
            else channel.sender.Ack(packet.block,now);
        } else if(!gNetwork.host && packet.block.type!=coop_checkpoint::Type::Ack) {
            coop_checkpoint::Packet ack;
            if(channel.receiver.Receive(packet.block,ack)) {
                QueueAck(channel,ack,packet.phase);
                if(!gNetwork.cachedProof[packet.phase-1].empty()) {
                    coop_checkpoint::Completed completed;
                    if(channel.receiver.Take(completed)) {
                        const bool valid=completed.file==gNetwork.cachedProof[packet.phase-1];
                        coop_checkpoint::Packet committed;
                        if(channel.receiver.Commit(valid,committed)){QueueAck(channel,committed,packet.phase);channel.complete=true;}
                        else if(!valid){++gNetwork.rejected;gNetwork.rejectReason=3;}
                    }
                }
            }
            else if(packet.block.type==coop_checkpoint::Type::Data) {++gNetwork.rejected;gNetwork.rejectReason=3;}
        } else {++gNetwork.rejected;gNetwork.rejectReason=2;}
    } else {++gNetwork.rejected;gNetwork.rejectReason=2;}
    ReleaseSRWLockExclusive(&gLock);return true;
}

bool PrepareCoopFleetBeforeStreaming(const coop_world::Identity& id,bool freeRoam) {
    using namespace CoopFleetAssetsNative;
    if(!IsCoopGameplayActive()) return true;
    if(!Publish(id) || freeRoam!=(id.raceEpoch==0) || !VerifyNative()) return Hold("[FLEET_ASSETS] Initial fleet identity/native layout unavailable; streaming held.\r\n");
    if(gGame.host) {
        if(!gGame.prepared) {
            if(gCache.rosterValid) {
                gGame.roster=gCache.roster;
                if(!StageRoster(gGame.roster,true,true)) return Hold("[FLEET_ASSETS] Cached authoritative roster cannot be rebuilt for this native load; streaming held.\r\n");
            } else {
                if(!CaptureRoster(freeRoam) || !StageRoster(gGame.roster,true)) return Hold("[FLEET_ASSETS] Native fleet roster unsupported; streaming held.\r\n");
                gCache.roster=gGame.roster;gCache.rosterValid=true;
            }
            gGame.prepared=true;char message[160]{};_snprintf_s(message,sizeof(message),_TRUNCATE,
                "[FLEET_ASSETS] Host native ambient/traffic roster published: ambient=%u traffic=%u disabled=%u.\r\n",
                static_cast<unsigned>(gGame.roster.ambientCount),static_cast<unsigned>(gGame.roster.trafficCount),static_cast<unsigned>(gGame.roster.trafficDisabled));LogMessage(message);
        }
        const bool acknowledged=SendManifest(RosterPhase,&gGame.roster);
        return acknowledged || !CoopFleetWaitForPeerAssets();
    }
    if(gGame.prepared) return true;
    if(gCache.rosterValid) {
        if(gCache.roster.freeRoam!=(freeRoam?1:0) || !StageRoster(gCache.roster,false))
            return Hold("[FLEET_ASSETS] Cached authoritative roster cannot be rebuilt for this native load; streaming held.\r\n");
        gGame.roster=gCache.roster;gGame.prepared=true;gGame.holdLogged=false;return true;
    }
    Roster roster;if(!TakeManifest(RosterPhase,&roster)) return false;
    const bool valid=roster.freeRoam==(freeRoam?1:0) && StageRoster(roster,false);
    AcceptManifest(RosterPhase,valid,&roster);
    if(!valid) return Hold("[FLEET_ASSETS] Host fleet parts/event traffic mismatch; streaming held.\r\n");
    gGame.roster=roster;gCache.roster=roster;gCache.rosterValid=true;gGame.prepared=true;gGame.holdLogged=false;
    char message[160]{};_snprintf_s(message,sizeof(message),_TRUNCATE,
        "[FLEET_ASSETS] Client authoritative ambient/traffic roster accepted: ambient=%u traffic=%u disabled=%u.\r\n",
        static_cast<unsigned>(roster.ambientCount),static_cast<unsigned>(roster.trafficCount),static_cast<unsigned>(roster.trafficDisabled));LogMessage(message);return true;
}
bool CommitCoopFleetAfterHumans(const coop_world::Identity& id) {
    using namespace CoopFleetAssetsNative;
    if(!IsCoopGameplayActive()) return true;
    uintptr_t array=0;uint32_t count=0;
    if(!Scope(Wire(id)) || !gGame.prepared || !ReadArray(array,count) || array!=gGame.array ||
        !Human(array,false) || !Human(array+kStride,true) || !VerifyNative()) return Hold("[FLEET_ASSETS] Two-human fleet insertion unavailable; streaming held.\r\n");
    if(gGame.committed) return count==gGame.committedCount;
    uint32_t base=2;
    if(!gGame.roster.freeRoam) {
        while(base<count) {
            uint32_t type=0;if(!ReadGameMemory(array+base*kStride+4,&type,4)) return false;
            if(type!=2) break;++base;
        }
        for(uint32_t slot=base;slot<count;++slot) {uint32_t type=0;if(!ReadGameMemory(array+slot*kStride+4,&type,4) || type!=3) return false;}
    } else if(count!=2) return false;
    const uint32_t added=gGame.roster.ambientCount+gGame.roster.trafficCount;
    if(base+added>22) return Hold("[FLEET_ASSETS] Native total capacity exceeded; streaming held.\r\n");
    alignas(16) uint8_t backups[22][kStride]{};uint32_t counters[4]{};
    if(!ReadGameMemory(gAddresses.participants,counters,sizeof(counters))) return false;
    uint8_t probe[kStride]{};
    for(uint32_t i=0;i<(std::max)(count,base+added);++i) {
        if(!ReadGameMemory(array+i*kStride,probe,sizeof(probe))) return false;
        if(i<count) {gApi.ctor(backups[i]+0x10);gApi.copy(backups[i],reinterpret_cast<const uint8_t*>(array+i*kStride));}
    }
    auto rollback=[&]() {
        for(uint32_t i=base;i<count;++i) gApi.copy(reinterpret_cast<uint8_t*>(array+i*kStride),backups[i]);
        SafeWriteBytes(gAddresses.count,&count,4);SafeWriteBytes(gAddresses.participants,counters,sizeof(counters));
    };
    if(!SafeWriteBytes(gAddresses.count,&base,4)) return false;
    for(uint32_t i=0;i<added;++i) {
        gApi.add(gGame.staged[i]);uint32_t actual=0;
        if(!ReadGameMemory(gAddresses.count,&actual,4) || actual!=base+i+1) {rollback();return Hold("[FLEET_ASSETS] Native fleet insertion failed; original roster restored.\r\n");}
    }
    uint32_t finalCounters[4]{};std::memcpy(finalCounters,counters,sizeof(counters));
    if(gGame.roster.freeRoam) {finalCounters[0]=1;finalCounters[1]=gGame.host?gGame.roster.ambientCount:0;
        finalCounters[2]=1;finalCounters[3]=gGame.host?0:gGame.roster.ambientCount;}
    if(!SafeWriteBytes(gAddresses.participants,finalCounters,sizeof(finalCounters))) {rollback();return false;}
    gGame.committedCount=base+added;gGame.committed=true;gGame.holdLogged=false;
    char message[192]{};_snprintf_s(message,sizeof(message),_TRUNCATE,
        "[FLEET_ASSETS] Authoritative ambient/traffic parameters committed outside race seat quota: total=%u ambient=%u traffic=%u.\r\n",
        gGame.committedCount,static_cast<unsigned>(gGame.roster.ambientCount),static_cast<unsigned>(gGame.roster.trafficCount));LogMessage(message);return true;
}
uint32_t CoopFleetCommittedParameterCount() {
    using namespace CoopFleetAssetsNative;return gGame.committed && Scope(gGame.identity)?gGame.committedCount:0;
}
bool CaptureCoopFleetHostSelection(const coop_world::Identity& id) {
    using namespace CoopFleetAssetsNative;
    if(!IsCoopGameplayActive()) return true;
    if(!Scope(Wire(id)) || !gGame.host || !gGame.committed || !VerifyNative()) return false;
    if(gGame.replay) {
        if(!gGame.palettePrepared) {
            if(!gCache.paletteValid || !StagePalette(gCache.palette)) return false;
            gGame.palette=gCache.palette;gGame.palettePrepared=true;
        }
        return ApplyCoopFleetClientSelection(id);
    }
    if(!gGame.palettePrepared) {
        if(!CapturePalette()) return Hold("[FLEET_ASSETS] Native selected palette invalid or leased; Stage6 held.\r\n");
        gGame.palettePrepared=gGame.paletteApplied=true;gGame.holdLogged=false;
        gCache.palette=gGame.palette;gCache.paletteValid=true;
        LogMessage("[FLEET_ASSETS] Native host traffic palette/model selection published.\r\n");
    }
    SendManifest(PalettePhase,&gGame.palette);return true; // Do not rerun native selection while ACK is pending.
}
bool CoopFleetSelectionPlanReady(const coop_world::Identity& id) {
    using namespace CoopFleetAssetsNative;
    if(!IsCoopGameplayActive()) return true;
    if(!Scope(Wire(id)) || !gGame.committed) return false;
    if(gGame.host) return true;
    if(gGame.palettePrepared) return true;
    if(gCache.paletteValid) {
        if(!StagePalette(gCache.palette)) return Hold("[FLEET_ASSETS] Cached host palette parts unavailable; Stage6 held.\r\n");
        gGame.palette=gCache.palette;gGame.palettePrepared=true;gGame.holdLogged=false;return true;
    }
    Palette palette;if(!TakeManifest(PalettePhase,&palette)) return false;
    const bool valid=StagePalette(palette);AcceptManifest(PalettePhase,valid,&palette);
    if(!valid) return Hold("[FLEET_ASSETS] Host selected traffic palette parts unavailable; Stage6 held.\r\n");
    gGame.palette=palette;gCache.palette=palette;gCache.paletteValid=true;gGame.palettePrepared=true;gGame.holdLogged=false;return true;
}
bool PrepareCoopFleetPaletteBeforeStreaming(const coop_world::Identity& id) {
    return CoopFleetSelectionPlanReady(id);
}
bool ApplyCoopFleetClientSelection(const coop_world::Identity& id) {
    using namespace CoopFleetAssetsNative;
    if(!IsCoopGameplayActive()) return true;
    if(!Scope(Wire(id)) || (gGame.host && !gGame.replay) || !gGame.committed || !gGame.palettePrepared || !VerifyNative()) return false;
    if(gGame.paletteApplied) return true;
    Catalog catalog;
    if(!ReadCatalog(catalog) || catalog.count!=gGame.palette.paletteCount) return Hold("[FLEET_ASSETS] Local native palette catalog mismatch; Stage6 held.\r\n");
    uintptr_t targets[kPalette]{},parameters[kTraffic]{};
    // Preflight every entry before any live write. Native Reset412840 must
    // already have retired old-world leases; never overwrite a referenced ride.
    for(size_t i=0;i<gGame.palette.paletteCount;++i) {
        targets[i]=Node(catalog,gGame.palette.entries[i].model);
        if(!targets[i] || !Unleased(targets[i],false)) return Hold("[FLEET_ASSETS] Palette resource/car lease still active; Stage6 held.\r\n");
    }
    for(size_t i=0;i<gGame.palette.trafficCount;++i) {
        parameters[i]=FindParameter(3,gGame.palette.traffic[i].id);
        uint32_t probe=0;if(!parameters[i] || !ReadGameMemory(parameters[i]+0x908,&probe,4)) return false;
    }
    for(size_t i=0;i<gGame.palette.paletteCount;++i) {
        gApi.copyRide(reinterpret_cast<uint8_t*>(targets[i]+0x10),gGame.palettes[i]+0x10);
        const uint32_t selected=gGame.palette.entries[i].selected;
        if(!SafeWriteBytes(targets[i]+0x910,&selected,4)) return false;
    }
    for(size_t i=0;i<gGame.palette.trafficCount;++i)
        if(!SafeWriteBytes(parameters[i]+0x908,&gGame.palette.traffic[i].model,4)) return false;
    gGame.paletteApplied=true;gGame.holdLogged=false;
    LogMessage("[FLEET_ASSETS] Host palette mirrored; native412DB0 owns loading and resource leases.\r\n");return true;
}
bool CoopFleetAssetsReadyForStage7(const coop_world::Identity& id) {
    using namespace CoopFleetAssetsNative;
    if(!IsCoopGameplayActive()) return true;
    if(!Scope(Wire(id)) || !gGame.palettePrepared || !gGame.paletteApplied) return false;
    if(gGame.host && !SendManifest(PalettePhase,&gGame.palette) && CoopFleetWaitForPeerAssets()) return false;
    Catalog catalog;if(!ReadCatalog(catalog)) return false;
    for(size_t i=0;i<gGame.palette.paletteCount;++i) {
        const auto& entry=gGame.palette.entries[i];if(!entry.selected) continue;
        const auto node=Node(catalog,entry.model);uint32_t resource=0,selected=0;
        if(!node || !Matches(node,entry.portable) || !ReadGameMemory(node+0x910,&selected,4) || selected!=entry.selected ||
            !ReadGameMemory(node+0x8F8,&resource,4) || !LoadedResource(resource,entry.portable)) return false;
    }
    return true;
}
bool NotifyCoopFleetAssetsAfterStage7(const coop_world::Identity& id,uint64_t generation) {
    using namespace CoopFleetAssetsNative;
    if(!IsCoopGameplayActive()) return true;
    if(!generation || generation!=gGame.localGeneration || !CoopFleetAssetsReadyForStage7(id)) return false;
    Catalog catalog;if(!ReadCatalog(catalog)) return false;
    for(size_t i=0;i<static_cast<size_t>(gGame.roster.ambientCount)+gGame.roster.trafficCount;++i) {
        const auto& entry=gGame.roster.entries[i];const auto parameter=FindParameter(entry.type,entry.id);
        const auto* expected=Expected(entry.type,entry.id,true);
        if(!parameter || !Matches(parameter,expected)) return false;
        if(entry.type==3) {
            const auto node=Node(catalog,Value<uint32_t>(expected,0x10));uint32_t refs=0,resource=0;
            if(!node || !ReadGameMemory(node+0x908,&refs,4) || !refs ||
                !ReadGameMemory(parameter+0x8F8,&resource,4) || !LoadedResource(resource,expected)) return false;
        }
    }
    gGame.generation=generation;gGame.assetsReady=true;return true;
}
bool CoopFleetAssetsReady(const coop_world::Identity& id) {
    using namespace CoopFleetAssetsNative;return Scope(Wire(id)) && gGame.assetsReady && gGame.generation;
}
bool CoopFleetReservedNativeId(const coop_world::Identity& id,uint32_t kind,uint8_t nativeId) {
    using namespace CoopFleetAssetsNative;
    if(!Scope(Wire(id)) || !gGame.committed) return false;
    return (kind==static_cast<uint32_t>(coop_world::Kind::Traffic) && nativeId>=50 && nativeId<50+gGame.roster.trafficCount) ||
        (kind==static_cast<uint32_t>(coop_world::Kind::AmbientRacer) && nativeId>=100 && nativeId<100+gGame.roster.ambientCount);
}
bool ReadCoopFleetReservation(const coop_world::Identity& identity,uintptr_t parameter,uint32_t& kind,uint8_t& id) {
    using namespace CoopFleetAssetsNative;
    uintptr_t array=0;uint32_t count=0;
    if(!Scope(Wire(identity)) || !gGame.committed || !ReadArray(array,count) ||
        array!=gGame.array || count!=gGame.committedCount) return false;
    const uint32_t added=gGame.roster.ambientCount+gGame.roster.trafficCount;
    if(added>count || count-added<2) return false;
    for(uint32_t i=0;i<added;++i) {
        if(parameter!=array+(count-added+i)*kStride) continue;
        const auto& entry=gGame.roster.entries[i];uint32_t type=0;uint8_t driver=0;
        if(!ReadGameMemory(parameter+4,&type,4) || type!=entry.type ||
            !ReadGameMemory(parameter+1,&driver,1) || driver!=entry.id) return false;
        kind=static_cast<uint32_t>(type==3?coop_world::Kind::Traffic:coop_world::Kind::AmbientRacer);
        id=driver;return true;
    }
    return false;
}
bool ReadCoopFleetParameter(uintptr_t parameter,uint32_t& kind,uint8_t& id,uint32_t& model,uint64_t& appearanceHash) {
    using namespace CoopFleetAssetsNative;
    if(!Scope(gGame.identity) || !gGame.committed) return false;
    uint32_t type=0;uint8_t driver=0;
    if(!ReadGameMemory(parameter+4,&type,4) || !ReadGameMemory(parameter+1,&driver,1) ||
        FindParameter(type,driver)!=parameter) return false;
    const auto* expected=type==3 && gGame.assetsReady?LoadedTrafficAppearance(parameter):Expected(type,driver,gGame.assetsReady);
    if(!Matches(parameter,expected)) return false;
    kind=static_cast<uint32_t>(type==3?coop_world::Kind::Traffic:coop_world::Kind::AmbientRacer);id=driver;
    model=Value<uint32_t>(expected,0x10);appearanceHash=ComputePeerAppearanceFingerprint(expected);return appearanceHash!=0;
}
bool ShouldPreserveCoopFleetRideSetup(uintptr_t ride) {
    using namespace CoopFleetAssetsNative;
    if(!ride || !Scope(gGame.identity) || !gGame.committed) return false;
    uint32_t kind=0,model=0;uint8_t id=0;uint64_t hash=0;
    if(ReadCoopFleetParameter(ride-0x10,kind,id,model,hash)) return true;
    // At420030 the palette has just been copied into the actual parameter but
    // the post-stage observer has not run yet. Allow only its exact manifest.
    uint32_t type=0;uint8_t driver=0;
    return gGame.paletteApplied && ReadGameMemory(ride-0x10+4,&type,4) && type==3 &&
        ReadGameMemory(ride-0x10+1,&driver,1) && FindParameter(type,driver)==ride-0x10 &&
        Matches(ride-0x10,Expected(type,driver,true));
}
bool ReadCoopFleetPalettePaint(uintptr_t ride,uint8_t& paint) {
    using namespace CoopFleetAssetsNative;
    if(!ride || !Scope(gGame.identity) || !gGame.palettePrepared) return false;
    Catalog catalog;if(!ReadCatalog(catalog)) return false;
    for(size_t i=0;i<gGame.palette.paletteCount;++i) {
        if(Node(catalog,gGame.palette.entries[i].model)+0x10==ride) {
            paint=gGame.palette.entries[i].portable[0x17];return true;
        }
    }
    return false;
}

// Both sites are CALLs in the loader, not global engine replacements. Their
// wrappers have the exact native thiscall stack cleanup and leave native
// factories, resource requests and final traffic rebuild ownership intact.
namespace CoopFleetAssetHooks {
struct Sites {
    uintptr_t selection=0x57F996, completed=0x57FA11;
    uintptr_t nativeSelection=0x412BE0, nativeCompleted=0x420030;
};
static Sites sites{};
static bool enabled=false;
static unsigned selectionFailures=0;
static void NativeSelection(uintptr_t manager,uint32_t region) {
    reinterpret_cast<void(__thiscall*)(void*,uint32_t)>(sites.nativeSelection)(reinterpret_cast<void*>(manager),region);
}
static void NativeCompleted(uintptr_t manager) {
    reinterpret_cast<void(__thiscall*)(void*)>(sites.nativeCompleted)(reinterpret_cast<void*>(manager));
}
}
void __fastcall HookedCoopFleetPaletteSelection(void* manager,void*,uint32_t region) {
    using namespace CoopFleetAssetHooks;
    coop_world::Identity identity{};
    if(!enabled || !gCoopFleetEnabled || !IsCoopGameplayActive() ||
        reinterpret_cast<uintptr_t>(manager)!=CoopFleetAssetsNative::gAddresses.palette || !CurrentCoopFleetIdentity(identity)) {
        NativeSelection(reinterpret_cast<uintptr_t>(manager),region);return;
    }
    if(CoopSnapshot().role==coop::Role::Host) {
        if(!CoopFleetAssetsNative::gGame.replay) NativeSelection(reinterpret_cast<uintptr_t>(manager),region);
        if(!CaptureCoopFleetHostSelection(identity)) ++selectionFailures;
    } else if(!ApplyCoopFleetClientSelection(identity)) {
        // Stage7's barrier remains closed. Do not pick independent random
        // models or manufacture a missing local palette on a failed preflight.
        ++selectionFailures;
        LogMessage("[FLEET_ASSETS] Client palette selection refused; Stage7 remains held.\r\n");
    }
}
void __fastcall HookedCoopFleetPaletteCompleted(void* manager,void*) {
    using namespace CoopFleetAssetHooks;
    NativeCompleted(reinterpret_cast<uintptr_t>(manager));
    if(enabled && (gCoopAIEnabled || gCoopFleetEnabled) && IsCoopGameplayActive() &&
        reinterpret_cast<uintptr_t>(manager)==CoopFleetAssetsNative::gAddresses.palette)
        OnCoopFleetNativeStage7Completed();
}
bool InstallCoopFleetAssetsHooks() {
    using namespace CoopFleetAssetHooks;
    if(enabled) return true;
    uint8_t originals[2][5]{},patches[2][5]{};
    const uintptr_t addresses[]{sites.selection,sites.completed};
    const uintptr_t native[]{sites.nativeSelection,sites.nativeCompleted};
    const uintptr_t hooks[]{reinterpret_cast<uintptr_t>(&HookedCoopFleetPaletteSelection),reinterpret_cast<uintptr_t>(&HookedCoopFleetPaletteCompleted)};
    if(!CoopFleetAssetsNative::VerifyNative()) return false;
    for(unsigned i=0;i<2;++i) {
        uint8_t expected[5]{0xE8};const int32_t distance=static_cast<int32_t>(native[i]-(addresses[i]+5));
        std::memcpy(expected+1,&distance,4);
        if(!ReadGameMemory(addresses[i],originals[i],5) || std::memcmp(originals[i],expected,5)) {
            LogMessage("[FLEET_ASSETS] Native Stage6/7 call signatures differ; hooks disabled.\r\n");return false;
        }
        patches[i][0]=0xE8;const int32_t relative=static_cast<int32_t>(hooks[i]-(addresses[i]+5));
        std::memcpy(patches[i]+1,&relative,4);
    }
    DWORD protections[2]{};
    if(!VirtualProtect(reinterpret_cast<void*>(addresses[0]),5,PAGE_EXECUTE_READWRITE,&protections[0])) return false;
    if(!VirtualProtect(reinterpret_cast<void*>(addresses[1]),5,PAGE_EXECUTE_READWRITE,&protections[1])) {
        DWORD unused=0;VirtualProtect(reinterpret_cast<void*>(addresses[0]),5,protections[0],&unused);return false;
    }
    const bool first=SafeWriteBytes(addresses[0],patches[0],5);
    const bool second=first && SafeWriteBytes(addresses[1],patches[1],5);
    if(!second) {
        if(first) SafeWriteBytes(addresses[1],originals[1],5);
        SafeWriteBytes(addresses[0],originals[0],5);
    }
    for(unsigned i=0;i<2;++i) {
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(addresses[i]),5);
        DWORD unused=0;VirtualProtect(reinterpret_cast<void*>(addresses[i]),5,protections[i],&unused);
    }
    enabled=first && second;
    LogMessage(enabled?"[HOOK] Fleet Stage6 palette selection and Stage7 completion observers installed.\r\n":
        "[FLEET_ASSETS] Stage6/7 hook write failed; original calls restored.\r\n");
    return enabled;
}
