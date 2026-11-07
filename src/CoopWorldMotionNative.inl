// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Include after CoopMotionNative.inl and the declaration of ReadManagedCoopAI.
// ReadManagedCoopAI must authorize CURRENT CLIENT REPLICA bindings only; host
// authoritative AI, stale generations and both human cars must return false.
namespace CoopWorldPhysicalMotionNative {
constexpr uintptr_t kTrafficMoverVtable = 0x0078456C;
constexpr uintptr_t kBeltMoverVtable = 0x007A2144;
constexpr uintptr_t kTrafficDriverVtable = 0x00785428;
constexpr uintptr_t kRacerDriverVtable = 0x00785370;
constexpr uintptr_t kSpecialRacerDriverVtable = 0x00785518;
static uint32_t trafficApplied = 0, kinematicApplied = 0, bodyApplied = 0;
static uint32_t captureFromCache = 0, captureFromBody = 0, captureRefused = 0;

struct Target {
    uintptr_t car = 0, ride = 0, wrapper = 0, body = 0;
    uint32_t kind = 0, mode = 0;
    float centerOfMass[3]{};
};

static bool ExcludesHumanBody(uintptr_t humanCar, uintptr_t wrapper, uintptr_t body) {
    using namespace CoopPhysicalMotionNative;
    if (!humanCar) return true;
    uintptr_t humanWrapper = 0, vtable = 0, humanBody = 0;
    uint32_t mode = 0;
    if (!IsValidUserPtr(humanCar) ||
        !ReadGameMemory(humanCar + 0x28, &mode, sizeof(mode)) ||
        !ReadGameMemory(humanCar + 0x34, &humanWrapper, sizeof(humanWrapper)) ||
        !IsValidUserPtr(humanWrapper) || humanWrapper == wrapper ||
        !ReadGameMemory(humanWrapper, &vtable, sizeof(vtable))) return false;
    if (mode == 0 && vtable == kInactivePhysicsVtable) return true;
    if (mode == 4 && vtable == kBeltMoverVtable) {
        uintptr_t owner = 0;
        return ReadGameMemory(humanWrapper + 8, &owner, sizeof(owner)) && owner == humanCar;
    }
    if (!((mode == 1 && vtable == kPhysicsVtable) ||
          (mode == 3 && vtable == kRemotePhysicsVtable))) return false;
    return ReadGameMemory(humanWrapper + 0x20, &humanBody, sizeof(humanBody)) &&
        IsValidUserPtr(humanBody) && (!body || humanBody != body);
}

static bool ReadTarget(uintptr_t car, uintptr_t localCar, Target& result) {
    using namespace CoopPhysicalMotionNative;
    const uintptr_t peerCar = CoopRaceCar(true);
    if (!enabled || !setBodyState || !deriveOrigin || !updateRender || !setVelocity ||
        !IsCoopGameplayActive() || !IsValidUserPtr(car) || !IsValidUserPtr(localCar) ||
        localCar != CoopRaceCar(false) || car == localCar || car == peerCar) return false;

    Target target{};
    target.car = car;
    uintptr_t params = 0, actualRide = 0, geometry = 0, embeddedGeometry = 0, terrain = 0;
    uintptr_t wrapperVtable = 0, witness = 0, witnessVtable = 0, witnessCar = 0;
    uintptr_t witnessPhysics = 0, witnessBody = 0;
    uint32_t actualKind = 0, witnessHasBody = 0;
    if (!ReadManagedCoopAI(car, target.ride, target.kind) ||
        (target.kind != 2 && target.kind != 3) || !IsValidUserPtr(target.ride) ||
        !ReadGameMemory(car + 0x14, &params, sizeof(params)) || !IsValidUserPtr(params) ||
        target.ride != params + 0x10 ||
        !ReadGameMemory(params + 4, &actualKind, sizeof(actualKind)) || actualKind != target.kind ||
        !ReadGameMemory(car + 0x10, &actualRide, sizeof(actualRide)) || actualRide != target.ride ||
        !ReadGameMemory(car + 0x20, &geometry, sizeof(geometry)) || geometry != target.ride + 0x40 ||
        !ReadGameMemory(car + 0x40, &embeddedGeometry, sizeof(embeddedGeometry)) || embeddedGeometry != geometry ||
        !ReadGameMemory(car + 0x44, &terrain, sizeof(terrain)) || terrain != car + 0x5C4 ||
        !ReadGameMemory(car + 0x28, &target.mode, sizeof(target.mode)) ||
        !ReadGameMemory(car + 0x34, &target.wrapper, sizeof(target.wrapper)) || !IsValidUserPtr(target.wrapper) ||
        !ReadGameMemory(target.wrapper, &wrapperVtable, sizeof(wrapperVtable)) ||
        !ReadGameMemory(car + 0x3C, &witness, sizeof(witness)) || !IsValidUserPtr(witness) ||
        !ReadGameMemory(witness, &witnessVtable, sizeof(witnessVtable)) || witnessVtable != kWitnessVtable ||
        !ReadGameMemory(witness + 0x130, &witnessCar, sizeof(witnessCar)) || witnessCar != car ||
        !ReadGameMemory(witness + 0x5C, &witnessHasBody, sizeof(witnessHasBody)) ||
        !ReadGameMemory(witness + 0x64, &witnessPhysics, sizeof(witnessPhysics)) ||
        !ReadGameMemory(witness + 0x68, &witnessBody, sizeof(witnessBody))) return false;

    const bool traffic = target.kind == 3;
    const bool kinematic = target.mode == 0;
    const bool belt = target.mode == 4;
    const bool remoteBody = target.mode == 3;
    const bool trafficMover = traffic && target.mode == 2;
    if (traffic ? !((target.mode == 0 && wrapperVtable == kInactivePhysicsVtable) ||
                   (target.mode == 2 && wrapperVtable == kTrafficMoverVtable) ||
                   (target.mode == 3 && wrapperVtable == kRemotePhysicsVtable) ||
                   (target.mode == 4 && wrapperVtable == kBeltMoverVtable)) :
        !((target.mode == 0 && wrapperVtable == kInactivePhysicsVtable) ||
          (target.mode == 1 && wrapperVtable == kPhysicsVtable) ||
          (target.mode == 3 && wrapperVtable == kRemotePhysicsVtable) ||
          (target.mode == 4 && wrapperVtable == kBeltMoverVtable))) return false;

    if (trafficMover || kinematic || belt) {
        uintptr_t owner = 0;
        // TrafficMover 0x408740 stores Car itself at +8. Mode-zero 0x604491
        // instead stores Car+40. BeltMover 5A9030 also stores Car itself at
        // +8, and its +20/+24 are scalar motion values, NOT a physics body.
        if (!ReadGameMemory(target.wrapper + 8, &owner, sizeof(owner)) ||
            owner != (kinematic ? car + 0x40 : car) || witnessHasBody != 0 ||
            witnessPhysics != 0 || witnessBody != 0 ||
            !ReadGameMemory(geometry, target.centerOfMass, sizeof(target.centerOfMass)) ||
            !FiniteVector(target.centerOfMass, 100.0f)) return false;
    } else {
        uintptr_t owner = 0, base = 0, wrapperParameters = 0, bodyParameters = 0, bodyVtable = 0;
        float mass = 0, inverseMass = 0, diagonal[3]{};
        if (!ReadGameMemory(target.wrapper + (remoteBody ? 0x34 : 0x5C), &owner, sizeof(owner)) || owner != car ||
            !ReadGameMemory(target.wrapper + (remoteBody ? 0x30 : 0x58), &base, sizeof(base)) || base != car + 0x40 ||
            !ReadGameMemory(target.wrapper + (remoteBody ? 0x2C : 0x54), &wrapperParameters, sizeof(wrapperParameters)) ||
            wrapperParameters != geometry ||
            !ReadGameMemory(target.wrapper + 0x20, &target.body, sizeof(target.body)) || !IsValidUserPtr(target.body) ||
            !ReadGameMemory(target.body, &bodyVtable, sizeof(bodyVtable)) || bodyVtable != kBodyVtable ||
            !ReadGameMemory(target.body + 0x14, &bodyParameters, sizeof(bodyParameters)) || bodyParameters != geometry + 0x110 ||
            witnessHasBody != 1 || witnessPhysics != target.wrapper + 8 || witnessBody != target.body ||
            !ReadGameMemory(target.body + 0x164, &mass, sizeof(mass)) ||
            !ReadGameMemory(target.body + 0x168, &inverseMass, sizeof(inverseMass)) ||
            !ReadGameMemory(target.body + 0x170, &diagonal[0], sizeof(float)) ||
            !ReadGameMemory(target.body + 0x184, &diagonal[1], sizeof(float)) ||
            !ReadGameMemory(target.body + 0x198, &diagonal[2], sizeof(float)) ||
            !ReadGameMemory(remoteBody ? geometry : target.body + 0x1F0,
                target.centerOfMass, sizeof(target.centerOfMass)) ||
            !std::isfinite(mass) || mass <= 0 || mass > 1000000.0f ||
            !std::isfinite(inverseMass) || inverseMass <= 0 ||
            !FiniteVector(diagonal, 1000000.0f) || diagonal[0] <= 0 || diagonal[1] <= 0 || diagonal[2] <= 0 ||
            !FiniteVector(target.centerOfMass, 100.0f) ||
            (target.body < car + 0x524 && target.body + 0x200 > car) ||
            (target.body < localCar + 0x524 && target.body + 0x200 > localCar) ||
            (peerCar && target.body < peerCar + 0x524 && target.body + 0x200 > peerCar) ||
            !WritableRange(target.body + 0x20, 0xAC)) return false;
    }
    if (!ExcludesHumanBody(localCar, target.wrapper, target.body) ||
        !ExcludesHumanBody(peerCar, target.wrapper, target.body) ||
        !WritableRange(car + 0x40, 0x484)) return false;
    result = target;
    return true;
}
}

