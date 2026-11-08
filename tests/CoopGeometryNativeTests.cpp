// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
// An isolated x86 fixture for the production hooks. It never opens a game.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

static unsigned checks=0;
static void Check(bool value,const char* name) {
    ++checks;if(!value){std::fprintf(stderr,"FAIL: %s\n",name);std::exit(1);}
}
static bool coopActive=true;
static std::vector<std::string> logs;
static uintptr_t fixtureHeader=0;
static bool ReadGameMemory(uintptr_t from,void* to,size_t bytes) {
    if(from==0x82DBE0&&bytes==4){std::memcpy(to,&fixtureHeader,4);return true;}
    if(!bytes)return true;
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(from),&info,sizeof(info))||info.State!=MEM_COMMIT||
        (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))||from+bytes<from||
        from+bytes>reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize)return false;
    std::memcpy(to,reinterpret_cast<void*>(from),bytes);return true;
}
static void SafeWriteBytes(uintptr_t target,const void* bytes,size_t count){std::memcpy(reinterpret_cast<void*>(target),bytes,count);}
static void LogMessage(const char* text){logs.emplace_back(text);}
static bool IsCoopGameplayActive(){return coopActive;}
#include "../src/CoopGeometryHeadersNative.inl"

alignas(16) static unsigned char headerBytes[0xC0]{};
alignas(16) static unsigned char solidBytes[0x90]{};
static unsigned opens=0,nativeCloses=0,callbackCalls=0;
static uintptr_t callbackContext=0;
static const char path[]="CARS\\SKYLINE\\GEOMETRY.BIN";
static int32_t& References(){return *reinterpret_cast<int32_t*>(headerBytes+0xB4);}
static void DirtyFloatingState(){
    __asm {
        pxor xmm0,xmm0
        fldz
    }
}
static uintptr_t __fastcall FakeLookup(void*,void*,const char* name) {
    DirtyFloatingState();
    return References()>0&&coop_geometry::SamePath(path,name)?fixtureHeader:0;
}
static int __cdecl FakeOpen(const char* name,int,void*,void*) {
    DirtyFloatingState();
    if(!coop_geometry::SamePath(path,name)||References()<=0)return 0;
    ++opens;++References();return 1;
}
static uintptr_t __cdecl FakeSolid(uint32_t key,void*) {
    return key==2?reinterpret_cast<uintptr_t>(solidBytes):0;
}
static uintptr_t __fastcall FakeIndex(void*,void*,uint32_t key){return key==1||key==2?fixtureHeader:0;}
static void __cdecl Completed(void* context){++callbackCalls;callbackContext=reinterpret_cast<uintptr_t>(context);}
static int __cdecl HandleClose(uintptr_t owner,const char* name,void* callback,void* context) {
    Check(owner==0x82DBC8&&coop_geometry::SamePath(name,path),"native original receives manager and filename unchanged");
    Check(References()>0,"native close owns an existing reference");++nativeCloses;
    if(!--References()&&callback)reinterpret_cast<void(__cdecl*)(void*)>(callback)(context);
    return 1;
}
static uint32_t enteredEax=0,enteredEcx=0,enteredEdx=0,enteredEbx=0,enteredEsi=0,enteredEdi=0,enteredFlags=0;
alignas(16) static uint32_t enteredXmm[4]{};
static uint32_t pumpEax=0,pumpEcx=0,pumpEdx=0,pumpEbx=0,pumpEsi=0,pumpEdi=0,pumpFlags=0;
alignas(16) static uint32_t pumpXmm[4]{};
__declspec(naked) static void CloseTail() {
    __asm {
        mov enteredEax,eax
        mov enteredEcx,ecx
        mov enteredEdx,edx
        mov enteredEbx,ebx
        mov enteredEsi,esi
        mov enteredEdi,edi
        pushfd
        pop enteredFlags
        movdqa enteredXmm,xmm0
        push dword ptr [esp+16]
        push dword ptr [esp+16]
        push dword ptr [esp+16]
        push ecx
        call HandleClose
        add esp,16
        pop ebx
        ret 12
    }
}
__declspec(naked) static void PumpTail() {
    __asm {
        mov pumpEax,eax
        mov pumpEcx,ecx
        mov pumpEdx,edx
        mov pumpEbx,ebx
        mov pumpEsi,esi
        mov pumpEdi,edi
        pushfd
        pop pumpFlags
        movdqa pumpXmm,xmm0
        add esp,11Ch
        ret
    }
}
static uintptr_t closeEntry=0x493560,pumpEntry=0x63B160;
static uintptr_t callCallback=0,callContext=0xBEEF;
alignas(16) static uint32_t expectedXmm[4]={0x12345678,0x23456789,0x34567890,0x45678901};
static float afterFloat=0;
__declspec(naked) static void ExerciseClose() {
    __asm {
        push ebx
        push esi
        push edi
        movdqa xmm0,expectedXmm
        fld1
        mov eax,11223344h
        mov ecx,82DBC8h
        mov edx,55667788h
        mov ebx,12345678h
        mov esi,23456789h
        mov edi,34567890h
        stc
        push callContext
        push callCallback
        push offset path
        call dword ptr [closeEntry]
        fstp afterFloat
        pop edi
        pop esi
        pop ebx
        ret
    }
}
__declspec(naked) static void ExercisePump() {
    __asm {
        push ebx
        push esi
        push edi
        movdqa xmm0,expectedXmm
        fld1
        mov eax,11223344h
        mov ecx,8A3288h
        mov edx,55667788h
        mov ebx,12345678h
        mov esi,23456789h
        mov edi,34567890h
        stc
        call dword ptr [pumpEntry]
        fstp afterFloat
        pop edi
        pop esi
        pop ebx
        ret
    }
}
static void ResetHeader() {
    std::memset(headerBytes,0,sizeof(headerBytes));fixtureHeader=reinterpret_cast<uintptr_t>(headerBytes);
    *reinterpret_cast<uintptr_t*>(headerBytes)=0x82DBE0;
    std::memcpy(headerBytes+8,path,sizeof(path));References()=1;
}
int main() {
    using namespace CoopGeometryHeadersNative;
    Check(sizeof(void*)==4,"fixture is x86");
    // Exercise the exact stolen instructions and real production hooks without
    // depending on arbitrary low virtual pages being free under this OS/ASLR.
    void* closePage=VirtualAlloc(nullptr,0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    void* pumpPage=VirtualAlloc(nullptr,0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    Check(closePage&&pumpPage,"isolated native entry pages allocated");
    closeEntry=closeAddress=reinterpret_cast<uintptr_t>(closePage);
    pumpEntry=pumpAddress=reinterpret_cast<uintptr_t>(pumpPage);
    const uint8_t closeBytes[]={0x53,0x8B,0x5C,0x24,0x08};
    const uint8_t pumpBytes[]={0x81,0xEC,0x1C,0x01,0,0};
    std::memcpy(reinterpret_cast<void*>(closeEntry),closeBytes,5);std::memcpy(reinterpret_cast<void*>(pumpEntry),pumpBytes,6);
    *reinterpret_cast<uint8_t*>(closeEntry)=0xCC;
    Check(!Install()&&!enabled,"unsupported existing hook disables geometry adapter");
    Check(*reinterpret_cast<uint8_t*>(closeEntry)==0xCC&&!std::memcmp(reinterpret_cast<void*>(pumpEntry),pumpBytes,6),"signature rejection overwrites neither foreign nor supported entry");
    std::memcpy(reinterpret_cast<void*>(closeEntry),closeBytes,5);
    Check(Install()&&enabled,"verified original native entries accept installation");
    Check(*reinterpret_cast<uint8_t*>(closeEntry)==0xE9&&*reinterpret_cast<uint8_t*>(pumpEntry)==0xE9,"production hooks installed at both verified entries");
    closeResume=reinterpret_cast<uintptr_t>(CloseTail);pumpResume=reinterpret_cast<uintptr_t>(PumpTail);
    lookup=reinterpret_cast<LookupFn>(FakeLookup);open=FakeOpen;solid=FakeSolid;index=reinterpret_cast<IndexFn>(FakeIndex);
    ResetHeader();Begin();ExerciseClose();
    Check(References()==1&&opens==1&&nativeCloses==1&&leases.Size()==1,"native Stage5 close retains exactly our owned index reference");
    Check(enteredEax==0x11223344&&enteredEcx==manager&&enteredEdx==0x55667788&&enteredEbx==reinterpret_cast<uintptr_t>(path)&&enteredEsi==0x23456789&&enteredEdi==0x34567890,"close entry restores registers before original instructions execute");
    Check((enteredFlags&1)&&!std::memcmp(enteredXmm,expectedXmm,16)&&afterFloat==1,"close entry preserves flags SIMD and x87 state across leasing calls");
    Release();Check(References()==0&&!leases.Size()&&nativeCloses==2,"cleanup calls native original exactly once for its own lease");

    ResetHeader();Begin();ExercisePump();
    Check(References()==2&&leases.Size()==1,"first native pump pins open car geometry index");
    Check(pumpEax==0x11223344&&pumpEcx==loader&&pumpEdx==0x55667788&&pumpEbx==0x12345678&&pumpEsi==0x23456789&&pumpEdi==0x34567890,"pump entry preserves all registers across index scan");
    Check(!std::memcmp(pumpXmm,expectedXmm,16)&&afterFloat==1,"pump entry preserves SIMD and x87 state");
    // Native sub ESP,11Ch deliberately changes arithmetic flags after hook restoration.
    callCallback=reinterpret_cast<uintptr_t>(Completed);ExerciseClose();callCallback=0;
    Check(References()==0&&!leases.Size()&&callbackCalls==1&&callbackContext==0xBEEF,"callback-bearing close drops our lease before original callback and preserves context");
    Release();

    ResetHeader();coopActive=false;active=true;ExerciseClose();
    Check(References()==0&&!leases.Size(),"co-op departure cannot pin a new singleplayer header before UI cleanup");
    ResetHeader();ExercisePump();
    Check(References()==1&&!leases.Size(),"native pump remains unchanged outside co-op");
    active=false;coopActive=true;

    alignas(4) unsigned char node[0x960]{},body[0x318]{},wheels[0x78]{};
    *reinterpret_cast<uintptr_t*>(node+0x954)=reinterpret_cast<uintptr_t>(body);
    *reinterpret_cast<uintptr_t*>(node+0x958)=reinterpret_cast<uintptr_t>(wheels);
    *reinterpret_cast<uint32_t*>(body+0x14)=2;*reinterpret_cast<uint32_t*>(body+0x18)=3;*reinterpret_cast<uint32_t*>(body+0x1C)=2;
    *reinterpret_cast<uintptr_t*>(solidBytes+0x84)=0x1234;active=true;
    coop_geometry::Residency residency{};
    Check(ResourceDrawable(reinterpret_cast<uintptr_t>(node),residency)&&residency.indexed==1&&residency.resident==1,"drawable gate permits absent optional LOD and present render data");
    *reinterpret_cast<uint32_t*>(wheels+0x18)=1;
    Check(!ResourceDrawable(reinterpret_cast<uintptr_t>(node),residency)&&residency.missing==1&&residency.firstMissing==1,"drawable gate blocks indexed pending wheel mesh");
    *reinterpret_cast<uint32_t*>(body+0x14)=193;
    Check(!ResourceDrawable(reinterpret_cast<uintptr_t>(node),residency),"malformed descriptor count cannot read beyond native array");
    active=false;
    VirtualFree(closePage,0,MEM_RELEASE);VirtualFree(pumpPage,0,MEM_RELEASE);
    std::printf("Geometry native fixture: %u checks passed; no game process used.\n",checks);
}
