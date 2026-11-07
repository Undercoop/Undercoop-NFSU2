// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Include after CoopWorldMotionNative.inl. All writes require its current
// Client replica authorization; authoritative Host reads do not mutate memory.
#include "CoopWorldWheel.h"
namespace CoopWorldWheelNative {
constexpr uintptr_t kWheelVtable = 0x007A20E8;
constexpr uintptr_t kTelemetryVtable = 0x007A2560;
constexpr uintptr_t kDormantDriverVtable = 0x007A5CA8;
constexpr uintptr_t kRemoteDriverVtable = 0x00784680;
constexpr uintptr_t kRemoteControlsVtable = 0x007855D8;
struct Layout {
    uintptr_t car = 0, geometry = 0, wrapper = 0, body = 0, telemetry = 0;
    uintptr_t wheel[4]{};
    uint32_t mode = 0, kind = 0;
};
static uint32_t captured = 0, applied = 0, refused = 0;

static bool ReadLayout(uintptr_t car, Layout& output) {
    using namespace CoopPhysicalMotionNative;
    Layout layout{}; layout.car = car;
    uintptr_t params = 0, ride = 0, vtable = 0, owner = 0, base = 0, boundGeometry = 0;
    uintptr_t bodyVtable = 0, bodyGeometry = 0, embeddedGeometry = 0;
    if (!enabled || !IsCoopGameplayActive() || !IsValidUserPtr(car) ||
        car == CoopRaceCar(false) || car == CoopRaceCar(true) ||
        !ReadGameMemory(car + 0x14, &params, sizeof(params)) || !IsValidUserPtr(params) ||
        !ReadGameMemory(car + 0x10, &ride, sizeof(ride)) || ride != params + 0x10 ||
        !ReadGameMemory(params + 4, &layout.kind, 4) || (layout.kind != 2 && layout.kind != 3) ||
        !ReadGameMemory(car + 0x20, &layout.geometry, sizeof(layout.geometry)) || layout.geometry != ride + 0x40 ||
        !ReadGameMemory(car + 0x40, &embeddedGeometry, sizeof(embeddedGeometry)) || embeddedGeometry != layout.geometry ||
        !ReadGameMemory(car + 0x28, &layout.mode, 4) || (layout.mode != 1 && layout.mode != 3) ||
        !ReadGameMemory(car + 0x34, &layout.wrapper, sizeof(layout.wrapper)) || !IsValidUserPtr(layout.wrapper) ||
        !ReadGameMemory(layout.wrapper, &vtable, sizeof(vtable)) ||
        vtable != (layout.mode == 3 ? kRemotePhysicsVtable : kPhysicsVtable) ||
        !ReadGameMemory(layout.wrapper + (layout.mode == 3 ? 0x34 : 0x5C), &owner, sizeof(owner)) || owner != car ||
        !ReadGameMemory(layout.wrapper + (layout.mode == 3 ? 0x30 : 0x58), &base, sizeof(base)) || base != car + 0x40 ||
        !ReadGameMemory(layout.wrapper + (layout.mode == 3 ? 0x2C : 0x54), &boundGeometry, sizeof(boundGeometry)) || boundGeometry != layout.geometry ||
        !ReadGameMemory(layout.wrapper + 0x20, &layout.body, sizeof(layout.body)) || !IsValidUserPtr(layout.body) ||
        !ReadGameMemory(layout.body, &bodyVtable, sizeof(bodyVtable)) || bodyVtable != kBodyVtable ||
        !ReadGameMemory(layout.body + 0x14, &bodyGeometry, sizeof(bodyGeometry)) || bodyGeometry != layout.geometry + 0x110 ||
        !CoopWorldPhysicalMotionNative::ExcludesHumanBody(CoopRaceCar(false), layout.wrapper, layout.body) ||
        !CoopWorldPhysicalMotionNative::ExcludesHumanBody(CoopRaceCar(true), layout.wrapper, layout.body)) return false;
    // Both native constructors (5B5E4B, 5B6F45) create the same Wheel object.
    // Bind every wheel to its exact geometry and embedded Car, not just a VT.
    for (unsigned index = 0; index < 4; ++index) {
        uintptr_t wheelGeometry = 0, springGeometry = 0, tuning = 0, wheelOwner = 0;
        uint32_t ordinal = 0;
        if (!ReadGameMemory(layout.wrapper + 0x38 + index * 4, &layout.wheel[index], sizeof(uintptr_t)) ||
            !IsValidUserPtr(layout.wheel[index]) ||
            !ReadGameMemory(layout.wheel[index], &vtable, sizeof(vtable)) || vtable != kWheelVtable ||
            !ReadGameMemory(layout.wheel[index] + 0x14, &wheelGeometry, sizeof(wheelGeometry)) ||
            wheelGeometry != layout.geometry + 0x10 + index * 0x30 ||
            !ReadGameMemory(wheelGeometry + 0x18, &ordinal, sizeof(ordinal)) || ordinal != index ||
            !ReadGameMemory(layout.wheel[index] + 0x18, &springGeometry, sizeof(springGeometry)) ||
            springGeometry != layout.geometry + 0xD0 + (index / 2) * 0x20 ||
            !ReadGameMemory(layout.wheel[index] + 0x1C, &tuning, sizeof(tuning)) || tuning != layout.geometry + 0x270 ||
            !ReadGameMemory(layout.wheel[index] + 0x20, &wheelOwner, sizeof(wheelOwner)) || wheelOwner != car + 0x40) return false;
        for (unsigned other = 0; other < index; ++other)
            if (layout.wheel[other] == layout.wheel[index]) return false;
    }
    if (layout.mode == 3) {
        uintptr_t telemetryGeometry = 0, telemetryBody = 0, terrain = 0;
        // 5B0030 binds +14 to body, +18 to geometry and +1C to terrain.
        if (!ReadGameMemory(layout.wrapper + 0x24, &layout.telemetry, sizeof(layout.telemetry)) ||
            !IsValidUserPtr(layout.telemetry) ||
            !ReadGameMemory(layout.telemetry, &vtable, sizeof(vtable)) || vtable != kTelemetryVtable ||
            !ReadGameMemory(layout.telemetry + 0x14, &telemetryBody, sizeof(telemetryBody)) || telemetryBody != layout.body ||
            !ReadGameMemory(layout.telemetry + 0x18, &telemetryGeometry, sizeof(telemetryGeometry)) || telemetryGeometry != layout.geometry ||
            !ReadGameMemory(layout.telemetry + 0x1C, &terrain, sizeof(terrain)) || terrain != car + 0x5C4) return false;
    }
    output = layout;
    return true;
}
static bool Writable(const Layout& layout) {
    using namespace CoopPhysicalMotionNative;
    if (layout.mode != 3 || !WritableRange(layout.car + 0x330, 0xF8) ||
        !WritableRange(layout.telemetry + 0x20, 0x94) ||
        !WritableRange(layout.telemetry + 0x19C, 16)) return false;
    for (unsigned wheel = 0; wheel < 4; ++wheel)
        if (!WritableRange(layout.wheel[wheel] + 0x24, 8) ||
            !WritableRange(layout.wheel[wheel] + 0x100, 0x20) ||
            !WritableRange(layout.wheel[wheel] + 0x160, 0x14)) return false;
    return true;
}
static bool Driver(const Layout& layout) {
    uintptr_t driver = 0, vtable = 0, owner = 0, controls = 0, carControls = 0;
    uint32_t role = 0; uint8_t remote = 0;
    if (!ReadGameMemory(layout.car + 0x2C, &driver, sizeof(driver)) || !IsValidUserPtr(driver) ||
        !ReadGameMemory(driver, &vtable, sizeof(vtable)) ||
        (vtable != kDormantDriverVtable && vtable != CoopWorldPhysicalMotionNative::kRacerDriverVtable &&
            vtable != CoopWorldPhysicalMotionNative::kSpecialRacerDriverVtable &&
            vtable != CoopWorldPhysicalMotionNative::kTrafficDriverVtable && vtable != kRemoteDriverVtable) ||
        !ReadGameMemory(driver + 4, &owner, sizeof(owner)) || owner != layout.car ||
        !ReadGameMemory(driver + 8, &controls, sizeof(controls)) || !IsValidUserPtr(controls) ||
        !ReadGameMemory(layout.car + 0x30, &carControls, sizeof(carControls)) || controls != carControls)
        return false;
    if (vtable == kRemoteDriverVtable) {
        // Career race replicas use the stock RemoteDriver (426650), not the
        // dormant ambient Driver. Its +4/+8 are the same Car/control links;
        // the constructor installs RemoteControls VT 7855D8. Do not inspect
        // an AIDriver tail or let a remote human grant AI write authority.
        uintptr_t params = 0, controlVtable = 0;
        if (!ReadGameMemory(layout.car + 0x24, &role, 4) || role != 5 ||
            !ReadGameMemory(layout.car + 0x14, &params, sizeof(params)) ||
            !ReadGameMemory(params + 0x900, &remote, 1) || remote != 1 ||
            !ReadGameMemory(controls, &controlVtable, sizeof(controlVtable)) ||
            controlVtable != kRemoteControlsVtable) return false;
    }
    return true; // Driver/control tail is deliberately neither read nor written.
}
}

