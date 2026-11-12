// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#include "../src/CoopAnimationScene.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace scene = coop_animation_scene;
static unsigned checks = 0, failures = 0;
static void Check(bool value, const char* why, unsigned line) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL line %u: %s\n", line, why); }
}
#define CHECK(...) Check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

struct Memory {
    std::map<uintptr_t, unsigned char> bytes;
    unsigned reads = 0;
    uintptr_t fail = 0, mutateAt = 0, mutateAddress = 0;
    uint32_t mutateValue = 0;
    unsigned mutateRead = 0, visits = 0;
    bool sawLowRead = false;
    template<class T> void Put(uintptr_t address, const T& value) {
        const auto* data = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(T); ++i) bytes[address + i] = data[i];
    }
    bool Read(uintptr_t address, void* out, size_t size) {
        ++reads;
        if (address < 0x10000 || address > UINT32_MAX || size - 1 > UINT32_MAX - address) sawLowRead = true;
        if (address == mutateAt && ++visits == mutateRead) Put(mutateAddress, mutateValue);
        if (address == fail) return false;
        auto* destination = static_cast<unsigned char*>(out);
        for (size_t i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            if (it == bytes.end()) return false;
            destination[i] = it->second;
        }
        return true;
    }
    auto Reader() { return [this](uintptr_t address, void* out, size_t size) { return Read(address, out, size); }; }
};
constexpr uint32_t owner = 0x100000, sceneHead = owner + 8, first = 0x200000, second = 0x200100;
static void Empty(Memory& m, uint32_t flow = 3) {
    m.Put(scene::kFlow, flow);
    m.Put(scene::kScene, owner);
    m.Put(sceneHead, scene::detail::Links{sceneHead, sceneHead});
}
static void World(Memory& m, uint32_t connection = 1) {
    m.Put(scene::kPlayers, uint32_t(0x300000));
    m.Put(0x300004, uint32_t(0x400000));
    m.Put(0x400554, connection);
}
static void One(Memory& m, uint32_t part = 0, uint32_t state = scene::kOpen) {
    Empty(m);
    m.Put(sceneHead, scene::detail::Links{first, first});
    m.Put(first, scene::detail::Node{sceneHead, sceneHead, 0, part, state});
}
static void MissingSceneAndContext() {
    for (uint32_t flow : {0u, 1u, 2u, 4u, 5u, 7u, UINT32_MAX}) {
        Memory m; Empty(m, flow);
        CHECK(scene::Query(m.Reader(), 0) == scene::kBusy);
        CHECK(m.reads == 1);
    }
    for (uint32_t invalid : {0u, 8u, 0xFFFFu, 0xFFFFFFF8u}) {
        Memory m; Empty(m); m.Put(scene::kScene, invalid);
        CHECK(scene::Query(m.Reader(), 0) == scene::kBusy);
        CHECK(!m.sawLowRead);
    }
    Memory missing;
    CHECK(scene::Query(missing.Reader(), 0) == scene::kBusy);
    Memory m; Empty(m);
    for (uint32_t invalidPart : {4u, 99u, UINT32_MAX}) {
        const auto before = m.reads;
        CHECK(scene::Query(m.Reader(), invalidPart) == scene::kBusy);
        CHECK(m.reads == before);
    }
}
static void ValidLists() {
    for (uint32_t flow : {3u, 6u}) {
        Memory m; Empty(m, flow); if (flow == 6) World(m);
        for (uint32_t part = 0; part != 4; ++part) CHECK(scene::Query(m.Reader(), part) == scene::kClosed);
        CHECK(!m.sawLowRead);
    }
    for (uint32_t part = 0; part != 4; ++part) {
        for (uint32_t state : {scene::kOpen, scene::kClosed, scene::kBusy}) {
            Memory m; One(m, part, state);
            CHECK(scene::Query(m.Reader(), part) == state);
            CHECK(scene::Query(m.Reader(), (part + 1) % 4) == scene::kClosed);
        }
    }
    Memory m; One(m);
    m.Put(sceneHead, scene::detail::Links{first, second});
    m.Put(first, scene::detail::Node{second, sceneHead, 0, 0, scene::kClosed});
    m.Put(second, scene::detail::Node{sceneHead, first, 0, 3, scene::kOpen});
    CHECK(scene::Query(m.Reader(), 3) == scene::kOpen); // final actual node is included
    CHECK(scene::Query(m.Reader(), 0) == scene::kClosed);
    m.Put(second, scene::detail::Node{sceneHead, first, 0, 0, scene::kOpen});
    CHECK(scene::Query(m.Reader(), 0) == scene::kClosed); // retain first matching animation
}
static void MalformedLists() {
    for (auto invalid : {0u, 8u, 0xFFFFu, 0xFFFFFFFCu}) {
        Memory m; Empty(m); m.Put(sceneHead, scene::detail::Links{invalid, invalid});
        CHECK(scene::Query(m.Reader(), 0) == scene::kBusy);
        CHECK(!m.sawLowRead);
    }
    for (unsigned change = 0; change != 7; ++change) {
        Memory m; One(m);
        auto node = scene::detail::Node{sceneHead, sceneHead, 0, 0, scene::kOpen};
        if (change == 0) node.next = 0;
        if (change == 1) node.previous = 0;
        if (change == 2) node.next = first;
        if (change == 3) node.part = 4;
        if (change == 4) node.state = 3;
        if (change == 5) node.next = second; // unmapped node
        if (change == 6) m.Put(sceneHead, scene::detail::Links{first, second}); // wrong tail
        m.Put(first, node);
        CHECK(scene::Query(m.Reader(), 0) == scene::kBusy);
    }
    Memory m; One(m);
    m.Put(sceneHead, scene::detail::Links{first, second});
    m.Put(first, scene::detail::Node{second, sceneHead, 0, 0, scene::kOpen});
    m.Put(second, scene::detail::Node{first, first, 0, 1, scene::kClosed});
    CHECK(scene::Query(m.Reader(), 0) == scene::kBusy); // bounded cycle does not return an earlier match
    CHECK(m.reads < 12);
    Memory emptyBad; Empty(emptyBad); emptyBad.Put(sceneHead, scene::detail::Links{sceneHead, first});
    CHECK(scene::Query(emptyBad.Reader(), 0) == scene::kBusy);
}
static void WorldReadiness() {
    for (unsigned change = 0; change != 7; ++change) {
        Memory m; Empty(m, 6); World(m);
        if (change == 0) m.Put(scene::kPlayers, uint32_t(0));
        if (change == 1) m.Put(0x300004, uint32_t(0));
        if (change == 2) m.Put(0x400554, uint32_t(0));
        if (change == 3) m.fail = scene::kPlayers;
        if (change == 4) m.fail = 0x300004;
        if (change == 5) m.fail = 0x400554;
        if (change == 6) m.Put(0x300004, uint32_t(0xFFFFFFF0));
        CHECK(scene::Query(m.Reader(), 0) == scene::kBusy);
        CHECK(!m.sawLowRead);
    }
    for (auto connection : {1u, 0x100u, 0x12345678u}) {
        Memory m; Empty(m, 6); World(m, connection);
        CHECK(scene::Query(m.Reader(), 0) == scene::kClosed);
    }
}
static void ReadFailuresAndTeardown() {
    for (auto address : {scene::kFlow, scene::kScene, uintptr_t(sceneHead), uintptr_t(first)}) {
        Memory m; One(m); m.fail = address;
        CHECK(scene::Query(m.Reader(), 0) == scene::kBusy);
    }
    for (unsigned change = 0; change != 3; ++change) {
        Memory m; One(m);
        m.mutateAt = scene::kFlow; m.mutateRead = 2;
        if (change == 0) { m.mutateAddress = scene::kFlow; m.mutateValue = 5; }
        if (change == 1) { m.mutateAddress = scene::kScene; m.mutateValue = 0; }
        if (change == 2) { m.mutateAddress = sceneHead; m.mutateValue = sceneHead; }
        CHECK(scene::Query(m.Reader(), 0) == scene::kBusy);
    }
    Memory m; Empty(m, 6); World(m);
    m.mutateAt = scene::kFlow; m.mutateRead = 2;
    m.mutateAddress = 0x400554; m.mutateValue = 0;
    CHECK(scene::Query(m.Reader(), 0) == scene::kBusy);
}
static void Budget() {
    for (size_t count : {scene::kMaxNodes, scene::kMaxNodes + 1}) {
        Memory m; Empty(m);
        const auto last = first + uint32_t((count - 1) * 0x100);
        m.Put(sceneHead, scene::detail::Links{first, last});
        for (size_t i = 0; i < count; ++i) {
            const auto at = first + uint32_t(i * 0x100);
            m.Put(at, scene::detail::Node{i + 1 == count ? sceneHead : at + 0x100,
                i ? at - 0x100 : sceneHead, 0, uint32_t(i % 4), scene::kClosed});
        }
        CHECK(scene::Query(m.Reader(), 0) == (count == scene::kMaxNodes ? scene::kClosed : scene::kBusy));
        CHECK(m.reads <= scene::kMaxNodes + 6);
        CHECK(!m.sawLowRead);
    }
}
int main() {
    MissingSceneAndContext(); ValidLists(); MalformedLists(); WorldReadiness(); ReadFailuresAndTeardown(); Budget();
    std::printf("Animation scene tests: %u checks, %u failures.\n", checks, failures);
    return failures ? 1 : 0;
}
