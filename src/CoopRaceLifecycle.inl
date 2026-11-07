// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Game memory/UI only on the native game thread; network state under this lock.
static SRWLOCK gRaceLifecycleLock=SRWLOCK_INIT;
static coop_race::State gRaceLifecycle;
static std::array<RaceEventPacket,8> gRaceMailbox{};
static size_t gRaceMailboxCount=0;
static bool gRaceLoadLaunched=false, gRaceCheckpointPending=false, gRaceCheckpointSaved=false;
static DWORD gRaceLastEventSend=0,gRaceLastControlSend=0;
static bool gRaceFinishHooksReady=false;
static void* gRaceDeferredNativeSwitch=nullptr;
struct RaceNativeConsentWindow {
    uintptr_t gameFlow=0,nativeFlow=0;
    uint64_t session=0;
    uint32_t epoch=0,track=0;
};
static RaceNativeConsentWindow gRaceNativeConsentWindow;
static CoopRaceState gRaceBeforeInvitation=COOP_RACE_IDLE;
static uint32_t gRaceStableNativeState=0,gRaceStableNativeEpoch=0,gRaceResumeNativeState=7;
static uint32_t gRaceLastHostEpoch=0,gRaceLastPeerRequest=0,gRaceLocalRequest=0;
static uint64_t gRaceIntentSession=0;
static uint32_t gRaceIntentMembership=0,gRaceIntentTicket=0;
static RaceEventPacket gRaceTerminalEvent{};
static RaceEventPacket gRacePreviousEvent{};
static RaceEventPacket gRaceReceivedTerminal{};
static DWORD gRaceTerminalUntil=0;
static uintptr_t gRaceFinalizeLoader=0;
static uint32_t gRaceFinalizeEpoch=0;
static DWORD gRaceFinalizeStarted=0,gRaceFinalizeLastTry=0;
static bool gRaceFinalizeFailed=false;
static const char* gRaceFinalizeReason="none";
static void SendCurrentRaceEvent(uint16_t type);
static coop_race::State RaceStateSnapshot();
static void BindCoopRaceState();
static bool RaceEventCurrent(const RaceEventPacket& p,const coop::SessionSnapshot& session) {
    return session.joined&&session.fresh&&session.membershipNonce&&p.sessionId==session.sessionId&&
        p.membershipNonce==session.membershipNonce&&p.ticket==session.ticket;
}
static bool FlushPendingRaceSave() {
    if(!gRaceCheckpointPending || gRaceCheckpointSaved) return true;
    if(RaceStateSnapshot().session!=CoopSnapshot().sessionId) {
        gRaceCheckpointPending=false;gRaceCheckpointSaved=false;
        LogMessage("[RACE_SAVE] Stale session receipt discarded; it cannot block a new lobby.\r\n");
        return true;
    }
    if(!gProfileContext.IsCoopActive() || !SaveLocalCoopRaceProgress(GetTickCount())) return false;
    gRaceCheckpointSaved=true;gRaceCheckpointPending=false;
    LogMessage("[RACE_SAVE] Pending local reward receipt persisted; next event permitted.\r\n");
    return true;
}
struct RaceLaunchRollback {
    bool valid=false;uint32_t epoch=0;uint64_t session=0;
    uint32_t membershipNonce=0,ticket=0;
    coop_race::State lifecycle;
    uintptr_t nativeFlow=0;uint32_t track=0,resumeState=7;
    bool resultEntered=false,careerArmed=false,careerApplied=false,historyReady=false,countsLogged=false;
    void* appliedObject=nullptr;void* appliedResult=nullptr;uint32_t appliedHash=0;
    uintptr_t resultCoordinator=0,localResultRow=0;
    bool checkpointPending=false,checkpointSaved=false;
    bool localReady=false,peerReady=false,triggered=false,countdownActive=false;
    uint32_t countdownStart=0;DWORD lastControl=0;
    bool peerSpawnLoaded=false;uint64_t localLoadToken=0;
};
static RaceLaunchRollback gRaceLaunchRollback;
using RaceSendFn=void(*)(const void*,size_t,coop::Endpoint);
static RaceSendFn gRaceSend=SendSessionBytes;
static void(*gRaceOverlay)(const char*)=SetOverlayText;
bool CoopRaceHooksReady() { return gRaceFinishHooksReady; }
void PrepareNativeCoopRaceIntent() {
    // Compatibility entry: the launch hook supplies the native GameFlow pointer.
    HoldNativeCoopRaceIntent(reinterpret_cast<void*>(0x865480));
}

namespace CoopPeerRaceNative {
struct Addresses {
    uintptr_t career=0x85AD40, raceFlow=0x8900D8;
    uintptr_t gameFlow=0x8654A4, gameMode=0x850078;
    uintptr_t selectedEvent=0x863470, selectedRoute=0x85EF48;
    uintptr_t track=0x89E7A0, params=0x89E7A0;
    uintptr_t routeList=0x85BAB8, routeCount=0x85EF4C;
    uintptr_t gameFlowObject=0x865480;
    uintptr_t switchReason=0x7FAA08;
};
static Addresses addresses;
using LookupFn=void*(__thiscall*)(void*,uint32_t);
using SelectFn=void(__thiscall*)(void*,void*);
using StateFn=void(__thiscall*)(void*,int);
using MessageFn=void(__thiscall*)(void*,uint32_t,uint32_t);
static LookupFn gLookupEvent=reinterpret_cast<LookupFn>(0x5003F0);
static SelectFn gSelectEvent=reinterpret_cast<SelectFn>(0x541160);
static StateFn gChangeState=reinterpret_cast<StateFn>(0x603630);
static MessageFn gMessage=reinterpret_cast<MessageFn>(0x601020);
using SwitchFn=void(__thiscall*)(void*);
static SwitchFn gSwitchTrack=reinterpret_cast<SwitchFn>(0x5819F0);
// Native pause-menu Restart (actions 3/10) calls this before clearing the
// profile's current event and enqueueing RF0/message9. It is not message16.
static SwitchFn gRestartRace=reinterpret_cast<SwitchFn>(0x603DE0);
}

namespace CoopInterruptedRace {
struct World {
    uintptr_t raceFlow=0, coordinator=0;
    uint32_t track=0, epoch=0, event=0;
    bool exitRequested=false, pauseClosed=false, refusalNotified=false;
};
static World world;
using AbortFn=void(__cdecl*)();
static AbortFn gAbort=reinterpret_cast<AbortFn>(0x004A3DF0);
static uintptr_t gAbortSignature=0x004A3DF0;
// Stock post-race cancellation, as called by 4F3AFB. 513950 clears the
// selected event and pending career result before the deferred RF message11.
static constexpr BYTE kAbortBytes[]={
    0xB9,0x40,0xAD,0x85,0x00,0xE8,0x56,0xFB,0x06,0x00,
    0x8B,0x0D,0xD8,0x00,0x89,0x00,0x6A,0x00,0x6A,0x0B,
    0xE8,0xD7,0x03,0x15,0x00,0xE8,0x92,0x90,0x06,0x00,
    0x50,0xE8,0x4C,0x91,0x08,0x00,0x83,0xC4,0x04,0x84,
    0xC0,0x74,0x0C,0xE8,0x80,0x90,0x06,0x00,0x50,0xE8,
    0x7A,0x3B,0x09,0x00,0x59,0xC3
};

static bool CurrentWorld() {
    using CoopPeerRaceNative::addresses;
    uint32_t flow=0,track=0;
    uintptr_t raceFlow=0,coordinator=0;
    // A transient failed read does not retire the finish guard.
    if (!IsCoopGameplayActive() ||
        (ReadGameMemory(addresses.gameFlow,&flow,4) && flow!=6) ||
        (ReadGameMemory(addresses.track,&track,4) && track!=world.track) ||
        (world.raceFlow && ReadGameMemory(addresses.raceFlow,&raceFlow,4) && raceFlow!=world.raceFlow) ||
        (world.coordinator && ReadGameMemory(CoopRaceFinishNative::kCoordinator,&coordinator,4) && coordinator!=world.coordinator)) {
        world={};
        return false;
    }
    return true;
}

static bool Observe(const coop::SessionSnapshot& session) {
    if (world.track && CurrentWorld()) return true;
    if (!gRaceFinishHooksReady || !IsCoopGameplayActive() || !gRaceIntentSession ||
        !gRaceIntentMembership || !gRaceIntentTicket) return false;
    // A transport outage can leave the same membership alive. Only a retired
    // membership/ticket ends the race, regardless of its freshness bit.
    if (session.joined && session.sessionId==gRaceIntentSession &&
        session.membershipNonce==gRaceIntentMembership && session.ticket==gRaceIntentTicket) return false;
    const auto state=RaceStateSnapshot();
    if (!state.ready || state.session!=gRaceIntentSession || !state.token ||
        !state.track || state.track==4000 || state.careerCommitted) return false;
    const auto& event=gPendingRaceEvent.seed==state.token?gPendingRaceEvent:gRacePreviousEvent;
    if (event.sessionId!=state.session || event.membershipNonce!=gRaceIntentMembership ||
        event.ticket!=gRaceIntentTicket || event.seed!=state.token || event.trackId!=state.track ||
        !event.careerEventHash) return false;
    using CoopPeerRaceNative::addresses;
    uint32_t flow=0,track=0,mode=0;
    if (!ReadGameMemory(addresses.gameFlow,&flow,4) || flow!=6 ||
        !ReadGameMemory(addresses.gameMode,&mode,4) || mode!=1 ||
        !ReadGameMemory(addresses.track,&track,4) || track!=state.track) return false;
    uintptr_t coordinator=0;
    ReadGameMemory(CoopRaceFinishNative::kCoordinator,&coordinator,4);
    // Keep a confirmed result that already produced its native team history.
    // An ACK alone cannot make the unmaterialized result safe to run solo.
    if (state.ResultReady() && coordinator && CoopRaceFinishNative::gResultEntered &&
        CoopRaceFinishNative::gCareerHistoryReady && coordinator==CoopRaceFinishNative::gResultCoordinator) return false;
    world.track=track;world.epoch=state.token;world.event=event.careerEventHash;
    ReadGameMemory(addresses.raceFlow,&world.raceFlow,4);
    world.coordinator=coordinator;
    char line[224]{};
    std::snprintf(line, sizeof(line),
        "[RACE_INTERRUPTED] session=%llu ticket=%u track=%u epoch=%u event=%08X ended before the shared result. Native finish and reward blocked.\r\n",
        static_cast<unsigned long long>(gRaceIntentSession), gRaceIntentTicket, world.track, world.epoch, world.event);
    LogMessage(line);
    return true;
}

static void RequestExit() {
    if (world.exitRequested) return;
    using namespace CoopPeerRaceNative;
    BYTE signature[sizeof(kAbortBytes)]{};
    uintptr_t event=0,liveFlow=0,coordinator=0;
    uint32_t hash=0,nativeState=0;
    const bool verified=!gPendingTrackLoader && IsValidUserPtr(world.raceFlow) && IsValidUserPtr(world.coordinator) &&
        ReadGameMemory(addresses.raceFlow,&liveFlow,4) && liveFlow==world.raceFlow &&
        ReadGameMemory(CoopRaceFinishNative::kCoordinator,&coordinator,4) && coordinator==world.coordinator &&
        ReadGameMemory(addresses.selectedEvent,&event,4) && IsValidUserPtr(event) &&
        ReadGameMemory(event+8,&hash,4) && hash==world.event &&
        ReadGameMemory(gAbortSignature,signature,sizeof(signature)) && !std::memcmp(signature,kAbortBytes,sizeof(signature));
    if (verified && ReadGameMemory(liveFlow,&nativeState,4)) {
        uint32_t saved=0,reason=0;
        if (nativeState==6 && ReadGameMemory(liveFlow+0x24,&saved,4) && saved==4 &&
            ReadGameMemory(liveFlow+0x4C,&reason,4) && reason==7) {
            // Close the ordinary native Pause overlay before queuing its exit.
            if (!world.pauseClosed) {
                world.pauseClosed=true;
                gChangeState(reinterpret_cast<void*>(liveFlow),4);
            }
            return;
        }
        if (nativeState==4) {
            world.exitRequested=true;
            CoopAnnouncements::Post("Teammate disconnected. Race cancelled. Reconnect from the main menu.");
            LogMessage("[RACE_INTERRUPTED] Verified native race cancellation queued; returning to the main menu.\r\n");
            gAbort();
            return;
        }
    }
    if (!world.refusalNotified) {
        world.refusalNotified=true;
        CoopAnnouncements::Post("Teammate disconnected. Race cancelled. Return to the main menu to reconnect.");
        LogMessage("[RACE_INTERRUPTED] Native exit proof unavailable. Finish stays blocked until the old world exits.\r\n");
    }
}
} // namespace CoopInterruptedRace

