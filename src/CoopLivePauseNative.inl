// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include "CoopSharedPause.h" // Reserve/consume the retired wire identity only.

// Pause menus stay local. The native RF6 UI and its Resume/Restart/Leave
// handlers remain intact. Exact native simulation phases borrow RF4 while an
// ordinary CO-OP pause stays RF6 for UI/input dispatch between world updates.
namespace CoopLivePause {
struct Addresses {
    uintptr_t gameFlow=0x8654A4,gameMode=0x850078,raceFlow=0x8900D8;
};
static Addresses addresses;
static bool installed=false;
static uintptr_t dragPauseCall=0x5FA945,dragMode=0x850088;
static bool dragPauseInstalled=false;
static uintptr_t nativeContinuation=0x58131E,liveContinuation=0x581349;
static uintptr_t inputContinuation=0;
static unsigned shadowDepth=0;
static DWORD shadowThread=0;
using ControllerStep=void(__cdecl*)();
using WorldStep=void(__thiscall*)(void*,float);
static ControllerStep controllerStep=reinterpret_cast<ControllerStep>(0x5EB390);
static WorldStep worldStep=reinterpret_cast<WorldStep>(0x609CE0);
static ControllerStep racerStep=reinterpret_cast<ControllerStep>(0x600900);
static bool LiveMenu() {
    if(!installed || !IsCoopGameplayActive() || gPendingTrackLoader) return false;
    uint32_t flow=0,mode=0,state=0,saved=0,reason=0;uintptr_t race=0;
    return ReadGameMemory(addresses.gameFlow,&flow,4) && flow==6 &&
        ReadGameMemory(addresses.gameMode,&mode,4) && mode==1 &&
        ReadGameMemory(addresses.raceFlow,&race,4) && IsValidUserPtr(race) &&
        ReadGameMemory(race,&state,4) && state==6 &&
        ReadGameMemory(race+0x24,&saved,4) && saved==4 &&
        ReadGameMemory(race+0x4C,&reason,4) && reason==7;
}
struct WorldScope {uintptr_t flow=0;};
static WorldScope BeginWorld() {
    WorldScope scope{};
    if(!LiveMenu()) return scope;
    uintptr_t race=0;const uint32_t playing=4;
    if(ReadGameMemory(addresses.raceFlow,&race,4) &&
        SafeWriteBytes(race,&playing,sizeof(playing))) {
        scope.flow=race;shadowThread=GetCurrentThreadId();++shadowDepth;
    }
    return scope;
}
static void EndWorld(const WorldScope& scope) {
    if(!scope.flow) return;
    if(shadowDepth) --shadowDepth;
    uintptr_t current=0;uint32_t game=0,mode=0,state=0,saved=0,reason=0;
    // A real finish/loader/state transition owns its new state. Never roll it
    // back to Pause, nor touch a replaced native RaceFlow allocation.
    if(!gPendingTrackLoader && ReadGameMemory(addresses.gameFlow,&game,4) && game==6 &&
        ReadGameMemory(addresses.gameMode,&mode,4) && mode==1 &&
        ReadGameMemory(addresses.raceFlow,&current,4) && current==scope.flow &&
        ReadGameMemory(current,&state,4) && state==4 &&
        ReadGameMemory(current+0x24,&saved,4) && saved==4 &&
        ReadGameMemory(current+0x4C,&reason,4) && reason==7) {
        const uint32_t paused=6;SafeWriteBytes(current,&paused,sizeof(paused));
    }
    if(!shadowDepth) shadowThread=0;
}
static void __cdecl Controllers() {
    const auto scope=BeginWorld();
    __try {controllerStep();} __finally {EndWorld(scope);}
}
static void __fastcall Simulate(void* self,void*,float delta) {
    const auto scope=BeginWorld();
    __try {worldStep(self,delta);} __finally {EndWorld(scope);}
}
static void __cdecl Racers() {
    const auto scope=BeginWorld();
    __try {racerStep();} __finally {EndWorld(scope);}
}
static bool __stdcall InputHeld() {return shadowDepth && shadowThread==GetCurrentThreadId();}
__declspec(naked) static void InputHook() {
    __asm {
        pushfd
        pushad
        mov ebx,esp
        sub esp,528
        and esp,0xfffffff0
        mov edi,esp
        fxsave [edi]
        call InputHeld
        fxrstor [edi]
        test al,al
        jnz menu_input
        mov esp,ebx
        popad
        popfd
        jmp dword ptr [inputContinuation]
    menu_input:
        mov esp,ebx
        popad
        popfd
        // Exact stock RF6 input-mask result (5F34F4): the local driver cannot
        // steer/accelerate through the pause UI while AI simulation advances.
        xor eax,eax
        xor edx,edx
        ret
    }
}
static bool __stdcall ShouldAdvance() {return LiveMenu();}

static bool DragFailureCanPause(uintptr_t coordinator) {
    if(!installed || gPendingTrackLoader || gCoopRaceState!=COOP_RACE_IN_PROGRESS || !CoopFinishRaceActive())return false;
    uint32_t mode=0,state=0;uintptr_t native=0,liveCoordinator=0;
    if(!ReadGameMemory(dragMode,&mode,4) || mode!=2 ||
        !ReadGameMemory(addresses.raceFlow,&native,4) || !IsValidUserPtr(native) ||
        !ReadGameMemory(native,&state,4) || state!=4 ||
        !ReadGameMemory(CoopRaceFinishNative::kCoordinator,&liveCoordinator,4) ||
        !IsValidUserPtr(liveCoordinator) || coordinator!=liveCoordinator)return false;
    const auto race=RaceStateSnapshot();
    const auto& own=race.finish[race.host?0:1];
    // Native 4A4020 labels status 8 Totaled and 9 Engine Blown in Drag.
    return race.ready && !race.ResultReady() && coop_race::ValidFinish(own) && (own.status==8 || own.status==9);
}

static int __fastcall DragPauseAllowed(void* coordinator,void*) {
    // The post-finish controller has one input-only AllPlayersDone call.
    // Leave the other callers (including native result entry) untouched.
    const int done=CoopRaceFinishNative::HookAllPlayersDone(coordinator,nullptr);
    if(done || !DragFailureCanPause(reinterpret_cast<uintptr_t>(coordinator)))return done;
    LogMessage("[DRAG_WAIT] Local pause allowed after Drag failure; shared results still pending.\r\n");
    return 1;
}

static bool InstallDragWaitPause() {
    if(dragPauseInstalled)return true;
    if(!gRaceFinishHooksReady)return false;
    // 5FA900 checks native action BD, state and modal locks, then asks this
    // one query. Native message7 still owns opening and closing its menu.
    const BYTE expected[]{0xE8,0x66,0xDC,0xFF,0xFF,0x85,0xC0,0x75,0x0E};
    BYTE observed[sizeof(expected)]{};
    if(!ReadGameMemory(dragPauseCall,observed,sizeof(observed)) || std::memcmp(observed,expected,sizeof(expected)))return false;
    DWORD old=0;
    auto* target=reinterpret_cast<BYTE*>(dragPauseCall);
    if(!VirtualProtect(target,5,PAGE_EXECUTE_READWRITE,&old))return false;
    const auto relative=static_cast<int32_t>(reinterpret_cast<uintptr_t>(&DragPauseAllowed)-(dragPauseCall+5));
    std::memcpy(target+1,&relative,4);
    FlushInstructionCache(GetCurrentProcess(),target,5);
    DWORD ignored=0;VirtualProtect(target,5,old,&ignored);
    dragPauseInstalled=true;
    return true;
}

__declspec(naked) static void FrameHook() {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, 0xfffffff0
        mov edi, esp
        fxsave [edi]
        call ShouldAdvance
        fxrstor [edi]
        test al, al
        jnz live_menu
        mov esp, ebx
        popad
        popfd
        // Replay the single displaced MOV, with the original flags/registers.
        mov eax, dword ptr [addresses + 8]
        mov eax, dword ptr [eax]
        jmp dword ptr [nativeContinuation]
    live_menu:
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [liveContinuation]
    }
}
static bool Install() {
    if(installed) return true;
    constexpr uintptr_t entry=0x581319;
    // Includes the RF6 classification and normal world-update destination.
    // Foreign hooks fail closed. No native dispatch slots are replaced.
    const BYTE expected[]{0xA1,0xD8,0x00,0x89,0x00,0x3B,0xC3,0x74,0x08,
        0x39,0x30,0x75,0x04,0xB0,0x01,0xEB,0x02,0x32,0xC0};
    const BYTE liveExpected[]{0x39,0x35,0xA4,0x54,0x86,0x00};
    BYTE actual[sizeof(expected)]{},liveActual[sizeof(liveExpected)]{};
    if(!ReadGameMemory(entry,actual,sizeof(actual)) || std::memcmp(actual,expected,sizeof(actual)) ||
        !ReadGameMemory(liveContinuation,liveActual,sizeof(liveActual)) ||
        std::memcmp(liveActual,liveExpected,sizeof(liveActual))) {
        LogMessage("[LIVE_MENU] Native frame signature differs; live-menu adapter not installed.\r\n");return false;
    }
    struct Patch {uintptr_t at;size_t bytes;BYTE before[6],after[6];DWORD protect;};
    Patch patches[5]{{entry,5},{0x5812C1,5},{0x581407,5},{0x58148E,5},{0x5F34C0,6}};
    std::memcpy(patches[0].before,expected,5);
    const uintptr_t original[]{0,0x5EB390,0x609CE0,0x600900};
    const uintptr_t replacement[]{uintptr_t(&FrameHook),uintptr_t(&Controllers),uintptr_t(&Simulate),uintptr_t(&Racers),uintptr_t(&InputHook)};
    const BYTE inputExpected[]{0x8B,0x41,0x04,0x8B,0x50,0x1C};
    std::memcpy(patches[4].before,inputExpected,6);
    for(unsigned i=0;i<5;++i) {
        auto& patch=patches[i];
        if(i>0 && i<4) {patch.before[0]=0xE8;const int32_t offset=int32_t(original[i]-(patch.at+5));std::memcpy(patch.before+1,&offset,4);}
        BYTE observed[6]{};
        if(!ReadGameMemory(patch.at,observed,patch.bytes) || std::memcmp(observed,patch.before,patch.bytes)) {
            LogMessage("[LIVE_MENU] Native AI/input call signature differs; live-menu adapter not installed.\r\n");return false;
        }
        patch.after[0]=(i>0&&i<4)?0xE8:0xE9;
        const int32_t jump=int32_t(replacement[i]-(patch.at+5));std::memcpy(patch.after+1,&jump,4);
        if(patch.bytes==6)patch.after[5]=0x90;
    }
    auto* input=static_cast<BYTE*>(VirtualAlloc(nullptr,11,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    if(!input)return false;
    std::memcpy(input,inputExpected,6);input[6]=0xE9;
    const int32_t back=int32_t(0x5F34C6-(uintptr_t(input)+11));std::memcpy(input+7,&back,4);
    FlushInstructionCache(GetCurrentProcess(),input,11);
    unsigned writable=0;
    for(;writable<5;++writable) {
        auto& patch=patches[writable];
        if(!VirtualProtect(reinterpret_cast<void*>(patch.at),patch.bytes,PAGE_EXECUTE_READWRITE,&patch.protect))break;
    }
    if(writable!=5) {
        for(unsigned i=writable;i>0;--i){auto& patch=patches[i-1];DWORD unused=0;VirtualProtect(reinterpret_cast<void*>(patch.at),patch.bytes,patch.protect,&unused);}
        VirtualFree(input,0,MEM_RELEASE);return false;
    }
    inputContinuation=uintptr_t(input);
    for(auto& patch:patches) {
        std::memcpy(reinterpret_cast<void*>(patch.at),patch.after,patch.bytes);
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(patch.at),patch.bytes);
    }
    // Restore in reverse: multiple call sites occupy the same code page.
    for(unsigned i=5;i>0;--i){auto& patch=patches[i-1];DWORD unused=0;VirtualProtect(reinterpret_cast<void*>(patch.at),patch.bytes,patch.protect,&unused);}
    installed=true;
    LogMessage("[LIVE_MENU] Shared pause removed; local pause menus keep AI/world/race progress running. Native menu handlers and local input suppression preserved.\r\n");
    return true;
}
}

bool CoopSpecialClockPaused() {
    if(CoopLivePause::LiveMenu()) return false;
    uintptr_t race=0;uint32_t state=0,reason=0;
    return ReadGameMemory(CoopLivePause::addresses.raceFlow,&race,4) && IsValidUserPtr(race) &&
        ReadGameMemory(race,&state,4) && state==6 &&
        ReadGameMemory(race+0x4C,&reason,4) && reason==7;
}
bool ReceiveCoopRetiredPausePacket(const char* data,int bytes) {
    uint32_t magic=0;if(!data || bytes<4) return false;std::memcpy(&magic,data,4);
    // Delayed/old pause commands can never stop this world or open a menu.
    return magic==coop_pause::kMagic;
}
bool InstallCoopLivePauseHook() {
    if(!CoopLivePause::Install())return false;
    if(!CoopLivePause::InstallDragWaitPause())
        LogMessage("[DRAG_WAIT] Native post-finish Pause signature differs; stock input retained.\r\n");
    return true;
}