bool ReadCoopWorldWheelState(uintptr_t car, coop_world_wheel::State& output) {
    using namespace CoopWorldWheelNative;
    output = {};
    Layout layout{};
    if (!ReadLayout(car, layout)) { ++refused; return false; }
    coop_world_wheel::State result{};
    for (unsigned i = 0; i < 4; ++i) {
        const uintptr_t wheel = layout.wheel[i];
        if (!ReadGameMemory(wheel + 0x24, &result.phase[i], 4) ||
            !ReadGameMemory(wheel + 0x28, &result.speed[i], 4) ||
            !ReadGameMemory(car + 0x3F8 + i * 4, &result.suspension[i], 4) ||
            !ReadGameMemory(car + 0x350 + i * 4, &result.scalar50[i], 4) ||
            !ReadGameMemory(wheel + 0x100, result.slip[i], 8) ||
            !ReadGameMemory(wheel + 0x164, &result.load[i], 4) ||
            !ReadGameMemory(wheel + 0x168, &result.spring[i], 4) ||
            !ReadGameMemory(wheel + 0x170, &result.ratio[i], 4) ||
            !ReadGameMemory(wheel + 0x11C, &result.ground[i], 1) ||
            !ReadGameMemory(wheel + 0x160, &result.skid[i], 1)) { ++refused; return false; }
    }
    result.validMask = coop_world_wheel::kAllWheels;
    if (!coop_world_wheel::Valid(result)) { ++refused; return false; }
    output = result; ++captured;
    return true;
}