bool CoopFinishRaceInterrupted() {
    return CoopInterruptedRace::Observe(CoopSnapshot());
}

bool PopulateNativeCoopRaceProposal() {
    using namespace CoopPeerRaceNative;
    uintptr_t event=0;uint32_t hash=0,track=0;
    gRaceFinalizeReason="selected-event";
    if(!ReadGameMemory(addresses.selectedEvent,&event,sizeof(event)) || !IsValidUserPtr(event) ||
        !ReadGameMemory(event+8,&hash,4) || hash!=gPendingRaceEvent.careerEventHash) return false;
    gRaceFinalizeReason="track";
    if(!ReadGameMemory(addresses.track,&track,4) || track!=gPendingRaceEvent.trackId) return false;
    // A failed read must not partially mutate the approved packet.
    auto p=gPendingRaceEvent;gRaceFinalizeReason="parameters";
    if(!(ReadGameMemory(addresses.params+4,&p.direction,4) && ReadGameMemory(addresses.params+0x1C,&p.laps,4) &&
        ReadGameMemory(addresses.params+0x10,&p.gameMode,4) &&
        ReadGameMemory(addresses.gameMode,p.nativeContext,sizeof(p.nativeContext)) &&
        ReadGameMemory(addresses.params,p.raceParams,sizeof(p.raceParams)))) return false;
    if(p.direction>1 || !p.laps || p.laps>99) return false;
    gPendingRaceEvent=p;gRaceFinalizeReason="none";return true;
}
static bool SelectAuthoritativeNativeRoute() {
    using namespace CoopPeerRaceNative;
    if(!gPendingRaceEvent.trackId || gPendingRaceEvent.trackId==4000 || !gPendingRaceEvent.laps ||
        gPendingRaceEvent.direction>1) return false;
    const uint32_t desired[]={gPendingRaceEvent.trackId,gPendingRaceEvent.direction,gPendingRaceEvent.laps};
    uintptr_t route=0;uint32_t value[3]{};
    if(ReadGameMemory(addresses.selectedRoute,&route,sizeof(route)) && IsValidUserPtr(route) &&
        ReadGameMemory(route,value,sizeof(value)) && !std::memcmp(value,desired,sizeof(value))) return true;
    uint32_t count=0;
    if(!ReadGameMemory(addresses.routeCount,&count,4) || !count || count>4) return false;
    for(uint32_t i=0;i<count;++i) {
        route=addresses.routeList+i*0xD24;
        if(ReadGameMemory(route,value,sizeof(value)) && !std::memcmp(value,desired,sizeof(value)))
            return SafeWriteBytes(addresses.selectedRoute,&route,sizeof(route));
    }
    return false;
}
bool CoopUrlRestartHasRetainedContext();
bool StartNativePeerRace() {
    using namespace CoopPeerRaceNative;
    const auto session=CoopSnapshot();
    uint32_t gameFlow=0,gameMode=0;
    if(CoopSpecialBusy()||!CoopSpecialBothCarsForRace(gPendingRaceEvent.careerEventHash))return false;
    if(!IsCoopGameplayActive() || !session.joined || !session.fresh ||
        !ReadGameMemory(addresses.gameFlow,&gameFlow,4) || gameFlow!=6 ||
        !ReadGameMemory(addresses.gameMode,&gameMode,4) || gameMode!=1 ||
        !RaceEventCurrent(gPendingRaceEvent,session) || !gPendingRaceEvent.careerEventHash) return false;
    void* career=reinterpret_cast<void*>(addresses.career);
    void* event=gLookupEvent(career,gPendingRaceEvent.careerEventHash);
    uint32_t hash=0;uintptr_t flow=0;uint32_t state=0;
    if(!IsValidUserPtr(reinterpret_cast<uintptr_t>(event)) ||
        !ReadGameMemory(reinterpret_cast<uintptr_t>(event)+8,&hash,4) || hash!=gPendingRaceEvent.careerEventHash ||
        !ReadGameMemory(addresses.raceFlow,&flow,sizeof(flow)) || !IsValidUserPtr(flow) ||
        !ReadGameMemory(flow,&state,4) || state>7) return false;
    // Native 499B10 binds the local event before leaving the old world. Keep
    // the old WorldEvent/RaceParameters intact while its HUD still exists:
    // 581820 first tears it down, then 5322E0 builds this event's parameters.
    // The reported 4C66F9 null HUD read occurred with those premature copies.
    // Selecting again reconstructs every U.R.L. heat and erases points. A
    // proven same-event restart retains the existing selected heat/history.
    const bool retained=CoopUrlRetainsNativeEvent();
    if(!retained&&CoopUrlRestartHasRetainedContext())return false;
    if(!retained)gSelectEvent(career,event);
    if(!SelectAuthoritativeNativeRoute()) return false;
    if(retained) {
        // A post-race Return producer can already have consumed +8730 while
        // its shared movie/continuation is waiting. The native route/heat and
        // individual totals are still owned by this exact parent epoch, so
        // selecting541160 again would erase them. Restore ONLY its canonical
        // event pointer: 581915 otherwise sees null and builds Free Roam4000.
        uintptr_t selected=0;
        if(!ReadGameMemory(addresses.selectedEvent,&selected,sizeof(selected)) ||
            (selected&&selected!=reinterpret_cast<uintptr_t>(event))) return false;
        if(!selected && !SafeWriteBytes(addresses.selectedEvent,&event,sizeof(event))) return false;
        if(!selected)LogMessage("[RACE_RESTART] Retained native event rebound after post-race Return; heat/points preserved.\r\n");
    }
    if(gPendingRaceEvent.parentEpoch) {
        CoopMovieRaceRestartCommitted(session.sessionId,gPendingRaceEvent.parentEpoch,
            gPendingRaceEvent.seed,gPendingRaceEvent.careerEventHash);
        gRestartRace(reinterpret_cast<void*>(flow));
        LogMessage("[RACE_RESTART] Approved local restart entered native Restart/message9.\r\n");
        return true;
    }
    if(state!=0) gChangeState(reinterpret_cast<void*>(flow),0);
    // Native message16 sets the launch reason and performs exactly one switch.
    gMessage(reinterpret_cast<void*>(flow),16,0);
    LogMessage("[RACE_LOAD] Local event launch requested; old HUD parameters retained until native teardown.\r\n");
    return true;
}

