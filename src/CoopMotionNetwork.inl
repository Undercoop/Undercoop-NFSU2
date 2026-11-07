// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Network thread only: consume the latest game-thread publication, never read
// native Car/Player objects here. Human motion goes before bulk Fleet traffic.
static bool SendLatestCoopHumanMotion(uint32_t& sequence,uint32_t& lastPublication,DWORD now) {
    const auto session=CoopSnapshot();PublishedMotion publication{};
    if(!IsCoopGameplayActive() || !session.joined || !session.fresh || !session.peer.Valid() ||
        !ReadPublishedMotion(publication,now) || publication.packet.sessionId!=session.sessionId ||
        publication.publication==lastPublication || gGameSocket==INVALID_SOCKET)return false;
    auto sent=publication.packet;sent.sequence=sequence+1;
    if(SendMeasuredCoopDatagram(&sent,sizeof(sent),session.peer)!=sizeof(sent))return false;
    sequence=sent.sequence;lastPublication=publication.publication;return true;
}
static void TraceCoopNetworkBudget(DWORD now) {
    static DWORD last=0;static uint64_t lastBytes=0,lastPackets=0;
    if(!last){last=now;lastBytes=gCoopTxBytes.load();lastPackets=gCoopTxPackets.load();return;}
    const DWORD elapsed=now-last;if(elapsed<5000)return;
    const uint64_t bytes=gCoopTxBytes.load(),packets=gCoopTxPackets.load();
    char line[224]{};
    std::snprintf(line,sizeof(line),"[NET_BUDGET] raw_membership_tx=%.1f KiB/s datagrams=%.1f/s send_errors=%llu last_socket_error=%u; connected gameplay wire/queues are reported in NETWORK613.\r\n",
        double(bytes-lastBytes)*1000.0/double(elapsed)/1024.0,double(packets-lastPackets)*1000.0/double(elapsed),
        static_cast<unsigned long long>(gCoopTxErrors.load()),gCoopTxLastError.load());
    LogMessage(line);last=now;lastBytes=bytes;lastPackets=packets;
}
