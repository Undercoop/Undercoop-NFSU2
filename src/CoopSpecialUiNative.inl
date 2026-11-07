// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Owned Special UI settlement gates. Native photo proof still runs; only
// irreversible camera/FNG/world exits and solo failure retry are held. No UI
// pointer is persisted, no generic FNG pop and no key injection is used.
namespace CoopSpecialNative { namespace Ui {
enum class Screen {Photo,Magazine,Failure};
static constexpr char kPhoto[]="UI_Showcase_Preview.fng",kPhotoAlt[]="UI_ShowCase_Preview.fng";
static constexpr char kMagazine[]="UI_MagazineReward.fng",kFailure[]="UI_EngageShopDialog.fng";
using FindFn=void*(__cdecl*)(const char*);
static FindFn findHandler=reinterpret_cast<FindFn>(0x51BD10);
static ReceiptStore::Store* storage=&ReceiptStore::store;
static uint32_t (*carId)()=Proof::ReadOwnCarId;
static const char* Name(Screen screen){return screen==Screen::Photo?kPhoto:screen==Screen::Magazine?kMagazine:kFailure;}
static uintptr_t Vtable(Screen screen){return screen==Screen::Photo?0x78F998:screen==Screen::Magazine?0x79D830:0x78F978;}
static uintptr_t Handler(Screen screen){return screen==Screen::Photo?0x4CF170:screen==Screen::Magazine?0x55B260:0x499690;}
struct Live {Screen screen=Screen::Photo;uintptr_t handler=0;};
struct Exit {bool valid=false,quiesceAttempted=false,quiesced=false,fault=false;Screen screen=Screen::Photo;
    coop_special_receipt::Scope scope;uintptr_t handler=0;};
static Exit pending;
static bool Scope(){
    return installed&&OwnCareer()&&storage&&!coop_special_receipt::Empty(storage->tx)&&
        coop_special_receipt::Valid(storage->tx)&&Reward::Operation(storage->tx.scope);
}
static bool Settled(){return Scope()&&storage->Ready()&&coop_special_receipt::Terminal(storage->tx.phase);}
static bool FindCall(const char* name,uintptr_t& found){
    __try {found=uintptr_t(findHandler(name));return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool ReadLive(Screen screen,uintptr_t expected,Live& out,bool requireTop=false){
    uintptr_t found=0,vt=0,fn=0,name=0;
    if(!Scope()||!FindCall(Name(screen),found)||!IsValidUserPtr(found)||(expected&&found!=expected)||
        !Read(found,vt)||vt!=Vtable(screen)||!Read(vt+8,fn)||fn!=Handler(screen)||!Read(found+4,name)||
        !(Choice::String(name,Name(screen))||(screen==Screen::Photo&&Choice::String(name,kPhotoAlt)))||
        carId()!=storage->tx.scope.carId)return false;
    const auto* event=ActiveRecord();uint32_t bound=0;
    if(!event)return false;
    if(screen!=Screen::Failure){
        const uint32_t cover=event->spec.kind==coop_special::Kind::Show?event->spec.destination:event->spec.id;
        if(!Read(CoopSpecialNative::addresses.career+0x4218,bound)||bound!=cover)return false;
    }
    if(!::CoopSpecialNative::Feng::Owned(Name(screen),found,requireTop,Choice::addresses.uiManager))return false;
    out={screen,found};return true;
}
static bool (*readLive)(Screen,uintptr_t,Live&,bool)=ReadLive;
static bool Remember(Screen screen,uintptr_t handler){
    if(pending.valid){return pending.screen==screen&&pending.handler==handler&&
        coop_special_receipt::Same(pending.scope,storage->tx.scope);}
    pending.valid=true;pending.screen=screen;pending.handler=handler;pending.scope=storage->tx.scope;return true;
}
static bool Kind(Screen screen){
    if(!Scope())return false;
    const auto kind=coop_special::Kind(storage->tx.scope.kind);
    if(screen==Screen::Failure)return coop_special::Timed(kind);
    if(kind==coop_special::Kind::Show){
        // Stock has NINE photo Show destinations and ELEVEN frontend magazine
        // destinations. Both are timed drives, but their exit owners differ.
        const auto* event=ActiveRecord();const auto* cover=event?Reward::findRecord(event->spec.destination):nullptr;
        if(!cover||coop_special::Timed(cover->spec.kind))return true; // Unknown owner stays fail-closed.
        return screen==Screen::Magazine?cover->spec.kind==coop_special::Kind::Frontend:cover->spec.kind==coop_special::Kind::Photo;
    }
    return screen==Screen::Magazine?kind==coop_special::Kind::Frontend:kind==coop_special::Kind::Photo;
}
static bool ExitGate(Screen screen,void* self){
    if(!Kind(screen))return false;
    // While an owned operation is unsettled even an unprovable/recycled UI
    // must not fall through to a full world load. Never dispatch it ourselves.
    Live live;
    if(!readLive(screen,uintptr_t(self),live,false))return true;
    if(Settled())return false;
    Remember(screen,live.handler);return true;
}
static bool PhotoExit(void* self){return ExitGate(Screen::Photo,self);}
static bool MagazineExit(void* self){return ExitGate(Screen::Magazine,self);}
static bool Photo(void* self,uint32_t message,uintptr_t,uintptr_t,uintptr_t){
    if(!Kind(Screen::Photo))return false;
    // Never block406415E3: its genuine533680 proof4CEEB7 must reach Proof.
    // E1FDE1D1 may start the native5-frame closing countdown. Freeze only the
    // final C983 frame, BEFORE5379A0 can destroy self and queue RF transition.
    if(message!=0xC98356BA)return false;
    uint32_t remaining=0;
    if(!Read(uintptr_t(self)+0x4C,remaining)||remaining!=1)return false;
    Live live;if(!readLive(Screen::Photo,uintptr_t(self),live,false))return true;
    return !Settled();
}
static bool Failure(void* self,uint32_t message,uintptr_t widget,uintptr_t,uintptr_t){
    if(!Kind(Screen::Failure))return false;
    uint8_t failure=0;uintptr_t mission=0;uint32_t phase=0;const auto* event=ActiveRecord();
    if(!Read(CoopSpecialNative::addresses.career+0xC00,failure)||!failure)return false; // Ordinary shops are unchanged.
    uint32_t button=0;
    const bool click=message==Engage::kClick;
    if(click&&(!IsValidUserPtr(widget)||!Read(widget+0x10,button)))return true;
    const bool retry=click&&button==Engage::kTimedButton;
    const bool cancel=message==0x911AB364||(click&&button==Engage::kCancelButton);
    if(!retry&&!cancel)return false;
    // Clearing its mission before dispatch makes stock fall through to the
    // ordinary-shop4031C0 branch. A stale failure UI is held, NEVER replayed.
    if(!event||!Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission!=event->pointer||
        !Read(CoopSpecialNative::addresses.career+0xD1C,phase)||(phase!=6&&phase!=7))return true;
    Live live;if(!readLive(Screen::Failure,uintptr_t(self),live,false))return true;
    if(retry)return true; // Solo native5150D0 teleports/restarts, even after cancel.
    if(Settled()&&storage->tx.phase==coop_special_receipt::Phase::Cancelled)return false;
    if(!(state.Phase()&coop_special::Commit))state.Fail(1,runtimeNow());
    Remember(Screen::Failure,live.handler);return true;
}
static bool PhotoQuiesceCall(uintptr_t handler){
    __try {reinterpret_cast<Detours::ZeroFn>(Detours::Original(Detours::PhotoExit))(
        reinterpret_cast<void*>(handler));return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
// Abort driver may restore the exact own camera before capturing cancellation.
// This native helper starts an animation but cannot pass the final-frame gate.
// It does NOT mean the FNG/world is destroyed, and does NOT award any reward.
static coop_special::NativeResult QuiescePhoto(){
    using Result=coop_special::NativeResult;
    if(!Kind(Screen::Photo)||!state.Terminal()||(state.Phase()&coop_special::Commit)||
        storage->tx.phase!=coop_special_receipt::Phase::Prepared)return Result::Refused;
    Live live;if(!readLive(Screen::Photo,0,live,true))return Result::Wait;
    if(!Remember(Screen::Photo,live.handler)||pending.fault)return Result::Refused;
    if(pending.quiesced)return Result::Applied;
    if(pending.quiesceAttempted)return Result::Refused;
    uintptr_t dynamic=0;
    if(!Read(live.handler+0x50,dynamic)||dynamic!=0x4CEDC0)return Result::Wait;
    pending.quiesceAttempted=true;
    if(!PhotoQuiesceCall(live.handler)||!Read(live.handler+0x50,dynamic)||dynamic){pending.fault=true;return Result::Refused;}
    pending.quiesced=true;return Result::Applied;
}
// Replay a retained *actual* exit only after an exact own terminal receipt.
// Fresh lookups precede every call; never read self after a native exit/load.
static bool DispatchCall(Screen screen,uintptr_t handler){
    __try {
        if(screen==Screen::Photo)reinterpret_cast<Detours::ZeroFn>(Detours::Original(Detours::PhotoExit))(reinterpret_cast<void*>(handler));
        else if(screen==Screen::Magazine)reinterpret_cast<Detours::ZeroFn>(Detours::Original(Detours::MagazineExit))(reinterpret_cast<void*>(handler));
        else reinterpret_cast<Detours::HandlerFn>(Detours::Original(Detours::FailureUi))(
            reinterpret_cast<void*>(handler),0x911AB364,0,0,0);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool DispatchSettledExit(){
    if(!pending.valid||!Settled()||!coop_special_receipt::Same(pending.scope,storage->tx.scope)||pending.fault)return false;
    Live live;if(!readLive(pending.screen,pending.handler,live,true))return false;
    if(pending.screen==Screen::Failure){
        const auto* event=ActiveRecord();uintptr_t mission=0;uint8_t failure=0;uint32_t phase=0;
        if(storage->tx.phase!=coop_special_receipt::Phase::Cancelled||!event||
            !Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission!=event->pointer||
            !Read(CoopSpecialNative::addresses.career+0xC00,failure)||!failure||
            !Read(CoopSpecialNative::addresses.career+0xD1C,phase)||phase!=7)return false;
    }
    // Original final frame (not a synthesized C983 message) owns FNG removal.
    if(pending.screen==Screen::Photo&&pending.quiesced){pending={};return true;}
    const Screen screen=pending.screen;const uintptr_t handler=live.handler;
    pending={}; // Clear BEFORE native reentry/free. Receipt itself remains durable.
    return DispatchCall(screen,handler);
}
static bool Reset(){if(pending.valid&&(!Settled()||pending.fault))return false;pending={};return true;}
}} // namespace CoopSpecialNative::Ui
