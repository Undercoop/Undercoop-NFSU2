// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Include after the appearance readers and CoopAIWorldRuntime.inl. This is a
// game-thread diagnostic only: no engine calls, native writes or AI bindings.
namespace CoopTrafficInventory {
constexpr unsigned kLimit = 22;
constexpr size_t kParameterStride = 0x940;
struct Addresses {
    uintptr_t parameterArray = 0x0089E810;
    uintptr_t parameterCount = 0x0089E814;
    uintptr_t carSlots = 0x0089CCF8;
    uintptr_t track = 0x0089E7A0;
};
static Addresses addresses{};
static DWORD NativeNow() { return GetTickCount(); }
struct Api {
    bool (*read)(uintptr_t, void*, SIZE_T) = ReadGameMemory;
    bool (*validPointer)(uintptr_t) = IsValidUserPtr;
    bool (*validModel)(uint32_t) = IsSupportedCarModel;
    bool (*appearance)(uintptr_t, uint8_t*) = ReadNativeAppearancePortable;
    uint64_t (*fingerprint)(const uint8_t*) = ComputePeerAppearanceFingerprint;
    bool (*coop)() = IsCoopGameplayActive;
    DWORD (*now)() = NativeNow;
    void (*log)(const char*) = LogMessage;
};
static Api api{};
enum class Refusal : uint8_t {
    None, ParameterUnreadable, InvalidModel, AppearanceUnreadable,
    MissingCar, DuplicateCar, CarUnreadable, RideMismatch
};
enum class Scope : uint8_t { Ready, Unreadable, InvalidCount, InvalidArray, Changed };
struct Row {
    uintptr_t parameter = 0, ride = 0, car = 0, mover = 0, moverVtable = 0, visual = 0;
    uint32_t ordinal = 0, pendingModel = 0, model = 0, mode = 0, transition = 0, matches = 0;
    uint64_t appearanceHash = 0;
    uint8_t nativeId = 0, paint = 0, active = 0;
    Refusal refusal = Refusal::None;
};
struct Snapshot {
    uintptr_t parameterArray = 0;
    uint32_t track = 0, parameterCount = 0, rowCount = 0;
    uint32_t nullSlots = 0, unreadableCars = 0, staleCars = 0, duplicateSlots = 0;
    uint32_t unreadableParameters = 0;
    Scope scope = Scope::Unreadable;
    Row rows[kLimit]{};
};
static const char* Reason(Refusal value) {
    switch (value) {
    case Refusal::None: return "ok";
    case Refusal::ParameterUnreadable: return "parameter_unreadable";
    case Refusal::InvalidModel: return "invalid_model";
    case Refusal::AppearanceUnreadable: return "appearance_unreadable";
    case Refusal::MissingCar: return "missing_car";
    case Refusal::DuplicateCar: return "duplicate_car";
    case Refusal::CarUnreadable: return "car_unreadable";
    case Refusal::RideMismatch: return "ride_mismatch";
    }
    return "unknown";
}
static const char* ScopeName(Scope value) {
    switch (value) {
    case Scope::Ready: return "stable";
    case Scope::Unreadable: return "unreadable_globals";
    case Scope::InvalidCount: return "invalid_count";
    case Scope::InvalidArray: return "invalid_array";
    case Scope::Changed: return "changed_generation";
    }
    return "unknown";
}
static bool Capture(Snapshot& result) {
    result = Snapshot{};
    if (!api.read || !api.validPointer || !api.validModel || !api.appearance || !api.fingerprint ||
        !api.read(addresses.parameterArray, &result.parameterArray, sizeof(uintptr_t)) ||
        !api.read(addresses.parameterCount, &result.parameterCount, 4) ||
        !api.read(addresses.track, &result.track, 4)) return false;
    if (!result.parameterCount || result.parameterCount > kLimit) {
        result.scope = Scope::InvalidCount; return false;
    }
    const size_t span = result.parameterCount * kParameterStride;
    if (!api.validPointer(result.parameterArray) || result.parameterArray + span < result.parameterArray ||
        !api.validPointer(result.parameterArray + span - 1)) {
        result.scope = Scope::InvalidArray; return false;
    }
    struct CarIdentity { uintptr_t car = 0, parameter = 0, ride = 0; bool readable = false; };
    CarIdentity identities[kLimit]{};
    uintptr_t cars[kLimit]{};
    if (!api.read(addresses.carSlots, cars, sizeof(cars))) return false;
    for (unsigned slot = 0; slot < kLimit; ++slot) {
        auto& item = identities[slot]; item.car = cars[slot];
        if (!item.car) { ++result.nullSlots; continue; }
        for (unsigned prior = 0; prior < slot; ++prior)
            if (cars[prior] == item.car) { ++result.duplicateSlots; break; }
        if (!api.validPointer(item.car) || !api.read(item.car + 0x14, &item.parameter, sizeof(uintptr_t))) {
            ++result.unreadableCars; continue;
        }
        if (item.parameter < result.parameterArray || item.parameter >= result.parameterArray + span ||
            (item.parameter - result.parameterArray) % kParameterStride) ++result.staleCars;
        item.readable = api.read(item.car + 0x10, &item.ride, sizeof(uintptr_t));
        if (!item.readable) ++result.unreadableCars;
    }
    for (uint32_t ordinal = 0; ordinal < result.parameterCount; ++ordinal) {
        const uintptr_t parameter = result.parameterArray + ordinal * kParameterStride;
        uint32_t type = 0;
        if (!api.read(parameter + 4, &type, 4)) { ++result.unreadableParameters; continue; }
        if (type != 3) continue;
        Row& row = result.rows[result.rowCount++];
        row.parameter = parameter; row.ride = parameter + 0x10; row.ordinal = ordinal;
        if (!api.read(parameter + 1, &row.nativeId, 1) ||
            !api.read(parameter + 0x908, &row.pendingModel, 4) ||
            !api.read(row.ride, &row.model, 4) || !api.read(parameter + 0x17, &row.paint, 1))
            row.refusal = Refusal::ParameterUnreadable;
        else if (!api.validModel(row.model)) row.refusal = Refusal::InvalidModel;
        else {
            uint8_t portable[0x940]{};
            if (!api.appearance(row.ride, portable)) row.refusal = Refusal::AppearanceUnreadable;
            else row.appearanceHash = api.fingerprint(portable);
        }
        const CarIdentity* match = nullptr;
        for (const auto& item : identities) {
            // A readable owner pointer is sufficient to detect duplicate or
            // malformed candidates; wrong RideInfo must never become a match.
            if (item.car && item.parameter == parameter) { ++row.matches; match = &item; }
        }
        if (!row.matches) { row.refusal = Refusal::MissingCar; continue; }
        if (row.matches != 1) { row.refusal = Refusal::DuplicateCar; continue; }
        row.car = match->car;
        if (!match->readable) { row.refusal = Refusal::CarUnreadable; continue; }
        if (match->ride != row.ride) { row.refusal = Refusal::RideMismatch; continue; }
        if (!api.read(row.car + 0x28, &row.mode, 4) || !api.read(row.car + 0x550, &row.active, 1) ||
            !api.read(row.car + 0x34, &row.mover, sizeof(uintptr_t)) ||
            !api.validPointer(row.mover) || !api.read(row.mover, &row.moverVtable, sizeof(uintptr_t)) ||
            !api.read(row.car + 0x554, &row.visual, sizeof(uintptr_t)) ||
            !api.read(row.car + 0x96C, &row.transition, 4)) row.refusal = Refusal::CarUnreadable;
    }
    uintptr_t finalArray = 0; uint32_t finalCount = 0, finalTrack = 0;
    if (!api.read(addresses.parameterArray, &finalArray, sizeof(uintptr_t)) ||
        !api.read(addresses.parameterCount, &finalCount, 4) || !api.read(addresses.track, &finalTrack, 4)) return false;
    if (finalArray != result.parameterArray || finalCount != result.parameterCount || finalTrack != result.track) {
        result.scope = Scope::Changed; return false;
    }
    result.scope = Scope::Ready; return true;
}
static DWORD lastTrace = 0;
static bool haveTraced = false;
} // namespace CoopTrafficInventory

