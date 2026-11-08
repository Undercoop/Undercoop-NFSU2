// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
// Isolated production-adapter fixture; no game process is opened or executed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

static unsigned checks=0,clears=0,creates=0,keyCalls=0;
static DWORD clockNow=1000;
static bool coopActive=true,queueIdle=true,repairSucceeds=true;
static std::array<uintptr_t,22> cars{};
static uintptr_t kCarArray=reinterpret_cast<uintptr_t>(cars.data());
struct {uint32_t pendingResource=0;} gPeerAppearanceRefresh;
struct {uintptr_t car=0;} gPeerModelPoseRestore;
static std::map<uint32_t,unsigned> readiness;
static unsigned char database[0x40]{},rows[72]{},part[16]{},nativeOwner[0x100*4]{};
static unsigned char car[0x960]{},parameter[0x940]{},visual[0x2EC0]{},model[24]{},solid[0x90]{},auxiliary[0x170]{};
static std::map<uintptr_t,std::vector<uint8_t>> signatures;
static uint32_t wanted[2]={0,0};
static void Check(bool condition,const char* name){++checks;if(!condition){std::fprintf(stderr,"FAIL: %s\n",name);std::exit(1);}}
static bool IsCoopGameplayActive(){return coopActive;}
static bool NativeAppearanceQueueIdle(){return queueIdle;}
static bool IsValidUserPtr(uintptr_t pointer){return pointer>=0x10000&&pointer<0xFFF00000;}
static void LogMessage(const char*){}
static DWORD FixtureTick(){return clockNow;}
static bool ReadGameMemory(uintptr_t from,void* to,size_t bytes){
    if(from==0x8A1CC8&&bytes==4){const uintptr_t p=reinterpret_cast<uintptr_t>(database);std::memcpy(to,&p,4);return true;}
    if(from==0x8A1CB8&&bytes==4){const uintptr_t p=reinterpret_cast<uintptr_t>(nativeOwner);std::memcpy(to,&p,4);return true;}
    auto signature=signatures.find(from);
    if(signature!=signatures.end()){if(signature->second.size()<bytes)return false;std::memcpy(to,signature->second.data(),bytes);return true;}
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(reinterpret_cast<void*>(from),&region,sizeof(region))||region.State!=MEM_COMMIT||
       (region.Protect&(PAGE_GUARD|PAGE_NOACCESS))||from+bytes<from||
       from+bytes>reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize)return false;
    std::memcpy(to,reinterpret_cast<void*>(from),bytes);return true;
}
namespace CoopGeometryHeadersNative {
static bool enabled=true,active=true;
static unsigned Probe(uint32_t key){const auto i=readiness.find(key);return i==readiness.end()?0:i->second;}
}
#define GetTickCount FixtureTick
#include "../src/CoopGeometryVisualNative.inl"
#undef GetTickCount

