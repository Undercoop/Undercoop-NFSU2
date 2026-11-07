// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Include after the race-world runtime. These are narrowly scoped pose-write
// guards, not a replacement for native Car/controller/witness lifecycle ticks.
namespace CoopAINativePlacementHooks {
struct Entries {
    uintptr_t route = 0x006012FE, routeContinue = 0x00601304, routeSkip = 0x00601525;
    uintptr_t wake = 0x005F6D2F, wakeContinue = 0x005F6D34, nativeWake = 0x005F5010;
    uintptr_t integration = 0x005AB037, integrationContinue = 0x005AB041, integrationSkip = 0x005AB09B;
    uintptr_t integrationDivisor = 0x007FC040;
};
static Entries entries{}; // Fixtures replace only these private site addresses.
static bool enabled = false;
static uint32_t routeSkipped = 0, wakeSkipped = 0, integrationSkipped = 0;
// A separately installed fleet adapter may authorize exact current ambient/
// pooled traffic identities. Null preserves the original race-only boundary.
static bool (*fleetPlacement)(uintptr_t) = nullptr;
// Only the impact adapter may temporarily release a verified Client body.
// Driver/route authority remains Host-owned even during local contact physics.
static bool (*localImpactIntegration)(uintptr_t) = nullptr;
static bool ReadSignature(uintptr_t address, const uint8_t* expected, size_t bytes) {
    uint8_t actual[16]{};
    return expected && bytes && bytes <= sizeof(actual) &&
        ReadGameMemory(address, actual, bytes) && !std::memcmp(actual, expected, bytes);
}
}

// Indirect targets let isolated fixtures execute the actual x86 hook bodies.
// Production defaults are the exact original instruction boundaries.
static uintptr_t gCoopAIPlacementRouteContinue = 0x00601304;
static uintptr_t gCoopAIPlacementRouteSkip = 0x00601525;
static uintptr_t gCoopAIPlacementWakeContinue = 0x005F6D34;
static uintptr_t gCoopAIPlacementNativeWake = 0x005F5010;
static uintptr_t gCoopAIIntegrationContinue = 0x005AB041;
static uintptr_t gCoopAIIntegrationSkip = 0x005AB09B;
static uintptr_t gCoopAIIntegrationDivisor = 0x007FC040;

bool ShouldSuppressCoopAIPlacement(uintptr_t car) {
    if (!CoopAINativePlacementHooks::enabled || !CoopPhysicalMotionNative::enabled ||
        !ShouldSuppressCoopAIController(car)) return false;
    if (CoopAINativePlacementHooks::fleetPlacement &&
        CoopAINativePlacementHooks::fleetPlacement(car)) return true;
    uintptr_t ride = 0, parameter = 0, actualRide = 0;
    uint32_t kind = 0, ordinal = 0, model = 0, type = 0;
    // The controller predicate can serve other world adapters. Require the
    // current finalized race roster here, explicitly excluding ambient/traffic.
    return ReadManagedCoopAI(car, ride, kind) && kind == 2 &&
        car != CoopRaceCar(false) && car != CoopRaceCar(true) &&
        ReadGameMemory(car + 0x14, &parameter, sizeof(parameter)) &&
        ride == parameter + 0x10 && ReadGameMemory(parameter + 4, &type, 4) && type == 2 &&
        ReadGameMemory(car + 0x10, &actualRide, sizeof(actualRide)) && actualRide == ride &&
        ReadCoopAIRosterParam(parameter, ordinal, model) && ordinal != 0;
}

// Mode three is MellowMover: its +4 virtual tick (5AAD90) integrates its
// body through 5AA480 before refreshing native body/wheel caches at 5A2B60.
// A Client replica normally receives a complete authoritative body state. A
// verified local contact lease temporarily runs these exact stock substeps;
// otherwise do not run a second local simulation. Retain the original cache tick,
// wheel/hub placement, transmission and native lifecycle around that block.
// This is deliberately not a generic wrapper/body suppression permission.
bool ShouldSuppressCoopAIIntegration(uintptr_t wrapper) {
    uintptr_t vtable = 0, car = 0;
    CoopWorldPhysicalMotionNative::Target target{};
    return IsValidUserPtr(wrapper) &&
        ReadGameMemory(wrapper, &vtable, sizeof(vtable)) &&
        vtable == CoopPhysicalMotionNative::kRemotePhysicsVtable &&
        ReadGameMemory(wrapper + 0x34, &car, sizeof(car)) &&
        ShouldSuppressCoopAIPlacement(car) &&
        CoopWorldPhysicalMotionNative::ReadTarget(car, CoopRaceCar(false), target) &&
        (target.kind == 2 || target.kind == 3) && target.mode == 3 && target.wrapper == wrapper && target.body &&
        !(CoopAINativePlacementHooks::localImpactIntegration &&
          CoopAINativePlacementHooks::localImpactIntegration(car));
}