// Host capture is read-only and does not use the client replica predicate.
// Native 0x5A5540/0x5A2B60 copy body+20 to Car+60, body+30 to Car+90,
// body+A0 to Car+70 and body+C0 to Car+110. These are actual velocities,
// unlike body+70/+80 which hold momentum. Never guess a body for other modes.
bool ReadCoopWorldPhysicalMotion(uintptr_t car, float* position, float* quaternion,
    float* velocity, float* angular) {
    using namespace CoopPhysicalMotionNative;
    using namespace CoopWorldPhysicalMotionNative;
    uintptr_t parameter = 0, ride = 0; uint32_t type = 0;
    if (!position || !quaternion || !velocity || !angular || !IsValidUserPtr(car) ||
        !ReadGameMemory(car + 0x14, &parameter, sizeof(parameter)) || !IsValidUserPtr(parameter) ||
        !ReadGameMemory(car + 0x10, &ride, sizeof(ride)) || ride != parameter + 0x10 ||
        !ReadGameMemory(parameter + 4, &type, 4) || (type != 2 && type != 3)) { ++captureRefused; return false; }
    float p[3]{}, q[4]{}, v[3]{}, a[3]{};
    if (ReadGameMemory(car + 0x60, p, sizeof(p)) && FiniteVector(p, 1000000.0f) &&
        ReadCoopPhysicalMotion(car, q, v, a)) {
        std::memcpy(position, p, sizeof(p)); std::memcpy(quaternion, q, sizeof(q));
        std::memcpy(velocity, v, sizeof(v)); std::memcpy(angular, a, sizeof(a));
        ++captureFromCache; return true;
    }
    uint32_t mode = 0, hasBody = 0;
    uintptr_t wrapper = 0, wrapperVtable = 0, body = 0, bodyVtable = 0;
    uintptr_t owner = 0, base = 0, geometry = 0, embeddedGeometry = 0, wrapperGeometry = 0, bodyParameters = 0;
    uintptr_t witness = 0, witnessVtable = 0, witnessOwner = 0, witnessPhysics = 0, witnessBody = 0;
    float matrix[16]{};
    if (!ReadGameMemory(car + 0x28, &mode, 4) || (mode != 1 && mode != 3) || (type == 3 && mode != 3) ||
        !ReadGameMemory(car + 0x34, &wrapper, sizeof(wrapper)) || !IsValidUserPtr(wrapper) ||
        !ReadGameMemory(wrapper, &wrapperVtable, sizeof(wrapperVtable)) ||
        wrapperVtable != (mode == 3 ? kRemotePhysicsVtable : kPhysicsVtable) ||
        !ReadGameMemory(car + 0x20, &geometry, sizeof(geometry)) || geometry != ride + 0x40 ||
        !ReadGameMemory(car + 0x40, &embeddedGeometry, sizeof(embeddedGeometry)) || embeddedGeometry != geometry ||
        !ReadGameMemory(wrapper + (mode == 3 ? 0x34 : 0x5C), &owner, sizeof(owner)) || owner != car ||
        !ReadGameMemory(wrapper + (mode == 3 ? 0x30 : 0x58), &base, sizeof(base)) || base != car + 0x40 ||
        !ReadGameMemory(wrapper + (mode == 3 ? 0x2C : 0x54), &wrapperGeometry, sizeof(wrapperGeometry)) ||
        wrapperGeometry != geometry ||
        !ReadGameMemory(wrapper + 0x20, &body, sizeof(body)) || !IsValidUserPtr(body) ||
        !ReadGameMemory(body, &bodyVtable, sizeof(bodyVtable)) || bodyVtable != kBodyVtable ||
        !ReadGameMemory(body + 0x14, &bodyParameters, sizeof(bodyParameters)) || bodyParameters != geometry + 0x110 ||
        !ReadGameMemory(car + 0x3C, &witness, sizeof(witness)) || !IsValidUserPtr(witness) ||
        !ReadGameMemory(witness, &witnessVtable, sizeof(witnessVtable)) || witnessVtable != kWitnessVtable ||
        !ReadGameMemory(witness + 0x130, &witnessOwner, sizeof(witnessOwner)) || witnessOwner != car ||
        !ReadGameMemory(witness + 0x5C, &hasBody, 4) || hasBody != 1 ||
        !ReadGameMemory(witness + 0x64, &witnessPhysics, sizeof(witnessPhysics)) || witnessPhysics != wrapper + 8 ||
        !ReadGameMemory(witness + 0x68, &witnessBody, sizeof(witnessBody)) || witnessBody != body ||
        !ReadGameMemory(body + 0x20, p, sizeof(p)) || !FiniteVector(p, 1000000.0f) ||
        !ReadGameMemory(body + 0x30, matrix, sizeof(matrix)) || !coop_motion::MatrixToQuaternion(matrix, q) ||
        !ReadGameMemory(body + 0xA0, v, sizeof(v)) || !FiniteVector(v, 500.0f) ||
        !ReadGameMemory(body + 0xC0, a, sizeof(a)) || !FiniteVector(a, 100.0f)) {
        ++captureRefused; return false;
    }
    std::memcpy(position, p, sizeof(p)); std::memcpy(quaternion, q, sizeof(q));
    std::memcpy(velocity, v, sizeof(v)); std::memcpy(angular, a, sizeof(a));
    ++captureFromBody; return true;
}

