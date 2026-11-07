// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Native OutRun entry is process-wide, although the co-op world has two human
// Cars. Keep the stock manager/scoring lifecycle and bind its two contestants
// to one granted entity generation. Never turn a Client replica into an AI.
namespace CoopFleetNative {
using OutrunScoreFn = void(__thiscall*)(void*);
using OutrunRoadProjectionFn = float(__thiscall*)(void*, const float*, uint32_t);
static OutrunScoreFn sortOutrunScores = reinterpret_cast<OutrunScoreFn>(0x005EEC80);
static OutrunScoreFn sortNativeRaceRows = reinterpret_cast<OutrunScoreFn>(0x005EEC80);
static OutrunScoreFn measureOutrunScore = reinterpret_cast<OutrunScoreFn>(0x005FC510);
static OutrunRoadProjectionFn projectOutrunRoadPosition = reinterpret_cast<OutrunRoadProjectionFn>(0x0040CBC0);
static bool outrunResultMeasured=false;
static bool OutrunSortReady(uintptr_t address) {
    constexpr uint8_t bytes[]{0x83,0xEC,0x08,0x53,0x55,0x56,0x57,0x8B,0xF9};
    return IsValidUserPtr(address) && CoopAINativePlacementHooks::ReadSignature(address, bytes, sizeof(bytes));
}
struct SpectatorScore {
    uintptr_t row = 0, car = 0, parameter = 0;
    uint8_t ranks[2]{}, finishOrder = 0;
    uint16_t status = 0;
    float meters = 0;
};
static SpectatorScore spectatorScores[2]{};
static uint32_t spectatorCount = 0;
static bool OutrunScoringReady() {
    constexpr uint8_t measureBytes[]{0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x44,0x53,0x8B,0xD9};
    constexpr uint8_t projectionBytes[]{0x8B,0x44,0x24,0x08,0x83,0xEC,0x08,0x85,0xC0,0x56,0x8B,0xF1};
    return sortOutrunScores && measureOutrunScore && projectOutrunRoadPosition &&
        OutrunSortReady(reinterpret_cast<uintptr_t>(sortOutrunScores)) &&
        CoopAINativePlacementHooks::ReadSignature(reinterpret_cast<uintptr_t>(measureOutrunScore), measureBytes, sizeof(measureBytes)) &&
        CoopAINativePlacementHooks::ReadSignature(reinterpret_cast<uintptr_t>(projectOutrunRoadPosition), projectionBytes, sizeof(projectionBytes));
}
static bool ScoreRows(uintptr_t manager, uintptr_t rows[6], uint32_t& count) {
    count = 0;
    if (!IsValidUserPtr(manager) || !ReadGameMemory(manager + 0x24, &count, 4) || !count || count > 6 ||
        !ReadGameMemory(manager + 0xBD0, rows, count * sizeof(uintptr_t)) ||
        !CoopPhysicalMotionNative::WritableRange(manager + 0x28, 4) ||
        !CoopPhysicalMotionNative::WritableRange(manager + 0xBD0, count * sizeof(uintptr_t))) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t car = 0, back = 0, parameter = 0; uint32_t kind = 0;
        const uintptr_t begin = manager + 0x30;
        if (rows[i] < begin || rows[i] >= begin + 6 * 0x1F0 || (rows[i] - begin) % 0x1F0 ||
            !ReadGameMemory(rows[i], &car, sizeof(car)) || !IsValidUserPtr(car) ||
            !ReadGameMemory(car + 0x1C, &back, sizeof(back)) || back != rows[i] ||
            !ReadGameMemory(car + 0x14, &parameter, sizeof(parameter)) || !IsValidUserPtr(parameter) ||
            !ReadGameMemory(parameter + 4, &kind, 4) || (kind != 1 && kind != 2) ||
            !CoopPhysicalMotionNative::WritableRange(rows[i] + 0xA, 3) ||
            !CoopPhysicalMotionNative::WritableRange(rows[i] + 0x10, 2) ||
            !CoopPhysicalMotionNative::WritableRange(rows[i] + 0x24, 4)) return false;
        for (uint32_t before = 0; before < i; ++before) if (rows[before] == rows[i]) return false;
    }
    return true;
}
static bool CaptureSpectators(const PendingOutrun& selected, const uintptr_t rows[6], uint32_t count) {
    spectatorCount = 0;
    for (uint32_t i = 0; i < count; ++i) {
        SpectatorScore saved{}; saved.row = rows[i];
        if (!ReadGameMemory(saved.row, &saved.car, sizeof(saved.car))) return false;
        if (saved.car == selected.human) continue;
        if (spectatorCount >= 2 || (saved.car != CoopRaceCar(false) && saved.car != CoopRaceCar(true)) ||
            !ReadGameMemory(saved.car + 0x14, &saved.parameter, sizeof(saved.parameter)) ||
            !ReadGameMemory(saved.row + 0xA, saved.ranks, sizeof(saved.ranks)) ||
            !ReadGameMemory(saved.row + 0xC, &saved.finishOrder, 1) ||
            !ReadGameMemory(saved.row + 0x10, &saved.status, 2) ||
            !ReadGameMemory(saved.row + 0x24, &saved.meters, 4)) return false;
        spectatorScores[spectatorCount++] = saved;
    }
    return true;
}
static void RestoreSpectators(const PendingOutrun& selected) {
    if (CurrentOutrun(selected)) for (uint32_t i = 0; i < spectatorCount; ++i) {
        const auto& saved = spectatorScores[i]; uintptr_t car = 0, back = 0, parameter = 0;
        if ((saved.car != CoopRaceCar(false) && saved.car != CoopRaceCar(true)) ||
            !ReadGameMemory(saved.row, &car, sizeof(car)) || car != saved.car ||
            !ReadGameMemory(car + 0x1C, &back, sizeof(back)) || back != saved.row ||
            !ReadGameMemory(car + 0x14, &parameter, sizeof(parameter)) || parameter != saved.parameter) continue;
        SafeWriteBytes(saved.row + 0xA, saved.ranks, sizeof(saved.ranks));
        SafeWriteBytes(saved.row + 0xC, &saved.finishOrder, 1);
        SafeWriteBytes(saved.row + 0x10, &saved.status, 2);
        SafeWriteBytes(saved.row + 0x24, &saved.meters, 4);
    }
    spectatorCount = 0;
}
static bool SortOutrun(const PendingOutrun& selected) {
    if (!OutrunScoringReady()) return false;
    __try { sortOutrunScores(reinterpret_cast<void*>(selected.manager)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
static bool BindContestants(const PendingOutrun& selected) {
    uintptr_t rows[6]{}, ordered[6]{}, humanScore = 0, opponentScore = 0;
    uint32_t count = 0, excluded = 0;
    if (!ScoreRows(selected.manager, rows, count)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t car = 0; ReadGameMemory(rows[i], &car, sizeof(car));
        if (car == selected.human) humanScore = rows[i];
        if (car == selected.car) opponentScore = rows[i];
    }
    if (!humanScore || !opponentScore || humanScore == opponentScore) return false;
    // 42ED F0 state 4 reads rank 1 as the leader and rank 2 as its opponent.
    // Comparator 5EEC00 does NOT examine the excluded status at +10. It sorts
    // signed finish-order +C before signed meters +24. Keep spectators after
    // both contestants even when their old distance would otherwise win.
    // Preserve +24's allocation count: reset/destruction uses inline rows.
    ordered[0] = humanScore; ordered[1] = opponentScore; uint32_t next = 2;
    for (uint32_t i = 0; i < count; ++i) if (rows[i] != humanScore && rows[i] != opponentScore) ordered[next++] = rows[i];
    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t status = rows[i] == humanScore || rows[i] == opponentScore ? 0 : 7;
        const uint8_t finishOrder = status ? 127 : 0;
        if (!SafeWriteBytes(rows[i] + 0x10, &status, 2) ||
            !SafeWriteBytes(rows[i] + 0xC, &finishOrder, 1)) return false;
        if (!status) { const float meters = 0; if (!SafeWriteBytes(rows[i] + 0x24, &meters, 4)) return false; }
        if (status) ++excluded;
    }
    return SafeWriteBytes(selected.manager + 0xBD0, ordered, count * sizeof(uintptr_t)) &&
        SafeWriteBytes(selected.manager + 0x28, &excluded, 4) && SortOutrun(selected);
}
static bool MeterDriver(const PendingOutrun& selected, uintptr_t car, uintptr_t& driver) {
    uintptr_t road = 0, owner = 0, vtable = 0; uint8_t driverFields[0x14]{}, roadFields[0x1C]{};
    // Only the real local human or the Host's typed native opponent can lend
    // its road locator. A remote human/Client replica never supplies a driver.
    if (car != CoopRaceCar(false) && !(Scope(coop::Role::Host, false) && car == selected.car && Ambient(car, driver))) return false;
    return ReadGameMemory(car + 0x2C, &driver, sizeof(driver)) && IsValidUserPtr(driver) &&
        ReadGameMemory(driver, driverFields, sizeof(driverFields)) &&
        ReadGameMemory(driver, &vtable, sizeof(vtable)) && vtable != 0x007A5CA8 &&
        ReadGameMemory(driver + 4, &owner, sizeof(owner)) && owner == car &&
        ReadGameMemory(driver + 0xC, &road, sizeof(road)) && IsValidUserPtr(road) &&
        ReadGameMemory(road, roadFields, sizeof(roadFields));
}
static bool UpdateContestantMeters(const PendingOutrun& selected) {
    uintptr_t rows[6]{}, humanScore = 0, opponentScore = 0; uint32_t count = 0;
    if (!CurrentOutrun(selected) || !ScoreRows(selected.manager, rows, count) || !OutrunScoringReady()) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t car = 0; ReadGameMemory(rows[i], &car, sizeof(car));
        if (car == selected.human) humanScore = rows[i];
        else if (car == selected.car) opponentScore = rows[i];
        else { const uint8_t excluded = 127; if (!SafeWriteBytes(rows[i] + 0xC, &excluded, 1)) return false; }
    }
    if (!humanScore || !opponentScore || (rows[0] != humanScore && rows[0] != opponentScore) ||
        (rows[1] != humanScore && rows[1] != opponentScore)) return false;
    uint16_t humanStatus = 0, opponentStatus = 0; uint8_t humanFinish = 0, opponentFinish = 0;
    if (!ReadGameMemory(humanScore + 0x10, &humanStatus, 2) ||
        !ReadGameMemory(opponentScore + 0x10, &opponentStatus, 2) ||
        !ReadGameMemory(humanScore + 0xC, &humanFinish, 1) || !ReadGameMemory(opponentScore + 0xC, &opponentFinish, 1)) return false;
    // Never revive a native winner/loser. Use EXACT signed native projection
    // from 5FC6B1..5FC6D7: road+40CBC0(leader) minus road+40CBC0(contender),
    // with the native inverse driver+10 direction flag. The existing LOCAL
    // contestant supplies its full driver+C road locator when BD0 is dormant.
    // Calling 5FC510 with a substituted BD0 loses the sign beyond its 30m
    // locator check, while borrowing a driver leaves its <=10m/s vector
    // fallback unable to sign a stopped/slow overtake. The native projection
    // itself works at every speed and writes no Car, driver, or road fields.
    if (!humanStatus && !opponentStatus && !humanFinish && !opponentFinish) {
        const uintptr_t anchorCar = selected.human == CoopRaceCar(false) ? selected.human : selected.car;
        uintptr_t leaderCar = 0, driver = 0;
        if (!ReadGameMemory(rows[0], &leaderCar, sizeof(leaderCar))) return false;
        const bool nativeLeader = MeterDriver(selected, leaderCar, driver);
        if (nativeLeader || MeterDriver(selected, anchorCar, driver)) {
            uintptr_t road = 0; uint8_t direction = 0; float humanPosition[3]{}, opponentPosition[3]{};
            if (!ReadGameMemory(driver + 0xC, &road, sizeof(road)) ||
                !ReadGameMemory(driver + 0x10, &direction, 1) ||
                !ReadGameMemory(selected.human + 0x60, humanPosition, sizeof(humanPosition)) ||
                !ReadGameMemory(selected.car + 0x60, opponentPosition, sizeof(opponentPosition)) ||
                !CoopPhysicalMotionNative::FiniteVector(humanPosition, 1000000.0f) ||
                !CoopPhysicalMotionNative::FiniteVector(opponentPosition, 1000000.0f)) return false;
            const float* leaderPosition = rows[0] == humanScore ? humanPosition : opponentPosition;
            const float* contenderPosition = rows[0] == humanScore ? opponentPosition : humanPosition;
            float leaderProjection = 0, contenderProjection = 0; bool called = false;
            __try {
                leaderProjection = projectOutrunRoadPosition(reinterpret_cast<void*>(road), leaderPosition, !direction);
                contenderProjection = projectOutrunRoadPosition(reinterpret_cast<void*>(road), contenderPosition, !direction);
                called = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            const float gap = leaderProjection - contenderProjection, zero = 0;
            if (!called || !std::isfinite(gap) || std::fabs(gap) > 2000000.0f || !CurrentOutrun(selected) ||
                !SafeWriteBytes(rows[0] + 0x24, &zero, 4) || !SafeWriteBytes(rows[1] + 0x24, &gap, 4)) return false;
        } else return false;
    }
    return SortOutrun(selected);
}
static bool EnterSelectedOutrun(const PendingOutrun& selected) {
    uint32_t mode = 0, state=0, counts[4]{}; uintptr_t view = 0, oldHuman = 0, oldOpponent = 0, rows[6]{};
    uint32_t scoreCount = 0;
    if (engagement.car || !enterOutrun || !resetOutrun || !CurrentOutrun(selected) || !CoopSnapshot().fresh ||
        !port.outrunApproved || !port.outrunApproved(selected.car, selected.human) ||
        !ReadGameMemory(addresses.gameMode, &mode, 4) || mode != 0 ||
        !ReadGameMemory(addresses.firstView, &view, sizeof(view)) || !IsValidUserPtr(view) ||
        !ReadGameMemory(view + 4, &oldHuman, sizeof(oldHuman)) || oldHuman != CoopRaceCar(false) ||
        !ReadGameMemory(view + 0x250, &oldOpponent, sizeof(oldOpponent)) ||
        !ReadGameMemory(addresses.participantCounts, counts, sizeof(counts)) ||
        !ScoreRows(selected.manager, rows, scoreCount) || scoreCount > 2 || !OutrunScoringReady() ||
        !CoopPhysicalMotionNative::WritableRange(view + 4, sizeof(uintptr_t)) ||
        !CoopPhysicalMotionNative::WritableRange(view + 0x250, sizeof(uintptr_t)) ||
        !CoopPhysicalMotionNative::WritableRange(addresses.participantCounts, sizeof(counts)) ||
        !CaptureSpectators(selected, rows, scoreCount)) return false;
    // 6071C5 overwrites C0 with the two native human reservations. The stock
    // C0==2 && C4==0 guard at 6072C1 then bypasses opponent admission. Present
    // its real local AI contestant during this synchronous entry, and restore
    // all additive participant counts before returning to world/render code.
    const uint32_t localAI = 1;
    if (!SafeWriteBytes(addresses.participantCounts + 4, &localAI, 4) ||
        !SafeWriteBytes(view + 4, &selected.human, sizeof(selected.human)) ||
        !SafeWriteBytes(view + 0x250, &selected.car, sizeof(selected.car))) {
        SafeWriteBytes(addresses.participantCounts, counts, sizeof(counts));
        SafeWriteBytes(view + 4, &oldHuman, sizeof(oldHuman));
        SafeWriteBytes(view + 0x250, &oldOpponent, sizeof(oldOpponent));
        spectatorCount = 0;
        return false;
    }
    starting = selected; bool called = false;
    __try { enterOutrun(reinterpret_cast<void*>(selected.manager)); called = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    starting = {};
    bool restored = SafeWriteBytes(addresses.participantCounts, counts, sizeof(counts));
    restored = SafeWriteBytes(view + 4, &oldHuman, sizeof(oldHuman)) && restored;
    restored = SafeWriteBytes(view + 0x250, &oldOpponent, sizeof(oldOpponent)) && restored;
    if (!called || !restored || !ReadGameMemory(addresses.gameMode, &mode, 4) || mode != 7 ||
        !ReadGameMemory(selected.manager+0xC40,&state,4) || state!=4 ||
        !BindContestants(selected)) {
        __try { resetOutrun(reinterpret_cast<void*>(selected.manager)); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        RestoreSpectators(selected);
        return false;
    }
    engagement = selected; outrunResultMeasured=false; ++outrunResumed;
    char message[220]{};
    std::snprintf(message, sizeof(message), "[FLEET_OUTRUN] Native entry role=%s owner=%s entity=%llu generation=%u dormantClient=%u\r\n",
        CoopSnapshot().role == coop::Role::Host ? "host" : "client", selected.human == CoopRaceCar(false) ? "local" : "peer",
        static_cast<unsigned long long>(selected.key.id), selected.key.generation, CoopSnapshot().role == coop::Role::Client);
    LogMessage(message); return true;
}

static bool OwnsOutrunScorer(uintptr_t manager) {
    uint32_t mode=0,track=0;
    return engagement.car && engagement.manager==manager && IsCoopGameplayActive() &&
        ReadGameMemory(addresses.track,&track,4) && track==4000 &&
        ReadGameMemory(addresses.gameMode,&mode,4) && mode==7;
}

static bool HoldOutrunResult(uintptr_t manager) {
    // The Host drives the AI in either role. The challenger alone commits
    // the win or loss and ends its lease after the native result is applied.
    return OwnsOutrunScorer(manager) && (!outrunResultMeasured || engagement.human!=CoopRaceCar(false));
}
}

static void __fastcall HookedCoopFleetOutrunMeasure(void* score,void*) {
    using namespace CoopFleetNative;
    uintptr_t manager=0;
    if(!engagement.car || !ReadGameMemory(addresses.raceManager,&manager,sizeof(manager)) || !OwnsOutrunScorer(manager))
        measureOutrunScore(score);
}

static void __fastcall HookedCoopFleetOutrunSort(void* manager,void*) {
    using namespace CoopFleetNative;
    if(OwnsOutrunScorer(reinterpret_cast<uintptr_t>(manager))) {
        // 60791A is in the same native update as the 300 m finish test.
        // Updating after it allows stock's unsigned measurement to award a
        // false win to the old leader before the co-op game-thread tick.
        outrunResultMeasured=port.outrunApproved && port.outrunApproved(engagement.car,engagement.human) &&
            UpdateContestantMeters(engagement);
        return;
    }
    // Preserve the normal race-finish detour and unmodified SP sorting.
    sortNativeRaceRows(manager);
}

bool SetCoopFleetOutrunNativeSort(uintptr_t address) {
    // The race-finish hook replaces 5EEC80. Use its verified original-code
    // trampoline, whose displaced nine bytes have this same native signature.
    if (!CoopFleetNative::OutrunSortReady(address)) return false;
    CoopFleetNative::sortOutrunScores = reinterpret_cast<CoopFleetNative::OutrunScoreFn>(address);
    return true;
}
bool IsCoopFleetOutrunActive(uintptr_t ambientCar, uintptr_t humanCar) {
    using namespace CoopFleetNative; uint32_t mode = 0;
    return engagement.car == ambientCar && engagement.human == humanCar && CurrentOutrun(engagement) &&
        ReadGameMemory(addresses.gameMode, &mode, 4) && mode == 7;
}
void TickCoopFleetOutrunNative() {
    using namespace CoopFleetNative;
    if (pending.car && !CurrentOutrun(pending)) pending = {};
    if (!engagement.car) return;
    uint32_t mode = 0;
    if (!CurrentOutrun(engagement)) { engagement = {}; spectatorCount = 0; return; }
    if (!ReadGameMemory(addresses.gameMode, &mode, 4)) return;
    if (mode != 7) { RestoreSpectators(engagement); engagement = {}; return; }
    if (port.outrunApproved && port.outrunApproved(engagement.car, engagement.human)) return;
    // A released/expired grant cannot continue steering the peer's opponent.
    // The pointer/session/generation checks above prevent stale-manager calls.
    __try { resetOutrun(reinterpret_cast<void*>(engagement.manager)); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    RestoreSpectators(engagement);
    engagement = {};
    LogMessage("[FLEET_OUTRUN] Native engagement ended after authoritative lease release/expiry.\r\n");
}
bool StartCoopFleetHostOutrun(uintptr_t ambientCar, uintptr_t humanCar) {
    using namespace CoopFleetNative;
    if (IsCoopFleetOutrunActive(ambientCar, humanCar)) return true;
    uint8_t opponentActive=0;
    if (!Scope(coop::Role::Host) || !Human(humanCar, true) || !SelectedAmbient(ambientCar) ||
        !ReadGameMemory(ambientCar+0x550,&opponentActive,1) || opponentActive!=1 ||
        !port.worldEpoch || !port.outrunKey) return false;
    PendingOutrun selected{}; selected.car = ambientCar; selected.human = humanCar;
    if (!ReadGameMemory(addresses.raceManager, &selected.manager, sizeof(selected.manager)) ||
        !ReadGameMemory(ambientCar + 0x14, &selected.parameter, sizeof(selected.parameter)) ||
        !port.outrunKey(ambientCar, selected.key)) return false;
    selected.session = CoopSnapshot().sessionId; selected.epoch = port.worldEpoch(); selected.requested = GetTickCount();
    return EnterSelectedOutrun(selected);
}
