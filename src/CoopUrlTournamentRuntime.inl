// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Native work stays on the UI/game thread; UDP only copies bounded packets.
namespace CoopUrlRuntime {
static coop_url::State state;
static SRWLOCK lock=SRWLOCK_INIT;
static coop_url::Packet inbox[16]{},outbound{};static size_t inboxCount=0;
static coop::Endpoint peer{};static bool published=false;static DWORD lastSent=0,lastTry=0,lastTrace=0;
static bool advancing=false,wireActive=false;static uint32_t appliedEpoch=0;static RaceEventPacket nextEvent{};
struct Transfer {uint64_t session=0;uint32_t event=0,oldTrack=0,newTrack=0,oldEpoch=0,newEpoch=0;};
static Transfer transfer;
static void Publish() {
    const auto session=CoopSnapshot();
    AcquireSRWLockExclusive(&lock);
    published=wireActive && state.session && session.joined && session.sessionId==state.session;
    if(published){outbound=state.Build();peer=session.peer;}
    ReleaseSRWLockExclusive(&lock);
}
static bool Current() {
    const auto session=CoopSnapshot();
    return session.joined&&session.fresh&&session.sessionId==state.session&&
        state.context.eventHash==gPendingRaceEvent.careerEventHash&&state.context.heatEpoch==gPendingRaceEvent.seed;
}
static void Trace(const char* reason) {
    const DWORD now=GetTickCount();if(now-lastTrace<3000)return;lastTrace=now;
    char line[260]{};std::snprintf(line,sizeof(line),
        "[URL_STATE] %s event=%08X root=%u heat=%u/%u epoch=%u mask=%u ready=%u next=%u/%u commit=%u totals=%u/%u.\r\n",
        reason,state.context.eventHash,state.context.rootEpoch,state.context.index+1,state.context.total,state.context.heatEpoch,
        unsigned(state.mask),state.Ready()?1u:0u,state.localNext?1u:0u,state.peerNext?1u:0u,state.nextCommit?1u:0u,state.Total(0),state.Total(1));LogMessage(line);
}
static bool PrepareNext() {
    if(!Current()||!state.NextReady())return false;
    const auto old=gPendingRaceEvent;const auto& c=state.context;
    const uintptr_t route=CoopPeerRaceNative::addresses.routeList+(c.index+1)*0xD24;
    uint32_t values[3]{},count=0;
    if(!ReadGameMemory(CoopPeerRaceNative::addresses.routeCount,&count,4)||count!=c.total||c.index+1>=count||
        !ReadGameMemory(route,values,sizeof(values))||!values[0]||values[0]==4000||values[1]>1||!values[2]||values[2]>99)return false;
    nextEvent=old;nextEvent.parentEpoch=old.seed;nextEvent.seed=state.nextEpoch[c.index];nextEvent.requestId=0;
    nextEvent.trackId=values[0];nextEvent.direction=values[1];nextEvent.laps=values[2];
    std::memcpy(nextEvent.raceParams,values,4);std::memcpy(nextEvent.raceParams+4,values+1,4);
    std::memcpy(nextEvent.raceParams+0x1C,values+2,4);
    transfer={state.session,c.eventHash,c.track,values[0],c.heatEpoch,nextEvent.seed};
    // Parts must still be live at authorization. Restore the old intent before
    // replaying its native menu action; no event selection/reconstruction.
    gPendingRaceEvent=nextEvent;const bool ready=!gCoopAIEnabled||AuthorizeCoopAIRosterRestartBeforeTeardown();
    gPendingRaceEvent=old;if(!ready){transfer={};return false;}
    advancing=true;const bool replayed=ReplayCoopUrlNextHeat();advancing=false;
    if(!replayed){transfer={};Trace("native-next-action-not-ready");}return replayed;
}
}

