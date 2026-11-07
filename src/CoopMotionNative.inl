// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Verified against SPEED2.EXE x86: full pose application is distinct from
// 0x5F5010, which rebuilds yaw and settles the car against terrain.
namespace CoopPhysicalMotionNative {
using SetBodyStateFn = void(__thiscall*)(void*, const float*, const float*,
    const float*, const float*, float*);
using DeriveOriginFn = void(__thiscall*)(void*, const float*);
using UpdateRenderFn = void(__thiscall*)(void*);
using SetVelocityFn = void(__thiscall*)(void*, const float*);

static SetBodyStateFn setBodyState = reinterpret_cast<SetBodyStateFn>(0x0059B9B0);
static DeriveOriginFn deriveOrigin = reinterpret_cast<DeriveOriginFn>(0x00592400);
static UpdateRenderFn updateRender = reinterpret_cast<UpdateRenderFn>(0x00592460);
static SetVelocityFn setVelocity = reinterpret_cast<SetVelocityFn>(0x00592160);
static bool enabled = false;
static uint32_t kinematicApplied = 0, bodyApplied = 0;

// Test fixtures substitute private signature pages and typed calls.
struct EntryAddresses {
    uintptr_t body = 0x0059B9B0, origin = 0x00592400;
    uintptr_t render = 0x00592460, velocity = 0x00592160;
};
static EntryAddresses entries;
constexpr uintptr_t kPhysicsVtable = 0x007A25E0;
constexpr uintptr_t kRemotePhysicsVtable = 0x007A2538;
constexpr uintptr_t kBodyVtable = 0x007A20C4;
// 0x604491 and native 0x5F46A0 mode zero construct this inert +8
// embedded-physics link. It has no rigid body or nested solver wrapper.
constexpr uintptr_t kInactivePhysicsVtable = 0x007A5BB0;
// 5A9030's native belt/cinematic mover owns Car at+8, has no rigid body.
// Career intro/resume can retain this mode after VP6 cleanup on either role.
constexpr uintptr_t kBeltPhysicsVtable = 0x007A2144;
constexpr uintptr_t kWitnessVtable = 0x007A2470;

static bool ReadVerifiedMode(uintptr_t car, uint32_t& mode) {
    uintptr_t wrapper = 0, vtable = 0;
    uint32_t candidate = 0;
    if (!IsValidUserPtr(car) || !ReadGameMemory(car + 0x28, &candidate, sizeof(candidate)) ||
        !ReadGameMemory(car + 0x34, &wrapper, sizeof(wrapper)) || !IsValidUserPtr(wrapper) ||
        !ReadGameMemory(wrapper, &vtable, sizeof(vtable))) return false;
    if ((candidate == 0 && vtable == kInactivePhysicsVtable) ||
        (candidate == 4 && vtable == kBeltPhysicsVtable) ||
        (candidate == 1 && vtable == kPhysicsVtable) ||
        (candidate == 3 && vtable == kRemotePhysicsVtable)) {
        mode = candidate;
        return true;
    }
    return false;
}

static bool FiniteVector(const float* vector, float limit) {
    if (!vector) return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(vector[i]) || std::fabs(vector[i]) > limit) return false;
    return true;
}

static bool WritableRange(uintptr_t address, size_t bytes) {
    if (!IsValidUserPtr(address) || !bytes || address + bytes < address ||
        !IsValidUserPtr(address + bytes - 1)) return false;
    const uintptr_t end = address + bytes;
    while (address < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD protection = region.Protect & 0xFF;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (regionEnd <= address) return false;
        address = regionEnd < end ? regionEnd : end;
    }
    return true;
}

static bool VerifySignature(uintptr_t entry, const BYTE* signature, size_t size) {
    BYTE actual[16]{};
    return size <= sizeof(actual) && ReadGameMemory(entry, actual, size) &&
        std::memcmp(actual, signature, size) == 0;
}
}

