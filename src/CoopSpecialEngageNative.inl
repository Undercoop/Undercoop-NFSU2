// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Verified stock Special Event dialog adapter. No hooks are installed here.
// It never persists a UI/widget pointer, teleports a car, writes a mission row,
// grants a reward or synthesizes an input. The lifecycle owns admission and
// durable preparation; native calls must only run on its game thread.
namespace CoopSpecialNative { namespace Engage {
static constexpr char kScreen[]="UI_EngageEventDialog.fng";
static constexpr uint32_t kClick=0x0C407210,kTimedButton=0xD72F002A,kCoverButton=0xB8A7C6CE,kCancelButton=0x63857DE0;
struct Addresses {uintptr_t manager=0x8384C4,vtable=0x791838,handler=0x498F00,raceFlow=0x8900D8;};
static Addresses addresses;
using FindFn=void*(__cdecl*)(const char*);
using ObjectFn=void*(__cdecl*)(const char*,uint32_t);
using SnapshotFn=void(__thiscall*)(void*);
using OpenFn=void(__cdecl*)();
static FindFn findHandler=reinterpret_cast<FindFn>(0x51BD10);
static ObjectFn findObject=reinterpret_cast<ObjectFn>(0x5379C0);
static SnapshotFn snapshot=reinterpret_cast<SnapshotFn>(0x500B00);
static OpenFn openDialog=reinterpret_cast<OpenFn>(0x498D00);
static bool (*atStart)(const coop_special::Spec&)=OwnAtStart;
static bool (*eligible)(const coop_special::Spec&)=OwnOpportunity;
static const Catalog* (*catalog)()=FactsCatalog;
struct Live {coop_special::Spec spec{};uintptr_t handler=0,widget=0;};
struct StartPermit {bool active=false,used=false;uintptr_t mission=0;};
static StartPermit startPermit;
using CloseFn=void(__cdecl*)(const char*);
using ResumeFn=void(__thiscall*)(void*,uint32_t,uint32_t);
static CloseFn closePrompt=reinterpret_cast<CloseFn>(0x558F00);
static ResumeFn resumeWorld=reinterpret_cast<ResumeFn>(0x601020);
struct CoverCancel {bool valid=false,resumeEntered=false,fault=false;coop_special::Spec spec{};uintptr_t flow=0;};
static CoverCancel coverCancel;
static bool CancelBusy(){return coverCancel.valid;}
static bool CancelFault(){return coverCancel.valid&&coverCancel.fault;}
static bool CancelPending(const coop_special::Spec& spec){return coverCancel.valid&&coop_special::Same(coverCancel.spec,spec);}
static bool PromptGone(){const char* names[]{kScreen};return Feng::Absent(names,1,addresses.manager);}
static bool CoverDialog(uintptr_t& flow,uint32_t& state,uint8_t& active){
    uint32_t saved=0,reason=0;
    return Read(addresses.raceFlow,flow)&&IsValidUserPtr(flow)&&Read(flow,state)&&
        Read(flow+0x24,saved)&&saved==4&&Read(flow+0x28,active)&&active<=1&&
        Read(flow+0x4C,reason)&&reason==17;
}
static bool CloseCoverCall(){
    __try {closePrompt(kScreen);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool ResumeCoverCall(uintptr_t flow){
    __try {resumeWorld(reinterpret_cast<void*>(flow),8,1);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool PollCoverCancel(){
    uintptr_t flow=0,mission=1;uint32_t state=0;uint8_t active=1;
    if(!coverCancel.valid||coverCancel.fault||!OwnCareer()||
        !Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission||
        !CoverDialog(flow,state,active)||flow!=coverCancel.flow||!PromptGone())return false;
    if(state==6&&active==1&&!coverCancel.resumeEntered){
        // The named FEng removal can be queued. Resume ONLY after its actual
        // package is gone. Never repeat a native call after an uncertain fault.
        coverCancel.resumeEntered=true;
        if(!ResumeCoverCall(flow)){coverCancel.fault=true;return false;}
        return false;
    }
    if(state!=4||active||!coverCancel.resumeEntered)return false;
    LogMessage("[SPECIAL_CANCEL] Unstarted cover prompt removed; native world resumed, no photo proof or reward.\r\n");
    coverCancel={};return true;
}
// Driver's Start callback must allow only this exact first nested 541120 ->
// 533BE0. A broad replay bit would also bypass reentrant solo/duplicate starts.
static bool TakeStartPermission(void* manager,uintptr_t mission){
    if(!startPermit.active||startPermit.used||uintptr_t(manager)!=CoopSpecialNative::addresses.career+0xC00||
        !mission||mission!=startPermit.mission)return false;
    startPermit.used=true;return true;
}
static bool CallsFind(uint32_t hash,uintptr_t& handler,uintptr_t& object){
    __try {handler=reinterpret_cast<uintptr_t>(findHandler(kScreen));object=reinterpret_cast<uintptr_t>(findObject(kScreen,hash));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool FindRecordByPointer(uintptr_t pointer,const Record*& out){
    out=nullptr;const auto* all=catalog();if(!all||!pointer)return false;
    for(unsigned i=0;i<all->coverCount;++i)if(all->covers[i].pointer==pointer){out=&all->covers[i];return true;}
    for(unsigned i=0;i<all->missionCount;++i)if(all->missions[i].pointer==pointer){out=&all->missions[i];return true;}
    return false;
}
static bool ReadOpportunity(bool copied,const Record*& record){
    record=nullptr;uintptr_t area=0,race=0,shop=0,cover=0,mission=0;uint32_t areaHash=0;
    const uintptr_t career=CoopSpecialNative::addresses.career;
    if(!Read(career+(copied?0x8704:0x8700),area)||!IsValidUserPtr(area)||!Read(area+0x30,areaHash)||
        !Read(career+(copied?0x8708:0x871C),race)||race||!Read(career+(copied?0x870C:0x8724),shop)||shop||
        !Read(career+(copied?0x8710:0x8728),cover)||!Read(career+(copied?0x8714:0x8720),mission))return false;
    // The stock dialog prioritizes a cover over a timed mission when both
    // descriptors exist. Never silently accept the other/recycled opportunity.
    const Record* found=nullptr;if(!FindRecordByPointer(cover?cover:mission,found)||found->spec.start!=areaHash||
        (cover?coop_special::Timed(found->spec.kind):!coop_special::Timed(found->spec.kind)))return false;
    record=found;return true;
}
static bool ReadLive(const coop_special::Spec& expected,Live& out){
    Live next;const Record* current=nullptr;
    if(!coop_special::Valid(expected)||!OwnCareer()||!ReadOpportunity(true,current)||!coop_special::Same(current->spec,expected))return false;
    const uint32_t button=coop_special::Timed(expected.kind)?kTimedButton:kCoverButton;
    if(!CallsFind(button,next.handler,next.widget)||!IsValidUserPtr(next.handler)||!IsValidUserPtr(next.widget))return false;
    uintptr_t vt=0,fn=0,name=0;uint32_t hash=0;
    if(!Read(next.handler,vt)||vt!=addresses.vtable||!Read(vt+8,fn)||fn!=addresses.handler||
        !Read(next.handler+4,name)||!Choice::String(name,kScreen)||!Read(next.widget+0x10,hash)||hash!=button||
        !::CoopSpecialNative::Feng::Owned(kScreen,next.handler,true,addresses.manager))return false;
    next.spec=expected;out=next;return true;
}
static bool CallsOpen(){
    __try {snapshot(reinterpret_cast<void*>(CoopSpecialNative::addresses.career));openDialog();return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool UiCanOpen(){
    return ::CoopSpecialNative::Feng::Idle(addresses.manager);
}
static bool Ensure(const coop_special::Spec& expected){
    if(!OwnCareer()||!atStart(expected)||!eligible(expected))return false;
    Live current;if(ReadLive(expected,current))return true;
    // Snapshot only the EXACT currently encountered native area/opportunity.
    // 498D00 then creates its registered 4CE650 ->4B5260 stock dialog. It does
    // not accept/start it. A pending native push can finish on the next frame.
    const Record* opportunity=nullptr;
    if(!ReadOpportunity(false,opportunity)||!coop_special::Same(opportunity->spec,expected))return false;
    uintptr_t found=0,object=0;
    if(!UiCanOpen()||!CallsFind(coop_special::Timed(expected.kind)?kTimedButton:kCoverButton,found,object)||found)return false;
    return CallsOpen()&&ReadLive(expected,current);
}
// Returns a classified, freshly owned native Accept, never the value supplied
// by a peer. Native Cancel is a separate path, NOT a race accept button.
static bool AcceptButton(void* self,uint32_t message,uintptr_t widget){
    uintptr_t vt=0,name=0;uint32_t hash=0;
    return message==kClick&&IsValidUserPtr(uintptr_t(self))&&IsValidUserPtr(widget)&&
        Read(uintptr_t(self),vt)&&vt==addresses.vtable&&Read(uintptr_t(self)+4,name)&&Choice::String(name,kScreen)&&
        Read(widget+0x10,hash)&&(hash==kTimedButton||hash==kCoverButton);
}
static bool Accepted(void* self,uint32_t message,uintptr_t widget,Record& out){
    if(!AcceptButton(self,message,widget))return false;
    const Record* current=nullptr;Live live;
    if(!ReadOpportunity(true,current)||!ReadLive(current->spec,live)||live.handler!=uintptr_t(self)||live.widget!=widget)return false;
    out=*current;return true;
}
static bool CallsAccept(const Live& live){
    __try {reinterpret_cast<Detours::HandlerFn>(Detours::Original(Detours::Outer))(
        reinterpret_cast<void*>(live.handler),kClick,live.widget,0,0);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool Accept(const coop_special::Spec& expected){
    Live live;if(!OwnCareer()||!atStart(expected)||!eligible(expected)||!ReadLive(expected,live))return false;
    const Record* opportunity=nullptr;
    if(startPermit.active||!ReadOpportunity(true,opportunity)||!coop_special::Same(opportunity->spec,expected))return false;
    startPermit={true,false,coop_special::Timed(expected.kind)?opportunity->pointer:0};
    const bool okay=CallsAccept(live),began=coop_special::Timed(expected.kind)?startPermit.used:!startPermit.used;
    startPermit={};
    // Original is already a trampoline: leave ALL entry gates active for UI
    // reentry, especially early payout/completion and a second user Accept.
    return okay&&began;
}
static bool CancelUnstarted(const coop_special::Spec& expected){
    Live live;uintptr_t mission=1,handler=0,button=0;uint32_t hash=0;
    if(coverCancel.valid)return CancelPending(expected)&&PollCoverCancel();
    if(!coop_special::Timed(expected.kind)){
        uintptr_t flow=0;uint32_t state=0;uint8_t active=0;
        if(startPermit.active||!OwnCareer()||!Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission||
            !ReadLive(expected,live)||!CoverDialog(flow,state,active)||state!=6||active!=1)return false;
        // Stock hides Cancel for covers and its RF exit deliberately skips
        // cover prompts. Clicking that hidden button cannot close this UI.
        // Use the stock NAMED Event close (also used by timed-event teardown),
        // then the exact RF6->saved4 resume; never invoke a photo/camera exit.
        coverCancel={true,false,false,expected,flow};
        if(!CloseCoverCall()){coverCancel.fault=true;return false;}
        LogMessage("[SPECIAL_CANCEL] Named unstarted cover close queued; waiting for real FEng/RaceFlow cleanup.\r\n");
        return false;
    }
    if(startPermit.active||!OwnCareer()||!Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission||
        !ReadLive(expected,live)||!CallsFind(kCancelButton,handler,button)||handler!=live.handler||!IsValidUserPtr(button)||
        !Read(button+0x10,hash)||hash!=kCancelButton)return false;
    live.widget=button;
    // Native Cancel63857DE0 is NOT Accept. It resumes/closes only this fresh
    // owned engage dialog. Arrived camera/unique/failure UI cannot pass ReadLive.
    // No broad Outer replay permission: a reentrant Accept stays intercepted.
    return CallsAccept(live);
}
}} // namespace CoopSpecialNative::Engage
