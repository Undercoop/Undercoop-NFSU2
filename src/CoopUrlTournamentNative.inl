// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Career U.R.L. keeps its native route records and individual point totals.
// Included after lifecycle/A.I. declarations; all native access is game-thread only.
#include "CoopUrlTournament.h"
#include "CoopRaceLifecycle.h"
uint32_t CoopUrlCurrentRootEpoch();
bool CoopUrlReadCareerOutcome(uint8_t& rank);
bool CoopUrlRequestNextHeat();
bool CoopUrlMayAdvanceNative();
void CoopUrlNativeAdvanced(uint32_t oldIndex,uint32_t nextIndex);
void CoopUrlResultPrepared();
bool ReadCoopUrlHeatContext(coop_url::Context& out);

namespace CoopUrlTournamentNative {
struct Addresses {
    uintptr_t career=0x85AD40,mode=0x850078,flow=0x8654A4,track=0x89E7A0;
    uintptr_t drift=0x89E7D9,coordinator=0x890118,group=0x85036C;
};
static Addresses addresses;
constexpr size_t kHeatStride=0xD24,kResultStride=0x224;
struct Racer {uintptr_t object=0,car=0;uint16_t canonical=0,native=0;};
static bool SameContext(const coop_url::Context& a,const coop_url::Context& b) {
    return !std::memcmp(&a,&b,sizeof(a));
}
template<class T> static bool Read(uintptr_t at,T& value){return ReadGameMemory(at,&value,sizeof(value));}
static uintptr_t Tournament(){return addresses.career+0xD24;}
static uintptr_t Heat(unsigned index){return Tournament()+0x54+index*kHeatStride;}
static bool Base(coop_url::Context& out) {
    uint32_t mode=0,flow=0,track=0,total=0,eventHash=0;uint8_t type=0,drift=0,active=0;
    uintptr_t event=0,route=0;const auto session=CoopSnapshot();const auto state=RaceStateSnapshot();
    if(!IsCoopGameplayActive() || !session.joined || !session.fresh || !state.ready ||
        state.session!=session.sessionId || state.session!=gPendingRaceEvent.sessionId ||
        !state.token || state.token!=gPendingRaceEvent.seed ||
        !Read(addresses.mode,mode) || mode!=1 || !Read(addresses.flow,flow) || flow!=6 ||
        !Read(addresses.drift,drift) || drift || !Read(addresses.track,track) || track==4000 ||
        track!=state.track || track!=gPendingRaceEvent.trackId ||
        !Read(addresses.career+0x8730,event) || !IsValidUserPtr(event) ||
        !Read(event+8,eventHash) || !eventHash || eventHash!=gPendingRaceEvent.careerEventHash ||
        !Read(event+0xF,type) || type!=4 || !Read(Tournament()+1,active) || !active ||
        !Read(Tournament()+0x34E8,total) || total<1 || total>4 ||
        !Read(Tournament()+0x34E4,route) || route<Heat(0) ||
        (route-Heat(0))%kHeatStride || (route-Heat(0))/kHeatStride>=total)return false;
    uint32_t routeTrack=0;if(!Read(route,routeTrack) || routeTrack!=track)return false;
    out={};out.eventHash=eventHash;out.rootEpoch=CoopUrlCurrentRootEpoch();
    if(!out.rootEpoch)out.rootEpoch=state.token;
    out.heatEpoch=state.token;out.track=track;out.index=uint8_t((route-Heat(0))/kHeatStride);out.total=uint8_t(total);
    return true;
}
static bool Grid(coop_url::Context& context,Racer (&racers)[6]) {
    uintptr_t coordinator=0;uint32_t count=0;const auto session=CoopSnapshot();
    if(!Read(addresses.coordinator,coordinator) || !IsValidUserPtr(coordinator) ||
        !Read(coordinator+0x24,count) || count<2 || count>6)return false;
    context.count=uint8_t(count);context.ids[0]=1;context.ids[1]=2;
    uint8_t mask=0;
    for(unsigned i=0;i<count;++i) {
        uintptr_t racer=0,car=0,owner=0,parameter=0;uint32_t type=0;uint16_t id=0;uint8_t paramId=0,remote=0;
        if(!Read(coordinator+0xBD0+i*4,racer) || !IsValidUserPtr(racer) ||
            !Read(racer,car) || !IsValidUserPtr(car) || !Read(car+0x1C,owner) || owner!=racer ||
            !Read(car+0x14,parameter) || !IsValidUserPtr(parameter) || !Read(parameter+4,type) ||
            !Read(car+0xA,id) || !Read(parameter+1,paramId) || id!=paramId || !Read(parameter+0x900,remote))return false;
        unsigned index=6;uint16_t canonical=0;
        if(type==1 && !remote && id==0 && CoopFinishIsLocalRacer(racer)) {
            canonical=session.role==coop::Role::Host?1:2;index=canonical-1;
        } else if(type==1 && remote==1 && id==kPeerRenderPlayerIndex && CoopFinishIsPeerRacer(racer)) {
            canonical=session.role==coop::Role::Host?2:1;index=canonical-1;
        } else if(type==2 && id>=100 && id<105) {
            uint32_t ordinal=0,model=0;
            if(remote!=(session.role==coop::Role::Host?0:1) ||
                !ReadCoopAIRosterParam(parameter,ordinal,model))return false;
            canonical=id;index=2;
            // Native roster planning may retain a protected later source ID.
            // Keep each exact native AI identity, then canonicalize its order.
            while(index<count && racers[index].object)++index;
        } else return false;
        if(index>=count || (index<2 && context.ids[index]!=canonical) || (mask&(1u<<index)))return false;
        for(unsigned previous=0;previous<6;++previous)if(racers[previous].object==racer || racers[previous].car==car)return false;
        racers[index]={racer,car,canonical,id};mask|=uint8_t(1u<<index);
    }
    if(mask!=uint8_t((1u<<count)-1))return false;
    std::sort(racers+2,racers+count,[](const Racer& a,const Racer& b){return a.canonical<b.canonical;});
    for(unsigned i=2;i<count;++i){
        if(i>2 && racers[i].canonical==racers[i-1].canonical)return false;
        context.ids[i]=racers[i].canonical;
    }
    return true;
}
static uint16_t NativeId(uint16_t canonical) {
    if(canonical>=100)return canonical;
    const bool host=CoopSnapshot().role==coop::Role::Host;
    return canonical==(host?1:2)?0:uint16_t(kPeerRenderPlayerIndex);
}
static bool ValidHeat(const coop_url::Heat& heat) {
    if(!coop_url::Valid(heat))return false;uint8_t ranks=0;
    for(unsigned i=0;i<heat.context.count;++i) {
        const auto& row=heat.rows[i];
        if(row.id!=heat.context.ids[i] || row.rank<1 || row.rank>heat.context.count ||
            (ranks&(1u<<(row.rank-1))) || row.status>11 || (i<2 && !row.status))return false;
        ranks|=uint8_t(1u<<(row.rank-1));
    }
    return true;
}
static bool ApplyGroup(uintptr_t group,const coop_url::Heat& heat) {
    uint32_t count=0;if(!Read(group+0xCF0,count) || count!=heat.context.count)return false;
    uintptr_t rows[6]{},sorted[6]{};
    for(unsigned i=0;i<count;++i) {
        uint32_t id=~0u;const uintptr_t row=group+i*kResultStride;
        if(!Read(row+4,id))return false;unsigned found=6;
        for(unsigned j=0;j<count;++j)if(id==NativeId(heat.rows[j].id)){if(found!=6)return false;found=j;}
        if(found==6 || rows[found])return false;rows[found]=row;sorted[heat.rows[found].rank-1]=row;
    }
    for(unsigned i=0;i<count;++i)if(!rows[i] || !sorted[i])return false;
    for(unsigned i=0;i<count;++i) {
        const auto& row=heat.rows[i];const uint32_t rank=row.rank,status=row.status;
        if(!SafeWriteBytes(rows[i],&rank,4) || !SafeWriteBytes(rows[i]+8,&status,4) ||
            !SafeWriteBytes(rows[i]+12,&row.ticks,4))return false;
    }
    return SafeWriteBytes(group+0xCD8,sorted,count*4);
}
static coop_url::Heat applied{};static bool haveApplied=false;
using SuccessFn=bool(__thiscall*)(void*,uint32_t);
using NextAvailableFn=bool(__thiscall*)(void*);
using NextActionFn=void(__thiscall*)(void*,uintptr_t,uint32_t,uint32_t,uint32_t,uint32_t);
static SuccessFn successOriginal=reinterpret_cast<SuccessFn>(0x5287A0);
static NextAvailableFn availableOriginal=reinterpret_cast<NextAvailableFn>(0x513FA0);
static NextActionFn actionOriginal=reinterpret_cast<NextActionFn>(0x4BEC90);
struct PendingAction {bool valid=false;coop_url::Context context{};void* self=nullptr;uintptr_t package=0;uint32_t message=0,a=0,b=0,c=0;};
static PendingAction pending;static coop_url::Context replayed{};static bool haveReplayed=false,replaying=false;
struct InFlightAction {
    bool valid=false;uint64_t session=0;coop_url::Context oldContext{};
    void* self=nullptr;uintptr_t package=0;uint32_t newEpoch=0,newTrack=0;
};
static InFlightAction inFlight;
static bool InFlightDuplicate(void* self,uintptr_t package) {
    if(!inFlight.valid || self!=inFlight.self || package!=inFlight.package)return false;
    const auto session=CoopSnapshot();const auto state=RaceStateSnapshot();uintptr_t selected=0;uint32_t track=0;
    const auto& old=inFlight.oldContext;
    // A queued old PostRace input can outlive its native replay. The new grid
    // is deliberately not ready then, so live Base cannot recognize URL yet.
    // Retain only this already-committed exact action/old->new identity.
    return IsCoopGameplayActive() && session.joined && session.fresh && session.sessionId==inFlight.session &&
        state.session==inFlight.session && !state.ready && state.token==inFlight.newEpoch && state.track==inFlight.newTrack &&
        gPendingRaceEvent.sessionId==inFlight.session && gPendingRaceEvent.careerEventHash==old.eventHash &&
        gPendingRaceEvent.parentEpoch==old.heatEpoch && gPendingRaceEvent.seed==inFlight.newEpoch &&
        gPendingRaceEvent.trackId==inFlight.newTrack && CoopUrlCurrentRootEpoch()==old.rootEpoch &&
        Read(Tournament()+0x34E4,selected) && selected==Heat(old.index+1) &&
        Read(selected,track) && track==inFlight.newTrack;
}
static bool __fastcall Success(void* self,void*,uint32_t accepted) {
    coop_url::Context context{};uint8_t rank=0;
    if(reinterpret_cast<uintptr_t>(self)==Tournament() && Base(context))
        return CoopUrlReadCareerOutcome(rank) && rank==1;
    return successOriginal(self,accepted);
}
static bool __fastcall Available(void* self,void*) {
    coop_url::Context context{};
    if(reinterpret_cast<uintptr_t>(self)==Tournament() && Base(context))
        return context.index+1<context.total && RaceStateSnapshot().BothFinished();
    return availableOriginal(self);
}
static void __fastcall Action(void* self,void*,uintptr_t package,uint32_t message,uint32_t a,uint32_t b,uint32_t c) {
    coop_url::Context context{};
    if(!replaying && message==0x0C407210) {
        if(InFlightDuplicate(self,package))return;
        if(!ReadCoopUrlHeatContext(context)){actionOriginal(self,package,message,a,b,c);return;}
        inFlight={}; // A verified fresh native grid ends the old UI handoff.
        if(haveReplayed && SameContext(replayed,context))return;
        if(context.index+1>=context.total)return;
        if(pending.valid && !SameContext(pending.context,context))pending={};
        if(!pending.valid)pending={true,context,self,package,message,a,b,c};
        CoopUrlRequestNextHeat();return; // Full action retained: no premature pointer increment or RF message10.
    }
    actionOriginal(self,package,message,a,b,c);
}
struct Detour {uintptr_t address;const BYTE* expected;size_t length;void* hook;BYTE* trampoline=nullptr;bool installed=false;};
static constexpr BYTE successBytes[]={0x55,0x8B,0xEC,0x51,0x8B,0xD1};
static constexpr BYTE availableBytes[]={0x8B,0x81,0xE4,0x34,0x00,0x00};
static constexpr BYTE actionBytes[]={0x55,0x8B,0xEC,0x81,0x7D,0x0C,0x10,0x72,0x40,0x0C};
static Detour hooks[]={
    {0x5287A0,successBytes,sizeof(successBytes),reinterpret_cast<void*>(Success)},
    {0x513FA0,availableBytes,sizeof(availableBytes),reinterpret_cast<void*>(Available)},
    {0x4BEC90,actionBytes,sizeof(actionBytes),reinterpret_cast<void*>(Action)}
};
static bool installed=false;
static void Publish(unsigned i,uintptr_t address) {
    if(i==0)successOriginal=reinterpret_cast<SuccessFn>(address);
    else if(i==1)availableOriginal=reinterpret_cast<NextAvailableFn>(address);
    else actionOriginal=reinterpret_cast<NextActionFn>(address);
}
static void Rollback(unsigned i) {
    auto& h=hooks[i];
    if(h.installed){DWORD old=0,unused=0;if(!VirtualProtect(reinterpret_cast<void*>(h.address),h.length,PAGE_EXECUTE_READWRITE,&old))return;
        std::memcpy(reinterpret_cast<void*>(h.address),h.expected,h.length);FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(h.address),h.length);
        VirtualProtect(reinterpret_cast<void*>(h.address),h.length,old,&unused);h.installed=false;}
    Publish(i,h.address);if(h.trampoline)VirtualFree(h.trampoline,0,MEM_RELEASE);h.trampoline=nullptr;
}
}