static void ResetRaceIntentSession(uint64_t session) {
    const auto live=CoopSnapshot();
    const uint32_t membership=live.sessionId==session?live.membershipNonce:0;
    const uint32_t ticket=live.sessionId==session?live.ticket:0;
    if(gRaceIntentSession==session&&gRaceIntentMembership==membership&&gRaceIntentTicket==ticket) return;
    // Keep the old native world proof before this resets the network race.
    CoopInterruptedRace::Observe(live);
    // A new joined session cannot retire an older held native world before
    // that world's pending consent has returned through its native handler.
    if(gRaceNativeConsentWindow.epoch) CancelCoopRaceInvitation(false);
    gRaceLastHostEpoch=0;gRaceLastPeerRequest=0;gRaceLocalRequest=0;
    gRaceDeferredNativeSwitch=nullptr;gRaceTerminalUntil=0;gRaceTerminalEvent={};gRaceReceivedTerminal={};
    gRaceNativeConsentWindow={};
    gRaceStableNativeState=0;gRaceStableNativeEpoch=0;gRaceResumeNativeState=7;
    gRaceLaunchRollback={};gRaceFinalizeLoader=0;gRaceFinalizeEpoch=0;gRaceFinalizeFailed=false;
    gCoopRaceState=COOP_RACE_IDLE;gRaceBeforeInvitation=COOP_RACE_IDLE;
    gPendingRaceEvent={};gRacePreviousEvent={};gRaceProposalPrepared=false;gRaceLoadLaunched=false;gTriggerRaceLoading=false;
    gRaceCheckpointPending=false;gRaceCheckpointSaved=false;
    AcquireSRWLockExclusive(&gRaceLifecycleLock);
    gRaceIntentSession=session;gRaceIntentMembership=membership;gRaceIntentTicket=ticket;
    gRaceLifecycle={};
    // New-generation bytes can arrive before this UI tick. Preserve those;
    // only retire invitations queued for the abandoned membership/career.
    size_t kept=0;for(size_t i=0;i<gRaceMailboxCount;++i)
        if(RaceEventCurrent(gRaceMailbox[i],live))gRaceMailbox[kept++]=gRaceMailbox[i];
    gRaceMailboxCount=kept;
    ReleaseSRWLockExclusive(&gRaceLifecycleLock);
    ResetCountdownSyncState();
}
static void ObserveNativeRaceResumeState() {
    using namespace CoopPeerRaceNative;
    if(gCoopRaceState!=COOP_RACE_IN_PROGRESS) return;
    const auto race=RaceStateSnapshot();uint32_t flow=0,state=0;uintptr_t native=0;
    if(race.token && ReadGameMemory(addresses.gameFlow,&flow,4) && flow==6 &&
        ReadGameMemory(addresses.raceFlow,&native,sizeof(native)) && IsValidUserPtr(native) &&
        ReadGameMemory(native,&state,4) && (state==4 || state==7)) {
        gRaceStableNativeState=state;gRaceStableNativeEpoch=race.token;
    }
}
static uint32_t NextRaceNonce(uint32_t previous) {
    const uint32_t clock=GetTickCount()|1u;
    uint32_t next=(!previous || static_cast<int32_t>(clock-previous)>0)?clock:previous+1;
    return next?next:1;
}
static bool NativeEventDescription(void* event,RaceEventPacket& p,bool selectedRoute) {
    using namespace CoopPeerRaceNative;
    const uintptr_t address=reinterpret_cast<uintptr_t>(event);
    int16_t track=0;uint8_t direction=0,laps=0,type=0;
    if(!IsValidUserPtr(address) || !ReadGameMemory(address+8,&p.careerEventHash,4) || !p.careerEventHash ||
        !ReadGameMemory(address+0x18,&track,2) || track<=0 || track==4000 ||
        !ReadGameMemory(address+0x1A,&direction,1) || !ReadGameMemory(address+0x1B,&laps,1) ||
        !ReadGameMemory(address+0x0F,&type,1)) return false;
    p.trackId=static_cast<uint32_t>(track);p.direction=direction&1u;p.laps=laps;
    // Native Career type is only a label until 5322E0 finalizes RaceParameters.
    static const uint32_t modes[]={2,3,4,9,8,5};
    p.gameMode=type<6?modes[type]:2;
    p.timedResult=type<=3;
    uintptr_t route=0;uint32_t nativeRoute[3]{};
    if(selectedRoute && ReadGameMemory(addresses.selectedRoute,&route,sizeof(route)) && IsValidUserPtr(route) &&
        ReadGameMemory(route,nativeRoute,sizeof(nativeRoute)) && nativeRoute[0] && nativeRoute[0]!=4000) {
        p.trackId=nativeRoute[0];p.direction=nativeRoute[1];p.laps=nativeRoute[2];
    }
    return p.laps>0 && p.laps<=99 && p.direction<=1;
}
static bool HoldNativeCoopRaceIntentImpl(void* gameFlow,bool nativeRestart) {
    using namespace CoopPeerRaceNative;
    const auto session=CoopSnapshot();uint32_t flow=0,track=0,mode=0,reason=0,state=0;uintptr_t event=0,raceFlow=0;
    if(reinterpret_cast<uintptr_t>(gameFlow)!=addresses.gameFlowObject || !IsCoopGameplayActive() ||
        !CoopRaceHooksReady() || !session.joined || !session.fresh ||
        !ReadGameMemory(addresses.gameFlow,&flow,4) || flow!=6 ||
        !ReadGameMemory(addresses.gameMode,&mode,4) || mode!=1 ||
        (!nativeRestart && (!ReadGameMemory(addresses.switchReason,&reason,4) || (reason!=16 && reason!=9))) ||
        !ReadGameMemory(addresses.raceFlow,&raceFlow,sizeof(raceFlow)) || !IsValidUserPtr(raceFlow) ||
        !ReadGameMemory(raceFlow,&state,4) || state>7 ||
        !ReadGameMemory(addresses.track,&track,4) ||
        !ReadGameMemory(addresses.selectedEvent,&event,sizeof(event))) return false;
    if(CoopSpecialBusy()){
        gRaceOverlay("Finish or cancel the shared special event before entering a race.");return true;
    }
    ResetRaceIntentSession(session.sessionId);
    if(!FlushPendingRaceSave()) {
        gRaceOverlay("[ CO-OP ] Saving your race reward. Please try again shortly.");
        return true; // Never destroy a rewarded world before its own save succeeds.
    }
    // START owns the one permitted switch. Re-entrant native messages while
    // consent is pending are consumed without scheduling another world teardown.
    if(gCoopRaceState==COOP_RACE_STARTING) return false;
    if(gCoopRaceState==COOP_RACE_HOST_WAITING_ACCEPT || gCoopRaceState==COOP_RACE_REQUESTING ||
        gCoopRaceState==COOP_RACE_CLIENT_INVITED) return true;
    const auto old=RaceStateSnapshot();
    const bool restartCommand=nativeRestart || reason==9;
    const bool restarting=restartCommand && gCoopRaceState==COOP_RACE_IN_PROGRESS && old.session==session.sessionId && old.token && track==old.track;
    if((nativeRestart || reason==9) && !restarting) return false;
    if(track!=4000 && !restarting) return false;
    RaceEventPacket p{};
    if(restarting) {
        // The native restart resets selection before BeginSwitch; the already
        // finalized epoch is the race being restarted, not a stale menu pointer.
        if(gPendingRaceEvent.sessionId!=old.session || gPendingRaceEvent.seed!=old.token ||
            gPendingRaceEvent.trackId!=track || !gPendingRaceEvent.careerEventHash) return false;
        p=gPendingRaceEvent;p.requestId=0;
    } else if(!NativeEventDescription(reinterpret_cast<void*>(event),p,true)) return false;
    if(!CoopSpecialBothCarsForRace(p.careerEventHash)){
        gRaceOverlay("Both players must be driving an SUV for this event.");return true;
    }
    ResetRaceIntentSession(session.sessionId);
    p.magic=kRaceEventMagic;p.version=kRacePacketVersion;p.sessionId=session.sessionId;
    p.membershipNonce=session.membershipNonce;p.ticket=session.ticket;
    p.parentEpoch=restarting?old.token:0;p.senderPlayer=session.role==coop::Role::Host?1:2;
    gRacePreviousEvent=gPendingRaceEvent;
    gRaceBeforeInvitation=restarting?COOP_RACE_IN_PROGRESS:COOP_RACE_IDLE;
    // BeginSwitch arrives in RF0. Retain the last native gameplay/result state
    // for this epoch, not a loading state or the old Free Roam epoch.
    gRaceResumeNativeState=!restarting?7:(gRaceStableNativeEpoch==old.token?
        gRaceStableNativeState:(old.ResultReady()?7:4));
    uint32_t pausedState=0;
    if(nativeRestart && state==6 && ReadGameMemory(raceFlow+0x24,&pausedState,4) && (pausedState==4 || pausedState==7))
        gRaceResumeNativeState=pausedState;
    gRaceDeferredNativeSwitch=gameFlow;gRaceProposalPrepared=false;gRaceLoadLaunched=false;
    if(session.role==coop::Role::Host) {
        p.seed=NextRaceNonce(gRaceLastHostEpoch);gRaceLastHostEpoch=p.seed;p.eventType=RACE_EVENT_PROPOSE;
        gCoopRaceState=COOP_RACE_HOST_WAITING_ACCEPT;
    } else {
        gRaceLocalRequest=NextRaceNonce(gRaceLocalRequest);p.requestId=gRaceLocalRequest;
        p.seed=p.requestId;p.eventType=RACE_EVENT_REQUEST;gCoopRaceState=COOP_RACE_REQUESTING;
    }
    gPendingRaceEvent=p;gRaceStateTimeoutMs=GetTickCount()+30000;gRaceLastEventSend=0;
    gRaceNativeConsentWindow={reinterpret_cast<uintptr_t>(gameFlow),raceFlow,p.sessionId,p.seed,track};
    SendCurrentRaceEvent(p.eventType);
    LogMessage(restarting?"[RACE_RESTART] Native restart held before teardown; peer approval requested.\r\n":
        "[RACE] Native event held before teardown; peer approval requested.\r\n");
    return true;
}
bool HoldNativeCoopRaceIntent(void* gameFlow) {
    return HoldNativeCoopRaceIntentImpl(gameFlow,false);
}
bool HoldNativeCoopRaceRestart(void* nativeFlow) {
    using namespace CoopPeerRaceNative;
    uintptr_t current=0;uint32_t state=0,pausedState=0;
    if(!ReadGameMemory(addresses.raceFlow,&current,sizeof(current)) ||
        current!=reinterpret_cast<uintptr_t>(nativeFlow) || !IsValidUserPtr(current) ||
        !ReadGameMemory(current,&state,4) ||
        (state==6 && (!ReadGameMemory(current+0x24,&pausedState,4) || (pausedState!=4 && pausedState!=7))) ||
        !HoldNativeCoopRaceIntentImpl(reinterpret_cast<void*>(addresses.gameFlowObject),true)) return false;
    // Restart came from native pause state6. Leave that state through its real
    // handler so UI, simulation pause and input are released, without unloading
    // the old world. Cancellation can therefore resume the same race safely.
    if(state==6) {
        // This also covers simultaneous initiation after a peer proposal has
        // already arrived: use THIS pause's saved state, never default RF7.
        gRaceResumeNativeState=pausedState;gChangeState(nativeFlow,pausedState);
    }
    return true;
}
static void CaptureRaceLaunchRollback() {
    if(!RaceEventCurrent(gPendingRaceEvent,CoopSnapshot()))return;
    if(gRaceLaunchRollback.valid && gRaceLaunchRollback.epoch==gPendingRaceEvent.seed &&
        gRaceLaunchRollback.session==gPendingRaceEvent.sessionId&&
        gRaceLaunchRollback.membershipNonce==gPendingRaceEvent.membershipNonce&&gRaceLaunchRollback.ticket==gPendingRaceEvent.ticket) return;
    RaceLaunchRollback saved{};uint32_t flow=0,nativeState=0;
    using namespace CoopPeerRaceNative;
    if(!ReadGameMemory(addresses.gameFlow,&flow,4) || flow!=6 ||
        !ReadGameMemory(addresses.raceFlow,&saved.nativeFlow,sizeof(saved.nativeFlow)) || !IsValidUserPtr(saved.nativeFlow) ||
        !ReadGameMemory(saved.nativeFlow,&nativeState,4) || (nativeState!=0 && nativeState!=4 && nativeState!=7) ||
        !ReadGameMemory(addresses.track,&saved.track,4)) return;
    saved.lifecycle=RaceStateSnapshot();saved.session=gPendingRaceEvent.sessionId;saved.epoch=gPendingRaceEvent.seed;
    saved.membershipNonce=gPendingRaceEvent.membershipNonce;saved.ticket=gPendingRaceEvent.ticket;
    saved.resumeState=nativeState==4 || nativeState==7?nativeState:gRaceResumeNativeState;
    using namespace CoopRaceFinishNative;
    saved.resultEntered=gResultEntered;saved.careerArmed=gCareerCommitArmed;saved.countsLogged=gParticipantCountsHoldLogged;
    saved.careerApplied=gCareerCommitApplied;
    saved.historyReady=gCareerHistoryReady;
    saved.appliedObject=gAppliedCommitObject;saved.appliedResult=gAppliedCommitResult;saved.appliedHash=gAppliedCommitHash;
    saved.resultCoordinator=gResultCoordinator;saved.localResultRow=gLocalResultRow;
    saved.checkpointPending=gRaceCheckpointPending;saved.checkpointSaved=gRaceCheckpointSaved;saved.lastControl=gRaceLastControlSend;
    saved.localReady=gLocalReadyForCountdown.load();saved.peerReady=gPeerReadyForCountdown.load();
    saved.triggered=gCountdownTriggered.load();saved.countdownActive=gCountdownActive.load();saved.countdownStart=gCountdownStartAtMs.load();
    saved.peerSpawnLoaded=gCar1LoadedForRace;saved.localLoadToken=gCoopFleetLocalLoadToken;
    saved.valid=true;gRaceLaunchRollback=saved;CaptureCoopRaceReturnBackup();
}
static bool RestoreRaceLaunchRollback() {
    const auto saved=gRaceLaunchRollback;uint32_t flow=0,track=0,state=0;uintptr_t native=0;
    using namespace CoopPeerRaceNative;
    if(!IsCoopGameplayActive() || !saved.valid || saved.epoch!=gPendingRaceEvent.seed || saved.session!=gPendingRaceEvent.sessionId ||
        !RaceEventCurrent(gPendingRaceEvent,CoopSnapshot()) || saved.membershipNonce!=gPendingRaceEvent.membershipNonce||saved.ticket!=gPendingRaceEvent.ticket ||
        !ReadGameMemory(addresses.gameFlow,&flow,4) || flow!=6 ||
        !ReadGameMemory(addresses.track,&track,4) || track!=saved.track ||
        !ReadGameMemory(addresses.raceFlow,&native,sizeof(native)) || native!=saved.nativeFlow ||
        !ReadGameMemory(native,&state,4) || (state!=0 && state!=4 && state!=7)) return false;
    AcquireSRWLockExclusive(&gRaceLifecycleLock);gRaceLifecycle=saved.lifecycle;ReleaseSRWLockExclusive(&gRaceLifecycleLock);
    using namespace CoopRaceFinishNative;
    gResultEntered=saved.resultEntered;gCareerCommitArmed=saved.careerArmed;gParticipantCountsHoldLogged=saved.countsLogged;
    gCareerCommitApplied=saved.careerApplied;
    gCareerHistoryReady=saved.historyReady;
    gAppliedCommitObject=saved.appliedObject;gAppliedCommitResult=saved.appliedResult;gAppliedCommitHash=saved.appliedHash;
    gResultCoordinator=saved.resultCoordinator;gLocalResultRow=saved.localResultRow;
    gRaceCheckpointPending=saved.checkpointPending;gRaceCheckpointSaved=saved.checkpointSaved;gRaceLastControlSend=saved.lastControl;
    gLocalReadyForCountdown.store(saved.localReady);gPeerReadyForCountdown.store(saved.peerReady);
    gCountdownTriggered.store(saved.triggered);gCountdownActive.store(saved.countdownActive);gCountdownStartAtMs.store(saved.countdownStart);
    gCar1LoadedForRace=saved.peerSpawnLoaded;gCoopFleetLocalLoadToken=saved.localLoadToken;
    gRaceResumeNativeState=saved.resumeState;gRaceLaunchRollback.valid=false;RestoreCoopRaceReturnBackup();return true;
}
static void CancelRejectedNativeRaceLaunch() {
    if(!RestoreRaceLaunchRollback()) return;
    gRaceLoadLaunched=false;gCoopRaceState=COOP_RACE_HOST_WAITING_ACCEPT;
    CancelCoopRaceInvitation(true);
    LogMessage("[RACE_LOAD] Native preparation rejected before teardown; old race/results restored.\r\n");
}
bool StartApprovedNativeCoopRace() {
    const auto session=CoopSnapshot();uint32_t flow=0,mode=0;
    using namespace CoopPeerRaceNative;
    CaptureRaceLaunchRollback();
    if(CoopSpecialBusy()||!CoopSpecialBothCarsForRace(gPendingRaceEvent.careerEventHash)){
        gRaceOverlay("Finish the special event, or select qualifying SUVs on both players.");
        CancelRejectedNativeRaceLaunch();return false;
    }
    if(!IsCoopGameplayActive() || !session.joined || !session.fresh ||
        !RaceEventCurrent(gPendingRaceEvent,session) ||
        !ReadGameMemory(addresses.gameFlow,&flow,4) || flow!=6 ||
        !ReadGameMemory(addresses.gameMode,&mode,4) || mode!=1) { CancelRejectedNativeRaceLaunch();return false; }
    if(gPendingRaceEvent.parentEpoch&&CoopUrlRestartHasRetainedContext()&&!CoopUrlRetainsNativeEvent()) {
        LogMessage("[RACE_RESTART] Retained heat route proof differs; restart refused before teardown.\r\n");
        CancelRejectedNativeRaceLaunch();return false;
    }
    // Validate and arm the retained loaded roster while native parts are still
    // alive. Stage3 runs after Restart has released their resource leases.
    if (gCoopAIEnabled && gPendingRaceEvent.parentEpoch &&
        !AuthorizeCoopAIRosterRestartBeforeTeardown()) {
        LogMessage("[AI_ROSTER] Restart loaded roster refused before teardown; old race kept.\r\n");
        CancelRejectedNativeRaceLaunch();return false;
    }
    if(gPendingRaceEvent.parentEpoch) {
        // Stock Restart retains both human parameters and therefore does not
        // invoke the normal local AddVehicle reset. Scope the spawn latch and
        // loader generation to THIS approved load, not the previous grid.
        ResetPeerSpawnForLocalSelection(1,0);
        LogMessage("[RACE_RESTART] New local load generation armed; retained peer must be recommitted.\r\n");
    }
    CaptureCoopRaceReturnOrigin();
    gCoopRaceState=COOP_RACE_STARTING;gRaceProposalPrepared=false;
    gRaceFinalizeLoader=0;gRaceFinalizeEpoch=gPendingRaceEvent.seed;gRaceFinalizeFailed=false;
    ResetCountdownSyncState();
    // Bind a fresh race epoch before loader/fleet callbacks, including restart.
    BindCoopRaceState();
    if(gRaceDeferredNativeSwitch && session.role==coop::Role::Host) {
        uintptr_t event=0;uint32_t hash=0;
        if(ReadGameMemory(addresses.selectedEvent,&event,sizeof(event)) && IsValidUserPtr(event) &&
            ReadGameMemory(event+8,&hash,4) && hash==gPendingRaceEvent.careerEventHash && SelectAuthoritativeNativeRoute()) {
            void* deferred=gRaceDeferredNativeSwitch;gRaceDeferredNativeSwitch=nullptr;
            if(gPendingRaceEvent.parentEpoch) {
                uintptr_t native=0;
                if(!ReadGameMemory(addresses.raceFlow,&native,sizeof(native)) || !IsValidUserPtr(native)) {
                    CancelRejectedNativeRaceLaunch();return false;
                }
                CoopMovieRaceRestartCommitted(session.sessionId,gPendingRaceEvent.parentEpoch,
                    gPendingRaceEvent.seed,gPendingRaceEvent.careerEventHash);
                gRestartRace(reinterpret_cast<void*>(native));
            } else gSwitchTrack(deferred);
            gRaceNativeConsentWindow={};gRaceLaunchRollback.valid=false;return true;
        }
    }
    if(StartNativePeerRace()) { gRaceDeferredNativeSwitch=nullptr;gRaceNativeConsentWindow={};gRaceLaunchRollback.valid=false;return true; }
    // This adapter returns false only BEFORE message16 or Restart/message9.
    // Restore the old epoch and result latches before cancelling the intent.
    CancelRejectedNativeRaceLaunch();
    return false;
}
const char* CoopRaceLoadingFailureMessage() {
    return IsCoopGameplayActive() && gCoopRaceState==COOP_RACE_STARTING && gRaceFinalizeFailed?
        "Race loading validation failed. Restart both games.":nullptr;
}
bool FinalizeCoopNativeRaceLoading(void* loader) {
    const auto session=CoopSnapshot();
    if(!IsCoopGameplayActive() || session.role!=coop::Role::Host || gCoopRaceState!=COOP_RACE_STARTING) return true;
    if(gRaceFinalizeFailed) return false;
    if(gRaceProposalPrepared) return true;
    uint32_t stage=0,flow=0;
    using namespace CoopPeerRaceNative;
    // Called only from the exact live stage3 entry/held callback, never from
    // UI/network guesses or a destroyed old world. Bind retries to this epoch.
    if(!RaceEventCurrent(gPendingRaceEvent,session) ||
        !IsValidUserPtr(reinterpret_cast<uintptr_t>(loader)) || !ReadGameMemory(reinterpret_cast<uintptr_t>(loader),&stage,4) || stage!=3 ||
        !ReadGameMemory(addresses.gameFlow,&flow,4) || (flow!=4 && flow!=5)) return false;
    if(gRaceFinalizeLoader && (gRaceFinalizeLoader!=reinterpret_cast<uintptr_t>(loader) || gRaceFinalizeEpoch!=gPendingRaceEvent.seed)) return false;
    const DWORD now=GetTickCount();
    if(!gRaceFinalizeLoader) { gRaceFinalizeLoader=reinterpret_cast<uintptr_t>(loader);gRaceFinalizeEpoch=gPendingRaceEvent.seed;gRaceFinalizeStarted=now;gRaceFinalizeLastTry=now-250; }
    if(now-gRaceFinalizeLastTry<250) return false;
    gRaceFinalizeLastTry=now;
    if(!PopulateNativeCoopRaceProposal()) {
        if(now-gRaceFinalizeStarted>=10000) {
            uintptr_t event=0;uint32_t hash=0,track=0;
            ReadGameMemory(addresses.selectedEvent,&event,sizeof(event));
            if(IsValidUserPtr(event)) ReadGameMemory(event+8,&hash,4);
            ReadGameMemory(addresses.track,&track,4);
            char diagnostic[360]{};
            std::snprintf(diagnostic,sizeof(diagnostic),"[RACE_LOAD_FAILED] reason=%s session=%llu epoch=%lu loader=%p stage=%lu flow=%lu expectedHash=%08lX actualHash=%08lX expectedTrack=%lu actualTrack=%lu. Old world already destroyed; hold retained. Restart both games.\r\n",
                gRaceFinalizeReason,static_cast<unsigned long long>(session.sessionId),static_cast<unsigned long>(gPendingRaceEvent.seed),loader,
                static_cast<unsigned long>(stage),static_cast<unsigned long>(flow),static_cast<unsigned long>(gPendingRaceEvent.careerEventHash),
                static_cast<unsigned long>(hash),static_cast<unsigned long>(gPendingRaceEvent.trackId),static_cast<unsigned long>(track));
            LogMessage(diagnostic);gRaceFinalizeFailed=true;
            gRaceTerminalEvent=gPendingRaceEvent;gRaceTerminalEvent.eventType=RACE_EVENT_ABORT;gRaceTerminalEvent.senderPlayer=1;
            gRaceTerminalUntil=now+3000;gRaceSend(&gRaceTerminalEvent,sizeof(gRaceTerminalEvent),session.peer);
            gRaceOverlay(CoopRaceLoadingFailureMessage());
        }
        return false;
    }
    gRaceProposalPrepared=true;gRaceLoadLaunched=true;BindCoopRaceState();
    SendCurrentRaceEvent(RACE_EVENT_START);
    LogMessage("[RACE] Host finalized native Career parameters; matched START published.\r\n");
    return true;
}
static bool RaceIntentPending() {
    return gCoopRaceState==COOP_RACE_HOST_WAITING_ACCEPT || gCoopRaceState==COOP_RACE_CLIENT_INVITED ||
        gCoopRaceState==COOP_RACE_REQUESTING;
}
#include "CoopRaceConsentNative.inl"
void CancelCoopRaceInvitation(bool notifyPeer) {
    const bool retainedStart=gRaceNativeConsentWindow.epoch &&
        gRaceNativeConsentWindow.gameFlow==reinterpret_cast<uintptr_t>(gRaceDeferredNativeSwitch);
    const bool waitingStart=gCoopRaceState==COOP_RACE_STARTING &&
        (CoopSnapshot().role==coop::Role::Client || retainedStart) && (!gRaceLoadLaunched || gTriggerRaceLoading);
    if(!RaceIntentPending() && !waitingStart) return;
    const auto session=CoopSnapshot();
    if(notifyPeer && RaceEventCurrent(gPendingRaceEvent,session)) {
        gRaceTerminalEvent=gPendingRaceEvent;
        gRaceTerminalEvent.eventType=gCoopRaceState==COOP_RACE_CLIENT_INVITED?RACE_EVENT_DECLINE:RACE_EVENT_ABORT;
        gRaceTerminalEvent.senderPlayer=session.role==coop::Role::Host?1:2;
        gRaceTerminalUntil=GetTickCount()+3000;
        gRaceSend(&gRaceTerminalEvent,sizeof(gRaceTerminalEvent),session.peer);
    }
    RestoreRaceLaunchRollback(); // Only succeeds while the exact old native world is still alive.
    if(gRaceDeferredNativeSwitch) {
        // State0 has queued a switch, but the consent gate kept the world alive.
        // Native dispatcher 80214C maps RF7 to Free Roam/post-race (603EB0),
        // RF4 to racing (60B3A0). RF1 is loading and must never be used as a
        // cancellation target. 581820/5815C0 must never run on decline.
        uint32_t flow=0,state=0;uintptr_t raceFlow=0;
        using namespace CoopPeerRaceNative;
        if(ReadGameMemory(addresses.gameFlow,&flow,4) && flow==6 &&
            ReadGameMemory(addresses.raceFlow,&raceFlow,sizeof(raceFlow)) && IsValidUserPtr(raceFlow) &&
            ReadGameMemory(raceFlow,&state,4) && state==0) {
            const uint8_t clear=0;
            SafeWriteBytes(raceFlow+0x47,&clear,1); // Native restart request latch.
            gChangeState(reinterpret_cast<void*>(raceFlow),gRaceResumeNativeState);
        }
    }
    // Native state must leave held RF0 before retiring its logical consent
    // identity: ChangeState can drain an already-queued native message4.
    gCoopRaceState=gRaceBeforeInvitation;
    gPendingRaceEvent=gRacePreviousEvent;gRaceProposalPrepared=gCoopRaceState==COOP_RACE_IN_PROGRESS;
    gRaceLoadLaunched=gCoopRaceState==COOP_RACE_IN_PROGRESS;
    gRaceDeferredNativeSwitch=nullptr;gRaceNativeConsentWindow={};gTriggerRaceLoading=false;
    gRaceOverlay("");
    LogMessage("[RACE] Invitation cancelled before world teardown; native world retained.\r\n");
}
bool HandleCoopRaceInviteKey(WPARAM key) {
    if(gCoopRaceState==COOP_RACE_STARTING && key==coop_console::kDeclineKey) {
        CancelCoopRaceInvitation(true);return true;
    }
    if(!RaceIntentPending()) return false;
    if(key==coop_console::kDeclineKey) { CancelCoopRaceInvitation(true);return true; }
    if(gCoopRaceState!=COOP_RACE_CLIENT_INVITED ||
        key!=coop_console::kAcceptKey) return false;
    const auto session=CoopSnapshot();
    if(!session.joined || !session.fresh) { CancelCoopRaceInvitation(false);return true; }
    if(!RaceEventCurrent(gPendingRaceEvent,session)) {ResetRaceIntentSession(session.sessionId);return true;}
    if(session.role==coop::Role::Host) {
        // The Client's request is only intent. Host re-resolves its native
        // event and chooses the authoritative route/laps/epoch after approval.
        RaceEventPacket proposal=gPendingRaceEvent;
        void* event=CoopPeerRaceNative::gLookupEvent(reinterpret_cast<void*>(CoopPeerRaceNative::addresses.career),proposal.careerEventHash);
        const uint32_t expectedHash=proposal.careerEventHash;
        if(!NativeEventDescription(event,proposal,false) || proposal.careerEventHash!=expectedHash) {
            CancelCoopRaceInvitation(true);return true;
        }
        proposal.seed=NextRaceNonce(gRaceLastHostEpoch);gRaceLastHostEpoch=proposal.seed;
        proposal.senderPlayer=1;proposal.eventType=RACE_EVENT_PROPOSE;
        gPendingRaceEvent=proposal;gCoopRaceState=COOP_RACE_HOST_WAITING_ACCEPT;
        SendCurrentRaceEvent(RACE_EVENT_PROPOSE);
        LogMessage("[RACE] Host approved Client event request; authoritative proposal sent.\r\n");
    } else {
        gCoopRaceState=COOP_RACE_STARTING;gRaceLoadLaunched=false;
        SendCurrentRaceEvent(RACE_EVENT_ACCEPT);
        LogMessage("[RACE] Peer invitation accepted; waiting for finalized Host START.\r\n");
    }
    gRaceOverlay("[ CO-OP RACE ] Race accepted! Synchronizing loading...");
    ConfigureOverlayLayout(true);return true;
}