bool ApplyCoopWorldWheelState(uintptr_t car, uintptr_t localCar, const coop_world_wheel::State& input) {
    using namespace CoopWorldWheelNative;
    // All four wheels are captured together. Missing/unsupported Host layouts
    // do not authorize inventing local tire ground/slip or disabling any FX.
    if (input.validMask != coop_world_wheel::kAllWheels || !coop_world_wheel::Valid(input)) return false;
    CoopWorldPhysicalMotionNative::Target target{};
    Layout layout{};
    if (!CoopWorldPhysicalMotionNative::ReadTarget(car, localCar, target) || target.mode != 3 || !target.body ||
        !ReadLayout(car, layout) || layout.body != target.body || !Driver(layout) || !Writable(layout)) {
        ++refused; return false;
    }
    uintptr_t ride = 0; uint32_t kind = 0;
    if (!ReadManagedCoopAI(car, ride, kind) || ride != target.ride || kind != target.kind ||
        localCar != CoopRaceCar(false) || car == CoopRaceCar(true)) return false;
    uint32_t groundCount = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const uintptr_t wheel = layout.wheel[i], telemetry = layout.telemetry;
        const uint32_t ground = input.ground[i];
        // 5A2B60 consumes telemetry then refreshes Wheel/cache fields. Mirror
        // both representations so the next native tick cannot resurrect stale
        // slip/speed/ground and generate false smoke/skid/audio.
        if (!SafeWriteBytes(telemetry + 0x20 + i * 4, &input.speed[i], 4) ||
            !SafeWriteBytes(telemetry + 0x30 + i * 4, &input.suspension[i], 4) ||
            !SafeWriteBytes(telemetry + 0x50 + i * 4, &input.scalar50[i], 4) ||
            !SafeWriteBytes(telemetry + 0x60 + i * 4, &ground, 4) ||
            !SafeWriteBytes(telemetry + 0x90 + i * 8, input.slip[i], 8) ||
            !SafeWriteBytes(telemetry + 0xB0 + i, &input.skid[i], 1) ||
            !SafeWriteBytes(telemetry + 0x19C + i * 4, &input.phase[i], 4) ||
            !SafeWriteBytes(wheel + 0x24, &input.phase[i], 4) ||
            !SafeWriteBytes(wheel + 0x28, &input.speed[i], 4) ||
            !SafeWriteBytes(wheel + 0x100, input.slip[i], 8) ||
            !SafeWriteBytes(wheel + 0x118, &input.slip[i][0], 4) ||
            !SafeWriteBytes(wheel + 0x11C, &input.ground[i], 1) ||
            !SafeWriteBytes(wheel + 0x160, &input.skid[i], 1) ||
            !SafeWriteBytes(wheel + 0x164, &input.load[i], 4) ||
            !SafeWriteBytes(wheel + 0x168, &input.spring[i], 4) ||
            !SafeWriteBytes(wheel + 0x170, &input.ratio[i], 4) ||
            !SafeWriteBytes(car + 0x330 + i * 4, &input.speed[i], 4) ||
            !SafeWriteBytes(car + 0x340 + i * 4, &input.phase[i], 4) ||
            !SafeWriteBytes(car + 0x350 + i * 4, &input.scalar50[i], 4) ||
            !SafeWriteBytes(car + 0x360 + i * 4, &input.spring[i], 4) ||
            !SafeWriteBytes(car + 0x390 + i, &input.ground[i], 1) ||
            !SafeWriteBytes(car + 0x3B4 + i * 8, input.slip[i], 8) ||
            !SafeWriteBytes(car + 0x3F8 + i * 4, &input.suspension[i], 4)) return false;
        groundCount += ground;
    }
    if (!SafeWriteBytes(car + 0x424, &groundCount, 4)) return false;
    ++applied; return true;
}
