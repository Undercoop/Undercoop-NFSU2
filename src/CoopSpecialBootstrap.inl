// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Bootstrap is the OWN receipt boundary before ANY initial progress merge.
// Pending local rewards keep their entire native baseline; the incoming shared
// manifest is retained separately until that OWN transaction is settled.
namespace CoopSpecialNative { namespace Bootstrap {
using Phase=coop_special_receipt::Phase;using Scope=coop_special_receipt::Scope;using Bytes=ReceiptStore::Bytes;
static ReceiptStore::Store* storage=&ReceiptStore::store;
static std::string boundPersonal,boundCampaign;
static bool initialHeld=false,deferredKnown=false,loadedPending=false;
static coop_career::Progress deferred;
static DWORD lastRetry=0,lastNotice=0;
static ReceiptStore::Ports (*previewPorts)()=ReceiptStore::ProductionPorts;
static bool ReadPreview(const std::string& own,Bytes& out){return ReceiptStore::ProductionRead(own,ReceiptStore::NativeId(),out);}
static bool (*readPreview)(const std::string&,Bytes&)=ReadPreview;
enum class Preview {Rejected,FullBootstrap,Admitted};
static Preview ReadMenuPreview(const std::string& team,const std::string& own,const Bytes& payload){
    // A selected OWN profile preview is not a shared career launch or mission
    // admission. Do not bind/rebind the runtime lease merely to open Resume /
    // New Career choices. Pending receipts require the full fenced bootstrap;
    // conflicting or unreadable independent probes reject this preview.
    if(!OwnCareer()||!CoopPersonalSave::Name(team)||!CoopPersonalSave::Name(own)||
        state.Active()||Abort::ExitPending()||RecoveryNative::Busy()||lifecycle.InNativeCall()||
        Reward::attempt.entered||!storage)return Preview::Rejected;
    if(storage->Status()==ReceiptStore::LoadState::Faulted||
        (storage->Ready()&&!storage->MayImport())||
        (!coop_special_receipt::Empty(storage->tx)&&!coop_special_receipt::Terminal(storage->tx.phase))||
        (gSpecialSaveFence.Status()!=coop_special_save::Mode::Inactive&&gSpecialSaveFence.Status()!=coop_special_save::Mode::Open))return Preview::FullBootstrap;
    ReceiptStore::Store probe(previewPorts());
    if(!probe.Bind(own,team))return Preview::Rejected;
    if(!probe.MayImport()||(!coop_special_receipt::Empty(probe.tx)&&!coop_special_receipt::Terminal(probe.tx.phase)))return Preview::FullBootstrap;
    Bytes encoded,canonical;
    if(!coop_save::EncodeNativeProfileFile(payload,encoded)||!coop_special_receipt::Native(encoded,own)||
        !readPreview(own,canonical)||encoded!=canonical)return Preview::Rejected;
    LogMessage("[SPECIAL_BOOTSTRAP] Verified own canonical menu preview admitted with no pending reward; full native barrier remains required at actual career launch.\r\n");
    return Preview::Admitted;
}
static bool MenuPreview(const std::string& team,const std::string& own,const Bytes& payload){return ReadMenuPreview(team,own,payload)==Preview::Admitted;}
static bool Denied(const char* reason,const std::string& team,const std::string& own){
    const DWORD now=GetTickCount();static DWORD reported=0;static std::string previous;
    if(!reported||previous!=reason||now-reported>=3000){
        reported=now;previous=reason;char line[480]{};
        std::snprintf(line,sizeof(line),"[SPECIAL_BOOTSTRAP] denied=%s campaign=%s personal=%s coop=%u restore=%u receipt=%u ready=%u lease=%u observing=%u error=%lu.\r\n",
            reason,team.c_str(),own.c_str(),unsigned(gProfileContext.IsCoopActive()),unsigned(gRestoreSpPending),unsigned(storage->Status()),
            unsigned(storage->Ready()),unsigned(gSpecialSaveFence.Status()),unsigned(gSpecialSaveFence.Observing()),GetLastError());LogMessage(line);
    }
    return false;
}
static bool BarrierReady(){return ConfigureCoopProfileContext()&&CoopSpecialNativeSaveIatReady()&&gSpecialSaveFence.Observing();}
static bool (*barrierReady)()=BarrierReady;
static bool BindFence(const std::string& own){
    if(gSpecialSaveFence.Status()==coop_special_save::Mode::Open&&!gSpecialSaveFence.Unbind())return false;
    return gSpecialSaveFence.Bind(gProfileContext.ActiveRoot(),WideText(own.c_str()));
}
static bool (*bindFence)(const std::string&)=BindFence;
static bool Arm(const Scope& own){return gSpecialSaveFence.Arm({own.session,own.ticket,own.operation});}
static bool (*arm)(const Scope&)=Arm;
static bool Finish(const Scope& own,Phase phase){
    if(!coop_special_receipt::Terminal(phase))return false;
    if(gSpecialSaveFence.Status()==coop_special_save::Mode::Open)return true;
    return gSpecialSaveFence.Finish({{own.session,own.ticket,own.operation},phase==Phase::Paid?
        coop_special_save::Settlement::Paid:coop_special_save::Settlement::Cancelled});
}
static bool (*finish)(const Scope&,Phase)=Finish;
static bool NoOldSurface(){
    uintptr_t mission=1;
    if(Engage::CancelBusy()||!Read(addresses.career+0xC08,mission)||mission)return false;
    const char* screens[]{Choice::kScreen,Ui::kPhoto,Ui::kPhotoAlt,Ui::kMagazine,Ui::kFailure,Engage::kScreen};
    return Feng::Absent(screens,sizeof(screens)/sizeof(screens[0]),Engage::addresses.manager);
}
static bool (*noOldSurface)()=NoOldSurface;
static bool NoOldNative(){return !lifecycle.InNativeCall()&&noOldSurface();}
static bool (*noOldNative)()=NoOldNative;
static bool RetryIo(DWORD now);
static bool Held(){return installed&&OwnCareer()&&storage&&storage->Ready()&&!storage->MayImport();}
static bool Remember(const coop_career::Progress& incoming){
    if(!CoopPersonalSave::ValidateNative(incoming))return false;
    if(!deferredKnown){deferred=incoming;deferredKnown=true;return true;}
    coop_career::Progress next;
    if(!coop_career::Combine(deferred,incoming,next))return false;
    deferred=std::move(next);return true;
}
static bool Bind(const std::string& team,const std::string& own){
    if(!barrierReady())return Denied("native-save-barrier",team,own);
    if(!OwnCareer())return Denied("own-career-context",team,own);
    if(!CoopPersonalSave::Name(team)||!CoopPersonalSave::Name(own))return Denied("unsafe-profile-name",team,own);
    if(boundPersonal==own&&boundCampaign==team&&storage->Ready()){
        return coop_special_receipt::Empty(storage->tx)||coop_special_receipt::Terminal(storage->tx.phase)||arm(storage->tx.scope);
    }
    if(!boundPersonal.empty()&&(boundPersonal!=own||boundCampaign!=team)){
        if(!storage->Ready()||!storage->MayImport()||state.Active()||Abort::ExitPending()||lifecycle.InNativeCall())return false;
        if(!coop_special_receipt::Empty(storage->tx)&&!finish(storage->tx.scope,storage->tx.phase))return false;
        storage->Unbind();deferredKnown=false;initialHeld=loadedPending=false;
    }
    if(!bindFence(own))return Denied("own-native-fence-bind",team,own);
    if(!storage->Bind(own,team))return Denied("own-receipt-read",team,own);
    boundPersonal=own;boundCampaign=team;
    if(!coop_special_receipt::Empty(storage->tx)&&!coop_special_receipt::Terminal(storage->tx.phase)){
        if(!arm(storage->tx.scope))return false;
        loadedPending=true;
    }
    return true;
}
static bool OriginalHostAbort(const coop_special_receipt::Transaction& tx,coop_special_receipt::AuthenticatedAbortDecision& out){
    // Original coordinator's Prepared was NEVER exposed as Commit (Publish
    // requires Authorized flush). Its own fresh-process cancellation is a
    // local durable decision, not a fabricated CURRENT-session peer packet.
    if(!coop_special_receipt::Valid(tx)||tx.phase!=Phase::Prepared||tx.scope.localPlayer!=1)return false;
    const auto& s=tx.scope;out={s,{s.session,s.ticket,s.hostNonce,s.clientNonce}};return true;
}
static bool RecoverBeforeLoad(Bytes& payload,bool applyNative){
    if(!storage->Ready())return false;
    if(coop_special_receipt::Empty(storage->tx)||coop_special_receipt::Terminal(storage->tx.phase))return true;
    const auto phase=storage->tx.phase;Bytes restore;
    if(phase==Phase::Sealed||phase==Phase::CancelPending){
        restore=storage->tx.after;
        if(!(phase==Phase::Sealed?storage->RecoverSealed():storage->RecoverCancellation()))return false;
    }else if(phase==Phase::Prepared&&(!storage->tx.ownDone||storage->tx.scope.localPlayer==1)&&noOldNative()){
        coop_special_receipt::AuthenticatedAbortDecision decision;
        const auto own=storage->tx.scope;const bool proven=storage->tx.ownDone&&OriginalHostAbort(storage->tx,decision);
        if(storage->tx.ownDone&&!proven)return false;
        restore=storage->tx.before;
        if(!storage->CancelAndInstall(own,restore,true,proven?&decision:nullptr))return false;
    }
    if(!restore.empty()){
        if(!coop_special_receipt::Native(restore,boundPersonal))return false;
        payload.assign(restore.begin()+0x24,restore.end());
        // This is ONLY a menu/bootstrap load. NEVER deserialize a live award.
        if(applyNative){uint32_t flow=0;
            if(!Read(addresses.flow,flow)||flow!=3||!NativeDeserializeProfile(payload.data()))return false;
        }
        if(!finish(storage->tx.scope,storage->tx.phase))return false;
    }
    return true;
}
static bool Initial(const std::string& team,const std::string& own,uint32_t launch,bool host,Bytes& payload,coop_career::Progress* incoming){
    initialHeld=false;
    // First menu admission runs BEFORE BindNames(), so Driver::Tick cannot yet
    // service its ordinary named-profile reload. A transient initial own-file
    // sharing/read error must be explicitly revalidated here too; repeating
    // Store::Bind on a faulted Store deliberately can never clear that fault.
    if(storage->Status()==ReceiptStore::LoadState::Faulted&&!RetryIo(GetTickCount()))
        return Denied("own-receipt-retry",team,own);
    if(!Bind(team,own))return false;
    if(launch==2&&!storage->MayImport())return false; // Never reset an owed same-name career.
    if(!RecoverBeforeLoad(payload,host))return Denied("own-pending-recovery",team,own);
    if(storage->MayImport())return true;
    Bytes full;if(!coop_save::EncodeNativeProfileFile(payload,full)||!coop_special_receipt::Native(full,own))return false;
    // A valid different personal image cannot silently replace a pending
    // baseline; preserving every copy beats overwriting later personal data.
    if(full!=storage->tx.before){
        // Menu serializers may refresh transient/options bytes after reading
        // a profile. Store.Load has ALREADY proved the canonical own file is
        // exactly this protected baseline. Never import a peer image here.
        uint32_t flow=0;
        if(!Read(addresses.flow,flow)||flow!=3||!noOldNative()||Reward::attempt.entered||
            !coop_special_receipt::Native(storage->tx.before,own))return false;
        payload.assign(storage->tx.before.begin()+0x24,storage->tx.before.end());
        if(host&&!NativeDeserializeProfile(payload.data()))return false;
    }
    coop_career::Progress local;
    if(!coop_career::Extract(payload,local,CoopPersonalSave::DiscoveryEligible,true)||!CoopPersonalSave::ValidateNative(local))return false;
    if(incoming&&!Remember(*incoming))return false;
    if(host&&CoopPersonalSave::haveCommon&&!Remember(CoopPersonalSave::common))return false;
    if(incoming)*incoming=local;
    CoopPersonalSave::common=local;CoopPersonalSave::haveCommon=true;
    CoopPersonalSave::historicalSpecials=local;CoopPersonalSave::haveHistoricalSpecials=true;
    initialHeld=true;
    LogMessage("[SPECIAL_RECOVERY] Own pending baseline loaded without peer completion/stage import; shared manifest retained until own settlement.\r\n");
    return true;
}
static bool RetryIo(DWORD now){
    if(!storage||storage->Status()!=ReceiptStore::LoadState::Faulted)return storage&&storage->Ready();
    if(lastRetry&&now-lastRetry<2000)return false;lastRetry=now;
    const bool restored=storage->Reload();
    LogMessage(restored?"[SPECIAL_RECEIPT] Explicit game-thread reload validated durable own receipt after IO retry.\r\n":
        "[SPECIAL_RECEIPT] Own receipt still unreadable/conflicting; native reward and imports remain fenced.\r\n");
    return restored;
}
static bool ReleaseDeferred(DWORD flow){
    if(!deferredKnown)return true;
    if(!storage->Ready()||!storage->MayImport()||!CoopSharedCareerCanApply(flow)||
        (state.Active()&&!state.LocalSaved())||Abort::ExitPending())return false;
    if(!CoopPersonalSave::MergeLive(deferred,true))return false;
    deferredKnown=false;initialHeld=false;CoopPersonalSave::dirty=true;
    LogMessage("[SPECIAL_RECOVERY] Retained shared career manifest applied AFTER own durable settlement; personal car/cash preserved.\r\n");return true;
}
}} // namespace CoopSpecialNative::Bootstrap
bool CoopSpecialPrepareInitial(const char* team,const char* own,uint32_t launch,bool host,std::vector<uint8_t>& payload,coop_career::Progress* incoming){
    using namespace CoopSpecialNative;
    return !installed||(!OwnCareer()?false:team&&own&&Bootstrap::Initial(team,own,launch,host,payload,incoming));
}
bool CoopSpecialInitialHeld(){return CoopSpecialNative::installed&&CoopSpecialNative::Bootstrap::initialHeld;}
static bool CoopSpecialPrepareMenuPreview(const char* team,const char* own,std::vector<uint8_t>& payload){
    using namespace CoopSpecialNative;
    if(!installed)return true;
    if(!team||!own)return false;
    const auto decision=Bootstrap::ReadMenuPreview(team,own,payload);
    if(decision==Bootstrap::Preview::Admitted)return true;
    return decision==Bootstrap::Preview::FullBootstrap&&CoopSpecialPrepareInitial(team,own,1,true,payload,nullptr);
}
bool CoopSpecialBeforeNewCareer(const char* name){
    using namespace CoopSpecialNative;if(!installed||!OwnCareer())return true;
    if(!name||!Bootstrap::Bind(name,name)||!ReceiptStore::MayImport()||state.Active()||Abort::ExitPending()){
        CoopAnnouncements::Post("A pending special event must be settled before starting another career.");return false;}
    return true;
}
