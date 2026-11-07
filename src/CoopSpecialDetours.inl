// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include <intrin.h>
// Verified x86 native ABI boundary only. No mission policy, timers, geometry,
// payout, save writes, synthetic input or stale UI replay lives in this layer.
// The complete adapter set must be supplied before installation is attempted.
namespace CoopSpecialNative { namespace Detours {
enum Site:unsigned {Outer,Start,Timer,Arrival,Payout,CoverDone,ShowDone,Unique,UniqueDone,Failure,PhotoUi,PhotoExit,MagazineExit,FailureUi,Count};
using HandlerFn=void(__thiscall*)(void*,uint32_t,uintptr_t,uintptr_t,uintptr_t);
using OneFn=void(__thiscall*)(void*,uintptr_t);
using ZeroFn=void(__thiscall*)(void*);
struct Callbacks {
    bool (*outer)(void*,uint32_t,uintptr_t,uintptr_t,uintptr_t)=nullptr;
    bool (*start)(void*,uintptr_t)=nullptr;
    bool (*timer)(void*)=nullptr;
    bool (*arrival)(void*,uintptr_t)=nullptr;
    bool (*payout)(void*,uintptr_t)=nullptr;
    bool (*coverDone)(void*,uintptr_t)=nullptr; // Verified native return site, not constructor/payout inference.
    bool (*showDone)(void*,uintptr_t)=nullptr;
    bool (*unique)(void*,uint32_t,uintptr_t,uintptr_t,uintptr_t)=nullptr;
    bool (*uniqueDone)(void*,uintptr_t)=nullptr; // Confirm561875 and Forfeit561793 are NOT equivalent.
    bool (*failure)(void*)=nullptr;
    bool (*photoUi)(void*,uint32_t,uintptr_t,uintptr_t,uintptr_t)=nullptr;
    bool (*photoExit)(void*)=nullptr;
    bool (*magazineExit)(void*)=nullptr;
    bool (*failureUi)(void*,uint32_t,uintptr_t,uintptr_t,uintptr_t)=nullptr;
};
static Callbacks callbacks;
static uint32_t replayMask=0;
static bool installedEntries=false;
static decltype(&VirtualAlloc) allocate=VirtualAlloc;
static decltype(&VirtualProtect) protect=VirtualProtect;
static decltype(&VirtualFree) release=VirtualFree;
struct Entry {uintptr_t address=0;unsigned length=0;BYTE expected[12]{};void* replacement=nullptr;BYTE* trampoline=nullptr;bool active=false;int relativeCall=-1;};
static void __fastcall OuterHook(void*,void*,uint32_t,uintptr_t,uintptr_t,uintptr_t);
static void __fastcall StartHook(void*,void*,uintptr_t);
static void __fastcall TimerHook(void*,void*);
static void __fastcall ArrivalHook(void*,void*,uintptr_t);
static void __fastcall PayoutHook(void*,void*,uintptr_t);
static void __fastcall CoverDoneHook(void*,void*);
static void __fastcall ShowDoneHook(void*,void*,uintptr_t);
static void __fastcall UniqueHook(void*,void*,uint32_t,uintptr_t,uintptr_t,uintptr_t);
static void __fastcall UniqueDoneHook(void*,void*);
static void __fastcall FailureHook(void*,void*);
static void __fastcall PhotoUiHook(void*,void*,uint32_t,uintptr_t,uintptr_t,uintptr_t);
static void __fastcall PhotoExitHook(void*,void*);
static void __fastcall MagazineExitHook(void*,void*);
static void __fastcall FailureUiHook(void*,void*,uint32_t,uintptr_t,uintptr_t,uintptr_t);
static std::array<Entry,Count> entries{{
    {0x498F00,6,{0x55,0x8B,0xEC,0x8B,0x45,0x08},reinterpret_cast<void*>(OuterHook)},
    {0x533BE0,6,{0x55,0x8B,0xEC,0x83,0xEC,0x1C},reinterpret_cast<void*>(StartHook)},
    {0x5016A0,5,{0x8B,0x41,0x08,0x85,0xC0},reinterpret_cast<void*>(TimerHook)},
    {0x532B10,6,{0x56,0x57,0x8B,0x7C,0x24,0x0C},reinterpret_cast<void*>(ArrivalHook)},
    {0x541210,5,{0x53,0x8B,0x5C,0x24,0x08},reinterpret_cast<void*>(PayoutHook)},
    {0x533680,8,{0x83,0xEC,0x1C,0xA1,0xAC,0x00,0x89,0x00},reinterpret_cast<void*>(CoverDoneHook)},
    {0x541960,10,{0x56,0x8B,0xF1,0x83,0xBE,0x1C,0x01,0x00,0x00,0x03},reinterpret_cast<void*>(ShowDoneHook)},
    {0x561720,6,{0x55,0x8B,0xEC,0x56,0x8B,0xF1},reinterpret_cast<void*>(UniqueHook)},
    {0x5419C0,6,{0x8B,0x15,0x28,0x84,0x83,0x00},reinterpret_cast<void*>(UniqueDoneHook)},
    {0x501600,6,{0x8B,0x91,0x1C,0x01,0x00,0x00},reinterpret_cast<void*>(FailureHook)},
    {0x4CF170,5,{0x55,0x8B,0xEC,0x51,0x56},reinterpret_cast<void*>(PhotoUiHook)},
    // The first relative CALL must be relocated, not copied at a new EIP.
    {0x499FA0,9,{0x53,0x56,0x8B,0xF1,0xE8,0x67,0xFF,0xFF,0xFF},reinterpret_cast<void*>(PhotoExitHook),nullptr,false,4},
    {0x54B090,6,{0x56,0x8B,0xF1,0x8A,0x46,0x70},reinterpret_cast<void*>(MagazineExitHook)},
    {0x499690,6,{0x55,0x8B,0xEC,0x8B,0x45,0x08},reinterpret_cast<void*>(FailureUiHook)}
}};
static uintptr_t Original(Site site){const auto& e=entries[site];return e.trampoline?uintptr_t(e.trampoline):e.address;}
static bool Intercept(Site site){return installedEntries&&installed&&OwnCareer()&&!(replayMask&(1u<<site));}
static void __fastcall OuterHook(void* self,void*,uint32_t msg,uintptr_t a,uintptr_t b,uintptr_t c){
    if(Intercept(Outer)&&callbacks.outer(self,msg,a,b,c))return;reinterpret_cast<HandlerFn>(Original(Outer))(self,msg,a,b,c);
}
static void __fastcall StartHook(void* self,void*,uintptr_t value){
    if(Intercept(Start)&&callbacks.start(self,value))return;reinterpret_cast<OneFn>(Original(Start))(self,value);
}
static void __fastcall TimerHook(void* self,void*){
    if(Intercept(Timer)&&callbacks.timer(self))return;reinterpret_cast<ZeroFn>(Original(Timer))(self);
}
static void __fastcall ArrivalHook(void* self,void*,uintptr_t value){
    if(Intercept(Arrival)&&callbacks.arrival(self,value))return;reinterpret_cast<OneFn>(Original(Arrival))(self,value);
}
static void __fastcall PayoutHook(void* self,void*,uintptr_t value){
    if(Intercept(Payout)&&callbacks.payout(self,value))return;reinterpret_cast<OneFn>(Original(Payout))(self,value);
}
static void __fastcall CoverDoneHook(void* self,void*){
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    if(Intercept(CoverDone)&&callbacks.coverDone(self,caller))return;reinterpret_cast<ZeroFn>(Original(CoverDone))(self);
}
static void __fastcall ShowDoneHook(void* self,void*,uintptr_t value){
    if(Intercept(ShowDone)&&callbacks.showDone(self,value))return;reinterpret_cast<OneFn>(Original(ShowDone))(self,value);
}
static void __fastcall UniqueHook(void* self,void*,uint32_t msg,uintptr_t a,uintptr_t b,uintptr_t c){
    if(Intercept(Unique)&&callbacks.unique(self,msg,a,b,c))return;reinterpret_cast<HandlerFn>(Original(Unique))(self,msg,a,b,c);
}
static void __fastcall UniqueDoneHook(void* self,void*){
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    if(Intercept(UniqueDone)&&callbacks.uniqueDone(self,caller))return;reinterpret_cast<ZeroFn>(Original(UniqueDone))(self);
}
static void __fastcall FailureHook(void* self,void*){
    if(Intercept(Failure)&&callbacks.failure(self))return;reinterpret_cast<ZeroFn>(Original(Failure))(self);
}
static void __fastcall PhotoUiHook(void* self,void*,uint32_t msg,uintptr_t a,uintptr_t b,uintptr_t c){
    if(Intercept(PhotoUi)&&callbacks.photoUi(self,msg,a,b,c))return;reinterpret_cast<HandlerFn>(Original(PhotoUi))(self,msg,a,b,c);
}
static void __fastcall PhotoExitHook(void* self,void*){
    if(Intercept(PhotoExit)&&callbacks.photoExit(self))return;reinterpret_cast<ZeroFn>(Original(PhotoExit))(self);
}
static void __fastcall MagazineExitHook(void* self,void*){
    if(Intercept(MagazineExit)&&callbacks.magazineExit(self))return;reinterpret_cast<ZeroFn>(Original(MagazineExit))(self);
}
static void __fastcall FailureUiHook(void* self,void*,uint32_t msg,uintptr_t a,uintptr_t b,uintptr_t c){
    if(Intercept(FailureUi)&&callbacks.failureUi(self,msg,a,b,c))return;reinterpret_cast<HandlerFn>(Original(FailureUi))(self,msg,a,b,c);
}
struct ReplayScope {
    uint32_t previous;explicit ReplayScope(uint32_t allowed):previous(replayMask){replayMask|=allowed;}
    ~ReplayScope(){replayMask=previous;}
    ReplayScope(const ReplayScope&)=delete;ReplayScope& operator=(const ReplayScope&)=delete;
};
static void Jump(BYTE* at,const void* destination){at[0]=0xE9;const int32_t rel=int32_t(uintptr_t(destination)-uintptr_t(at)-5);std::memcpy(at+1,&rel,4);}
static bool ExpectedPatch(const Entry& e){
    BYTE current[12]{},patch[12]{};std::memset(patch,0x90,e.length);Jump(patch,e.replacement);
    // Jump above is relative to the temporary buffer; compute the actual
    // address-relative displacement before comparing a currently owned hook.
    const int32_t rel=int32_t(uintptr_t(e.replacement)-e.address-5);std::memcpy(patch+1,&rel,4);
    return ReadSpan(e.address,current,e.length)&&!std::memcmp(current,patch,e.length);
}
static bool Rollback(){
    installedEntries=false;bool okay=true;
    for(size_t i=entries.size();i>0;--i){auto& e=entries[i-1];
        if(e.active){DWORD old=0,ignored=0;
            // Never overwrite a later third-party detour, nor free a trampoline
            // that another live hook could still call through.
            if(!ExpectedPatch(e)||!protect(reinterpret_cast<void*>(e.address),e.length,PAGE_EXECUTE_READWRITE,&old)){okay=false;continue;}
            std::memcpy(reinterpret_cast<void*>(e.address),e.expected,e.length);FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(e.address),e.length);
            protect(reinterpret_cast<void*>(e.address),e.length,old,&ignored);e.active=false;
        }
        if(e.trampoline){release(e.trampoline,0,MEM_RELEASE);e.trampoline=nullptr;}
    }
    replayMask=0;return okay;
}
static bool Install(){
    if(installedEntries)return true;
    if(!callbacks.outer||!callbacks.start||!callbacks.timer||!callbacks.arrival||!callbacks.payout||!callbacks.coverDone||
        !callbacks.showDone||!callbacks.unique||!callbacks.uniqueDone||!callbacks.failure||!callbacks.photoUi||
        !callbacks.photoExit||!callbacks.magazineExit||!callbacks.failureUi)return false;
    for(const auto& e:entries){BYTE actual[12]{};
        if(e.active||e.trampoline||e.length<5||e.length>12||!e.replacement||
            (e.relativeCall!=-1&&(e.relativeCall<0||unsigned(e.relativeCall)+5>e.length||e.expected[e.relativeCall]!=0xE8))||
            !ReadSpan(e.address,actual,e.length)||std::memcmp(actual,e.expected,e.length))return false;
    }
    // Reserve every trampoline before changing the first native entry point.
    for(auto& e:entries){
        e.trampoline=static_cast<BYTE*>(allocate(nullptr,32,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
        if(!e.trampoline){Rollback();return false;}
        std::memcpy(e.trampoline,e.expected,e.length);
        if(e.relativeCall>=0){
            int32_t old=0;std::memcpy(&old,e.expected+e.relativeCall+1,4);
            const uintptr_t target=e.address+e.relativeCall+5+old;
            const int32_t relocated=int32_t(target-uintptr_t(e.trampoline+e.relativeCall+5));
            std::memcpy(e.trampoline+e.relativeCall+1,&relocated,4);
        }
        Jump(e.trampoline+e.length,reinterpret_cast<void*>(e.address+e.length));
        if(!FlushInstructionCache(GetCurrentProcess(),e.trampoline,e.length+5)){Rollback();return false;}
    }
    for(auto& e:entries){DWORD old=0,ignored=0;
        if(!protect(reinterpret_cast<void*>(e.address),e.length,PAGE_EXECUTE_READWRITE,&old)){Rollback();return false;}
        Jump(reinterpret_cast<BYTE*>(e.address),e.replacement);if(e.length>5)std::memset(reinterpret_cast<void*>(e.address+5),0x90,e.length-5);e.active=true;
        const bool flushed=FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(e.address),e.length)!=FALSE;
        const bool protectedAgain=protect(reinterpret_cast<void*>(e.address),e.length,old,&ignored)!=FALSE;
        if(!flushed||!protectedAgain){Rollback();return false;}
    }
    installedEntries=true;return true;
}
}} // namespace CoopSpecialNative::Detours
