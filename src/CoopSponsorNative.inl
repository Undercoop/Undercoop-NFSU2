// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Include after CoopCareerNative.inl and CoopPersonalSave.inl. All State,
// catalog reads, approval, and native replay belong exclusively to UiTick.
// Network callbacks only move bounded, pointer-free packets through mailboxes.
#include "CoopSponsor.h"
void ShowCoopChatStatus(const char*);
void SetCoopSponsorPrompt(const char*);
namespace CoopSponsorNative {
using HandlerFn=void(__thiscall*)(void*,uint32_t,uintptr_t,uintptr_t,uintptr_t);
using SignFn=void(__thiscall*)(void*,uintptr_t);
using FindFn=void*(__cdecl*)(const char*);
using RatingFn=float(__thiscall*)(void*);
using EligibleFn=int(__thiscall*)(void*,float);
using PageGateFn=bool(__thiscall*)(void*);
using SwitchPageFn=void(__cdecl*)(const char*,const char*,uint32_t);
static HandlerFn handlerOriginal=reinterpret_cast<HandlerFn>(0x4C1340);
static SignFn signOriginal=reinterpret_cast<SignFn>(0x532CF0);
static HandlerFn popupOriginal=reinterpret_cast<HandlerFn>(0x4C12E0);
static PageGateFn pageGateOriginal=reinterpret_cast<PageGateFn>(0x4A4610);
static SwitchPageFn switchPage=reinterpret_cast<SwitchPageFn>(0x537980);
static FindFn findHandler=reinterpret_cast<FindFn>(0x51BD10);
static RatingFn rating=reinterpret_cast<RatingFn>(0x5005E0);
static EligibleFn eligible=reinterpret_cast<EligibleFn>(0x500670);
static PageGateFn quotaAvailable=reinterpret_cast<PageGateFn>(0x500610);
struct Addresses {
    uintptr_t career=0x85AD40,uiStage=0x862838,strings=0x838428,manager=0x8363DC,ui=0x8384C4;
    uintptr_t gameMode=0x850078,resultContext=0x863470,contextRacers=0x85EF4C;
};
static Addresses addresses;
static constexpr uint32_t kConfirm=0xD05FC3A3;
static coop_sponsor::State state;
static SRWLOCK mailboxLock=SRWLOCK_INIT;
static std::array<coop_sponsor::Packet,16> inbox{};static size_t inboxCount=0;
static coop_sponsor::Packet outbound{};static bool haveOutbound=false,installed=false;
static uint32_t sentSequence=0,nonceCounter=0;
static uint64_t boundSession=0;static uint32_t boundTicket=0;
static bool replaying=false,signObserved=false;static uintptr_t replayContract=0;
static uint32_t promptProposal=0,promptOption=0;static bool promptDeclined=false,promptDone=false;
static uint32_t waitProposal=0;static unsigned waitReason=0;static DWORD waitAt=0;
static uintptr_t deferredPopupHandler=0;static DWORD deferredPopupAt=0;
struct NativeReceipt {
    bool valid=false,actionComplete=false,saved=false;
    uint64_t session=0;uint32_t ticket=0,proposal=0,option=0,lastSaveAttempt=0,lastReplayAttempt=0;
    coop_sponsor::Context context{};uintptr_t contract=0,page=0,handler=0;
};
static NativeReceipt receipt;
static bool Persist(DWORD now){return CoopPersonalSave::CaptureAndQueue(now,true);}
static bool (*persist)(DWORD)=Persist;
struct LiveContext {
    coop_sponsor::Context shared{};
    std::array<uintptr_t,coop_sponsor::kMaximumOptions> contracts{};
    std::array<std::array<char,64>,coop_sponsor::kMaximumOptions> names{};
    uintptr_t handler=0,selected=0,signedContract=0;
    uint32_t eligibleCount=0;float previousRating=0;
};
static bool Scope(coop::SessionSnapshot& session){
    session=CoopSnapshot();return installed && IsCoopGameplayActive() && gProfileContext.IsCoopActive() &&
        !gRestoreSpPending && session.joined && session.fresh && session.sessionId && session.ticket &&
        (session.role==coop::Role::Host || session.role==coop::Role::Client);
}
static uint32_t Hash(const char* text){
    uint32_t hash=2166136261u;for(size_t i=0;text && text[i];++i){unsigned char c=static_cast<unsigned char>(text[i]);
        if(c>='a' && c<='z')c-=32;hash=(hash^c)*16777619u;}return hash?hash:1;
}
static void Mix(uint32_t& hash,uint32_t value){for(unsigned i=0;i<4;++i){hash=(hash^uint8_t(value))*16777619u;value>>=8;}}
template<class T>static bool Read(uintptr_t at,T& value){return ReadGameMemory(at,&value,sizeof(value));}
static bool Text(uintptr_t at,std::array<char,64>& text){
    text={};if(!IsValidUserPtr(at))return false;
    for(size_t i=0;i<text.size();++i){if(!Read(at+i,text[i]))return false;
        if(!text[i])return i!=0;if(uint8_t(text[i])<32 || uint8_t(text[i])>126)return false;}
    return false;
}
static bool ReadLive(LiveContext& out){
    out={};uint32_t stage=0;uintptr_t catalog=0,strings=0;int8_t count=0;
    if(CoopPersonalSave::campaign.empty() || !Read(addresses.career+0x7AF8,stage) || stage>=8 ||
       !Read(addresses.career+0x72D8+stage*0x104+0x100,catalog) || !IsValidUserPtr(catalog) ||
       !Read(catalog+1,count) || count<1 || count>int(coop_sponsor::kMaximumOptions) ||
       !Read(addresses.strings,strings) || !IsValidUserPtr(strings))return false;
    out.shared.campaign=Hash(CoopPersonalSave::campaign.c_str());out.shared.stage=stage;
    out.shared.count=uint32_t(count);uint32_t digest=2166136261u;Mix(digest,stage);Mix(digest,uint32_t(count));
    for(int i=0;i<count;++i){
        uintptr_t contract=0;uint16_t name=0;int16_t bonus=0,minimumRating=0;
        if(!Read(catalog+8+i*4,contract) || !IsValidUserPtr(contract) || !Read(contract,name) ||
           !Read(contract+0xC,bonus) || bonus<0 || !Read(contract+0xE,minimumRating) ||
           !Text(strings+name,out.names[i]))return false;
        const uint32_t id=Hash(out.names[i].data());out.shared.options[i]=id;out.contracts[i]=contract;
        Mix(digest,id);Mix(digest,uint16_t(bonus));Mix(digest,uint16_t(minimumRating));
        // Native stage configuration pairs each offer with its race quota.
        uint16_t quota=0;if(!Read(catalog+0x1C+i*2,quota))return false;Mix(digest,quota);
    }
    out.shared.catalog=digest?digest:1;
    if(!coop_sponsor::Valid(out.shared) || !Read(addresses.career+0x73C0+stage*0x104,out.signedContract))return false;
    __try {
        out.previousRating=rating(reinterpret_cast<void*>(addresses.career+0x71D4+stage*0x104));
        if(!std::isfinite(out.previousRating))return false;
        const int available=eligible(reinterpret_cast<void*>(addresses.career+0x72D8+stage*0x104),out.previousRating);
        if(available<0 || available>count)return false;out.eligibleCount=uint32_t(available);
        out.handler=reinterpret_cast<uintptr_t>(findHandler("UI_Sponsorship_new.fng"));
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    if(out.handler){
        uint32_t uiStage=~0u;uintptr_t item=0,manager=0,node=0,name=0;std::array<char,64> page{};
        if(!IsValidUserPtr(out.handler) || !Read(addresses.uiStage,uiStage) || uiStage!=stage ||
           !Read(out.handler+0x58,item) || !IsValidUserPtr(item) || !Read(item+0x48,out.selected) ||
           !Read(addresses.manager,manager) || !IsValidUserPtr(manager) || !Read(manager+0x38,node) ||
           !IsValidUserPtr(node) || !Read(node+8,name) || !Text(name,page) || _stricmp(page.data(),"UI_Sponsorship_new.fng"))out.handler=0;
    }return true;
}
static int OptionIndex(const LiveContext& live,uint32_t option){
    for(uint32_t i=0;i<live.shared.count;++i)if(live.shared.options[i]==option)return int(i);return -1;
}
static bool Offered(const LiveContext& live,int index){
    return index>=0 && uint32_t(index)<state.OfferCount() && coop_sponsor::Same(live.shared,state.Current());
}
static void ClearDeferredPopup(){deferredPopupHandler=0;deferredPopupAt=0;}
static void Log(const char* event){
    char line[256];std::snprintf(line,sizeof(line),
        "[SPONSOR] %s session=%llu ticket=%u campaign=%08X stage=%u catalog=%08X offers=%u proposal=%u sponsor=%08X approvedCommit=%u localApplied=%u bothApplied=%u.\r\n",
        event,static_cast<unsigned long long>(state.Session()),state.Ticket(),state.Current().campaign,
        state.Current().stage,state.Current().catalog,state.OfferCount(),state.Proposal(),state.Option(),unsigned(state.Committed()),
        unsigned(state.LocalApplied()),unsigned(state.Complete()));LogMessage(line);
}
static void WaitNotice(unsigned reason,const char* event,const char* text){
    const DWORD now=GetTickCount();
    if(waitProposal==state.Proposal() && waitReason==reason && waitAt && now-waitAt<5000)return;
    waitProposal=state.Proposal();waitReason=reason;waitAt=now;
    SetCoopSponsorPrompt(text);ShowCoopChatStatus(text);Log(event);
}
static void ClearMailbox(){AcquireSRWLockExclusive(&mailboxLock);inboxCount=0;haveOutbound=false;sentSequence=0;ReleaseSRWLockExclusive(&mailboxLock);}
static void Bind(const coop::SessionSnapshot& session){
    if(boundSession==session.sessionId && boundTicket==session.ticket)return;
    uint32_t nonce=GetTickCount()^(GetCurrentProcessId()*0x9E3779B9u)^(++nonceCounter*0x85EBCA6Bu);if(!nonce)nonce=nonceCounter;
    state.Bind(session.sessionId,session.ticket,session.role==coop::Role::Host,nonce);
    boundSession=session.sessionId;boundTicket=session.ticket;promptProposal=promptOption=0;promptDeclined=promptDone=false;
    waitProposal=0;waitReason=0;waitAt=0;receipt={};ClearDeferredPopup();ClearMailbox();SetCoopSponsorPrompt("");
}
static void Publish(uint32_t now){coop_sponsor::Packet packet;
    if(!coop_sponsor::Valid(state.Current())){AcquireSRWLockExclusive(&mailboxLock);haveOutbound=false;ReleaseSRWLockExclusive(&mailboxLock);return;}
    if(!state.Poll(now,packet))return;
    AcquireSRWLockExclusive(&mailboxLock);outbound=packet;haveOutbound=true;ReleaseSRWLockExclusive(&mailboxLock);
}
static void Prompt(const LiveContext& live){
    if(state.Proposal()!=promptProposal || state.Option()!=promptOption){
        promptProposal=state.Proposal();promptOption=state.Option();promptDeclined=promptDone=false;
        if(promptProposal){const int index=OptionIndex(live,promptOption);char message[240];
            std::snprintf(message,sizeof(message),state.IsHost()?"Sponsor proposed: %s. Waiting for Player 2 approval.":
                "Host proposes sponsor: %s. Confirm it or press F5 to approve. F6 declines.",index>=0?live.names[index].data():"unknown");
            ShowCoopChatStatus(message);Log("proposal-visible");}
    }
    if(state.Rejected() && !promptDeclined){promptDeclined=true;ShowCoopChatStatus("Sponsor declined. Host can choose another sponsor.");Log("declined");}
    if(state.Complete() && !promptDone){promptDone=true;ShowCoopChatStatus("Both players signed and saved the agreed sponsor.");Log("both-applied-and-saved");}
}
static void RefreshPrompt(const LiveContext& live,uint32_t now){
    if(deferredPopupHandler)return;
    if(!live.handler && !state.AwaitingDecision() && !state.DueCommit()){SetCoopSponsorPrompt("");return;}
    if(waitAt && waitProposal==state.Proposal() && now-waitAt<5000)return;
    if(state.Complete()){
        SetCoopSponsorPrompt("Both players signed and saved the agreed sponsor.");
        return;
    }
    if(state.Committed()){
        SetCoopSponsorPrompt("Sponsor approved. Signing and saving for both players.");
        return;
    }
    if(live.signedContract){
        SetCoopSponsorPrompt("This stage already has a sponsor.");
        return;
    }
    if(state.Pending()){
        const int index=OptionIndex(live,state.Option());
        const char* name=index>=0?live.names[index].data():"the proposed sponsor";
        char message[256]{};
        const char* format=state.IsHost()?"Sponsor proposed: %s. Waiting for Player 2 approval.":
            state.AwaitingDecision()?"Host proposes %s. Select it and confirm, or press F5 to approve. F6 declines.":
                "%s approved. Waiting for the Host to confirm.";
        std::snprintf(message,sizeof(message),format,name);
        SetCoopSponsorPrompt(message);
        return;
    }
    if(state.IsHost()){
        const char* text=state.Rejected()?"Player 2 declined. Choose a sponsor to propose.":
            "Choose a sponsor to propose to Player 2.";
        SetCoopSponsorPrompt(text);
    }else{
        SetCoopSponsorPrompt("Waiting for the Host to propose a sponsor.");
    }
}
static bool ApproveProposal(const LiveContext& live){
    if(!state.AwaitingDecision())return false;
    if(!coop_sponsor::Same(live.shared,state.Current())){
        WaitNotice(2,"approval-wait-context","Your sponsor offers changed. Waiting for a matching Host proposal. F6 declines.");return false;
    }
    const int index=OptionIndex(live,state.Option());
    if(!Offered(live,index)){
        WaitNotice(3,"approval-wait-local-eligibility","Waiting for the Host's sponsor offers to sync. F6 declines.");return false;
    }
    if(!live.handler){WaitNotice(4,"approval-wait-sponsor-screen","Open the sponsor selection screen to approve this sponsor. F6 declines.");return false;}
    if(live.signedContract){WaitNotice(5,"approval-wait-existing-contract","Your current stage already has a sponsor. F6 declines this proposal.");return false;}
    if(!state.Decide(true))return false;
    waitAt=0;Log("client-approved");const DWORD now=GetTickCount();RefreshPrompt(live,now);Publish(now);return true;
}
static void __fastcall Sign(void* career,void*,uintptr_t contract){
    coop::SessionSnapshot session;
    if(!IsCoopGameplayActive() || !gProfileContext.IsCoopActive()){signOriginal(career,contract);return;}
    // No asynchronous or independent native path may sign a co-op contract.
    if(!Scope(session) || session.sessionId!=state.Session() || session.ticket!=state.Ticket() ||
       !replaying || !state.DueCommit() || !replayContract ||
       reinterpret_cast<uintptr_t>(career)!=addresses.career)return;
    signObserved=true;signOriginal(career,replayContract);
}
static void __fastcall Handler(void* self,void*,uint32_t message,uintptr_t a,uintptr_t b,uintptr_t c){
    if(replaying){handlerOriginal(self,message,a,b,c);return;}
    if(!IsCoopGameplayActive() || !gProfileContext.IsCoopActive()){handlerOriginal(self,message,a,b,c);return;}
    if(message!=kConfirm){
        // Keep the agreement's live screen available until native replay.
        if((state.Pending() || state.DueCommit()) && (message==0x911AB364 || message==0xE1FDE1D1))return;
        handlerOriginal(self,message,a,b,c);return;
    }
    coop::SessionSnapshot session;if(!Scope(session))return;Bind(session);
    LiveContext live;if(!ReadLive(live) || live.handler!=reinterpret_cast<uintptr_t>(self)){
        WaitNotice(1,"confirm-wait-sponsor-screen","The sponsor screen is still loading. Try again in a moment.");return;
    }
    state.SetContext(live.shared);
    state.SetHostOffers(live.eligibleCount,live.previousRating);
    const DWORD now=GetTickCount();
    if(live.signedContract || state.Committed()){RefreshPrompt(live,now);return;}
    if(session.role==coop::Role::Client){
        if(state.AwaitingDecision()){
            const int index=OptionIndex(live,state.Option());
            if(index>=0 && live.selected==live.contracts[index])ApproveProposal(live);
            else{
                char message[256]{};
                std::snprintf(message,sizeof(message),"The Host chose %s. Confirm that sponsor or press F5 to approve it. F6 declines.",
                    index>=0?live.names[index].data():"another sponsor");
                WaitNotice(7,"client-confirmed-different-sponsor",message);
            }
        }else RefreshPrompt(live,now);
        return;
    }
    if(state.Pending()){RefreshPrompt(live,now);return;}
    uint32_t option=0;
    for(uint32_t i=0;i<live.shared.count;++i)
        if(live.contracts[i]==live.selected && Offered(live,int(i)))option=live.shared.options[i];
    if(option && state.Propose(option)){waitAt=0;Log("host-proposed");Prompt(live);RefreshPrompt(live,now);Publish(now);}
    else WaitNotice(8,"host-proposal-wait-offers","Waiting for matching sponsor offers on both players. Try confirming again.");
}
static bool ReceiptCurrent(){
    return receipt.valid && receipt.session==state.Session() && receipt.ticket==state.Ticket() &&
        receipt.proposal==state.Proposal() && receipt.option==state.Option() && coop_sponsor::Same(receipt.context,state.Current());
}
static bool ReadResultsPage(uintptr_t& page){
    uintptr_t manager=0;page=0;
    return Read(addresses.manager,manager) &&
        (!manager || (IsValidUserPtr(manager) && Read(manager+0x38,page)));
}
static bool SponsorOwnsForeground(uintptr_t handler){
    uintptr_t ui=0,engine=0,package=0,descriptor=0,bound=0,owner=0;
    uint8_t active=0,removing=0;
    if(!Read(addresses.ui,ui) || !IsValidUserPtr(ui) || !Read(ui+8,engine) || !IsValidUserPtr(engine) ||
       !Read(engine+0xE4,package) || !IsValidUserPtr(package) ||
       !Read(package+0x14,active) || active!=1 || !Read(package+0x15,removing) || removing ||
       !Read(package+0x40,descriptor) || !IsValidUserPtr(descriptor) ||
       !Read(descriptor+0x18,bound) || bound!=package || !Read(descriptor+0x14,owner) || owner!=handler)return false;
    for(unsigned offset:{0x1Cu,0x20u,0x24u,0x28u}){
        uint32_t state=0;if(!Read(package+offset,state) || state!=1)return false;
    }
    return true;
}
static uintptr_t StageAddress(uint32_t stage){return addresses.career+0x72D8+stage*0x104;}
static bool HasMatchedHostOffers(const LiveContext& live){
    return state.HasHostOffers() && coop_sponsor::Same(live.shared,state.Current());
}
static bool ClientSponsorStage(LiveContext& live){
    coop::SessionSnapshot session;uint32_t uiStage=0;
    return Scope(session) && session.role==coop::Role::Client && state.Session()==session.sessionId &&
        state.Ticket()==session.ticket && ReadLive(live) &&
        Read(addresses.uiStage,uiStage) && uiStage==live.shared.stage;
}
static bool ClientPresentation(LiveContext& live){return ClientSponsorStage(live) && HasMatchedHostOffers(live);}
static int NativeOffersWithHostRating(const LiveContext& live){
    __try {return eligible(reinterpret_cast<void*>(StageAddress(live.shared.stage)),state.HostPreviousRating());}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
static bool __fastcall SponsorPageAvailable(void* self,void*){
    LiveContext live;
    if(!ClientSponsorStage(live))return pageGateOriginal(self);
    uint32_t mode=0,racers=0;uintptr_t context=0;
    if(!Read(addresses.gameMode,mode) || !Read(addresses.resultContext,context) ||
       (context && !Read(addresses.contextRacers,racers)))return pageGateOriginal(self);
    if(mode!=1 || (context && racers>1) || live.shared.stage<=1 || live.signedContract)return false;
    // Results can assemble their pages before the Host publishes the finish.
    // With no offer snapshot yet, only provisionally add the native pages if
    // a sponsorship's race quota is already met. The popup holds entry.
    if(HasMatchedHostOffers(live))return state.OfferCount() || NativeOffersWithHostRating(live)>0;
    __try {return quotaAvailable(reinterpret_cast<void*>(StageAddress(live.shared.stage)));}
    __except(EXCEPTION_EXECUTE_HANDLER){return pageGateOriginal(self);}
}
static int __fastcall SponsorListCount(void* nativeStage,void*,float nativeRating){
    LiveContext live;
    if(ClientPresentation(live) && reinterpret_cast<uintptr_t>(nativeStage)==StageAddress(live.shared.stage) &&
       !live.signedContract && state.OfferCount())return int(state.OfferCount());
    return eligible(nativeStage,nativeRating);
}
static constexpr uint32_t kPopupContinue=0x0C407210,kPopupClick=0x406415E3,kPopupBack=0x911AB364;
static bool ReadSponsorPopup(uintptr_t handler,uintptr_t& manager,uintptr_t& page,uintptr_t& name){
    std::array<char,64> package{};
    return SponsorOwnsForeground(handler) && Read(addresses.manager,manager) && IsValidUserPtr(manager) &&
        Read(manager+0x38,page) && IsValidUserPtr(page) && Read(page+8,name) && Text(name,package) &&
        !_stricmp(package.data(),"UI_SponsorPopup.fng");
}
static bool SkipUnavailableSponsor(uintptr_t handler){
    uintptr_t manager=0,page=0,sponsor=0,next=0,last=0,previous=0,fromName=0,sponsorName=0,toName=0;
    std::array<char,64> selection{},to{};
    if(!ReadSponsorPopup(handler,manager,page,fromName) ||
       !Read(manager+0x34,last) || page==last ||
       !Read(page,sponsor) || !IsValidUserPtr(sponsor) || sponsor==last || sponsor==page ||
       !Read(sponsor+4,previous) || previous!=page ||
       !Read(sponsor+8,sponsorName) || !Text(sponsorName,selection) || _stricmp(selection.data(),"UI_Sponsorship_new.fng") ||
       !Read(sponsor,next) || !IsValidUserPtr(next) || next==sponsor || next==page ||
       !Read(next+4,previous) || previous!=sponsor ||
       !Read(next+8,toName) || !Text(toName,to) || _stricmp(to.data(),"UI_PostRace.fng"))return false;
    // Use the stock result-page transition, without altering the linked pages.
    __try {
        *reinterpret_cast<uintptr_t*>(manager+0x38)=next;
        switchPage(reinterpret_cast<const char*>(fromName),reinterpret_cast<const char*>(toName),0);
    }__except(EXCEPTION_EXECUTE_HANDLER){
        __try {
            if(*reinterpret_cast<uintptr_t*>(addresses.manager)==manager &&
               *reinterpret_cast<uintptr_t*>(manager+0x38)==next)*reinterpret_cast<uintptr_t*>(manager+0x38)=page;
        }__except(EXCEPTION_EXECUTE_HANDLER){}
        return false;
    }
    return true;
}
static void __fastcall SponsorPopup(void* self,void*,uint32_t message,uintptr_t a,uintptr_t b,uintptr_t c){
    if(message==kPopupBack && deferredPopupHandler==reinterpret_cast<uintptr_t>(self))ClearDeferredPopup();
    coop::SessionSnapshot session;
    if((message!=kPopupContinue && message!=kPopupClick) || !Scope(session) || session.role!=coop::Role::Client){
        popupOriginal(self,message,a,b,c);return;
    }
    Bind(session);
    LiveContext live;
    const bool matched=ClientPresentation(live);
    if(matched && state.OfferCount() && !live.signedContract){
        ClearDeferredPopup();waitAt=0;popupOriginal(self,message,a,b,c);return;
    }
    if(matched && (live.signedContract || (!state.OfferCount() && NativeOffersWithHostRating(live)<=0)) &&
       SkipUnavailableSponsor(reinterpret_cast<uintptr_t>(self))){
        ClearDeferredPopup();waitAt=0;Log("client-skipped-no-sponsor");return;
    }
    if(deferredPopupHandler!=reinterpret_cast<uintptr_t>(self)){
        deferredPopupHandler=reinterpret_cast<uintptr_t>(self);deferredPopupAt=GetTickCount();
    }
    WaitNotice(9,"popup-wait-host-offers","Waiting for the Host's sponsor offers to sync.");
}
static void TickOfferPopup(const LiveContext& live,uint32_t now){
    if(!deferredPopupHandler)return;
    const uintptr_t handler=deferredPopupHandler;
    uintptr_t manager=0,page=0,name=0;uint32_t uiStage=0;
    if(!ReadSponsorPopup(handler,manager,page,name)){ClearDeferredPopup();waitAt=0;return;}
    if(!HasMatchedHostOffers(live) || !Read(addresses.uiStage,uiStage) || uiStage!=live.shared.stage)return;
    if(state.OfferCount() && !live.signedContract){
        ClearDeferredPopup();waitAt=0;
        __try {popupOriginal(reinterpret_cast<void*>(handler),kPopupContinue,0,0,0);}
        __except(EXCEPTION_EXECUTE_HANDLER){Log("client-sponsor-popup-transition-exception");}
    }else if((live.signedContract || NativeOffersWithHostRating(live)<=0 || now-deferredPopupAt>=5000) &&
             SkipUnavailableSponsor(handler)){
        ClearDeferredPopup();waitAt=0;Log("client-skipped-no-sponsor");
    }
}
static bool CompleteNativeReceipt(){
    if(!ReceiptCurrent() || receipt.actionComplete || !receipt.page)return false;
    uintptr_t page=0,name=0,signedContract=0;std::array<char,64> pageName{};
    if(!ReadResultsPage(page) || page==receipt.page || SponsorOwnsForeground(receipt.handler) ||
       (page && (!IsValidUserPtr(page) || !Read(page+8,name) || !Text(name,pageName) ||
        !_stricmp(pageName.data(),"UI_Sponsorship_new.fng"))) ||
       !Read(addresses.career+0x73C0+receipt.context.stage*0x104,signedContract) || signedContract!=receipt.contract)return false;
    receipt.actionComplete=true;Log("native-contract-applied-save-pending");return true;
}
static void RecordNativeAttempt(const LiveContext& live,int index,uintptr_t page,uint32_t now){
    if(!ReceiptCurrent())receipt={};
    receipt.valid=true;receipt.session=state.Session();receipt.ticket=state.Ticket();
    receipt.proposal=state.Proposal();receipt.option=state.Option();receipt.context=live.shared;receipt.contract=live.contracts[index];
    receipt.page=page;receipt.handler=live.handler;receipt.lastReplayAttempt=now;
}
static bool SaveReceipt(uint32_t now){
    coop::SessionSnapshot session;uintptr_t signedContract=0;
    if(!Scope(session) || !receipt.valid || !receipt.actionComplete || receipt.saved ||
       session.sessionId!=receipt.session || session.ticket!=receipt.ticket ||
       Hash(CoopPersonalSave::campaign.c_str())!=receipt.context.campaign ||
       !Read(addresses.career+0x73C0+receipt.context.stage*0x104,signedContract) || signedContract!=receipt.contract ||
       (receipt.lastSaveAttempt && now-receipt.lastSaveAttempt<500))return false;
    receipt.lastSaveAttempt=now;
    if(!persist(now)){Log("personal-save-pending");return false;}
    receipt.saved=true;if(ReceiptCurrent())state.MarkApplied();Log("personal-sponsor-saved-and-progress-queued");return true;
}
static bool Apply(const LiveContext& live){
    coop::SessionSnapshot session;LiveContext current;
    if(!Scope(session) || session.sessionId!=state.Session() || session.ticket!=state.Ticket() ||
       !ReadLive(current) || !coop_sponsor::Same(current.shared,live.shared) || current.handler!=live.handler)return false;
    const int index=OptionIndex(current,state.Option());uintptr_t page=0;
    if(!state.DueCommit() || !coop_sponsor::Same(current.shared,state.Current()) || !current.handler ||
       !Offered(current,index) || (current.signedContract && current.signedContract!=current.contracts[index]) ||
       !ReadResultsPage(page) || !page)return false;
    if(ReceiptCurrent() && receipt.actionComplete){SaveReceipt(GetTickCount());return state.LocalApplied();}
    if(!SponsorOwnsForeground(current.handler)){
        WaitNotice(21,"native-sponsor-screen-covered","Sponsor approved. Close any open dialog to continue.");return false;
    }
    const uint32_t now=GetTickCount();
    if(ReceiptCurrent() && now-receipt.lastReplayAttempt<500)return false;
    RecordNativeAttempt(current,index,page,now);
    // The stock signer skips its cash award when this stage already holds a
    // contract. A retry can therefore finish an interrupted page transition.
    bool returned=false;
    replayContract=current.contracts[index];signObserved=false;replaying=true;
    __try {handlerOriginal(reinterpret_cast<void*>(current.handler),kConfirm,0,0,0);returned=true;}
    __except(EXCEPTION_EXECUTE_HANDLER){}
    replaying=false;replayContract=0;
    if(!CompleteNativeReceipt()){
        WaitNotice(20,!returned?"native-replay-exception-held":signObserved?"native-advance-pending":"native-sign-unobserved",
            "Sponsor approved. Waiting for the game to finish the sponsor screen.");return false;
    }
    SaveReceipt(GetTickCount());return state.LocalApplied();
}
struct Hook {
    uintptr_t address;const BYTE* signature;size_t length;void* replacement;bool callSite=false;
    BYTE* trampoline=nullptr;bool active=false;
};
static constexpr BYTE handlerBytes[]={0x55,0x8B,0xEC,0x8B,0x45,0x14};
static constexpr BYTE signBytes[]={0x56,0x8B,0xF1,0x8B,0x86,0xF8,0x7A,0x00,0x00};
static constexpr BYTE popupBytes[]={0x8B,0x44,0x24,0x04,0x3D,0x10,0x72,0x40,0x0C};
static constexpr BYTE pageGateBytes[]={0x51,0x83,0x3D,0x78,0x00,0x85,0x00,0x01};
static constexpr BYTE listCountCall[]={0xE8,0xD5,0xF0,0x03,0x00};
static Hook hooks[]={{0x4C1340,handlerBytes,sizeof(handlerBytes),reinterpret_cast<void*>(Handler)},
    {0x532CF0,signBytes,sizeof(signBytes),reinterpret_cast<void*>(Sign)},
    {0x4C12E0,popupBytes,sizeof(popupBytes),reinterpret_cast<void*>(SponsorPopup)},
    {0x4A4610,pageGateBytes,sizeof(pageGateBytes),reinterpret_cast<void*>(SponsorPageAvailable)},
    {0x4C1596,listCountCall,sizeof(listCountCall),reinterpret_cast<void*>(SponsorListCount),true}};
static void Original(unsigned i,uintptr_t address){
    switch(i){
    case 0:handlerOriginal=reinterpret_cast<HandlerFn>(address);break;
    case 1:signOriginal=reinterpret_cast<SignFn>(address);break;
    case 2:popupOriginal=reinterpret_cast<HandlerFn>(address);break;
    case 3:pageGateOriginal=reinterpret_cast<PageGateFn>(address);break;
    }
}
static void Rollback(){for(unsigned i=0;i<std::size(hooks);++i){auto& h=hooks[i];if(h.active){DWORD old=0,ignored=0;
    if(!VirtualProtect(reinterpret_cast<void*>(h.address),h.length,PAGE_EXECUTE_READWRITE,&old))continue;
    std::memcpy(reinterpret_cast<void*>(h.address),h.signature,h.length);FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(h.address),h.length);
    VirtualProtect(reinterpret_cast<void*>(h.address),h.length,old,&ignored);h.active=false;}
    Original(i,h.address);if(h.trampoline){VirtualFree(h.trampoline,0,MEM_RELEASE);h.trampoline=nullptr;}}installed=false;}
}
bool HandleCoopSponsorDecisionKey(unsigned key){
    using namespace CoopSponsorNative;if(key!=0x74 && key!=0x75)return false;
    coop::SessionSnapshot session;if(!Scope(session) || session.role!=coop::Role::Client || !state.AwaitingDecision())return false;
    // Declining performs no native action and remains available even when the
    // local screen/catalog is unavailable or this career cannot sign the offer.
    if(key==0x75){if(state.Decide(false)){waitAt=0;Log("client-declined");Publish(GetTickCount());}return true;}
    LiveContext live;if(!ReadLive(live)){
        WaitNotice(1,"approval-wait-catalog","Sponsor offers are still loading. F6 declines this proposal.");return true;
    }
    ApproveProposal(live);
    return true;
}
void TickCoopSponsorUi(){
    using namespace CoopSponsorNative;coop::SessionSnapshot session;
    if(!Scope(session)){if(boundSession){state.Unbind();boundSession=0;boundTicket=0;receipt={};ClearDeferredPopup();ClearMailbox();SetCoopSponsorPrompt("");}return;}
    Bind(session);LiveContext live;const uint32_t now=GetTickCount();CompleteNativeReceipt();SaveReceipt(now);
    const bool liveReady=ReadLive(live);
    if(!liveReady && !ReceiptCurrent())state.SetContext({});
    // Keep the committed acknowledgement alive across a native stage handoff
    // until both own-profile saves are acknowledged. New proposals cannot
    // reuse this old context, and all native writes still recheck ReadLive.
    if(liveReady && (!ReceiptCurrent() || state.Complete())){
        state.SetContext(live.shared);
        state.SetHostOffers(live.eligibleCount,live.previousRating);
    }
    std::array<coop_sponsor::Packet,16> packets;size_t count=0;
    AcquireSRWLockExclusive(&mailboxLock);count=inboxCount;std::copy_n(inbox.begin(),count,packets.begin());inboxCount=0;ReleaseSRWLockExclusive(&mailboxLock);
    for(size_t i=0;i<count;++i)state.Receive(packets[i]);
    TickOfferPopup(live,now);
    if(liveReady){Prompt(live);if(state.DueCommit()){
        if(live.handler)Apply(live);
        else if(!ReceiptCurrent())WaitNotice(6,"commit-wait-sponsor-screen",
            "Sponsor approved. Open the sponsor selection screen to finish signing and saving.");
    }}
    if(liveReady)RefreshPrompt(live,GetTickCount());
    else if(!deferredPopupHandler && !state.AwaitingDecision())SetCoopSponsorPrompt("");
    SaveReceipt(now);Publish(now);
}
bool ReceiveCoopSponsorPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now){
    using namespace CoopSponsorNative;(void)now;uint32_t magic=0;if(!data || bytes<4)return false;
    std::memcpy(&magic,data,4);if(magic!=coop_sponsor::kMagic)return false;
    if(bytes!=sizeof(coop_sponsor::Packet) || sender.sin_family!=AF_INET || !IsJoinedCoopSender(sender))return true;
    coop_sponsor::Packet packet{};std::memcpy(&packet,data,sizeof(packet));const auto session=CoopSnapshot();
    if(!coop_sponsor::Valid(packet) || packet.session!=session.sessionId || packet.ticket!=session.ticket ||
       packet.player!=(session.role==coop::Role::Host?2:1))return true;
    AcquireSRWLockExclusive(&mailboxLock);if(inboxCount<inbox.size())inbox[inboxCount++]=packet;ReleaseSRWLockExclusive(&mailboxLock);return true;
}
void TickCoopSponsorNetwork(DWORD now){
    using namespace CoopSponsorNative;(void)now;const auto session=CoopSnapshot();
    if(!session.joined || !session.fresh || !session.peer.Valid())return;
    coop_sponsor::Packet packet{};bool send=false;AcquireSRWLockExclusive(&mailboxLock);
    if(haveOutbound && outbound.sequence!=sentSequence && outbound.session==session.sessionId && outbound.ticket==session.ticket){
        packet=outbound;sentSequence=packet.sequence;send=true;}
    ReleaseSRWLockExclusive(&mailboxLock);if(send)SendSessionBytes(&packet,sizeof(packet),session.peer);
}
bool InstallCoopSponsorHooks(){
    using namespace CoopSponsorNative;if(installed)return true;
    for(const auto& h:hooks){BYTE bytes[16]{};if(!ReadGameMemory(h.address,bytes,h.length) || std::memcmp(bytes,h.signature,h.length)){
        LogMessage("[SPONSOR] Native signature mismatch; shared sponsor hooks unavailable.\r\n");return false;}}
    for(unsigned i=0;i<std::size(hooks);++i){auto& h=hooks[i];
        if(!h.callSite)h.trampoline=static_cast<BYTE*>(VirtualAlloc(nullptr,32,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
        DWORD old=0,ignored=0;if((!h.callSite && !h.trampoline) || !VirtualProtect(reinterpret_cast<void*>(h.address),h.length,PAGE_EXECUTE_READWRITE,&old)){Rollback();return false;}
        if(h.trampoline){
            std::memcpy(h.trampoline,h.signature,h.length);CoopCareerNative::WriteRelativeJump(h.trampoline+h.length,reinterpret_cast<void*>(h.address+h.length));
            FlushInstructionCache(GetCurrentProcess(),h.trampoline,h.length+5);Original(i,reinterpret_cast<uintptr_t>(h.trampoline));
        }
        CoopCareerNative::WriteRelativeJump(reinterpret_cast<BYTE*>(h.address),h.replacement);
        if(h.callSite)*reinterpret_cast<BYTE*>(h.address)=0xE8;
        if(h.length>5)std::memset(reinterpret_cast<void*>(h.address+5),0x90,h.length-5);
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(h.address),h.length);VirtualProtect(reinterpret_cast<void*>(h.address),h.length,old,&ignored);h.active=true;
    }installed=true;LogMessage("[SPONSOR] Host proposal / Client approval / paired native signing installed.\r\n");return true;
}
