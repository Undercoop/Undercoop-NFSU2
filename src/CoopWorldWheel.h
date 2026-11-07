// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <cmath>
#include <cstdint>

// Value-only authoritative tire telemetry. Optional spinner animation at
// Car+4E8/+4F8 is deliberately not part of this state: it is not tire motion.
namespace coop_world_wheel {
constexpr uint32_t kAllWheels = 0x0Fu;
struct State {
    uint32_t validMask = 0;
    float phase[4]{};
    float speed[4]{};
    float suspension[4]{};
    float scalar50[4]{};
    float slip[4][2]{};
    float load[4]{};
    float spring[4]{};
    float ratio[4]{};
    uint8_t ground[4]{};
    uint8_t skid[4]{};
};
static_assert(sizeof(State) == 156, "wheel wire values must not contain pointers");

inline bool Bounded(float value, float limit) {
    return std::isfinite(value) && std::fabs(value) <= limit;
}
inline bool Valid(const State& state) {
    if (state.validMask & ~kAllWheels) return false;
    for (unsigned wheel = 0; wheel < 4; ++wheel) {
        if (!(state.validMask & (1u << wheel))) continue;
        if (!Bounded(state.phase[wheel], 1.0e9f) ||
            !Bounded(state.speed[wheel], 10000.0f) ||
            !Bounded(state.suspension[wheel], 1000.0f) ||
            !Bounded(state.scalar50[wheel], 100000.0f) ||
            !Bounded(state.slip[wheel][0], 100000.0f) ||
            !Bounded(state.slip[wheel][1], 100000.0f) ||
            !Bounded(state.load[wheel], 1.0e7f) ||
            !Bounded(state.spring[wheel], 1.0e7f) ||
            !Bounded(state.ratio[wheel], 100.0f) ||
            state.ground[wheel] > 1 || state.skid[wheel] > 1) return false;
    }
    return true;
}
}
