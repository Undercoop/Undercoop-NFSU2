// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Native event +0D is the stock SUV-only flag. Native527890 tests model+88A
// after resolving a garage model. Never grant a peer another car/garage slot.
namespace CoopSpecialNative { namespace Suv {
static uintptr_t modelTableAddress=0x8A1CCC;
static bool (*modelSupported)(uint32_t)=IsSupportedCarModel;
static bool (*ownCar)(Choice::OwnCar&,uintptr_t*)=Choice::ReadGarageCar;
static void* (*eventLookup)(uint32_t)=CoopPersonalSave::lookup;
static bool Model(uint32_t index,bool& suv){
    if(!modelSupported(index))return false;uintptr_t table=0;uint8_t flag=0;
    if(!Read(modelTableAddress,table)||!Read(table+index*0x890+0x88A,flag)||flag>1)return false;
    suv=flag!=0;return true;
}
static bool Both(uint32_t eventHash){
    if(!installed||!OwnCareer())return true;
    uintptr_t event=0;
    __try {event=uintptr_t(eventLookup(eventHash));}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    uint32_t hash=0;uint8_t required=0;
    if(!IsValidUserPtr(event)||!Read(event+8,hash)||hash!=eventHash||!Read(event+0x0D,required)||required>1)return false;
    if(!required)return true;
    const auto session=CoopSnapshot();Choice::OwnCar own;bool ownSuv=false,peerSuv=false;CarSyncPacket packet{};DWORD at=0;
    if(!session.joined||!session.fresh||!ownCar(own,nullptr)||!Model(own.modelIndex,ownSuv)||!ownSuv)return false;
    AcquireSRWLockShared(&gPeerSyncLock);const bool known=gHavePeerSyncPkt;packet=gLatestPeerSyncPkt;at=gPeerSyncReceivedAtMs;
    ReleaseSRWLockShared(&gPeerSyncLock);
    uint32_t model=~0u;std::memcpy(&model,packet.vehicleParam+0x10,4);
    return known&&IsReadyPeerSelection(packet,session.role==coop::Role::Host?2:1,at,runtimeNow())&&
        packet.selectionStage==2&&Model(model,peerSuv)&&peerSuv;
}
}}
bool CoopSpecialBothCarsForRace(uint32_t hash){return CoopSpecialNative::Suv::Both(hash);}
