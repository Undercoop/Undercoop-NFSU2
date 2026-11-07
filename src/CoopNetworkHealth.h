// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopNetworkGeneration.h"
#include "CoopTime.h"
#include "CoopSession.h"

namespace coop_network {
// Discovery heartbeats can still work when a firewall blocks connected
// gameplay. Do not leave a career waiting forever on that partial connection.
constexpr std::uint32_t kGameplayRecoveryMs=coop::kPlayingPeerTimeoutMs;
class Health {
public:
    bool TimedOut(const coop::SessionSnapshot& state,bool connected,std::uint32_t now) {
        const Generation generation{state.sessionId,state.membershipNonce,state.ticket};
        const bool needsLink=state.joined&&state.fresh&&state.ticket&&
            (state.phase==coop::Phase::ProfileSync||state.phase==coop::Phase::InitialLoading||state.phase==coop::Phase::Playing);
        if(!needsLink||connected){waiting_=false;generation_={};return false;}
        if(!waiting_||generation!=generation_){waiting_=true;generation_=generation;since_=now;return false;}
        if(coop_time::Elapsed(now,since_)<kGameplayRecoveryMs)return false;
        waiting_=false;generation_={};return true;
    }
private:
    Generation generation_{};
    std::uint32_t since_=0;
    bool waiting_=false;
};
}
