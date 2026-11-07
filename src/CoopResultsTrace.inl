// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Passive, bounded result-menu diagnostics. Native handlers are called once,
// with unchanged arguments; this module never advances a page on its own.
namespace CoopResultsTrace {
using DispatchFn=void(__thiscall*)(void*,uint32_t,uint32_t,uint32_t,uint32_t);
using AdvanceFn=void(__thiscall*)(void*);
using WidgetFn=void(__thiscall*)(void*,uintptr_t,uintptr_t,uint32_t,uint32_t);
static DispatchFn gDispatch=reinterpret_cast<DispatchFn>(0x55B5C0);
static AdvanceFn gAdvance=reinterpret_cast<AdvanceFn>(0x4BFF80);
static WidgetFn gWidget=reinterpret_cast<WidgetFn>(0x5367E0);
static uintptr_t kManager=0x8363DC,kGameFlow=0x8654A4,kRaceFlow=0x8900D8;
static uintptr_t kInputHashTable=0x7F7D20;
static bool gInstalled=false;
static uint32_t (*gCaptureReader)()=nullptr;
struct Context { uintptr_t manager=0,raceFlow=0; uint32_t page=0,last=0; };
// No locks across native callbacks/reentrant messages. Counters are private to
// each game/input thread; logging itself uses the existing log writer.
static thread_local uintptr_t gContextManager=0;
static thread_local unsigned gWindowEvents=0,gDispatchEvents=0,gKeyboardEvents=0,gAdvanceEvents=0;
static thread_local unsigned gStateCalls=0,gBufferCalls=0,gFilteredCalls=0;
static thread_local uint32_t gSeenMessages[16]{};
static thread_local unsigned gSeenCount=0;
static thread_local DWORD gLastSnapshot=0,gLastBufferStatus=0;
static thread_local DWORD gLastDetailed=0;
static thread_local unsigned gAttempt=0;
static thread_local uint32_t gLastPage=0,gLastKeys=~uint32_t(0);
static thread_local HRESULT gLastStateHr=E_UNEXPECTED;
static thread_local BYTE gLastRawState[256]{};
static thread_local bool gHaveRawState=false;

template<class T> static T Read(uintptr_t address) {
    T value{}; ReadGameMemory(address,&value,sizeof(value)); return value;
}
static void Name(uintptr_t address,char (&name)[64]) {
    std::memset(name,0,sizeof(name));
    if(IsValidUserPtr(address)) ReadGameMemory(address,name,sizeof(name)-1);
    name[sizeof(name)-1]=0;
}
#include "CoopResultsRoutes.inl"
static void InputAttempt(const char* reason) {
    ++gAttempt;
    const DWORD now=GetTickCount();
    // Windows and buffered input often report the same physical key together.
    // Every event is logged; only their expensive route walk is coalesced.
    if(!gLastDetailed || now-gLastDetailed>=75){gLastDetailed=now;RouteSnapshot(reason);}
}
static bool Active(Context& context) {
    if(!gInstalled || !IsCoopGameplayActive() || Read<uint32_t>(kGameFlow)!=6) return false;
    context.manager=Read<uintptr_t>(kManager); context.raceFlow=Read<uintptr_t>(kRaceFlow);
    if(!IsValidUserPtr(context.manager) || !IsValidUserPtr(context.raceFlow) ||
        Read<uint32_t>(context.raceFlow)!=7) return false;
    context.page=Read<uint32_t>(context.manager+0x38);
    context.last=Read<uint32_t>(context.manager+0x34);
    if(gContextManager!=context.manager) {
        gContextManager=context.manager;
        gWindowEvents=gDispatchEvents=gKeyboardEvents=gAdvanceEvents=0;
        gStateCalls=gBufferCalls=gFilteredCalls=gSeenCount=0;
        gLastSnapshot=gLastBufferStatus=gLastPage=0;
        gLastDetailed=0;gAttempt=0;
        gLastKeys=~uint32_t(0); gLastStateHr=E_UNEXPECTED;
        gHaveRawState=false;std::memset(gLastRawState,0,sizeof(gLastRawState));
    }
    return true;
}
static void Snapshot(const char* reason) {
    const uintptr_t manager=Read<uintptr_t>(kManager),race=Read<uintptr_t>(kRaceFlow);
    const uintptr_t current=Read<uintptr_t>(manager+0x38),last=Read<uintptr_t>(manager+0x34);
    const uintptr_t ui=Read<uintptr_t>(0x8384C4),engine=Read<uintptr_t>(ui+8);
    const uintptr_t uiManager=Read<uintptr_t>(0x8384D8);
    char pageName[64],lastName[64],line[768];
    Name(Read<uintptr_t>(current+8),pageName); Name(Read<uintptr_t>(last+8),lastName);
    std::snprintf(line,sizeof(line),
        "[RESULT_TRACE] %s ms=%lu tid=%lu focus=%u flow=%u mode=%u track=%u rf=%u manager=%08X page=%08X(%s) last=%08X(%s) ui=%u/%u/%u engine=%u/%u/%u filter=%u dialog=%u/%08X menuClock=%u debounceAt=%u simClock=%u DIcalls=%u/%u filtered=%u consoleFlags=%u.\r\n",
        reason,GetTickCount(),GetCurrentThreadId(),IsGameWindowFocused()?1u:0u,
        Read<uint32_t>(kGameFlow),Read<uint32_t>(0x850078),Read<uint32_t>(0x89E7A0),
        Read<uint32_t>(race),static_cast<unsigned>(manager),static_cast<unsigned>(current),pageName,
        static_cast<unsigned>(last),lastName,Read<uint8_t>(ui+4),Read<uint8_t>(ui+5),Read<uint8_t>(ui+6),
        Read<uint8_t>(engine),Read<uint8_t>(engine+1),Read<uint8_t>(engine+2),Read<uint32_t>(0x864F2C),
        Read<uint8_t>(0x83A9C0),static_cast<unsigned>(Read<uintptr_t>(0x83A9C4)),
        Read<uint32_t>(0x8651AC),Read<uint32_t>(uiManager+0x68),Read<uint32_t>(0x86518C),
        gStateCalls,gBufferCalls,gFilteredCalls,gCaptureReader?gCaptureReader():0);
    LogMessage(line);
    uintptr_t seen[12]{},package=Read<uintptr_t>(engine+0xE4);
    for(unsigned index=0;index<12 && IsValidUserPtr(package);++index) {
        bool repeated=false;
        for(unsigned before=0;before<index;++before) if(seen[before]==package) repeated=true;
        if(repeated) break;
        seen[index]=package;
        const uintptr_t descriptor=Read<uintptr_t>(package+0x40),handler=Read<uintptr_t>(descriptor+0x14);
        const uintptr_t vtable=Read<uintptr_t>(handler);
        char name[64]; Name(Read<uintptr_t>(package+0xC),name);
        std::snprintf(line,sizeof(line),
            "[RESULT_PACKAGE] %s p=%08X bound=%08X states=%u/%u/%u/%u flags=%u/%u focus=%08X h=%08X vt=%08X event=%08X owner=%08X hFlags=%u/%u.\r\n",
            name,static_cast<unsigned>(package),static_cast<unsigned>(Read<uintptr_t>(descriptor+0x18)),
            Read<uint32_t>(package+0x1C),Read<uint32_t>(package+0x20),Read<uint32_t>(package+0x24),Read<uint32_t>(package+0x28),
            Read<uint8_t>(package+0x14),Read<uint8_t>(package+0x15),static_cast<unsigned>(Read<uintptr_t>(package+0x8C)),
            static_cast<unsigned>(handler),static_cast<unsigned>(vtable),static_cast<unsigned>(Read<uintptr_t>(vtable+8)),
            static_cast<unsigned>(Read<uintptr_t>(handler+0xC)),Read<uint8_t>(handler+0x10),Read<uint8_t>(handler+0x11));
        LogMessage(line); package=Read<uintptr_t>(package+4);
    }
}
static void Tick() {
    Context context{};
    if(!Active(context)) { gContextManager=0; return; }
    const DWORD now=GetTickCount();
    if(!gLastSnapshot || context.page!=gLastPage || now-gLastSnapshot>=5000) {
        gLastSnapshot=now; gLastPage=context.page; Snapshot("tick");
    }
}
static bool Interesting(uint32_t message) {
    switch(message) {
    case 0x406415E3: case 0x911AB364: case 0x34DC1BEC:
    case 0xC519BFC0: case 0x0C407210: case 0xE1FDE1D1:
    case 0x3411395C: case 0x35E413DD: case 0x24DD40CF: case 0xD78D7BF1:
    case 0xD05FC3A3: case 0xB5971BF1: case 0x9120409E: return true;
    default: break;
    }
    // Mirror50B2A0's immutable action hash lists without invoking a native
    // callback. Every mapped menu action remains traced after incidental
    // lifecycle messages exhaust the separate first-seen diagnostic budget.
    if(!message)return false;
    for(unsigned i=0;i<19;++i)if(Read<uint32_t>(kInputHashTable+i*4)==message ||
        Read<uint32_t>(kInputHashTable+0x58+i*4)==message)return true;
    for(unsigned i=0;i<2;++i)if(Read<uint32_t>(kInputHashTable+0x4C+i*4)==message)return true;
    return message==0x936A6A7F;
}
static bool ShouldTraceMessage(uint32_t message) {
    if(Interesting(message)) return true;
    for(unsigned index=0;index<gSeenCount;++index) if(gSeenMessages[index]==message) return false;
    if(gSeenCount==16) return false;
    gSeenMessages[gSeenCount++]=message; return true;
}
static void __fastcall Dispatch(void* object,void*,uint32_t message,uint32_t a,uint32_t b,uint32_t c) {
    Context context{};
    const bool trace=Active(context) && ShouldTraceMessage(message);
    char line[384];
    if(trace) {
        ++gDispatchEvents;
        const uintptr_t handler=reinterpret_cast<uintptr_t>(object);
        char name[64]; Name(Read<uintptr_t>(handler+4),name);
        std::snprintf(line,sizeof(line),
            "[RESULT_EVENT] before ms=%lu h=%08X(%s) msg=%08X args=%08X/%08X/%08X owner=%08X flags=%u/%u page=%08X.\r\n",
            GetTickCount(),static_cast<unsigned>(handler),name,message,a,b,c,
            static_cast<unsigned>(Read<uintptr_t>(handler+0xC)),Read<uint8_t>(handler+0x10),Read<uint8_t>(handler+0x11),context.page);
        LogMessage(line);
    }
    gDispatch(object,message,a,b,c); // Exactly one native call, even outside COOP.
    if(trace) {
        const uintptr_t manager=Read<uintptr_t>(kManager);
        std::snprintf(line,sizeof(line),"[RESULT_EVENT] after msg=%08X manager=%08X page=%08X.\r\n",
            message,static_cast<unsigned>(manager),static_cast<unsigned>(Read<uintptr_t>(manager+0x38)));
        LogMessage(line); if(Interesting(message)) Snapshot("event-after");
    }
}
static void __fastcall Advance(void* object,void*) {
    Context context{}; const bool trace=Active(context);
    if(trace) { ++gAdvanceEvents; Snapshot("advance-before"); }
    gAdvance(object); // Do not fabricate acceptance or bypass native result/save handling.
    if(trace) Snapshot("advance-after");
}
static void Window(UINT message,WPARAM key,LPARAM data) {
    if(message!=WM_KEYDOWN && message!=WM_KEYUP && message!=WM_SYSKEYDOWN && message!=WM_SYSKEYUP &&
        message!=WM_LBUTTONDOWN && message!=WM_LBUTTONUP && message!=WM_RBUTTONDOWN && message!=WM_RBUTTONUP &&
        message!=WM_MBUTTONDOWN && message!=WM_MBUTTONUP && message!=WM_MOUSEWHEEL &&
        message!=WM_ACTIVATE && message!=WM_ACTIVATEAPP && message!=WM_KILLFOCUS) return;
    Context context{};
    if(!Active(context)) return;
    ++gWindowEvents; char line[192];
    std::snprintf(line,sizeof(line),"[RESULT_WINDOW] ms=%lu msg=%04X w=%08X l=%08X focus=%u page=%08X.\r\n",
        GetTickCount(),message,static_cast<unsigned>(key),static_cast<unsigned>(data),IsGameWindowFocused()?1u:0u,context.page);
    LogMessage(line);
    if(message==WM_KEYDOWN || message==WM_SYSKEYDOWN || message==WM_LBUTTONDOWN ||
        message==WM_RBUTTONDOWN || message==WM_MBUTTONDOWN || message==WM_MOUSEWHEEL)InputAttempt("window-input");
}
static void KeyboardState(HRESULT hr,DWORD size,const void* data,bool focused) {
    Context context{}; if(!Active(context)) return;
    ++gStateCalls;
    uint32_t keys=0;
    if(SUCCEEDED(hr) && size==256 && data) {
        const auto* bytes=static_cast<const BYTE*>(data);
        keys=(bytes[0x1C]&0x80?1u:0u)|(bytes[0x9C]&0x80?2u:0u)|
             (bytes[0x39]&0x80?4u:0u)|(bytes[1]&0x80?8u:0u);
        for(unsigned i=0;i<256;++i)if(((gHaveRawState?gLastRawState[i]:0)^bytes[i])&0x80) {
            char line[160];std::snprintf(line,sizeof(line),"[RESULT_DI_STATE_CHANGE] ms=%lu rawOffset=%u down=%u focus=%u.\r\n",
                GetTickCount(),i,unsigned((bytes[i]&0x80)!=0),unsigned(focused));LogMessage(line);
        }
        std::memcpy(gLastRawState,bytes,256);gHaveRawState=true;
    }
    if(keys==gLastKeys && hr==gLastStateHr) return;
    ++gKeyboardEvents; gLastKeys=keys; gLastStateHr=hr; char line[192];
    std::snprintf(line,sizeof(line),"[RESULT_DI_STATE] ms=%lu hr=%08X bytes=%lu keys(enter/numpad/space/esc)=%X focus=%u.\r\n",
        GetTickCount(),static_cast<unsigned>(hr),size,keys,focused?1u:0u);
    LogMessage(line);
}
static void BufferStatus(HRESULT hr,DWORD count,DWORD flags,bool focused,bool filtered) {
    Context context{}; if(!Active(context)) return;
    ++gBufferCalls; if(filtered) ++gFilteredCalls;
    const DWORD now=GetTickCount();
    if(now-gLastBufferStatus<1000) return;
    gLastBufferStatus=now; char line[200];
    std::snprintf(line,sizeof(line),"[RESULT_DI_BUFFER] ms=%lu hr=%08X count=%lu flags=%lu focus=%u filtered=%u.\r\n",
        now,static_cast<unsigned>(hr),count,flags,focused?1u:0u,filtered?1u:0u);
    LogMessage(line);
}
static void BufferedEvent(DWORD offset,DWORD data,DWORD stamp,DWORD sequence,uintptr_t action) {
    Context context{}; if(!Active(context)) return;
    ++gKeyboardEvents; char line[208];
    std::snprintf(line,sizeof(line),"[RESULT_DI_EVENT] ms=%lu offset=%lu data=%08X stamp=%lu seq=%lu action=%08X.\r\n",
        GetTickCount(),offset,data,stamp,sequence,static_cast<unsigned>(action));
    LogMessage(line);
    if(data&0x80)InputAttempt("buffered-input");
}
static void __fastcall Widget(void* engine,void*,uintptr_t object,uintptr_t package,uint32_t message,uint32_t device) {
    Context context{};const bool trace=Active(context)&&Interesting(message);
    if(trace) {
        char line[288];std::snprintf(line,sizeof(line),
            "[RESULT_WIDGET] before ms=%lu engine=%08X object=%08X package=%08X msg=%08X device=%u objectType=%u events=%08X.\r\n",
            GetTickCount(),unsigned(reinterpret_cast<uintptr_t>(engine)),unsigned(object),unsigned(package),message,device,
            Read<uint32_t>(object+0x18),unsigned(Read<uintptr_t>(object+0x3C)));LogMessage(line);
        Events("actual-widget",Read<uintptr_t>(object+0x3C));
    }
    gWidget(engine,object,package,message,device);
    if(trace){char line[192];std::snprintf(line,sizeof(line),"[RESULT_WIDGET] after msg=%08X parentEvents=%u advances=%u page=%08X.\r\n",
        message,gDispatchEvents,gAdvanceEvents,Read<uint32_t>(Read<uintptr_t>(kManager)+0x38));LogMessage(line);}
}
struct Detour { uintptr_t address; size_t length; BYTE expected[8]; void* hook; BYTE* trampoline=nullptr; bool installed=false; };
static Detour gDetours[]{
    {0x55B5C0,8,{0xA0,0x21,0x85,0x83,0x00,0x83,0xEC,0x34},reinterpret_cast<void*>(&Dispatch)},
    {0x4BFF80,6,{0x8B,0x41,0x38,0x39,0x41,0x34},reinterpret_cast<void*>(&Advance)},
    {0x5367E0,5,{0x53,0x8B,0x5C,0x24,0x10},reinterpret_cast<void*>(&Widget)}
};
static void Jump(BYTE* at,const void* destination) {
    const int32_t relative=static_cast<int32_t>(reinterpret_cast<intptr_t>(destination)-reinterpret_cast<intptr_t>(at+5));
    at[0]=0xE9; std::memcpy(at+1,&relative,4);
}
static void Publish(unsigned index,uintptr_t target) {
    if(index==0) gDispatch=reinterpret_cast<DispatchFn>(target);
    else if(index==1)gAdvance=reinterpret_cast<AdvanceFn>(target);
    else gWidget=reinterpret_cast<WidgetFn>(target);
}
static bool Rollback(unsigned index) {
    auto& detour=gDetours[index];
    if(detour.installed) {
        DWORD old=0,ignored=0;
        if(!VirtualProtect(reinterpret_cast<void*>(detour.address),detour.length,PAGE_EXECUTE_READWRITE,&old)) return false;
        std::memcpy(reinterpret_cast<void*>(detour.address),detour.expected,detour.length);
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(detour.address),detour.length);
        VirtualProtect(reinterpret_cast<void*>(detour.address),detour.length,old,&ignored);
        detour.installed=false;
    }
    Publish(index,detour.address);
    if(detour.trampoline) VirtualFree(detour.trampoline,0,MEM_RELEASE);
    detour.trampoline=nullptr;
    return true;
}
static bool Install() {
    if(gInstalled) return true;
    for(const auto& detour:gDetours) {
        BYTE actual[8]{};
        if(!ReadGameMemory(detour.address,actual,detour.length) || std::memcmp(actual,detour.expected,detour.length)!=0) {
            char line[256]{};
            std::snprintf(line,sizeof(line),"[RESULT_TRACE] Native signature differs at %08X; actual=%02X %02X %02X %02X %02X %02X %02X %02X; passive menu trace not installed (foreign patches are not replaced).\r\n",
                unsigned(detour.address),actual[0],actual[1],actual[2],actual[3],actual[4],actual[5],actual[6],actual[7]);
            LogMessage(line);return false;
        }
    }
    for(unsigned index=0;index<std::size(gDetours);++index) {
        auto& detour=gDetours[index];
        detour.trampoline=static_cast<BYTE*>(VirtualAlloc(nullptr,32,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
        DWORD old=0,ignored=0;
        if(!detour.trampoline || !VirtualProtect(reinterpret_cast<void*>(detour.address),detour.length,PAGE_EXECUTE_READWRITE,&old)) {
            bool restored=true;
            for(unsigned undo=0;undo<std::size(gDetours);++undo) if(!Rollback(undo)) restored=false;
            LogMessage(restored ? "[RESULT_TRACE] Passive menu trace failed; native entries restored.\r\n" :
                "[RESULT_TRACE] Passive menu trace failed; rollback incomplete, forwarding trampoline retained.\r\n");
            return false;
        }
        // Verified complete instructions, no displaced relative call/branch.
        // Advance's CMP is replayed so its native JE observes the original flags.
        std::memcpy(detour.trampoline,detour.expected,detour.length);
        Jump(detour.trampoline+detour.length,reinterpret_cast<void*>(detour.address+detour.length));
        FlushInstructionCache(GetCurrentProcess(),detour.trampoline,detour.length+5);
        Publish(index,reinterpret_cast<uintptr_t>(detour.trampoline));
        Jump(reinterpret_cast<BYTE*>(detour.address),detour.hook);
        if(detour.length>5) std::memset(reinterpret_cast<BYTE*>(detour.address+5),0x90,detour.length-5);
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(detour.address),detour.length);
        VirtualProtect(reinterpret_cast<void*>(detour.address),detour.length,old,&ignored);
        detour.installed=true;
    }
    gInstalled=true;
    LogMessage("[RESULT_TRACE] Post-race input, widget commands, registry routing and page advances traced; no per-race input cap, no auto-continue or save changes.\r\n");
    return true;
}
} // namespace CoopResultsTrace
