// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Drift is offline scoring on each real local Player, not a native LAN lobby.
// Stock 56DE1F dereferences a LAN Player for the remote flag; no such object
// exists for our peer or Client AI replicas. Adapt only proven native grid
// replicas; leave local/Host AI scoring and list sort native. Replica scoring
// controllers must not score proxy motion twice, even on the first grid frame.
namespace CoopDriftNative {
static uintptr_t scoreOriginal=0,scoreContinue=0x56DE27;
static uintptr_t tickOriginal=0,localOriginal=0,localContinue=0x56DEAD;
static uintptr_t driftFlagAddress=0x89E7D9,listAddress=0x864630;
static uint64_t scoreSession=0;static uint32_t scoreEpoch=0;
static uintptr_t scoreCar=0;static uint32_t lastScore=0;
static bool installed=false;
// All local/AI/LAN paths branch to56D942. Never detour56D93F across
// that target: those jumps would enter the middle of a five-byte JMP.
static uintptr_t nameOriginal=0,nameContinue=0x56D947;
static uintptr_t nameArrayAddress=0x89E810,nameCountAddress=0x89E814,nameTrackAddress=0x89E7A0;
static uintptr_t resultLapAddress=0x89E7BC;
static uintptr_t finishDurationAddress=0x89E7DC,finishDefaultDurationAddress=0x7A5690;
static uintptr_t participantAddress=0x89E7C0;
static uintptr_t coordinatorAddress=0x890080;
using BuilderFn=void(__cdecl*)();static BuilderFn builderOriginal=reinterpret_cast<BuilderFn>(0x56D850);
static void __cdecl BuilderHook() {
    uint32_t counts[4]{},temporary[4]{};bool changed=false;
    const auto session=CoopSnapshot();uint32_t track=0;
    if(IsCoopGameplayActive() && session.joined && gPendingRaceEvent.sessionId==session.sessionId &&
        gPendingRaceEvent.raceParams[0x39] && ReadGameMemory(nameTrackAddress,&track,4) && track!=4000 &&
        track==gPendingRaceEvent.trackId && ReadGameMemory(participantAddress,counts,sizeof(counts)) &&
        counts[0]==1 && counts[2]==1 && counts[1]+counts[3]<=4 && counts[3] &&
        CoopAIRosterRaceCount()==2+counts[1]+counts[3]) {
        std::memcpy(temporary,counts,sizeof(counts));temporary[1]+=temporary[3];temporary[3]=0;
        changed=SafeWriteBytes(participantAddress,temporary,sizeof(temporary));
    }
    // Native drift score construction reads only C4 for AI. Client replicas
    // normally belong to CC. Present that same roster ONLY inside this builder,
    // then restore actual native local/remote categories even on native unwinding.
    __try {builderOriginal();} __finally {if(changed)SafeWriteBytes(participantAddress,counts,sizeof(counts));}
}
static uintptr_t __cdecl PeerName(uintptr_t parameter,uintptr_t nativePlayer) {
    const auto session=CoopSnapshot();uintptr_t array=0;uint32_t count=0,track=0,type=0;
    uint8_t id=0,remote=0;char name[16]{};
    // This constructor runs during loading, before real Car/Player objects
    // exist. Prove the exact dedicated parameter, not a missing generic LAN player.
    if(nativePlayer || !IsCoopGameplayActive() || !session.joined || !session.sessionId ||
        gPendingRaceEvent.sessionId!=session.sessionId || !gPendingRaceEvent.raceParams[0x39] ||
        !ReadGameMemory(nameArrayAddress,&array,4) || !IsValidUserPtr(array) ||
        !ReadGameMemory(nameCountAddress,&count,4) || count<2 || count>22 || parameter!=array+0x940 ||
        !ReadGameMemory(nameTrackAddress,&track,4) || track==4000 || track!=gPendingRaceEvent.trackId ||
        !ReadGameMemory(parameter+4,&type,4) || type!=1 ||
        !ReadGameMemory(parameter+1,&id,1) || id!=kPeerRenderPlayerIndex ||
        !ReadGameMemory(parameter+0x900,&remote,1) || remote!=1 ||
        !ReadGameMemory(parameter+0x90C,name,sizeof(name)) || !name[0] || !std::memchr(name,0,sizeof(name)))return 0;
    return parameter+0x90C; // Same lifetime/source as native local and AI names.
}
__declspec(naked) static void NameHook() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,528
        and esp,0FFFFFFF0h
        mov [esp+512],eax
        fxsave [esp]
        push dword ptr [eax+28]
        push esi
        call PeerName
        add esp,8
        test eax,eax
        jz original
        mov edx,[esp+512]
        mov [edx],eax
        lea eax,[esi+20h]
        mov [edx+16],eax
        mov [edx+24],eax // displaced MOV ECX,EBX before GetPerformanceTier
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        jmp dword ptr [nameContinue]
    original:
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        jmp dword ptr [nameOriginal]
    }
}
static bool Active() {
    uint8_t drift=0;
    // Career::BuildRaceParams532332 sets mode89E7B0=1 for ALL career
    // events;5323C4 sets byte89E7D9 for type5 drift. Never confuse the
    // proposal's pre-finalization UI label with this actual native flag.
    return CoopFinishRaceActive() && gPendingRaceEvent.raceParams[0x39] &&
        ReadGameMemory(driftFlagAddress,&drift,1) && drift!=0;
}
static bool ScoreEventMatches(const coop_race::State& state,const RaceEventPacket& event) {
    return state.session==event.sessionId && state.token==event.seed && state.track==event.trackId;
}
static const RaceEventPacket& GridEvent() {
    const auto& next=gPendingRaceEvent;
    const auto& previous=gRacePreviousEvent;
    // Receiving a restart proposal replaces the pending epoch while the
    // original score loop still visits the old Cars, including in results.
    if(!next.parentEpoch || next.parentEpoch==next.seed || next.parentEpoch!=previous.seed ||
        !previous.careerEventHash ||
        next.sessionId!=previous.sessionId || next.membershipNonce!=previous.membershipNonce ||
        next.ticket!=previous.ticket || next.trackId!=previous.trackId ||
        next.careerEventHash!=previous.careerEventHash ||
        !(RaceIntentPending() || gCoopRaceState==COOP_RACE_STARTING))return next;
    if(ScoreEventMatches(RaceStateSnapshot(),previous))return previous;
    // START can bind the new lifecycle before native teardown. Its rollback
    // snapshot identifies the old world until the native switch owns it.
    const auto& saved=gRaceLaunchRollback;
    uintptr_t native=0;
    if(saved.valid && saved.session==next.sessionId && saved.epoch==next.seed && saved.track==previous.trackId &&
        saved.membershipNonce==next.membershipNonce && saved.ticket==next.ticket &&
        IsValidUserPtr(saved.nativeFlow) && ScoreEventMatches(saved.lifecycle,previous) &&
        ReadGameMemory(CoopPeerRaceNative::addresses.raceFlow,&native,4) && native==saved.nativeFlow)return previous;
    return next;
}
static bool Grid(uintptr_t& array,uint32_t& count,uintptr_t& coordinator,uint32_t& cars) {
    const auto session=CoopSnapshot();uint32_t flow=0,track=0;uint8_t drift=0;
    const auto& event=GridEvent();
    // Native 608C6A calls the entire drift scoreboard on the FIRST race
    // frame, before lifecycle readiness and before TickCoopAIWorld creates
    // bindings. Replica ownership is a committed native grid property, not
    // a received pose or synchronized-GO property. Never route such a Car
    // through stock 56DE1F's nonexistent LAN Player just because it is early.
    if(!IsCoopGameplayActive() || !session.joined || !session.sessionId ||
        (session.role!=coop::Role::Host && session.role!=coop::Role::Client) ||
        event.sessionId!=session.sessionId || !event.seed || !event.raceParams[0x39] ||
        (&event==&gRacePreviousEvent && (event.membershipNonce!=session.membershipNonce || event.ticket!=session.ticket)) ||
        !ReadGameMemory(gRaceContextAddresses.flow,&flow,4) || flow!=6 ||
        !ReadGameMemory(nameTrackAddress,&track,4) || !track || track==4000 || track!=event.trackId ||
        !ReadGameMemory(driftFlagAddress,&drift,1) || !drift ||
        !ReadGameMemory(nameArrayAddress,&array,4) || !IsValidUserPtr(array) ||
        !ReadGameMemory(nameCountAddress,&count,4) || count<2 || count>22 ||
        !ReadGameMemory(coordinatorAddress,&coordinator,4) || !IsValidUserPtr(coordinator) ||
        !ReadGameMemory(coordinator+0x18,&cars,4) || cars<2 || cars>22)return false;
    return true;
}
static bool ReplicaGrid(uintptr_t car,uintptr_t& parameter) {
    uint32_t count=0,cars=0;uintptr_t array=0,coordinator=0,player=1,ride=0;
    uint8_t remote=0,id=0;uint16_t carId=0;
    if(!IsValidUserPtr(car) || !Grid(array,count,coordinator,cars) ||
        !ReadGameMemory(car+0x14,&parameter,4) || parameter<array || parameter>=array+count*0x940 ||
        (parameter-array)%0x940 || !ReadGameMemory(car+0x10,&ride,4) || ride!=parameter+0x10 ||
        !ReadGameMemory(car+0x18,&player,4) || player ||
        !ReadGameMemory(parameter+0x900,&remote,1) || remote!=1 ||
        !ReadGameMemory(parameter+1,&id,1) || !ReadGameMemory(car+0xA,&carId,2) || carId!=id)return false;
    for(uint32_t i=0;i<cars;++i) {uintptr_t candidate=0;
        if(!ReadGameMemory(coordinator+0x1C+i*4,&candidate,4))return false;
        if(candidate==car)return true;
    }
    return false;
}
static bool Peer(uintptr_t car) {
    uintptr_t param=0,array=0;uint32_t type=0;uint8_t id=0;
    return ReplicaGrid(car,param) && ReadGameMemory(nameArrayAddress,&array,4) && param==array+0x940 &&
        ReadGameMemory(param+4,&type,4) && type==1 &&
        ReadGameMemory(param+1,&id,1) && id==kPeerRenderPlayerIndex;
}
static bool ClientAI(uintptr_t car) {
    uintptr_t param=0;uint32_t ordinal=0,model=0;
    // ReadCoopAIRosterParam proves this exact local parameter against the
    // committed session/event manifest, stable AI ID and model. It does not
    // depend on a world-motion binding being published one frame later.
    const auto session=CoopSnapshot();
    if(session.role!=coop::Role::Client || !ReplicaGrid(car,param))return false;
    const auto& event=GridEvent();
    bool owned=false;
    if(&event==&gRacePreviousEvent) {
        if(!session.joined || !session.peer.Valid() || event.sessionId!=session.sessionId ||
            event.membershipNonce!=session.membershipNonce || event.ticket!=session.ticket)return false;
        owned=ReadCommittedCoopAIParam(param,{event.sessionId,event.seed,event.trackId,event.careerEventHash},
            session.role,ordinal,model);
    } else owned=ReadCoopAIRosterParam(param,ordinal,model);
    return owned && ordinal && ordinal<=5;
}
static bool Bank(uintptr_t car,float& value) {
    uintptr_t racer=0,owner=0,controller=0,bank=0;
    return IsValidUserPtr(car) && ReadGameMemory(car+0x1C,&racer,4) && IsValidUserPtr(racer) &&
        ReadGameMemory(racer,&owner,4) && owner==car && ReadGameMemory(racer+0x1D4,&controller,4) && IsValidUserPtr(controller) &&
        ReadGameMemory(controller,&bank,4) && IsValidUserPtr(bank) && ReadGameMemory(bank+0x2C,&value,4) &&
        std::isfinite(value) && value>=0 && value<=1.0e9f;
}
static coop_race::State GridScoreState(const RaceEventPacket& event) {
    if(&event==&gPendingRaceEvent && !Active())return {};
    const auto state=RaceStateSnapshot();
    if(ScoreEventMatches(state,event))return state;
    return &event==&gRacePreviousEvent && ScoreEventMatches(gRaceLaunchRollback.lifecycle,event) ?
        gRaceLaunchRollback.lifecycle : coop_race::State{};
}
static bool FrozenBank(uintptr_t car,float& value) {
    const auto state=GridScoreState(GridEvent());
    if(!state.token)return false;
    const bool local=car==CoopRaceCar(false),peer=car==CoopRaceCar(true);
    if(!local && !peer)return false;
    const auto& finish=state.finish[local?(state.host?0:1):(state.host?1:0)];
    if(!coop_race::ValidFinish(finish) || !finish.drift)return false;
    value=finish.driftScore;return true;
}
static bool __cdecl IsPeerScore(uintptr_t car,uintptr_t nativePlayer) { return !nativePlayer && (Peer(car) || ClientAI(car)); }
static uint32_t __cdecl PeerScore(uintptr_t car) {
    if(ClientAI(car)) {float value=0;return Bank(car,value)?static_cast<uint32_t>(value):0;}
    float frozen=0;if(FrozenBank(car,frozen))return static_cast<uint32_t>(frozen);
    // Grid ownership is valid before the result lifecycle binds. Its older
    // state must never retain a reused Car's previous-event score on entry.
    const auto& event=GridEvent();
    if(scoreSession!=event.sessionId || scoreEpoch!=event.seed || scoreCar!=car) {
        scoreSession=event.sessionId;scoreEpoch=event.seed;scoreCar=car;lastScore=0;
    }
    RemotePlayerState peer{};
    if(GetRemotePlayerState(peer,false) && peer.current.sessionId==event.sessionId &&
        peer.current.trackId==event.trackId && peer.current.driftEpoch==event.seed &&
        IsValidPacket(peer.current)) lastScore=static_cast<uint32_t>(peer.current.driftScore);
    return lastScore; // Retain bank on packet loss/finish wait; never another event's score.
}
static bool __cdecl IsPeerTick(uintptr_t controller) {
    uintptr_t car=0;
    return IsValidUserPtr(controller) && ReadGameMemory(controller+8,&car,4) && (Peer(car) || ClientAI(car));
}
static void RestoreFrozenScoreRows() {
    const auto state=GridScoreState(GridEvent());
    if(!state.token)return;
    uintptr_t node=0;if(!ReadGameMemory(listAddress,&node,4))return;
    // 56DEA5 is after every native score read but BEFORE the stock score sort
    // and HUD draw. Restore immutable FLOAT banks here, including their native
    // fractions, rather than changing physical controllers or local scoring.
    for(unsigned n=0;node!=listAddress && n<6;++n) {
        uintptr_t next=0;uint32_t id=~uint32_t(0);float score=0;bool frozen=false;
        if(!IsValidUserPtr(node) || !ReadGameMemory(node,&next,4) ||
            !ReadGameMemory(node+8,&id,4))return;
        if(id==0 || id==kPeerRenderPlayerIndex) {
            const auto& finish=state.finish[id==0?(state.host?0:1):(state.host?1:0)];
            if(coop_race::ValidFinish(finish) && finish.drift){score=finish.driftScore;frozen=true;}
        } else if(id>=100 && id<105 && state.driftSealed && (state.driftMask&(1u<<(id-100)))) {
            score=state.driftAi[id-100];frozen=true;
        }
        if(frozen && !SafeWriteBytes(node+0x10,&score,4))return;
        node=next;
    }
}
static uintptr_t __cdecl LocalUiCar() {
    uintptr_t array=0,coordinator=0,local=0;uint32_t count=0,cars=0;
    if(!Grid(array,count,coordinator,cars))return 0;
    RestoreFrozenScoreRows();
    // The native list may already be sorted peer-first before localRoot or
    // result lifecycle is bound. Find the unique real local parameter/Player
    // in this same grid rather than dereferencing the first Car's NULL Player.
    for(uint32_t i=0;i<cars;++i) {
        uintptr_t car=0,parameter=0,ride=0,player=0;uint32_t type=0;uint16_t carId=1;uint8_t remote=1,id=1;
        if(!ReadGameMemory(coordinator+0x1C+i*4,&car,4) || !IsValidUserPtr(car) ||
            !ReadGameMemory(car+0x14,&parameter,4) || parameter!=array ||
            !ReadGameMemory(car+0x10,&ride,4) || ride!=parameter+0x10 ||
            !ReadGameMemory(parameter+4,&type,4) || type!=1 ||
            !ReadGameMemory(parameter+0x900,&remote,1) || remote ||
            !ReadGameMemory(parameter+1,&id,1) || id || !ReadGameMemory(car+0xA,&carId,2) || carId ||
            !ReadGameMemory(car+0x18,&player,4) || !IsValidUserPtr(player))continue;
        if(local)return 0;local=car;
    }
    return local;
}
__declspec(naked) static void ScoreHook() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,528
        and esp,0FFFFFFF0h
        mov [esp+512],eax
        fxsave [esp]
        push dword ptr [eax+28]
        push esi
        call IsPeerScore
        add esp,8
        test al,al
        jz original
        push esi
        call PeerScore
        add esp,4
        mov edx,[esp+512]
        mov [edx+20],eax
        mov eax,dword ptr [listAddress]
        mov eax,[eax]
        mov [edx+28],eax
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        jmp dword ptr [scoreContinue]
    original:
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        jmp dword ptr [scoreOriginal]
    }
}
__declspec(naked) static void TickHook() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,528
        and esp,0FFFFFFF0h
        mov [esp+512],eax
        fxsave [esp]
        push ecx
        call IsPeerTick
        add esp,4
        test al,al
        jz original
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        ret
    original:
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        jmp dword ptr [tickOriginal]
    }
}
__declspec(naked) static void LocalUiHook() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,528
        and esp,0FFFFFFF0h
        mov [esp+512],eax
        fxsave [esp]
        call LocalUiCar
        test eax,eax
        jz original
        mov edx,[esp+512]
        mov [edx+4],eax
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        jmp dword ptr [localContinue]
    original:
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        jmp dword ptr [localOriginal]
    }
}
static constexpr BYTE scoreBytes[]={0x8B,0x50,0x0C,0xA1,0x30,0x46,0x86,0x00};
static constexpr BYTE tickBytes[]={0x51,0x8B,0x41,0x08,0x56};
static constexpr BYTE localBytes[]={0xA1,0x80,0x00,0x89,0x00,0x8B,0x70,0x1C};
static constexpr BYTE nameBytes[]={0x8D,0x5E,0x20,0x8B,0xCB};
using RankFn=uint32_t(__cdecl*)(uint32_t);
static RankFn nativeRank=reinterpret_cast<RankFn>(0x56DD90);
static RankFn rankOriginal=reinterpret_cast<RankFn>(0x56DD90);
using LabelFn=uint32_t(__stdcall*)(void*);
static LabelFn labelOriginal=reinterpret_cast<LabelFn>(0x4AB4B0);
using PrepareFn=void(__thiscall*)(void*,uint32_t);
static PrepareFn prepareOriginal=reinterpret_cast<PrepareFn>(0x54CA60);
static unsigned DriftSlot(const coop_race::State& state,uint32_t carId) {
    if(carId==0)return state.host?0:1;
    if(carId==kPeerRenderPlayerIndex)return state.host?1:0;
    return carId>=100 && carId<105?carId-98:7;
}
static uint32_t __cdecl FinalRank(uint32_t player) {
    const auto state=RaceStateSnapshot();
    if(IsCoopUrlTournament() && (player==0 || player==kPeerRenderPlayerIndex)) {
        uint8_t rank=0;uint32_t ticks=0;uint16_t status=0;
        if(ReadCoopUrlFrozenRacer(CoopRaceRacer(player!=0),rank,ticks,status))return rank;
    }
    if(Active() && ScoreEventMatches(state,gPendingRaceEvent) && state.ResultReady()) {
        const uint8_t rank=state.DriftRank(DriftSlot(state,player));
        if(rank)return rank;
    }
    return rankOriginal(player);
}
static uint32_t __stdcall FinishLabel(void* player) {
    // Stock4AB4B0 queries the live score list as soon as this LOCAL driver
    // crosses. It must not announce Winner while the other driver still scores.
    float score=0;uintptr_t localPlayer=0,racer=0,owner=0;uint16_t status=0;const auto car=CoopRaceCar(false);
    const auto state=RaceStateSnapshot();
    if(IsCoopUrlTournament() && player && car &&
        ReadGameMemory(car+0x18,&localPlayer,4) && localPlayer==reinterpret_cast<uintptr_t>(player) &&
        ReadGameMemory(car+0x1C,&racer,4) && IsValidUserPtr(racer) &&
        ReadGameMemory(racer+0x10,&status,2) && status) {
        uint8_t rank=0;uint32_t ticks=0;uint16_t terminal=0;
        if(!CoopUrlReadHeatResult(rank,ticks,terminal))return 0xE863DD5C;
    }
    // This can run reentrantly inside606CB0, after native status was written
    // but before HookFinish has captured the final bank on return.
    const bool finishing=FrozenBank(car,score) ||
        (Active() && state.session==gPendingRaceEvent.sessionId && state.token==gPendingRaceEvent.seed &&
         state.track==gPendingRaceEvent.trackId && ReadGameMemory(car+0x1C,&racer,4) &&
         IsValidUserPtr(racer) && ReadGameMemory(racer,&owner,4) && owner==car &&
         ReadGameMemory(racer+0x10,&status,2) && status!=0);
    if(player && finishing && ReadGameMemory(car+0x18,&localPlayer,4) &&
        localPlayer==reinterpret_cast<uintptr_t>(player) && !state.ResultReady())
        return 0xE863DD5C; // Original localized "Waiting for final race results...".
    return labelOriginal(player);
}
static void __fastcall PrepareResult(void* object,void*,uint32_t argument) {
    prepareOriginal(object,argument);
    CoopUrlNativePrepared();
    // 54CA60 first copies the individual results, then tests success for stage
    // and sponsor progress. Pay/save the agreed drift receipt at this boundary,
    // before an optional sponsorship popup can postpone the ordinary Continue.
    const auto state=RaceStateSnapshot();
    if(Active() && state.Drift() && state.ResultReady() && CoopFinishSceneScopeMatches() &&
        CoopRaceFinishNative::gCareerHistoryReady && CoopRaceFinishNative::gCareerCommitArmed &&
        !CoopRaceFinishNative::gCareerCommitApplied)
        CoopRaceFinishNative::HookOrdinaryCareerCommit(object,nullptr);
}
static uintptr_t careerRankCall=0x528908;
static BYTE careerRankBytes[5]{};static bool careerRankInstalled=false;
static uint32_t __cdecl CareerRank(uint32_t player) {
    uint8_t rank=0;uint32_t ticks=0;uint16_t status=0;
    // Only the original career success predicate's local-player query.
    // UI standings and individual result rows are not changed to team rank.
    const auto state=RaceStateSnapshot();
    if(player==0 && Active() && state.Drift() && state.ResultReady() &&
        (CoopRaceFinishNative::gCareerCommitArmed || CoopRaceFinishNative::gCareerCommitApplied) &&
        CoopRaceFinishNative::gCareerHistoryReady && CoopFinishSceneScopeMatches() &&
        CoopFinishReadTeamResult(rank,ticks,status))return rank;
    return nativeRank(player);
}
static bool InstallCareerRank() {
    if(careerRankInstalled)return true;
    int32_t relative=0;
    if(!ReadGameMemory(careerRankCall,careerRankBytes,5))return false;
    std::memcpy(&relative,careerRankBytes+1,4);
    if(careerRankBytes[0]!=0xE8 || careerRankCall+5+relative!=reinterpret_cast<uintptr_t>(nativeRank))return false;
    DWORD old=0;if(!VirtualProtect(reinterpret_cast<void*>(careerRankCall),5,PAGE_EXECUTE_READWRITE,&old))return false;
    auto* at=reinterpret_cast<BYTE*>(careerRankCall);at[0]=0xE8;
    relative=static_cast<int32_t>(reinterpret_cast<uintptr_t>(CareerRank)-(careerRankCall+5));std::memcpy(at+1,&relative,4);
    FlushInstructionCache(GetCurrentProcess(),at,5);DWORD ignored=0;VirtualProtect(at,5,old,&ignored);
    careerRankInstalled=true;return true;
}
static void RestoreCareerRank() {
    if(!careerRankInstalled)return;DWORD old=0,ignored=0;
    if(VirtualProtect(reinterpret_cast<void*>(careerRankCall),5,PAGE_EXECUTE_READWRITE,&old)) {
        std::memcpy(reinterpret_cast<void*>(careerRankCall),careerRankBytes,5);
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(careerRankCall),5);
        VirtualProtect(reinterpret_cast<void*>(careerRankCall),5,old,&ignored);careerRankInstalled=false;
    }
}
static bool ResultSlot(const coop_race::State& state,uintptr_t racer,uint16_t& carId,unsigned& slot) {
    if(!state.Drift() || !state.ResultReady() || !IsValidUserPtr(racer))return false;
    uintptr_t car=0,ownedRacer=0,parameter=0;
    if(!ReadGameMemory(racer,&car,4) || !IsValidUserPtr(car) ||
        !ReadGameMemory(car+0x1C,&ownedRacer,4) || ownedRacer!=racer ||
        !ReadGameMemory(car+0xA,&carId,2))return false;
    slot=DriftSlot(state,carId);
    if(carId==0)return CoopFinishIsLocalRacer(racer);
    if(carId==kPeerRenderPlayerIndex)return CoopFinishIsPeerRacer(racer);
    uint32_t ordinal=0,model=0;uint8_t id=0;
    return slot<7 && ReadGameMemory(car+0x14,&parameter,4) && ReadCoopAIRosterParam(parameter,ordinal,model) &&
        ReadGameMemory(parameter+1,&id,1) && id==carId;
}
}
bool CoopFinishReadDriftResult(uintptr_t racer,uint8_t& rank,uint32_t& ticks,uint16_t& status) {
    using namespace CoopDriftNative;
    const auto state=GridScoreState(GridEvent());
    uint16_t id=0;unsigned slot=0;
    if(!ResultSlot(state,racer,id,slot))return false;
    const auto resultRank=state.DriftRank(slot);
    if(!resultRank)return false;
    if(slot<2) {
        ticks=state.finish[slot].ticks;
        status=state.finish[slot].status;
    } else {
        uint8_t nativeRank=0;
        if(!CoopFinishReadManagedAI(racer,nativeRank,ticks,status) &&
            (!ReadGameMemory(racer+0x198,&ticks,4) || !ReadGameMemory(racer+0x10,&status,2)))return false;
    }
    rank=resultRank;
    return true;
}
bool CoopFinishWriteDriftScore(uintptr_t row,uintptr_t racer) {
    using namespace CoopDriftNative;
    const auto state=GridScoreState(GridEvent());
    uint32_t laps=0,rowId=0;uint16_t carId=0;unsigned slot=0;float score=0;
    if(!IsValidUserPtr(row) || !ResultSlot(state,racer,carId,slot) || !state.DriftScore(slot,score) ||
        !ReadGameMemory(resultLapAddress,&laps,4) || laps<1 || laps>20 ||
        !ReadGameMemory(row+4,&rowId,4) || rowId!=carId)return false;
    // The stock serializer only sums this process's individual lap banks.
    // Event Results (4BF454) and its comparator (5032F0) consume the final
    // cumulative slot. Preserve the earlier native lap cells and supply the
    // authenticated total to that exact slot before either consumer runs.
    const uint32_t displayed=static_cast<uint32_t>(static_cast<double>(score)+0.5);
    return SafeWriteBytes(row+0x198+laps*4,&displayed,4) && SafeWriteBytes(row+0x21C,&score,4);
}
namespace CoopDriftNative {
static void UpdateFinishTimer(const coop_race::State& state) {
    if(!state.ready || gCoopRaceState!=COOP_RACE_IN_PROGRESS || !ScoreEventMatches(state,gPendingRaceEvent))return;
    uintptr_t coordinator=0;uint32_t nativeState=0,start=0;
    if(!ReadGameMemory(CoopRaceFinishNative::kCoordinator,&coordinator,4) || !IsValidUserPtr(coordinator) ||
        !ReadGameMemory(coordinator,&nativeState,4) || nativeState!=4 ||
        !ReadGameMemory(coordinator+0x10,&start,4) || !start || start==0x7FFFFFFF)return;
    uint32_t firstTicks=0;
    for(const auto& finish:state.finish) {
        if(coop_race::ValidFinish(finish) && finish.drift && finish.status==1 && finish.ticks &&
            finish.ticks<0x7FFFFFFF && (!firstTicks || finish.ticks<firstTicks))firstTicks=finish.ticks;
    }
    // Stock 606FD1 arms this for a first-place AI or split-screen human.
    // Use the earliest authenticated human crossing in CO-OP. Project its
    // elapsed quarter-ms ticks onto this machine's native race start. Both
    // the HUD (609216) and native timeout finish (607B9D) use these fields.
    struct Timer {uint32_t origin=0;float seconds=0;} old{},next{};
    if(firstTicks) {
        if(!ReadGameMemory(finishDurationAddress,&next.seconds,4) || !std::isfinite(next.seconds) ||
            next.seconds<=0 || next.seconds>300) {
            if(!ReadGameMemory(finishDefaultDurationAddress,&next.seconds,4) || next.seconds!=30.0f)return;
        }
        next.origin=start+firstTicks;
        if(!next.origin || next.origin==0x7FFFFFFF)--next.origin;
    }
    if(ReadGameMemory(coordinator+0x1C,&old,sizeof(old)) && std::memcmp(&old,&next,sizeof(old)))
        SafeWriteBytes(coordinator+0x1C,&next,sizeof(next));
}
}
void TickCoopDriftResults(DWORD flow) {
    using namespace CoopDriftNative;
    const auto state=RaceStateSnapshot();
    if(flow!=6 || !Active())return;
    if(!state.BothFinished())UpdateFinishTimer(state);
    if(!state.host || !state.Drift() || state.driftSealed)return;
    float scores[5]{};uint8_t mask=0;uintptr_t array=0;uint32_t count=0;
    uint32_t raceCount=CoopAIRosterRaceCount();
    if(!ReadGameMemory(nameArrayAddress,&array,4) || !IsValidUserPtr(array) ||
        !ReadGameMemory(nameCountAddress,&count,4) || count<2 || count>22)return;
    if(!raceCount && !gCoopAIEnabled) {
        uint32_t participants[4]{},types[2]{};uint8_t ids[2]{};
        if(!ReadGameMemory(participantAddress,participants,sizeof(participants)) ||
            participants[0]!=1 || participants[1]!=0 || participants[2]!=1 || participants[3]!=0 ||
            !ReadGameMemory(array+4,&types[0],4) || !ReadGameMemory(array+0x940+4,&types[1],4) ||
            !ReadGameMemory(array+1,&ids[0],1) || !ReadGameMemory(array+0x940+1,&ids[1],1) ||
            types[0]!=1 || types[1]!=1 || ids[0]!=0 || ids[1]!=kPeerRenderPlayerIndex)return;
        raceCount=2; // Proven AI-disabled two-human grid; empty bank vector.
    }
    if(raceCount<2 || raceCount>6 || count<raceCount)return;
    for(uint32_t i=2;i<raceCount;++i) {
        const uintptr_t parameter=array+i*0x940;uint32_t ordinal=0,model=0;uint8_t id=0;
        if(!ReadCoopAIRosterParam(parameter,ordinal,model) || !ReadGameMemory(parameter+1,&id,1) || id<100 || id>=105)return;
        uintptr_t node=0;if(!ReadGameMemory(listAddress,&node,4))return;bool found=false;
        for(unsigned n=0;node!=listAddress && n<6;++n) {uint32_t rowId=0;uintptr_t next=0;float score=0;
            if(!IsValidUserPtr(node) || !ReadGameMemory(node+8,&rowId,4) || !ReadGameMemory(node+0x10,&score,4) ||
                !ReadGameMemory(node,&next,4))return;
            if(rowId==id) {if(!std::isfinite(score) || score<0 || score>1.0e9f)return;
                scores[id-100]=score;mask|=uint8_t(1u<<(id-100));found=true;break;}
            node=next;
        }
        if(!found)return;
    }
    AcquireSRWLockExclusive(&gRaceLifecycleLock);const bool sealed=gRaceLifecycle.session==state.session && gRaceLifecycle.token==state.token && gRaceLifecycle.SealDrift(mask,scores);
    ReleaseSRWLockExclusive(&gRaceLifecycleLock);
    if(sealed) {
        char line[176]{};
        std::snprintf(line,sizeof(line),"[DRIFT_RESULT] Final banks sealed: Host=%.2f Client=%.2f AI mask=%02X; awaiting exact peer acknowledgement.\r\n",
            state.finish[0].driftScore,state.finish[1].driftScore,mask);
        LogMessage(line);
    }
}
bool ReadCoopLocalDriftScore(uintptr_t car,float& score,uint32_t& epoch) {
    score=0;epoch=0;uintptr_t racer=0,controller=0,bank=0,owner=0;float value=0;
    // A restart binds its new epoch while the OLD world still exists. Never
    // publish that old bank under the new token before the new native grid.
    if(!CoopDriftNative::Active() || !RaceStateSnapshot().ready || !car || car!=CoopRaceCar(false))return false;
    if(CoopDriftNative::FrozenBank(car,value)) {score=value;epoch=RaceStateSnapshot().token;return true;}
    if(
        !ReadGameMemory(car+0x1C,&racer,4) || !IsValidUserPtr(racer) ||
        !ReadGameMemory(racer,&owner,4) || owner!=car ||
        !ReadGameMemory(racer+0x1D4,&controller,4) || !IsValidUserPtr(controller) ||
        !ReadGameMemory(controller,&bank,4) || !IsValidUserPtr(bank) ||
        !ReadGameMemory(bank+0x2C,&value,4) || !std::isfinite(value) || value<0 || value>2000000000.0f) return false;
    score=value;epoch=RaceStateSnapshot().token;return true;
}
bool InstallCoopDriftHooks() {
    using namespace CoopDriftNative;
    using namespace CoopRaceFinishNative;
    if(installed) return true;
    static Detour hooks[]={
        {0x56DE1F,scoreBytes,sizeof(scoreBytes),reinterpret_cast<void*>(ScoreHook)},
        {0x56DEE0,tickBytes,sizeof(tickBytes),reinterpret_cast<void*>(TickHook)},
        {0x56DEA5,localBytes,sizeof(localBytes),reinterpret_cast<void*>(LocalUiHook)},
        {0x56D942,nameBytes,sizeof(nameBytes),reinterpret_cast<void*>(NameHook)},
        {0x56D850,reinterpret_cast<const BYTE*>("\x55\x8B\xEC\x83\xE4\xF0"),6,reinterpret_cast<void*>(BuilderHook)},
        {0x56DD90,reinterpret_cast<const BYTE*>("\x56\x8B\x35\x30\x46\x86\x00"),7,reinterpret_cast<void*>(FinalRank)},
        {0x4AB4B0,reinterpret_cast<const BYTE*>("\x8B\x44\x24\x04\x8B\x48\x04"),7,reinterpret_cast<void*>(FinishLabel)},
        {0x54CA60,reinterpret_cast<const BYTE*>("\x56\x8B\xF1\x8A\x86\x25\x0D\x00\x00"),9,reinterpret_cast<void*>(PrepareResult)}
    };
    for(const auto& h:hooks) {
        BYTE bytes[16]{};
        if(!ReadGameMemory(h.address,bytes,h.length) || std::memcmp(bytes,h.expected,h.length)) return false;
    }
    if(!InstallCareerRank())return false;
    for(auto& h:hooks) if(!Install(h)) {
        for(auto& previous:hooks) RollBack(previous);
        RestoreCareerRank();
        scoreOriginal=tickOriginal=localOriginal=nameOriginal=0;return false;
    }
    scoreOriginal=reinterpret_cast<uintptr_t>(hooks[0].trampoline);
    tickOriginal=reinterpret_cast<uintptr_t>(hooks[1].trampoline);
    localOriginal=reinterpret_cast<uintptr_t>(hooks[2].trampoline);
    nameOriginal=reinterpret_cast<uintptr_t>(hooks[3].trampoline);
    builderOriginal=reinterpret_cast<BuilderFn>(hooks[4].trampoline);
    rankOriginal=reinterpret_cast<RankFn>(hooks[5].trampoline);
    labelOriginal=reinterpret_cast<LabelFn>(hooks[6].trampoline);
    prepareOriginal=reinterpret_cast<PrepareFn>(hooks[7].trampoline);
    installed=true;
    LogMessage("[DRIFT] Native local drift bank retained; event-scoped peer score and real local HUD adapters installed.\r\n");
    return true;
}
