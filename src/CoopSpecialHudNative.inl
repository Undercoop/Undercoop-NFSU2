// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Include after Abort/Bootstrap. The stock Bank and Show timer controllers
// share ONE Show_case_backing image.4CA090 only reapplies changed masks; when
// Show ends, its hide can outlive Cash's unchanged visible mask.4A9EF0 restores
// the three Cash labels every frame, but never that shared image.
namespace CoopSpecialNative { namespace Hud {
struct Addresses {uintptr_t director=0x8900AC,raceFlow=0x8900D8,ui=0x8384C4,hudVtable=0x798F10;};
static Addresses addresses;
using FindFn=uintptr_t(__cdecl*)(const char*,uint32_t);
using MaskFn=void(__thiscall*)(void*,uint32_t,uint32_t);
static FindFn findText=reinterpret_cast<FindFn>(0x537A10),findImage=reinterpret_cast<FindFn>(0x537A60);
static MaskFn applyMask=reinterpret_cast<MaskFn>(0x4C6690);
static ReceiptStore::Store* storage=&ReceiptStore::store;
struct Latch {uint64_t session=0;uint32_t ticket=0,operation=0,event=0;uint8_t player=0;bool attempted=false;};
static Latch restored,pending;
static bool Same(const Latch& proof){return proof.attempted&&proof.session==state.Session()&&proof.ticket==state.Ticket()&&
    proof.operation==state.Operation()&&proof.event==state.Current().id&&proof.player==state.Player();}
static bool Packages(){
    if(!Feng::Idle(addresses.ui))return false;
    uintptr_t ui=0,engine=0;
    if(!Read(addresses.ui,ui)||!Read(ui+8,engine)||!IsValidUserPtr(engine))return false;
    unsigned hud=0;
    for(unsigned offset:{0xE4u,0xF4u}){
        uintptr_t at=0;if(!Read(engine+offset,at))return false;
        std::array<uintptr_t,64> seen{};unsigned count=0;
        while(at){
            if(count==seen.size()||std::find(seen.begin(),seen.begin()+count,at)!=seen.begin()+count)return false;
            seen[count++]=at;Feng::Package package;
            // A disabled/removing or queued interactive package still owns
            // teardown. Never restore Bank through it or a shared pause menu.
            if(!Feng::ReadPackage(at,package)||!Feng::Passive(package.name))return false;
            if(offset==0xE4&&package.active&&Feng::Name(package.name,"HUD_SingleRace.fng"))++hud;
            at=package.next;
        }
    }
    return hud==1;
}
static bool Lookup(uintptr_t screen,std::array<uintptr_t,4>& widgets){
    __try {
        //505450 uppercases GUI names before hashing (catalog names do not).
        widgets={findText(reinterpret_cast<const char*>(screen),0x192F96D9),
            findText(reinterpret_cast<const char*>(screen),0x1930B057),
            findText(reinterpret_cast<const char*>(screen),0x31B0D166),
            findImage(reinterpret_cast<const char*>(screen),0x9A4E84A9)};return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool OwnedCash(uintptr_t& cash,uint32_t& low,uint32_t& high,bool& backingHidden){
    uintptr_t director=0,hud=0,vt=0,screen=0,cashScreen=0,timer=0,timerScreen=0,bank=0,data=0;
    uint32_t globalLow=0,globalHigh=0,maskLow=0,maskHigh=0,timerLow=0,timerHigh=0,timerMaskLow=0,timerMaskHigh=0;
    if(!Read(addresses.director,director)||!IsValidUserPtr(director)||!Read(director+0x9A4,hud)||!IsValidUserPtr(hud)||
        !Read(hud,vt)||vt!=addresses.hudVtable||!Read(hud+0x10,screen)||!Feng::Name(screen,"HUD_SingleRace.fng")||
        !Read(hud+8,globalLow)||!Read(hud+0xC,globalHigh)||!(globalHigh&0x80000)||globalHigh&4||
        !Read(hud+0x80,cash)||!IsValidUserPtr(cash)||!Read(cash+8,cashScreen)||cashScreen!=screen||
        !Read(cash+0x10,maskLow)||maskLow||!Read(cash+0x14,maskHigh)||maskHigh!=0x80000||
        !Read(cash+0x18,low)||!Read(cash+0x1C,high)||!(high&maskHigh)||
        !Read(hud+0x64,timer)||!IsValidUserPtr(timer)||!Read(timer+8,timerScreen)||timerScreen!=screen||
        !Read(timer+0x10,timerMaskLow)||timerMaskLow||!Read(timer+0x14,timerMaskHigh)||timerMaskHigh!=4||
        !Read(timer+0x18,timerLow)||!Read(timer+0x1C,timerHigh)||
        (timerMaskLow&timerLow)||(timerMaskHigh&timerHigh))return false;
    std::array<uintptr_t,4> widgets{};
    if(!findText||!findImage||!Lookup(screen,widgets)||!Read(cash+0x20,bank)||bank!=widgets[0]||
        !Read(cash+0x24,data)||data!=widgets[1])return false;
    for(unsigned i=0;i<widgets.size();++i){uint32_t type=0,flags=0;
        if(!IsValidUserPtr(widgets[i])||!Read(widgets[i]+0x18,type)||type!=(i==3?1u:2u)||
            !Read(widgets[i]+0x1C,flags)||(i!=3&&bool(flags&1)))return false;
        if(i==3)backingHidden=bool(flags&1);
        for(unsigned j=0;j<i;++j)if(widgets[i]==widgets[j])return false;
    }
    //4C6690 traverses this sentinel list and changes every listed widget.
    // Prove its complete constructor-defined ownership before invoking it.
    uintptr_t node=0,previous=cash;if(!Read(cash,node))return false;unsigned mask=0;
    for(unsigned i=0;i<4;++i){uintptr_t next=0,back=0,widget=0;
        if(!IsValidUserPtr(node)||node==cash||!Read(node,next)||!Read(node+4,back)||back!=previous||!Read(node+8,widget))return false;
        unsigned found=4;for(unsigned j=0;j<4;++j)if(widget==widgets[j])found=j;
        if(found==4||mask&(1u<<found))return false;mask|=1u<<found;previous=node;node=next;
    }
    uintptr_t tail=0;return node==cash&&mask==15&&Read(cash+4,tail)&&tail==previous;
}
static bool Replay(uintptr_t cash,uint32_t low,uint32_t high){
    __try {applyMask(reinterpret_cast<void*>(cash),low,high);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool Tick(){
    if(!installed||!OwnCareer()||!IsCoopGameplayActive()||!state.Terminal()||!state.LocalSaved()||
        !coop_special::Timed(state.Current().kind)||(state.Phase()&coop_special::Commit)||Same(restored)||
        !storage||!storage->Ready()||storage->tx.phase!=coop_special_receipt::Phase::Cancelled||
        !Reward::Operation(storage->tx.scope)||Abort::ExitPending())return false;
    // Keep the proven cancellation identity when Driver retires Abort's old
    // witness. A real modal/HUD transition may finish on a later frame, after
    // both Saved bits made State inactive. Never require Active here.
    if(Abort::OwnQuiescent())pending={state.Session(),state.Ticket(),state.Operation(),state.Current().id,state.Player(),true};
    if(!Same(pending))return false;
    uintptr_t mission=1,flow=0;uint32_t phase=0,gameFlow=0,mode=0,track=0,rf=0;uint8_t dialog=1;
    if(!Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission||
        !Read(CoopSpecialNative::addresses.career+0xD1C,phase)||(phase!=0&&phase!=7)||
        !Read(CoopSpecialNative::addresses.flow,gameFlow)||gameFlow!=6||
        !Read(CoopSpecialNative::addresses.mode,mode)||mode!=1||!Read(areaAddresses.track,track)||track!=4000||
        !Read(addresses.raceFlow,flow)||!IsValidUserPtr(flow)||!Read(flow,rf)||rf!=4||!Read(flow+0x28,dialog)||dialog||
        !Packages())return false;
    uintptr_t cash=0;uint32_t low=0,high=0;bool backingHidden=false;
    if(!applyMask||!OwnedCash(cash,low,high,backingHidden))return false;
    restored={state.Session(),state.Ticket(),state.Operation(),state.Current().id,state.Player(),true};
    if(!backingHidden)return false; // Already correct after the verified timer-mask transition.
    if(!Replay(cash,low,high))return false; // An uncertain side effect is never retried.
    LogMessage("[SPECIAL_HUD] Restored shared Bank backing with exact native Cash visibility; personal balance unchanged.\r\n");return true;
}
}} // namespace CoopSpecialNative::Hud
