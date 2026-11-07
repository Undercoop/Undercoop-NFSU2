// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Planning is independent of game memory. The game-thread adapter must capture
// the host's finalized vanilla event list before inserting the second human.
// VehicleParam bytes passed between processes must come from the existing
// SerializeVehicleParamPortable function and be reconstructed with native
// RideInfo/part lookup helpers; a raw process-local VehicleParam is not portable.
namespace coop_ai_roster {
constexpr uint32_t kMaximumRacers = 6;
constexpr uint32_t kHumanSeats = 2;
constexpr uint32_t kMaximumNativeOpponents = kMaximumRacers - 1;
constexpr size_t kVehicleParamBytes = 0x940;
constexpr size_t kEventOpponentBytes = 0x0C;

enum class Status : uint8_t {
    Ready,
    InvalidCapacity,
    HumanCapacityConflict,
    InvalidNativeRoster,
    ProtectedCapacityConflict
};

enum class CapacityPolicy : uint8_t { Strict, PreserveProtected };

struct NativeOpponent {
    // Native finalized parameter-array slot, not the model or current rank.
    uint8_t nativeSlot = 0;
    // A fixed event preset/character must survive the second-human insertion.
    // Determine this from the original event descriptor; do not guess by model.
    bool protectedEntry = false;
};

struct PortableOpponent {
    // The event+0x40+i*0x0C descriptor contains no process-local pointers. Keep
    // its original preset/driver/tuning data beside the finalized portable ride.
    std::array<uint8_t, kEventOpponentBytes> eventDescriptor{};
    std::array<uint8_t, kVehicleParamBytes> vehicleParam{};
};

struct Plan {
    Status status = Status::InvalidCapacity;
    uint8_t nativeCapacity = 0;
    uint8_t effectiveCapacity = 0;
    bool protectedCapacityException = false;
    // Verified single-player/no-opponent events retain the existing second
    // human seat; this is distinct from preserving a mandatory boss opponent.
    bool extraHuman = false;
    uint8_t totalCount = 0;
    uint8_t aiCount = 0;
    // References the native input list, in vanilla event order. Rebuilding the
    // host and peer uses these identical identities even if their local slots
    // or native cars differ. Final param slots 0/1 are the two humans; AI slots
    // and native starting-grid positions begin at 2 and 3 respectively.
    std::array<uint8_t, kMaximumRacers - kHumanSeats> sourceIndices{};
    std::array<uint8_t, kMaximumRacers - kHumanSeats> nativeSlots{};
    std::array<uint8_t, kMaximumRacers - kHumanSeats> grids{};

    bool Ready() const { return status == Status::Ready; }
};

inline Plan BuildPlan(uint32_t nativeCapacity, const NativeOpponent* opponents,
    size_t opponentCount, CapacityPolicy policy = CapacityPolicy::PreserveProtected) {
    Plan result;
    if (nativeCapacity == 0 || nativeCapacity > kMaximumRacers) return result;
    // Single-player event list = one local human plus its native opponents.
    // Incomplete captures must not silently lose a character or invent a car.
    if ((opponentCount && !opponents) || opponentCount != nativeCapacity - 1 ||
        opponentCount > kMaximumNativeOpponents) {
        result.status = Status::InvalidNativeRoster;
        return result;
    }
    size_t protectedCount = 0;
    for (size_t i = 0; i < opponentCount; ++i) {
        if (opponents[i].nativeSlot == 0 || opponents[i].nativeSlot >= nativeCapacity) {
            result.status = Status::InvalidNativeRoster;
            return result;
        }
        for (size_t previous = 0; previous < i; ++previous) {
            if (opponents[previous].nativeSlot == opponents[i].nativeSlot) {
                result.status = Status::InvalidNativeRoster;
                return result;
            }
        }
        protectedCount += opponents[i].protectedEntry ? 1 : 0;
    }
    uint32_t effectiveCapacity = nativeCapacity < kHumanSeats ? kHumanSeats : nativeCapacity;
    if (protectedCount > effectiveCapacity - kHumanSeats) {
        // The special-event policy preserves its mandatory characters alongside
        // the two humans. Ordinary events retain their vanilla capacity. Native
        // six-grid/result limits still apply; an unrepresentable event is held.
        const uint32_t required = kHumanSeats + static_cast<uint32_t>(protectedCount);
        if (policy != CapacityPolicy::PreserveProtected || required > kMaximumRacers) {
            result.status = Status::ProtectedCapacityConflict;
            return result;
        }
        effectiveCapacity = required;
    }
    const size_t aiSeats = effectiveCapacity - kHumanSeats;
    std::array<bool, kMaximumNativeOpponents> selected{};
    size_t selectedCount = 0;
    for (size_t i = 0; i < opponentCount; ++i) {
        if (opponents[i].protectedEntry) { selected[i] = true; ++selectedCount; }
    }
    for (size_t i = 0; i < opponentCount && selectedCount < aiSeats; ++i) {
        if (!selected[i]) { selected[i] = true; ++selectedCount; }
    }
    for (size_t i = 0; i < opponentCount; ++i) {
        if (!selected[i]) continue;
        const size_t output = result.aiCount++;
        result.sourceIndices[output] = static_cast<uint8_t>(i);
        result.nativeSlots[output] = opponents[i].nativeSlot;
        result.grids[output] = static_cast<uint8_t>(output + 3);
    }
    result.nativeCapacity = static_cast<uint8_t>(nativeCapacity);
    result.effectiveCapacity = static_cast<uint8_t>(effectiveCapacity);
    result.extraHuman = nativeCapacity < kHumanSeats;
    result.protectedCapacityException = effectiveCapacity != nativeCapacity && !result.extraHuman;
    result.totalCount = result.effectiveCapacity;
    result.status = Status::Ready;
    return result;
}

// C0/C4/C8/CC are additive local-human/local-AI/remote-human/remote-AI
// categories, not a total-count field. Never let the native AddVehicle remote
// counter increment these finalized values a second time.
inline std::array<uint32_t, 4> ParticipantCounts(const Plan& plan, bool host) {
    if (!plan.Ready()) return {};
    return host ? std::array<uint32_t, 4>{{1, plan.aiCount, 1, 0}} :
        std::array<uint32_t, 4>{{1, 0, 1, plan.aiCount}};
}
} // namespace coop_ai_roster