static coop_race::State RaceStateSnapshot() {
    AcquireSRWLockShared(&gRaceLifecycleLock);auto state=gRaceLifecycle;
    ReleaseSRWLockShared(&gRaceLifecycleLock);return state;
}
bool CoopRaceReturnedToFreeRoam(DWORD flow,uint32_t track) {
    if(flow!=6 || track!=4000 || !IsCoopGameplayActive()) return false;
    const auto state=RaceStateSnapshot();const auto session=CoopSnapshot();
    // Accept/START are still processed in the old Free Roam world. Only a
    // race that actually reached its native grid can subsequently return.
    // That grid must belong to the CURRENT intent. A completed old epoch
    // remains ready in Free Roam while a new PROPOSE awaits its START; treating
    // the old grid as that new race's return would erase Client STARTING.
    return RaceEventCurrent(gPendingRaceEvent,session) && state.ready && state.token && state.session && state.session==session.sessionId &&
        state.track!=0 && state.track!=4000 && gPendingRaceEvent.sessionId==state.session &&
        gPendingRaceEvent.seed==state.token && gPendingRaceEvent.trackId==state.track;
}
static void BindCoopRaceState() {
    auto session=CoopSnapshot();
    if(!IsCoopGameplayActive() || !RaceEventCurrent(gPendingRaceEvent,session) || !gPendingRaceEvent.seed || !gPendingRaceEvent.trackId) return;
    AcquireSRWLockExclusive(&gRaceLifecycleLock);
    if(gRaceLifecycle.session!=session.sessionId || gRaceLifecycle.token!=gPendingRaceEvent.seed) {
        gRaceLifecycle.Begin(session.sessionId,gPendingRaceEvent.seed,gPendingRaceEvent.trackId,
            session.role==coop::Role::Host,gPendingRaceEvent.timedResult!=0);
        gRaceLoadLaunched=false;gRaceCheckpointSaved=false;gRaceCheckpointPending=false;
        gRaceLastControlSend=0;gRaceLastEventSend=0;
    }
    ReleaseSRWLockExclusive(&gRaceLifecycleLock);
}
bool CoopFinishRaceActive() {
    if(!IsCoopGameplayActive() || !gRaceFinishHooksReady) return false;
    uint32_t flow=0,track=0;
    if(!ReadGameMemory(gRaceContextAddresses.flow,&flow,4) || flow!=6 ||
        !ReadGameMemory(gRaceContextAddresses.track,&track,4) || track==0 || track==4000) return false;
    auto session=CoopSnapshot();auto state=RaceStateSnapshot();
    // A disconnect must not turn one finisher into a completed team result.
    return session.joined && state.session==session.sessionId && state.token &&
        (!gRaceIntentMembership || (session.membershipNonce==gRaceIntentMembership && session.ticket==gRaceIntentTicket)) &&
        track==state.track && (gCoopRaceState==COOP_RACE_STARTING || gCoopRaceState==COOP_RACE_IN_PROGRESS ||
        (RaceIntentPending() && gRaceBeforeInvitation==COOP_RACE_IN_PROGRESS && gPendingRaceEvent.parentEpoch==state.token));
}
static uintptr_t CoopRaceRacer(bool peer) {
    const uintptr_t car=CoopRaceCar(peer);
    uintptr_t racer=0,owner=0;
    if(!IsValidUserPtr(car) || !ReadGameMemory(car+0x1C,&racer,sizeof(racer)) ||
        !IsValidUserPtr(racer) || !ReadGameMemory(racer,&owner,sizeof(owner)) || owner!=car) return 0;
    return racer;
}
static bool ReadTimedRacerId(uintptr_t racer,bool host,uint16_t& id) {
    if(CoopFinishIsLocalRacer(racer)){id=host?0:1;return true;}
    if(CoopFinishIsPeerRacer(racer)){id=host?1:0;return true;}
    uintptr_t car=0,owner=0,parameter=0;uint32_t type=0;uint8_t driver=0,remote=0;
    return ReadGameMemory(racer,&car,4) && IsValidUserPtr(car) &&
        ReadGameMemory(car+0x1C,&owner,4) && owner==racer &&
        ReadGameMemory(car+0x14,&parameter,4) && IsValidUserPtr(parameter) &&
        ReadGameMemory(parameter+4,&type,4) && type==2 &&
        ReadGameMemory(parameter+0x900,&remote,1) && remote==(host?0:1) &&
        ReadGameMemory(parameter+1,&driver,1) && driver>=2 &&
        ReadGameMemory(car+0xA,&id,2) && id==driver;
}
static void TickCoopTimedResults(DWORD flow) {
    using namespace CoopRaceFinishNative;
    const auto state=RaceStateSnapshot();
    if(flow!=6 || !state.host || !state.timed || state.timedSealed || !state.BothFinished() || !CoopFinishRaceActive())return;
    uintptr_t coordinator=0;uint32_t count=0;
    if(!ReadGameMemory(kCoordinator,&coordinator,4) || !IsValidUserPtr(coordinator) ||
        !ReadGameMemory(coordinator+0x24,&count,4) || count<2 || count>kMaximumRacers)return;
    coop_race::TimedOpponent ai[4]{};uint8_t aiCount=0,humans=0;
    for(uint32_t i=0;i<count;++i) {
        uintptr_t racer=0;uint16_t id=0;
        if(!ReadRacer(coordinator,i,racer) || !ReadTimedRacerId(racer,true,id))return;
        if(id<2) {
            if(humans&(1u<<id))return;
            humans|=uint8_t(1u<<id);
            continue;
        }
        if(aiCount>=4)return;
        auto& row=ai[aiCount++];row.id=id;
        if(!ReadGameMemory(racer+0x10,&row.status,2) || !ReadGameMemory(racer+0x198,&row.ticks,4))return;
        if(!row.status || row.status>12 || ((row.status==1 || row.status==6) && !coop_race::TimedSuccess(row.status,row.ticks)))
            row.status=coop_race::kUnfinishedStatus;
        if(!coop_race::TimedSuccess(row.status,row.ticks))row.ticks=0;
    }
    if(humans!=3)return;
    std::sort(ai,ai+aiCount,[](const auto& a,const auto& b){return a.id<b.id;});
    AcquireSRWLockExclusive(&gRaceLifecycleLock);
    const bool sealed=gRaceLifecycle.session==state.session && gRaceLifecycle.token==state.token && gRaceLifecycle.SealTimed(aiCount,ai);
    const auto team=sealed?gRaceLifecycle.Team():coop_race::Finish{};
    ReleaseSRWLockExclusive(&gRaceLifecycleLock);
    if(sealed) {
        char line[220]{};
        std::snprintf(line,sizeof(line),"[RACE_RESULT] Timed rows sealed: host=%u/%u peer=%u/%u AI=%u teamRank=%u.\r\n",
            state.finish[0].ticks,unsigned(state.finish[0].status),state.finish[1].ticks,unsigned(state.finish[1].status),unsigned(aiCount),unsigned(team.rank));
        LogMessage(line);
    }
}

