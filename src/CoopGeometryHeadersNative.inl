// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#include "CoopGeometryHeaders.h"

namespace CoopGeometryHeadersNative {
static constexpr uintptr_t manager=0x82DBC8, loader=0x8A3288;
static coop_geometry::HeaderLeases<> leases;
static bool enabled=false,active=false,releasing=false,budgetLogged=false;
using LookupFn=uintptr_t(__thiscall*)(void*,const char*);
using OpenFn=int(__cdecl*)(const char*,int,void*,void*);
using CloseFn=int(__thiscall*)(void*,const char*,void*,void*);
using SolidFn=uintptr_t(__cdecl*)(uint32_t,void*);
using IndexFn=uintptr_t(__thiscall*)(void*,uint32_t);
static LookupFn lookup=reinterpret_cast<LookupFn>(0x48CE80);
static OpenFn open=reinterpret_cast<OpenFn>(0x4941C0);
static SolidFn solid=reinterpret_cast<SolidFn>(0x48DF90);
static IndexFn index=reinterpret_cast<IndexFn>(0x48CED0);
// Kept explicit so isolated native-entry fixtures do not need to reserve the
// original game's low address pages in an unrelated Windows process.
static uintptr_t closeAddress=0x493560,pumpAddress=0x63B160;
static uintptr_t closeResume=0x493565,pumpResume=0x63B166;
__declspec(naked) static void CloseOriginal() {
    __asm {
        push ebx
        mov ebx,[esp+8]
        jmp dword ptr [closeResume]
    }
}
static CloseFn close=reinterpret_cast<CloseFn>(CloseOriginal);
static bool Read(const char* path,coop_geometry::Header& out) {
    const uintptr_t node=lookup(reinterpret_cast<void*>(manager),path);
    return node && ReadGameMemory(node+8,out.path,sizeof(out.path)) &&
        std::memchr(out.path,0,sizeof(out.path)) &&
        ReadGameMemory(node+0xB4,&out.references,4) && (out.address=node)!=0;
}
static bool Acquire(const char* path) {return open(path,0,nullptr,nullptr)!=0;}
static bool Retain(const char* path) {
    const bool okay=leases.Retain(path,Read,Acquire);
    if(!okay && !budgetLogged && leases.Size()>=256) {
        LogMessage("[GEOMETRY] Header lease budget reached; native ownership preserved.\r\n");budgetLogged=true;
    }
    return okay;
}
static void Release() {
    if(!active && !leases.Size())return;
    active=false;releasing=true;
    const auto count=leases.Release(Read,[](const char* path){close(reinterpret_cast<void*>(manager),path,nullptr,nullptr);});
    releasing=false;budgetLogged=false;
    if(count) {char text[128]{};std::snprintf(text,sizeof(text),"[GEOMETRY] Released %u owned car geometry index leases.\r\n",static_cast<unsigned>(count));LogMessage(text);}
}
// Called at Stage4, before native mesh submission. The scope survives native
// Stage5 header retirement and live car/AI changes; the frontend ends it.
static void Begin() {if(enabled&&IsCoopGameplayActive())active=true;}
static void Tick(DWORD flow) {
    if(active && (!IsCoopGameplayActive() || flow==1 || flow==3))Release();
}
static void __cdecl BeforeClose(uintptr_t owner,const char* path,void* callback) {
    if(!enabled||!active||releasing||!IsCoopGameplayActive()||owner!=manager||!path)return;
    char name[128]{};
    if(!ReadGameMemory(reinterpret_cast<uintptr_t>(path),name,sizeof(name))||!std::memchr(name,0,sizeof(name))||
        !coop_geometry::GeometryPath(name))return;
    if(callback) {
        // Native only calls a close callback at zero refs. Drop OUR reference
        // first, so a caller's completion barrier has precisely stock timing.
        leases.Drop(name,Read,[](const char* value){close(reinterpret_cast<void*>(manager),value,nullptr,nullptr);});
    } else Retain(name);
}
static void __cdecl BeforePump(uintptr_t owner) {
    if(!enabled||!active||releasing||!IsCoopGameplayActive()||owner!=loader)return;
    // Pin open car geometry indexes before the first batch and subsequent
    // native live-traffic batches. No per-render-frame header scan or file IO.
    constexpr uintptr_t head=manager+0x18;
    uintptr_t next=0;
    if(!ReadGameMemory(head,&next,4))return;
    char paths[256][128]{};unsigned count=0;
    for(unsigned n=0;next&&next!=head&&n<1024;++n) {
        const uintptr_t current=next;
        if(!ReadGameMemory(current,&next,4)||next==current)break;
        char path[128]{};
        if(ReadGameMemory(current+8,path,sizeof(path))&&std::memchr(path,0,sizeof(path))&&coop_geometry::GeometryPath(path)) {
            if(count==256)break;
            std::memcpy(paths[count++],path,sizeof(path));
        }
    }
    // The lookup/open API MRU-relinks headers, so only call after traversal.
    for(unsigned i=0;i<count;++i)Retain(paths[i]);
}
__declspec(naked) static void CloseHook() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,528
        and esp,0FFFFFFF0h
        mov [esp+512],eax
        fxsave [esp]
        push dword ptr [eax+44]
        push dword ptr [eax+40]
        push dword ptr [eax+24]
        call BeforeClose
        add esp,12
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        jmp CloseOriginal
    }
}
__declspec(naked) static void PumpHook() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,528
        and esp,0FFFFFFF0h
        mov [esp+512],eax
        fxsave [esp]
        push dword ptr [eax+24]
        call BeforePump
        add esp,4
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        sub esp,11Ch
        jmp dword ptr [pumpResume]
    }
}
static unsigned Probe(uint32_t key) {
    const uintptr_t mesh=solid(key,nullptr);uintptr_t data=0;
    if(mesh && ReadGameMemory(mesh+0x84,&data,4) && data)return 2;
    const uintptr_t header=index(reinterpret_cast<void*>(manager),key);
    return header?1:0;
}
static bool ResourceDrawable(uintptr_t node,coop_geometry::Residency& result) {
    uintptr_t body=0,wheels=0;uint32_t count=0;
    if(!enabled||!active || !ReadGameMemory(node+0x954,&body,4)||!body||
        !ReadGameMemory(node+0x958,&wheels,4)||!wheels||
        !ReadGameMemory(body+0x14,&count,4)||count>192)return false;
    uint32_t keys[200]{};
    if(!ReadGameMemory(body+0x18,keys,count*4)||!ReadGameMemory(wheels+0x18,keys+count,8*4))return false;
    result=coop_geometry::Inspect(keys,count+8,Probe);
    return result.indexed!=0 && result.missing==0;
}
static bool Install() {
    if(enabled)return true;
    struct Patch {uintptr_t at;uint8_t bytes[6];size_t size;void* hook;bool done=false;};
    Patch patches[]={
        {closeAddress,{0x53,0x8B,0x5C,0x24,0x08},5,reinterpret_cast<void*>(CloseHook)},
        {pumpAddress,{0x81,0xEC,0x1C,0x01,0,0},6,reinterpret_cast<void*>(PumpHook)}
    };
    for(const auto& p:patches) {uint8_t found[6]{};if(!ReadGameMemory(p.at,found,p.size)||std::memcmp(found,p.bytes,p.size)) {
        LogMessage("[GEOMETRY] Unsupported native header/pump entry; existing mod hooks preserved, geometry adapter disabled.\r\n");return false;
    }}
    for(auto& p:patches) {
        DWORD old=0;if(!VirtualProtect(reinterpret_cast<void*>(p.at),p.size,PAGE_EXECUTE_READWRITE,&old)) {
            for(auto& undo:patches)if(undo.done)SafeWriteBytes(undo.at,undo.bytes,undo.size);
            return false;
        }
        uint8_t bytes[6]={0xE9,0,0,0,0,0x90};
        const int32_t relative=static_cast<int32_t>(reinterpret_cast<uintptr_t>(p.hook)-(p.at+5));
        std::memcpy(bytes+1,&relative,4);std::memcpy(reinterpret_cast<void*>(p.at),bytes,p.size);
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(p.at),p.size);
        DWORD unused=0;VirtualProtect(reinterpret_cast<void*>(p.at),p.size,old,&unused);p.done=true;
    }
    enabled=true;LogMessage("[GEOMETRY] Verified scoped car/shared/wheel geometry index leases installed for native vehicle streaming.\r\n");return true;
}
}
