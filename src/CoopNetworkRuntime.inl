// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Value-only network dispatch. This file must not read/write native engine
// memory, deserialize a profile, create a vehicle, or consume disk car caches.
// Every recognized outer packet owns a distinct identity. A receiver consumes
// its magic even when the remaining packet is malformed, so a collision drops
// another protocol before its receiver can ever see it. Persisted manifests
// and embedded payload signatures are deliberately not outer wire identities.
static constexpr bool CoopApplicationPacketIdsUnique(){
    constexpr uint32_t ids[]{coop::kSessionMagic,coop_checkpoint::kMagic,coop_mods::kMagic,
        coop_pause::kMagic,kRaceEventMagic,coop_race::kMagic,coop_url::kMagic,coop_chat::kMagic,
        coop_movie::kMagic,coop_sponsor::kMagic,coop_visual::kMagic,coop_special::kMagic,
        coop_special_recovery::kMagic,coop_world_impact::kMagic,CoopAIRosterNative::kMagic,
        CoopFleetAssetsNative::kMagic,coop_world_wire::kEventMagic,coop_world_wire::kLeaseMagic,
        coop_fleet_load::kMagic,kPacketMagic,kCarCustomizationMagic,coop_transport::kMagic,
        kBeaconMagic,kJoinMagic,kAckMagic};
    for(size_t i=0;i<std::size(ids);++i){
        if(!ids[i])return false;
        for(size_t j=i+1;j<std::size(ids);++j)if(ids[i]==ids[j])return false;
    }return true;
}
static_assert(CoopApplicationPacketIdsUnique(),"Each outer CO-OP packet must have a unique dispatcher identity");
static std::atomic<uint64_t> gCoopRawGameplayRejected{0},gCoopHumanRejected{0},gCoopHumanReceived{0};
static bool ReceiveCoopHumanPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now){
    uint32_t magic=0;if(!data||bytes<4)return false;std::memcpy(&magic,data,4);
    if(magic!=kPacketMagic&&magic!=kCarCustomizationMagic)return false;
    const auto session=CoopSnapshot();
    if(!IsCoopGameplayActive()||!session.joined||!session.fresh||sender.sin_family!=AF_INET||
        session.peer!=coop::Endpoint{sender.sin_addr.s_addr,ntohs(sender.sin_port)}){++gCoopHumanRejected;return true;}
    const coop_network::Generation live{session.sessionId,session.membershipNonce,session.ticket};
    const uint32_t peerIndex=session.role==coop::Role::Host?2:1;
    bool accepted=false;
    if(magic==kPacketMagic&&bytes==sizeof(PositionPacket)){
        PositionPacket packet{};std::memcpy(&packet,data,sizeof(packet));
        if(IsValidPacket(packet)&&packet.membershipNonce==live.membership&&packet.ticket==live.ticket&&packet.sessionId==live.session)
            accepted=UpdateRemotePlayerState(packet,true);
    }else if(magic==kPacketMagic&&bytes==sizeof(CarSyncPacket)){
        CarSyncPacket packet{};std::memcpy(&packet,data,sizeof(packet));uint32_t model=UINT32_MAX;
        std::memcpy(&model,packet.vehicleParam+0x10,sizeof(model));
        if(packet.version==kPacketVersion&&packet.size==sizeof(packet)&&packet.playerIndex==peerIndex&&
            packet.selectionStage<=2&&coop_car_catalog::PortableModelIndex(model)){
            AcquireSRWLockExclusive(&gPeerSyncLock);
            if(gPeerSelectionSerial.Accept(AppearanceGeneration(packet),live,packet.sequence)){
                gLatestPeerSyncPkt=packet;gPeerSyncReceivedAtMs=now;gHavePeerSyncPkt=true;accepted=true;
            }
            ReleaseSRWLockExclusive(&gPeerSyncLock);
        }
    }else if(magic==kCarCustomizationMagic&&bytes==sizeof(CarCustomizationPacket)){
        CarCustomizationPacket packet{};std::memcpy(&packet,data,sizeof(packet));
        if(packet.version==kPacketVersion&&packet.size==sizeof(packet)&&packet.playerIndex==peerIndex&&
            coop_car_catalog::PortableModelIndex(packet.carModelID)){
            AcquireSRWLockExclusive(&gPeerCarLock);
            if(gPeerCarSerial.Accept(AppearanceGeneration(packet),live,packet.sequence)){
                gLatestPeerCar=packet;gHavePeerCar=true;accepted=true;
            }
            ReleaseSRWLockExclusive(&gPeerCarLock);
        }
    }
    if(accepted)++gCoopHumanReceived;else ++gCoopHumanRejected;
    return true;
}
static void DispatchCoopApplicationPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now){
    if(ReceiveCoopSessionPacket(data,bytes,sender,now))return;
    if(ReceiveCoopModPacket(data,bytes,sender,now))return;
    const auto admission=CoopSnapshot();
    if(!admission.initialBarrierComplete&&!CoopModsReady(admission))return;
    if(ReceiveCoopRetiredPausePacket(data,bytes))return;
    if(ReceiveCoopRacePacket(data,bytes,sender,now))return;
    if(ReceiveCoopUrlPacket(data,bytes,sender,now))return;
    if(ReceiveCoopChatPacket(data,bytes,sender,now))return;
    if(ReceiveCoopMoviePacket(data,bytes,sender,now))return;
    if(ReceiveCoopSponsorPacket(data,bytes,sender,now))return;
    if(ReceiveCoopVisualPacket(data,bytes,sender,now))return;
    if(ReceiveCoopSpecialPacket(data,bytes,sender,now))return;
    if(ReceiveCoopSpecialRecoveryPacket(data,bytes,sender,now))return;
    if(ReceiveCoopWorldImpactPacket(data,bytes,sender,now))return;
    if(ReceiveCoopAIRosterPacket(data,bytes,sender,now))return;
    if(gCoopFleetEnabled&&ReceiveCoopFleetAssetsPacket(data,bytes,sender,now))return;
    if(gCoopFleetEnabled&&sender.sin_family==AF_INET&&ReceiveCoopFleetWorldPacket(data,static_cast<size_t>(bytes),
        coop::Endpoint{sender.sin_addr.s_addr,ntohs(sender.sin_port)},now))return;
    if(ReceiveCoopAIWorldPacket(data,bytes,sender,now))return;
    ReceiveCoopHumanPacket(data,bytes,sender,now);
}
static void TraceCoopNetworkState(DWORD now){
    static DWORD last=0;static bool haveLast=false;static bool wasReady=false;
    static coop_network::Generation lastGeneration{};
    static bool recovering=false;
    const auto session=CoopSnapshot();const auto wire=gCoopTransport.Snapshot();
    const bool ready=CoopConnectedTransportReady(session);
    const coop_network::Generation generation{session.sessionId,session.membershipNonce,session.ticket};
    if (generation != lastGeneration || session.phase != coop::Phase::Playing) {
        recovering = false;
    } else if (session.joined && session.fresh && wasReady && !ready) {
        recovering = true;
        CoopAnnouncements::Post("Connection interrupted. Reconnecting to your teammate...");
    } else if (recovering && ready) {
        recovering = false;
        CoopAnnouncements::Post("Teammate connection restored.");
    }
    lastGeneration = generation;
    if(haveLast&&ready==wasReady&&coop_time::Elapsed(now,last)<5000)return;
    haveLast=true;last=now;wasReady=ready;
    char ip[32]{};const auto address=SessionAddress(session.peer);inet_ntop(AF_INET,&address.sin_addr,ip,sizeof(ip));
    char line[1900]{};
    std::snprintf(line,sizeof(line),
        "[NETWORK613] role=%u session=%llu membership=%u ticket=%u phase=%u flow=%u peer=%s:%u transport_port=%u joined=%u transport_ready=%u rtt_ms=%u jitter_ms=%u loss_ppm=%u queued_control=%zu queued_state=%zu queued_bulk=%zu enqueued=%llu submitted=%llu wire_tx_bytes=%llu wire_tx_datagrams=%llu wire_rx_bytes=%llu wire_rx_datagrams=%llu app_received=%llu human_accepted=%llu human_rejected=%llu raw_gameplay_rejected=%llu queue_rejected=%llu expired=%llu invalid=%llu transport_generation=%llu retired=%llu receive_overflow=%llu"
        " peer_age_ms=%u reliable_in_flight=%zu queued_receive=%zu connects=%llu disconnects=%llu; queue acceptance is not peer application/commit.\r\n",
        unsigned(session.role),static_cast<unsigned long long>(session.sessionId),session.membershipNonce,session.ticket,unsigned(session.phase),gPublishedGameFlow.load(),
        ip,session.peer.port,unsigned(session.peer.port)+coop_transport::kPortOffset,unsigned(session.joined),unsigned(ready),wire.rttMs,wire.rttVarianceMs,wire.lossPartsPerMillion,
        wire.queuedControl,wire.queuedState,wire.queuedBulk,static_cast<unsigned long long>(wire.enqueued),static_cast<unsigned long long>(wire.submitted),
        static_cast<unsigned long long>(wire.socketTxBytes),static_cast<unsigned long long>(wire.socketTxDatagrams),static_cast<unsigned long long>(wire.socketRxBytes),
        static_cast<unsigned long long>(wire.socketRxDatagrams),static_cast<unsigned long long>(wire.received),static_cast<unsigned long long>(gCoopHumanReceived.load()),
        static_cast<unsigned long long>(gCoopHumanRejected.load()),static_cast<unsigned long long>(gCoopRawGameplayRejected.load()),static_cast<unsigned long long>(wire.rejected),
        static_cast<unsigned long long>(wire.expired),static_cast<unsigned long long>(wire.invalid),
        static_cast<unsigned long long>(wire.generation),static_cast<unsigned long long>(wire.retired),static_cast<unsigned long long>(wire.receiveOverflow),
        session.peerAgeMs, wire.reliableInFlightBytes, wire.queuedReceive,
        static_cast<unsigned long long>(wire.connects), static_cast<unsigned long long>(wire.disconnects));
    LogMessage(line);
    if(gScriptsDirectory[0]){
        char path[MAX_PATH]{};std::snprintf(path,sizeof(path),"%sNFSU2Coop-Network-P%u.log",gScriptsDirectory,gIsSecondInstance?2u:1u);
        HANDLE file=CreateFileA(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file!=INVALID_HANDLE_VALUE){DWORD written=0;WriteFile(file,line,DWORD(std::strlen(line)),&written,nullptr);CloseHandle(file);}
    }
}
static void TickCoopConnectedTransport(DWORD now){
    const auto session=CoopSnapshot();const bool active=session.joined&&session.fresh;
    gCoopTransport.SetPeer(active?session.sessionId:0,active?session.membershipNonce:0,active?session.peer:coop::Endpoint{},session.role==coop::Role::Host,now);
    gCoopTransport.Poll(now);
    static coop_network::Health health;
    if(health.TimedOut(session,CoopConnectedTransportReady(session),now)){
        bool closed=false;
        AcquireSRWLockExclusive(&gSessionLock);
        const auto live=gSession.Snapshot(GetTickCount());
        if(live.joined&&live.sessionId==session.sessionId&&live.membershipNonce==session.membershipNonce&&live.ticket==session.ticket){
            // Leave produces an action; the game thread cancels its native
            // loading wait. Never tear down native game objects here.
            gSession.Leave(GetTickCount());closed=true;
        }
        ReleaseSRWLockExclusive(&gSessionLock);
        if(closed){
            gCoopTransport.SetPeer(0,0,{},false,now);
            LogMessage("[NETWORK613] Connected gameplay unavailable for 15 seconds; partial session closed, pending native waits cancelled on game thread.\r\n");
            CoopAnnouncements::Post("Gameplay connection did not recover. CO-OP disconnected; return to the main menu to reconnect.");
        }
    }
}
static void DrainCoopConnectedTransport(DWORD now){
    char bytes[coop_transport::kMaxPayloadBytes]{};coop::Endpoint sender{};
    for(unsigned i=0;i<256;++i){const int count=gCoopTransport.Receive(bytes,sizeof(bytes),sender);if(!count)break;if(count<0)continue;
        const auto address=SessionAddress(sender);DispatchCoopApplicationPacket(bytes,count,address,now);
    }
}
