// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Live co-op appearance requests add a second texture generation while the
// stock single-player car streaming pool4 still holds the old world. Log proof:
// 63ACE0 ->48D420 ->63A1B0 ->43BD50, return63A240, all eviction candidates live.
// For ONLY our proven resource groups use the engine's existing pool0 path.
// Stock63ACE0 already uses flags0 for non-race loader modes. Allocation/free
// stay native440390/440540 (pool index is stored in each allocation header).
// No texture-pool resizing, foreign handle release, forced eviction or ignored assert.
namespace CoopPeerTextureNative {
struct Owned {uint32_t id=0;uintptr_t node=0,group=0;};
static std::array<Owned,64> owned{};
static uintptr_t manager=0x83579C;
static uintptr_t allocateOriginal=0x48D420;
static bool enabled=false;
static unsigned redirected=0;
static uintptr_t slotPoolAddress=0x8A1D00;
using TrimFn=void(__thiscall*)(void*,uint32_t);
using GrowFn=void(__thiscall*)(void*,uint32_t);
static TrimFn trim=reinterpret_cast<TrimFn>(0x63B130);
static GrowFn grow=reinterpret_cast<GrowFn>(0x440BB0);
static bool EnsureSlot() {
    uintptr_t pool=0;uint32_t used=0,capacity=0,stride=0,flags=0;
    if(!IsCoopGameplayActive() || !ReadGameMemory(slotPoolAddress,&pool,4) || !IsValidUserPtr(pool) ||
        !ReadGameMemory(pool+0x18,&used,4) || !ReadGameMemory(pool+0x2C,&capacity,4) ||
        !ReadGameMemory(pool+0x28,&stride,4) || stride!=0x1240 || capacity<22 || capacity>32 || used>capacity) return false;
    if(used<capacity)return true;
    // Native63AE70 otherwise forces eviction BEFORE its slot allocator can
    // grow. Reclaim only unreferenced native cache nodes, never live visuals.
    trim(reinterpret_cast<void*>(gPeerAppearanceLoader),1);
    if(!ReadGameMemory(pool+0x18,&used,4) || !ReadGameMemory(pool+0x2C,&capacity,4) || used>capacity)return false;
    if(used<capacity)return true;
    if(capacity>=32 || !ReadGameMemory(pool+0x14,&flags,4) || !(flags&1))return false;
    // The exact native slot allocator grows/links its own metadata segment.
    // Bound at32 nodes (stock22 + co-op generations); no texture RAM budget
    // or existing pointer is replaced. Native reset/destructor owns the page.
    grow(reinterpret_cast<void*>(pool),(std::min)(2u,32u-capacity));
    return ReadGameMemory(pool+0x18,&used,4) && ReadGameMemory(pool+0x2C,&capacity,4) &&
        capacity<=32 && used<capacity;
}
static bool Matches(const Owned& item,uintptr_t group) {
    if(!item.id || item.group!=group || !group) return false;
    // Native637840 stamps a process-monotonic803690 ID at123C. A cached
    // address alone is never authority after native reclamation/reuse.
    uintptr_t actualGroup=0;
    return FindPeerAppearanceResourceNode(item.id)==item.node &&
        ReadGameMemory(item.node+0x95C,&actualGroup,4) && actualGroup==group;
}
static uint32_t __cdecl SelectPool(uintptr_t nativeManager,uintptr_t group,uint32_t flags) {
    if(!enabled || nativeManager!=manager || flags!=4) return flags;
    for(const auto& item:owned) if(Matches(item,group)) {++redirected;return 0;}
    return flags; // Local player, native shops, traffic/AI, SP and other pools untouched.
}
__declspec(naked) static void AllocateHook() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,528
        and esp,0FFFFFFF0h
        mov [esp+512],eax
        fxsave [esp]
        push dword ptr [eax+48] // original third argument: allocation flags
        push dword ptr [eax]    // EDI: native texture group in63ACE0
        push dword ptr [eax+24] // ECX: native texture memory manager
        call SelectPool
        add esp,12
        mov edx,[esp+512]
        mov [edx+48],eax
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        jmp dword ptr [allocateOriginal] // original RET0C/return/callback pipeline
    }
}
// Also retain the selected flags in the parent's EBP.494D60 takes that same
// pool for its read/callback pipeline, not just the allocation call's argument.
__declspec(naked) static void FlagsHook() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,528
        and esp,0FFFFFFF0h
        mov [esp+512],eax
        fxsave [esp]
        push 4
        push edi
        push dword ptr [manager]
        call SelectPool
        add esp,12
        mov edx,[esp+512]
        mov [edx+8],eax // EBP, originally MOV EBP,4
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        ret
    }
}
struct CallSite {uintptr_t at=0;void* hook=nullptr;BYTE bytes[5]{};bool installed=false;};
static CallSite calls[]={
    {0x63AD79,reinterpret_cast<void*>(FlagsHook)},
    {0x63AD8A,reinterpret_cast<void*>(AllocateHook)},
    {0x63ADAD,reinterpret_cast<void*>(AllocateHook)}
};
static bool WriteCall(CallSite& site,bool install) {
    auto* target=reinterpret_cast<BYTE*>(site.at);DWORD previous=0;
    if(!VirtualProtect(target,5,PAGE_EXECUTE_READWRITE,&previous))return false;
    if(install) {
        target[0]=0xE8;
        const int32_t displacement=static_cast<int32_t>(reinterpret_cast<uintptr_t>(site.hook)-(site.at+5));
        std::memcpy(target+1,&displacement,4);
    } else std::memcpy(target,site.bytes,5);
    FlushInstructionCache(GetCurrentProcess(),target,5);
    DWORD ignored=0;VirtualProtect(target,5,previous,&ignored);site.installed=install;return true;
}
}
bool MarkOwnedPeerTextureResource(uint32_t resource) {
    using namespace CoopPeerTextureNative;
    if(!enabled || !IsCoopGameplayActive() || !resource) return false;
    const uintptr_t node=FindPeerAppearanceResourceNode(resource);uintptr_t group=0;int32_t refs=0;
    if(!node || !ReadGameMemory(node+0x95C,&group,4) || group!=node+0xCF0 ||
        !ReadGameMemory(node+8,&refs,4) || refs<=0) return false;
    for(auto& item:owned) {
        if(item.id==resource && Matches(item,group))return true;
        if(item.id && FindPeerAppearanceResourceNode(item.id)!=item.node)item={};
    }
    for(auto& item:owned) if(!item.id) {item={resource,node,group};return true;}
    return false; // No unclassified request may enter the dangerous old pool.
}
bool InstallPeerTextureAllocationHooks() {
    using namespace CoopPeerTextureNative;
    if(enabled)return true;
    for(auto& call:calls) {
        int32_t displacement=0;
        if(!ReadGameMemory(call.at,call.bytes,5)) return false;
        std::memcpy(&displacement,call.bytes+1,4);
        if(call.hook==reinterpret_cast<void*>(FlagsHook)) {
            if(call.bytes[0]!=0xBD || displacement!=4)return false;
        } else if(call.bytes[0]!=0xE8 || call.at+5+displacement!=allocateOriginal)return false;
    }
    for(auto& call:calls) if(!WriteCall(call,true)) {
        for(auto& changed:calls)if(changed.installed)WriteCall(changed,false);
        return false;
    }
    enabled=true;
    LogMessage("[APPEARANCE_MEMORY] Verified private peer texture groups use native main-pool allocation/free; stock car pool and assertions unchanged.\r\n");
    return true;
}
