// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Bounded, observation-only trace of the native frame-delta decision.
// No clocks, pause flags, cameras, physics, or save data are changed.
#include <intrin.h>

namespace CoopFrameTrace {
using DeltaFn = float(__thiscall*)(void*, float);
static DeltaFn gNativeDelta = reinterpret_cast<DeltaFn>(0x601A60);
static uint32_t gTicket = 0;
static unsigned gSamples[4]{};
static bool gInstalled = false;

static int Classify(uintptr_t caller) {
    switch (caller) {
    case 0x58133E: return 0; // RaceFlow 6: native freeze branch.
    case 0x5813A3: return 1; // Transient frame-controller pause.
    case 0x581428: return 2; // GameManager pause or RaceFlow 1.
    case 0x5813DD: return 3; // Normal world-delta calculation.
    default: return -1;
    }
}

static void Capture(uintptr_t caller, float input, float output) {
    const int path = Classify(caller);
    if (path < 0 || !IsCoopGameplayActive() || gCareerLaunchKind != 2 ||
        !gNativeCareerReleasedTicket) return;
    uint32_t flow = 0;
    if (!ReadGameMemory(kGameFlowState, &flow, 4) || flow != 6) return;
    if (gTicket != gNativeCareerReleasedTicket) {
        gTicket = gNativeCareerReleasedTicket;
        std::memset(gSamples, 0, sizeof(gSamples));
    }
    if (gSamples[path] >= 3) return;
    ++gSamples[path];
    uintptr_t world = 0, race = 0, frame = 0;
    uint32_t raceState = ~uint32_t(0), flags[5]{};
    float scale = 0;
    ReadGameMemory(0x890080, &world, 4);
    ReadGameMemory(0x8900D8, &race, 4);
    ReadGameMemory(0x8650B8, &frame, 4);
    if (IsValidUserPtr(race)) ReadGameMemory(race, &raceState, 4);
    if (IsValidUserPtr(world)) {
        ReadGameMemory(world + 0xFC, &flags[0], 4);
        ReadGameMemory(world + 0x100, &flags[1], 4);
        ReadGameMemory(world + 0x108, &scale, 4);
    }
    ReadGameMemory(0x890084, &flags[2], 4);
    ReadGameMemory(0x890088, &flags[3], 4);
    if (IsValidUserPtr(frame)) ReadGameMemory(frame + 0x1C, &flags[4], 4);
    char line[320]{};
    std::snprintf(line, sizeof(line),
        "[FRAME_PATH] ticket=%u path=%d caller=%08X input=%.7f output=%.7f race=%u pause=%u/%u/%u/%u framePause=%u scale=%.4f\r\n",
        gTicket, path, unsigned(caller), input, output, raceState,
        flags[0], flags[1], flags[2], flags[3], flags[4], scale);
    LogMessage(line);
}

static float __fastcall Probe(void* self, void*, float input) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const float output = gNativeDelta(self, input);
    Capture(caller, input, output);
    return output; // Preserve native x87 float return and RET 4 ABI.
}

static bool Install() {
    if (gInstalled) return true;
    constexpr uintptr_t entry = 0x601A60;
    const BYTE expected[9]{0x83,0xEC,0x0C,0xD9,0x05,0x30,0x57,0x7A,0x00};
    BYTE actual[9]{};
    if (!ReadGameMemory(entry, actual, sizeof(actual)) ||
        std::memcmp(actual, expected, sizeof(actual))) {
        LogMessage("[FRAME_PATH] Native delta signature differs; observation hook not installed.\r\n");
        return false;
    }
    auto* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, 14,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    std::memcpy(trampoline, actual, 9);
    trampoline[9] = 0xE9;
    const int32_t back = int32_t((entry + 9) - (uintptr_t(trampoline) + 14));
    std::memcpy(trampoline + 10, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 14);
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(entry), 9, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    gNativeDelta = reinterpret_cast<DeltaFn>(trampoline);
    BYTE patch[9]{0xE9,0,0,0,0,0x90,0x90,0x90,0x90};
    const int32_t jump = int32_t(uintptr_t(&Probe) - (entry + 5));
    std::memcpy(patch + 1, &jump, 4);
    std::memcpy(reinterpret_cast<void*>(entry), patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(entry), 9);
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(entry), 9, oldProtect, &unused);
    gInstalled = true;
    LogMessage("[FRAME_PATH] Native delta observation enabled (at most 12 samples per new COOP career).\r\n");
    return true;
}
}
