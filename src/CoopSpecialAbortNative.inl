// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Own CURRENT-operation cancellation. The durable local receipt is authority
// for identity; only State's authenticated Failed/Declined decision authorizes
// abort. Native side effects are latched before their call and never retried
// after an uncertain exception. No world teleport, generic FNG pop, reward
// completion, foreign save image or replay-mask bypass is used here.
namespace CoopSpecialNative { namespace Abort {
using Scope=coop_special_receipt::Scope;
using Phase=coop_special_receipt::Phase;
using Bytes=ReceiptStore::Bytes;
using Result=coop_special::NativeResult;
enum class Surface {None,Engage,Travelling,DestinationShop,Unique,Photo,Magazine,Failure,Unknown,DestinationCover};
struct Live {Surface surface=Surface::Unknown;uintptr_t handler=0,mission=0;uint32_t phase=0;};
struct Attempt {
    bool valid=false,entered=false,quiesced=false,fault=false,dispatching=false,uniqueDoneConsumed=false,exitFrame=false;
    Scope scope;Surface surface=Surface::Unknown;uintptr_t handler=0,mission=0;
};
static Attempt attempt;
static ReceiptStore::Store* storage=&ReceiptStore::store;
static const Record* (*activeRecord)()=ActiveRecord;
static uint32_t (*carId)()=Proof::ReadOwnCarId;
static bool BeginDispatched(){return lifecycle.Began();}
// Driver replaces this with its pre-Accept latch. A failed/partially executed
// Begin must never be mistaken for "nothing started" just because no UI has
// completed its queued push yet.
static bool (*beginDispatched)()=BeginDispatched;
static bool (*fenceHeld)(const Scope&)=Reward::FenceHeld;
static bool (*serialize)(Bytes&)=Reward::Serialize;
static bool StateAuthorization(const Scope& scope,coop_special_receipt::AuthenticatedAbortDecision& decision){
    if(!state.Terminal()||(state.Phase()&coop_special::Commit)||!Reward::Operation(scope))return false;
    decision.old=scope;decision.current={state.Session(),state.Ticket(),state.Player()==1?state.Nonce():state.PeerNonce(),
        state.Player()==2?state.Nonce():state.PeerNonce()};return coop_special_receipt::Valid(decision.current);
}
static bool (*authorization)(const Scope&,coop_special_receipt::AuthenticatedAbortDecision&)=StateAuthorization;
static bool (*ownedUnique)(uintptr_t,uintptr_t&,bool)=Choice::OwnedScreen;
static bool (*readEngage)(const coop_special::Spec&,Engage::Live&)=Engage::ReadLive;
static bool (*cancelEngage)(const coop_special::Spec&)=Engage::CancelUnstarted;
static bool (*idleHud)()=Destination::IdleHud;
static bool UniqueCar(){Choice::OwnCar own;return Choice::ReadOwnCar(own)&&storage&&own.id==storage->tx.scope.carId;}
static bool (*uniqueCar)()=UniqueCar;
static bool (*recordCurrent)(const Record&)=[](const Record& row){return coop_special::Timed(row.spec.kind)?
    Destination::NativeRow(row):Reward::NativeCover(row);};
static bool DestinationShop(const Record& row){uintptr_t area=0,race=0,shop=0,cover=0;uint32_t hash=0,shopArea=0,areaHash=0;
    const auto career=CoopSpecialNative::addresses.career;
    return Read(career+0x8704,area)&&IsValidUserPtr(area)&&Read(career+0x8708,race)&&!race&&
        Read(career+0x870C,shop)&&IsValidUserPtr(shop)&&Read(career+0x8710,cover)&&!cover&&
        Read(shop+0x38,hash)&&hash==row.spec.destination&&Read(shop+0x3C,shopArea)&&
        Read(area+0x30,areaHash)&&areaHash&&areaHash==shopArea;
}
static bool (*destinationShop)(const Record&)=DestinationShop;
static const Record* DestinationCover(const Record& row){
    if(row.spec.kind!=coop_special::Kind::Show)return nullptr;
    const auto* cover=Reward::findRecord(row.spec.destination);
    return cover&&!coop_special::Timed(cover->spec.kind)&&cover->spec.stage==row.spec.stage?cover:nullptr;
}
static bool PendingPrompt(const Record& row){
    const auto* cover=attempt.surface==Surface::DestinationCover?DestinationCover(row):nullptr;
    return Engage::CancelPending(cover?cover->spec:row.spec);
}
static bool PollPrompt(const Record& row){
    const auto* cover=attempt.surface==Surface::DestinationCover?DestinationCover(row):nullptr;
    return cancelEngage(cover?cover->spec:row.spec);
}
static bool Operation(){return installed&&OwnCareer()&&storage&&storage->Ready()&&
    !coop_special_receipt::Empty(storage->tx)&&coop_special_receipt::Valid(storage->tx)&&Reward::Operation(storage->tx.scope);}
static bool Decision(){return Operation()&&state.Terminal()&&!(state.Phase()&coop_special::Commit)&&
    (state.Phase()&(coop_special::Failed|coop_special::Declined))&&state.Nonce()&&state.PeerNonce();}
static bool SameAttempt(){return attempt.valid&&storage&&coop_special_receipt::Same(attempt.scope,storage->tx.scope);}
static bool Request(const coop_special::NativeRequest& request,const Record& row){
    return request.action==coop_special::NativeAction::Abort&&request.serial&&request.session==state.Session()&&
        request.ticket==state.Ticket()&&request.nonce==state.Nonce()&&request.operation==state.Operation()&&
        request.request==state.RequestId()&&coop_special::Same(request.spec,state.Current())&&
        coop_special::Same(row.spec,state.Current())&&row.pointer&&recordCurrent&&recordCurrent(row);
}
static bool NativeMission(const Record& row,uint32_t expected,bool empty=false){uintptr_t mission=0;uint32_t phase=0;
    return Read(CoopSpecialNative::addresses.career+0xC08,mission)&&(empty?!mission:mission==row.pointer)&&
        Read(CoopSpecialNative::addresses.career+0xD1C,phase)&&phase==expected;
}
static bool PhotoMode(uintptr_t mode){return mode==0x4CEDC0||mode==0x4CEA70||mode==0x4B5A70||mode==0x4B5AD0;}
static bool PhotoReady(uintptr_t handler){uintptr_t mode=0;
    return Read(handler+0x50,mode)&&(PhotoMode(mode)||(Ui::pending.valid&&Ui::pending.quiesced&&
        !Ui::pending.fault&&Ui::pending.handler==handler&&storage&&coop_special_receipt::Same(Ui::pending.scope,storage->tx.scope)));
}
static bool ReadSurface(const Record& row,Live& out){
    Live next;uintptr_t mission=0;uint32_t phase=0;uint8_t failure=0;
    if(!Read(CoopSpecialNative::addresses.career+0xC08,mission)||
        !Read(CoopSpecialNative::addresses.career+0xD1C,phase)||
        !Read(CoopSpecialNative::addresses.career+0xC00,failure))return false;
    next.mission=mission;next.phase=phase;
    // Always identify a real owned arrived UI before the no-Begin branch.
    // It may have been created synchronously by an original Begin that then
    // failed a postcondition; lifecycle.Began alone cannot distinguish that.
    Ui::Live ui;uintptr_t selector=0;
    if(coop_special::Timed(row.spec.kind)){
        if(mission&&mission!=row.pointer)return false;
        if(mission==row.pointer&&(phase==6||phase==7)&&failure&&Ui::readLive(Ui::Screen::Failure,0,ui,true)){
            next.surface=Surface::Failure;next.handler=ui.handler;
        }else if(mission==row.pointer&&phase==1&&idleHud())next.surface=Surface::Travelling;
        else if(mission==row.pointer&&phase==2&&!failure&&
            (row.spec.kind==coop_special::Kind::Visual||row.spec.kind==coop_special::Kind::Performance)&&destinationShop(row)&&
            Ui::readLive(Ui::Screen::Failure,0,ui,true)){
            next.surface=Surface::DestinationShop;next.handler=ui.handler;
        }else if(mission==row.pointer&&phase==4&&(row.spec.kind==coop_special::Kind::Visual||row.spec.kind==coop_special::Kind::Performance)&&
            ownedUnique(0,selector,false)&&uniqueCar()){
            next.surface=Surface::Unique;next.handler=selector;
        }else if(mission==row.pointer&&phase==3&&row.spec.kind==coop_special::Kind::Show&&Ui::readLive(Ui::Screen::Photo,0,ui,true)&&PhotoReady(ui.handler)){
            next.surface=Surface::Photo;next.handler=ui.handler;
        }else if(mission==row.pointer&&phase==3&&row.spec.kind==coop_special::Kind::Show&&Ui::readLive(Ui::Screen::Magazine,0,ui,true)){
            next.surface=Surface::Magazine;next.handler=ui.handler;
        }else if(mission==row.pointer&&phase==3&&!failure&&row.spec.kind==coop_special::Kind::Show){
            const auto* cover=DestinationCover(row);Engage::Live prompt;
            if(cover&&readEngage(cover->spec,prompt)){next.surface=Surface::DestinationCover;next.handler=prompt.handler;}
        }
    }else if(!mission){
        const auto screen=row.spec.kind==coop_special::Kind::Frontend?Ui::Screen::Magazine:Ui::Screen::Photo;
        if(Ui::readLive(screen,0,ui,true)&&(screen!=Ui::Screen::Photo||PhotoReady(ui.handler))){
            next.surface=screen==Ui::Screen::Magazine?Surface::Magazine:Surface::Photo;next.handler=ui.handler;}
    }
    if(next.surface==Surface::Unknown&&!mission){
        Engage::Live prompt;
        if(readEngage(row.spec,prompt)){next.surface=Surface::Engage;next.handler=prompt.handler;}
        else if(beginDispatched&&!beginDispatched())next.surface=Surface::None;
    }
    out=next;return next.surface!=Surface::Unknown;
}
static bool (*readSurface)(const Record&,Live&)=ReadSurface;
static bool NativeAck(const Record& row,bool clear){
    __try {
        // Only this exact mission and a known active/acknowledged phase may
        // enter native501640; it dereferences its mission pointer internally.
        uintptr_t mission=0;uint32_t phase=0;
        if(!Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission!=row.pointer||
            !Read(CoopSpecialNative::addresses.career+0xD1C,phase)||(phase!=1&&phase!=2&&phase!=3&&phase!=4&&phase!=6&&phase!=7))return false;
        if(phase!=7)Destination::acknowledgeFailure(reinterpret_cast<void*>(CoopSpecialNative::addresses.career+0xC00));
        if(!NativeMission(row,7))return false;
        if(clear){Destination::clearFailedMission(reinterpret_cast<void*>(CoopSpecialNative::addresses.career+0xC00));
            return NativeMission(row,7,true);}
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool (*acknowledge)(const Record&,bool)=NativeAck;
static bool NativeForfeit(uintptr_t handler){
    __try {reinterpret_cast<Detours::HandlerFn>(Detours::Original(Detours::Unique))(
        reinterpret_cast<void*>(handler),Choice::kForfeit,0,0,0);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool (*forfeit)(uintptr_t)=NativeForfeit;
// ALWAYS consume forfeit5419C0. It would mark a mission completed without a
// part. Only record the exact one-shot native return while our fresh owned
// abort is actively on the stack; no public token can create this proof.
static bool HoldUniqueDone(void* self,uintptr_t nativeReturn){
    if(!installed||!OwnCareer())return false;
    if(SameAttempt()&&Decision()&&attempt.dispatching&&!attempt.fault&&!attempt.uniqueDoneConsumed&&
        storage->tx.phase==Phase::Prepared&&attempt.surface==Surface::Unique&&nativeReturn==0x561793&&
        uintptr_t(self)==CoopSpecialNative::addresses.career+0xC00){
        uintptr_t handler=0,mission=0;uint8_t complete=0;
        if(ownedUnique(attempt.handler,handler,true)&&handler==attempt.handler&&
            Read(handler+0x150,complete)&&complete==1&&Read(CoopSpecialNative::addresses.career+0xC08,mission)&&mission==attempt.mission)
            attempt.uniqueDoneConsumed=true;
    }
    return true;
}
static bool PhotoQuiescence(uintptr_t handler){
    Ui::Live fresh;uintptr_t mode=0;
    if(!Ui::readLive(Ui::Screen::Photo,handler,fresh,true)||fresh.handler!=handler||
        !Read(handler+0x50,mode)||!Ui::Remember(Ui::Screen::Photo,handler)||Ui::pending.fault)return false;
    if(Ui::pending.quiesced)return true;
    if(!PhotoMode(mode))return false;
    if(Ui::pending.quiesceAttempted)return false;
    // Read-only PE audit:499FA0 restores all E9/EA/EB preview adjust flags,
    // finishes screenshot98,501290(0)/5012E0(0),then resets50 and starts the
    // closing animation. It assumes none of the four proven dynamic modes.
    // Final C983/FNG/RF remains protected by Ui until own Cancelled.
    Ui::pending.quiesceAttempted=true;
    if(!Ui::PhotoQuiesceCall(handler)||!Read(handler+0x50,mode)||mode){Ui::pending.fault=true;return false;}
    Ui::pending.quiesced=true;return true;
}
static bool (*quiescePhoto)(uintptr_t)=PhotoQuiescence;
static bool NativeQuiesce(const Record& row,const Live& live){
    if(live.surface==Surface::None)return !beginDispatched()&&!live.mission;
    if(live.surface==Surface::Engage)return !live.mission&&cancelEngage(row.spec);
    if(live.surface==Surface::DestinationCover){
        const auto* cover=DestinationCover(row);Engage::Live prompt;
        // Stop/clear the exact timed mission BEFORE closing its destination
        // Continue. Never take the hidden cover Cancel or any completion path.
        return cover&&readEngage(cover->spec,prompt)&&prompt.handler==live.handler&&
            acknowledge(row,true)&&cancelEngage(cover->spec);
    }
    if(live.surface==Surface::Travelling)return acknowledge(row,true);
    if(live.surface==Surface::DestinationShop)return acknowledge(row,true);
    if(live.surface==Surface::Unique){
        uintptr_t current=0;uint8_t complete=0;
        if(!ownedUnique(live.handler,current,false)||current!=live.handler||!uniqueCar()||
            !Read(current+0x150,complete)||complete)return false;
        attempt.dispatching=true;const bool called=forfeit(current);attempt.dispatching=false;
        if(!called||!attempt.uniqueDoneConsumed||!Read(current+0x150,complete)||complete!=1)return false;
        return acknowledge(row,true);
    }
    if(live.surface==Surface::Photo){
        if(!quiescePhoto(live.handler))return false;
        return !coop_special::Timed(row.spec.kind)||acknowledge(row,true);
    }
    if(live.surface==Surface::Failure){
        // Native failure UI is its own cleanup owner. Keep current mission
        // and failure flag through501640 and durable Cancelled. Its later
        // originalCancel + destructor533CF0 must still see that exact mission,
        // otherwise stock499690 falls into ordinary-shop4031C0.
        return Ui::Remember(Ui::Screen::Failure,live.handler)&&acknowledge(row,false);
    }
    if(live.surface==Surface::Magazine){
        // Frontend has no native timed controller/photo camera. Its proof and
        // every reward body remain held.54B090 (the only full world exit) is
        // held by Ui. Merely keep its fresh own exit until cancellation flush.
        return Ui::Remember(Ui::Screen::Magazine,live.handler)&&
            (coop_special::Timed(row.spec.kind)?acknowledge(row,true):!live.mission);
    }
    return false;
}
static bool (*quiesce)(const Record&,const Live&)=NativeQuiesce;
static bool CancellationImage(const Scope& own,const Bytes& before,const Bytes& after){
    if(!coop_special_receipt::Native(before,own.personal)||!coop_special_receipt::Native(after,own.personal))return false;
    const std::vector<uint8_t> a(before.begin()+0x24,before.end()),b(after.begin()+0x24,after.end());
    if(coop_career::Completed(b,own.event)&&!coop_career::Completed(a,own.event))return false;
    const auto* row=activeRecord();
    return own.kind!=uint8_t(coop_special::Kind::Show)||(row&&row->spec.id==own.event&&
        (!coop_career::Completed(b,row->spec.destination)||coop_career::Completed(a,row->spec.destination)));
}
static bool (*cancellationImage)(const Scope&,const Bytes&,const Bytes&)=CancellationImage;
static bool ScreenGone(Surface surface){
    uintptr_t found=~uintptr_t(0);
    if(surface==Surface::Unique){
        __try {found=uintptr_t(Choice::findHandler(Choice::kScreen));return !found;}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    const auto screen=surface==Surface::Photo?Ui::Screen::Photo:surface==Surface::Magazine?Ui::Screen::Magazine:Ui::Screen::Failure;
    if(surface!=Surface::Photo&&surface!=Surface::Magazine&&surface!=Surface::Failure)return false;
    if(!Ui::FindCall(Ui::Name(screen),found)||found)return false;
    // Both verified stock spellings identify this screen. An absent primary
    // lookup alone must not retire a differently cased live photo handler.
    return surface!=Surface::Photo||(Ui::FindCall(Ui::kPhotoAlt,found)&&!found);
}
static bool (*screenGone)(Surface)=ScreenGone;
// Native controller quiescence is deliberately different from UI destruction.
// A failure dialog retains its phase7 pointer until native post-Cancelled
// destructor; photo/unique closing frames remain held before any world load.
static bool OwnQuiescent(){return SameAttempt()&&attempt.quiesced&&!attempt.fault&&!attempt.dispatching;}
static bool ExitPending(){
    if(!OwnQuiescent())return false;
    if(attempt.surface==Surface::Unique||attempt.surface==Surface::DestinationShop)return !attempt.exitFrame;
    return Ui::pending.valid&&coop_special_receipt::Same(Ui::pending.scope,attempt.scope);
}
static Result Execute(const coop_special::NativeRequest& request,const Record& row){
    if(!Decision()||!Request(request,row)||!fenceHeld(storage->tx.scope))return Result::Refused;
    if(storage->tx.phase==Phase::Paid||storage->tx.phase==Phase::Authorized||storage->tx.phase==Phase::Sealed)return Result::Refused;
    if(storage->tx.phase==Phase::CancelPending||storage->tx.phase==Phase::Cancelled)
        return storage->RecoverCancellation()?Result::Applied:Result::Wait;
    if(storage->tx.phase!=Phase::Prepared||carId()!=storage->tx.scope.carId)return Result::Refused;
    if(attempt.valid&&!SameAttempt())return Result::Refused;
    if(attempt.fault||attempt.dispatching)return Result::Refused;
    if(!attempt.quiesced&&attempt.entered&&
        (attempt.surface==Surface::Engage||attempt.surface==Surface::DestinationCover)&&PendingPrompt(row)){
        if(!PollPrompt(row)){
            if(Engage::CancelFault()){attempt.fault=true;return Result::Refused;}
            return Result::Wait;
        }
        attempt.quiesced=true;
    }
    if(!attempt.quiesced){
        if(attempt.entered)return Result::Refused;
        Live live;if(!readSurface(row,live))return Result::Wait;
        attempt={};attempt.valid=true;attempt.entered=true;attempt.scope=storage->tx.scope;
        attempt.surface=live.surface;attempt.handler=live.handler;attempt.mission=live.mission;
        if(!quiesce(row,live)){
            // A named cover-prompt removal and RF resume are asynchronous.
            // Retain this exact receipt/surface and poll; do not call the
            // hidden Cancel or treat a queued native exit as an exception.
            if((live.surface==Surface::Engage||live.surface==Surface::DestinationCover)&&PendingPrompt(row)&&!Engage::CancelFault())return Result::Wait;
            attempt.fault=true;return Result::Refused;
        }
        attempt.quiesced=true;
    }
    Bytes after;if(!serialize(after)||!cancellationImage(attempt.scope,storage->tx.before,after))return Result::Wait;
    if(!Decision()||!SameAttempt()||storage->tx.phase!=Phase::Prepared||carId()!=attempt.scope.carId||
        !fenceHeld(attempt.scope))return Result::Refused;
    // This evidence is assembled ONLY from the authenticated local State, not
    // accepted from a network payload/caller. OwnDone's old intent cannot be
    // cancelled by a unilateral Client failure request or a lost peer.
    coop_special_receipt::AuthenticatedAbortDecision decision;
    if(!authorization(storage->tx.scope,decision)||!coop_special_receipt::Same(decision.old,storage->tx.scope))return Result::Refused;
    return storage->CancelAndInstall(storage->tx.scope,after,true,&decision)?Result::Applied:Result::Wait;
}
static bool Unique(void* self,uint32_t message,uintptr_t a,uintptr_t b,uintptr_t c){
    if(!installed||!OwnCareer())return false;
    if(SameAttempt()&&attempt.surface==Surface::Unique){
        // Only the native event loop's actual frame may do54FD40/5404A0. A
        // synthetic frame is never sent. Clear-before-return prevents nested
        // native frame reentry from causing a second free-roam transition.
        if(message==0xC98356BA&&Operation()&&storage->tx.phase==Phase::Cancelled&&OwnQuiescent()&&
            !attempt.exitFrame&&uintptr_t(self)==attempt.handler){
            uintptr_t handler=0;uint8_t complete=0;
            if(ownedUnique(attempt.handler,handler,true)&&handler==attempt.handler&&
                Read(handler+0x150,complete)&&complete==1){attempt.exitFrame=true;return false;}
        }
        return true;
    }
    return Proof::Unique(self,message,a,b,c);
}
static bool ShopExitReady(uintptr_t handler){
    if(!Operation()||storage->tx.phase!=Phase::Cancelled||!OwnQuiescent()||attempt.surface!=Surface::DestinationShop||
        attempt.exitFrame||handler!=attempt.handler)return false;
    Ui::Live live;uintptr_t mission=1;uint32_t phase=0;uint8_t failure=1;const auto* row=activeRecord();
    return row&&coop_special::Same(row->spec,state.Current())&&destinationShop(*row)&&
        Ui::readLive(Ui::Screen::Failure,handler,live,true)&&live.handler==handler&&
        Read(CoopSpecialNative::addresses.career+0xC08,mission)&&!mission&&
        Read(CoopSpecialNative::addresses.career+0xD1C,phase)&&phase==7&&
        Read(CoopSpecialNative::addresses.career+0xC00,failure)&&!failure;
}
static bool Failure(void* self,uint32_t message,uintptr_t widget,uintptr_t b,uintptr_t c){
    if(!installed||!OwnCareer())return false;
    uint32_t button=0;const bool click=message==Engage::kClick;
    if(click&&(!IsValidUserPtr(widget)||!Read(widget+0x10,button)))return Operation();
    const bool cancel=message==0x911AB364||(click&&button==Engage::kCancelButton);
    const bool accept=click&&button==Engage::kTimedButton;
    if((cancel||accept)&&SameAttempt()&&attempt.surface==Surface::DestinationShop){
        if(cancel&&ShopExitReady(uintptr_t(self))){attempt.exitFrame=true;return false;}
        return true;
    }
    // The destination's normal EnterShop prompt is phase2/failure0. It is
    // distinct from phase4 unique selection and phase6/7 failed mission UI.
    // Its genuine Cancel is a shared-abort request, never a solo world exit.
    if((cancel||accept)&&Operation()&&storage->tx.phase==Phase::Prepared){
        const auto* row=activeRecord();uintptr_t mission=0;uint32_t phase=0;uint8_t failure=1;Ui::Live live;
        if(row&&(row->spec.kind==coop_special::Kind::Visual||row->spec.kind==coop_special::Kind::Performance)&&
            Read(CoopSpecialNative::addresses.career+0xC08,mission)&&mission==row->pointer&&
            Read(CoopSpecialNative::addresses.career+0xD1C,phase)&&phase==2&&
            Read(CoopSpecialNative::addresses.career+0xC00,failure)&&!failure&&
            destinationShop(*row)&&Ui::readLive(Ui::Screen::Failure,uintptr_t(self),live,true)){
            if(cancel){if(!(state.Phase()&coop_special::Commit))state.Fail(1,runtimeNow());return true;}
            if(state.Terminal()||state.AwaitingFailureDecision()||!(state.Phase()&coop_special::Release))return true;
        }
    }
    return Ui::Failure(self,message,widget,b,c);
}
static bool ShopExitCall(uintptr_t handler){
    __try {reinterpret_cast<Detours::HandlerFn>(Detours::Original(Detours::FailureUi))(
        reinterpret_cast<void*>(handler),0x911AB364,0,0,0);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool TickAfterSettlement(){
    if(!Operation()||storage->tx.phase!=Phase::Cancelled||!OwnQuiescent())return false;
    if(attempt.surface==Surface::Unique)return attempt.exitFrame; // Wait for actual native C983 frame.
    if(attempt.surface==Surface::DestinationShop){
        if(attempt.exitFrame)return true;
        if(!ShopExitReady(attempt.handler))return false;
        // This is the normal destination-shop Escape branch, NOT a failed
        // mission fallback:499813 sets54 then RF1; destructor500B40 clears
        // only the copied encounter pointers8704..8710. Controller
        // was already stopped/cleared and own cancellation is durable.
        attempt.exitFrame=true;const bool called=ShopExitCall(attempt.handler);
        if(!called)attempt.fault=true;
        return called;
    }
    if(attempt.surface==Surface::Photo||attempt.surface==Surface::Magazine||attempt.surface==Surface::Failure){
        // The user's REAL allowed exit/frame can beat our next tick. Retire
        // only after an explicit successful native lookup proves absence;
        // ReadLive(false), a modal, replacement UI or an exception is NOT
        // absence. Failure additionally needs its exact destructor cleanup.
        if(Ui::pending.valid&&coop_special_receipt::Same(Ui::pending.scope,attempt.scope)&&screenGone(attempt.surface)){
            if(coop_special::Timed(state.Current().kind)){uintptr_t mission=1;uint32_t phase=~0u;
                if(!Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission||
                    !Read(CoopSpecialNative::addresses.career+0xD1C,phase)||(phase!=0&&phase!=7))return false;}
            Ui::pending={};return true;
        }
        const bool retained=Ui::pending.valid;const bool called=Ui::DispatchSettledExit();
        // Ui retires its pointer BEFORE native free/load. If its guarded
        // call then failed, preserve an explicit uncertain-exit latch rather
        // than replaying or pretending the old native world was destroyed.
        if(!called&&retained&&!Ui::pending.valid)attempt.fault=true;
        return called;
    }
    return true;
}
static bool Reset(){
    if(attempt.valid&&(!storage||!storage->Ready()||!coop_special_receipt::Same(attempt.scope,storage->tx.scope)||
        storage->tx.phase!=Phase::Cancelled||attempt.fault||attempt.dispatching||ExitPending()))return false;
    attempt={};return true;
}
}} // namespace CoopSpecialNative::Abort
