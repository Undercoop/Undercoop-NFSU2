// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Included immediately after CoopCareerNative.inl. Game thread owns State and
// native calls; the network thread only copies bounded packets through a mailbox.
// Boot logos before a joined career are outside this adapter's scope.
#ifndef COOP_MOVIE_SYNC_NATIVE_INCLUDED
#define COOP_MOVIE_SYNC_NATIVE_INCLUDED
#include "CoopMovieSync.h"
bool CoopFinishSceneScopeMatches();
bool CoopPeerMoviePresentationReady();
bool CoopMovieSourceGone(const char*);
coop_race::State RaceStateSnapshot();
namespace CoopMovieNative {
using StartEventFn=void(__cdecl*)(const char*,const void*);
using StartNameFn=void(__cdecl*)(const char*,const char*);
using PostRaceFn=void(__thiscall*)(void*);
using PostRaceUiFn=void(__thiscall*)(void*,uint32_t,uintptr_t,uintptr_t,uintptr_t);
using SelectResultFn=bool(__thiscall*)(void*,char*);
using ShowFn=void(__thiscall*)(void*,const char*,const char*,int);
using GlobalFn=void(__cdecl*)();
using FlowFn=int(__thiscall*)(void*,int,int);
using CloseFn=void(__cdecl*)(const char*);
using OverlayFn=void(__thiscall*)(void*,int,const char*,const char*,int,int);
using RaceQueryFn=bool(__cdecl*)();using RaceSourceFn=const char*(__cdecl*)();
static StartEventFn gBefore=reinterpret_cast<StartEventFn>(0x53D630);
static StartNameFn gAfter=reinterpret_cast<StartNameFn>(0x53D7A0);
static StartEventFn gTutorial=reinterpret_cast<StartEventFn>(0x557DF0);
static PostRaceFn gPostRace=reinterpret_cast<PostRaceFn>(0x4A3E30);
static PostRaceUiFn gPostRaceUi=reinterpret_cast<PostRaceUiFn>(0x4F39A0);
static SelectResultFn gSelectResult=reinterpret_cast<SelectResultFn>(0x54C9D0);
static ShowFn gShow=reinterpret_cast<ShowFn>(0x5377A0);
static GlobalFn gPrepareIntroA=reinterpret_cast<GlobalFn>(0x4A78E0);
static GlobalFn gPrepareIntroB=reinterpret_cast<GlobalFn>(0x4C57C0);
static GlobalFn gTutorialContinuation=reinterpret_cast<GlobalFn>(0x499560);
static PostRaceFn gPreparePostMovie=reinterpret_cast<PostRaceFn>(0x43B640);
static FlowFn gFlowMessage=reinterpret_cast<FlowFn>(0x5F41E0);
static CloseFn gCloseMovie=reinterpret_cast<CloseFn>(0x558F00);
static PostRaceFn gMovieContinuation=reinterpret_cast<PostRaceFn>(0x54AF80);
static OverlayFn gOverlay=reinterpret_cast<OverlayFn>(0x555D00);
static RaceQueryFn gRaceQuery=reinterpret_cast<RaceQueryFn>(0x4A9330);
static RaceSourceFn gRaceSource=reinterpret_cast<RaceSourceFn>(0x4A9340);
static bool (*gSourceReady)(const char*)=IsNativeCoopCareerSourceReady;
static bool (*gPeerPresentationReady)()=CoopPeerMoviePresentationReady;
static bool (*gSourceGone)(const char*)=CoopMovieSourceGone;
struct Addresses {uintptr_t playlist=0x8382A0,index=0x83856C,ui=0x8384C4,
    career=0x85AD40,eventStrings=0x838428,raceFlow=0x8900D8,nis=0x827E88,
    gameFlow=0x8654A4,inputHashes=0x7F7D20;};
static Addresses gAddresses;
static coop_movie::State gState;
static SRWLOCK gMailboxLock=SRWLOCK_INIT;
static std::array<coop_movie::Packet,16> gInbox{};
static size_t gInboxCount=0;
static coop_movie::Packet gOutbound;
static bool gHaveOutbound=false,gInstalled=false,gStarting=false;
static uint32_t gSentSequence=0,gAppliedScene=0,gAppliedOrdinal=~uint32_t(0);
static uint64_t gBoundSession=0;static uint32_t gBoundTicket=0;
static char gSource[64]{};
static uint8_t gContinuation=0;
static bool gNativeStarted=false,gNaturalPending=false,gTerminalHeld=false,gDisconnected=false;
static bool gRootCommitted=false,gAbandonHandled=false,gContinuationDelivered=false;
static coop_movie::Playlist gLocalPlan;
struct Request {
    uint32_t event=0,epoch=0;uint8_t flow=0,continuation=0;
    bool nullTutorial=false,root=false;char source[64]{};coop_movie::Playlist plan;
};
static Request gCurrent;
// A peer can watch a tutorial before reaching its own native trigger. Retain
// that unclaimed presentation, then deliver the later local continuation once
// without asking both players to watch the same clip a second time.
struct TutorialReceipt {
    uint32_t event=0,epoch=0,scene=0,claimedScene=0;
    coop_movie::Playlist plan;
    char source[64]{};
    bool valid=false,claimed=false,pending=false,closeRequested=false;
};
static std::array<TutorialReceipt,16> gTutorialReceipts{};
static size_t gTutorialReceiptNext=0;
static std::set<std::string> gTutorialCloseNames; // Native removal may borrow the pointer beyond this tick.
static std::array<Request,8> gPending;
static size_t gPendingCount=0;static bool gHaveCurrent=false,gCapturingRoot=false,gNullTutorial=false;
static uint32_t gRootEvent=0,gRootEpoch=0;
static uintptr_t gRootOwner=0;
static DWORD gLastRootWait=0;
static DWORD gLastPrepareWait=0;
static DWORD gLastPeerSurfaceWait=0;
struct RetiredRaceContext {uint32_t event=0,epoch=0;};
static std::array<RetiredRaceContext,32> gRetiredRaceEpochs{};
static size_t gRetiredRaceEpochNext=0;
static uint32_t gRestartMovieEvent=0,gRestartMovieEpoch=0;
static uint64_t gStatePacketsAccepted=0,gStatePacketsRejected=0;
static std::atomic<uint64_t> gMailboxReceived{0},gMailboxOverflow{0},gNetworkSnapshots{0};
static void SelectCurrent(const Request&);
static bool NativeDecoderIdle();
static bool RetiredRace(uint8_t flow,uint32_t epoch,uint32_t event) {
    if((flow!=1&&flow!=4)||!epoch)return false;
    for(const auto& retired:gRetiredRaceEpochs)if(retired.epoch==epoch&&retired.event==event)return true;
    return false;
}
static bool RetiredContext(uint8_t flow,uint32_t event,uint32_t epoch) {
    if(RetiredRace(flow,epoch,event))return true;
    if((flow!=1&&flow!=4)||!gRestartMovieEvent||event!=gRestartMovieEvent)return false;
    // Only the current verified native event or the just-committed new epoch
    // may introduce another movie for this event. In particular an old
    // high-sequence pre-race snapshot cannot resurrect its retired scene.
    if(epoch==gRestartMovieEpoch)return false;
    return !(epoch&&gPendingRaceEvent.sessionId==gBoundSession&&
        gPendingRaceEvent.careerEventHash==event&&gPendingRaceEvent.seed==epoch);
}
static bool RetiredPacket(const coop_movie::Packet& packet) {return RetiredContext(packet.flow,packet.event,packet.epoch);}
static void FilterRetiredAlternatives(coop_movie::Packet& packet) {
    if(packet.alternativeCount>8)return;
    // Preserve normal strict refusal of malformed trailing contexts. Filtering
    // a retired offer must never turn a corrupt packet into a valid one.
    for(size_t i=0;i<8;++i){const auto& c=packet.alternatives[i];
        if(i<packet.alternativeCount){if(c.flow!=1&&c.flow!=2&&c.flow!=4&&c.flow!=6)return;}
        else if(c.flow||c.event||c.epoch)return;
    }
    uint8_t keep=0;
    for(uint8_t i=0;i<packet.alternativeCount;++i){const auto c=packet.alternatives[i];
        if(!RetiredContext(c.flow,c.event,c.epoch))packet.alternatives[keep++]=c;}
    packet.alternativeCount=keep;for(size_t i=keep;i<8;++i)packet.alternatives[i]={};
}
static bool CanPresentPeerIntro(const coop::SessionSnapshot& session) {
    // The local Resume has already committed its own native continuation.
    // Either role may have no personal opening scene. Its peer's introduction
    // is presentation only; it must not replay/reset this player's career.
    return (session.role==coop::Role::Host||session.role==coop::Role::Client)&&
        session.careerLaunched&&session.ticket&&gNativeCareerReleasedTicket==session.ticket&&
        (session.phase==coop::Phase::InitialLoading||session.phase==coop::Phase::Playing)&&
        gCareerLaunchKind==1&&!gCareerLaunchPending;
}
static bool SameRequest(const Request& a,const Request& b) {
    return coop_movie::SameContext(a.flow,a.event,a.epoch,b.flow,b.event,b.epoch)&&
        a.nullTutorial==b.nullTutorial&&!std::strcmp(a.source,b.source)&&
        coop_movie::Digest(a.plan)==coop_movie::Digest(b.plan);
}
static bool SameTutorial(const TutorialReceipt& seen,const Request& request) {
    if(!seen.valid || request.flow!=2 || seen.event!=request.event || seen.epoch!=request.epoch ||
        seen.plan.count!=request.plan.count)return false;
    for(unsigned i=0;i<seen.plan.count;++i)
        if(!coop_movie::SameName(seen.plan.names[i],request.plan.names[i]))return false;
    return true;
}
static void RememberPassiveTutorial() {
    if(!gHaveCurrent || gCurrent.flow!=2 || gCurrent.continuation!=3 || !gState.Plan().count)return;
    TutorialReceipt seen;seen.event=gCurrent.event;seen.epoch=gCurrent.epoch;seen.scene=gState.Scene();
    seen.plan=gState.Plan();seen.valid=true;
    for(auto& previous:gTutorialReceipts) {
        if(previous.valid&&previous.event==seen.event&&previous.epoch==seen.epoch) {
            if(previous.scene==seen.scene&&previous.claimed)return;
            previous=seen;return;
        }
    }
    gTutorialReceipts[gTutorialReceiptNext++%gTutorialReceipts.size()]=seen;
}
static bool AttachLocalTutorial(const Request& request) {
    if(request.flow!=2 || request.continuation!=2 || !request.plan.count)return false;
    if(gHaveCurrent && gCurrent.flow==2 && gCurrent.continuation==3 && !gContinuationDelivered &&
        coop_movie::SameContext(request.flow,request.event,request.epoch,gCurrent.flow,gCurrent.event,gCurrent.epoch)) {
        TutorialReceipt presenting;presenting.event=gCurrent.event;presenting.epoch=gCurrent.epoch;
        presenting.plan=gState.Plan();presenting.valid=true;
        if(SameTutorial(presenting,request)) {
            if(!gNativeStarted) {
                // No UI was presented yet. Use the real accepted source and
                // callback for this same paired scene, without a new offer.
                SelectCurrent(request);
            } else {
                // Keep the live passive overlay's no-op cleanup. Its accepted
                // local source and career continuation are retired afterwards.
                RememberPassiveTutorial();
                for(auto& seen:gTutorialReceipts)if(SameTutorial(seen,request)) {
                    seen.claimed=seen.pending=true;seen.claimedScene=gState.Scene();
                    std::memcpy(seen.source,request.source,sizeof(seen.source));break;
                }
            }
            LogMessage("[MOVIE_SYNC] Local tutorial attached to the existing peer presentation; no second movie request.\r\n");
            return true;
        }
    }
    for(auto& seen:gTutorialReceipts) {
        if(!SameTutorial(seen,request))continue;
        if(seen.claimed) {
            if(seen.claimedScene==gState.Scene())return true;
            continue; // A later genuine episode may request the same tutorial.
        }
        seen.claimed=true;seen.pending=true;seen.claimedScene=gState.Scene();
        std::memcpy(seen.source,request.source,sizeof(seen.source));
        LogMessage("[MOVIE_SYNC] Already presented peer tutorial claimed by the local native trigger; continuation queued without replay.\r\n");
        return true;
    }
    return false;
}
static void DeliverTutorialContinuations() {
    if((gState.Active()&&!gContinuationDelivered)||(gNativeStarted&&!gContinuationDelivered)||
        gPendingTrackLoader||!NativeDecoderIdle())return;
    uint32_t gameFlow=0,nativeState=0;uintptr_t raceFlow=0;
    if(!ReadGameMemory(gAddresses.gameFlow,&gameFlow,4)||gameFlow!=6||
        !ReadGameMemory(gAddresses.raceFlow,&raceFlow,4)||!IsValidUserPtr(raceFlow)||
        !ReadGameMemory(raceFlow,&nativeState,4)||(nativeState!=4&&nativeState!=6))return;
    for(auto& seen:gTutorialReceipts) {
        if(!seen.pending)continue;
        if(seen.source[0]&&!gSourceGone(seen.source)) {
            // This is the already accepted native producer's OWN source, not
            // an unrelated peer prompt. Close once, then wait for actual FEng
            // removal/input rebuilding on a later tick before its callback.
            if(!seen.closeRequested&&gSourceReady(seen.source)) {
                seen.closeRequested=true;
                const auto stable=gTutorialCloseNames.emplace(seen.source).first;
                gCloseMovie(stable->c_str());
            }
            continue;
        }
        if(!gPeerPresentationReady())continue;
        // Run on the next game/UI tick, after the Enter-Shop producer's native
        // callback has unwound. Never reenter its menu handler synchronously.
        seen.pending=false;
        gTutorialContinuation();
        LogMessage("[MOVIE_SYNC] Delayed local tutorial continuation delivered once; shared movie was not replayed.\r\n");
    }
}
static uint32_t RequestEpoch(uint32_t flow,uint32_t event) {
    const auto s=CoopSnapshot();
    return (flow==1||flow==4)&&event&&gPendingRaceEvent.sessionId==s.sessionId&&
        gPendingRaceEvent.careerEventHash==event?gPendingRaceEvent.seed:0;
}
static void SelectCurrent(const Request& request) {
    gCurrent=request;gHaveCurrent=true;std::memcpy(gSource,request.source,sizeof(gSource));
    gContinuation=request.continuation;gNullTutorial=request.nullTutorial;gLocalPlan=request.plan;
    gNativeStarted=gNaturalPending=gDisconnected=gContinuationDelivered=false;gTerminalHeld=request.root;
}
static bool OfferRequest(const Request& request) {
    if(!gState.Offer(request.flow,request.event,request.plan,request.epoch))return false;
    SelectCurrent(request);return true;
}
static bool QueueRequest(const Request& request) {
    for(size_t i=0;i<gPendingCount;++i)if(SameRequest(gPending[i],request))return true;
    if(gPendingCount==gPending.size()) {
        LogMessage("[MOVIE_SYNC] Bounded native request queue exhausted; unexpected distinct native producers.\r\n");return false;
    }
    gPending[gPendingCount++]=request;return true;
}
static bool AdoptPeerRequest();
static void DrainRequests() {
    if(gState.CurrentPhase()==coop_movie::Phase::Done) {
        if(!gContinuationDelivered||!gPendingCount)return;
        gState.BeginNext();gHaveCurrent=false;
    }
    if(gState.LocalOffered()) {
        if(gState.CurrentPhase()!=coop_movie::Phase::Preparing||!gState.PeerKnown()||gState.LocalMatchesPeer())return;
        // Host preference is stable when the Client already has that native
        // context in its queue. Without this catalog, simultaneous swaps can
        // chase each other indefinitely after crossed request packets.
        if(gState.IsHost()&&gState.PeerCanOffer(gCurrent.flow,gCurrent.event,gCurrent.epoch))return;
        for(size_t i=0;i<gPendingCount;++i) {
            const auto next=gPending[i];
            if(!coop_movie::SameContext(next.flow,next.event,next.epoch,gState.PeerFlow(),gState.PeerEvent(),gState.PeerEpoch()))continue;
            if(gState.ReplaceOffer(next.flow,next.event,next.plan,next.epoch)) {
                gPending[i]=gCurrent;SelectCurrent(next);return;
            }
        }
        if(!gState.IsHost())AdoptPeerRequest();
        return;
    }
    if(!gPendingCount)return;
    size_t chosen=0;
    if(gState.PeerKnown()) {
        for(;chosen<gPendingCount;++chosen)if(coop_movie::SameContext(gPending[chosen].flow,gPending[chosen].event,gPending[chosen].epoch,
            gState.PeerFlow(),gState.PeerEvent(),gState.PeerEpoch()))break;
        if(chosen==gPendingCount)return;
    }
    const auto next=gPending[chosen];if(!OfferRequest(next))return;
    for(size_t i=chosen+1;i<gPendingCount;++i)gPending[i-1]=gPending[i];--gPendingCount;
}
static void Log(const char* event) {
    char line[512];std::snprintf(line,sizeof(line),
        "[MOVIE_SYNC] %s scene=%u ordinal=%u flow=%u localContinuation=%u clips=%u movie=%s source=%s votes=%u/%u requestFlow=%u event=%08X epoch=%u requestedClips=%u requestedMovie=%s overlay=%u.\r\n",
        event,gState.Scene(),gState.Ordinal(),gState.Flow(),gContinuation,gState.Plan().count,
        gState.Ordinal()<gState.Plan().count?gState.Plan().names[gState.Ordinal()]:"none",gSource,
        unsigned(gState.LocalVote()),unsigned(gState.PeerVote()),unsigned(gCurrent.flow),gCurrent.event,gCurrent.epoch,
        unsigned(gCurrent.plan.count),gCurrent.plan.count?gCurrent.plan.names[0]:"none",unsigned(gNullTutorial));LogMessage(line);
}
static bool Scope(coop::SessionSnapshot& session) {
    session=CoopSnapshot();return gInstalled && IsCoopGameplayActive() &&
        session.joined && session.fresh && session.ticket && session.sessionId &&
        (session.role==coop::Role::Host || session.role==coop::Role::Client);
}
static void Bind(const coop::SessionSnapshot& session) {
    if(gBoundSession==session.sessionId && gBoundTicket==session.ticket) return;
    gState.Bind(session.sessionId,session.ticket,session.role==coop::Role::Host);
    gBoundSession=session.sessionId;gBoundTicket=session.ticket;gSource[0]=0;gContinuation=0;
    gNativeStarted=gNaturalPending=gTerminalHeld=gDisconnected=gRootCommitted=gAbandonHandled=gContinuationDelivered=false;gLocalPlan={};
    gAppliedScene=0;gAppliedOrdinal=~uint32_t(0);
    gCurrent={};gHaveCurrent=gCapturingRoot=gNullTutorial=false;gPendingCount=0;gRootEvent=gRootEpoch=0;
    gTutorialReceipts={};gTutorialReceiptNext=0;
    gRootOwner=0;gLastRootWait=gLastPrepareWait=gLastPeerSurfaceWait=0;gStatePacketsAccepted=gStatePacketsRejected=0;
    gRetiredRaceEpochs={};gRetiredRaceEpochNext=0;gRestartMovieEvent=gRestartMovieEpoch=0;
    AcquireSRWLockExclusive(&gMailboxLock);gInboxCount=0;gHaveOutbound=false;gSentSequence=0;ReleaseSRWLockExclusive(&gMailboxLock);
}
static bool ReadText(const char* source,char (&out)[64]) {
    std::memset(out,0,sizeof(out));if(!source) return false;
    // Native 4A3E30 supplies its movie name on the stack. ReadGameMemory's
    // game-pointer range intentionally excludes low x86 stacks, so it cannot
    // validate this string. Bounded process reads accept stack text as well as
    // native data without retaining its address or reading past its terminator.
    for(size_t i=0;i<sizeof(out);++i) {
        SIZE_T read=0;
        if(!ReadProcessMemory(GetCurrentProcess(),source+i,out+i,1,&read)||read!=1) return false;
        if(!out[i]) return i!=0;
    }
    return false;
}
static bool FindLivePeerSource(char (&out)[64]) {
    if(ReadText(gCurrentNativeScreen.c_str(),out)&&gSourceReady(out)) return true;
    // This native package is observed in both Explore and result logs and is
    // independent of a destroyed UI_PostRace/PC_Loading package.
    if(gSourceReady("UI_PC_Help_Bar.fng")) {std::strcpy(out,"UI_PC_Help_Bar.fng");return true;}
    return false;
}
static void PassivePresentation(Request& request) {
    request.continuation=3;
    // Explore need not retain the most recently pushed menu/loading package,
    // and the help bar is not guaranteed to be a node in UIData's source queue.
    // A passive peer has no source screen or career callback to replace. The
    // stock NULL-source movie overlay (557DF0) can present the shared playlist
    // without inventing a source node, then flow3 closes it without launching.
    request.nullTutorial=request.flow==2||!FindLivePeerSource(request.source);
    if(request.nullTutorial)request.source[0]=0;
}
static bool AdoptPeerRequest() {
    Request passive;passive.flow=gState.PeerFlow();passive.event=gState.PeerEvent();passive.epoch=gState.PeerEpoch();
    if(passive.flow==2&&!gPeerPresentationReady())return false;
    if(passive.flow!=2) {
        const auto race=RaceStateSnapshot();bool safe=false;
        if(passive.flow==1)safe=race.session==gBoundSession&&race.token==passive.epoch&&race.ready&&!race.startCommit;
        if(passive.flow==4) {
            uintptr_t native=0;uint32_t track=0,nativeState=0;
            safe=!CoopFinishRaceActive()&&ReadGameMemory(gRaceContextAddresses.track,&track,4)&&track==4000&&
                ReadGameMemory(gRaceContextAddresses.nativeFlow,&native,4)&&IsValidUserPtr(native)&&
                ReadGameMemory(native,&nativeState,4)&&nativeState==4;
        }
        if(!safe)return false;
    }
    PassivePresentation(passive);
    if(gPendingCount==gPending.size())return false;
    // The local real continuation is parked, NOT replaced/discarded. Client
    // plays the Host's safe passive scene first, then offers its original one.
    if(!gState.ReplaceOffer(passive.flow,passive.event,passive.plan,passive.epoch))return false;
    QueueRequest(gCurrent);SelectCurrent(passive);Log("matched-peer-first");return true;
}
static bool EventName(const void* event,size_t field,char (&out)[64],uint32_t& hash) {
    uintptr_t strings=0;uint16_t offset=0;const uintptr_t at=reinterpret_cast<uintptr_t>(event);
    return ReadGameMemory(at+8,&hash,4) && ReadGameMemory(gAddresses.eventStrings,&strings,4) &&
        ReadGameMemory(at+field,&offset,2) && ReadText(reinterpret_cast<const char*>(strings+offset),out);
}
static void ContinueWithoutMovie() {
    if(gContinuationDelivered)return;gContinuationDelivered=true;
    if(gContinuation==4) {
        uintptr_t raceFlow=0;ReadGameMemory(gAddresses.raceFlow,&raceFlow,4);
        // The original root callback also requires this native RaceFlow object.
        gFlowMessage(reinterpret_cast<void*>(raceFlow),0xB,0);
    }
    gTerminalHeld=false;
}
static bool WritePlan(const coop_movie::Playlist& plan) {
    const uint32_t index=0,flow=gContinuation?gContinuation:3;
    return SafeWriteBytes(gAddresses.playlist,plan.names,sizeof(plan.names)) &&
        SafeWriteBytes(gAddresses.index,&index,4) &&
        SafeWriteBytes(CoopCareerNative::gMovieGlobals.flow,&flow,4);
}
static bool StartNative(const coop_movie::Playlist& plan) {
    if(gContinuation==3&&gCurrent.flow==2&&!gPeerPresentationReady())return false;
    if(!gNullTutorial&&(!gSource[0] || !gSourceReady(gSource))) return false;
    uintptr_t ui=0;if(!ReadGameMemory(gAddresses.ui,&ui,4) || !IsValidUserPtr(ui)) return false;
    if(!WritePlan(plan)) return false;
    gStarting=true;
    if(gContinuation==6) {gPrepareIntroA();gPrepareIntroB();}
    if(gContinuation==4) gPreparePostMovie(reinterpret_cast<void*>(gAddresses.nis));
    if(gNullTutorial) {
        // Exact original557DF0 NULL-source branch: overlay slot4 only when
        //4A9330 finds PC_Loading AND4A9340 returns its CURRENT source package.
        // Never retain that returned pointer after this native call.
        const char* source=gRaceQuery()?gRaceSource():nullptr;
        gOverlay(reinterpret_cast<void*>(ui),source?4:0,"IG_PlayMovie.fng",source,0xFF,0);
    } else gShow(reinterpret_cast<void*>(ui),gSource,"IG_PlayMovie.fng",0);
    gStarting=false;gNativeStarted=true;gNaturalPending=false;gContinuationDelivered=false;
    CoopCareerNative::gPromptOwner=0;CoopCareerNative::gEnterHeld=false;
    Log("native-start");return true;
}
static bool NativeDecoderIdle() {
    uintptr_t player=0;uint8_t busy=0;
    if(!ReadGameMemory(CoopCareerNative::gMovieGlobals.player,&player,4)) return false;
    return !player || (IsValidUserPtr(player)&&ReadGameMemory(player+0x34,&busy,1)&&!busy);
}
static bool IsLiveMovie(void* object) {
    uintptr_t vtable=0,package=0;char name[64]{};const uintptr_t at=reinterpret_cast<uintptr_t>(object);
    return ReadGameMemory(at,&vtable,4)&&vtable==0x79CB44&&ReadGameMemory(at+4,&package,4)&&
        ReadText(reinterpret_cast<const char*>(package),name)&&std::strcmp(name,"IG_PlayMovie.fng")==0;
}
static void FrameOnce(void* object,uint32_t message,uintptr_t a,uintptr_t b,uintptr_t c) {
    uint8_t finished=0;
    if(message==CoopCareerNative::kFrameUpdate &&
       ReadGameMemory(reinterpret_cast<uintptr_t>(object)+0x50,&finished,1) && finished) {
        if(gContinuationDelivered) return;
        // Record before calling native cleanup: it can reenter the UI dispatcher.
        RememberPassiveTutorial();
        gContinuationDelivered=true;gTerminalHeld=false;
        if(gContinuation==3) {
            // A peer already in Explore has no race/tutorial continuation.
            // Use the native frontend/no-op cleanup branch directly; loading
            // PC_Loading here would wait for a world transition that never ran.
            uintptr_t package=0;ReadGameMemory(reinterpret_cast<uintptr_t>(object)+4,&package,4);
            CoopCareerNative::gStopExpiredMovie();gCloseMovie(reinterpret_cast<const char*>(package));
            gMovieContinuation(object);return;
        }
    }
    CoopCareerNative::gMovieHandler(object,message,a,b,c);
}
static void Publish(uint32_t now) {
    std::array<coop_movie::Context,8> alternatives{};
    for(size_t i=0;i<gPendingCount;++i)alternatives[i]={gPending[i].event,gPending[i].epoch,gPending[i].flow};
    gState.Alternatives(alternatives.data(),gPendingCount);
    coop_movie::Packet packet;
    if(!gState.Build(now,packet)) return;
    AcquireSRWLockExclusive(&gMailboxLock);gOutbound=packet;gHaveOutbound=true;ReleaseSRWLockExclusive(&gMailboxLock);
}
static void __cdecl Before(const char* source,const void* event) {
    char name[64]{};uint32_t hash=0;
    if(EventName(event,2,name,hash)&&CoopMovieCaptureStart(1,source,name,hash)) return;
    gBefore(source,event);
}
static void __cdecl After(const char* source,const char* name) {
    coop::SessionSnapshot session;if(!Scope(session)){gAfter(source,name);return;}
    uintptr_t event=0;uint32_t hash=0;
    if(ReadGameMemory(gAddresses.career+0x8730,&event,4)&&IsValidUserPtr(event))ReadGameMemory(event+8,&hash,4);
    if(!hash&&CoopFinishSceneScopeMatches())hash=gPendingRaceEvent.careerEventHash;
    if(CoopMovieCaptureStart(4,source,name,hash)) return;gAfter(source,name);
}
static void __cdecl Tutorial(const char* source,const void* descriptor) {
    const char* name=reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(descriptor)+0x20);
    if(CoopMovieCaptureStart(2,source,name,0)) return;gTutorial(source,descriptor);
}
static void __fastcall PostRace(void* object,void*) {
    coop::SessionSnapshot session;if(!Scope(session)) {gPostRace(object);return;}
    Bind(session);
    uintptr_t source=0,event=0;uint32_t hash=0;char sourceName[64]{};
    ReadGameMemory(gAddresses.career+0x8730,&event,4);
    if(event)ReadGameMemory(event+8,&hash,4);
    const uint32_t epoch=RequestEpoch(4,hash);
    if(gRootCommitted&&(!event||(hash==gRootEvent&&epoch==gRootEpoch)))return;
    if(!ReadGameMemory(reinterpret_cast<uintptr_t>(object)+4,&source,4) ||
       !ReadText(reinterpret_cast<const char*>(source),sourceName) || !gSourceReady(sourceName)) {
        gPostRace(object);return;
    }
    // The original commit and scene selection execute exactly once. Its return
    // string is copied immediately, before the stack and active event disappear.
    gRootCommitted=true;gRootEvent=hash;gRootEpoch=epoch;
    gRootOwner=reinterpret_cast<uintptr_t>(object);gLastRootWait=0;
    char movie[64]{};const bool haveMovie=gSelectResult(reinterpret_cast<void*>(gAddresses.career),movie);
    gCapturingRoot=true;const bool captured=CoopMovieCaptureStart(4,sourceName,haveMovie?movie:nullptr,hash);gCapturingRoot=false;
    if(captured)return;
    // This branch is only an invalid native movie name; retain native behavior,
    // without replaying the already committed root callback a second time.
    if(haveMovie) gAfter(sourceName,movie);else {
        gContinuation=4;ContinueWithoutMovie();
    }
}
// 54C9D0 intentionally clears the completed event before the paired movie
// barrier. A second PostRace carousel click otherwise reaches 5287A0 through
// 543D40 BEFORE PostRace's root duplicate guard and dereferences event+0xF.
// Hold only input to that exact committed old result owner/epoch. Native
// frame, save-completion and movie cleanup messages continue unchanged.
static bool HoldCommittedResultInput(void* object,uint32_t message) {
    if(!gRootCommitted || gRootOwner!=reinterpret_cast<uintptr_t>(object) || !message ||
        message==0xC98356BA || message==0xE1FDE1D1)return false;
    switch(message) {
    case 0x0C407210: case 0x406415E3: case 0x911AB364:
    case 0x3411395C: case 0x35E413DD: case 0x24DD40CF:
    case 0x30ED2368: case 0xD78D7BF1: case 0x33C42E03:
    case 0x451E768E: case 0xE1A57D51: break;
    default: {
        // Include controller/remapped carousel actions too, not just Enter
        // and the mouse-generated widget accept. This is50B2A0's read-only
        // classification, with no native callback or dispatcher side effect.
        bool input=false;uint32_t hash=0;
        for(unsigned i=0;i<19&&!input;++i) {
            if(ReadGameMemory(gAddresses.inputHashes+i*4,&hash,4)&&hash==message)input=true;
            if(ReadGameMemory(gAddresses.inputHashes+0x58+i*4,&hash,4)&&hash==message)input=true;
        }
        for(unsigned i=0;i<2&&!input;++i)if(ReadGameMemory(gAddresses.inputHashes+0x4C+i*4,&hash,4)&&hash==message)input=true;
        if(!input && message!=0x936A6A7F)return false;
        break;
    }
    }
    uintptr_t flow=0;uint32_t state=0,gameFlow=0;
    // Peer timeout cannot resurrect the event already consumed by54C9D0.
    // Retain ownership of this exact old world until the local continuation
    // or profile teardown, even before the disconnect fallback's next tick.
    return gInstalled && IsCoopGameplayActive() && gRootCommitted && gRootOwner==reinterpret_cast<uintptr_t>(object) &&
        gRootEvent && gRootEpoch && gBoundSession && gBoundTicket &&
        gPendingRaceEvent.sessionId==gBoundSession &&
        (gPendingRaceEvent.seed==gRootEpoch||gPendingRaceEvent.parentEpoch==gRootEpoch) &&
        gPendingRaceEvent.careerEventHash==gRootEvent &&
        ReadGameMemory(gAddresses.gameFlow,&gameFlow,4) && gameFlow==6 &&
        ReadGameMemory(gAddresses.raceFlow,&flow,4) && IsValidUserPtr(flow) &&
        ReadGameMemory(flow,&state,4) && state==7;
}
static void __fastcall PostRaceUi(void* object,void*,uint32_t message,uintptr_t a,uintptr_t b,uintptr_t c) {
    if(HoldCommittedResultInput(object,message)) {
        char line[224];std::snprintf(line,sizeof(line),
            "[POSTRACE_RETURN] Duplicate input held owner=%08X message=%08X event=%08X epoch=%u phase=%u delivered=%u.\r\n",
            unsigned(gRootOwner),message,gRootEvent,gRootEpoch,unsigned(gState.CurrentPhase()),unsigned(gContinuationDelivered));
        LogMessage(line);return;
    }
    gPostRaceUi(object,message,a,b,c);
}
struct Hook {uintptr_t address;const BYTE* bytes;size_t length;void* replacement;BYTE* trampoline=nullptr;bool installed=false;};
static constexpr BYTE kBefore[]{0xC7,0x05,0x68,0x85,0x83,0,1,0,0,0};
static constexpr BYTE kAfter[]{0xB8,0xA0,0x82,0x83,0}; // 53D7B0 branches to entry+5.
static constexpr BYTE kTutorial[]{0xC7,0x05,0x68,0x85,0x83,0,2,0,0,0};
static constexpr BYTE kPost[]{0x83,0xEC,0x40,0x56,0x8D,0x44,0x24,0x04};
static constexpr BYTE kPostUi[]{0x55,0x8B,0xEC,0x8B,0x45,0x14};
static Hook gHooks[]{
    {0x53D630,kBefore,sizeof(kBefore),reinterpret_cast<void*>(&Before)},
    {0x53D7A0,kAfter,sizeof(kAfter),reinterpret_cast<void*>(&After)},
    {0x557DF0,kTutorial,sizeof(kTutorial),reinterpret_cast<void*>(&Tutorial)},
    {0x4A3E30,kPost,sizeof(kPost),reinterpret_cast<void*>(&PostRace)},
    {0x4F39A0,kPostUi,sizeof(kPostUi),reinterpret_cast<void*>(&PostRaceUi)}
};
static void PublishOriginal(size_t index,uintptr_t original) {
    if(index==0) gBefore=reinterpret_cast<StartEventFn>(original);
    if(index==1) gAfter=reinterpret_cast<StartNameFn>(original);
    if(index==2) gTutorial=reinterpret_cast<StartEventFn>(original);
    if(index==3) gPostRace=reinterpret_cast<PostRaceFn>(original);
    if(index==4) gPostRaceUi=reinterpret_cast<PostRaceUiFn>(original);
}
using ProtectFn=BOOL(WINAPI*)(LPVOID,SIZE_T,DWORD,PDWORD);
static ProtectFn gProtect=VirtualProtect;
static bool Rollback(size_t index) {
    Hook& hook=gHooks[index];DWORD old=0,ignored=0;
    if(hook.installed) {
        if(!gProtect(reinterpret_cast<void*>(hook.address),hook.length,PAGE_EXECUTE_READWRITE,&old)) return false;
        std::memcpy(reinterpret_cast<void*>(hook.address),hook.bytes,hook.length);
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(hook.address),hook.length);
        gProtect(reinterpret_cast<void*>(hook.address),hook.length,old,&ignored);hook.installed=false;
    }
    PublishOriginal(index,hook.address);
    if(hook.trampoline) {VirtualFree(hook.trampoline,0,MEM_RELEASE);hook.trampoline=nullptr;}
    return true;
}
} // namespace CoopMovieNative

