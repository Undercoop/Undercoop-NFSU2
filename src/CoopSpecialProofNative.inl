// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Native proof and ordering only. No original payout/grant/UI-exit is invoked
// here. Driver installs these gates only with the COMPLETE save/recovery layer.
// Constructors may call native rewards early; those are never own Done proof.
namespace CoopSpecialNative { namespace Proof {
using Scope=coop_special_receipt::Scope;
static ReceiptStore::Store* storage=&ReceiptStore::store;
static const Record* (*activeRecord)()=ActiveRecord;
static const Record* (*findRecord)(uint32_t)=FindRuntimeRecord;
static uint32_t ReadOwnCarId(){Choice::OwnCar car;return Choice::ReadGarageCar(car)?car.id:0;}
static uint32_t (*carId)()=ReadOwnCarId;
static bool (*captureChoice)(const coop_special::Spec&,uintptr_t,Choice::Frozen&)=Choice::CaptureChoice;
static bool (*validateChoice)(const Choice::Frozen&,Choice::Live&)=Choice::ValidateSameChoice;
static bool (*ownedScreen)(uintptr_t,uintptr_t&,bool)=Choice::OwnedScreen;
struct Witness {bool valid=false;Scope identity;uint32_t cover=0;Choice::Frozen unique;};
static Witness pending;
static bool SameBase(Scope a,Scope b){a.choiceId=a.choiceSlot=b.choiceId=b.choiceSlot=0;return coop_special_receipt::Same(a,b);}
static bool ScopeFor(const Record& record,Scope& out){
    uint32_t stage=~0u;
    if(!installed||!OwnCareer()||!state.Active()||state.Terminal()||!(state.Phase()&coop_special::Release)||
        !(state.Own()&coop_special::Arrived)||!lifecycle.NativeProofWindow(state)||!storage||!storage->Ready()||
        !coop_special::Same(record.spec,state.Current())||!Read(CoopSpecialNative::addresses.career+0x7AF8,stage)||stage!=record.spec.stage||
        !CoopPersonalSave::Name(CoopPersonalSave::campaign)||!CoopPersonalSave::Name(CoopPersonalSave::personal)||
        CampaignHash(CoopPersonalSave::campaign)!=state.CurrentContext().campaign)return false;
    Scope own;own.campaign=CoopPersonalSave::campaign;own.personal=CoopPersonalSave::personal;
    own.event=record.spec.id;own.catalog=state.CurrentContext().catalog;own.kind=uint8_t(record.spec.kind);own.stage=record.spec.stage;
    own.session=state.Session();own.ticket=state.Ticket();own.operation=state.Operation();own.localPlayer=state.Player();
    own.hostNonce=state.Player()==1?state.Nonce():state.PeerNonce();own.clientNonce=state.Player()==2?state.Nonce():state.PeerNonce();
    own.carId=carId();
    if(!coop_special_receipt::Valid(own)||storage->tx.phase!=coop_special_receipt::Phase::Prepared||
        !SameBase(own,storage->tx.scope))return false;
    out=storage->tx.scope;return true;
}
static bool Tick(){
    const auto* record=activeRecord();Scope own;
    if(!pending.valid||!record||!ScopeFor(*record,own)||!SameBase(own,pending.identity))return false;
    if(pending.unique.valid){
        Choice::Live live;
        if(!validateChoice(pending.unique,live)||!Choice::Same(live.stable,pending.unique.stable)||
            live.stable.car.id!=own.carId)return false;
        if(coop_special_receipt::ChoiceKnown(own)){
            if(own.choiceId!=live.stable.id||own.choiceSlot!=live.stable.slot)return false;
        }else{
            if(!storage->BindChoice(own,live.stable.id,live.stable.slot))return false;
            own=storage->tx.scope;
        }
    }
    // Flush own genuine proof BEFORE setting/publishing protocol Done. A UI
    // constructor, coincidental cash balance or peer packet cannot reach here.
    if(!storage->tx.ownDone&&!storage->MarkDone(own))return false;
    return NoteDone(record->spec.id,runtimeNow());
}
static bool Cover(void* self,uintptr_t nativeReturn){
    if(!installed||!OwnCareer())return false;
    const auto* record=activeRecord();Scope own;
    if(!record||!ScopeFor(*record,own)||uintptr_t(self)!=CoopSpecialNative::addresses.career+0x4210)return true;
    const uint32_t hash=record->spec.kind==coop_special::Kind::Show?record->spec.destination:record->spec.id;
    const auto* cover=findRecord(hash);uint32_t bound=0;
    if(!cover||coop_special::Timed(cover->spec.kind)||cover->spec.stage!=record->spec.stage||
        !Read(uintptr_t(self)+8,bound)||bound!=cover->spec.id)return true;
    // Original PE has exactly two 533680 call sites: frontend view return
    // 498EC4 and actual camera Continue return 4CEEB7. Neither constructor's
    // 541210/541960 call is accepted as the user's shutter/view proof.
    if(nativeReturn!=(cover->spec.kind==coop_special::Kind::Frontend?0x498EC4u:0x4CEEB7u))return true;
    if(pending.valid&&(!SameBase(own,pending.identity)||pending.unique.valid||pending.cover!=hash))return true;
    pending.valid=true;pending.identity=own;pending.cover=hash;Tick();return true; // Native completion stays held until shared Commit.
}
static bool Unique(void* self,uint32_t message,uintptr_t,uintptr_t,uintptr_t){
    if(!installed||!OwnCareer())return false;
    if(message==0xC98356BA&&pending.valid&&pending.unique.valid&&pending.unique.owner==uintptr_t(self)&&
        storage&&storage->Ready()&&coop_special_receipt::Terminal(storage->tx.phase)&&SameBase(storage->tx.scope,pending.identity)&&
        storage->tx.scope.personal==CoopPersonalSave::personal&&storage->tx.scope.campaign==CoopPersonalSave::campaign){
        uintptr_t handler=0;uint8_t completed=0;
        if(ownedScreen(uintptr_t(self),handler,true)&&handler==uintptr_t(self)&&Read(uintptr_t(self)+0x150,completed)&&completed==1)
            return false; // Only after own durable settlement may stock 54FD40/5404A0 exit/load free roam.
    }
    const auto* record=activeRecord();Scope own;
    if(!record||(record->spec.kind!=coop_special::Kind::Visual&&record->spec.kind!=coop_special::Kind::Performance)||!ScopeFor(*record,own))return true;
    if(message==Choice::kForfeit){state.Fail(1,runtimeNow());return true;} // Never native 5419C0's completed-without-part branch.
    if(message==Choice::kConfirm){
        Choice::Frozen frozen=pending.unique;
        if(!captureChoice(record->spec,uintptr_t(self),frozen)||!frozen.valid||!Choice::Valid(frozen.stable)||
            frozen.stable.car.id!=own.carId||(pending.valid&&!SameBase(own,pending.identity)))return true;
        pending.valid=true;pending.identity=own;pending.unique=frozen;pending.cover=0;Tick();return true;
    }
    if(pending.valid&&pending.unique.valid){
        // A frozen selection must not change between durable Done and native
        // replay. The normal frame tick is safe while completion byte150 is0.
        uint8_t complete=1;
        return uintptr_t(self)!=pending.unique.owner||message!=0xC98356BA||!Read(uintptr_t(self)+0x150,complete)||complete!=0;
    }
    return false;
}
static bool HoldPayout(void*,uintptr_t){return installed&&OwnCareer();}
static bool HoldShowDone(void*,uintptr_t){return installed&&OwnCareer();}
static bool HoldUniqueDone(void*,uintptr_t){return installed&&OwnCareer();}
static void Reset(){pending={};}
}} // namespace CoopSpecialNative::Proof