bool ApplyCoopWorldPhysicalMotion(uintptr_t car, uintptr_t localCar, const float* position,
    const float* quaternion, const float* velocity, const float* angular) {
    using namespace CoopPhysicalMotionNative;
    if (!FiniteVector(position, 1000000.0f) || !FiniteVector(velocity, 500.0f) ||
        !FiniteVector(angular, 100.0f) || !quaternion || !coop_motion::ValidQuaternion(quaternion)) return false;
    CoopWorldPhysicalMotionNative::Target target{};
    float matrix[16]{};
    if (!CoopWorldPhysicalMotionNative::ReadTarget(car, localCar, target) ||
        !coop_motion::QuaternionToMatrix(quaternion, matrix) || !coop_motion::IsRotation(matrix)) return false;
    // Recheck authority immediately before writes, independently of the native
    // layout checks above. A role/session transition cannot retain old authority.
    uintptr_t authorizedRide = 0; uint32_t authorizedKind = 0;
    if (!ReadManagedCoopAI(car, authorizedRide, authorizedKind) || authorizedRide != target.ride ||
        authorizedKind != target.kind || localCar != CoopRaceCar(false) || car == CoopRaceCar(true)) return false;
    __try {
        if (!SafeWriteBytes(car + 0x60, position, sizeof(float) * 3) ||
            !SafeWriteBytes(car + 0x90, matrix, sizeof(matrix)) ||
            !SafeWriteBytes(car + 0x70, velocity, sizeof(float) * 3) ||
            !SafeWriteBytes(car + 0x110, angular, sizeof(float) * 3)) return false;
        if (target.body)
            setBodyState(reinterpret_cast<void*>(target.body), position, matrix, velocity, angular, nullptr);
        deriveOrigin(reinterpret_cast<void*>(car + 0x40), target.centerOfMass);
        updateRender(reinterpret_cast<void*>(car + 0x40));
        setVelocity(reinterpret_cast<void*>(car + 0x40), velocity);
        // The native minimap resolver (4AC300) reads the uint16 yaw at
        // Car+40C, not the render matrix. Keep it in step with the accepted
        // full pose; never call terrain placement (which would flatten it).
        const float horizontalForward = matrix[0] * matrix[0] + matrix[1] * matrix[1];
        if (horizontalForward > 1.0e-8f) {
            constexpr double twoPi = 6.28318530717958647692;
            double yaw = std::atan2(static_cast<double>(matrix[1]), static_cast<double>(matrix[0]));
            if (yaw < 0.0) yaw += twoPi;
            const uint16_t heading = static_cast<uint16_t>(
                static_cast<uint32_t>(yaw * (65536.0 / twoPi)) & 0xFFFFu);
            if (!SafeWriteBytes(car + 0x40C, &heading, sizeof(heading))) return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (target.kind == 3) ++CoopWorldPhysicalMotionNative::trafficApplied;
    else if (target.body) ++CoopWorldPhysicalMotionNative::bodyApplied;
    else ++CoopWorldPhysicalMotionNative::kinematicApplied;
    return true;
}

// AI controller suppression is an identity/authority decision, not permission
// to write an arbitrary native physics layout. Client distant/countdown modes
// may legitimately have no writable body; allowing their driver to run would
// start a second independent AI simulation. Host/SP always fail authorization.
bool ShouldSuppressCoopAIController(uintptr_t object) {
    using namespace CoopWorldPhysicalMotionNative;
    uintptr_t ride = 0, car = 0, vtable = 0, boundDriver = 0, parameter = 0;
    uint32_t kind = 0, role = 0, actualKind = 0;
    if (ReadManagedCoopAI(object, ride, kind)) {
        return ReadGameMemory(object + 0x14, &parameter, sizeof(parameter)) &&
            ride == parameter + 0x10 && ReadGameMemory(parameter + 4, &actualKind, 4) && actualKind == kind;
    }
    if (!IsValidUserPtr(object) || !ReadGameMemory(object, &vtable, sizeof(vtable)) ||
        (vtable != kRacerDriverVtable && vtable != kSpecialRacerDriverVtable && vtable != kTrafficDriverVtable) ||
        !ReadGameMemory(object + 4, &car, sizeof(car)) || !ReadManagedCoopAI(car, ride, kind) ||
        !ReadGameMemory(car + 0x14, &parameter, sizeof(parameter)) || ride != parameter + 0x10 ||
        !ReadGameMemory(parameter + 4, &actualKind, 4) || actualKind != kind ||
        !ReadGameMemory(car + 0x2C, &boundDriver, sizeof(boundDriver)) || boundDriver != object ||
        !ReadGameMemory(car + 0x24, &role, 4)) return false;
    return (kind == 2 && ((role == 3 && vtable == kRacerDriverVtable) ||
                         (role == 2 && vtable == kSpecialRacerDriverVtable))) ||
           (kind == 3 && role == 4 && vtable == kTrafficDriverVtable);
}

// Read-only predicate for identity-scoped driver/mover hooks. Unknown objects,
// ordinary AI, host simulation and single-player always preserve native ticks.
// Accepts a Car*, verified AI Driver*, TrafficMover* or body-free BeltMover*.
bool ShouldSuppressCoopAITick(uintptr_t object) {
    using namespace CoopWorldPhysicalMotionNative;
    Target target{};
    const uintptr_t localCar = CoopRaceCar(false);
    if (ReadTarget(object, localCar, target)) return true;
    uintptr_t vtable = 0, car = 0, boundObject = 0;
    uint32_t role = 0;
    if (!IsValidUserPtr(object) || !ReadGameMemory(object, &vtable, sizeof(vtable))) return false;
    if (vtable == kTrafficMoverVtable) {
        return ReadGameMemory(object + 8, &car, sizeof(car)) && ReadTarget(car, localCar, target) &&
            target.kind == 3 && target.mode == 2 && target.wrapper == object;
    }
    if (vtable == kBeltMoverVtable) {
        return ReadGameMemory(object + 8, &car, sizeof(car)) && ReadTarget(car, localCar, target) &&
            target.mode == 4 && target.wrapper == object;
    }
    if (vtable != kTrafficDriverVtable && vtable != kRacerDriverVtable &&
        vtable != kSpecialRacerDriverVtable) return false;
    if (!ReadGameMemory(object + 4, &car, sizeof(car)) || !ReadTarget(car, localCar, target) ||
        !ReadGameMemory(car + 0x2C, &boundObject, sizeof(boundObject)) || boundObject != object ||
        !ReadGameMemory(car + 0x24, &role, sizeof(role))) return false;
    return (target.kind == 3 && role == 4 && vtable == kTrafficDriverVtable) ||
        (target.kind == 2 && ((role == 3 && vtable == kRacerDriverVtable) ||
                            (role == 2 && vtable == kSpecialRacerDriverVtable)));
}
