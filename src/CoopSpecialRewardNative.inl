// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Game-thread, OWN reward replay only. Included after Proof. Still inactive
// until the complete native/recovery driver is installed. Calling an original
// trampoline does not relax any hook: nested UI reentry must remain gated.
namespace CoopSpecialNative { namespace Reward {
using Scope=coop_special_receipt::Scope;
using Bytes=ReceiptStore::Bytes;
using Phase=coop_special_receipt::Phase;
using Result=coop_special::NativeResult;
static ReceiptStore::Store* storage=&ReceiptStore::store;
static const Record* (*activeRecord)()=ActiveRecord;
static const Record* (*findRecord)(uint32_t)=FindRuntimeRecord;
static uint32_t (*carId)()=Proof::ReadOwnCarId;
static bool (*validateChoice)(const Choice::Frozen&,Choice::Live&)=Choice::ValidateSameChoice;
static bool (*ownedScreen)(uintptr_t,uintptr_t&,bool)=Choice::OwnedScreen;
struct Grant {Scope scope;Record event,cover;Choice::Live choice;};
struct Attempt {
    bool valid=false,entered=false,applied=false,fault=false,dispatching=false,uniqueDone=false;
    Scope scope;uintptr_t handler=0,mission=0;
};
static Attempt attempt;
static bool FenceHeld(const Scope& own){
    return CoopSpecialNativeSaveIatReady()&&gSpecialSaveFence.Held({own.session,own.ticket,own.operation});
}
static bool (*fenceHeld)(const Scope&)=FenceHeld;
static bool Serialize(Bytes& file){
    std::vector<uint8_t> payload(coop_career::kNativePayloadSize);
    return NativeSerializeProfile(payload.data())&&coop_save::EncodeNativeProfileFile(payload,file)&&
        coop_special_receipt::Native(file,CoopPersonalSave::personal);
}
static bool (*serialize)(Bytes&)=Serialize;
// The ONLY permitted nested 5419C0 body is the exact current Authorized own
// confirm call. A native forfeit, constructor, repeated call, another manager,
// or reentrant UI confirm never consumes this one-shot capability.
static bool HoldUniqueDone(void* self,uintptr_t nativeReturn){
    if(!installed||!OwnCareer())return false;
    if(!attempt.valid||!attempt.dispatching||attempt.fault||attempt.uniqueDone||
        nativeReturn!=0x561875||uintptr_t(self)!=CoopSpecialNative::addresses.career+0xC00||
        !storage||!storage->Ready()||storage->tx.phase!=Phase::Authorized||
        !coop_special_receipt::Same(attempt.scope,storage->tx.scope))return true;
    uintptr_t mission=0,owner=0;uint8_t complete=1;
    if(!attempt.handler||!ownedScreen(attempt.handler,owner,false)||owner!=attempt.handler||
        !Read(attempt.handler+0x150,complete)||complete||
        !Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission!=attempt.mission)return true;
    attempt.uniqueDone=true;return false;
}
static bool NativeCover(const Record& cover){
    BYTE bytes[0x40]{};uint32_t hash=0,area=0;float stars=0;
    if(coop_special::Timed(cover.spec.kind)||!ReadSpan(cover.pointer,bytes,sizeof(bytes)))return false;
    std::memcpy(&hash,bytes+0x20,4);std::memcpy(&area,bytes+0x2C,4);std::memcpy(&stars,bytes+0x3C,4);
    return hash==cover.spec.id&&area==cover.spec.start&&bytes[0x24]==cover.nativeType&&
        bytes[0x25]==cover.spec.stage&&bytes[0x35]==cover.mandatory&&stars==cover.stars;
}
static bool MissionIs(const Record& row,uint32_t phase){
    uintptr_t mission=0;uint32_t nativePhase=0;
    return Read(CoopSpecialNative::addresses.career+0xC08,mission)&&mission==row.pointer&&
        Read(CoopSpecialNative::addresses.career+0xD1C,nativePhase)&&nativePhase==phase;
}
static bool NativeGrant(const Grant& grant){
    // No C++ unwind objects cross this SEH boundary. No replayMask is set.
    __try {
        const auto career=CoopSpecialNative::addresses.career;
        if(grant.event.spec.kind==coop_special::Kind::Visual||grant.event.spec.kind==coop_special::Kind::Performance){
            reinterpret_cast<Detours::HandlerFn>(Detours::Original(Detours::Unique))(
                reinterpret_cast<void*>(grant.choice.handler),Choice::kConfirm,0,0,0);
        }else{
            // Preserve the native camera constructor's order for a timed Show.
            // Plain frontend uses its actual payout -> show -> cover sequence.
            if(grant.event.spec.kind==coop_special::Kind::Show&&grant.cover.spec.kind==coop_special::Kind::Photo)
                reinterpret_cast<Detours::OneFn>(Detours::Original(Detours::ShowDone))(
                    reinterpret_cast<void*>(career+0xC00),grant.cover.pointer);
            reinterpret_cast<Detours::OneFn>(Detours::Original(Detours::Payout))(
                reinterpret_cast<void*>(career),grant.cover.pointer);
            if(grant.event.spec.kind!=coop_special::Kind::Show||grant.cover.spec.kind==coop_special::Kind::Frontend)
                reinterpret_cast<Detours::OneFn>(Detours::Original(Detours::ShowDone))(
                    reinterpret_cast<void*>(career+0xC00),grant.cover.pointer);
            reinterpret_cast<Detours::ZeroFn>(Detours::Original(Detours::CoverDone))(
                reinterpret_cast<void*>(career+0x4210));
        }
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool (*grantNative)(const Grant&)=NativeGrant;
static bool NativeApplied(const Grant& grant){
    if(grant.event.spec.kind==coop_special::Kind::Visual||grant.event.spec.kind==coop_special::Kind::Performance){
        uint8_t complete=0;uintptr_t mission=1;uint32_t phase=0;
        return attempt.uniqueDone&&Read(grant.choice.handler+0x150,complete)&&complete==1&&
            Read(CoopSpecialNative::addresses.career+0xC08,mission)&&!mission&&
            Read(CoopSpecialNative::addresses.career+0xD1C,phase)&&phase==0; // Stock5419F3..FB clears phase/pointer to0.
    }
    // Durable after-image verification below proves both the exact cover and
    // (for Show) mission completion. Merely gaining some cash is not proof.
    return true;
}
static bool (*nativeApplied)(const Grant&)=NativeApplied;
static bool BuildGrant(const Record& event,const Scope& own,Grant& out){
    if(!Proof::pending.valid||!Proof::SameBase(Proof::pending.identity,own)||
        !coop_special::Same(event.spec,state.Current())||own.carId!=carId())return false;
    Grant next;next.scope=own;next.event=event;
    if(event.spec.kind==coop_special::Kind::Visual||event.spec.kind==coop_special::Kind::Performance){
        if(!Proof::pending.unique.valid||!validateChoice(Proof::pending.unique,next.choice)||
            !Choice::Same(next.choice.stable,Proof::pending.unique.stable)||next.choice.stable.car.id!=own.carId||
            next.choice.stable.id!=own.choiceId||next.choice.stable.slot!=own.choiceSlot||!MissionIs(event,4))return false;
    }else{
        const uint32_t hash=event.spec.kind==coop_special::Kind::Show?event.spec.destination:event.spec.id;
        const auto* cover=findRecord(hash);uint32_t bound=0;
        if(!cover||cover->spec.stage!=event.spec.stage||Proof::pending.cover!=hash||!NativeCover(*cover)||
            !Read(CoopSpecialNative::addresses.career+0x4218,bound)||bound!=hash||
            (event.spec.kind==coop_special::Kind::Show&&!MissionIs(event,3)))return false;
        next.cover=*cover;
    }
    out=next;return true;
}
static bool (*buildGrant)(const Record&,const Scope&,Grant&)=BuildGrant;
static bool Operation(const Scope& own){
    return coop_special_receipt::Valid(own)&&own.personal==CoopPersonalSave::personal&&own.campaign==CoopPersonalSave::campaign&&
        own.session==state.Session()&&own.ticket==state.Ticket()&&own.operation==state.Operation()&&
        own.localPlayer==state.Player()&&own.hostNonce==(state.Player()==1?state.Nonce():state.PeerNonce())&&
        own.clientNonce==(state.Player()==2?state.Nonce():state.PeerNonce())&&own.event==state.Current().id&&
        own.catalog==state.CurrentContext().catalog&&own.stage==state.Current().stage&&own.kind==uint8_t(state.Current().kind);
}
static bool CanCommit(){
    return installed&&OwnCareer()&&state.Active()&&(state.Phase()&coop_special::Commit)&&
        !state.Terminal()&&storage&&storage->Ready()&&Operation(storage->tx.scope)&&storage->tx.ownDone&&fenceHeld(storage->tx.scope);
}
// Host Publish calls this BEFORE exposing shared Commit. Client calls it only
// after the immutable authenticated Commit has arrived through State.
static bool Authorize(){return CanCommit()&&(storage->tx.phase==Phase::Authorized||
    storage->tx.phase==Phase::Sealed||storage->tx.phase==Phase::Paid||storage->Authorize(storage->tx.scope));}
static bool OwnCompletion(const Scope& own,const Bytes& file){
    if(!coop_special_receipt::Native(file,own.personal))return false;
    const std::vector<uint8_t> payload(file.begin()+0x24,file.end());
    if(!coop_career::Completed(payload,own.event))return false;
    if(own.kind==uint8_t(coop_special::Kind::Show)){
        const auto* event=activeRecord();
        if(!event||event->spec.id!=own.event||!coop_career::Completed(payload,event->spec.destination))return false;
    }
    return true;
}
static bool (*ownCompletion)(const Scope&,const Bytes&)=OwnCompletion;
static Result Commit(const coop_special::NativeRequest& request){
    if(request.action!=coop_special::NativeAction::Commit||!request.serial||request.session!=state.Session()||
        request.ticket!=state.Ticket()||request.nonce!=state.Nonce()||request.operation!=state.Operation()||
        request.request!=state.RequestId()||!coop_special::Same(request.spec,state.Current())||!CanCommit())return Result::Refused;
    // A durable post-image is sufficient; NEVER re-enter an old native grant
    // or require a now-destroyed UI pointer for Sealed/Paid replay.
    if(storage->tx.phase==Phase::Sealed||storage->tx.phase==Phase::Paid)
        return storage->RecoverSealed()?Result::Applied:Result::Wait;
    if(!Authorize())return Result::Wait;
    const Scope own=storage->tx.scope;
    if(attempt.valid&&!coop_special_receipt::Same(attempt.scope,own))return Result::Refused;
    if(attempt.fault||attempt.dispatching)return Result::Refused;
    if(!attempt.entered){
        const auto* event=activeRecord();Grant grant;
        if(!event||!buildGrant(*event,own,grant))return Result::Wait;
        attempt={true,true,false,false,true,false,own,grant.choice.handler,event->pointer};
        // Latch BEFORE native side effects / reentrant UI. If a call faults or
        // an impossible postcondition occurs, this process NEVER grants again.
        const bool called=grantNative(grant);attempt.dispatching=false;
        if(!called||!nativeApplied(grant)){attempt.fault=true;return Result::Refused;}
        attempt.applied=true;
    }
    if(!attempt.applied)return Result::Refused;
    Bytes after;
    if(!serialize(after)||!ownCompletion(own,after))return Result::Wait;
    // On ambiguous IO caller must explicitly Reload storage. Retain the RAM
    // applied latch: retry serialization/Sealed install, NOT the native reward.
    return storage->SealAndInstall(after)?Result::Applied:Result::Wait;
}
static bool Reset(){
    // An uncertain/unsaved native grant may not be forgotten in this process.
    if(attempt.valid&&attempt.entered&&(!storage||!storage->Ready()||
        !coop_special_receipt::Same(storage->tx.scope,attempt.scope)||storage->tx.phase!=Phase::Paid))return false;
    attempt={};return true;
}
}} // namespace CoopSpecialNative::Reward
