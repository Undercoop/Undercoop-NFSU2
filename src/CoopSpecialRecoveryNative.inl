// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Fresh OWN native context for an immutable old Authorized receipt. No peer
// profile/handler/part pointer, raw engine-structure write, fake input, car
// teleport or global replay-mask capability is used. State is restored locally
// offline: its old authorization can NEVER produce/consume gameplay packets.
namespace CoopSpecialNative { namespace RecoveryNative {
using Scope=coop_special_receipt::Scope;using Phase=coop_special_receipt::Phase;
using Result=coop_special::NativeResult;
enum class Step {Fresh,Arrival,Dialog,Accept,Selection,Proof,Commit,Exit,Complete};
struct Context {Record event,cover;Destination::Descriptor descriptor;};
struct Dialog {uintptr_t handler=0,button=0;bool shop=false;};
struct Attempt {
    bool valid=false,fault=false,inCall=false,startCalled=false,arrivalCalled=false,openCalled=false,acceptCalled=false;
    bool selectCalled=false,exitCalled=false,exitFrame=false,uiSeen=false;Scope scope;Step step=Step::Fresh;
};
static Attempt attempt;
static ReceiptStore::Store* storage=&ReceiptStore::store;
static const Catalog* (*catalog)()=FactsCatalog;
static const Record* (*findRecord)(uint32_t)=FindRuntimeRecord;
static uint32_t (*carId)()=Proof::ReadOwnCarId;
static bool NativeRow(const Record& row){return coop_special::Timed(row.spec.kind)?Destination::NativeRow(row):Reward::NativeCover(row);}
static bool (*nativeRow)(const Record&)=NativeRow;
static bool (*readOwnCar)(Choice::OwnCar&,uintptr_t*)=Choice::ReadOwnCar;
static bool (*ownedSelector)(uintptr_t,uintptr_t&,bool)=Choice::OwnedScreen;
static bool (*fenceHeld)(const Scope&)=Reward::FenceHeld;
static bool ExitReady(const Scope& scope){return storage&&storage->Ready()&&storage->tx.phase==Phase::Paid&&
    coop_special_receipt::Same(storage->tx.scope,scope)&&!gSpecialSaveFence.Held({scope.session,scope.ticket,scope.operation});}
static bool (*exitReady)(const Scope&)=ExitReady;
static bool Same(){return attempt.valid&&storage&&coop_special_receipt::Same(attempt.scope,storage->tx.scope);}
static bool Names(){return installed&&OwnCareer()&&storage&&storage->Ready()&&
    !coop_special_receipt::Empty(storage->tx)&&coop_special_receipt::Valid(storage->tx)&&
    storage->tx.scope.personal==CoopPersonalSave::personal&&storage->tx.scope.campaign==CoopPersonalSave::campaign;}
static bool NormalOperation(){return !state.OfflineRecovery()&&state.Session()&&Names()&&Reward::Operation(storage->tx.scope);}
static bool Busy(){return attempt.valid?attempt.step!=Step::Complete:
    Names()&&storage->tx.phase==Phase::Authorized&&!NormalOperation();}
static bool Offline(){return Busy()&&Same()&&state.OfflineRecovery()&&Reward::Operation(attempt.scope);}
static bool Facts(Context& out){
    if(!Names())return false;const auto& scope=storage->tx.scope;const auto* all=catalog();
    const auto* event=all?Find(*all,scope.event):nullptr;uint32_t stage=~0u;
    if(!all||all->digest!=scope.catalog||CampaignHash(scope.campaign)!=state.CurrentContext().campaign||
        !event||event->spec.stage!=scope.stage||uint8_t(event->spec.kind)!=scope.kind||
        !Read(CoopSpecialNative::addresses.career+0x7AF8,stage)||stage>=8||stage<scope.stage||
        (storage->tx.phase==Phase::Authorized&&stage!=scope.stage)||scope.carId!=carId()||
        !nativeRow(*event))return false;
    Context next;next.event=*event;
    if(event->spec.kind!=coop_special::Kind::Visual&&event->spec.kind!=coop_special::Kind::Performance){
        const auto* cover=findRecord(event->spec.kind==coop_special::Kind::Show?event->spec.destination:event->spec.id);
        if(!cover||cover->spec.stage!=event->spec.stage||!Reward::NativeCover(*cover))return false;next.cover=*cover;
    }
    out=next;return true;
}
static bool (*facts)(Context&)=Facts;
static bool FreshWorld(){
    uint32_t flow=0,mode=0,track=0,phase=~0u;uintptr_t mission=1,car=0;float position[3]{};
    return Read(CoopSpecialNative::addresses.flow,flow)&&flow==6&&Read(CoopSpecialNative::addresses.mode,mode)&&mode==1&&
        Read(areaAddresses.track,track)&&track==4000&&Read(CoopSpecialNative::addresses.career+0xC08,mission)&&!mission&&
        Read(CoopSpecialNative::addresses.career+0xD1C,phase)&&(phase==0||phase==7)&&Destination::IdleHud()&&ReadOwnAreaPosition(car,position);
}
static bool (*freshWorld)()=FreshWorld;
static bool NativeStart(const Context& context){
    __try {reinterpret_cast<Detours::OneFn>(Detours::Original(Detours::Start))(
        reinterpret_cast<void*>(CoopSpecialNative::addresses.career+0xC00),context.event.pointer);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool (*start)(const Context&)=NativeStart;
static bool Resolve(Context& context){
    if(coop_special::Timed(context.event.spec.kind)){uint32_t area=0;
        return Destination::FreshDescriptor(context.event,context.descriptor,area);}
    const uintptr_t area=FindArea(context.cover.spec.start);uint32_t actual=0;
    if(!IsValidUserPtr(area)||!Read(area+0x30,actual)||actual!=context.cover.spec.start)return false;
    context.descriptor={area,0,0,context.cover.pointer,0};return true;
}
static bool (*resolve)(Context&)=Resolve;
static bool NativeArrival(const Context& context){return Destination::ArrivalCall(context.descriptor);}
static bool (*arrival)(const Context&)=NativeArrival;
static bool BoundCover(const Context& context){uint32_t hash=0;return Read(CoopSpecialNative::addresses.career+0x4218,hash)&&hash==context.cover.spec.id;}
static bool NativeAfterArrival(const Context& context){
    return coop_special::Timed(context.event.spec.kind)?Reward::MissionIs(context.event,context.descriptor.shop?2:3):BoundCover(context);
}
static bool (*afterArrival)(const Context&)=NativeAfterArrival;
static bool ReadDialog(const Context& context,Dialog& out){
    Dialog next;
    if(context.event.spec.kind==coop_special::Kind::Visual||context.event.spec.kind==coop_special::Kind::Performance){
        Ui::Live live;if(!Reward::MissionIs(context.event,2)||!Abort::DestinationShop(context.event)||
            !Ui::readLive(Ui::Screen::Failure,0,live,true))return false;
        uintptr_t widget=0;__try {widget=uintptr_t(Engage::findObject(Ui::kFailure,Engage::kTimedButton));}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        uint32_t hash=0;if(!IsValidUserPtr(widget)||!Read(widget+0x10,hash)||hash!=Engage::kTimedButton)return false;
        next={live.handler,widget,true};
    }else{
        Engage::Live live;if(!BoundCover(context)||!Engage::ReadLive(context.cover.spec,live))return false;
        next={live.handler,live.widget,false};
    }
    out=next;return true;
}
static bool (*readDialog)(const Context&,Dialog&)=ReadDialog;
static bool NativeQueuePending(){uint8_t pending=1;return !Read(uintptr_t(0x861E78),pending)||pending!=0;}
static bool (*queuePending)()=NativeQueuePending;
static bool (*canOpen)()=Engage::UiCanOpen;
static bool NativeOpen(){return Engage::CallsOpen();}
static bool (*open)()=NativeOpen;
static bool NativeAccept(const Dialog& dialog){
    __try {reinterpret_cast<Detours::HandlerFn>(Detours::Original(dialog.shop?Detours::FailureUi:Detours::Outer))(
        reinterpret_cast<void*>(dialog.handler),Engage::kClick,dialog.button,0,0);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool (*accept)(const Dialog&)=NativeAccept;
static bool AfterAccept(const Context& context){return context.event.spec.kind==coop_special::Kind::Visual||
    context.event.spec.kind==coop_special::Kind::Performance?Reward::MissionIs(context.event,4):BoundCover(context);}
static bool (*afterAccept)(const Context&)=AfterAccept;
// Select through the stock widget setter, never by overwriting its selected
// pointer. Every candidate is freshly resolved from this OWN intrusive list;
// no durable/recovered process pointer can be passed into native code.
struct Selection {uintptr_t handler=0,item=0;Choice::Stable stable;};
static bool SelectionSignatures(uintptr_t vtable){uintptr_t select=0,refresh=0;
    return vtable==0x79C0F4&&Read(vtable+0x14,select)&&select==0x52F680&&
        Read(uintptr_t(0x79CAB0)+0x10,refresh)&&refresh==0x5618B0;
}
static bool (*selectionSignatures)(uintptr_t)=SelectionSignatures;
static bool FindSelection(const Context& context,Selection& out){
    uintptr_t handler=0,first=0,last=0,vt=0;Choice::OwnCar own;uint8_t performance=0;
    if(!ownedSelector(0,handler,false)||!readOwnCar(own,nullptr)||own.id!=attempt.scope.carId||
        !Read(handler+0x151,performance)||performance>1||
        (performance!=0)!=(context.event.spec.kind==coop_special::Kind::Performance)||
        !Read(handler+0x4C,vt)||!selectionSignatures(vt)||
        !Read(handler+0x50,first)||!Read(handler+0x54,last))return false;
    const uintptr_t sentinel=handler+0x50;uintptr_t cursor=first,previous=sentinel,target=0;unsigned count=0,hits=0;
    while(cursor!=sentinel){
        if(++count>128||cursor<4||!IsValidUserPtr(cursor-4))return false;
        const uintptr_t item=cursor-4;uintptr_t next=0,back=0,kind=0;uint32_t slot=~0u,id=0;
        if(!Read(cursor,next)||!Read(cursor+4,back)||back!=previous||!Read(item,kind)||kind!=0x79B578||!Read(item+0x48,slot))return false;
        if(slot==attempt.scope.choiceSlot){
            if(performance)id=slot<10?slot+1:0;
            else {uintptr_t part=0;if(!Read(item+0x4C,part)||!IsValidUserPtr(part)||!Read(part,id))return false;
                uintptr_t resolved=0;if(id==attempt.scope.choiceId&&(!Choice::NativePart(own.modelIndex,slot,id,resolved)||part!=resolved))return false;}
            if(id==attempt.scope.choiceId){uint8_t disabled=1,visible=0,locked=1,enabled=0,selectable=0;
                if(!Read(item+0x20,disabled)||disabled||!Read(item+0x21,visible)||visible!=1||!Read(item+0x22,locked)||locked||
                    !Read(item+0x44,enabled)||enabled!=1||!Read(item+0x45,selectable)||selectable!=1)return false;
                target=item;++hits;}
        }
        previous=cursor;cursor=next;
    }
    if(hits!=1||previous!=last)return false;
    Selection found;found.handler=handler;found.item=target;found.stable={own,attempt.scope.choiceId,attempt.scope.choiceSlot,context.event.spec.kind};
    if(!Choice::Valid(found.stable))return false;out=found;return true;
}
static bool (*findSelection)(const Context&,Selection&)=FindSelection;
using SelectFn=bool(__thiscall*)(void*,uintptr_t);
static SelectFn selectItem=reinterpret_cast<SelectFn>(0x52F680);
static Detours::ZeroFn refreshSelection=reinterpret_cast<Detours::ZeroFn>(0x5618B0);
static bool NativeSelect(const Selection& selection){
    __try {
        if(!selectItem(reinterpret_cast<void*>(selection.handler+0x4C),selection.item))return false;
        refreshSelection(reinterpret_cast<void*>(selection.handler));return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool (*select)(const Selection&)=NativeSelect;
static bool Capture(const Context& context,const Selection& selection){
    Choice::Frozen frozen;
    if(!Choice::CaptureChoice(context.event.spec,selection.handler,frozen)||!Choice::Same(frozen.stable,selection.stable)||
        frozen.stable.car.id!=attempt.scope.carId||frozen.stable.id!=attempt.scope.choiceId||frozen.stable.slot!=attempt.scope.choiceSlot)return false;
    Proof::pending={};Proof::pending.valid=true;Proof::pending.identity=attempt.scope;Proof::pending.unique=frozen;return true;
}
static bool (*capture)(const Context&,const Selection&)=Capture;
static bool BindCover(const Context& context){
    Ui::Live live;const auto screen=context.cover.spec.kind==coop_special::Kind::Frontend?Ui::Screen::Magazine:Ui::Screen::Photo;
    if(!BoundCover(context)||!Ui::readLive(screen,0,live,true))return false;
    if(screen==Ui::Screen::Photo){uintptr_t mode=0;if(!Read(live.handler+0x50,mode)||!Abort::PhotoMode(mode))return false;}
    Proof::pending={};Proof::pending.valid=true;Proof::pending.identity=attempt.scope;Proof::pending.cover=context.cover.spec.id;return true;
}
static bool (*bindCover)(const Context&)=BindCover;
static bool ScreenGone(const Context& context){
    return Abort::ScreenGone(context.event.spec.kind==coop_special::Kind::Visual||context.event.spec.kind==coop_special::Kind::Performance?
        Abort::Surface::Unique:context.cover.spec.kind==coop_special::Kind::Frontend?Abort::Surface::Magazine:Abort::Surface::Photo);
}
static bool (*screenGone)(const Context&)=ScreenGone;
static bool Exit(const Context& context){
    if(!exitReady(attempt.scope))return false;
    if(attempt.uiSeen&&screenGone(context))return true;
    if(context.event.spec.kind==coop_special::Kind::Visual||context.event.spec.kind==coop_special::Kind::Performance)return false;
    if(attempt.exitCalled)return false;
    const auto screen=context.cover.spec.kind==coop_special::Kind::Frontend?Ui::Screen::Magazine:Ui::Screen::Photo;
    Ui::Live live;if(!Ui::readLive(screen,0,live,true))return false;
    if(screen==Ui::Screen::Photo){uintptr_t mode=0;if(!Read(live.handler+0x50,mode)||!Abort::PhotoMode(mode))return false;}
    attempt.exitCalled=true;attempt.inCall=true;
    const bool called=Ui::DispatchCall(screen,live.handler);attempt.inCall=false;
    if(!called)attempt.fault=true;
    return false; // Wait for fresh native lookup absence, never assume UI died.
}
static bool (*exit)(const Context&)=Exit;
static bool Initialize(){
    if(!Names()||storage->tx.phase!=Phase::Authorized||!storage->tx.ownDone||NormalOperation()||
        (state.Session()&&!state.OfflineRecovery())||lifecycle.InNativeCall()||
        (Reward::attempt.valid&&Reward::attempt.entered)||!fenceHeld(storage->tx.scope))return false;
    const auto scope=storage->tx.scope;const auto* all=catalog();const auto* event=all?Find(*all,scope.event):nullptr;
    if(!all||all->digest!=scope.catalog||!event||event->spec.stage!=scope.stage||uint8_t(event->spec.kind)!=scope.kind||
        !state.RestoreAuthorized(scope.session,scope.ticket,uint8_t(scope.localPlayer),scope.hostNonce,scope.clientNonce,
            scope.operation,{CampaignHash(scope.campaign),scope.catalog,uint8_t(scope.stage)},event->spec)||!lifecycle.ResumeAuthorized(state))return false;
    Context context;if(!facts(context)||!freshWorld())return false;
    if(Proof::pending.valid&&!Proof::SameBase(Proof::pending.identity,scope))return false;
    attempt={};attempt.valid=true;attempt.scope=scope;return true;
}
// RecoveryRuntime may learn an irrevocable OLD Host Commit while the same
// process still owns its original unpaid Client UI. Keep that fresh local
// witness/context; rebuilding Start/Arrival would be wrong and unnecessary.
// The caller has already durably Authorized its OWN matching receipt.
static bool AdoptExisting(){
    if(attempt.valid)return Same()&&!attempt.fault;
    if(!Names()||storage->tx.phase!=Phase::Authorized||!storage->tx.ownDone||state.OfflineRecovery()||state.Terminal()||
        !Reward::Operation(storage->tx.scope)||lifecycle.InNativeCall()||
        (Reward::attempt.valid&&Reward::attempt.entered)||!fenceHeld(storage->tx.scope)||
        !Proof::pending.valid||!Proof::SameBase(Proof::pending.identity,storage->tx.scope))return false;
    Context context;if(!facts(context))return false;
    const auto scope=storage->tx.scope;
    if(context.event.spec.kind==coop_special::Kind::Visual||context.event.spec.kind==coop_special::Kind::Performance){
        Choice::Live choice;
        if(!Proof::pending.unique.valid||!Reward::validateChoice(Proof::pending.unique,choice)||
            !Choice::Same(choice.stable,Proof::pending.unique.stable)||choice.stable.car.id!=scope.carId||
            choice.stable.id!=scope.choiceId||choice.stable.slot!=scope.choiceSlot||!Reward::MissionIs(context.event,4))return false;
    }else{
        Ui::Live live;const auto screen=context.cover.spec.kind==coop_special::Kind::Frontend?Ui::Screen::Magazine:Ui::Screen::Photo;
        if(Proof::pending.unique.valid||Proof::pending.cover!=context.cover.spec.id||!BoundCover(context)||
            !Ui::readLive(screen,0,live,false)||
            (context.event.spec.kind==coop_special::Kind::Show&&!Reward::MissionIs(context.event,3)))return false;
    }
    if(!state.RestoreAuthorized(scope.session,scope.ticket,uint8_t(scope.localPlayer),scope.hostNonce,scope.clientNonce,
        scope.operation,{CampaignHash(scope.campaign),scope.catalog,uint8_t(scope.stage)},context.event.spec)||!lifecycle.ResumeAuthorized(state))return false;
    attempt={};attempt.valid=true;attempt.scope=scope;attempt.uiSeen=true;attempt.step=Step::Commit;return true;
}
// true means OWN Paid is durable; Bootstrap may now Finish(oldFence,Paid).
// Busy remains true until the real fresh native exit has completed. Keep the
// old offline State and continue Tick while Busy; do not start normal packets.
static bool Tick(DWORD flow){
    if(!attempt.valid){if(!Names()||storage->tx.phase!=Phase::Authorized)return true;
        if(flow!=6||!Initialize())return false;}
    if(!Same()||attempt.fault)return false;
    if(attempt.inCall)return storage->tx.phase==Phase::Paid;
    if(attempt.step==Step::Complete)return storage->tx.phase==Phase::Paid;
    if((storage->tx.phase==Phase::Authorized||storage->tx.phase==Phase::Sealed)&&!fenceHeld(attempt.scope))return false;
    Context context;if(!facts(context))return false;
    if(attempt.step==Step::Fresh){
        if(!freshWorld())return false;
        if(coop_special::Timed(context.event.spec.kind)){
            attempt.startCalled=true;attempt.inCall=true;const bool called=start(context);attempt.inCall=false;
            if(!called||!Reward::MissionIs(context.event,1)){attempt.fault=true;return false;}
        }
        attempt.step=Step::Arrival;
    }
    if(attempt.step==Step::Arrival){
        if(!resolve(context))return false;
        attempt.arrivalCalled=true;attempt.inCall=true;const bool called=arrival(context);attempt.inCall=false;
        if(!called||!afterArrival(context)){attempt.fault=true;return false;}
        attempt.step=Step::Dialog;
    }
    if(attempt.step==Step::Dialog){
        Dialog dialog;
        if(!readDialog(context,dialog)){
            if(!attempt.openCalled&&!queuePending()&&canOpen()){
                attempt.openCalled=true;attempt.inCall=true;const bool called=open();attempt.inCall=false;
                if(!called)attempt.fault=true;
            }
            return false;
        }
        attempt.step=Step::Accept;
    }
    if(attempt.step==Step::Accept){
        Dialog dialog;if(!readDialog(context,dialog))return false;
        attempt.acceptCalled=true;attempt.inCall=true;const bool called=accept(dialog);attempt.inCall=false;
        if(!called||!afterAccept(context)){attempt.fault=true;return false;}
        attempt.step=dialog.shop?Step::Selection:Step::Proof;
    }
    if(attempt.step==Step::Selection){
        Selection selection;if(!findSelection(context,selection))return false;
        attempt.selectCalled=true;attempt.inCall=true;const bool called=select(selection);attempt.inCall=false;
        if(!called||!capture(context,selection)){attempt.fault=true;return false;}attempt.uiSeen=true;attempt.step=Step::Commit;
    }
    if(attempt.step==Step::Proof){if(!bindCover(context))return false;attempt.uiSeen=true;attempt.step=Step::Commit;}
    if(attempt.step==Step::Commit){
        // Bootstrap can finish a previously Sealed own image while this RAM
        // grant remains latched (e.g. after explicit receipt Reload). Paid
        // never needs a grant or a still-held fence again.
        if(storage->tx.phase==Phase::Paid){state.MarkSaved(runtimeNow());attempt.step=Step::Exit;}
        else {
        const coop_special::NativeRequest request{coop_special::NativeAction::Commit,state.Session(),state.Ticket(),state.Nonce(),
            state.Operation(),state.RequestId(),1,state.Current()};
        attempt.inCall=true;const Result result=Reward::Commit(request);attempt.inCall=false;
        if(result==Result::Refused){attempt.fault=true;return false;}
        if(result!=Result::Applied)return false;
        if(storage->tx.phase!=Phase::Paid){attempt.fault=true;return false;}
        state.MarkSaved(runtimeNow());attempt.step=Step::Exit;
        }
    }
    if(attempt.step==Step::Exit&&exit(context))attempt.step=Step::Complete;
    return storage->tx.phase==Phase::Paid&&!attempt.fault;
}
static bool Outer(void*,uint32_t message,uintptr_t,uintptr_t,uintptr_t){return Offline()&&(message==Engage::kClick||message==0x911AB364);}
static bool Start(void* self,uintptr_t){return Offline()&&uintptr_t(self)==CoopSpecialNative::addresses.career+0xC00;}
static bool Timer(void* self){return Offline()&&uintptr_t(self)==CoopSpecialNative::addresses.career+0xC00;}
static bool Arrival(void* self,uintptr_t){return Offline()&&uintptr_t(self)==CoopSpecialNative::addresses.career;}
static bool Cover(void* self,uintptr_t){return Offline()&&uintptr_t(self)==CoopSpecialNative::addresses.career+0x4210;}
static bool Fail(void* self){return Offline()&&uintptr_t(self)==CoopSpecialNative::addresses.career+0xC00;}
static bool Failure(void* self,uint32_t message,uintptr_t a,uintptr_t b,uintptr_t c){
    if(Offline()&&(message==Engage::kClick||message==0x911AB364))return true;
    return Abort::Failure(self,message,a,b,c);
}
static bool Unique(void* self,uint32_t message,uintptr_t a,uintptr_t b,uintptr_t c){
    if(!Offline())return Abort::Unique(self,message,a,b,c);
    uintptr_t handler=0;uint8_t completed=0;
    if(!ownedSelector(uintptr_t(self),handler,true)||handler!=uintptr_t(self)||!Read(handler+0x150,completed))return true;
    if(message==0x35F8620B)return false; // Actual stock initialization/preview, no grant.
    if(message!=0xC98356BA)return true; // Old immutable choice is restored automatically, never ask/confirm a different reward.
    if(!completed)return false;
    if(storage->tx.phase==Phase::Paid&&exitReady(attempt.scope)&&!attempt.exitFrame){attempt.exitFrame=true;return false;}
    return true;
}
static bool Photo(void* self,uint32_t message,uintptr_t a,uintptr_t b,uintptr_t c){
    if(Offline()){
        if(message==0x406415E3||message==0x911AB364||message==0x11223344||message==0x11223345)return true;
        uint32_t remaining=0;if(message==0xC98356BA&&Read(uintptr_t(self)+0x4C,remaining)&&remaining==1&&
            (storage->tx.phase!=Phase::Paid||!exitReady(attempt.scope)))return true;
    }
    return Ui::Photo(self,message,a,b,c);
}
static bool PhotoExit(void* self){
    if(Offline()){if(storage->tx.phase!=Phase::Paid||!exitReady(attempt.scope)||attempt.exitCalled)return true;
        Ui::Live live;if(!Ui::readLive(Ui::Screen::Photo,uintptr_t(self),live,true))return true;attempt.exitCalled=true;return false;}
    return Ui::PhotoExit(self);
}
static bool MagazineExit(void* self){
    if(Offline()){if(storage->tx.phase!=Phase::Paid||!exitReady(attempt.scope)||attempt.exitCalled)return true;
        Ui::Live live;if(!Ui::readLive(Ui::Screen::Magazine,uintptr_t(self),live,true))return true;attempt.exitCalled=true;return false;}
    return Ui::MagazineExit(self);
}
static bool Reset(){
    if(attempt.valid&&(attempt.fault||attempt.inCall||attempt.step!=Step::Complete||!Same()||storage->tx.phase!=Phase::Paid))return false;
    attempt={};return true;
}
}} // namespace CoopSpecialNative::RecoveryNative