__declspec(naked) void HookedCoopAIReplicaIntegration() {
    __asm {
        // Here ESI=MellowMover, and [original ESP+10] is dt. Neither skipped
        // original instruction changes EFLAGS or GPRs. Preserve the incoming
        // x87/SSE state while evaluating authority on the game thread.
        lea esp, [esp - 4]
        pushfd
        pushad
        mov ebp, esp
        sub esp, 528
        lea ebx, [esp + 15]
        and ebx, -16
        fxsave [ebx]
        push esi
        call ShouldSuppressCoopAIIntegration
        add esp, 4
        movzx eax, al
        mov [ebp + 36], eax
        fxrstor [ebx]
        mov esp, ebp
        cmp dword ptr [ebp + 36], 0
        jne skip_replica_integration
        popad
        popfd
        lea esp, [esp + 4]
        fld dword ptr [esp + 0x10]
        // Use a saved GPR to replay the absolute native operand without
        // leaking a scratch register to the native continuation.
        push eax
        mov eax, dword ptr [gCoopAIIntegrationDivisor]
        fdiv dword ptr [eax]
        pop eax
        jmp dword ptr [gCoopAIIntegrationContinue]
    skip_replica_integration:
        inc dword ptr [CoopAINativePlacementHooks::integrationSkipped]
        popad
        popfd
        lea esp, [esp + 4]
        // 5AB09B calls native 5A2B60(dt), then the original wrapper returns
        // that same dt in ST0 with ret 4. No stack slot or raw flag is changed.
        jmp dword ptr [gCoopAIIntegrationSkip]
    }
}

__declspec(naked) void HookedCoopAIRoutePlacement() {
    __asm {
        // Save all registers, EFLAGS and the complete x87/SSE environment.
        // At this site ESI=Car, EBX has NOT been pushed by the route branch.
        lea esp, [esp - 4]
        pushfd
        pushad
        mov ebp, esp
        sub esp, 528
        lea ebx, [esp + 15]
        and ebx, -16
        fxsave [ebx]
        push esi
        call ShouldSuppressCoopAIPlacement
        add esp, 4
        movzx eax, al
        mov [ebp + 36], eax
        fxrstor [ebx]
        mov esp, ebp
        cmp dword ptr [ebp + 36], 0
        jne skip_placement
        popad
        popfd
        lea esp, [esp + 4]
        mov eax, dword ptr [esi + 0x96C]
        jmp dword ptr [gCoopAIPlacementRouteContinue]
    skip_placement:
        inc dword ptr [CoopAINativePlacementHooks::routeSkipped]
        popad
        popfd
        lea esp, [esp + 4]
        // Preserve the pending request itself: no raw transition flag writes.
        jmp dword ptr [gCoopAIPlacementRouteSkip]
    }
}

__declspec(naked) void HookedCoopAIWakeGroundPlacement() {
    __asm {
        // EDI=Car and ECX=Car at the original void 5F5010 call site.
        lea esp, [esp - 4]
        pushfd
        pushad
        mov ebp, esp
        sub esp, 528
        lea ebx, [esp + 15]
        and ebx, -16
        fxsave [ebx]
        push edi
        call ShouldSuppressCoopAIPlacement
        add esp, 4
        movzx eax, al
        mov [ebp + 36], eax
        fxrstor [ebx]
        mov esp, ebp
        cmp dword ptr [ebp + 36], 0
        jne skip_ground_placement
        popad
        popfd
        lea esp, [esp + 4]
        call dword ptr [gCoopAIPlacementNativeWake]
        jmp dword ptr [gCoopAIPlacementWakeContinue]
    skip_ground_placement:
        inc dword ptr [CoopAINativePlacementHooks::wakeSkipped]
        popad
        popfd
        lea esp, [esp + 4]
        jmp dword ptr [gCoopAIPlacementWakeContinue]
    }
}