bool InstallCoopPhysicalMotion() {
    using namespace CoopPhysicalMotionNative;
    enabled = false;
    constexpr BYTE body[]{0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC,
        0x04,0x01,0x00,0x00,0x8B,0x45,0x08,0xD9};
    constexpr BYTE origin[]{0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,
        0x1C,0x8B,0x45,0x08,0x85,0xC0,0x56,0x8B};
    constexpr BYTE render[]{0x8D,0x41,0x50,0x50,0x81,0xC1,0x90,0x00,
        0x00,0x00,0x51,0xE8,0x40,0xEE,0xE6,0xFF};
    constexpr BYTE velocity[]{0x8B,0x44,0x24,0x04,0xD9,0x40,0x04,0x8B,
        0x10,0xD9,0x40,0x08,0x89,0x51,0x30,0xD9};
    // Check every entry before publishing availability. No native code patches.
    const bool validBody = VerifySignature(entries.body, body, sizeof(body));
    const bool validOrigin = VerifySignature(entries.origin, origin, sizeof(origin));
    const bool validRender = VerifySignature(entries.render, render, sizeof(render));
    const bool validVelocity = VerifySignature(entries.velocity, velocity, sizeof(velocity));
    enabled = validBody && validOrigin && validRender && validVelocity &&
        setBodyState && deriveOrigin && updateRender && setVelocity;
    LogMessage(enabled ? "[SYNC] Verified native full-3D body-state adapter enabled.\r\n" :
        "[ERROR] Native full-3D motion signatures differ; body-state adapter disabled.\r\n");
    return enabled;
}

bool ReadCoopPhysicalMotion(uintptr_t car, float* quaternion, float* velocity, float* angular) {
    using namespace CoopPhysicalMotionNative;
    if (!IsValidUserPtr(car) || !quaternion || !velocity || !angular) return false;
    float matrix[16]{}, linear[3]{}, spin[3]{};
    if (!ReadGameMemory(car + 0x90, matrix, sizeof(matrix)) ||
        !ReadGameMemory(car + 0x70, linear, sizeof(linear)) ||
        !ReadGameMemory(car + 0x110, spin, sizeof(spin)) ||
        !FiniteVector(linear, 500.0f) || !FiniteVector(spin, 100.0f) ||
        !coop_motion::IsRotation(matrix)) return false;
    float result[4]{};
    if (!coop_motion::MatrixToQuaternion(matrix, result)) return false;
    std::memcpy(quaternion, result, sizeof(result));
    std::memcpy(velocity, linear, sizeof(linear));
    std::memcpy(angular, spin, sizeof(spin));
    return true;
}