bool CoopFinishReadTimedResult(uintptr_t racer,uint8_t& rank,uint32_t& ticks,uint16_t& status) {
    const auto state=RaceStateSnapshot();uint16_t id=0;
    if(!state.timed || !state.ResultReady() || !ReadTimedRacerId(racer,state.host,id))return false;
    if(id<2) {
        const auto& finish=state.finish[id];ticks=finish.ticks;status=finish.status;
        if((status==1 || status==6) && !coop_race::TimedSuccess(status,ticks))status=coop_race::kUnfinishedStatus;
    } else {
        const auto* end=state.timedAi+state.timedCount;
        const auto* found=std::find_if(state.timedAi,end,[&](const auto& ai){return ai.id==id;});
        if(found==end)return false;
        ticks=found->ticks;status=found->status;
    }
    rank=state.TimedRank(id,status,ticks);
    return rank!=0;
}

bool CoopFinishHasOrderedResult() {
    const auto state=RaceStateSnapshot();
    return state.ResultReady() && (state.timed || state.Drift());
}
bool CoopFinishHasTimedResult() {
    const auto state=RaceStateSnapshot();
    return state.timed && state.ResultReady();
}
bool CoopFinishIsLocalRacer(uintptr_t racer) { return racer && racer==CoopRaceRacer(false); }
bool CoopFinishIsPeerRacer(uintptr_t racer) { return racer && racer==CoopRaceRacer(true); }
void CoopFinishCaptureLocal(uint8_t rank,uint32_t ticks,uint16_t status) {
    float score=0;uint32_t epoch=0;
    const bool drift=gPendingRaceEvent.raceParams[0x39]!=0;
    if(drift && (!ReadCoopLocalDriftScore(CoopRaceCar(false),score,epoch) || epoch!=gPendingRaceEvent.seed)) {
        LogMessage("[DRIFT_RESULT] Own final bank unavailable; result capture held.\r\n");return;
    }
    AcquireSRWLockExclusive(&gRaceLifecycleLock);
    bool captured=gRaceLifecycle.Capture(gRaceLifecycle.host?0:1,rank,ticks,status,score,drift);
    ReleaseSRWLockExclusive(&gRaceLifecycleLock);
    if(captured) LogMessage("[RACE_FINISH] Local finish locked; waiting for the other human.\r\n");
}
bool CoopFinishReadFrozen(uintptr_t racer,uint8_t& rank,uint32_t& ticks,uint16_t& status) {
    if(ReadCoopUrlFrozenRacer(racer,rank,ticks,status))return true;
    bool local=CoopFinishIsLocalRacer(racer),peer=CoopFinishIsPeerRacer(racer);
    if(!local && !peer) return false;
    auto state=RaceStateSnapshot();auto f=state.finish[local?(state.host?0:1):(state.host?1:0)];
    if(!coop_race::ValidFinish(f)) return false;
    if(state.timed && state.ResultReady())return CoopFinishReadTimedResult(racer,rank,ticks,status);
    if(state.Drift() && state.driftSealed)f.rank=state.DriftRank(local?(state.host?0:1):(state.host?1:0));
    rank=f.rank;ticks=f.ticks;status=f.status;return true;
}
bool CoopFinishReadTeamResult(uint8_t& rank,uint32_t& ticks,uint16_t& status) {
    auto state=RaceStateSnapshot();if(!state.ResultReady()) return false;
    if(IsCoopUrlTournament())return CoopUrlReadHeatResult(rank,ticks,status);
    auto f=state.Team();rank=f.rank;ticks=f.ticks;status=f.status;return true;
}
void CoopFinishNativeCareerCommitted() {
    AcquireSRWLockExclusive(&gRaceLifecycleLock);gRaceLifecycle.careerCommitted=true;
    ReleaseSRWLockExclusive(&gRaceLifecycleLock);
    if(!gRaceCheckpointSaved) gRaceCheckpointPending=true;
    // Save on the game thread immediately after the native win/fail callback.
    // A following invitation/Bind must not replace the old epoch before this
    // player's reward is serialized. Failed I/O retains the normal tick retry.
    if(gRaceCheckpointPending && gProfileContext.IsCoopActive() && SaveLocalCoopRaceProgress(GetTickCount())) {
        gRaceCheckpointSaved=true;gRaceCheckpointPending=false;
        LogMessage("[RACE_SAVE] Own native reward and shared receipt persisted before the next event.\r\n");
    }
}
bool CoopFinishCareerEventMatches(uint32_t hash) {
    if(!hash || coop_career::PersonalEvent(hash) || !IsCoopGameplayActive()) return false;
    const auto state=RaceStateSnapshot();const auto session=CoopSnapshot();
    return session.joined && session.fresh && state.ready && state.ResultReady() &&
        state.session==session.sessionId && state.session==gPendingRaceEvent.sessionId &&
        state.token && state.token==gPendingRaceEvent.seed && state.track==gPendingRaceEvent.trackId &&
        hash==gPendingRaceEvent.careerEventHash;
}
bool CoopFinishCareerRecordMatches(uintptr_t destination) {
    const uintptr_t career=CoopPeerRaceNative::addresses.career;
    uintptr_t record=0,event=0;uint32_t phase=0,hash=0;
    return ReadGameMemory(career+0x4208,&record,4) && IsValidUserPtr(record) &&
        destination==record+0x30 && ReadGameMemory(record+0x14,&phase,4) && (phase==3 || phase==4) &&
        ReadGameMemory(career+0x8730,&event,4) && IsValidUserPtr(event) &&
        ReadGameMemory(event+8,&hash,4) && CoopFinishCareerEventMatches(hash);
}
bool CoopFinishSceneScopeMatches() {
    const uintptr_t career=CoopPeerRaceNative::addresses.career;
    uintptr_t event=0,record=0;uint32_t hash=0;
    if(!ReadGameMemory(career+0x8730,&event,4)) return false;
    if(event) return IsValidUserPtr(event) && ReadGameMemory(event+8,&hash,4) && CoopFinishCareerEventMatches(hash);
    // Native acceptance may clear the selected event before scene cleanup.
    // Only its exact still-current applied record can retain that scene proof.
    using namespace CoopRaceFinishNative;
    return gCareerCommitApplied && reinterpret_cast<uintptr_t>(gAppliedCommitObject)==career &&
        ReadGameMemory(career+0x4208,&record,4) && IsValidUserPtr(record) &&
        record==reinterpret_cast<uintptr_t>(gAppliedCommitResult) && CoopFinishCareerEventMatches(gAppliedCommitHash);
}
bool CoopSharedCareerCanApply(DWORD flow) {
    if(flow!=6 || !IsCoopGameplayActive() || gPendingTrackLoader || RaceIntentPending() ||
        gCoopRaceState==COOP_RACE_STARTING) return false;
    const auto state=RaceStateSnapshot();
    if(state.session==CoopSnapshot().sessionId && state.ready && state.ResultReady() && !state.careerCommitted &&
        state.session==gPendingRaceEvent.sessionId && state.token==gPendingRaceEvent.seed &&
        state.track==gPendingRaceEvent.trackId) return false;
    // Never import a completion before THIS player's original native result
    // callback has credited its own reward. Applying Host-first completion
    // while Client still races/views results could suppress its local payout.
    return !CoopFinishRaceActive() || state.careerCommitted;
}
bool SaveLocalCoopRaceProgress(DWORD now) {
    const auto state=RaceStateSnapshot();auto team=state.Team();
    if(CoopUrlOwnsReceipt() && !CoopUrlReadFinalResult(team.rank,team.ticks,team.status))return false;
    if(!state.careerCommitted || !state.ResultReady() || !CoopFinishCareerEventMatches(gPendingRaceEvent.careerEventHash) ||
        coop_career::PersonalEvent(gPendingRaceEvent.careerEventHash)) return false;
    const coop_career::Outcome result{gPendingRaceEvent.careerEventHash,state.token,team.ticks,team.status,team.rank,0};
    return CoopPersonalSave::CaptureAndQueue(now,true,&result);
}
void CoopRaceGridReady() {
    BindCoopRaceState();AcquireSRWLockExclusive(&gRaceLifecycleLock);
    const bool first=!gRaceLifecycle.ready;gRaceLifecycle.ready=true;
    ReleaseSRWLockExclusive(&gRaceLifecycleLock);
    if(first) LogMessage("[RACE_GRID] Native grid reached; ready sent, waiting for peer grid.\r\n");
}
bool CoopRaceCountdownDeadline(uint32_t& deadline) {
    return RaceStateSnapshot().LocalDeadline(deadline);
}
void CoopRaceNativeCountdownReady() {
    BindCoopRaceState();AcquireSRWLockExclusive(&gRaceLifecycleLock);
    const bool first=gRaceLifecycle.ready && gRaceLifecycle.startCommit && !gRaceLifecycle.countdownReady;
    if(first)gRaceLifecycle.countdownReady=true;
    ReleaseSRWLockExclusive(&gRaceLifecycleLock);
    if(first)LogMessage("[RACE_COUNTDOWN] Native starter completed; HUD countdown ready, waiting for teammate.\r\n");
}
bool CoopRaceHudCountdownDeadline(uint32_t& deadline) {
    return RaceStateSnapshot().LocalCountdownDeadline(deadline);
}
void CoopRaceCountdownStarted() {
    const auto state=RaceStateSnapshot();uint32_t deadline=0;state.LocalCountdownDeadline(deadline);
    const uint32_t now=GetTickCount();char line[280]{};
    std::snprintf(line,sizeof(line),"[RACE_COUNTDOWN] Shared HUD countdown released session=%llu epoch=%u track=%u role=%u localDeadline=%u now=%u lateMs=%d offset=%d rttMs=%u; native quarter-ms origin projected.\r\n",
        static_cast<unsigned long long>(state.session),state.token,state.track,state.host?1u:2u,
        deadline,now,static_cast<int32_t>(now-deadline),state.countdownOffset,state.bestRtt);
    LogMessage(line);
}

