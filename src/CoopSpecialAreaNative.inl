// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Game-thread-only admission adapters. These use the same XY polygon test as
// 5D9BF0, not an invented distance/radius or the interpolated remote visual.
namespace CoopSpecialNative {
using AreaFindFn=void*(__cdecl*)(uint32_t);
using AreaBoundsFn=int(__cdecl*)(const float*,const float*,const float*,float);
using AreaPolygonFn=bool(__cdecl*)(const float*,const float*,int);
using AreaOpportunityFn=bool(__thiscall*)(void*,void*);
struct AreaAddresses {
    uintptr_t localRoot=0x8900B4,localCar=0x89CCF8,flow=0x8654A4,track=0x89E7A0,mode=0x850078;
};
static AreaAddresses areaAddresses;
static AreaFindFn areaFind=reinterpret_cast<AreaFindFn>(0x4FFC50);
static AreaBoundsFn areaBounds=reinterpret_cast<AreaBoundsFn>(0x43D410);
static AreaPolygonFn areaPolygon=reinterpret_cast<AreaPolygonFn>(0x43D890);
static AreaOpportunityFn coverOpportunity=reinterpret_cast<AreaOpportunityFn>(0x501150);
static AreaOpportunityFn missionOpportunity=reinterpret_cast<AreaOpportunityFn>(0x5135A0);
static constexpr DWORD kStartPeerPositionMaxAgeMs=500;
static constexpr unsigned kMaximumAreaVertices=256;
static bool AreaCoordinate(float value){return std::isfinite(value)&&std::fabs(value)<1000000.0f;}
static uintptr_t FindArea(uint32_t hash){
    if(!hash)return 0;
    uintptr_t result=0;
    __try {result=reinterpret_cast<uintptr_t>(areaFind(hash));}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    uint32_t identity=0;
    return IsValidUserPtr(result)&&Read(result+0x30,identity)&&identity==hash?result:0;
}
static bool AreaContainsCall(const BYTE* row,const float* position,int count){
    __try {
        // 5D9C24..4F: zero tolerance; polygon returns AL, not arbitrary EAX.
        return areaBounds(reinterpret_cast<const float*>(row+0x20),
            reinterpret_cast<const float*>(row+0x28),position,0.0f)!=0&&
            areaPolygon(position,reinterpret_cast<const float*>(row+0x44),count);
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool AreaAtPoint(uint32_t hash,float x,float y){
    if(!AreaCoordinate(x)||!AreaCoordinate(y))return false;
    const uintptr_t area=FindArea(hash);if(!area)return false;
    alignas(float) std::array<BYTE,0x44+kMaximumAreaVertices*8> row{};
    if(!ReadSpan(area,row.data(),0x44))return false;
    int16_t count=0,stride=0;std::memcpy(&count,row.data()+0x40,2);std::memcpy(&stride,row.data()+0x42,2);
    const size_t bytes=0x44+size_t(count>0?count:0)*8;
    if(count<3||count>int(kMaximumAreaVertices)||stride<int(bytes)||stride>0x4000||
        !ReadSpan(area,row.data(),bytes))return false;
    float bounds[4]{};std::memcpy(bounds,row.data()+0x20,sizeof(bounds));
    for(float value:bounds)if(!AreaCoordinate(value))return false;
    if(bounds[0]>bounds[2]||bounds[1]>bounds[3])return false;
    for(unsigned i=0;i<unsigned(count);++i){
        float vertex[2]{};std::memcpy(vertex,row.data()+0x44+i*8,8);
        if(!AreaCoordinate(vertex[0])||!AreaCoordinate(vertex[1])||vertex[0]<bounds[0]||
            vertex[0]>bounds[2]||vertex[1]<bounds[1]||vertex[1]>bounds[3])return false;
    }
    // Copying the checked record prevents using unloaded/recycled area memory
    // between the AABB and polygon calls. Nothing is written to the engine.
    const float point[2]{x,y};return AreaContainsCall(row.data(),point,count);
}
static bool AreaScope(const coop_special::Spec& spec,coop::SessionSnapshot& session){
    if(!coop_special::Valid(spec)||!OwnCareer()||!IsCoopGameplayActive())return false;
    session=CoopSnapshot();uint32_t flow=0,mode=0,track=0,stage=0;
    if(!session.joined||!session.fresh||!session.initialBarrierComplete||!session.sessionId||!session.ticket||
        (session.role!=coop::Role::Host&&session.role!=coop::Role::Client)||
        !Read(areaAddresses.flow,flow)||flow!=6||!Read(areaAddresses.mode,mode)||mode!=1||
        !Read(areaAddresses.track,track)||track!=4000||!Read(addresses.career+0x7AF8,stage)||stage!=spec.stage)return false;
    const auto* catalog=FactsCatalog();const auto* record=catalog?Find(*catalog,spec.id):nullptr;
    return record&&coop_special::Same(record->spec,spec);
}
static bool ReadOwnAreaPosition(uintptr_t& car,float(&position)[3]){
    uintptr_t root=0,firstCar=0;
    return Read(areaAddresses.localRoot,root)&&IsValidUserPtr(root)&&Read(root+4,car)&&IsValidUserPtr(car)&&
        Read(areaAddresses.localCar,firstCar)&&firstCar==car&&ReadSpan(car+0x60,position,sizeof(position))&&
        AreaCoordinate(position[0])&&AreaCoordinate(position[1])&&AreaCoordinate(position[2]);
}
static bool OwnAtStart(const coop_special::Spec& spec){
    coop::SessionSnapshot session;uintptr_t car=0;float position[3]{};
    return AreaScope(spec,session)&&ReadOwnAreaPosition(car,position)&&AreaAtPoint(spec.start,position[0],position[1]);
}
static bool RawPeerAtStart(const coop_special::Spec& spec,DWORD now){
    coop::SessionSnapshot session;
    if(!AreaScope(spec,session)||state.Session()!=session.sessionId||state.Ticket()!=session.ticket||
        state.CurrentContext().campaign!=CampaignHash(CoopPersonalSave::campaign)||
        state.CurrentContext().stage!=spec.stage)return false;
    const auto* catalog=FactsCatalog();if(!catalog||state.CurrentContext().catalog!=catalog->digest)return false;
    PublishedMotion local{};uintptr_t car=0;float position[3]{};
    if(!ReadOwnAreaPosition(car,position)||!ReadPublishedMotion(local,now)||local.local.playerInstance!=car||
        local.packet.sessionId!=session.sessionId||local.packet.flowState!=6||local.packet.trackId!=4000||!local.packet.worldEpoch)return false;
    RemotePlayerState peer{};
    AcquireSRWLockShared(&gRemoteStateLock);peer=gRemoteState;ReleaseSRWLockShared(&gRemoteStateLock);
    // Sender clocks/epochs are not compared with this machine's. receivedAtMs
    // is our own accepted-packet time; current is raw, rendered is never used.
    return peer.valid&&peer.isFromPeer&&now-peer.receivedAtMs<=kStartPeerPositionMaxAgeMs&&
        peer.current.sequence&&peer.current.worldEpoch&&peer.current.sessionId==session.sessionId&&
        peer.current.flowState==6&&peer.current.trackId==4000&&AreaCoordinate(peer.current.z)&&
        AreaAtPoint(spec.start,peer.current.x,peer.current.y);
}
static bool OwnOpportunity(const coop_special::Spec& spec){
    coop::SessionSnapshot session;if(!AreaScope(spec,session))return false;
    const auto* catalog=FactsCatalog();const auto* row=catalog?Find(*catalog,spec.id):nullptr;if(!row)return false;
    __try {
        return coop_special::Timed(spec.kind)?missionOpportunity(reinterpret_cast<void*>(addresses.career+0x7148),
            reinterpret_cast<void*>(row->pointer)):coverOpportunity(reinterpret_cast<void*>(addresses.career+0x4210),
            reinterpret_cast<void*>(row->pointer));
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool OwnEligible(const Record& record){
    const auto* catalog=FactsCatalog();const auto* current=catalog?Find(*catalog,record.spec.id):nullptr;
    return current&&current->pointer==record.pointer&&coop_special::Same(current->spec,record.spec)&&OwnOpportunity(record.spec);
}
} // namespace CoopSpecialNative
