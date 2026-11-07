// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Include after the Area/Choice/Detours/Runtime adapters. Game thread only.
// No hooks are installed here. Only a genuine own native Arrival callback can
// capture the witness; a peer motion packet or a hash supplied by the network
// is never a local arrival proof. Witness contains no process pointers.
namespace CoopSpecialNative { namespace Destination {
struct Descriptor {uintptr_t area=0,race=0,shop=0,cover=0,mission=0;};
static_assert(sizeof(Descriptor)==20,"Stock 532B10/515060 descriptor is five x86 pointers");
using ResolveFn=void(__thiscall*)(void*,Descriptor*);
using ZeroFn=void(__thiscall*)(void*);
struct Addresses {uintptr_t uiManager=0x8384C4,coverVtable=0x791838,coverEvent=0x498F00,shopVtable=0x78F978,shopEvent=0x499690;};
static Addresses addresses;
static ResolveFn resolve=reinterpret_cast<ResolveFn>(0x515060);
static ZeroFn acknowledgeFailure=reinterpret_cast<ZeroFn>(0x501640);
static ZeroFn clearFailedMission=reinterpret_cast<ZeroFn>(0x533CF0);
static const Catalog* (*catalog)()=FactsCatalog;
static bool (*arrivalProof)(uint32_t,DWORD)=NoteArrival;
struct Witness {
    uint64_t session=0;uint32_t ticket=0,nonce=0,operation=0,request=0,campaign=0,catalog=0;
    coop_special::Spec spec{};uint32_t area=0,car=0,world=0;
    bool valid=false,attempted=false,released=false,fault=false,nativeEntered=false;
};
static Witness witness;
static Witness aborted;
static void Reset(){witness={};aborted={};}
static bool Operation(const Witness& w){
    return w.valid&&w.session==state.Session()&&w.ticket==state.Ticket()&&w.nonce==state.Nonce()&&
        w.operation==state.Operation()&&w.request==state.RequestId()&&w.campaign==state.CurrentContext().campaign&&
        w.catalog==state.CurrentContext().catalog&&coop_special::Same(w.spec,state.Current());
}
static bool InRelease(){return Operation(witness)&&witness.attempted&&!witness.released&&!witness.fault;}
static bool Request(const coop_special::NativeRequest& r,coop_special::NativeAction action){
    return r.action==action&&r.serial&&r.session==state.Session()&&r.ticket==state.Ticket()&&r.nonce==state.Nonce()&&
        r.operation==state.Operation()&&r.request==state.RequestId()&&coop_special::Same(r.spec,state.Current());
}
// Re-read immutable native row scalars/names as well as the catalog cache key.
// A recycled pointer with a different row must not inherit an earlier witness.
static bool NativeRow(const Record& row){
    BYTE bytes[0x18]{};uintptr_t strings=0;char name[96]{},start[96]{};
    if(!ReadSpan(row.pointer,bytes,sizeof(bytes))||!Read(CoopSpecialNative::addresses.strings,strings)||!IsValidUserPtr(strings))return false;
    uint16_t nameAt=0,startAt=0;int16_t stage=0;int32_t seconds=0;uint32_t destination=0;
    std::memcpy(&nameAt,bytes,2);std::memcpy(&startAt,bytes+2,2);std::memcpy(&stage,bytes+4,2);
    std::memcpy(&destination,bytes+0xC,4);std::memcpy(&seconds,bytes+0x10,4);
    const uint8_t kind=row.spec.kind==coop_special::Kind::Visual?1:row.spec.kind==coop_special::Kind::Performance?2:4;
    return Text(strings+nameAt,name)&&Text(strings+startAt,start)&&NativeHash(name)==row.spec.id&&
        NativeHash(start)==row.spec.start&&stage==row.spec.stage&&destination==row.spec.destination&&
        seconds>0&&seconds<=3600&&uint32_t(seconds)*1000==row.spec.durationMs&&bytes[0x14]==kind;
}
static bool OwnedMission(const Record*& row,uint32_t& nativePhase){
    row=nullptr;nativePhase=0;
    if(!installed||!OwnCareer()||!IsCoopGameplayActive()||!state.Active()||!coop_special::Timed(state.Current().kind)||
        !lifecycle.NativeProofWindow(state))return false;
    uint32_t flow=0,mode=0,track=0,stage=0;uintptr_t mission=0;
    const auto* all=catalog();const auto* found=all?Find(*all,state.Current().id):nullptr;
    if(!found||!coop_special::Same(found->spec,state.Current())||all->digest!=state.CurrentContext().catalog||
        CampaignHash(CoopPersonalSave::campaign)!=state.CurrentContext().campaign||!NativeRow(*found)||
        !Read(areaAddresses.flow,flow)||flow!=6||!Read(areaAddresses.mode,mode)||mode!=1||
        !Read(areaAddresses.track,track)||track!=4000||!Read(CoopSpecialNative::addresses.career+0x7AF8,stage)||stage!=found->spec.stage||
        !Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission!=found->pointer||
        !Read(CoopSpecialNative::addresses.career+0xD1C,nativePhase))return false;
    row=found;return true;
}
static bool Joined(){
    const auto session=CoopSnapshot();return session.joined&&session.fresh&&session.initialBarrierComplete&&
        session.sessionId==state.Session()&&session.ticket==state.Ticket()&&
        (state.Player()==1?session.role==coop::Role::Host:session.role==coop::Role::Client);
}
static bool ResolveCall(Descriptor& out){
    __try {resolve(reinterpret_cast<void*>(CoopSpecialNative::addresses.career+0xC00),&out);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool DestinationHash(const Descriptor& d,uint32_t& hash){
    // Preserve 532B10 priority: race, shop, cover, mission. Ignored trailing
    // fields in a genuine area descriptor must not prevent its chosen target
    // from being held, nor cause a lower-priority cover to steal a shop entry.
    hash=0;if(d.race||!IsValidUserPtr(d.area)||(!d.shop&&!d.cover))return false;
    return d.shop?IsValidUserPtr(d.shop)&&Read(d.shop+0x38,hash):IsValidUserPtr(d.cover)&&Read(d.cover+0x20,hash);
}
static bool FreshDescriptor(const Record& row,Descriptor& out,uint32_t& areaHash){
    Descriptor next;uint32_t hash=0,recordArea=0;areaHash=0;
    if(!ResolveCall(next)||next.mission||bool(next.shop)==bool(next.cover)||!DestinationHash(next,hash)||hash!=row.spec.destination||
        (row.spec.kind==coop_special::Kind::Show?!next.cover:!next.shop)||
        !Read(next.area+0x30,areaHash)||!areaHash||
        !Read((next.shop?next.shop+0x3C:next.cover+0x2C),recordArea)||recordArea!=areaHash||
        FindArea(areaHash)!=next.area)return false;
    if(next.cover){
        const auto* all=catalog();const auto* cover=all?Find(*all,hash):nullptr;
        if(!cover||cover->pointer!=next.cover||coop_special::Timed(cover->spec.kind)||cover->spec.start!=areaHash)return false;
    }
    out=next;return true;
}
static bool SelectedSame(const Descriptor& incoming,const Descriptor& fresh){return incoming.area==fresh.area&&!incoming.race&&
    (fresh.shop?incoming.shop==fresh.shop:!incoming.shop&&incoming.cover==fresh.cover);}
static bool ReadOwn(uint32_t& carId,uint32_t& world,float(&position)[3],DWORD now){
    uintptr_t car=0;Choice::OwnCar own;PublishedMotion published{};
    if(!ReadOwnAreaPosition(car,position)||!Choice::ReadGarageCar(own)||!ReadPublishedMotion(published,now)||
        !published.packet.sequence||published.local.playerInstance!=car||published.packet.sessionId!=state.Session()||
        published.packet.flowState!=6||published.packet.trackId!=4000||!published.packet.worldEpoch)return false;
    carId=own.id;world=published.packet.worldEpoch;return carId!=0;
}
static Witness CurrentWitness(uint32_t area,uint32_t car,uint32_t world){
    Witness w;w.session=state.Session();w.ticket=state.Ticket();w.nonce=state.Nonce();w.operation=state.Operation();
    w.request=state.RequestId();w.campaign=state.CurrentContext().campaign;w.catalog=state.CurrentContext().catalog;
    w.spec=state.Current();w.area=area;w.car=car;w.world=world;w.valid=true;return w;
}
// Detours::Callbacks::arrival. Return false ONLY for unrelated native
// discoveries. A matching active destination stays held even if its purported
// local geometry/pointer is invalid: it must not fall through and open a solo UI.
static bool Intercept(void* self,uintptr_t descriptor){
    const Record* row=nullptr;uint32_t phase=0,hash=0;Descriptor incoming;
    if(uintptr_t(self)!=CoopSpecialNative::addresses.career||!OwnedMission(row,phase)||
        !(state.Phase()&coop_special::Go)||!ReadSpan(descriptor,&incoming,sizeof(incoming))||
        !DestinationHash(incoming,hash)||hash!=row->spec.destination)return false;
    if(state.Terminal()||!Joined()||phase!=1||(Operation(witness)&&(witness.attempted||witness.fault)))return true;
    Descriptor fresh;uint32_t area=0,car=0,world=0;float position[3]{};const DWORD now=runtimeNow();
    if(!FreshDescriptor(*row,fresh,area)||!SelectedSame(incoming,fresh)||!ReadOwn(car,world,position,now)||
        !AreaAtPoint(area,position[0],position[1]))return true;
    if(Operation(witness)){
        if(witness.area!=area||witness.car!=car||witness.world!=world)return true;
    }else witness=CurrentWitness(area,car,world);
    // Do not publish a proof when a native constructor/unknown old operation
    // has no matching game-thread lifecycle window.
    if(!(state.Own()&coop_special::Arrived)&&(!arrivalProof||!arrivalProof(row->spec.id,now)))witness={};
    return true;
}
static bool ArrivalCall(const Descriptor& descriptor){
    __try {reinterpret_cast<Detours::OneFn>(Detours::Original(Detours::Arrival))(
        reinterpret_cast<void*>(CoopSpecialNative::addresses.career),uintptr_t(&descriptor));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
// Native can already own a destination modal by the time the UI-thread
// observer runs. A real phase2/3, exact copied descriptor and owned FEng prompt
// are additional native evidence, NOT arrival inferred from a peer position.
static bool ArrivedPrompt(const Descriptor& d){
    uintptr_t area=0,race=0,shop=0,cover=0;
    const auto career=CoopSpecialNative::addresses.career;
    if(!Read(career+0x8704,area)||area!=d.area||!Read(career+0x8708,race)||race||
        !Read(career+0x870C,shop)||shop!=d.shop||!Read(career+0x8710,cover)||cover!=d.cover)return false;
    const char* screen=d.cover?"UI_EngageEventDialog.fng":"UI_EngageShopDialog.fng";
    uintptr_t ui=0,engine=0,package=0,descriptor=0,handler=0,vt=0,event=0;
    if(!Read(addresses.uiManager,ui)||!IsValidUserPtr(ui)||!Read(ui+8,engine)||!IsValidUserPtr(engine)||
        !Read(engine+0xE4,package))return false;
    std::array<uintptr_t,64> seen{};unsigned count=0;
    while(package){
        if(count==seen.size()||std::find(seen.begin(),seen.begin()+count,package)!=seen.begin()+count)return false;
        seen[count++]=package;Feng::Package p;if(!Feng::ReadPackage(package,p))return false;
        if(p.active&&Feng::Name(p.name,screen)){
            if(!Read(package+0x40,descriptor)||!IsValidUserPtr(descriptor)||!Read(descriptor+0x14,handler)||
                !IsValidUserPtr(handler)||!Read(handler,vt)||vt!=(d.cover?addresses.coverVtable:addresses.shopVtable)||
                !Read(vt+8,event)||event!=(d.cover?addresses.coverEvent:addresses.shopEvent))return false;
            return Feng::Owned(screen,handler,true,addresses.uiManager);
        }
        package=p.next;
    }
    return false;
}
static void Observe(){
    const Record* row=nullptr;uint32_t phase=0,area=0,car=0,world=0;Descriptor fresh;float position[3]{};
    // Cheap state/phase filter FIRST. While driving (phase1), re-reading both
    // immutable native strings character-by-character every rendered frame
    // proves nothing new; full proof is needed only at the actual modal.
    if(!state.Active()||!coop_special::Timed(state.Current().kind)||state.Terminal()||
        !(state.Phase()&coop_special::Go)||(state.Own()&coop_special::Arrived)||
        !Read(CoopSpecialNative::addresses.career+0xD1C,phase)||
        phase!=(state.Current().kind==coop_special::Kind::Show?3u:2u))return;
    if(!OwnedMission(row,phase)||!Joined())return;
    if(!FreshDescriptor(*row,fresh,area)||!ArrivedPrompt(fresh)||!ReadOwn(car,world,position,runtimeNow())||
        !AreaAtPoint(area,position[0],position[1]))return;
    witness=CurrentWitness(area,car,world);witness.nativeEntered=true;
    if(!arrivalProof||!arrivalProof(row->spec.id,runtimeNow())){witness={};return;}
    LogMessage("[SPECIAL_ARRIVAL] Exact native destination modal, own mission/car/polygon verified on UI thread.\r\n");
}
static coop_special::NativeResult Release(const coop_special::NativeRequest& request){
    using coop_special::NativeResult;
    if(!Request(request,coop_special::NativeAction::Release)||!Operation(witness)||witness.fault)return NativeResult::Refused;
    if(witness.released)return NativeResult::Applied;
    if(witness.attempted)return NativeResult::Refused;
    const Record* row=nullptr;uint32_t phase=0,area=0,car=0,world=0;float position[3]{};Descriptor fresh;
    if(!OwnedMission(row,phase)||!Joined()||state.Terminal()||!(state.Phase()&coop_special::Release)||
        !(state.Own()&coop_special::Arrived)||
        phase!=(witness.nativeEntered?(row->spec.kind==coop_special::Kind::Show?3u:2u):1u))return NativeResult::Wait;
    if(!FreshDescriptor(*row,fresh,area)||!ReadOwn(car,world,position,runtimeNow())||area!=witness.area||
        car!=witness.car||world!=witness.world||!AreaAtPoint(area,position[0],position[1]))return NativeResult::Wait;
    if(witness.nativeEntered){
        if(!ArrivedPrompt(fresh))return NativeResult::Wait;
        witness.released=true;return NativeResult::Applied; // Never replay an already-entered native destination.
    }
    witness.attempted=true; // Latch BEFORE original can pump native UI messages.
    const uintptr_t expectedMission=row->pointer;
    // The original trampoline already skips its own entry hook. Do NOT set
    // a global Arrival replay bit: a nested native callback must still reach
    // Intercept and be held by this exact operation's pre-call latch.
    if(!ArrivalCall(fresh)){witness.fault=true;return NativeResult::Refused;}
    uintptr_t mission=0;uint32_t result=0;
    if(!Operation(witness)||!Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission!=expectedMission||
        !Read(CoopSpecialNative::addresses.career+0xD1C,result)||result!=(fresh.shop?2u:3u)){
        witness.fault=true;return NativeResult::Refused;
    }
    witness.released=true;return NativeResult::Applied;
}
static bool IdleHud(){
    return ::CoopSpecialNative::Feng::Idle(addresses.uiManager);
}
static bool AbortCall(uintptr_t expectedMission){
    __try {acknowledgeFailure(reinterpret_cast<void*>(CoopSpecialNative::addresses.career+0xC00));
        uintptr_t current=0;uint32_t phase=0;
        if(!Read(CoopSpecialNative::addresses.career+0xC08,current)||current!=expectedMission||
            !Read(CoopSpecialNative::addresses.career+0xD1C,phase)||phase!=7)return false;
        clearFailedMission(reinterpret_cast<void*>(CoopSpecialNative::addresses.career+0xC00));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
// Bounded native cleanup ONLY before destination UI/photo/unique selection.
// Arrived phase 2/3 needs the driver's separately proven owned UI cleanup; never
// call unique's 5419C0, whose "forfeit" marks the event completed.
static coop_special::NativeResult AbortTravelling(const coop_special::NativeRequest& request){
    using coop_special::NativeResult;
    if(!Request(request,coop_special::NativeAction::Abort)||!state.Terminal()||(state.Phase()&coop_special::Commit))return NativeResult::Refused;
    if(Operation(aborted))return aborted.released?NativeResult::Applied:NativeResult::Refused;
    const Record* row=nullptr;uint32_t phase=0;
    if(!OwnedMission(row,phase)||phase!=1||!IdleHud()||(Operation(witness)&&witness.attempted))return NativeResult::Wait;
    uintptr_t localCar=0;float position[3]{};Choice::OwnCar own;
    if(!ReadOwnAreaPosition(localCar,position)||!Choice::ReadGarageCar(own))return NativeResult::Wait;
    aborted=CurrentWitness(0,own.id,0);aborted.attempted=true;
    if(!AbortCall(row->pointer)){aborted.fault=true;return NativeResult::Refused;}
    uintptr_t mission=1;uint32_t after=0;
    if(!Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission||
        !Read(CoopSpecialNative::addresses.career+0xD1C,after)||after!=7){aborted.fault=true;return NativeResult::Refused;}
    aborted.released=true;return NativeResult::Applied;
}
}} // namespace CoopSpecialNative::Destination