bool InstallCoopAIPlacementHooks() {
    using namespace CoopAINativePlacementHooks;
    if (enabled) return true;
    constexpr uint8_t routeBytes[]{0x8B, 0x86, 0x6C, 0x09, 0x00, 0x00};
    constexpr uint8_t wakeBytes[]{0xE8, 0xDC, 0xE2, 0xFF, 0xFF};
    constexpr uint8_t integrationBytes[]{0xD9, 0x44, 0x24, 0x10, 0xD8, 0x35, 0x40, 0xC0, 0x7F, 0x00};
    float divisor = 0;
    if (!CoopPhysicalMotionNative::enabled ||
        !ReadSignature(entries.route, routeBytes, sizeof(routeBytes)) ||
        !ReadSignature(entries.wake, wakeBytes, sizeof(wakeBytes)) ||
        !ReadSignature(entries.integration, integrationBytes, sizeof(integrationBytes)) ||
        !ReadGameMemory(entries.integrationDivisor, &divisor, sizeof(divisor)) ||
        !std::isfinite(divisor) || divisor <= 0.0f) {
        LogMessage("[ERROR] Native AI placement signatures differ; placement guards disabled.\r\n");
        return false;
    }
    // Validate ALL original sites before changing any. Keep the predicate
    // disabled during installation, so a partial installation passes through.
    DWORD routeProtection = 0, wakeProtection = 0, integrationProtection = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(entries.route), sizeof(routeBytes), PAGE_EXECUTE_READWRITE, &routeProtection)) return false;
    if (!VirtualProtect(reinterpret_cast<void*>(entries.wake), sizeof(wakeBytes), PAGE_EXECUTE_READWRITE, &wakeProtection)) {
        DWORD unused = 0;
        VirtualProtect(reinterpret_cast<void*>(entries.route), sizeof(routeBytes), routeProtection, &unused);
        return false;
    }
    if (!VirtualProtect(reinterpret_cast<void*>(entries.integration), sizeof(integrationBytes), PAGE_EXECUTE_READWRITE, &integrationProtection)) {
        DWORD unused = 0;
        VirtualProtect(reinterpret_cast<void*>(entries.wake), sizeof(wakeBytes), wakeProtection, &unused);
        VirtualProtect(reinterpret_cast<void*>(entries.route), sizeof(routeBytes), routeProtection, &unused);
        return false;
    }
    gCoopAIPlacementRouteContinue = entries.routeContinue;
    gCoopAIPlacementRouteSkip = entries.routeSkip;
    gCoopAIPlacementWakeContinue = entries.wakeContinue;
    gCoopAIPlacementNativeWake = entries.nativeWake;
    gCoopAIIntegrationContinue = entries.integrationContinue;
    gCoopAIIntegrationSkip = entries.integrationSkip;
    gCoopAIIntegrationDivisor = entries.integrationDivisor;
    uint8_t routePatch[sizeof(routeBytes)]{0xE9, 0, 0, 0, 0, 0x90};
    uint8_t wakePatch[sizeof(wakeBytes)]{0xE9, 0, 0, 0, 0};
    uint8_t integrationPatch[sizeof(integrationBytes)]{0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    const int32_t routeRelative = static_cast<int32_t>(reinterpret_cast<intptr_t>(&HookedCoopAIRoutePlacement) - (entries.route + 5));
    const int32_t wakeRelative = static_cast<int32_t>(reinterpret_cast<intptr_t>(&HookedCoopAIWakeGroundPlacement) - (entries.wake + 5));
    const int32_t integrationRelative = static_cast<int32_t>(reinterpret_cast<intptr_t>(&HookedCoopAIReplicaIntegration) - (entries.integration + 5));
    std::memcpy(routePatch + 1, &routeRelative, 4);
    std::memcpy(wakePatch + 1, &wakeRelative, 4);
    std::memcpy(integrationPatch + 1, &integrationRelative, 4);
    const bool routeWritten = SafeWriteBytes(entries.route, routePatch, sizeof(routePatch));
    const bool wakeWritten = routeWritten && SafeWriteBytes(entries.wake, wakePatch, sizeof(wakePatch));
    const bool integrationWritten = wakeWritten && SafeWriteBytes(entries.integration, integrationPatch, sizeof(integrationPatch));
    if (!integrationWritten) {
        // A failed guarded copy could have copied part of its destination.
        // Restore every attempted site, including the copy that failed.
        if (wakeWritten) SafeWriteBytes(entries.integration, integrationBytes, sizeof(integrationBytes));
        if (routeWritten) SafeWriteBytes(entries.wake, wakeBytes, sizeof(wakeBytes));
        SafeWriteBytes(entries.route, routeBytes, sizeof(routeBytes));
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(entries.route), sizeof(routeBytes));
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(entries.wake), sizeof(wakeBytes));
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(entries.integration), sizeof(integrationBytes));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(entries.integration), sizeof(integrationBytes), integrationProtection, &unused);
    VirtualProtect(reinterpret_cast<void*>(entries.wake), sizeof(wakeBytes), wakeProtection, &unused);
    VirtualProtect(reinterpret_cast<void*>(entries.route), sizeof(routeBytes), routeProtection, &unused);
    enabled = routeWritten && wakeWritten && integrationWritten;
    LogMessage(enabled ? "[HOOK] Client race-AI pending/wake/integration guards enabled; native wheel caches and Host/SP/traffic simulation preserved.\r\n" :
        "[ERROR] Client race-AI placement guards could not be installed.\r\n");
    return enabled;
}
