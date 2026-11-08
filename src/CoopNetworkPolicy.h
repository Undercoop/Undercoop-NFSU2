// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopTransport.h"
#include "CoopWorldWire.h"
#include "CoopPeerActivity.h"
#include <cstring>

namespace coop_network {
constexpr std::uint64_t kHumanMotionStream=UINT64_MAX;
struct DeliveryPolicy {bool valid=false,discovery=false;coop_transport::Delivery delivery=coop_transport::Delivery::ReliableControl;std::uint64_t stream=0;};
inline DeliveryPolicy Classify(const void* data,std::size_t size,std::uint32_t humanMagic,std::size_t humanSize){
    DeliveryPolicy out{};if(!data||size<4||size>coop_transport::kMaxPayloadBytes)return out;
    std::uint32_t magic=0;std::memcpy(&magic,data,4);out.valid=true;
    if(magic==coop::kSessionMagic){
        out.valid=size==sizeof(coop::SessionMessage);out.discovery=out.valid;return out;
    }
    if(magic==humanMagic&&size==humanSize){out.delivery=coop_transport::Delivery::LatestState;out.stream=kHumanMotionStream;return out;}
    if(magic==coop_activity::kMagic){
        coop_activity::Packet packet{};
        if(size!=sizeof(packet)){out.valid=false;return out;}
        std::memcpy(&packet,data,sizeof(packet));out.valid=coop_activity::Valid(packet);
        out.delivery=coop_transport::Delivery::LatestState;out.stream=coop_activity::kStream;return out;
    }
    if(magic==coop_world_wire::kEventMagic){
        coop_world::Event event{};std::uint32_t player=0;
        if(!coop_world_wire::DecodeEvent(data,size,event,player,1)){out.valid=false;return out;}
        // Race AI publishes a complete Spawn snapshot on every frame so the
        // first received pose can establish the replica. These are replaceable
        // motion, unlike real fleet spawns/removals and terminal race results.
        const bool racePose=event.action==coop_world::Action::Spawn&&
            event.state.kind==coop_world::Kind::RaceRacer&&
            event.state.motion.finishStatus==coop_world::kFinishRacing;
        if((event.action==coop_world::Action::Update||racePose)&&event.state.key.id!=kHumanMotionStream&&event.state.key.id!=coop_activity::kStream){
            out.delivery=coop_transport::Delivery::LatestState;out.stream=event.state.key.id;
        }
    }
    if(size>coop_transport::kUnfragmentedPayloadBytes)out.delivery=coop_transport::Delivery::ReliableBulk;
    return out;
}
}