static void SendCurrentRaceEvent(uint16_t type) {
    RaceEventPacket p=gPendingRaceEvent;p.eventType=type;
    const auto session=CoopSnapshot();p.senderPlayer=session.role==coop::Role::Host?1:2;
    if(RaceEventCurrent(p,session)) gRaceSend(&p,sizeof(p),session.peer);
}
static bool ValidRaceEvent(const RaceEventPacket& p,const coop::SessionSnapshot& session) {
    const bool type=p.eventType>=RACE_EVENT_PROPOSE && p.eventType<=RACE_EVENT_ABORT ||
        p.eventType==RACE_EVENT_REQUEST || p.eventType==RACE_EVENT_CANCEL_ACK;
    return p.magic==kRaceEventMagic && p.version==kRacePacketVersion && p.timedResult<=1 && RaceEventCurrent(p,session) &&
        p.senderPlayer==(session.role==coop::Role::Host?2u:1u) && p.seed && p.careerEventHash &&
        p.trackId>0 && p.trackId<32768 && p.trackId!=4000 && p.laps>0 && p.laps<=99 && p.direction<=1 && type &&
        (p.eventType!=RACE_EVENT_REQUEST || (p.senderPlayer==2 && p.requestId==p.seed));
}
static bool SameRaceIntent(const RaceEventPacket& a,const RaceEventPacket& b) {
    return a.sessionId==b.sessionId && a.membershipNonce==b.membershipNonce && a.ticket==b.ticket && a.seed==b.seed && a.trackId==b.trackId &&
        a.careerEventHash==b.careerEventHash && a.requestId==b.requestId && a.parentEpoch==b.parentEpoch && a.timedResult==b.timedResult;
}
static bool MatchesRaceWorld(const RaceEventPacket& p,DWORD flow) {
    uint32_t track=0;if(flow!=6 || !ReadGameMemory(CoopPeerRaceNative::addresses.track,&track,4)) return false;
    if(!p.parentEpoch) return track==4000;
    const auto old=RaceStateSnapshot();
    return old.session==p.sessionId && old.token==p.parentEpoch && old.track==track && p.trackId==track;
}
static void ProcessCoopRaceEvent(const RaceEventPacket& p,DWORD flow) {
    const auto session=CoopSnapshot();
    if(!IsCoopGameplayActive() || !session.joined || !session.fresh || !gRaceFinishHooksReady || !ValidRaceEvent(p,session)) return;
    if((p.eventType==RACE_EVENT_REQUEST || p.eventType==RACE_EVENT_PROPOSE) && !FlushPendingRaceSave()) return;
    ResetRaceIntentSession(session.sessionId);
    if(p.eventType==RACE_EVENT_CANCEL_ACK) {
        if(SameRaceIntent(p,gRaceTerminalEvent)) gRaceTerminalUntil=0;
        return;
    }
    if(p.eventType==RACE_EVENT_DECLINE || p.eventType==RACE_EVENT_ABORT) {
        if(SameRaceIntent(p,gRaceReceivedTerminal)) {
            RaceEventPacket ack=p;ack.eventType=RACE_EVENT_CANCEL_ACK;ack.senderPlayer=session.role==coop::Role::Host?1:2;
            gRaceSend(&ack,sizeof(ack),session.peer);return;
        }
        const bool matched=SameRaceIntent(p,gPendingRaceEvent);
        const bool requestCancelled=session.role==coop::Role::Host && p.requestId && p.seed==p.requestId && MatchesRaceWorld(p,flow);
        const bool requestReply=session.role==coop::Role::Client && p.eventType==RACE_EVENT_DECLINE &&
            p.requestId && p.requestId==gRaceLocalRequest && p.seed==p.requestId;
        const bool hostCancelled=session.role==coop::Role::Client && MatchesRaceWorld(p,flow) &&
            !requestReply &&
            (!gRaceLastHostEpoch || static_cast<int32_t>(p.seed-gRaceLastHostEpoch)>=0);
        if(!matched && !requestCancelled && !hostCancelled && !requestReply) return;
        if(requestCancelled && (!gRaceLastPeerRequest || static_cast<int32_t>(p.requestId-gRaceLastPeerRequest)>0))
            gRaceLastPeerRequest=p.requestId;
        if(hostCancelled) gRaceLastHostEpoch=p.seed;
        gRaceReceivedTerminal=p;
        RaceEventPacket ack=p;ack.eventType=RACE_EVENT_CANCEL_ACK;ack.senderPlayer=session.role==coop::Role::Host?1:2;
        gRaceSend(&ack,sizeof(ack),session.peer);
        if(matched && session.role==coop::Role::Host && gCoopRaceState==COOP_RACE_STARTING &&
            gRaceProposalPrepared && !RaceStateSnapshot().peerReady) {
            // Client native preparation rejected AFTER Host teardown/START.
            // Its old world can resume, but Host must never roll that world back.
            gRaceFinalizeFailed=true;
            LogMessage("[RACE_LOAD_FAILED] Token-matched peer cancelled finalized START before grid; Host world already destroyed. Hold retained. Restart both games.\r\n");
            gRaceOverlay(CoopRaceLoadingFailureMessage());
        }
        if(matched && (RaceIntentPending() || (gCoopRaceState==COOP_RACE_STARTING &&
            session.role==coop::Role::Client && (!gRaceLoadLaunched || gTriggerRaceLoading)))) CancelCoopRaceInvitation(false);
        return;
    }
    if(p.eventType==RACE_EVENT_REQUEST && session.role==coop::Role::Host) {
        if(CoopSpecialBusy()){
            RaceEventPacket decline=p;decline.senderPlayer=1;decline.eventType=RACE_EVENT_DECLINE;
            gRaceSend(&decline,sizeof(decline),session.peer);return;
        }
        if(gRaceLastPeerRequest && static_cast<int32_t>(p.requestId-gRaceLastPeerRequest)<=0) return;
        if(!MatchesRaceWorld(p,flow)) return;
        if(gCoopRaceState!=COOP_RACE_IDLE && gCoopRaceState!=COOP_RACE_IN_PROGRESS) {
            // Host's already-pending proposal wins simultaneous initiation.
            // A Client request nonce must never become a Host race epoch.
            gRaceLastPeerRequest=p.requestId;gRaceTerminalEvent=p;
            gRaceTerminalEvent.senderPlayer=1;gRaceTerminalEvent.eventType=RACE_EVENT_DECLINE;
            gRaceTerminalUntil=GetTickCount()+3000;
            gRaceSend(&gRaceTerminalEvent,sizeof(gRaceTerminalEvent),session.peer);return;
        }
        RaceEventPacket canonical=p;
        void* event=CoopPeerRaceNative::gLookupEvent(reinterpret_cast<void*>(CoopPeerRaceNative::addresses.career),p.careerEventHash);
        if(!NativeEventDescription(event,canonical,false) || canonical.careerEventHash!=p.careerEventHash ||
            (p.parentEpoch && canonical.trackId!=p.trackId)) return;
        gRaceLastPeerRequest=p.requestId;gRacePreviousEvent=gPendingRaceEvent;
        gRaceBeforeInvitation=gCoopRaceState;gPendingRaceEvent=p;
        gCoopRaceState=COOP_RACE_CLIENT_INVITED;gRaceStateTimeoutMs=GetTickCount()+30000;
        LogMessage("[RACE] Client native event request received; waiting for Host approval.\r\n");return;
    }
    if(p.eventType==RACE_EVENT_PROPOSE && session.role==coop::Role::Client) {
        if(CoopSpecialBusy()){
            RaceEventPacket decline=p;decline.senderPlayer=2;decline.eventType=RACE_EVENT_DECLINE;
            gRaceSend(&decline,sizeof(decline),session.peer);return;
        }
        if(gCoopRaceState!=COOP_RACE_REQUESTING && SameRaceIntent(p,gPendingRaceEvent)) return;
        if(gRaceLastHostEpoch && static_cast<int32_t>(p.seed-gRaceLastHostEpoch)<=0) return;
        const bool requested=gCoopRaceState==COOP_RACE_REQUESTING && p.requestId==gPendingRaceEvent.requestId &&
            p.careerEventHash==gPendingRaceEvent.careerEventHash && p.parentEpoch==gPendingRaceEvent.parentEpoch;
        if((!requested && gCoopRaceState!=COOP_RACE_IDLE && gCoopRaceState!=COOP_RACE_IN_PROGRESS) || !MatchesRaceWorld(p,flow)) return;
        if(!requested) { gRacePreviousEvent=gPendingRaceEvent;gRaceBeforeInvitation=gCoopRaceState; }
        // A Client request nonce becomes the authoritative Host epoch here.
        // Its SAME retained native world must remain held through finalized
        // START; otherwise the strict window would stop matching on approval.
        if(requested && gRaceNativeConsentWindow.session==gPendingRaceEvent.sessionId &&
            gRaceNativeConsentWindow.epoch==gPendingRaceEvent.seed &&
            gRaceNativeConsentWindow.gameFlow==reinterpret_cast<uintptr_t>(gRaceDeferredNativeSwitch))
            gRaceNativeConsentWindow.epoch=p.seed;
        gRaceLastHostEpoch=p.seed;gPendingRaceEvent=p;gRaceLoadLaunched=false;gRaceProposalPrepared=false;
        gRaceStateTimeoutMs=GetTickCount()+30000;
        gCoopRaceState=requested?COOP_RACE_STARTING:COOP_RACE_CLIENT_INVITED;
        if(requested) SendCurrentRaceEvent(RACE_EVENT_ACCEPT);
        LogMessage(requested?"[RACE] Host approved local request; awaiting finalized START.\r\n":
            "[RACE] Session-scoped peer invitation received on game thread.\r\n");return;
    }
    if(!SameRaceIntent(p,gPendingRaceEvent)) return;
    if(CoopUrlConsumeFinalizedStart(p,flow))return;
    if(p.eventType==RACE_EVENT_ACCEPT && session.role==coop::Role::Host && gCoopRaceState==COOP_RACE_HOST_WAITING_ACCEPT) {
        if(!StartApprovedNativeCoopRace()) {
            LogMessage("[RACE] Approved native event could not launch.\r\n");
        }
    } else if(p.eventType==RACE_EVENT_START && session.role==coop::Role::Client &&
        gCoopRaceState==COOP_RACE_STARTING && !gRaceLoadLaunched) {
        uint32_t nativeTrack=0,nativeLaps=0,nativeDirection=0;
        std::memcpy(&nativeTrack,p.raceParams,4);std::memcpy(&nativeDirection,p.raceParams+4,4);
        std::memcpy(&nativeLaps,p.raceParams+0x1C,4);
        if(nativeTrack!=p.trackId || nativeLaps!=p.laps || nativeDirection!=p.direction) return;
        gPendingRaceEvent=p;CaptureRaceLaunchRollback();BindCoopRaceState();gRaceLoadLaunched=true;gTriggerRaceLoading=true;
        LogMessage("[RACE] Finalized, token-matched Host START received; native load queued.\r\n");
    }
}
bool ReceiveCoopRacePacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now) {
    uint32_t magic=0;if(bytes<4) return false;std::memcpy(&magic,data,4);
    if(magic!=coop_race::kMagic && magic!=kRaceEventMagic) return false;
    if(!IsJoinedCoopSender(sender)) return true;
    const auto session=CoopSnapshot();
    if(magic==coop_race::kMagic) {
        if(bytes!=sizeof(coop_race::Packet)) return true;
        coop_race::Packet p{};std::memcpy(&p,data,sizeof(p));
        AcquireSRWLockExclusive(&gRaceLifecycleLock);gRaceLifecycle.Receive(p,now);
        ReleaseSRWLockExclusive(&gRaceLifecycleLock);return true;
    }
    if(bytes!=sizeof(RaceEventPacket)) return true;
    RaceEventPacket p{};std::memcpy(&p,data,sizeof(p));
    if(!ValidRaceEvent(p,session)) return true;
    AcquireSRWLockExclusive(&gRaceLifecycleLock);
    if(gRaceMailboxCount<gRaceMailbox.size()) gRaceMailbox[gRaceMailboxCount++]=p;
    ReleaseSRWLockExclusive(&gRaceLifecycleLock);return true;
}
void TickCoopRaceNetwork(DWORD now) {
    auto session=CoopSnapshot();if(!session.joined || !session.fresh) return;
    coop_race::Packet p{};bool send=false;
    AcquireSRWLockExclusive(&gRaceLifecycleLock);
    if(gRaceIntentSession==session.sessionId&&gRaceIntentMembership==session.membershipNonce&&gRaceIntentTicket==session.ticket&&
        gRaceLifecycle.session==session.sessionId && gRaceLifecycle.token && now-gRaceLastControlSend>=50) {
        p=gRaceLifecycle.Build(now);gRaceLastControlSend=now;send=true;
    }
    ReleaseSRWLockExclusive(&gRaceLifecycleLock);
    if(send) SendSessionBytes(&p,sizeof(p),session.peer);
}
void TickCoopRaceLifecycle(DWORD flow) {
    SaveCoopRaceReturnOrigin(flow);
    const auto session=CoopSnapshot();
    ResetRaceIntentSession(session.joined?session.sessionId:0);
    if (CoopInterruptedRace::Observe(session)) {
        CoopInterruptedRace::RequestExit();
        return;
    }
    std::array<RaceEventPacket,8> messages{};size_t count=0;
    AcquireSRWLockExclusive(&gRaceLifecycleLock);
    count=gRaceMailboxCount;std::copy_n(gRaceMailbox.begin(),count,messages.begin());gRaceMailboxCount=0;
    ReleaseSRWLockExclusive(&gRaceLifecycleLock);
    ObserveNativeRaceResumeState();
    for(size_t i=0;i<count;++i) ProcessCoopRaceEvent(messages[i],flow);
    if(!IsCoopGameplayActive()) return;
    const auto state=RaceStateSnapshot();
    DWORD now=GetTickCount();
    if(RaceIntentPending() && (!session.joined || !session.fresh || static_cast<int32_t>(now-gRaceStateTimeoutMs)>=0))
        CancelCoopRaceInvitation(session.joined && session.fresh);
    if(session.joined && session.fresh && now-gRaceLastEventSend>=200) {
        if(gRaceTerminalUntil && RaceEventCurrent(gRaceTerminalEvent,session) && static_cast<int32_t>(gRaceTerminalUntil-now)>0)
            gRaceSend(&gRaceTerminalEvent,sizeof(gRaceTerminalEvent),session.peer);
        if(gCoopRaceState==COOP_RACE_HOST_WAITING_ACCEPT) SendCurrentRaceEvent(RACE_EVENT_PROPOSE);
        else if(gCoopRaceState==COOP_RACE_REQUESTING) SendCurrentRaceEvent(RACE_EVENT_REQUEST);
        else if(gCoopRaceState==COOP_RACE_STARTING && !state.peerReady) {
            if(session.role==coop::Role::Host && gRaceProposalPrepared) SendCurrentRaceEvent(RACE_EVENT_START);
            else if(session.role==coop::Role::Client && !gRaceLoadLaunched) SendCurrentRaceEvent(RACE_EVENT_ACCEPT);
        }
        gRaceLastEventSend=now;
    }
    // Ready is published only by verified native grid/countdown callbacks.
    // flow=6 can still refer to the old world while a launch is pending.
    TickCoopDriftResults(flow);
    TickCoopTimedResults(flow);
    TickCoopRaceFinish(flow);
    if(gRaceCheckpointPending && !gRaceCheckpointSaved &&
        session.joined && session.fresh && gProfileContext.IsCoopActive()) {
        if(SaveLocalCoopRaceProgress(now)) {gRaceCheckpointSaved=true;gRaceCheckpointPending=false;
            LogMessage("[RACE_SAVE] This player's native reward/garage saved; only shared outcome/progress queued.\r\n");}
    }
}