bool CoopMovieCaptureStart(uint32_t flow,const char* source,const char* movie,uint32_t event) {
    using namespace CoopMovieNative;
    coop::SessionSnapshot session;if(gStarting || !Scope(session)) return false;Bind(session);
    Request request;request.flow=request.continuation=uint8_t(flow);request.event=event;
    request.nullTutorial=flow==2&&!source;request.root=gCapturingRoot;
    if(!request.nullTutorial) {
        if(source) {if(!ReadText(source,request.source))return false;}
        else if(!ReadText(gCurrentNativeScreen.c_str(),request.source))return false;
        if(!gSourceReady(request.source))return false;
    }
    if(movie) {
        char name[64]{};if(!ReadText(movie,name) || !coop_movie::Add(request.plan,name)) return false;
        if(flow==1 && _stricmp(name,"Scene20")==0) coop_movie::Add(request.plan,"DriftTut");
        if(flow==4 && _stricmp(name,"Scene07")==0) coop_movie::Add(request.plan,"UpperClass");
    }
    // Tutorial descriptors have no career hash. Their copied playlist is the
    // logical request identity; DragTut must not match an unrelated DriftTut.
    if(flow==2)request.event=coop_movie::Digest(request.plan);
    request.epoch=RequestEpoch(flow,request.event);
    if(RetiredRace(request.flow,request.epoch,request.event))return true;
    if(AttachLocalTutorial(request))return true;
    if(gHaveCurrent&&gState.LocalOffered()&&SameRequest(gCurrent,request))return true;
    if(gState.LocalOffered()||gState.CurrentPhase()==coop_movie::Phase::Done) {
        if(!QueueRequest(request))return false;DrainRequests();Log("queued-request");
    } else if(!OfferRequest(request))return false;
    Log("request");Publish(GetTickCount());return true;
}

