// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// Read-only policy for Animation Controller's native x86 animation scene.
// No engine routines, input polling or third-party implementation are used.
namespace coop_animation_scene {
constexpr uint32_t kOpen = 0, kClosed = 1, kBusy = 2;
constexpr size_t kMaxNodes = 128;
constexpr uintptr_t kFlow = 0x8654A4, kScene = 0x827C9C, kPlayers = 0x8900AC;

namespace detail {
inline bool Range(uintptr_t address, size_t bytes) {
    return address >= 0x10000 && bytes && bytes <= UINT32_MAX &&
        address <= uintptr_t(UINT32_MAX) - (bytes - 1);
}
template<class Reader, class T>
bool Read(Reader& reader, uintptr_t address, T& output) {
    return Range(address, sizeof(T)) && reader(address, &output, sizeof(T));
}
struct Links { uint32_t next = 0, previous = 0; };
struct Node {
    uint32_t next = 0, previous = 0, speedBits = 0, part = 0, state = 0;
};
static_assert(sizeof(Links) == 8 && sizeof(Node) == 20, "Native x86 scene layout");
struct World {
    uint32_t players = 0, car = 0, connection = 0;
    bool operator==(const World& other) const {
        return players == other.players && car == other.car && connection == other.connection;
    }
};
template<class Reader>
bool ReadWorld(Reader& reader, World& world) {
    return Read(reader, kPlayers, world.players) && Range(world.players, 8) &&
        Read(reader, uintptr_t(world.players) + 4, world.car) && Range(world.car, 0x558) &&
        Read(reader, uintptr_t(world.car) + 0x554, world.connection) && world.connection != 0;
}
}

// Busy deliberately blocks a toggle. Closed would ask the mod to start an
// animation and is therefore unsuitable when its car or scene is absent.
template<class Reader>
uint32_t Query(Reader&& reader, uint32_t part) {
    if (part > 3) return kBusy;
    uint32_t flow = 0, scene = 0;
    if (!detail::Read(reader, kFlow, flow) || (flow != 3 && flow != 6)) return kBusy;
    detail::World world{};
    if (flow == 6 && !detail::ReadWorld(reader, world)) return kBusy;
    if (!detail::Read(reader, kScene, scene) || !detail::Range(scene, 16)) return kBusy;
    const uintptr_t sentinel = uintptr_t(scene) + 8;
    detail::Links head{};
    if (!detail::Read(reader, sentinel, head) || !detail::Range(head.next, sizeof(detail::Links)) ||
        !detail::Range(head.previous, sizeof(detail::Links))) return kBusy;

    std::array<uint32_t, kMaxNodes> seen{};
    size_t count = 0;
    uint32_t current = head.next, previous = uint32_t(sentinel), result = kClosed;
    bool found = false;
    while (current != sentinel) {
        if (count == seen.size()) return kBusy;
        for (size_t i = 0; i < count; ++i) if (seen[i] == current) return kBusy;
        seen[count++] = current;
        detail::Node node{};
        if (!detail::Read(reader, current, node) || node.previous != previous ||
            !detail::Range(node.next, sizeof(detail::Links)) || node.part > 3 || node.state > kBusy)
            return kBusy;
        if (!found && node.part == part) { result = node.state; found = true; }
        previous = current;
        current = node.next;
    }
    if (previous != head.previous) return kBusy;

    // The reader may observe a teardown between individual bounded reads.
    // Refuse a result if the owning roots or sentinel changed meanwhile.
    uint32_t finalFlow = 0, finalScene = 0;
    detail::Links finalHead{};
    if (!detail::Read(reader, kFlow, finalFlow) || finalFlow != flow ||
        !detail::Read(reader, kScene, finalScene) || finalScene != scene ||
        !detail::Read(reader, sentinel, finalHead) || finalHead.next != head.next ||
        finalHead.previous != head.previous) return kBusy;
    if (flow == 6) {
        detail::World finalWorld{};
        if (!detail::ReadWorld(reader, finalWorld) || !(finalWorld == world)) return kBusy;
    }
    return result;
}
}
