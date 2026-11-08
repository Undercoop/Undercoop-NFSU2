// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.

namespace CoopGeometryVisualNative {
using KeyFn=uint32_t(__thiscall*)(void*,uint32_t,int,int);
using OwnerFn=uint32_t(__thiscall*)(void*);
static KeyFn key=reinterpret_cast<KeyFn>(0x60FF80);
static OwnerFn owner=reinterpret_cast<OwnerFn>(0x4FE110);
using ClearFn=void(__thiscall*)(void*);
using CreateFn=void(__thiscall*)(void*,void*);
static ClearFn clear=reinterpret_cast<ClearFn>(0x63A610);
static CreateFn create=reinterpret_cast<CreateFn>(0x63A5A0);
static bool supported=false;
struct Observed {
    uintptr_t car=0,ride=0,visual=0;
    uint32_t lastKey=0;unsigned repeats=0;
    DWORD next=0,firstSeen=0;
    bool settled=false;
};
static std::array<Observed,22> observed{};
static DWORD at=0;
static unsigned cursor=0;
static bool Install() {
    // This adapter uses the stock mesh-key ABI. Do not bypass an unknown mod's
    // alternative metadata layout or renderer entry.
    struct Signature {uintptr_t at;uint8_t data[8];size_t size;};
    const Signature signatures[]={
        {0x60FF80,{0x8B,0x54,0x24,0x0C,0x8B,0x44,0x24,0x08},8},
        {0x4FE110,{0x6A,0,0x68,0x66,0x3E,0xB0,0xEB},7},
        {0x634800,{0x55,0x8B,0xEC,0x83,0xE4,0xF0},6},
        {0x63A610,{0x56,0x8B,0xF1,0x8B,0x8E,0x54,0x05,0x00},8},
        {0x63A5A0,{0x6A,0xFF,0x68,0x24,0xA5,0x77,0x00},7}
    };
    for(const auto& s:signatures){uint8_t data[8]{};if(!ReadGameMemory(s.at,data,s.size)||std::memcmp(data,s.data,s.size))return false;}
    supported=true;return true;
}
static uint32_t FirstKey(uintptr_t part,unsigned piece) {
    uint16_t row=0;uint8_t mode=0,which=0;uintptr_t database=0,rows=0,owners=0;
    uint32_t hash=UINT32_MAX;uint8_t checked[36]{};
    if(!IsValidUserPtr(part)||!ReadGameMemory(part+0xC,&row,2)||row==0xFFFF ||
        !ReadGameMemory(part+6,&mode,1)||!ReadGameMemory(part+7,&which,1)||
        !ReadGameMemory(0x8A1CC8,&database,4)||!database||
        !ReadGameMemory(database+0x2C,&rows,4)||!rows||
        !ReadGameMemory(rows+row*36,checked,sizeof(checked)))return 0;
    if(mode==1) {
        if(!ReadGameMemory(0x8A1CB8,&owners,4)||!owners||!ReadGameMemory(owners+which*4,&hash,4))return 0;
    } else if(mode!=0) hash=owner(reinterpret_cast<void*>(part));
    return key(reinterpret_cast<void*>(rows+row*36),hash,static_cast<int>(piece),0);
}
static void Tick(DWORD flow) {
    if(!supported||!CoopGeometryHeadersNative::enabled||!CoopGeometryHeadersNative::active||
        flow!=6||!IsCoopGameplayActive()) {at=0;observed={};return;}
    const DWORD now=GetTickCount();
    if(at&&now-at<250)return;
    at=now;
    if(!NativeAppearanceQueueIdle()||gPeerAppearanceRefresh.pendingResource||gPeerModelPoseRestore.car)return;
    // One native car slot per tick, not a fleet-wide scan every rendered frame.
    const unsigned slot=cursor++%observed.size();uintptr_t car=0;
    if(!ReadGameMemory(kCarArray+slot*4,&car,4)||!IsValidUserPtr(car)){observed[slot]={};return;}
    auto& state=observed[slot];
    if(state.car==car&&state.next&&static_cast<int32_t>(now-state.next)<0)return;
    uintptr_t ride=0,param=0,visual=0,visualOwner=0,visualRide=0,aux=0;uint32_t type=0,mode=0;uint8_t active=0;
    if(!ReadGameMemory(car+0x10,&ride,4)||!IsValidUserPtr(ride)||
        !ReadGameMemory(car+0x14,&param,4)||!param||ride!=param+0x10||
        !ReadGameMemory(param+4,&type,4)||type>2|| // traffic has a different body/LOD representation
        !ReadGameMemory(car+0x28,&mode,4)||(mode!=1&&mode!=3)||
        !ReadGameMemory(car+0x550,&active,1)||!active||
        !ReadGameMemory(car+0x554,&visual,4)||!IsValidUserPtr(visual)||
        !ReadGameMemory(visual,&visualOwner,4)||visualOwner!=car||
        !ReadGameMemory(visual+4,&visualRide,4)||visualRide!=ride||
        !ReadGameMemory(car+0x958,&aux,4)||!IsValidUserPtr(aux))return;
    if(state.car!=car||state.ride!=ride||state.visual!=visual) {
        state={};state.car=car;state.ride=ride;state.visual=visual;state.firstSeen=now;
    }
    state.next=now+10000;
    // A healthy visual that later loses a detachable part may be intentional
    // crash/mod behavior. Repair only a newly observed incomplete visual, never
    // regenerate later damage or repeatedly rebuild a permanently invalid mod.
    if(state.settled||now-state.firstSeen>15000){state.settled=true;return;}
    uint32_t missingKey=0;unsigned missingSlot=0;
    bool pending=false;
    for(unsigned partSlot=0;partSlot<63;++partSlot) {
        if(partSlot>=35&&partSlot<=46)continue; // same exclusion as native634800
        uintptr_t part=0;uint8_t visibility=0;
        // Native638CFB/6395A7 and renderer62318C exclude zero visibility.
        // Preserve deliberately hidden/detached parts instead of regenerating
        // their unused mesh table simply because geometry is available.
        if(!ReadGameMemory(ride+0x838+partSlot,&visibility,1)||!visibility||
            !ReadGameMemory(ride+0x590+partSlot*4,&part,4)||!part)continue;
        for(unsigned piece=0;piece<2;++piece) {
            uintptr_t model=0,bound=0,data=0;
            if(!ReadGameMemory(visual+0x17A8+partSlot*32+piece*16,&model,4))continue;
            if(model&&ReadGameMemory(model+0xC,&bound,4)&&bound&&ReadGameMemory(bound+0x84,&data,4)&&data)continue;
            const uint32_t wanted=FirstKey(part,piece);
            if(!wanted)continue;
            const unsigned readiness=CoopGeometryHeadersNative::Probe(wanted);
            if(readiness==1){pending=true;state.next=now+1000;continue;}
            if(readiness==2){missingKey=wanted;missingSlot=partSlot;break;}
        }
        if(missingKey)break;
    }
    if(!missingKey){state.lastKey=0;state.repeats=0;state.settled=!pending;return;}
    if(state.lastKey!=missingKey){state.lastKey=missingKey;state.repeats=0;}
    if(state.repeats>=2)return;
    ++state.repeats;
    // Native634800 discards an unresolved LOD-A model. Loading its geometry
    // later cannot fix that null slot: rebuild once with the resident solid.
    // The original Car/Ride/physics/pose and installed part selection stay intact.
    clear(reinterpret_cast<void*>(car));
    create(reinterpret_cast<void*>(car),reinterpret_cast<void*>(ride));
    ReadGameMemory(car+0x554,&state.visual,4);
    state.next=now+1000;
    char text[192]{};std::snprintf(text,sizeof(text),
        "[GEOMETRY] Rebound incomplete native visual after mesh residency: car=%08X kind=%u slot=%u mesh=%08X attempt=%u.\r\n",
        static_cast<unsigned>(car),type,missingSlot,missingKey,state.repeats);LogMessage(text);
}
}