bool CoopMovieHandleMessage(void* object,uint32_t message,uintptr_t a,uintptr_t b,uintptr_t c) {
    using namespace CoopMovieNative;using namespace CoopCareerNative;
    if(!gInstalled || !gNativeStarted || !IsLiveMovie(object)) return false;
    coop::SessionSnapshot session;
    if(!Scope(session) || gDisconnected) {
        if(!IsCoopGameplayActive()) {gNaturalPending=gNativeStarted=false;return false;}
        // Replay a withheld natural end on this fresh callback object; no cached
        // object address survives native package destruction or session changes.
        if(gNaturalPending) {
            gNaturalPending=false;gMovieHandler(object,0xC3960EB9,0,0,0);gNativeStarted=false;
            if(message!=0xC3960EB9) FrameOnce(object,message,a,b,c);
            return true;
        }
        gNativeStarted=false;return false;
    }
    if(message==0xC3960EB9) {gNaturalPending=true;gState.End();Log("natural-end");Publish(GetTickCount());return true;}
    if(message==kSkipInputA || message==kSkipInputB) {
        if(CanSkipCurrentMovie(object)) {gState.Vote();Log("vote");Publish(GetTickCount());}
        return true;
    }
    if(message!=kFrameUpdate) return false;
    const bool enter=IsGameWindowFocused()&&(GetAsyncKeyState(VK_RETURN)&0x8000)!=0;
    if(enter&&!gEnterHeld&&CanSkipCurrentMovie(object)) gState.Vote();gEnterHeld=enter;
    coop::SessionSnapshot prompt=session;prompt.ticket=gState.Scene()*5+gState.Ordinal();
    prompt.localSkipVote=gState.LocalVote();prompt.peerSkipVote=gState.PeerVote();prompt.skipDecided=gState.Skip();
    UpdateMovieSkipPrompt(object,prompt);
    const uint32_t now=GetTickCount();gState.Tick(now);
    if(gState.Due(now)==coop_movie::Action::Advance && NativeDecoderIdle() &&
       (gNaturalPending||CanSkipCurrentMovie(object))) {
        if(gAppliedScene!=gState.Scene() || gAppliedOrdinal!=gState.Ordinal()) {
            gAppliedScene=gState.Scene();gAppliedOrdinal=gState.Ordinal();
            gMovieHandler(object,gState.Skip()?kSkipInputB:0xC3960EB9,0,0,0);
            gNaturalPending=false;gState.Applied();gPromptOwner=0;gEnterHeld=false;Log("advance");
        }
    }
    FrameOnce(object,message,a,b,c);Publish(now);return true;
}