bool ApplyCoopPhysicalMotion(uintptr_t car, uintptr_t localCar, const float* position,
    const float* quaternion, const float* velocity, const float* angular) {
    using namespace CoopPhysicalMotionNative;
    if (!enabled || !IsCoopGameplayActive() || !IsValidUserPtr(car) ||
        !IsValidUserPtr(localCar) || car == localCar || car != CoopRaceCar(true) ||
        localCar != CoopRaceCar(false) || !FiniteVector(position, 1000000.0f) ||
        !FiniteVector(velocity, 500.0f) || !FiniteVector(angular, 100.0f) ||
        !quaternion || !coop_motion::ValidQuaternion(quaternion)) return false;

    uintptr_t ride = 0, wrapper = 0, wrapperVtable = 0, owner = 0;
    uintptr_t basePhysics = 0, body = 0, bodyVtable = 0;
    uintptr_t localWrapper = 0, localWrapperVtable = 0, localBody = 0;
    uintptr_t physicsParameters = 0, bodyParameters = 0, geometry = 0, terrain = 0;
    uintptr_t carGeometry = 0;
    uint32_t physicsMode = 0;
    uintptr_t witness = 0, witnessVtable = 0, witnessCar = 0, witnessPhysics = 0, witnessBody = 0;
    uint32_t witnessHasBody = 0;
    float mass = 0, inverseMass = 0, diagonal[3]{}, centerOfMass[3]{};
    if (!ReadPeerSetupTarget(car, ride) ||
        // 0x5B5350 returns RideInfo+40; both constructor 0x60429E and
        // SetupRide 0x601152 use that embedded geometry, not another heap body.
        !ReadGameMemory(car + 0x20, &carGeometry, sizeof(carGeometry)) || carGeometry != ride + 0x40 ||
        !ReadGameMemory(car + 0x40, &geometry, sizeof(geometry)) || geometry != carGeometry ||
        !ReadGameMemory(car + 0x44, &terrain, sizeof(terrain)) || terrain != car + 0x5C4 ||
        !ReadGameMemory(car + 0x28, &physicsMode, sizeof(physicsMode)) ||
        !ReadGameMemory(car + 0x34, &wrapper, sizeof(wrapper)) || !IsValidUserPtr(wrapper) ||
        !ReadGameMemory(wrapper, &wrapperVtable, sizeof(wrapperVtable)) ||
        (wrapperVtable != kPhysicsVtable && wrapperVtable != kRemotePhysicsVtable &&
            wrapperVtable != kInactivePhysicsVtable && wrapperVtable != kBeltPhysicsVtable)) return false;
    const bool beltLayout = wrapperVtable == kBeltPhysicsVtable;
    const bool kinematicLayout = wrapperVtable == kInactivePhysicsVtable || beltLayout;
    // 0x5F4400 selects native physics mode three for remote players. Its
    // 0x5B6D00 wrapper and mode-one 0x5B5CC0 wrapper own the same body class,
    // but their owner/base/parameter members have different verified offsets.
    const bool remoteLayout = wrapperVtable == kRemotePhysicsVtable;
    if (physicsMode != (beltLayout ? 4U : kinematicLayout ? 0U : remoteLayout ? 3U : 1U)) return false;
    // 0x5AEC20 constructs this witness and binds +130 to its owning Car.
    // 0x5938C0 clears +64/+68 when mode zero has no body.
    if (!ReadGameMemory(car + 0x3C, &witness, sizeof(witness)) || !IsValidUserPtr(witness) ||
        !ReadGameMemory(witness, &witnessVtable, sizeof(witnessVtable)) || witnessVtable != kWitnessVtable ||
        !ReadGameMemory(witness + 0x130, &witnessCar, sizeof(witnessCar)) || witnessCar != car ||
        !ReadGameMemory(witness + 0x5C, &witnessHasBody, sizeof(witnessHasBody)) ||
        !ReadGameMemory(witness + 0x64, &witnessPhysics, sizeof(witnessPhysics)) ||
        !ReadGameMemory(witness + 0x68, &witnessBody, sizeof(witnessBody)) ||
        !ReadGameMemory(localCar + 0x34, &localWrapper, sizeof(localWrapper)) || !IsValidUserPtr(localWrapper) ||
        localWrapper == wrapper || !ReadGameMemory(localWrapper, &localWrapperVtable, sizeof(localWrapperVtable)) ||
        (localWrapperVtable != kPhysicsVtable && localWrapperVtable != kRemotePhysicsVtable &&
            localWrapperVtable != kInactivePhysicsVtable && localWrapperVtable != kBeltPhysicsVtable)) return false;

    if (kinematicLayout) {
        // Mode zero is the real 0x604491/0x5F4768 object, not an outer
        // container. Only +8 is its owner link; +20 must never be read as body.
        if (!ReadGameMemory(wrapper + 8, &basePhysics, sizeof(basePhysics)) || basePhysics != (beltLayout?car:car+0x40) ||
            witnessHasBody != 0 || witnessPhysics != 0 || witnessBody != 0 ||
            !ReadGameMemory(geometry, centerOfMass, sizeof(centerOfMass)) ||
            !FiniteVector(centerOfMass, 100.0f)) return false;
    } else {
    const uintptr_t ownerOffset = remoteLayout ? 0x34 : 0x5C;
    const uintptr_t baseOffset = remoteLayout ? 0x30 : 0x58;
    const uintptr_t parameterOffset = remoteLayout ? 0x2C : 0x54;
    // Read ownership and initialized inertia before touching either car/body.
    if (!ReadGameMemory(wrapper + ownerOffset, &owner, sizeof(owner)) || owner != car ||
        !ReadGameMemory(wrapper + baseOffset, &basePhysics, sizeof(basePhysics)) || basePhysics != car + 0x40 ||
        !ReadGameMemory(wrapper + 0x20, &body, sizeof(body)) || !IsValidUserPtr(body) ||
        !ReadGameMemory(body, &bodyVtable, sizeof(bodyVtable)) || bodyVtable != kBodyVtable ||
        // Native 0x5938C0 binds collision witness +130 to its Car, +64 to
        // wrapper+8 and +68 to the same body for both modes one and three.
        witnessHasBody != 1 || witnessPhysics != wrapper + 8 || witnessBody != body ||
        !ReadGameMemory(wrapper + parameterOffset, &physicsParameters, sizeof(physicsParameters)) || physicsParameters != geometry ||
        !ReadGameMemory(body + 0x14, &bodyParameters, sizeof(bodyParameters)) || bodyParameters != physicsParameters + 0x110 ||
        localWrapperVtable != kPhysicsVtable ||
        !ReadGameMemory(localWrapper + 0x20, &localBody, sizeof(localBody)) || !IsValidUserPtr(localBody) || localBody == body ||
        !ReadGameMemory(body + 0x164, &mass, sizeof(mass)) ||
        !ReadGameMemory(body + 0x168, &inverseMass, sizeof(inverseMass)) ||
        !ReadGameMemory(body + 0x170, &diagonal[0], sizeof(float)) ||
        !ReadGameMemory(body + 0x184, &diagonal[1], sizeof(float)) ||
        !ReadGameMemory(body + 0x198, &diagonal[2], sizeof(float)) ||
        // 0x5A2B60 derives the remote model origin from parameter+0; the
        // full solver's 0x5A5540 instead uses the body center-of-mass cache.
        !ReadGameMemory(remoteLayout ? physicsParameters : body + 0x1F0,
            centerOfMass, sizeof(centerOfMass)) ||
        !std::isfinite(mass) || mass <= 0 || mass > 1000000.0f ||
        !std::isfinite(inverseMass) || inverseMass <= 0 ||
        !FiniteVector(diagonal, 1000000.0f) || diagonal[0] <= 0 || diagonal[1] <= 0 || diagonal[2] <= 0 ||
        !FiniteVector(centerOfMass, 100.0f) ||
        (body < car + 0x524 && body + 0x200 > car) ||
        (body < localCar + 0x524 && body + 0x200 > localCar) ||
        !WritableRange(body + 0x20, 0xAC)) return false;
    }
    if (!WritableRange(car + 0x40, 0x484)) return false;

    float matrix[16]{};
    if (!coop_motion::QuaternionToMatrix(quaternion, matrix) || !coop_motion::IsRotation(matrix)) return false;
    __try {
        // These fields are exactly the inputs used by native 0x5A48A0.
        if (!SafeWriteBytes(car + 0x60, position, sizeof(float) * 3) ||
            !SafeWriteBytes(car + 0x90, matrix, sizeof(matrix)) ||
            !SafeWriteBytes(car + 0x70, velocity, sizeof(float) * 3) ||
            !SafeWriteBytes(car + 0x110, angular, sizeof(float) * 3)) return false;
        // The engine derives linear/angular momentum from actual velocities;
        // body +0x70/+0x80 are momentum and must never be written as velocities.
        if (!kinematicLayout)
            setBodyState(reinterpret_cast<void*>(body), position, matrix, velocity, angular, nullptr);
        deriveOrigin(reinterpret_cast<void*>(car + 0x40), centerOfMass);
        updateRender(reinterpret_cast<void*>(car + 0x40));
        setVelocity(reinterpret_cast<void*>(car + 0x40), velocity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (kinematicLayout) ++kinematicApplied;
    else ++bodyApplied;
    return true;
}
