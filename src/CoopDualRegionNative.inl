// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// The PC view builder normally registers just the local camera's world region.
// Native Car::SetPhysicsMode falls back to BeltMover when terrain is absent.
// Register the peer's second native streaming centre AFTER the view builder,
// without inventing a second viewport or changing native traffic spawn policy.
namespace CoopDualRegionNative {
struct Addresses {
    uintptr_t epilogue = 0x0044F18D, centre = 0x005D6040;
    uintptr_t streamer = 0x0088EB00, flow = 0x008654A4, mode = 0x00850078;
};
static Addresses addresses{};
using CentreFn = void(__thiscall*)(void*, int, const float*, const float*);
static CentreFn centre = reinterpret_cast<CentreFn>(0x005D6040);
static bool enabled = false;
static uint32_t registered = 0, refused = 0;

static bool Refresh() {
    if (!enabled || !centre || !gCoopAIEnabled || !IsCoopGameplayActive()) return false;
    const auto session = CoopSnapshot();
    DWORD flow = 0, mode = 0;
    uint8_t localEnabled = 0, secondEnabled = 0;
    if (!session.joined || !session.fresh ||
        (session.role != coop::Role::Host && session.role != coop::Role::Client) ||
        !ReadGameMemory(addresses.flow, &flow, 4) || flow != 6 ||
        !ReadGameMemory(addresses.mode, &mode, 4) || mode != 1 ||
        !ReadGameMemory(addresses.streamer + 0xC4, &localEnabled, 1) || localEnabled != 1 ||
        !ReadGameMemory(addresses.streamer + 0x120, &secondEnabled, 1) || secondEnabled != 0) return false;
    const uintptr_t car = CoopRaceCar(true), local = CoopRaceCar(false);
    uintptr_t parameter = 0, ride = 0;
    uint32_t type = 0;
    uint8_t remote = 0, index = 0, active = 0;
    float position[3]{}, velocity[3]{};
    if (!IsValidUserPtr(local) || !IsValidUserPtr(car) || car == local ||
        !ReadGameMemory(car + 0x14, &parameter, sizeof(parameter)) || !IsValidUserPtr(parameter) ||
        !ReadGameMemory(car + 0x10, &ride, sizeof(ride)) || ride != parameter + 0x10 ||
        !ReadGameMemory(parameter + 4, &type, 4) || type != 1 ||
        !ReadGameMemory(parameter + 0x900, &remote, 1) || remote != 1 ||
        !ReadGameMemory(parameter + 1, &index, 1) || index != kPeerRenderPlayerIndex ||
        !ReadGameMemory(car + 0x550, &active, 1) || active != 1 ||
        !ReadGameMemory(car + 0x60, position, sizeof(position)) ||
        !ReadGameMemory(car + 0x70, velocity, sizeof(velocity)) ||
        !CoopPhysicalMotionNative::FiniteVector(position, 1000000.0f) ||
        !CoopPhysicalMotionNative::FiniteVector(velocity, 500.0f)) { ++refused; return false; }
    // The verified native routine copies XY position and XY velocity into its
    // bounded slot; it doesn't retain these stack pointers or change Car state.
    __try { centre(reinterpret_cast<void*>(addresses.streamer), 1, position, velocity); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ++refused; return false; }
    ++registered; return true;
}
}

static void __cdecl RefreshCoopPeerWorldRegion() { CoopDualRegionNative::Refresh(); }
__declspec(naked) static void HookedCoopWorldRegionEpilogue() {
    __asm {
        pushfd
        pushad
        sub esp, 108
        fnsave [esp]
        call RefreshCoopPeerWorldRegion
        frstor [esp]
        add esp, 108
        popad
        popfd
        // Exact displaced native epilogue; return value and frame are intact.
        pop edi
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        ret
    }
}
static bool InstallCoopDualRegionHook() {
    using namespace CoopDualRegionNative;
    enabled = false;
    constexpr BYTE end[]{0x5F,0x5E,0x5B,0x8B,0xE5,0x5D};
    constexpr BYTE set[]{0x8B,0x44,0x24,0x04,0x6B,0xC0,0x5C,0x8D,0x84,0x08,0xB0,0x00,0x00,0x00};
    BYTE actualEnd[sizeof(end)]{}, actualSet[sizeof(set)]{};
    if (!centre || !ReadGameMemory(addresses.epilogue, actualEnd, sizeof(end)) ||
        !ReadGameMemory(addresses.centre, actualSet, sizeof(set)) ||
        std::memcmp(actualEnd, end, sizeof(end)) || std::memcmp(actualSet, set, sizeof(set))) {
        LogMessage("[ERROR] Native second streaming centre signatures differ; peer region hook disabled.\r\n");
        return false;
    }
    auto* target = reinterpret_cast<BYTE*>(addresses.epilogue);
    DWORD previous = 0, unused = 0;
    if (!VirtualProtect(target, sizeof(end), PAGE_EXECUTE_READWRITE, &previous)) return false;
    const int32_t jump = static_cast<int32_t>(reinterpret_cast<intptr_t>(&HookedCoopWorldRegionEpilogue) -
        (addresses.epilogue + 5));
    target[0] = 0xE9; std::memcpy(target + 1, &jump, sizeof(jump)); target[5] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(end));
    VirtualProtect(target, sizeof(end), previous, &unused);
    enabled = true;
    LogMessage("[AI_REGION] Verified native peer terrain streaming centre installed; SP and existing view centres preserved.\r\n");
    return true;
}
