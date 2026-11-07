// Copyright (C) 2026 batuhan.
// GNU GPL version 3 only, with LICENSE-7ZIP-EXCEPTION.txt.
#include "../src/CoopPeerPartVisibility.h"
#include <cstdio>
#include <cstdlib>

static unsigned checks = 0;
static void Check(bool value, const char* label) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
int main() {
    using namespace coop_peer_parts;
    for (unsigned mode = 0; mode < 3; ++mode) {
        std::array<std::uint8_t, 0x8F0> local{}, peer{}, expected{};
        local.fill(0x5A); peer.fill(0xA5);
        // Distinct arbitrary final flags include both hidden and visible parts,
        // plus stance values. Do not turn every mesh on as a blanket workaround.
        for (std::size_t i = 0; i < kVisibilityBytes; ++i)
            peer[kVisibilityOffset+i] = std::uint8_t(i*13+mode);
        expected = local;
        unsigned calls = 0;
        const auto normalize = [&](std::uint8_t* ride) {
            ++calls;
            Check(ride == local.data(), "normalizes only the local ride");
            std::memset(ride + kVisibilityOffset, 0xFF, kVisibilityBytes);
            // Native dependency resolution and local caches must survive.
            ride[0x590] = 0x31; ride[0x3C0] = 0x72;
        };
        Check(NormalizeFinalized(local.data(), peer.data(), normalize), "finalized normalization succeeds");
        Check(calls == 1, "native normalization called exactly once");
        std::memcpy(expected.data()+kVisibilityOffset, peer.data()+kVisibilityOffset, kVisibilityBytes);
        expected[0x590] = 0x31; expected[0x3C0] = 0x72;
        Check(local == expected, "only final value block restored; native parts and cache retained");
        Check(local[kVisibilityOffset-1] == 0x5A && local[kVisibilityOffset+kVisibilityBytes] == 0x5A,
              "adjacent fields untouched");
        const auto before = local;
        Check(NormalizeFinalized(local.data(), local.data(), normalize), "alias-safe finalized snapshot");
        Check(local == before, "snapshot survives native clobber even when source aliases destination");
    }
    unsigned calls = 0; std::array<std::uint8_t, 0x8F0> ride{};
    auto count = [&](std::uint8_t*) { ++calls; };
    Check(!NormalizeFinalized(nullptr, ride.data(), count), "invalid destination refused");
    Check(!NormalizeFinalized(ride.data(), nullptr, count), "invalid source refused");
    Check(calls == 0, "invalid rides never invoke native code");
    std::printf("Peer part visibility: %u checks passed.\n", checks);
}
