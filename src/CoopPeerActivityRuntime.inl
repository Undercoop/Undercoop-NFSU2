// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

namespace CoopPeerActivity {
static SRWLOCK lock=SRWLOCK_INIT;
static coop_activity::State state;
static void Bind(const coop::SessionSnapshot& s){
    const bool valid=s.joined&&s.fresh&&s.peer.Valid()&&(s.role==coop::Role::Host||s.role==coop::Role::Client);
    state.Bind(valid?coop_network::Generation{s.sessionId,s.membershipNonce,s.ticket}:coop_network::Generation{},
        valid?(s.role==coop::Role::Host?1:2):0);
}
// This observer is game-thread only. A bounded walk uses the real current
// active package list, not the most recently pushed/cached screen name.
static coop_activity::Scene Screen(bool& known){
    using coop_activity::Scene;
    known=false;
    uintptr_t ui=0,engine=0,cursor=0,seen[64]{};size_t count=0;
    if(!ReadGameMemory(0x8384C4,&ui,4)||!IsValidUserPtr(ui)||
       !ReadGameMemory(ui+8,&engine,4)||!IsValidUserPtr(engine)||
       !ReadGameMemory(engine+0xE4,&cursor,4))return Scene::Unknown;
    bool menu=false;
    while(cursor){
        if(!IsValidUserPtr(cursor)||count==std::size(seen)||std::find(seen,seen+count,cursor)!=seen+count)return Scene::Unknown;
        seen[count++]=cursor;
        uintptr_t next=0,name=0;uint8_t header[0x16]{};char text[96]{};
        if(!ReadGameMemory(cursor,header,sizeof(header)))return Scene::Unknown;
        std::memcpy(&next,header+4,4);std::memcpy(&name,header+0xC,4);
        const uint8_t active=header[0x14],removing=header[0x15];if(active>1||removing>1)return Scene::Unknown;
        if(active&&!removing){
            if(!CoopMovieSurface::ReadName(name,text))return Scene::Unknown;
            const auto scene=coop_activity::Screen(text);
            if(scene!=Scene::Unknown){known=true;return scene;}
            // A confirmation may cover a meaningful parent screen. Look for
            // that parent, but never describe an unknown active menu as driving.
            if(!CoopMovieSurface::Passive(text)&&!coop_activity::Prefix(text,"Chyron")&&
               !coop_activity::Equal(text,"StyleAnimator.fng"))menu=true;
        }
        cursor=next;
    }
    known=true;return menu?Scene::Menu:Scene::Unknown;
}
static coop_activity::Scene ActiveShop(coop_activity::Scene screen){
    using coop_activity::Scene;
    if(!coop_activity::CustomizationScreen(screen))return Scene::Unknown;
    // Stock54CBED writes the ENTERED shop at Career+8734; 513910/528560
    // read that field. CurrentShop+8724/copy+870C are nearby event triggers
    // and can remain populated while driving. Read a location only while an
    // actual customization surface is active, never from that pointer alone.
    uintptr_t shop=0;std::uint8_t kind=0xFF;
    if(!ReadGameMemory(0x85AD40+0x8734,&shop,4)||!IsValidUserPtr(shop)||
        !ReadGameMemory(shop+0x50,&kind,1))return Scene::Unknown;
    return coop_activity::Shop(kind);
}
static coop_activity::Activity ObserveNative(){
    coop_activity::Observation o;
    if(!ReadGameMemory(kGameFlowState,&o.flow,4))return {};
    ReadGameMemory(0x89E7A0,&o.track,4);
    uint32_t movieActive=0;uintptr_t movie=0;
    ReadGameMemory(CoopCareerNative::gMovieGlobals.active,&movieActive,4);
    ReadGameMemory(CoopCareerNative::gMovieGlobals.player,&movie,4);
    o.movie=movieActive&&IsValidUserPtr(movie);
    if(o.flow==3||o.flow==6)o.screen=Screen(o.screenKnown);
    if(o.flow==6){
        o.shop=ActiveShop(o.screen);
        uintptr_t native=0;uint32_t type=0;
        if(ReadGameMemory(gRaceContextAddresses.nativeFlow,&native,4)&&IsValidUserPtr(native))ReadGameMemory(native,&o.raceFlow,4);
        if(o.track&&o.track!=4000&&ReadGameMemory(CoopPeerRaceNative::addresses.params+0x10,&type,4))
            o.raceType=static_cast<uint8_t>(CleanGameMode(type));
        o.countdown=gCoopRaceState==COOP_RACE_STARTING;
        if(o.track&&o.track!=4000&&CoopFinishRaceActive()){
            const auto race=RaceStateSnapshot();
            o.waiting=coop_race::ValidFinish(race.finish[race.host?0:1])&&!race.ResultReady();
        }
        o.special=CoopSpecialBusy();
        if(o.special&&CoopSpecialNative::state.Active()){
            const auto& special=CoopSpecialNative::state;
            o.timedSpecial=coop_special::Timed(special.Current().kind);
            o.arrived=(special.Own()&coop_special::Arrived)!=0;
            o.waiting=(special.Own()&coop_special::Done)!=0&&!(special.Phase()&coop_special::Complete);
        }
        // The Outrun ownership helper validates the current native generation;
        // a stale manager pointer alone never reports a running challenge.
        const auto& e=CoopFleetNative::engagement;
        o.outrun=e.car&&IsCoopFleetOutrunActive(e.car,e.human);
    }
    return coop_activity::Classify(o);
}
static void Observe(DWORD now){
    static DWORD sampledAt=0;static bool sampled=false;
    if(sampled&&coop_time::Elapsed(now,sampledAt)<250)return;
    sampled=true;sampledAt=now;
    const auto session=CoopSnapshot();
    // Do not inspect native UI when there is no teammate to publish it to.
    const auto value=session.joined&&session.fresh?ObserveNative():coop_activity::Activity{};
    AcquireSRWLockExclusive(&lock);Bind(session);state.Observe(value,now);ReleaseSRWLockExclusive(&lock);
}
static coop_activity::Activity Peer(const coop::SessionSnapshot& session,DWORD now){
    AcquireSRWLockExclusive(&lock);Bind(session);const auto value=state.Peer(now);ReleaseSRWLockExclusive(&lock);return value;
}
}
bool ReceiveCoopPeerActivityPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now){
    uint32_t magic=0;if(!data||bytes<4)return false;std::memcpy(&magic,data,4);
    if(magic!=coop_activity::kMagic)return false;
    const auto session=CoopSnapshot();
    if(bytes!=sizeof(coop_activity::Packet)||sender.sin_family!=AF_INET||!session.joined||!session.fresh||
       session.peer!=coop::Endpoint{sender.sin_addr.s_addr,ntohs(sender.sin_port)})return true;
    coop_activity::Packet packet{};std::memcpy(&packet,data,sizeof(packet));
    AcquireSRWLockExclusive(&CoopPeerActivity::lock);CoopPeerActivity::Bind(session);
    CoopPeerActivity::state.Receive(packet,now);ReleaseSRWLockExclusive(&CoopPeerActivity::lock);return true;
}
void TickCoopPeerActivityNetwork(DWORD now){
    const auto session=CoopSnapshot();coop_activity::Packet packet{};
    AcquireSRWLockExclusive(&CoopPeerActivity::lock);CoopPeerActivity::Bind(session);
    const bool send=CoopPeerActivity::state.Poll(now,packet);ReleaseSRWLockExclusive(&CoopPeerActivity::lock);
    if(send)SendMeasuredCoopDatagram(&packet,sizeof(packet),session.peer);
}