void TickCoopMovies(DWORD flow) {
    using namespace CoopMovieNative;
    coop::SessionSnapshot session;
    if(!Scope(session)) {
        if(!IsCoopGameplayActive()) {
            gState=coop_movie::State{};gNativeStarted=gNaturalPending=gTerminalHeld=false;
            gBoundSession=0;gBoundTicket=0;gPendingCount=0;gHaveCurrent=false;
            gRootCommitted=false;gRootOwner=0;gRootEvent=gRootEpoch=0;
            AcquireSRWLockExclusive(&gMailboxLock);gHaveOutbound=false;gInboxCount=0;ReleaseSRWLockExclusive(&gMailboxLock);
            gAbandonHandled=true;return;
        }
        if(gState.Active()&&!gAbandonHandled) {
            gDisconnected=true;
            if(!gNativeStarted && gState.LocalOffered()) {
                if(gLocalPlan.count) {if(!StartNative(gLocalPlan)) return;}
                else ContinueWithoutMovie();
            }
            gAbandonHandled=true;
        }
        return;
    }
    Bind(session);std::array<coop_movie::Packet,16> packets;size_t count=0;
    AcquireSRWLockExclusive(&gMailboxLock);count=gInboxCount;
    std::copy_n(gInbox.begin(),count,packets.begin());gInboxCount=0;ReleaseSRWLockExclusive(&gMailboxLock);
    const uint32_t now=GetTickCount();for(size_t i=0;i<count;++i) {
        if(RetiredPacket(packets[i]))continue;
        FilterRetiredAlternatives(packets[i]);
        const bool wasDone=gState.CurrentPhase()==coop_movie::Phase::Done;
        const bool knewPeer=gState.PeerKnown();
        const uint8_t peerFlow=gState.PeerFlow();const uint32_t peerEvent=gState.PeerEvent(),peerEpoch=gState.PeerEpoch();
        const uint32_t before=gState.Scene();
        const bool accepted=gState.Receive(packets[i],now);
        if(accepted)++gStatePacketsAccepted;else ++gStatePacketsRejected;
        if(accepted&&(!knewPeer||peerFlow!=gState.PeerFlow()||peerEvent!=gState.PeerEvent()||peerEpoch!=gState.PeerEpoch())) {
            char line[192];std::snprintf(line,sizeof(line),"[MOVIE_SYNC] peer-request flow=%u event=%08X epoch=%u clips=%u.\r\n",
                unsigned(gState.PeerFlow()),gState.PeerEvent(),gState.PeerEpoch(),unsigned(packets[i].playlist.count));LogMessage(line);
        }
        if(accepted&&wasDone&&gState.Scene()!=before) {
            gNativeStarted=gNaturalPending=gTerminalHeld=false;gSource[0]=0;gLocalPlan={};
            gContinuationDelivered=false;gHaveCurrent=false;
        }
    }
    // A next-item decoder may finish before the previous advance ACK reaches
    // the Host. Retain that native notification until its ordinal is current.
    uint32_t nativeOrdinal=~uint32_t(0);
    if(gNaturalPending && ReadGameMemory(gAddresses.index,&nativeOrdinal,4) && nativeOrdinal==gState.Ordinal()) gState.End();
    DrainRequests();
    DeliverTutorialContinuations();
    if(gRootCommitted && gTerminalHeld && (!gLastRootWait || now-gLastRootWait>=5000)) {
        const bool firstWait=!gLastRootWait;
        gLastRootWait=now;Log("postrace-wait");
        if(firstWait)CoopAnnouncements::Post("Return to Free Roam is pending. Waiting for your teammate's post-race continuation.");
    }
    const auto race=RaceStateSnapshot();
    const bool peerBeforeGrid=gState.Flow()==1&&race.session==session.sessionId&&race.token==gState.Epoch()&&
        race.ready&&!race.startCommit;
    const bool blockedPeerTutorial=gState.Flow()==2&&!gState.LocalOffered()&&!gPeerPresentationReady();
    if(blockedPeerTutorial&&(!gLastPeerSurfaceWait||coop_time::Elapsed(now,gLastPeerSurfaceWait)>=5000)) {
        gLastPeerSurfaceWait=now;
        CoopAnnouncements::Post("Shared cutscene is waiting. Finish or close your current menu to continue.");
        Log("peer-menu-wait");
    }
    if(!blockedPeerTutorial)gLastPeerSurfaceWait=0;
    if(gState.Active()&&!gState.LocalOffered() && !blockedPeerTutorial && (!CoopFinishRaceActive()||peerBeforeGrid||gState.Flow()==2) &&
       (gState.Flow()!=6 || CanPresentPeerIntro(session))) {
        // A resumed player may have no personal opening movie. A peer which has
        // already returned to Explore uses its own no-op continuation, never a
        // destroyed result handler or the other player's personal career entry.
        if(gState.AcceptPeerOnly(gState.Flow())) {
            Request request;request.flow=gState.Flow();request.continuation=3;request.event=gState.Event();request.epoch=gState.Epoch();
            PassivePresentation(request);SelectCurrent(request);Log(request.nullTutorial?"peer-only-overlay":"peer-only-source");
        }
    }
    const bool passiveSurfaceReady=gContinuation!=3||gCurrent.flow!=2||gPeerPresentationReady();
    if(gState.CurrentPhase()==coop_movie::Phase::Preparing && passiveSurfaceReady && (gNullTutorial||(gSource[0]&&gSourceReady(gSource))))
        gState.ReadyForStart();
    if(gState.CurrentPhase()==coop_movie::Phase::Preparing&&
        (!gLastPrepareWait||coop_time::Elapsed(now,gLastPrepareWait)>=5000)){
        gLastPrepareWait=now;
        char line[704];std::snprintf(line,sizeof(line),
            "[MOVIE_WAIT] session=%llu ticket=%u role=%u gameFlow=%u scene=%u flow=%u localOffer=%u peerKnown=%u matches=%u source='%s' sourceReady=%u overlay=%u launchKind=%u released=%u pending=%u stateAccepted=%llu stateRejected=%llu rxTotal=%llu rxOverflow=%llu txSnapshots=%llu.\r\n",
            static_cast<unsigned long long>(session.sessionId),session.ticket,unsigned(session.role),flow,gState.Scene(),unsigned(gState.Flow()),
            unsigned(gState.LocalOffered()),unsigned(gState.PeerKnown()),unsigned(gState.LocalMatchesPeer()),gSource,
            unsigned(gSource[0]&&gSourceReady(gSource)),unsigned(gNullTutorial),gCareerLaunchKind,gNativeCareerReleasedTicket,unsigned(gCareerLaunchPending),
            static_cast<unsigned long long>(gStatePacketsAccepted),static_cast<unsigned long long>(gStatePacketsRejected),
            static_cast<unsigned long long>(gMailboxReceived.load()),static_cast<unsigned long long>(gMailboxOverflow.load()),
            static_cast<unsigned long long>(gNetworkSnapshots.load()));LogMessage(line);
    }
    gState.Tick(now);
    const auto action=gState.Due(now);
    if(action==coop_movie::Action::Start && StartNative(gState.Plan())) gState.Started();
    else if(action==coop_movie::Action::FinishWithoutMovie) {
        ContinueWithoutMovie();gState.FinishedWithoutMovie();Log("no-movie-continuation");
    }
    if(gState.CurrentPhase()==coop_movie::Phase::Done) gTerminalHeld=false;
    Publish(now);
}

