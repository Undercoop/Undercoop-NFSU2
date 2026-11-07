// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Include after the identity-scoped AI placement/integration guards. Only a
// current Client replica may acquire native contacts. Do not call Car::Activate:
// it installs a live AI driver, undoing the dormant-controller crash fixes.
namespace CoopWorldContactNative {
using ModeFn = void(__thiscall*)(void*, uint32_t);
using TerrainFn = void(__thiscall*)(void*);
using EnableFn = void(__thiscall*)(void*, bool);
using DisableFn = void(__thiscall*)(void*);
struct Addresses {
    uintptr_t mode = 0x005F46A0, terrain = 0x005F6CD0;
    uintptr_t enable = 0x00593100, disable = 0x005930F0;
    uintptr_t carSlots = 0x0089CCF8;
};
static Addresses entries;
static ModeFn setMode = reinterpret_cast<ModeFn>(0x005F46A0);
static TerrainFn refreshTerrain = reinterpret_cast<TerrainFn>(0x005F6CD0);
static EnableFn enableWitness = reinterpret_cast<EnableFn>(0x00593100);
static DisableFn disableWitness = reinterpret_cast<DisableFn>(0x005930F0);
static bool enabled = false;
static uint32_t created = 0, enabledCount = 0, disabledCount = 0, refused = 0;
constexpr uintptr_t kRemoteDriverVtable = 0x00784680;
constexpr uintptr_t kRemoteControlsVtable = 0x007855D8;

static bool Signature(uintptr_t address, const uint8_t* expected, size_t count) {
    uint8_t observed[16]{};
    return count <= sizeof(observed) && ReadGameMemory(address, observed, count) &&
        !std::memcmp(observed, expected, count);
}
static bool Install() {
    // Verify before the existing peer-despawn/terrain guards patch the entries.
    const uint8_t mode[]{0x6A,0xFF,0x68,0xA2,0x9B,0x77,0x00};
    const uint8_t terrain[]{0x51,0x56,0x57,0x8B,0xF9,0x6A,0x00,0x8D,0x47,0x60};
    const uint8_t enable[]{0x8A,0x44,0x24,0x04,0x84,0xC0,0x74,0x07,0xC6,0x41,0x17,0x00};
    const uint8_t disable[]{0xFE,0x41,0x17,0xC3};
    enabled = CoopPhysicalMotionNative::enabled && CoopAINativePlacementHooks::enabled &&
        Signature(entries.mode, mode, sizeof(mode)) && Signature(entries.terrain, terrain, sizeof(terrain)) &&
        Signature(entries.enable, enable, sizeof(enable)) && Signature(entries.disable, disable, sizeof(disable));
    return enabled;
}

static bool Witness(const CoopWorldPhysicalMotionNative::Target& target, uintptr_t& witness, uint8_t& depth) {
    uintptr_t owner = 0, vtable = 0;
    return ReadGameMemory(target.car + 0x3C, &witness, sizeof(witness)) &&
        IsValidUserPtr(witness) && ReadGameMemory(witness, &vtable, sizeof(vtable)) &&
        vtable == CoopPhysicalMotionNative::kWitnessVtable &&
        ReadGameMemory(witness + 0x130, &owner, sizeof(owner)) && owner == target.car &&
        ReadGameMemory(witness + 0x17, &depth, sizeof(depth)) &&
        CoopPhysicalMotionNative::WritableRange(witness + 0x17, 1);
}
static bool Disable(uintptr_t witness, uint8_t depth) {
    // Native Disable increments a byte counter; never increment it per frame.
    if (!depth) { disableWitness(reinterpret_cast<void*>(witness)); ++disabledCount; }
    return true;
}
}

