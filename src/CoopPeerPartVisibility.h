// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace coop_peer_parts {
constexpr std::size_t kVisibilityOffset = 0x838;
constexpr std::size_t kVisibilityBytes = 0xAA;

// The received ride is already finalized by its owner. Native normalization
// resolves dependent parts but also rebuilds the visibility/stance bytes from
// local defaults. Restore only that finalized value block AFTER normalization.
// Never copy the sender's part/cache/texture pointers back over local objects.
template<class Normalize>
inline bool NormalizeFinalized(std::uint8_t* localRide, const std::uint8_t* peerRide,
                               Normalize normalize) {
    if (!localRide || !peerRide) return false;
    std::array<std::uint8_t, kVisibilityBytes> finalized{};
    std::memcpy(finalized.data(), peerRide + kVisibilityOffset, finalized.size());
    normalize(localRide);
    std::memcpy(localRide + kVisibilityOffset, finalized.data(), finalized.size());
    return true;
}
}