void TraceCoopTrafficInventory(DWORD flow) {
    using namespace CoopTrafficInventory;
    if (!api.coop || !api.now || !api.log || flow != 6 || !api.coop()) {
        haveTraced = false; return;
    }
    const DWORD now = api.now();
    if (haveTraced && now - lastTrace < 5000) return;
    lastTrace = now; haveTraced = true;
    Snapshot snapshot{}; const bool stable = Capture(snapshot);
    unsigned okay = 0, refused = 0, missing = 0, duplicate = 0;
    for (unsigned i = 0; i < snapshot.rowCount; ++i) {
        const auto reason = snapshot.rows[i].refusal;
        if (reason == Refusal::None) ++okay; else ++refused;
        if (reason == Refusal::MissingCar) ++missing;
        if (reason == Refusal::DuplicateCar) ++duplicate;
    }
    char line[500]{};
    std::snprintf(line, sizeof(line),
        "[TRAFFIC_INV] track=%u array=%08llX params=%u traffic=%u ok=%u refused=%u missing=%u duplicate=%u stale_cars=%u unreadable_cars=%u duplicate_slots=%u unreadable_params=%u generation=%s\r\n",
        snapshot.track, static_cast<unsigned long long>(snapshot.parameterArray), snapshot.parameterCount,
        snapshot.rowCount, stable ? okay : 0, stable ? refused : snapshot.rowCount, missing, duplicate,
        snapshot.staleCars, snapshot.unreadableCars, snapshot.duplicateSlots, snapshot.unreadableParameters,
        ScopeName(snapshot.scope));
    api.log(line);
    if (!stable) return; // Do not publish rows from a mixed/native-load scope.
    for (unsigned i = 0; i < snapshot.rowCount; ++i) {
        const auto& row = snapshot.rows[i];
        std::snprintf(line, sizeof(line),
            "[TRAFFIC_INV_CAR] slot=%u id=%u param=%08llX ride=%08llX pending_model=%u actual_model=%u paint=%u appearance=%016llX car=%08llX matches=%u mode=%u active=%u mover=%08llX vt=%08llX visual=%08llX transition=%u result=%s\r\n",
            row.ordinal, static_cast<unsigned>(row.nativeId), static_cast<unsigned long long>(row.parameter),
            static_cast<unsigned long long>(row.ride), row.pendingModel, row.model, static_cast<unsigned>(row.paint),
            static_cast<unsigned long long>(row.appearanceHash), static_cast<unsigned long long>(row.car), row.matches,
            row.mode, static_cast<unsigned>(row.active), static_cast<unsigned long long>(row.mover),
            static_cast<unsigned long long>(row.moverVtable), static_cast<unsigned long long>(row.visual),
            row.transition, Reason(row.refusal));
        api.log(line);
    }
}
