// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.
#pragma once
#include "CoopTransport.h"
#include "CoopTime.h"
#include "CoopPeerActivity.h"
#include <cstdio>

namespace coop_connection_hud {
constexpr std::uint32_t kGoodPingMs = 100, kModeratePingMs = 200;
constexpr std::uint32_t kSampleFreshMs = 2500, kDisconnectVisibleMs = 15000;
constexpr unsigned kGreen = 0x83D69E, kOrange = 0xF0AB55, kRed = 0xED6A6A;
enum class Status { Hidden, Waiting, Connecting, Measuring, Connected, Interrupted, Disconnected };
struct View {
    Status status = Status::Hidden;
    bool visible = false;
    unsigned rgb = kOrange;
    char text[80]{};
    char activity[96]{};
};
class Model {
    std::uint64_t session_ = 0;
    std::uint32_t lostAt_ = 0;
    bool paired_ = false, hadLink_ = false, detached_ = false;
public:
    View Update(const coop::SessionSnapshot& s, const coop_transport::Statistics& wire,
                bool admitted, bool gameplay, std::uint32_t now, coop_activity::Activity peer = {}) {
        View v;
        const bool scope = s.sessionId || s.joined || gameplay;
        if (!scope) {
            if (!paired_) return v;
            if (!detached_) { detached_ = true; lostAt_ = now; }
            if (coop_time::Elapsed(now, lostAt_) >= kDisconnectVisibleMs) return v;
            v.status = Status::Disconnected;
        } else {
            detached_ = false;
            if (s.sessionId && s.sessionId != session_) {
                session_ = s.sessionId; paired_ = false; hadLink_ = false;
            }
            if (s.joined) paired_ = true;
            if (!s.joined) v.status = paired_ ? Status::Disconnected : Status::Waiting;
            else if (!s.fresh || !admitted || !wire.connected || !wire.ready)
                v.status = hadLink_ ? Status::Interrupted : Status::Connecting;
            else if (!wire.rttMeasured) v.status = Status::Measuring;
            else if (coop_time::Elapsed(now, wire.rttSampleAtMs) >= kSampleFreshMs)
                v.status = Status::Interrupted;
            else { v.status = Status::Connected; hadLink_ = true; }
        }
        v.visible = true;
        switch (v.status) {
        case Status::Waiting: std::snprintf(v.text, sizeof(v.text), "Waiting for teammate"); break;
        case Status::Connecting: std::snprintf(v.text, sizeof(v.text), "Connecting..."); break;
        case Status::Measuring: std::snprintf(v.text, sizeof(v.text), "Measuring ping..."); break;
        case Status::Interrupted: v.rgb = kRed; std::snprintf(v.text, sizeof(v.text), "Connection interrupted"); break;
        case Status::Disconnected: v.rgb = kRed; std::snprintf(v.text, sizeof(v.text), "Teammate disconnected"); break;
        case Status::Connected:
            v.rgb = wire.rttMs <= kGoodPingMs ? kGreen : wire.rttMs <= kModeratePingMs ? kOrange : kRed;
            if (wire.rttMs) std::snprintf(v.text, sizeof(v.text), "Connected | %u ms", wire.rttMs);
            else std::snprintf(v.text, sizeof(v.text), "Connected | <1 ms");
            break;
        default: break;
        }
        if(v.status==Status::Connected||v.status==Status::Measuring){
            if(!coop_activity::Valid(peer))peer={};
            if(peer.race)std::snprintf(v.activity,sizeof(v.activity),"Teammate: %s | %s",coop_activity::Label(peer.scene),coop_activity::RaceName(peer.race));
            else std::snprintf(v.activity,sizeof(v.activity),"Teammate: %s",coop_activity::Label(peer.scene));
        }
        return v;
    }
};
}