template<class T> static void Put(unsigned char* bytes,size_t offset,T value){std::memcpy(bytes+offset,&value,sizeof(value));}
static uint32_t __fastcall FakeKey(void* row,void*,uint32_t owner,int piece,int lod){
    Check(row==rows&&owner==UINT32_MAX&&lod==0&&piece>=0&&piece<2,"mesh key uses verified row, owner, piece and base LOD");
    ++keyCalls;return wanted[piece];
}
static uint32_t __fastcall FakeOwner(void*,void*){return UINT32_MAX;}
static void __fastcall FakeClear(void* target,void*){Check(target==car,"visual clear targets the validated native car");++clears;}
static void __fastcall FakeCreate(void* target,void*,void* ride){
    Check(target==car&&ride==parameter+0x10,"native create retains the existing Car and RideInfo");++creates;
    if(repairSucceeds){
        for(unsigned slot=0;slot<63;++slot)if(*reinterpret_cast<uintptr_t*>(parameter+0x10+0x590+slot*4))
            for(unsigned piece=0;piece<2;++piece)if(wanted[piece]&&CoopGeometryHeadersNative::Probe(wanted[piece])==2)
                Put(visual,0x17A8+slot*32+piece*16,reinterpret_cast<uintptr_t>(model));
    }
}
static void Base(unsigned slot=29,uint32_t type=1){
    using namespace CoopGeometryVisualNative;
    std::memset(car,0,sizeof(car));std::memset(parameter,0,sizeof(parameter));std::memset(visual,0,sizeof(visual));
    std::memset(part,0,sizeof(part));std::memset(model,0,sizeof(model));std::memset(solid,0,sizeof(solid));
    cars={};cars[0]=reinterpret_cast<uintptr_t>(car);readiness.clear();wanted[0]=11;wanted[1]=0;
    Put(database,0x2C,reinterpret_cast<uintptr_t>(rows));
    Put(car,0x10,reinterpret_cast<uintptr_t>(parameter+0x10));Put(car,0x14,reinterpret_cast<uintptr_t>(parameter));
    Put(parameter,4,type);Put(car,0x28,uint32_t(1));car[0x550]=1;
    Put(car,0x554,reinterpret_cast<uintptr_t>(visual));Put(car,0x958,reinterpret_cast<uintptr_t>(auxiliary));
    Put(visual,0,reinterpret_cast<uintptr_t>(car));Put(visual,4,reinterpret_cast<uintptr_t>(parameter+0x10));
    Put(parameter,0x10+0x590+slot*4,reinterpret_cast<uintptr_t>(part));
    parameter[0x10+0x838+slot]=1;
    Put(model,0xC,reinterpret_cast<uintptr_t>(solid));Put(solid,0x84,uintptr_t(0x1234));
    readiness[11]=2;
    at=0;cursor=0;observed={};clockNow=1000;
    clears=creates=keyCalls=0;coopActive=queueIdle=repairSucceeds=true;
    CoopGeometryHeadersNative::enabled=CoopGeometryHeadersNative::active=supported=true;
    gPeerAppearanceRefresh={};gPeerModelPoseRestore={};
    key=reinterpret_cast<KeyFn>(FakeKey);owner=reinterpret_cast<OwnerFn>(FakeOwner);
    clear=reinterpret_cast<ClearFn>(FakeClear);create=reinterpret_cast<CreateFn>(FakeCreate);
}
static void InspectAt(DWORD now){clockNow=now;CoopGeometryVisualNative::cursor=0;CoopGeometryVisualNative::Tick(6);}
int main(){
    using namespace CoopGeometryVisualNative;
    Check(sizeof(void*)==4,"fixture targets x86");
    signatures={
        {0x60FF80,{0x8B,0x54,0x24,0x0C,0x8B,0x44,0x24,0x08}},
        {0x4FE110,{0x6A,0,0x68,0x66,0x3E,0xB0,0xEB}},
        {0x634800,{0x55,0x8B,0xEC,0x83,0xE4,0xF0}},
        {0x63A610,{0x56,0x8B,0xF1,0x8B,0x8E,0x54,0x05,0x00}},
        {0x63A5A0,{0x6A,0xFF,0x68,0x24,0xA5,0x77,0x00}}
    };
    Check(Install()&&supported,"verified native key/owner/visual entries enable adapter");
    signatures[0x63A5A0][0]=0xE9;supported=false;
    Check(!Install()&&!supported,"unknown modded create-visual entry does not enable native ABI calls");
    Base();Tick(6);
    Check(clears==1&&creates==1,"missing base-LOD mesh is rebound after actual residency");
    InspectAt(2200);Check(creates==1&&observed[0].settled,"successful visual becomes settled after native rebind");
    Put(visual,0x17A8+29*32,uintptr_t(0));InspectAt(13000);
    Check(creates==1,"later intentional damage on a settled visual is not reconstructed");

    Base();wanted[0]=0;wanted[1]=22;readiness[22]=2;Tick(6);
    Check(creates==1,"second mesh piece is repaired independently of absent first piece");
    Base();readiness[11]=1;Tick(6);
    Check(!creates&&!observed[0].settled,"indexed nonresident mesh waits without destroying visual");
    readiness[11]=2;InspectAt(2300);Check(creates==1,"pending initial mesh repairs when drawable arrives");
    Base();wanted[0]=0;Tick(6);Check(!creates&&observed[0].settled,"optional empty base key does not force a rebuild");
    Base();readiness[11]=1;Tick(6);readiness[11]=2;InspectAt(17000);
    Check(!creates&&observed[0].settled,"initial repair expires after bounded residency window");
    Base();repairSucceeds=false;Tick(6);InspectAt(2300);InspectAt(3600);
    Check(creates==2,"per-visual failed repair budget cannot create an infinite reconstruction loop");
    Base(35);Tick(6);Check(!creates&&!keyCalls,"native excluded part slots are never repaired");
    Base();parameter[0x10+0x838+29]=0;Tick(6);Check(!creates&&!keyCalls,"deliberately hidden part is not regenerated");
    Base(29,3);Tick(6);Check(!creates&&!keyCalls,"traffic simplified visuals retain native representation");
    Base();Put(visual,0,uintptr_t(0));Tick(6);Check(!creates,"wrong visual owner prevents native reconstruction");
    Base();coopActive=false;Tick(6);Check(!creates&&!observed[0].car,"singleplayer never enters repair path");
    Base();gPeerAppearanceRefresh.pendingResource=1;Tick(6);Check(!creates,"staged peer refresh owns visual while pending");
    Base();queueIdle=false;Tick(6);Check(!creates,"native queue busy never reconstructs a visual");
    Base();Tick(6);Tick(4);Check(!observed[0].car,"world transition clears reused native car observations");
    std::printf("Geometry visual fixture: %u checks passed; no game process used.\n",checks);
}
