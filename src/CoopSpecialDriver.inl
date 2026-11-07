// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// The only production composition root. Native functions and receipt IO are
// called on the game thread; datagram handlers only enqueue bounded messages.
namespace CoopSpecialNative { namespace Driver {
using Scope=coop_special_receipt::Scope;using Phase=coop_special_receipt::Phase;
using Result=coop_special::NativeResult;using Request=coop_special::NativeRequest;
static bool beginEntered=false,beginFailed=false,showEntered=false,ticking=false;
static Scope begun,preparing;
static coop_special_receipt::AuthenticatedAbortDecision recoveredAbort;
static bool AbortAuthorization(const Scope& own,coop_special_receipt::AuthenticatedAbortDecision& out){
    if(state.OfflineRecovery()){
        if(!state.Terminal()||!coop_special_receipt::Same(recoveredAbort.old,own)||
            !coop_special_receipt::Valid(recoveredAbort.current))return false;
        out=recoveredAbort;return true;
    }
    return Abort::StateAuthorization(own,out);
}
static bool ReadCanonical(const std::string& own,ReceiptStore::Bytes& bytes){return ReceiptStore::ProductionRead(own,ReceiptStore::NativeId(),bytes);}
static bool (*readCanonical)(const std::string&,ReceiptStore::Bytes&)=ReadCanonical;
static DWORD lastRecoveryNotice=0;
static DWORD lastActionAt=0;static unsigned lastAction=0,lastResult=0;
static bool SameOperation(const Scope& scope){return Reward::Operation(scope);}
static bool ReceiptForOperation(){return ReceiptStore::Ready()&&!coop_special_receipt::Empty(ReceiptStore::tx)&&
    SameOperation(ReceiptStore::tx.scope);}
static bool BeginDispatched(){return beginEntered&&SameOperation(begun);}
static bool CurrentScope(const Record& row,Scope& own){
    if(!state.Active()||!state.Operation()||!state.Nonce()||!state.PeerNonce()||
        !coop_special::Same(row.spec,state.Current())||!CoopPersonalSave::Name(CoopPersonalSave::campaign)||
        !CoopPersonalSave::Name(CoopPersonalSave::personal))return false;
    Choice::OwnCar car;if(!Choice::ReadGarageCar(car))return false;
    own={};own.campaign=CoopPersonalSave::campaign;own.personal=CoopPersonalSave::personal;
    own.event=row.spec.id;own.catalog=state.CurrentContext().catalog;own.kind=uint8_t(row.spec.kind);own.stage=row.spec.stage;
    own.session=state.Session();own.ticket=state.Ticket();own.operation=state.Operation();own.localPlayer=state.Player();own.carId=car.id;
    own.hostNonce=state.Player()==1?state.Nonce():state.PeerNonce();own.clientNonce=state.Player()==2?state.Nonce():state.PeerNonce();
    return coop_special_receipt::Valid(own);
}
static bool ResetNativeWitnesses(){
    if(!Bootstrap::noOldNative()||!Reward::Reset()||!Abort::Reset()||!Ui::Reset()||!RecoveryNative::Reset())return false;
    Proof::Reset();Destination::Reset();Timer::clock={};Timer::display={};beginEntered=beginFailed=showEntered=false;begun={};return true;
}
static bool ExternalBusy(){
    return RaceIntentPending()||gCoopRaceState!=COOP_RACE_IDLE||gTriggerRaceLoading||
        CoopSponsorNative::state.Pending()||CoopFleetAnyOutrunBusy();
}
static bool ReadyInitial(){
    return Bootstrap::Bind(CoopPersonalSave::campaign,CoopPersonalSave::personal)&&ReceiptStore::MayImport()&&
        !RecoveryNative::Busy()&&!Abort::ExitPending();
}
static bool PublishAuthorization(){
    if(!ReceiptForOperation()||!ReceiptStore::tx.ownDone)return false;
    // Saved/Complete publication outlives the native save lease. Never require
    // an already released lease or re-authorize/re-grant a durable Paid image.
    const auto phase=ReceiptStore::tx.phase;
    return phase==Phase::Authorized||phase==Phase::Sealed||phase==Phase::Paid||Reward::Authorize();
}
static bool FinishOwn(){
    return ReceiptStore::Ready()&&!coop_special_receipt::Empty(ReceiptStore::tx)&&
        coop_special_receipt::Terminal(ReceiptStore::tx.phase)&&Bootstrap::finish(ReceiptStore::tx.scope,ReceiptStore::tx.phase);
}
static Result Prepare(const Request&,const Record& row){
    Scope own;
    if(!CurrentScope(row,own)||!Bootstrap::Bind(own.campaign,own.personal)||ExternalBusy())return Result::Refused;
    if(ReceiptForOperation())return ReceiptStore::tx.phase==Phase::Prepared&&Proof::SameBase(ReceiptStore::tx.scope,own)&&
        Reward::fenceHeld(ReceiptStore::tx.scope)?Result::Applied:Result::Refused;
    if(!ReceiptStore::MayImport()||!ResetNativeWitnesses()){
        // The current *unstarted* Engage dialog is expected, not an old UI.
        Engage::Live current;
        if(!ReceiptStore::MayImport()||beginEntered||Abort::ExitPending()||RecoveryNative::Busy()||
            !Reward::Reset()||!Abort::Reset()||!Ui::Reset()||!RecoveryNative::Reset()||!Engage::ReadLive(row.spec,current))return Result::Wait;
        Proof::Reset();Destination::Reset();Timer::clock={};Timer::display={};begun={};beginFailed=showEntered=false;
    }
    if(!runtimeOwnAtStart(row.spec)||!runtimePeerAtStart(row.spec,runtimeNow())||!runtimeOpportunity(row.spec))return Result::Wait;
    // Ensure only opens the real owned stock dialog; no Accept occurs before
    // both exact native start polygons and the durable OWN baseline are proved.
    if(!Engage::Ensure(row.spec)||!Bootstrap::arm(own))return Result::Wait;
    preparing=own;
    ReceiptStore::Bytes before;
    if(!Reward::serialize(before)||!ReceiptStore::PrepareLive(own,before))return Result::Wait;
    LogMessage("[SPECIAL_RECEIPT] Own current car/cash baseline flushed; native writes fenced before Armed.\r\n");
    return Result::Applied;
}
static Result Begin(const Request&,const Record& row){
    if(!ReceiptForOperation()||ReceiptStore::tx.phase!=Phase::Prepared||!Reward::fenceHeld(ReceiptStore::tx.scope)||
        !runtimeOwnAtStart(row.spec)||!runtimePeerAtStart(row.spec,runtimeNow())||!runtimeOpportunity(row.spec))return Result::Wait;
    // A frontend cover can produce its genuine view proof synchronously in
    // its constructor. Wait for shared Release BEFORE entering that constructor.
    if(!coop_special::Timed(row.spec.kind)&&!(state.Phase()&coop_special::Release))return Result::Wait;
    if(beginEntered)return beginFailed?Result::Refused:Result::Applied;
    Engage::Live live;if(!Engage::ReadLive(row.spec,live))return Result::Wait;
    begun=ReceiptStore::tx.scope;beginEntered=true; // Before any native reentry/side effect.
    if(!Engage::Accept(row.spec)){beginFailed=true;return Result::Refused;}
    return Result::Applied;
}
static Result AbortUnprepared(const Request& request,const Record& row){
    // A decline can precede Prepare on either peer. There is then no receipt,
    // no mission, and no reward to roll back. Do not touch another live surface.
    uintptr_t mission=1;if(beginEntered||!Read(addresses.career+0xC08,mission)||mission)return Result::Wait;
    // A serializer/IO failure may occur AFTER Arm but BEFORE Prepared. Record
    // that exact own baseline before cancellation so no lease is abandoned or
    // released by an invented in-memory "cancelled" assertion.
    if(coop_special_receipt::Valid(preparing)&&SameOperation(preparing)&&Reward::fenceHeld(preparing)){
        ReceiptStore::Bytes baseline;
        if(!ReceiptStore::Ready()||!readCanonical(preparing.personal,baseline)||
            !ReceiptStore::PrepareReceipt(preparing,baseline))return Result::Wait;
        const auto result=Abort::Execute(request,row);
        return result==Result::Applied?(FinishOwn()?Result::Applied:Result::Wait):result;
    }
    Engage::Live live;
    if(Engage::CancelPending(row.spec)||Engage::ReadLive(row.spec,live))
        return Engage::CancelUnstarted(row.spec)?Result::Applied:Result::Wait;
    // We ARE inside this Abort's lifecycle call. NoOldNative intentionally
    // rejects any in-flight lifecycle call, so using it here makes the peer
    // with no local prompt wait forever. Prove native surfaces separately;
    // do not weaken the menu/import barrier's reentrancy check.
    return Bootstrap::noOldSurface()?Result::Applied:Result::Wait;
}
static Result Execute(const Request& request,const Record& row){
    switch(request.action){
    case coop_special::NativeAction::Prepare:return Prepare(request,row);
    case coop_special::NativeAction::Begin:return Begin(request,row);
    case coop_special::NativeAction::Release:return coop_special::Timed(row.spec.kind)?Destination::Release(request):Result::Applied;
    case coop_special::NativeAction::Commit:{
        const auto result=Reward::Commit(request);
        return result==Result::Applied?(FinishOwn()?Result::Applied:Result::Wait):result;
    }
    case coop_special::NativeAction::Abort:{
        if(!ReceiptForOperation())return AbortUnprepared(request,row);
        const auto result=Abort::Execute(request,row);
        return result==Result::Applied?(FinishOwn()?Result::Applied:Result::Wait):result;
    }
    default:return Result::Refused;
    }
}
static Request RequestFor(coop_special::NativeAction action){return {action,state.Session(),state.Ticket(),state.Nonce(),
    state.Operation(),state.RequestId(),1,state.Current()};}
static Result ExecuteTracked(const Request& request,const Record& row){
    const Result result=Execute(request,row);const DWORD now=runtimeNow();
    if(result!=Result::Wait||unsigned(request.action)!=lastAction||unsigned(result)!=lastResult||now-lastActionAt>=3000){
        lastAction=unsigned(request.action);lastResult=unsigned(result);lastActionAt=now;
        uint32_t phase=~0u;uintptr_t mission=0;Read(addresses.career+0xD1C,phase);Read(addresses.career+0xC08,mission);
        char line[400]{};std::snprintf(line,sizeof(line),
            "[SPECIAL_NATIVE] action=%u result=%u event=%08X op=%u protocol=%u receipt=%u ownDone=%u lease=%u nativePhase=%u mission=%08X begin=%u/%u abortExit=%u recovery=%u.\r\n",
            unsigned(request.action),unsigned(result),row.spec.id,state.Operation(),unsigned(state.Phase()),unsigned(ReceiptStore::tx.phase),
            unsigned(ReceiptStore::tx.ownDone),unsigned(gSpecialSaveFence.Status()),phase,unsigned(mission),unsigned(beginEntered),unsigned(beginFailed),
            unsigned(Abort::ExitPending()),unsigned(RecoveryNative::Busy()));LogMessage(line);
    }
    return result;
}
static bool SettleBeforeUnbind(){
    if(RecoveryNative::Busy()||Abort::ExitPending()||Reward::attempt.dispatching)return false;
    if(!ReceiptForOperation())return true;
    if(coop_special_receipt::Terminal(ReceiptStore::tx.phase))return FinishOwn()&&Bootstrap::noOldNative();
    if(state.Phase()&coop_special::Commit){
        const auto* row=ActiveRecord();return row&&Execute(RequestFor(coop_special::NativeAction::Commit),*row)==Result::Applied&&Bootstrap::noOldNative();
    }
    // Prepared ownDone requires its original coordinator's immutable decision.
    // An expired UDP peer is never proof that an authorized reward was aborted.
    return ReceiptStore::tx.phase==Phase::Prepared;
}
static bool QuiesceBeforeUnbind(){
    if(!state.Active())return Bootstrap::noOldNative();
    if(!state.Terminal())state.Fail(4,runtimeNow());
    if(!state.Terminal())return false;
    const auto* row=ActiveRecord();return row&&Execute(RequestFor(coop_special::NativeAction::Abort),*row)==Result::Applied&&
        !Abort::ExitPending()&&Bootstrap::noOldNative();
}
static bool ApprovePrompt(const Record& row,DWORD now){
    // Clicking the freshly owned stock Continue/Accept is explicit consent,
    // just like F5. Do NOT replay stock solo Accept here: Prepare/Begin still
    // need both actual start polygons and the two durable own baselines.
    if(!state.Active()||!state.AwaitingDecision()||state.AwaitingFailureDecision()||
        lifecycle.InNativeCall()||Engage::CancelBusy()||Abort::ExitPending()||RecoveryNative::Busy()||
        ExternalBusy()||!ReceiptStore::MayImport()||!coop_special::Same(row.spec,state.Current()))return false;
    coop::SessionSnapshot session;coop_special::Context context;
    const bool scope=::CoopSpecialNative::Scope(session)&&session.sessionId==state.Session()&&session.ticket==state.Ticket()&&
        runtimeContext(context)&&coop_special::Same(context,state.CurrentContext())&&state.Fresh(now);
    const bool inside=runtimeOwnAtStart(row.spec),eligible=runtimeOpportunity(row.spec);
    if(!scope||!inside||!eligible){
        char line[240]{};std::snprintf(line,sizeof(line),
            "[SPECIAL_ENTRY] Native consent held event=%08X scopeFresh=%u ownAtStart=%u eligible=%u.\r\n",
            row.spec.id,unsigned(scope),unsigned(inside),unsigned(eligible));LogMessage(line);return false;
    }
    state.Presence(true,now);
    if(!state.Decide(true,now))return false;
    Trace("own-native-accept");Prompt();Publish(now);return true;
}
static bool Outer(void* self,uint32_t message,uintptr_t a,uintptr_t b,uintptr_t c){
    if(RecoveryNative::Outer(self,message,a,b,c))return true;
    Record row;
    if(Engage::Accepted(self,message,a,row)){
        // A Show destination's genuine cover prompt is part of the SAME
        // released mission, not a second proposal. Keep nested reward gates.
        const auto* active=ActiveRecord();
        if(state.Active()&&active&&active->spec.kind==coop_special::Kind::Show&&
            row.spec.id==active->spec.destination&&(state.Phase()&coop_special::Release)&&
            !state.Terminal()&&!state.AwaitingFailureDecision()&&Reward::MissionIs(*active,3)&&ReceiptForOperation()){
            if(!showEntered){showEntered=true;Engage::Live live;
                if(Engage::ReadLive(row.spec,live)&&!Engage::CallsAccept(live))beginFailed=true;}
            return true;
        }
        if(state.Active()){
            // An incoming proposal used to swallow this second player's
            // native Continue forever; only the separate F5 path set Accepted.
            ApprovePrompt(row,runtimeNow());return true;
        }
        if(!state.Active()&&!ExternalBusy()&&ReceiptStore::MayImport()&&!Abort::ExitPending()&&!RecoveryNative::Busy())Propose(row.spec.id,runtimeNow());
        return true; // Never leak the original solo Accept on a failed proposal.
    }
    if(Engage::AcceptButton(self,message,a)){
        // A real Special button whose current owner/opportunity cannot yet be
        // proved must NOT fall through into stock solo photo/mission setup.
        // That would show a camera but leave no shared operation to complete.
        static DWORD lastEntryNotice=0;const DWORD now=runtimeNow();
        if(!lastEntryNotice||now-lastEntryNotice>=3000){
            lastEntryNotice=now;uintptr_t area=0,cover=0,mission=0;
            Read(addresses.career+0x8704,area);Read(addresses.career+0x8710,cover);Read(addresses.career+0x8714,mission);
            char line[320]{};std::snprintf(line,sizeof(line),
                "[SPECIAL_ENTRY] Native Accept held: fresh FEng owner/opportunity not ready handler=%08X area=%08X cover=%08X mission=%08X catalog=%u.\r\n",
                unsigned(uintptr_t(self)),unsigned(area),unsigned(cover),unsigned(mission),unsigned(FactsCatalog()!=nullptr));LogMessage(line);
            CoopAnnouncements::Post("Special event is not ready. Keep the event prompt open and check both players' requirements.");
        }
        return true;
    }
    uint32_t button=0;const bool cancel=message==0x911AB364||
        (message==Engage::kClick&&IsValidUserPtr(a)&&Read(a+0x10,button)&&button==Engage::kCancelButton);
    if(cancel&&state.Active()){
        Engage::Live live;const auto* active=ActiveRecord();
        if(active&&Engage::ReadLive(active->spec,live)&&live.handler==uintptr_t(self)){
            if(!(state.Phase()&coop_special::Commit))state.Fail(1,runtimeNow());return true;}
    }
    return false;
}
static bool Start(void* self,uintptr_t row){
    if(RecoveryNative::Start(self,row))return true;
    // Only an exact first nested call from our owned original Engage Accept.
    return !Engage::TakeStartPermission(self,row);
}
static bool TimerGate(void* self){return RecoveryNative::Timer(self)||Timer::Tick(self)||
    (uintptr_t(self)==addresses.career+0xC00&&state.Active());}
static bool Arrival(void* self,uintptr_t descriptor){return RecoveryNative::Arrival(self,descriptor)||Destination::Intercept(self,descriptor);}
static bool Cover(void* self,uintptr_t caller){return RecoveryNative::Cover(self,caller)||Proof::Cover(self,caller);}
static bool UniqueDone(void* self,uintptr_t caller){
    if(!Reward::HoldUniqueDone(self,caller))return false;
    Abort::HoldUniqueDone(self,caller);return true;
}
static bool Failure(void* self){
    if(RecoveryNative::Fail(self))return true;
    if(uintptr_t(self)!=addresses.career+0xC00)return false;
    if(state.Active()&&!(state.Phase()&coop_special::Commit))state.Fail(2,runtimeNow());
    return true;
}
static bool Install(){
    Detours::callbacks={Outer,Start,TimerGate,Arrival,Proof::HoldPayout,Cover,Proof::HoldShowDone,
        RecoveryNative::Unique,UniqueDone,Failure,RecoveryNative::Photo,RecoveryNative::PhotoExit,
        RecoveryNative::MagazineExit,RecoveryNative::Failure};
    return Detours::Install();
}
static bool ImportReady(){return ReceiptStore::MayImport()&&!RecoveryNative::Busy()&&!Abort::ExitPending();}
static bool Held(){return (ReceiptStore::Status()!=ReceiptStore::LoadState::Unbound&&!ReceiptStore::MayImport())||
    Abort::ExitPending()||RecoveryNative::Busy()||(Proof::pending.valid&&!Bootstrap::noOldNative());}
static void Advance(){
    Destination::Observe();Timer::Advance();
    if(!state.Active()||!coop_special::Timed(state.Current().kind)||!(state.Phase()&coop_special::Go)||state.Terminal())return;
    static DWORD lastPulse=0;const DWORD now=runtimeNow();if(lastPulse&&now-lastPulse<5000)return;lastPulse=now;
    uintptr_t mission=0;uint32_t phase=0;Read(addresses.career+0xC08,mission);Read(addresses.career+0xD1C,phase);
    const Record* row=nullptr;uint32_t observedPhase=0;
    char line[480]{};std::snprintf(line,sizeof(line),
        "[SPECIAL_PROGRESS] event=%08X op=%u protocol=%u own=%u remainingMs=%u mission=%08X nativePhase=%u prepared=%u began=%u proofWindow=%u nativeOwned=%u witness=%u entered=%u released=%u hookTimer=%u hookArrival=%u clockUnitsPerSecond=4000 displayMs=%u.\r\n",
        state.Current().id,state.Operation(),unsigned(state.Phase()),unsigned(state.Own()),state.Remaining(),unsigned(mission),phase,
        unsigned(lifecycle.Prepared()),unsigned(lifecycle.Began()),unsigned(lifecycle.NativeProofWindow(state)),unsigned(Destination::OwnedMission(row,observedPhase)),
        unsigned(Destination::Operation(Destination::witness)),unsigned(Destination::witness.nativeEntered),unsigned(Destination::witness.released),
        unsigned(Detours::ExpectedPatch(Detours::entries[Detours::Timer])),unsigned(Detours::ExpectedPatch(Detours::entries[Detours::Arrival])),
        state.Player()==2?Timer::display.shown:state.Remaining());LogMessage(line);
}
static void OldDecision(){
    if(!ReceiptStore::Ready()||ReceiptStore::tx.phase!=Phase::Prepared||!ReceiptStore::tx.ownDone||
        ReceiptStore::tx.scope.localPlayer!=2)return;
    const auto evidence=RecoveryRuntime::Result();if(!coop_special_recovery_receipt::MatchesPending(ReceiptStore::tx,evidence))return;
    if(evidence.decision==coop_special_recovery::Decision::Commit){
        if(ReceiptStore::Authorize(ReceiptStore::tx.scope)){
            // Live old context already exists? Resume that exact lifecycle.
            // Otherwise retire only an empty/finished protocol incarnation;
            // fresh reconstruction owns its immutable old scope from here.
            LogMessage("[SPECIAL_RECOVERY] Original coordinator Commit authenticated and flushed locally.\r\n");
            RecoveryNative::AdoptExisting();
        }
    }else{
        coop_special_receipt::AuthenticatedAbortDecision decision;
        if(!coop_special_recovery_receipt::AbortDecision(ReceiptStore::tx,evidence,decision))return;
        if(Bootstrap::noOldNative()){
            if(ReceiptStore::CancelAndInstall(ReceiptStore::tx.scope,ReceiptStore::tx.before,true,&decision))FinishOwn();
        }else if(ReceiptForOperation()){
            const auto& own=ReceiptStore::tx.scope;const auto* row=ActiveRecord();
            if(row&&state.RestoreAbort(own.session,own.ticket,own.localPlayer,own.hostNonce,own.clientNonce,own.operation,
                {CampaignHash(own.campaign),own.catalog,own.stage},row->spec)){
                recoveredAbort=decision;lifecycle.Reset();ClearSpecialMailbox();
            }
        }
    }
}
static void Tick(DWORD flow){
    if(!installed||ticking)return;struct Lock{Lock(){ticking=true;}~Lock(){ticking=false;}} lock;
    if(!OwnCareer())return;
    const DWORD now=runtimeNow();
    if(!CoopPersonalSave::Name(CoopPersonalSave::campaign)||!CoopPersonalSave::Name(CoopPersonalSave::personal))return;
    if(!Bootstrap::RetryIo(now)||!Bootstrap::Bind(CoopPersonalSave::campaign,CoopPersonalSave::personal))return;
    RecoveryRuntime::Tick(now);OldDecision();
    if(state.OfflineRecovery()&&state.Terminal()&&ReceiptForOperation()){
        const auto* row=ActiveRecord();
        if(row&&Execute(RequestFor(coop_special::NativeAction::Abort),*row)==Result::Applied)state.MarkSaved(now);
        Abort::TickAfterSettlement();
        if(!ReceiptStore::MayImport()||Abort::ExitPending()||!Bootstrap::noOldNative())return;
        if(ResetNativeWitnesses()){state.Unbind();lifecycle.Reset();ClearSpecialMailbox();recoveredAbort={};}
    }
    if(RecoveryNative::Busy()||(!coop_special_receipt::Empty(ReceiptStore::tx)&&
        ReceiptStore::tx.phase==Phase::Authorized&&!ReceiptForOperation())){
        // No old wire is emitted while reconstructing an OWN authorized award.
        if(state.Session()&&!state.OfflineRecovery()){
            if(state.Active()||!Bootstrap::noOldNative())return;
            state.Unbind();lifecycle.Reset();ClearSpecialMailbox();
        }
        if(RecoveryNative::Tick(flow)&&ReceiptStore::tx.phase==Phase::Paid)FinishOwn();
        if(RecoveryNative::Busy())return;
        if(state.OfflineRecovery()&&ResetNativeWitnesses()){state.Unbind();lifecycle.Reset();ClearSpecialMailbox();}
    }
    Proof::Tick();Abort::TickAfterSettlement();
    if(ReceiptForOperation()&&coop_special_receipt::Terminal(ReceiptStore::tx.phase))FinishOwn();
    TickCoopSpecialUi(flow);
    Abort::TickAfterSettlement();
    Hud::Tick();
    if(!state.Active()&&!Abort::ExitPending()&&Bootstrap::noOldNative())ResetNativeWitnesses();
    Bootstrap::ReleaseDeferred(flow);
    if(!ReceiptStore::MayImport()&&(!lastRecoveryNotice||now-lastRecoveryNotice>=15000)){
        lastRecoveryNotice=now;LogMessage("[SPECIAL_RECOVERY] Own pending event retained; duplicate rewards and peer personal-profile import blocked.\r\n");
    }
}
}} // namespace CoopSpecialNative::Driver
bool ConfigureCoopSpecialDriver(){
    using namespace CoopSpecialNative;
    receiptMayImport=Driver::ImportReady;runtimeBusy=Driver::ExternalBusy;Abort::beginDispatched=Driver::BeginDispatched;
    runtimeHeld=Driver::Held;
    runtimeAdvance=Driver::Advance;
    Abort::authorization=Driver::AbortAuthorization;
    nativeAdapters={Driver::Install,Driver::ExecuteTracked,Driver::SettleBeforeUnbind,Driver::ReadyInitial,
        Driver::PublishAuthorization,Driver::QuiesceBeforeUnbind,Bootstrap::BarrierReady};
    const bool ready=InstallCoopSpecialHooks();if(ready)RecoveryRuntime::Enable(true);return ready;
}
void TickCoopSpecialController(DWORD flow){CoopSpecialNative::Driver::Tick(flow);}
bool ReceiveCoopSpecialRecoveryPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now){
    return CoopSpecialNative::installed&&CoopSpecialNative::RecoveryRuntime::Receive(data,bytes,sender,now);
}
void TickCoopSpecialRecoveryNetwork(DWORD now){CoopSpecialNative::RecoveryRuntime::NetworkTick(now);}
