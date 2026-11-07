// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include "CoopSpecialLifecycle.h"
// The UDP thread sees ONLY bounded packets/mailboxes. All native admission,
// UI ownership, receipt IO and native actions belong to the game thread.
namespace CoopSpecialNative {
struct NativeAdapters {
    bool (*install)()=nullptr;
    coop_special::NativeResult (*execute)(const coop_special::NativeRequest&,const Record&)=nullptr;
    bool (*settleBeforeUnbind)()=nullptr;
    bool (*initialReceiptReady)()=nullptr;
    bool (*commitAuthorizationDurable)()=nullptr;
    bool (*quiesceBeforeUnbind)()=nullptr;
    bool (*writeBarrierReady)()=nullptr;
};
static NativeAdapters nativeAdapters;
static coop_special::Lifecycle lifecycle;
static uint32_t nonceCounter=0,sentSequence=0,promptOperation=0,promptRequest=0,lastPhase=0;
static DWORD lastWaitNotice=0;
static SRWLOCK specialMailboxLock=SRWLOCK_INIT;
struct SpecialReceived {coop_special::Packet packet{};DWORD at=0;};
static std::array<SpecialReceived,16> specialInbox{};static size_t specialInboxCount=0;
static coop_special::Packet specialOutbound{};static bool specialHaveOutbound=false;
static DWORD (WINAPI *runtimeNow)()=GetTickCount;
static void (*runtimeSend)(const void*,size_t,coop::Endpoint)=SendSessionBytes;
static bool Scope(coop::SessionSnapshot& session){
    session=CoopSnapshot();return installed&&OwnCareer()&&IsCoopGameplayActive()&&session.joined&&session.fresh&&
        session.initialBarrierComplete&&session.sessionId&&session.ticket&&
        (session.role==coop::Role::Host||session.role==coop::Role::Client);
}
static bool ReadRuntimeContext(coop_special::Context& context){
    context={};uint32_t stage=0,mode=0;
    const auto* catalog=FactsCatalog();
    if(!catalog||!CoopPersonalSave::Name(CoopPersonalSave::campaign)||
        !Read(addresses.mode,mode)||mode!=1||!Read(addresses.career+0x7AF8,stage)||stage>=8)return false;
    context={CampaignHash(CoopPersonalSave::campaign),catalog->digest,uint8_t(stage)};
    // A native personal payout may advance a chapter before the peer's own
    // durable save finishes. Keep this same operation's original context until
    // both receipts are acknowledged; never reinterpret it as a new mission.
    if(state.Active()&&(state.Phase()&coop_special::Commit)&&
        context.campaign==state.CurrentContext().campaign&&context.catalog==state.CurrentContext().catalog)
        context=state.CurrentContext();
    return coop_special::Valid(context);
}
static bool (*runtimeContext)(coop_special::Context&)=ReadRuntimeContext;
static bool (*runtimeOwnAtStart)(const coop_special::Spec&)=OwnAtStart;
static bool (*runtimePeerAtStart)(const coop_special::Spec&,DWORD)=RawPeerAtStart;
static bool (*runtimeOpportunity)(const coop_special::Spec&)=OwnOpportunity;
static void ClearSpecialMailbox(){
    AcquireSRWLockExclusive(&specialMailboxLock);specialInboxCount=0;specialHaveOutbound=false;sentSequence=0;
    ReleaseSRWLockExclusive(&specialMailboxLock);
}
static bool SafeUnbind(){
    if(lifecycle.InNativeCall())return false;
    // A committed own image is settled BEFORE a session/campaign is retired.
    // Prepared-but-unpaid intent remains durable for the startup recovery fence.
    if(nativeAdapters.settleBeforeUnbind&&!nativeAdapters.settleBeforeUnbind())return false;
    // Stop the OLD native timer/UI without discarding a Prepared/Authorized
    // receipt. Clearing just the protocol state would otherwise leave a solo
    // native mission running after the peer/session disappeared.
    if(state.Active()&&!state.LocalSaved()&&!(state.Phase()&coop_special::Commit)&&
        (!nativeAdapters.quiesceBeforeUnbind||!nativeAdapters.quiesceBeforeUnbind()))return false;
    state.Unbind();lifecycle.Reset();ClearSpecialMailbox();promptOperation=promptRequest=lastPhase=0;return true;
}
static bool Bind(const coop::SessionSnapshot& session){
    if(state.Session()==session.sessionId&&state.Ticket()==session.ticket)return true;
    if(state.Session()&&!SafeUnbind())return false;
    if(!nativeAdapters.initialReceiptReady||!nativeAdapters.initialReceiptReady())return false;
    uint32_t nonce=runtimeNow()^(GetCurrentProcessId()*0x9E3779B9u)^(++nonceCounter*0x85EBCA6Bu);
    if(!nonce)nonce=nonceCounter;
    state.Bind(session.sessionId,session.ticket,session.role==coop::Role::Host?1:2,nonce);
    lifecycle.Reset();ClearSpecialMailbox();return true;
}
static void Trace(const char* reason){
    char line[320]{};std::snprintf(line,sizeof(line),
        "[SPECIAL] %s session=%llu ticket=%u campaign=%08X catalog=%08X stage=%u operation=%u request=%u event=%08X kind=%u phase=%u own=%u remainingMs=%u.\r\n",
        reason,static_cast<unsigned long long>(state.Session()),state.Ticket(),state.CurrentContext().campaign,state.CurrentContext().catalog,
        unsigned(state.CurrentContext().stage),state.Operation(),state.RequestId(),state.Current().id,unsigned(state.Current().kind),
        unsigned(state.Phase()),unsigned(state.Own()),state.Remaining());LogMessage(line);
}
static bool RuntimeBusy(){
    return RaceIntentPending()||gCoopRaceState!=COOP_RACE_IDLE||gTriggerRaceLoading;
}
static bool (*runtimeBusy)()=RuntimeBusy;
static bool (*runtimeHeld)()=[]{return false;};
// Called after fresh peer messages, before deciding deadline/failure/native
// work. Native simulation timer callbacks may stop inside an arrival modal.
static void (*runtimeAdvance)()=nullptr;
static const Record* FindRuntimeRecord(uint32_t hash){const auto* catalog=FactsCatalog();return catalog?Find(*catalog,hash):nullptr;}
static const Record* (*runtimeFind)(uint32_t)=FindRuntimeRecord;
static const Record* ActiveRecord(){
    const auto* record=runtimeFind(state.Current().id);
    return record&&coop_special::Same(record->spec,state.Current())?record:nullptr;
}
static coop_special::NativeGuard Guard(DWORD now){
    coop_special::NativeGuard guard{};coop_special::Context context;
    guard.context=OwnCareer()&&IsCoopGameplayActive()&&runtimeContext(context)&&
        coop_special::Same(context,state.CurrentContext())&&ActiveRecord();
    guard.busy=runtimeBusy();
    if(guard.context&&!(state.Phase()&coop_special::Go)){
        guard.ownAtStart=runtimeOwnAtStart(state.Current());guard.peerAtStart=runtimePeerAtStart(state.Current(),now);
    }else if(guard.context&&!lifecycle.Began()){
        // Repeat the ACTUAL position check immediately before native Begin,
        // not merely before draining the network/Prepare acknowledgment.
        guard.ownAtStart=runtimeOwnAtStart(state.Current());guard.peerAtStart=runtimePeerAtStart(state.Current(),now);
    }
    if(!lifecycle.Prepared()&&guard.context)guard.eligible=runtimeOpportunity(state.Current());
    return guard;
}
static void Publish(DWORD now){
    // A coordinator must not tell Client to pay and then lose that decision
    // in a crash before writing its own authorized intent to stable storage.
    if(state.Player()==1&&(state.Phase()&coop_special::Commit)&&
        (!nativeAdapters.commitAuthorizationDurable||!nativeAdapters.commitAuthorizationDurable()))return;
    coop_special::Packet packet;if(!state.Poll(now,packet))return;
    AcquireSRWLockExclusive(&specialMailboxLock);specialOutbound=packet;specialHaveOutbound=true;
    ReleaseSRWLockExclusive(&specialMailboxLock);
}
static void Prompt(){
    if(!state.Operation()&&!state.RequestId())return;
    if(promptOperation!=state.Operation()||promptRequest!=state.RequestId()){
        promptOperation=state.Operation();promptRequest=state.RequestId();lastWaitNotice=0;
        CoopAnnouncements::Post(state.AwaitingDecision()?"Special event invitation. F5 or the matching event's Continue/Accept to approve; F6 declines. Both players must meet at the start.":
            "Special event requested. Waiting for your teammate; F6 cancels before it begins.");Trace("proposal");
    }
    if(lastPhase!=state.Phase()){
        lastPhase=state.Phase();Trace("phase");
        if(state.Terminal())CoopAnnouncements::Post("Shared special event cancelled or failed. No shared completion was awarded.");
        else if(state.Phase()&coop_special::Complete)CoopAnnouncements::Post("Special event complete. Both personal rewards were saved.");
        else if(state.Phase()&coop_special::Commit)CoopAnnouncements::Post("Both objectives complete. Saving each player's own reward.");
        else if(state.Phase()&coop_special::Release)CoopAnnouncements::Post("Both players arrived. Complete your own photo or choose your own eligible part.");
        else if(state.Phase()&coop_special::Go)CoopAnnouncements::Post("Shared special event started. Both players must reach the destination.");
    }
}
static bool Propose(uint32_t hash,DWORD now){
    coop::SessionSnapshot session;coop_special::Context context;
    if(!Scope(session)||!Bind(session)||!runtimeContext(context)||!state.SetContext(context)||
        state.Active()||runtimeBusy())return false;
    const auto* record=runtimeFind(hash);
    if(!record||record->spec.stage!=context.stage||!runtimeOpportunity(record->spec)||!runtimeOwnAtStart(record->spec)||!state.Propose(record->spec,now)){
        if(!lastWaitNotice||now-lastWaitNotice>=3000){lastWaitNotice=now;
            CoopAnnouncements::Post("Special event is not ready. Check its native requirements and meet your teammate at the start.");}
        return false;
    }
    state.Presence(true,now);Prompt();Publish(now);return true;
}
// These require native proof. Position packets never complete a destination,
// and a cover reward callback before the shutter is NOT a completion proof.
static bool NoteArrival(uint32_t event,DWORD now){
    if(!installed||!state.Active()||state.Current().id!=event||!lifecycle.NativeProofWindow(state)||!ActiveRecord()||
        !(state.Phase()&coop_special::Go)||state.Terminal())return false;
    state.MarkArrived(now);Trace("own-native-arrival");return true;
}
static bool NoteDone(uint32_t event,DWORD now){
    if(!installed||!state.Active()||state.Current().id!=event||!lifecycle.NativeProofWindow(state)||!ActiveRecord()||
        !(state.Phase()&coop_special::Release)||!(state.Own()&coop_special::Arrived)||state.Terminal())return false;
    state.MarkDone(now);Trace("own-native-completion");return true;
}
} // namespace CoopSpecialNative
bool CoopSpecialBusy(){using namespace CoopSpecialNative;return installed&&OwnCareer()&&(state.Active()||(runtimeHeld&&runtimeHeld()));}
bool HandleCoopSpecialDecisionKey(unsigned key){
    using namespace CoopSpecialNative;if(key!=VK_F5&&key!=VK_F6)return false;
    coop::SessionSnapshot session;if(!Scope(session)||!state.Active())return false;
    if(state.AwaitingDecision())state.Decide(key==VK_F5,runtimeNow());
    else if(key==VK_F6&&!(state.Phase()&coop_special::Go))state.Fail(1,runtimeNow());
    else return false;
    Trace(key==VK_F5?"own-accept":"own-decline");Publish(runtimeNow());return true;
}
void TickCoopSpecialUi(DWORD flow){
    using namespace CoopSpecialNative;coop::SessionSnapshot session;
    if(!Scope(session)){if(state.Session())SafeUnbind();return;}
    if(!Bind(session))return;
    const DWORD now=runtimeNow();coop_special::Context context;
    const bool haveContext=(flow==3||flow==6)&&runtimeContext(context);
    if(!state.Active())state.SetContext(haveContext?context:coop_special::Context{});
    // Refresh own actual presence BEFORE Receive() can advance Host's Go.
    if(state.Active()&&!(state.Phase()&coop_special::Go))state.Presence(haveContext&&runtimeOwnAtStart(state.Current()),now);
    std::array<SpecialReceived,16> received;size_t count=0;
    AcquireSRWLockExclusive(&specialMailboxLock);count=specialInboxCount;std::copy_n(specialInbox.begin(),count,received.begin());specialInboxCount=0;
    ReleaseSRWLockExclusive(&specialMailboxLock);
    for(size_t i=0;i<count;++i)if(coop_time::Elapsed(now,received[i].at)<=coop_special::kFreshMs)state.Receive(received[i].packet,received[i].at);
    if(runtimeAdvance)runtimeAdvance();
    const auto request=lifecycle.Next(state,Guard(now),now);
    if(request){
        const auto* record=ActiveRecord();
        const auto result=record&&nativeAdapters.execute?nativeAdapters.execute(request,*record):coop_special::NativeResult::Refused;
        lifecycle.Complete(state,request,result,now);
    }
    Prompt();Publish(now);
}
bool ReceiveCoopSpecialPacket(const char* bytes,int length,const sockaddr_in& sender,DWORD now){
    using namespace CoopSpecialNative;uint32_t magic=0;if(!bytes||length<4)return false;
    std::memcpy(&magic,bytes,4);if(magic!=coop_special::kMagic)return false;
    if(!installed||length!=sizeof(coop_special::Packet)||sender.sin_family!=AF_INET||!IsJoinedCoopSender(sender))return true;
    coop_special::Packet packet;std::memcpy(&packet,bytes,sizeof(packet));const auto session=CoopSnapshot();
    if(!coop_special::Valid(packet)||packet.session!=session.sessionId||packet.ticket!=session.ticket||
        packet.player!=(session.role==coop::Role::Host?2:1))return true;
    AcquireSRWLockExclusive(&specialMailboxLock);
    if(specialInboxCount==specialInbox.size()){std::move(specialInbox.begin()+1,specialInbox.end(),specialInbox.begin());--specialInboxCount;}
    specialInbox[specialInboxCount++]={packet,now};ReleaseSRWLockExclusive(&specialMailboxLock);return true;
}
void TickCoopSpecialNetwork(DWORD now){
    using namespace CoopSpecialNative;(void)now;const auto session=CoopSnapshot();
    if(!installed||!session.joined||!session.fresh||!session.peer.Valid())return;
    coop_special::Packet packet;bool send=false;
    AcquireSRWLockExclusive(&specialMailboxLock);
    if(specialHaveOutbound&&specialOutbound.sequence!=sentSequence&&specialOutbound.session==session.sessionId&&specialOutbound.ticket==session.ticket){
        packet=specialOutbound;sentSequence=packet.sequence;send=true;
    }
    ReleaseSRWLockExclusive(&specialMailboxLock);if(send)runtimeSend(&packet,sizeof(packet),session.peer);
}
bool InstallCoopSpecialHooks(){
    using namespace CoopSpecialNative;if(installed)return true;
    if(!nativeAdapters.install||!nativeAdapters.execute||!nativeAdapters.settleBeforeUnbind||!nativeAdapters.initialReceiptReady||
        !nativeAdapters.commitAuthorizationDurable||!nativeAdapters.quiesceBeforeUnbind||!nativeAdapters.writeBarrierReady||
        !receiptMayImport||!nativeAdapters.writeBarrierReady()||!nativeAdapters.install())return false;
    installed=true;LogMessage("[SPECIAL] Verified shared mission lifecycle and own reward receipts installed.\r\n");return true;
}