uint32_t CoopUrlCurrentRootEpoch() {
    using namespace CoopUrlRuntime;const auto session=CoopSnapshot();
    if(state.session!=session.sessionId||state.context.eventHash!=gPendingRaceEvent.careerEventHash)return 0;
    if(state.context.heatEpoch==gPendingRaceEvent.seed||gPendingRaceEvent.parentEpoch==state.context.heatEpoch||
        (transfer.session==session.sessionId&&transfer.newEpoch==gPendingRaceEvent.seed))return state.context.rootEpoch;
    return 0;
}
bool CoopUrlRetainedTransition(uint32_t oldTrack,uint32_t newTrack,uint32_t oldEpoch,uint32_t newEpoch,uint32_t eventHash) {
    using namespace CoopUrlRuntime;const auto session=CoopSnapshot();
    return session.joined&&session.fresh&&transfer.session==session.sessionId&&state.session==session.sessionId&&
        transfer.event==eventHash&&state.context.eventHash==eventHash&&transfer.oldTrack==oldTrack&&transfer.newTrack==newTrack&&
        transfer.oldEpoch==oldEpoch&&transfer.newEpoch==newEpoch&&state.nextCommit&&
        state.nextEpoch[state.context.index]==newEpoch;
}
bool CoopUrlReadCareerOutcome(uint8_t& rank) {
    using namespace CoopUrlRuntime;if(!Current()||!state.Final())return false;
    rank=state.TeamRank();return rank!=0;
}
bool CoopUrlOwnsReceipt(){return CoopUrlRuntime::Current()&&CoopUrlRuntime::state.context.total!=0;}
bool CoopUrlRestartHasRetainedContext() {
    using namespace CoopUrlRuntime;const auto session=CoopSnapshot();const auto& c=state.context;
    return session.joined&&session.fresh&&state.session==session.sessionId&&c.total&&
        c.eventHash==gPendingRaceEvent.careerEventHash&&c.heatEpoch==gPendingRaceEvent.parentEpoch&&
        gPendingRaceEvent.seed!=c.heatEpoch&&gPendingRaceEvent.trackId==c.track;
}
bool CoopUrlRetainsNativeEvent() {
    using namespace CoopUrlRuntime;uintptr_t selected=0;uint32_t count=0,track=0;
    const auto& c=state.context;
    return CoopUrlRestartHasRetainedContext()&&
        ReadGameMemory(CoopPeerRaceNative::addresses.routeCount,&count,4)&&count==c.total&&
        ReadGameMemory(CoopPeerRaceNative::addresses.selectedRoute,&selected,4)&&
        selected==CoopPeerRaceNative::addresses.routeList+c.index*0xD24&&ReadGameMemory(selected,&track,4)&&track==c.track;
}
bool CoopUrlReadHeatResult(uint8_t& rank,uint32_t& ticks,uint16_t& status) {
    using namespace CoopUrlRuntime;if(!Current()||!state.Ready()||appliedEpoch!=state.context.heatEpoch)return false;
    const auto& heat=state.heats[state.context.index];
    const auto& row=heat.rows[heat.rows[1].rank<heat.rows[0].rank?1:0];
    rank=row.rank;ticks=row.ticks;status=row.status;return true;
}
bool CoopUrlReadFinalResult(uint8_t& rank,uint32_t& ticks,uint16_t& status) {
    using namespace CoopUrlRuntime;if(!CoopUrlReadCareerOutcome(rank))return false;
    const auto& row=state.heats[state.context.index].rows[state.BestHuman()];ticks=row.ticks;
    status=rank==1?1:row.status;return true;
}
bool CoopUrlRequestNextHeat() {
    using namespace CoopUrlRuntime;
    if(!Current()||!state.RequestNext())return true;
    LogMessage("[URL_NEXT] Local next-heat approval retained; waiting for the other player's native action.\r\n");Publish();return true;
}
bool CoopUrlMayAdvanceNative(){return CoopUrlRuntime::advancing&&CoopUrlRuntime::Current()&&CoopUrlRuntime::state.NextReady();}
void CoopUrlNativeAdvanced(uint32_t oldIndex,uint32_t nextIndex) {
    using namespace CoopUrlRuntime;
    if(!advancing||oldIndex!=state.context.index||nextIndex!=oldIndex+1||!state.Advance())return;
    gPendingRaceEvent=nextEvent;gCoopRaceState=COOP_RACE_STARTING;gRaceBeforeInvitation=COOP_RACE_IN_PROGRESS;
    appliedEpoch=0;
    ResetPeerSpawnForLocalSelection(1,0);
    // Keep the original reachable entrance while only the heat identity moves.
    auto& origin=CoopRaceReturn::origin;
    if(origin.valid&&!origin.applied&&origin.session==state.session&&origin.epoch==state.context.heatEpoch&&origin.track==state.context.track) {
        origin.epoch=nextEvent.seed;origin.track=nextEvent.trackId;
    }
    gRaceProposalPrepared=false;gRaceFinalizeLoader=0;gRaceFinalizeEpoch=nextEvent.seed;gRaceFinalizeFailed=false;
    gRaceDeferredNativeSwitch=nullptr;gRaceNativeConsentWindow={};ResetCountdownSyncState();BindCoopRaceState();
    gRaceLoadLaunched=true; // Bind clears this latch for every new epoch.
    LogMessage("[URL_NEXT] Native next heat advanced once; prior individual heat points retained and a fresh grid epoch armed.\r\n");Publish();
}
void CoopUrlResultPrepared() {
    uint8_t rank=0;
    if(CoopUrlReadCareerOutcome(rank)&&CoopRaceFinishNative::gCareerCommitArmed&&
        CoopRaceFinishNative::gCareerHistoryReady&&!CoopRaceFinishNative::gCareerCommitApplied)
        CoopRaceFinishNative::HookOrdinaryCareerCommit(reinterpret_cast<void*>(CoopPeerRaceNative::addresses.career),nullptr);
}
bool CoopUrlConsumeFinalizedStart(const RaceEventPacket& packet,DWORD flow) {
    using namespace CoopUrlRuntime;
    if(packet.eventType!=RACE_EVENT_START||CoopSnapshot().role!=coop::Role::Client||
        !gRaceLoadLaunched||gCoopRaceState!=COOP_RACE_STARTING||(flow!=4&&flow!=5)||
        !transfer.session||packet.sessionId!=transfer.session||packet.seed!=transfer.newEpoch||
        packet.parentEpoch!=transfer.oldEpoch||packet.trackId!=transfer.newTrack||packet.careerEventHash!=transfer.event)return false;
    uint32_t track=0,direction=0,laps=0;std::memcpy(&track,packet.raceParams,4);
    std::memcpy(&direction,packet.raceParams+4,4);std::memcpy(&laps,packet.raceParams+0x1C,4);
    if(track==packet.trackId&&direction==packet.direction&&laps==packet.laps)gPendingRaceEvent=packet;
    return true; // This native heat is already loading; never queue another switch.
}
bool ReceiveCoopUrlPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD) {
    uint32_t magic=0;if(!data||bytes<4)return false;std::memcpy(&magic,data,4);if(magic!=coop_url::kMagic)return false;
    if(bytes!=sizeof(coop_url::Packet)||!IsJoinedCoopSender(sender))return true;
    coop_url::Packet packet{};std::memcpy(&packet,data,sizeof(packet));const auto session=CoopSnapshot();
    if(!coop_url::Valid(packet)||packet.session!=session.sessionId||packet.player!=(session.role==coop::Role::Host?2:1))return true;
    AcquireSRWLockExclusive(&CoopUrlRuntime::lock);
    if(CoopUrlRuntime::inboxCount<16)CoopUrlRuntime::inbox[CoopUrlRuntime::inboxCount++]=packet;
    ReleaseSRWLockExclusive(&CoopUrlRuntime::lock);return true;
}
void TickCoopUrlNetwork(DWORD now) {
    using namespace CoopUrlRuntime;const auto session=CoopSnapshot();coop_url::Packet packet{};bool send=false;
    AcquireSRWLockExclusive(&lock);
    if(published&&session.joined&&session.fresh&&session.sessionId==outbound.session&&session.peer==peer&&now-lastSent>=50){packet=outbound;lastSent=now;send=true;}
    ReleaseSRWLockExclusive(&lock);if(send)SendSessionBytes(&packet,sizeof(packet),session.peer);
}
void TickCoopUrlTournament(DWORD flow) {
    using namespace CoopUrlRuntime;const auto session=CoopSnapshot();uint32_t track=0;
    ReadGameMemory(CoopPeerRaceNative::addresses.track,&track,4);
    if(!IsCoopGameplayActive()||!session.joined||state.session&&state.session!=session.sessionId||(flow==6&&track==4000)) {
        const bool keepReceipt=IsCoopGameplayActive()&&session.joined&&flow==6&&track==4000&&Current()&&state.Final();
        if(!keepReceipt){state={};transfer={};nextEvent={};advancing=false;appliedEpoch=0;}
        wireActive=false;Publish();return;
    }
    coop_url::Context context{};
    if(ReadCoopUrlHeatContext(context)) {
        wireActive=true;
        const bool changed=!coop_url::Same(state.context,context);
        if(changed)appliedEpoch=0;
        if(state.session&&(!coop_url::Compatible(state.context,context)))state={};
        if(state.session&&context.index==state.context.index&&context.heatEpoch!=state.context.heatEpoch&&
            gPendingRaceEvent.parentEpoch==state.context.heatEpoch)state.Restart(context);
        if(!state.Bind(session.sessionId,session.role==coop::Role::Host,context)){Trace("context-refused");return;}
        if(changed)ReconcileCoopUrlParticipants();
    }
    if(!state.session)return;
    coop_url::Packet incoming[16]{};size_t count=0;
    AcquireSRWLockExclusive(&lock);count=inboxCount;std::copy_n(inbox,count,incoming);inboxCount=0;ReleaseSRWLockExclusive(&lock);
    const auto base=RaceStateSnapshot();
    for(size_t i=0;i<count;++i) {
        const auto& packet=incoming[i];
        if(!state.host&&(packet.mask&(1u<<state.context.index))) {
            if(!base.BothFinished())continue;
            const auto& row=packet.heats[state.context.index].rows[1];const auto& own=base.finish[1];
            if(row.status!=own.status||row.ticks!=own.ticks)continue;
        }
        state.Receive(packet);
    }
    if(flow==6&&Current()&&base.ResultReady()) {
        if(state.host&&!(state.mask&(1u<<state.context.index))) {
            coop_url::Heat heat{};if(CaptureCoopUrlHeat(heat)&&state.Seal(heat))
                LogMessage("[URL_HEAT] Both final humans and Host AI standings sealed; awaiting exact peer receipt.\r\n");
        }
        if(state.Ready()&&ApplyCoopUrlHeat(state.heats[state.context.index]))appliedEpoch=state.context.heatEpoch;
        if(state.host&&state.localNext&&state.peerNext&&!state.nextEpoch[state.context.index]) {
            const auto epoch=NextRaceNonce(gRaceLastHostEpoch);
            if(state.OfferNext(epoch))gRaceLastHostEpoch=epoch;
        }
        if(state.NextReady()&&GetTickCount()-lastTry>=250){lastTry=GetTickCount();PrepareNext();}
    }
    Publish();Trace(state.Final()?"final-standings":"heat-consensus");
}