bool ReadCoopUrlHeatContext(coop_url::Context& out) {
    coop_url::Context context{};CoopUrlTournamentNative::Racer racers[6]{};
    if(!CoopUrlTournamentNative::Base(context) || !CoopUrlTournamentNative::Grid(context,racers))return false;
    out=context;return true;
}
bool IsCoopUrlTournament(){coop_url::Context context{};return CoopUrlTournamentNative::Base(context);}
bool ReconcileCoopUrlParticipants() {
    using namespace CoopUrlTournamentNative;coop_url::Context context{};if(!ReadCoopUrlHeatContext(context))return false;
    uint32_t entries[18]{},old[18]{},oldCount=0;
    if(!Read(Tournament()+0x50,oldCount) || oldCount>6 || !ReadGameMemory(Tournament()+8,old,sizeof(old)))return false;
    for(unsigned i=0;i<context.count;++i){
        const uint32_t native=NativeId(context.ids[i]);
        // The stock constructor sets ID/enabled but leaves +0 untouched.
        // Preserve the whole existing entry by identity; only a new peer gets
        // an initially empty +0. Repeated reconciliation is idempotent.
        for(unsigned prior=0;prior<oldCount;++prior)if(old[prior*3+1]==native){
            std::memcpy(entries+i*3,old+prior*3,12);break;
        }
        entries[i*3+1]=native;entries[i*3+2]=1;
    }
    const uint32_t count=context.count;
    return SafeWriteBytes(Tournament()+8,entries,sizeof(entries)) && SafeWriteBytes(Tournament()+0x50,&count,4);
}
bool CaptureCoopUrlHeat(coop_url::Heat& out) {
    using namespace CoopUrlTournamentNative;coop_url::Heat heat{};Racer racers[6]{};const auto state=RaceStateSnapshot();
    if(CoopSnapshot().role!=coop::Role::Host || !state.BothFinished() || !Base(heat.context) || !Grid(heat.context,racers))return false;
    float progress[6]{};uint8_t nativeRanks[6]{};
    for(unsigned i=0;i<heat.context.count;++i) {
        auto& row=heat.rows[i];row.id=heat.context.ids[i];
        if(i<2){const auto& finish=state.finish[i];row.status=finish.status;row.ticks=finish.ticks;nativeRanks[i]=finish.rank;}
        else if(!Read(racers[i].object+0xA,nativeRanks[i]) || !Read(racers[i].object+0x10,row.status) ||
            !Read(racers[i].object+0x198,row.ticks) || !Read(racers[i].object+0x20,progress[i]) ||
            !std::isfinite(progress[i]) || row.status>11)return false;
        if(nativeRanks[i]<1 || nativeRanks[i]>heat.context.count)return false;
    }
    unsigned order[6]={0,1,2,3,4,5};
    std::stable_sort(order,order+heat.context.count,[&](unsigned a,unsigned b){
        const auto& x=heat.rows[a];const auto& y=heat.rows[b];
        const bool completedA=coop_race::TimedSuccess(x.status,x.ticks),completedB=coop_race::TimedSuccess(y.status,y.ticks);
        if(completedA!=completedB)return completedA;
        if(completedA && x.ticks!=y.ticks)return x.ticks<y.ticks;
        if(!completedA && progress[a]!=progress[b])return progress[a]>progress[b];
        if(nativeRanks[a]!=nativeRanks[b])return nativeRanks[a]<nativeRanks[b];return x.id<y.id;
    });
    for(unsigned rank=0;rank<heat.context.count;++rank)heat.rows[order[rank]].rank=uint8_t(rank+1);
    if(!ValidHeat(heat))return false;out=heat;return true;
}
bool ApplyCoopUrlHeat(const coop_url::Heat& heat) {
    using namespace CoopUrlTournamentNative;coop_url::Context context{};Racer racers[6]{};
    if(!ValidHeat(heat) || !Base(context) || !Grid(context,racers) || !SameContext(context,heat.context))return false;
    for(unsigned i=0;i<context.count;++i){const auto& row=heat.rows[i];const uint8_t rank=row.rank;
        if(!SafeWriteBytes(racers[i].object+0xA,&rank,1) || !SafeWriteBytes(racers[i].object+0xC,&rank,1) ||
            !SafeWriteBytes(racers[i].object+0x10,&row.status,2) || !SafeWriteBytes(racers[i].object+0x198,&row.ticks,4))return false;}
    applied=heat;haveApplied=true;
    // The result group is built later inside EnterResult; an older group is
    // never accepted as a readiness proof merely because its IDs match.
    return true;
}
bool ReadCoopUrlFrozenRacer(uintptr_t racer,uint8_t& rank,uint32_t& ticks,uint16_t& status) {
    using namespace CoopUrlTournamentNative;coop_url::Context context{};Racer racers[6]{};
    if(!haveApplied || !Base(context) || !Grid(context,racers) || !SameContext(context,applied.context))return false;
    for(unsigned i=0;i<context.count;++i)if(racers[i].object==racer) {
        rank=applied.rows[i].rank;ticks=applied.rows[i].ticks;status=applied.rows[i].status;return true;
    }
    return false;
}
bool PrepareCoopUrlResults() {
    using namespace CoopUrlTournamentNative;coop_url::Context context{};
    if(!ReadCoopUrlHeatContext(context) || !haveApplied || !SameContext(context,applied.context))return false;
    return ReconcileCoopUrlParticipants();
}
bool CoopUrlHistoryCopied(uintptr_t destination) {
    using namespace CoopUrlTournamentNative;coop_url::Context context{};
    if(!ReadCoopUrlHeatContext(context) || !haveApplied || !SameContext(context,applied.context) ||
        destination!=Heat(context.index)+0x30)return false;
    return ApplyGroup(destination,applied);
}
bool CoopUrlResultGroupBuilt() {
    using namespace CoopUrlTournamentNative;coop_url::Context context{};
    return ReadCoopUrlHeatContext(context) && haveApplied && SameContext(context,applied.context) && ApplyGroup(addresses.group,applied);
}
void CoopUrlNativePrepared(){if(IsCoopUrlTournament())CoopUrlResultPrepared();}
bool ReplayCoopUrlNextHeat() {
    using namespace CoopUrlTournamentNative;coop_url::Context context{};
    if(!pending.valid || replaying || !CoopUrlMayAdvanceNative() || !ReadCoopUrlHeatContext(context) ||
        !SameContext(context,pending.context) || (haveReplayed && SameContext(replayed,context)))return false;
    const PendingAction action=pending;const auto session=CoopSnapshot();pending={};replayed=context;haveReplayed=true;replaying=true;
    actionOriginal(action.self,action.package,action.message,action.a,action.b,action.c);replaying=false;
    uintptr_t selected=0;Read(Tournament()+0x34E4,selected);
    if(selected!=Heat(context.index+1))return false;
    CoopUrlNativeAdvanced(context.index,context.index+1);
    const auto state=RaceStateSnapshot();uint32_t track=0;
    if(Read(selected,track) && track && gPendingRaceEvent.sessionId==session.sessionId &&
        gPendingRaceEvent.careerEventHash==context.eventHash && gPendingRaceEvent.parentEpoch==context.heatEpoch &&
        gPendingRaceEvent.seed && gPendingRaceEvent.seed!=context.heatEpoch && gPendingRaceEvent.trackId==track &&
        state.session==session.sessionId && state.token==gPendingRaceEvent.seed && state.track==track && !state.ready)
        inFlight={true,session.sessionId,context,action.self,action.package,gPendingRaceEvent.seed,track};
    return true;
}
bool InstallCoopUrlTournamentHooks() {
    using namespace CoopUrlTournamentNative;if(installed)return true;
    for(const auto& h:hooks){BYTE actual[16]{};if(!ReadGameMemory(h.address,actual,h.length) || std::memcmp(actual,h.expected,h.length))return false;}
    for(unsigned i=0;i<3;++i){auto& h=hooks[i];DWORD old=0,unused=0;
        h.trampoline=static_cast<BYTE*>(VirtualAlloc(nullptr,32,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
        if(!h.trampoline || !VirtualProtect(reinterpret_cast<void*>(h.address),h.length,PAGE_EXECUTE_READWRITE,&old)){
            for(unsigned undo=0;undo<3;++undo)Rollback(undo);return false;}
        std::memcpy(h.trampoline,h.expected,h.length);CoopRaceFinishNative::Jump(h.trampoline+h.length,reinterpret_cast<void*>(h.address+h.length));
        Publish(i,reinterpret_cast<uintptr_t>(h.trampoline));CoopRaceFinishNative::Jump(reinterpret_cast<BYTE*>(h.address),h.hook);
        if(h.length>5)std::memset(reinterpret_cast<void*>(h.address+5),0x90,h.length-5);
        FlushInstructionCache(GetCurrentProcess(),h.trampoline,h.length+5);FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(h.address),h.length);
        VirtualProtect(reinterpret_cast<void*>(h.address),h.length,old,&unused);h.installed=true;
    }
    installed=true;LogMessage("[URL] Native cumulative points, participant identities and synchronized next-heat action installed.\r\n");return true;
}