bool SetCoopAIReplicaContacts(uintptr_t car, uintptr_t localCar, bool active) {
    using namespace CoopWorldContactNative;
    using namespace CoopPhysicalMotionNative;
    CoopWorldPhysicalMotionNative::Target target{};
    uintptr_t witness = 0; uint8_t depth = 0;
    if (!CoopWorldContactNative::enabled || !setMode || !refreshTerrain || !enableWitness || !disableWitness ||
        !ShouldSuppressCoopAIPlacement(car) ||
        !CoopWorldPhysicalMotionNative::ReadTarget(car, localCar, target) || (target.kind != 2 && target.kind != 3) ||
        !Witness(target, witness, depth)) return false;
    __try {
        if (!active) return Disable(witness, depth);
        float position[3]{}, localPosition[3]{};
        if (!ReadGameMemory(car + 0x60, position, sizeof(position)) || !FiniteVector(position, 1000000.0f) ||
            !ReadGameMemory(localCar + 0x60, localPosition, sizeof(localPosition)) ||
            !FiniteVector(localPosition, 1000000.0f)) { ++refused; return false; }
        float squaredDistance = 0;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const float difference = position[axis] - localPosition[axis];
            squaredDistance += difference * difference;
        }
        // Distant replicas remain visible/map-synchronised but need no local
        // collision solver in an unloaded world region. Re-enable on approach.
        if (squaredDistance > 250.0f * 250.0f) return Disable(witness, depth);
        uintptr_t visual = 0, visualRide = 0;
        if (!ReadGameMemory(car + 0x554, &visual, sizeof(visual)) || !IsValidUserPtr(visual) ||
            !ReadGameMemory(visual + 4, &visualRide, sizeof(visualRide)) || visualRide != target.ride) {
            ++refused; return false;
        }
        if (!target.body) {
            uintptr_t driver = 0, driverVtable = 0, owner = 0, controls = 0, boundControls = 0, slotCar = 0;
            int16_t slot = -1;
            const uintptr_t geometry = target.ride + 0x40;
            if ((target.mode != 0 && target.mode != 4 && !(target.kind == 3 && target.mode == 2)) ||
                !ReadGameMemory(car + 8, &slot, sizeof(slot)) || slot < 0 || slot >= 22 ||
                !ReadGameMemory(CoopWorldContactNative::entries.carSlots + slot * sizeof(uintptr_t), &slotCar, sizeof(slotCar)) || slotCar != car ||
                !ReadGameMemory(car + 0x2C, &driver, sizeof(driver)) || !IsValidUserPtr(driver) ||
                !ReadGameMemory(driver, &driverVtable, sizeof(driverVtable)) ||
                (driverVtable != 0x007A5CA8 && driverVtable != kRemoteDriverVtable && driverVtable != CoopWorldPhysicalMotionNative::kRacerDriverVtable &&
                    driverVtable != CoopWorldPhysicalMotionNative::kSpecialRacerDriverVtable &&
                    !(target.kind == 3 && driverVtable == CoopWorldPhysicalMotionNative::kTrafficDriverVtable)) ||
                !ReadGameMemory(driver + 4, &owner, sizeof(owner)) || owner != car ||
                !ReadGameMemory(driver + 8, &boundControls, sizeof(boundControls)) ||
                !ReadGameMemory(car + 0x30, &controls, sizeof(controls)) || controls != boundControls ||
                !WritableRange(controls, 0x220) || !WritableRange(geometry, 0x2A0)) { ++refused; return false; }
            if (driverVtable == kRemoteDriverVtable) {
                uintptr_t params = 0, controlVtable = 0; uint32_t role = 0; uint8_t remote = 0;
                if (!ReadGameMemory(car + 0x24, &role, 4) || role != 5 ||
                    !ReadGameMemory(car + 0x14, &params, sizeof(params)) ||
                    !ReadGameMemory(params + 0x900, &remote, 1) || remote != 1 ||
                    !ReadGameMemory(controls, &controlVtable, sizeof(controlVtable)) ||
                    controlVtable != kRemoteControlsVtable) { ++refused; return false; }
            }
            float matrix[16]{}, quaternion[4]{}, velocity[3]{}, angular[3]{};
            uint8_t wheels[32]{}; uint16_t steer = 0;
            if (!ReadGameMemory(car + 0x90, matrix, sizeof(matrix)) ||
                !coop_motion::MatrixToQuaternion(matrix, quaternion) ||
                !ReadGameMemory(car + 0x70, velocity, sizeof(velocity)) || !FiniteVector(velocity, 500.0f) ||
                !ReadGameMemory(car + 0x110, angular, sizeof(angular)) || !FiniteVector(angular, 100.0f) ||
                !ReadGameMemory(car + 0x4E8, wheels, sizeof(wheels)) || !ReadGameMemory(car + 0x40E, &steer, 2)) {
                ++refused; return false;
            }
            // Native terrain bookkeeping produces +5C0. The installed wake
            // guard suppresses its ground-placement call for this exact replica.
            // Never force the readiness flag or feed a guessed rigid body.
            refreshTerrain(reinterpret_cast<void*>(car));
            uint8_t loaded = 0; uintptr_t terrainRegion = 0;
            if (!ReadGameMemory(car + 0x5C0, &loaded, 1) || !loaded ||
                !ReadGameMemory(car + 0x5CC, &terrainRegion, sizeof(terrainRegion)) || !IsValidUserPtr(terrainRegion))
                return Disable(witness, depth);
            if (!ShouldSuppressCoopAIPlacement(car) || !CoopWorldPhysicalMotionNative::ReadTarget(car, localCar, target) ||
                target.body || (target.kind != 2 && target.kind != 3)) { ++refused; return false; }
            // MellowMover is the verified native contact-capable remote body.
            // Its native constructor binds the witness and solver registry; AI
            // driver/integration suppression remains installed and unchanged.
            setMode(reinterpret_cast<void*>(car), 3);
            if (!CoopWorldPhysicalMotionNative::ReadTarget(car, localCar, target) || target.mode != 3 || !target.body ||
                !ApplyCoopWorldPhysicalMotion(car, localCar, position, quaternion, velocity, angular) ||
                !SafeWriteBytes(car + 0x4E8, wheels, sizeof(wheels)) || !SafeWriteBytes(car + 0x40E, &steer, 2) ||
                !Witness(target, witness, depth)) { ++refused; return false; }
            ++created;
            char message[160]{};
            std::snprintf(message, sizeof(message), "[AI_CONTACT] Client replica car=%08lX native_body=%08lX mode=3 activated.\r\n",
                static_cast<unsigned long>(car), static_cast<unsigned long>(target.body));
            LogMessage(message);
        }
        uint8_t terrainReady = 0;
        uintptr_t terrainRegion = 0;
        if (!ReadGameMemory(car + 0x5C0, &terrainReady, 1) ||
            !ReadGameMemory(car + 0x5CC, &terrainRegion, sizeof(terrainRegion))) { ++refused; return false; }
        if (!terrainReady || !IsValidUserPtr(terrainRegion)) {
            // The race loader already creates MellowMover bodies, unlike the
            // ambient factory above. Body existence is NOT terrain readiness.
            // Refresh native bookkeeping for a nearby accepted replica before
            // enabling its witness. The installed wake guard preserves its
            // full authoritative pose; never force +5C0 or fabricate a region.
            refreshTerrain(reinterpret_cast<void*>(car));
            if (!ShouldSuppressCoopAIPlacement(car) ||
                !CoopWorldPhysicalMotionNative::ReadTarget(car, localCar, target) ||
                !target.body || !Witness(target, witness, depth)) { ++refused; return false; }
            if (!ReadGameMemory(car + 0x5C0, &terrainReady, 1) || !terrainReady ||
                !ReadGameMemory(car + 0x5CC, &terrainRegion, sizeof(terrainRegion)) || !IsValidUserPtr(terrainRegion))
                return Disable(witness, depth);
        }
        if (depth) { enableWitness(reinterpret_cast<void*>(witness), true); ++enabledCount; }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ++refused; return false; }
}
