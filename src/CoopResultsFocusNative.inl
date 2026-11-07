// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Results focus ownership repair, independent of passive CoopResultsTrace.
// Never manufactures/replays Accept and never calls the page-advance routine.
namespace CoopResultsFocusNative {
using FocusFn=void(__thiscall*)(void*,void*,void*,bool);
using GetterFn=uintptr_t(__cdecl*)(const char*);
using MouseFn=void(__thiscall*)(void*,uintptr_t);
static FocusFn gSetFocus=reinterpret_cast<FocusFn>(0x52B3A0);
static GetterFn gGetter=reinterpret_cast<GetterFn>(0x51CD60);
static MouseFn gMouse=reinterpret_cast<MouseFn>(0x54E020);
static uintptr_t kUi=0x8384C4,kManager=0x8363DC,kRaceFlow=0x8900D8;
static uintptr_t kFlow=0x8654A4,kMode=0x850078,kHandlerEvent=0x4E9A20;
static bool gInstalled=false;
static thread_local bool gRepairing=false;
static constexpr uint32_t kNotSelectable=0x04000000;
static constexpr const char* kPage="UI_PostRaceResults.fng";
template<class T> static T Read(uintptr_t at){T v{};ReadGameMemory(at,&v,sizeof(v));return v;}
static bool PageName(uintptr_t at){
    char text[sizeof("UI_PostRaceResults.fng")]{};
    return IsValidUserPtr(at)&&ReadGameMemory(at,text,sizeof(text))&&std::memcmp(text,kPage,sizeof(text))==0;
}
static bool InertRanking(uintptr_t object){
    // All six names are present in the original InGameRace.bun Results FNG.
    // Type alone is NOT sufficient: NEXT itself is also a type-2 text object.
    const uint32_t hash=Read<uint32_t>(object+0x10);
    uintptr_t events=~uintptr_t(0);
    return IsValidUserPtr(object)&&hash>=0xDA2E9A32&&hash<=0xDA2E9A37&&
        Read<uint32_t>(object+0x18)==2&&ReadGameMemory(object+0x3C,&events,sizeof(events))&&events==0;
}
static bool NativeContinue(uintptr_t object){
    if(Read<uint32_t>(object+0x10)!=0x0019DE3E || Read<uint32_t>(object+0x18)!=2 ||
        (Read<uint32_t>(object+0x1C)&kNotSelectable))return false;
    uintptr_t event=Read<uintptr_t>(object+0x3C),seen[32]{};bool found=false;
    for(unsigned i=0;i<32&&IsValidUserPtr(event);++i){
        for(unsigned j=0;j<i;++j)if(seen[j]==event)return false;
        seen[i]=event;
        uint32_t header[6]{};
        if(!ReadGameMemory(event,header,sizeof(header)))return false;
        const uint32_t hash=header[3];
        // Setter notifications must never run a customized focus script that
        // advances the page without a user Accept. Stock NEXT has neither.
        if(hash==0x55D1E635||hash==0xABC08912)return false;
        if(hash==0x0C407210&&!found){
            // Native5367E0 uses the FIRST matching event, not a later duplicate.
            const uintptr_t commands=header[5];
            uint32_t command[3]{};
            if(header[4]!=1||!IsValidUserPtr(commands)||
                !ReadGameMemory(commands,command,sizeof(command))||command[0]!=2||
                command[1]!=0x406415E3||command[2]!=0)return false;
            found=true;
        }
        event=header[1];
    }
    return event==0&&found;
}
static bool QuietFocusEvents(uintptr_t event){
    uintptr_t seen[32]{};
    for(unsigned i=0;i<32&&IsValidUserPtr(event);++i){
        for(unsigned j=0;j<i;++j)if(seen[j]==event)return false;seen[i]=event;
        uint32_t header[6]{};
        if(!ReadGameMemory(event,header,sizeof(header)))return false;
        const uint32_t hash=header[3];
        if(hash==0x55D1E635||hash==0xABC08912)return false;
        event=header[1];
    }
    return event==0;
}
struct Owner{uintptr_t engine=0,package=0,next=0,focus=0;uintptr_t objects[256]{};uint32_t count=0;};
static bool Resolve(Owner& out,uintptr_t expectedEngine=0,uintptr_t expectedPackage=0){
    if(!gInstalled||!IsCoopGameplayActive()||Read<uint32_t>(kFlow)!=6||Read<uint32_t>(kMode)!=1)return false;
    const uintptr_t race=Read<uintptr_t>(kRaceFlow),manager=Read<uintptr_t>(kManager);
    if(!IsValidUserPtr(race)||!IsValidUserPtr(manager)||Read<uint32_t>(race)!=7||
        !PageName(Read<uintptr_t>(Read<uintptr_t>(manager+0x38)+8)))return false;
    const uintptr_t ui=Read<uintptr_t>(kUi);out.engine=Read<uintptr_t>(ui+8);
    if(!IsValidUserPtr(ui)||!IsValidUserPtr(out.engine)||(expectedEngine&&out.engine!=expectedEngine))return false;
    // Only the active top package, never a covered results page or cached owner.
    out.package=Read<uintptr_t>(out.engine+0xE4);
    if(!IsValidUserPtr(out.package)||(expectedPackage&&out.package!=expectedPackage)||
        !PageName(Read<uintptr_t>(out.package+0xC))||!QuietFocusEvents(Read<uintptr_t>(out.package+0x5C))||Read<uint8_t>(out.package+0x14)!=1||
        Read<uint8_t>(out.package+0x15)!=0)return false;
    for(unsigned offset:{0x1Cu,0x20u,0x24u,0x28u})if(Read<uint32_t>(out.package+offset)!=1)return false;
    const uintptr_t descriptor=Read<uintptr_t>(out.package+0x40),handler=Read<uintptr_t>(descriptor+0x14);
    if(!IsValidUserPtr(descriptor)||Read<uintptr_t>(descriptor+0x18)!=out.package||!IsValidUserPtr(handler)||
        !PageName(Read<uintptr_t>(handler+4))||Read<uintptr_t>(Read<uintptr_t>(handler)+8)!=kHandlerEvent||
        Read<uintptr_t>(handler+0xC)!=0||Read<uint8_t>(handler+0x10)||Read<uint8_t>(handler+0x11))return false;
    out.count=Read<uint32_t>(out.package+0x94);
    const uintptr_t array=Read<uintptr_t>(out.package+0x90);
    if(!out.count||out.count>256||!IsValidUserPtr(array)||
        !ReadGameMemory(array,out.objects,out.count*sizeof(uintptr_t)))return false;
    for(uint32_t i=0;i<out.count;++i){
        const uintptr_t object=out.objects[i];if(!IsValidUserPtr(object))return false;
        for(uint32_t j=0;j<i;++j)if(out.objects[j]==object)return false;
        if(Read<uint32_t>(object+0x10)==0x0019DE3E){
            if(out.next||!NativeContinue(object))return false;out.next=object;
        }
    }
    out.focus=Read<uintptr_t>(out.package+0x8C);
    return out.next!=0;
}
static void Repair(uintptr_t engine=0,uintptr_t package=0){
    if(gRepairing)return;
    Owner owner{};if(!Resolve(owner,engine,package))return;
    gRepairing=true;
    unsigned disabled=0;bool repair=false;
    for(uint32_t i=0;i<owner.count;++i){
        const uintptr_t object=owner.objects[i];if(!InertRanking(object))continue;
        if(object==owner.focus)repair=true;
        uint32_t flags=Read<uint32_t>(object+0x1C);
        if(!(flags&kNotSelectable)){flags|=kNotSelectable;
            if(SafeWriteBytes(object+0x1C,&flags,sizeof(flags)))++disabled;}
    }
    // This is native non-selectable metadata (51CC60), NOT a visibility bit.
    // It lasts for this loaded FNG only; no profile, row text or save is touched.
    if(repair)gSetFocus(reinterpret_cast<void*>(owner.package),reinterpret_cast<void*>(owner.engine),
        reinterpret_cast<void*>(owner.next),true);
    if(disabled||repair){char line[256];std::snprintf(line,sizeof(line),
        "[RESULT_FOCUS_REPAIR] package=%08X old=%08X next=%08X inertRows=%u repaired=%u; native focus only, no Accept or page advance.\r\n",
        unsigned(owner.package),unsigned(owner.focus),unsigned(owner.next),disabled,unsigned(repair));LogMessage(line);}
    gRepairing=false;
}
static uintptr_t __cdecl Getter(const char* name){
    if(PageName(reinterpret_cast<uintptr_t>(name)))Repair();
    return gGetter(name);
}
static void __fastcall Mouse(void* engine,void*,uintptr_t package){
    if(engine&&package)Repair(reinterpret_cast<uintptr_t>(engine),package);
    gMouse(engine,package);
}
static void Tick(){Repair();}
struct Detour{uintptr_t address;BYTE expected[5];void* hook;BYTE* trampoline=nullptr;bool installed=false;};
static Detour gDetours[]{
    {0x51CD60,{0xA1,0xC4,0x84,0x83,0x00},reinterpret_cast<void*>(&Getter)},
    {0x54E020,{0x83,0xEC,0x18,0x56,0x57},reinterpret_cast<void*>(&Mouse)}
};
static void Jump(BYTE* at,const void* target){at[0]=0xE9;const int32_t rel=static_cast<int32_t>(
    reinterpret_cast<intptr_t>(target)-reinterpret_cast<intptr_t>(at+5));std::memcpy(at+1,&rel,4);}
static void Publish(unsigned i,uintptr_t target){if(!i)gGetter=reinterpret_cast<GetterFn>(target);else gMouse=reinterpret_cast<MouseFn>(target);}
static bool Rollback(unsigned i){
    auto& d=gDetours[i];if(d.installed){DWORD old=0,ignored=0;
        if(!VirtualProtect(reinterpret_cast<void*>(d.address),5,PAGE_EXECUTE_READWRITE,&old))return false;
        std::memcpy(reinterpret_cast<void*>(d.address),d.expected,5);FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(d.address),5);
        VirtualProtect(reinterpret_cast<void*>(d.address),5,old,&ignored);d.installed=false;}
    Publish(i,d.address);if(d.trampoline)VirtualFree(d.trampoline,0,MEM_RELEASE);d.trampoline=nullptr;return true;
}
static bool Install(){
    if(gInstalled)return true;
    const BYTE setter[]{0x8A,0x44,0x24,0x0C,0x84,0xC0,0x53,0x56};BYTE actual[8]{};
    if(!ReadGameMemory(reinterpret_cast<uintptr_t>(gSetFocus),actual,sizeof(setter))||std::memcmp(actual,setter,sizeof(setter))){
        LogMessage("[RESULT_FOCUS_REPAIR] Native setter differs; repair not installed.\r\n");return false;}
    for(const auto& d:gDetours)if(!ReadGameMemory(d.address,actual,5)||std::memcmp(actual,d.expected,5)){
        LogMessage("[RESULT_FOCUS_REPAIR] Native input signature differs; repair not installed.\r\n");return false;}
    for(unsigned i=0;i<2;++i){auto& d=gDetours[i];DWORD old=0,ignored=0;
        d.trampoline=static_cast<BYTE*>(VirtualAlloc(nullptr,32,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
        if(!d.trampoline||!VirtualProtect(reinterpret_cast<void*>(d.address),5,PAGE_EXECUTE_READWRITE,&old)){
            bool restored=true;for(unsigned j=0;j<2;++j)if(!Rollback(j))restored=false;
            LogMessage(restored?"[RESULT_FOCUS_REPAIR] Install failed; native entries restored.\r\n":
                "[RESULT_FOCUS_REPAIR] Install failed; forwarding trampoline retained.\r\n");return false;}
        std::memcpy(d.trampoline,d.expected,5);Jump(d.trampoline+5,reinterpret_cast<void*>(d.address+5));
        FlushInstructionCache(GetCurrentProcess(),d.trampoline,10);Publish(i,reinterpret_cast<uintptr_t>(d.trampoline));
        Jump(reinterpret_cast<BYTE*>(d.address),d.hook);FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(d.address),5);
        VirtualProtect(reinterpret_cast<void*>(d.address),5,old,&ignored);d.installed=true;
    }
    gInstalled=true;LogMessage("[RESULT_FOCUS_REPAIR] Owned Results ranking labels excluded from native focus; keyboard/mouse and tick repair enabled, no auto-continue.\r\n");return true;
}
} // namespace CoopResultsFocusNative