// Called only after both players approved a native same-event restart and all
// replay admission checks succeeded. Decline/cancel never reaches this port.
void CoopMovieRaceRestartCommitted(uint64_t sessionId,uint32_t oldEpoch,uint32_t newEpoch,uint32_t event) {
    using namespace CoopMovieNative;
    coop::SessionSnapshot session;
    if(!oldEpoch||!newEpoch||oldEpoch==newEpoch||!event||!Scope(session)||
        session.sessionId!=sessionId||gBoundSession!=sessionId||gBoundTicket!=session.ticket||
        gPendingRaceEvent.sessionId!=sessionId||gPendingRaceEvent.membershipNonce!=session.membershipNonce||
        gPendingRaceEvent.ticket!=session.ticket||gPendingRaceEvent.parentEpoch!=oldEpoch||
        gPendingRaceEvent.seed!=newEpoch||gPendingRaceEvent.careerEventHash!=event)return;
    auto remember=[&](uint8_t flow,uint32_t hash,uint32_t epoch){
        if((flow!=1&&flow!=4)||hash!=event||!epoch||epoch==newEpoch||RetiredRace(flow,epoch,hash))return;
        gRetiredRaceEpochs[gRetiredRaceEpochNext++%gRetiredRaceEpochs.size()]={hash,epoch};
    };
    remember(1,event,oldEpoch);
    remember(gState.Flow(),gState.Event(),gState.Epoch());
    remember(gState.PeerFlow(),gState.PeerEvent(),gState.PeerEpoch());
    remember(gCurrent.flow,gCurrent.event,gCurrent.epoch);
    for(size_t i=0;i<gPendingCount;++i)remember(gPending[i].flow,gPending[i].event,gPending[i].epoch);
    // Remember the observed ancestry, not an assumption that the displayed
    // movie belonged to the immediately finished race (the reported case had
    // an older pre-race clip plus the current post-race return queued).
    bool state=false;for(const auto& retired:gRetiredRaceEpochs)if(retired.epoch)state=gState.RetireRace(retired.epoch,retired.event)||state;
    const bool current=RetiredRace(gCurrent.flow,gCurrent.epoch,gCurrent.event);
    size_t pending=0;
    for(size_t i=0;i<gPendingCount;++i)if(!RetiredRace(gPending[i].flow,gPending[i].epoch,gPending[i].event))gPending[pending++]=gPending[i];
    gPendingCount=pending;
    if(current) {
        gCurrent={};gHaveCurrent=false;gSource[0]=0;gContinuation=0;gLocalPlan={};
        gNativeStarted=gNaturalPending=gTerminalHeld=false;gContinuationDelivered=true;
        gAppliedScene=0;gAppliedOrdinal=~uint32_t(0);
    }
    if(gRootEpoch==oldEpoch) {
        gRootCommitted=gTerminalHeld=false;gRootOwner=0;gRootEvent=gRootEpoch=0;gLastRootWait=0;
    }
    AcquireSRWLockExclusive(&gMailboxLock);
    for(size_t i=0;i<gInboxCount;++i)remember(gInbox[i].flow,gInbox[i].event,gInbox[i].epoch);
    if(gHaveOutbound)remember(gOutbound.flow,gOutbound.event,gOutbound.epoch);
    gRestartMovieEvent=event;gRestartMovieEpoch=newEpoch;
    size_t keep=0;for(size_t i=0;i<gInboxCount;++i)if(!RetiredRace(gInbox[i].flow,gInbox[i].epoch,gInbox[i].event))gInbox[keep++]=gInbox[i];
    gInboxCount=keep;
    if(gHaveOutbound&&RetiredRace(gOutbound.flow,gOutbound.epoch,gOutbound.event))gHaveOutbound=false;
    ReleaseSRWLockExclusive(&gMailboxLock);
    if(current||state) {
        char line[224]{};std::snprintf(line,sizeof(line),
            "[MOVIE_SYNC] Approved restart retired old native movie/return epoch=%u next=%u event=%08X; no old Free Roam continuation.\r\n",oldEpoch,newEpoch,event);
        LogMessage(line);
    }
}

bool ReceiveCoopMoviePacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now) {
    using namespace CoopMovieNative;(void)now;uint32_t magic=0;
    if(bytes<4) return false;std::memcpy(&magic,data,4);if(magic!=coop_movie::kMagic) return false;
    const auto session=CoopSnapshot();
    if(bytes!=sizeof(coop_movie::Packet) || !session.joined || !session.fresh ||
       sender.sin_addr.s_addr!=session.peer.address || ntohs(sender.sin_port)!=session.peer.port) return true;
    coop_movie::Packet packet;std::memcpy(&packet,data,sizeof(packet));
    if(packet.session!=session.sessionId || packet.ticket!=session.ticket) return true;
    ++gMailboxReceived;
    AcquireSRWLockExclusive(&gMailboxLock);
    if(gInboxCount<gInbox.size()) gInbox[gInboxCount++]=packet;else ++gMailboxOverflow;
    ReleaseSRWLockExclusive(&gMailboxLock);return true;
}
void TickCoopMovieNetwork(DWORD now) {
    using namespace CoopMovieNative;(void)now;const auto session=CoopSnapshot();
    if(!session.joined || !session.fresh || !session.peer.Valid()) return;
    coop_movie::Packet packet;bool send=false;
    AcquireSRWLockExclusive(&gMailboxLock);
    if(gHaveOutbound && gOutbound.sequence!=gSentSequence && gOutbound.session==session.sessionId &&
       gOutbound.ticket==session.ticket) {packet=gOutbound;gSentSequence=packet.sequence;send=true;}
    ReleaseSRWLockExclusive(&gMailboxLock);
    if(send){++gNetworkSnapshots;SendSessionBytes(&packet,sizeof(packet),session.peer);}
}
bool InstallCoopMovieSyncHooks() {
    using namespace CoopMovieNative;if(gInstalled) return true;
    // Validate the entire batch before changing any entry. All displaced blocks
    // are complete instructions without relative calls/branches.
    for(const Hook& hook:gHooks) {BYTE actual[16]{};
        if(!ReadGameMemory(hook.address,actual,hook.length)||std::memcmp(actual,hook.bytes,hook.length)) {
            LogMessage("[MOVIE_SYNC] Native signatures differ; movie sync not installed.\r\n");return false;
        }
    }
    for(size_t i=0;i<std::size(gHooks);++i) {
        Hook& hook=gHooks[i];hook.trampoline=static_cast<BYTE*>(VirtualAlloc(nullptr,32,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
        DWORD old=0,ignored=0;
        if(!hook.trampoline || !gProtect(reinterpret_cast<void*>(hook.address),hook.length,PAGE_EXECUTE_READWRITE,&old)) {
            bool restored=true;for(size_t j=0;j<std::size(gHooks);++j) if(!Rollback(j)) restored=false;
            LogMessage(restored?"[MOVIE_SYNC] Install failed; all entries restored.\r\n":
                "[MOVIE_SYNC] Install failed; rollback incomplete, callable trampolines retained.\r\n");return false;
        }
        std::memcpy(hook.trampoline,hook.bytes,hook.length);
        CoopCareerNative::WriteRelativeJump(hook.trampoline+hook.length,reinterpret_cast<void*>(hook.address+hook.length));
        FlushInstructionCache(GetCurrentProcess(),hook.trampoline,hook.length+5);
        PublishOriginal(i,reinterpret_cast<uintptr_t>(hook.trampoline));
        CoopCareerNative::WriteRelativeJump(reinterpret_cast<BYTE*>(hook.address),hook.replacement);
        if(hook.length>5) std::memset(reinterpret_cast<BYTE*>(hook.address+5),0x90,hook.length-5);
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(hook.address),hook.length);
        gProtect(reinterpret_cast<void*>(hook.address),hook.length,old,&ignored);hook.installed=true;
    }
    gInstalled=true;LogMessage("[MOVIE_SYNC] Career VP6 request, clip votes and paired native continuation installed.\r\n");return true;
}
#endif
