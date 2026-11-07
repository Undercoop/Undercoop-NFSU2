// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Game-thread-only own unique choice witness. No grant, replay, pointer
// persistence, selection mutation, game hook installation or save I/O here.
// Include after CoopSpecialCatalog.inl. The lifecycle must hold native Confirm
// before it reaches 561720 and freeze selection messages until commit/abort.
#include "CoopCarCatalog.h"
namespace CoopSpecialNative { namespace Choice {
static constexpr char kScreen[]="UI_ChooseUniquePart.fng";
static constexpr uint32_t kConfirm=0xD05FC3A3,kForfeit=0x34DC1BEC;
struct Addresses {
    uintptr_t customization=0x838980,garage=0x83AD90,selection=0x863480,
        uiManager=0x8384C4,carTable=0x8A1CCC,partManager=0x8A2B68,mode=0x850078;
};
static Addresses addresses;
using FindFn=void*(__cdecl*)(const char*);
using PartFn=void*(__thiscall*)(void*,uint32_t,uint32_t,uint32_t,void*,int);
static FindFn findHandler=reinterpret_cast<FindFn>(0x51BD10);
static PartFn resolvePart=reinterpret_cast<PartFn>(0x61B980);
// The native career garage has five 7F8-byte entries. Its slot ordinal and
// persistent handle/model hash, not a process pointer or ride buffer, identify
// the chosen personal car. A different same-model garage car remains distinct.
struct OwnCar {
    uint32_t slot=0,handle=0,modelHash=0,modelIndex=0,id=0;
};
struct Stable {
    OwnCar car{};uint32_t id=0,slot=0;coop_special::Kind kind=coop_special::Kind::Show;
};
struct Live {
    Stable stable{};uintptr_t handler=0,item=0,part=0,car=0;
};
// owner is an ephemeral witness only. Never serialize Frozen/Live. Durable
// recovery stores only Stable and obtains a fresh current native selector.
struct Frozen {bool valid=false;coop_special::Spec spec{};Stable stable{};uintptr_t owner=0;};
static bool Same(const OwnCar& a,const OwnCar& b){return a.slot==b.slot&&a.handle==b.handle&&
    a.modelHash==b.modelHash&&a.modelIndex==b.modelIndex&&a.id==b.id;}
static bool Same(const Stable& a,const Stable& b){return Same(a.car,b.car)&&a.id==b.id&&a.slot==b.slot&&a.kind==b.kind;}
static bool Valid(const Stable& choice){
    if(choice.car.slot>=5||!choice.car.handle||!choice.car.modelHash||!coop_car_catalog::PortableModelIndex(choice.car.modelIndex)||!choice.car.id||!choice.id)return false;
    return choice.kind==coop_special::Kind::Performance?choice.slot<10&&choice.id==choice.slot+1:
        choice.kind==coop_special::Kind::Visual&&choice.slot<170;
}
static bool String(uintptr_t at,const char* expected){
    if(!expected)return false;
    for(unsigned i=0;i<96;++i){char c=0;if(!Read(at+i,c)||c!=expected[i])return false;if(!c)return true;}
    return false;
}
static bool ReadGarageCar(OwnCar& out,uintptr_t* liveCar=nullptr){
    OwnCar next;uint32_t mode=0,count=0;uintptr_t car=0,table=0;
    if(!Read(addresses.mode,mode)||mode!=1||!Read(CoopSpecialNative::addresses.career+0x8740,next.handle)||!next.handle||
        !Read(addresses.garage+0x812C,count)||!count||count>5)return false;
    unsigned matches=0,models=0;
    for(unsigned i=0;i<count;++i){uint32_t handle=0;const uintptr_t entry=addresses.garage+0x5954+i*0x7F8;
        if(!Read(entry+8,handle))return false;if(handle!=next.handle)continue;
        if(!Read(entry+0x20,next.modelHash)||!next.modelHash)return false;
        ++matches;next.slot=i;car=entry;
    }
    if(matches!=1||!Read(addresses.carTable,table)||!IsValidUserPtr(table))return false;
    // Native 503950 derives the saved model hash from this car database name.
    // Preparation runs in free roam before customization has been bound, so
    // resolve the persistent hash against the validated installed car catalog.
    // Never depend on the previous shop's transient RideInfo/model pointer.
    for(uint32_t i=0;i<coop_car_catalog::PortableCount();++i){uint32_t index=~0u;char name[96]{};
        if(!Read(table+i*0x890+0x840,index)||index!=i||!Text(table+i*0x890,name)||std::strlen(name)>=coop_car_catalog::kWireNameBytes)return false;
        if(NativeHash(name)==next.modelHash){next.modelIndex=i;++models;}
    }
    if(models!=1)return false;
    uint32_t digest=2166136261u;Mix(digest,next.slot);Mix(digest,next.handle);Mix(digest,next.modelHash);
    next.id=digest?digest:1;out=next;if(liveCar)*liveCar=car;return true;
}
static bool ReadOwnCar(OwnCar& out,uintptr_t* liveCar=nullptr){
    OwnCar next;uintptr_t garageCar=0,customCar=0;uint32_t selected=0,player=~0u,index=~0u;
    if(!ReadGarageCar(next,&garageCar)||!Read(addresses.selection,selected)||selected!=next.handle||
        !Read(addresses.customization+0x30,player)||player!=0||
        !Read(addresses.customization+0x2C,customCar)||customCar!=garageCar||
        !Read(addresses.customization+0x940,index)||index!=next.modelIndex)return false;
    out=next;if(liveCar)*liveCar=garageCar;return true;
}
static bool OwnedScreen(uintptr_t expected,uintptr_t& handler,bool allowCompleted=false){
    handler=0;uintptr_t found=0;
    __try {found=reinterpret_cast<uintptr_t>(findHandler(kScreen));}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    if(!IsValidUserPtr(found)||(expected&&found!=expected))return false;
    uintptr_t vtable=0,name=0;uint8_t completed=0,modal=0;
    if(!Read(found,vtable)||vtable!=0x79CAB0||!Read(found+4,name)||!String(name,kScreen)||
        !Read(found+0x150,completed)||completed>1||(!allowCompleted&&completed)||!Read(found+0x152,modal)||modal||
        !::CoopSpecialNative::Feng::Owned(kScreen,found,true,addresses.uiManager))return false;
    handler=found;return true;
}
static bool OwnedSelectedItem(uintptr_t handler,uintptr_t& selected){
    uintptr_t widgetVtable=0,first=0,last=0;
    const uintptr_t sentinel=handler+0x50;
    if(!Read(handler+0x4C,widgetVtable)||widgetVtable!=0x79C0F4||
        !Read(sentinel,first)||!Read(sentinel+4,last)||!Read(handler+0x58,selected)||
        !IsValidUserPtr(selected)||!IsValidUserPtr(first)||!IsValidUserPtr(last))return false;
    unsigned hits=0,count=0;uintptr_t cursor=first,previous=sentinel;
    // Intrusive nodes are item+4, not item. Native 5389D0..538A01 and
    // 52F7C5..52F7E4 explicitly subtract four before using SelectablePart.
    while(cursor!=sentinel){
        if(++count>128||cursor<4||!IsValidUserPtr(cursor-4))return false;
        uintptr_t next=0,back=0,vtable=0;
        if(!Read(cursor,next)||!IsValidUserPtr(next)||!Read(cursor+4,back)||back!=previous||
            !Read(cursor-4,vtable)||vtable!=0x79B578)return false;
        if(cursor-4==selected)++hits;previous=cursor;cursor=next;
    }
    if(hits!=1||previous!=last)return false;
    uint8_t disabled=0,visible=0,locked=0,enabled=0,selectable=0;
    return Read(selected+0x20,disabled)&&!disabled&&Read(selected+0x21,visible)&&visible==1&&
        Read(selected+0x22,locked)&&!locked&&Read(selected+0x44,enabled)&&enabled==1&&
        Read(selected+0x45,selectable)&&selectable==1;
}
static bool NativePart(uint32_t model,uint32_t slot,uint32_t id,uintptr_t& part){
    part=0;
    __try {part=reinterpret_cast<uintptr_t>(resolvePart(reinterpret_cast<void*>(addresses.partManager),model,slot,id,nullptr,-1));}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    uint32_t actual=0;return IsValidUserPtr(part)&&Read(part,actual)&&actual==id;
}
static bool ReadChoice(const coop_special::Spec& spec,uintptr_t expectedHandler,Live& out){
    if(!coop_special::Valid(spec)||(spec.kind!=coop_special::Kind::Visual&&spec.kind!=coop_special::Kind::Performance))return false;
    const auto* catalog=FactsCatalog();const auto* record=catalog?Find(*catalog,spec.id):nullptr;
    uintptr_t mission=0;uint32_t missionState=0;
    if(!record||!coop_special::Same(record->spec,spec)||
        !Read(CoopSpecialNative::addresses.career+0xC08,mission)||mission!=record->pointer||
        // Native Arrival514FD0 establishes phase2; EnterShop54CC80 then
        // changes it to4 BEFORE constructing UI_ChooseUniquePart. Phase2 is
        // only the shop invitation, not a valid grant/customization context.
        !Read(CoopSpecialNative::addresses.career+0xD1C,missionState)||missionState!=4)return false;
    Live next;uint8_t performance=0;
    if(!OwnedScreen(expectedHandler,next.handler)||!Read(next.handler+0x151,performance)||performance>1||
        (performance!=0)!=(spec.kind==coop_special::Kind::Performance)||
        !OwnedSelectedItem(next.handler,next.item)||!ReadOwnCar(next.stable.car,&next.car)||
        !Read(next.item+0x48,next.stable.slot))return false;
    next.stable.kind=spec.kind;
    if(performance){
        // Native 502240 maps the ten package identifiers to indices 0..9.
        // Index zero is represented by a nonzero durable choice ID of one.
        if(next.stable.slot>=10)return false;next.stable.id=next.stable.slot+1;
    }else{
        uintptr_t selectedPart=0;
        if(next.stable.slot>=170||!Read(next.item+0x4C,selectedPart)||!IsValidUserPtr(selectedPart)||
            !Read(selectedPart,next.stable.id)||!next.stable.id||
            !NativePart(next.stable.car.modelIndex,next.stable.slot,next.stable.id,next.part)||next.part!=selectedPart)return false;
    }
    if(!Valid(next.stable))return false;out=next;return true;
}
static bool CaptureChoice(const coop_special::Spec& spec,uintptr_t handler,Frozen& out){
    Live live;if(!handler||!ReadChoice(spec,handler,live))return false;
    if(out.valid)return out.owner==handler&&coop_special::Same(out.spec,spec)&&Same(out.stable,live.stable);
    out.valid=true;out.spec=spec;out.stable=live.stable;out.owner=live.handler;return true;
}
static bool ValidateSameChoice(const Frozen& frozen,Live& out){
    if(!frozen.valid||!frozen.owner||!Valid(frozen.stable))return false;
    Live next;if(!ReadChoice(frozen.spec,frozen.owner,next)||!Same(next.stable,frozen.stable))return false;
    out=next;return true;
}
// Recovery never reuses a saved handler/part pointer. It requires the original
// mission to have been safely reopened and the actual fresh selector to show
// the exact stored own choice. Missing/different choices are held, not granted.
static bool ResolveSameChoice(const coop_special::Spec& spec,const Stable& stable,Live& out){
    if(!Valid(stable))return false;Live next;
    if(!ReadChoice(spec,0,next)||!Same(next.stable,stable))return false;out=next;return true;
}
}} // namespace CoopSpecialNative::Choice
