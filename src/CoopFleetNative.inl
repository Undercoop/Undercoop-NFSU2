// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Include after world motion, race placement hooks and the fleet interface.
// Every mutation is gated by the current manifest; no native pointer travels
// over the wire. Null ports leave all native paths unchanged.
namespace CoopFleetNative {
struct Port {
    bool (*clientOwned)(uintptr_t, uintptr_t&, uint32_t&) = nullptr;
    bool (*identity)(uintptr_t, uintptr_t&, uint32_t&) = nullptr;
    uint64_t (*worldEpoch)() = nullptr;
    bool (*hostFocus)(uintptr_t, uintptr_t&) = nullptr;
    bool (*outrunApproved)(uintptr_t, uintptr_t) = nullptr;
    bool (*outrunBusy)(uintptr_t) = nullptr;
    void (*requestOutrun)(uintptr_t, uintptr_t) = nullptr;
    bool (*outrunKey)(uintptr_t, coop_world::EntityKey&) = nullptr;
    bool (*retainedClient)(uintptr_t,uintptr_t&,uint32_t&) = nullptr;
};
struct Addresses {
    uintptr_t candidate = 0x0042FA86, candidateContinue = 0x0042FA8B, candidateSkip = 0x0042FABA;
    uintptr_t trafficMover = 0x00408820, trafficMoverContinue = 0x00408826;
    uintptr_t beltMover = 0x0059EC80, beltMoverContinue = 0x0059EC86;
    uintptr_t placement = 0x0042FA9D, originalPlacement = 0x0042F800;
    uintptr_t nearPlacement = 0x0042E790, weightedPlacement = 0x0042E9F0, weight = 0x00418240;
    uintptr_t visible = 0x005ED5C0;
    uintptr_t outrun = 0x00607180, outrunContinue = 0x00607185;
    uintptr_t outrunMeasure = 0x006016B0, outrunSort = 0x0060791A;
    uintptr_t outrunResult = 0x00607C9C, outrunResultContinue = 0x00607CA3, outrunResultSkip = 0x00608C58;
    uintptr_t resetOutrun = 0x005F8490, participantCounts = 0x0089E7C0;
    uintptr_t resetHumanView = 0x005F851D, resetHumanViewContinue = 0x005F8523;
    uintptr_t resetHumanViewNull = 0x005F8528, resetHumanViewSkip = 0x005F8534;
    uintptr_t resetAmbient = 0x0042CD86, resetAmbientContinue = 0x0042CD8F, resetAmbientSkip = 0x0042CDA0;
    uintptr_t follow = 0x00428630, followContinue = 0x00428635;
    uintptr_t scannerPlacement = 0x0042A650, scannerPlacementContinue = 0x0042A658, scannerPlacementSkip = 0x0042A767;
    uintptr_t scannerRefresh = 0x0042A880, scannerRefreshContinue = 0x0042A88B, scannerRefreshSkip = 0x0042A925;
    uintptr_t flow = 0x008654A4, track = 0x0089E7A0, gameMode = 0x0089E7B0;
    uintptr_t mode = 0x00850078, firstView = 0x008900B4, raceManager = 0x00890118, carManager = 0x00890080;
    uintptr_t followDistance = 0x00783D28, followSeconds = 0x00783D30;
    uintptr_t paletteDensity = 0x004129B5, paletteDensityContinue = 0x004129BD;
    uintptr_t paletteRetain = 0x00412A19, paletteRetainContinue = 0x00412A20;
    uintptr_t paletteCreate = 0x00412B2D, paletteCreateContinue = 0x00412B34;
    uintptr_t palettePaint = 0x00412B76, nativePaint = 0x0061BC20;
    uintptr_t regions = 0x0088F298, findRegion = 0x005D9BF0, modelTable = 0x008A1CCC;
};
using PlacementFn = bool(__thiscall*)(void*, uintptr_t, const float*);
using NearFn = bool(__thiscall*)(void*, uintptr_t, uintptr_t);
using WeightedFn = bool(__thiscall*)(void*, uintptr_t, uintptr_t, float);
using WeightFn = float(__thiscall*)(void*, uintptr_t);
using VisibleFn = void(__thiscall*)(void*, bool);
using OutrunFn = void(__thiscall*)(void*);
using FollowFn = void(__thiscall*)(void*, uintptr_t, float, float, float);
using RegionFn = uintptr_t(__thiscall*)(void*, const float*, uint32_t, uintptr_t);
using PaintFn = uint32_t(__thiscall*)(void*, uintptr_t);
static Port port{};
static Addresses addresses{};
static PlacementFn originalPlacement = reinterpret_cast<PlacementFn>(0x0042F800);
static NearFn nearPlacement = reinterpret_cast<NearFn>(0x0042E790);
static WeightedFn weightedPlacement = reinterpret_cast<WeightedFn>(0x0042E9F0);
static WeightFn weight = reinterpret_cast<WeightFn>(0x00418240);
static VisibleFn visible = reinterpret_cast<VisibleFn>(0x005ED5C0);
static OutrunFn enterOutrun = reinterpret_cast<OutrunFn>(0x00607180);
static OutrunFn resetOutrun = reinterpret_cast<OutrunFn>(0x005F8490);
static FollowFn follow = reinterpret_cast<FollowFn>(0x00428630);
static RegionFn region = reinterpret_cast<RegionFn>(0x005D9BF0);
static PaintFn paint = reinterpret_cast<PaintFn>(0x0061BC20);
static bool enabled = false;
static uint32_t candidatesSkipped = 0, moverSkipped = 0, beltSkipped = 0, peerPlaced = 0;
static uint32_t outrunQueued = 0, outrunRefused = 0, outrunResumed = 0, followRefused = 0;
static uint32_t scannerPlacementSkipped = 0, scannerRefreshSkipped = 0;
struct PendingOutrun {
    uintptr_t car = 0, human = 0, parameter = 0, manager = 0;
    uint64_t session = 0, epoch = 0;
    coop_world::EntityKey key{};
    DWORD requested = 0;
};
static PendingOutrun pending{};
static PendingOutrun starting{}, engagement{};

static bool Scope(coop::Role role, bool requireFresh = true) {
    const auto session = CoopSnapshot();
    uint32_t flow = 0;
    return enabled && gCoopFleetEnabled && gCoopAIEnabled && IsCoopGameplayActive() && session.joined &&
        (!requireFresh || session.fresh) &&
        session.role == role && ReadGameMemory(addresses.flow, &flow, 4) && flow == 6;
}
static bool TypedIdentity(uintptr_t car, uintptr_t ride, uint32_t kind) {
    uintptr_t parameter = 0, actualRide = 0;
    uint32_t actualKind = 0; uint8_t id = 0;
    return IsValidUserPtr(car) && car != CoopRaceCar(false) && car != CoopRaceCar(true) &&
        ReadGameMemory(car + 0x14, &parameter, sizeof(parameter)) && IsValidUserPtr(parameter) &&
        ride == parameter + 0x10 && ReadGameMemory(car + 0x10, &actualRide, sizeof(actualRide)) && actualRide == ride &&
        ReadGameMemory(parameter + 4, &actualKind, 4) && actualKind == kind &&
        ReadGameMemory(parameter + 1, &id, 1) &&
        ((kind == 3 && id >= 50 && id <= 65) || (kind == 2 && id >= 100 && id <= 102));
}
static bool ClientCar(uintptr_t car) {
    uintptr_t ride = 0; uint32_t kind = 0;
    if(port.retainedClient && port.retainedClient(car,ride,kind) && TypedIdentity(car,ride,kind))return true;
    return Scope(coop::Role::Client, false) && port.clientOwned &&
        port.clientOwned(car, ride, kind) && TypedIdentity(car, ride, kind);
}
static bool BlockScannerCar(uintptr_t car) {
    // The scanner tests the replicated position, not the native driver layout.
    // A dormant Car can own a 0x54-byte role-zero driver, so reject its reserved
    // Client identity before any driver, appearance or first-pose dependency.
    return (car && IsCoopGameplayActive() && car == CoopRaceCar(true)) || ClientCar(car);
}
static bool BlockResetHumanView(uintptr_t score) {
    uintptr_t car = 0;
    // A remote human has no local Player/view. The stock free-roam reset
    // unconditionally clears Car+18->+250; leave the peer Car untouched.
    return (Scope(coop::Role::Host, false) || Scope(coop::Role::Client, false)) &&
        ReadGameMemory(score, &car, sizeof(car)) && car && car == CoopRaceCar(true);
}
static bool Human(uintptr_t car, bool peer) {
    uintptr_t parameter = 0, ride = 0; uint32_t kind = 0; uint8_t remote = 0, active = 0;
    float position[3]{}, velocity[3]{};
    return IsValidUserPtr(car) && car == CoopRaceCar(peer) && car != CoopRaceCar(!peer) &&
        ReadGameMemory(car + 0x14, &parameter, sizeof(parameter)) && IsValidUserPtr(parameter) &&
        ReadGameMemory(car + 0x10, &ride, sizeof(ride)) && ride == parameter + 0x10 &&
        ReadGameMemory(parameter + 4, &kind, 4) && kind == 1 &&
        ReadGameMemory(parameter + 0x900, &remote, 1) && remote == (peer ? 1 : 0) &&
        ReadGameMemory(car + 0x550, &active, 1) && active == 1 &&
        ReadGameMemory(car + 0x60, position, sizeof(position)) &&
        ReadGameMemory(car + 0x70, velocity, sizeof(velocity)) &&
        CoopPhysicalMotionNative::FiniteVector(position, 1000000.0f) &&
        CoopPhysicalMotionNative::FiniteVector(velocity, 500.0f);
}
static bool Ambient(uintptr_t car, uintptr_t& driver) {
    uintptr_t ride = 0, actualOwner = 0, vtable = 0; uint32_t kind = 0, role = 0;
    return port.identity && port.identity(car, ride, kind) && kind == 2 && TypedIdentity(car, ride, kind) &&
        ReadGameMemory(car + 0x24, &role, 4) && role == 3 &&
        ReadGameMemory(car + 0x2C, &driver, sizeof(driver)) && IsValidUserPtr(driver) &&
        ReadGameMemory(driver, &vtable, sizeof(vtable)) && vtable == CoopWorldPhysicalMotionNative::kRacerDriverVtable &&
        ReadGameMemory(driver + 4, &actualOwner, sizeof(actualOwner)) && actualOwner == car;
}
static bool SelectedAmbient(uintptr_t car) {
    // The Client's committed replica owns a dormant 0x54-byte driver. Its
    // manifest identity, rather than a Host racer driver, admits a challenge.
    if (ClientCar(car)) {
        uintptr_t ride = 0; uint32_t kind = 0;
        return port.clientOwned && port.clientOwned(car, ride, kind) && kind == 2 && TypedIdentity(car, ride, kind);
    }
    uintptr_t driver = 0;
    return Scope(coop::Role::Host, false) && Ambient(car, driver);
}
static bool CurrentOutrun(const PendingOutrun& selected) {
    const auto session = CoopSnapshot(); coop_world::EntityKey key{};
    uintptr_t manager = 0, parameter = 0; uint32_t track = 0;
    return (Scope(coop::Role::Host, false) || Scope(coop::Role::Client, false)) &&
        selected.car && selected.session == session.sessionId && port.worldEpoch && selected.epoch == port.worldEpoch() &&
        ReadGameMemory(addresses.track, &track, 4) && track == 4000 &&
        ReadGameMemory(addresses.raceManager, &manager, sizeof(manager)) && manager == selected.manager &&
        ReadGameMemory(selected.car + 0x14, &parameter, sizeof(parameter)) && parameter == selected.parameter &&
        SelectedAmbient(selected.car) && (Human(selected.human, false) || Human(selected.human, true)) &&
        selected.key.Valid() && port.outrunKey && port.outrunKey(selected.car, key) && key == selected.key;
}
static bool Selection(uintptr_t manager, PendingOutrun& result) {
    const auto session = CoopSnapshot();
    uint32_t track = 0, mode = 0; uintptr_t actualManager = 0, view = 0, human = 0, car = 0;
    if (!(Scope(coop::Role::Host, false) || Scope(coop::Role::Client, false)) || !port.worldEpoch || !port.worldEpoch() ||
        !ReadGameMemory(addresses.track, &track, 4) || track != 4000 ||
        !ReadGameMemory(addresses.gameMode, &mode, 4) || (mode != 0 && mode != 7) ||
        !ReadGameMemory(addresses.raceManager, &actualManager, sizeof(actualManager)) || actualManager != manager ||
        !IsValidUserPtr(manager) || !ReadGameMemory(addresses.firstView, &view, sizeof(view)) || !IsValidUserPtr(view) ||
        !ReadGameMemory(view + 4, &human, sizeof(human)) || !Human(human, false) ||
        !ReadGameMemory(view + 0x250, &car, sizeof(car)) || !SelectedAmbient(car)) return false;
    result = {}; result.car = car; result.human = human; result.manager = manager;
    if (!ReadGameMemory(car + 0x14, &result.parameter, sizeof(result.parameter))) return false;
    result.session = session.sessionId; result.epoch = port.worldEpoch(); result.requested = GetTickCount();
    if (port.outrunKey) port.outrunKey(car, result.key);
    return true;
}
static bool ManagedOutrunSelection(uintptr_t manager) {
    // A transient human/view/epoch validation failure must not send an owned
    // dormant opponent through unscoped stock entry. Prove only its committed
    // manifest identity here; do not inspect the possibly dormant AI driver.
    uintptr_t actualManager = 0, view = 0, car = 0, ride = 0; uint32_t track = 0, kind = 0;
    if (!(Scope(coop::Role::Host, false) || Scope(coop::Role::Client, false)) ||
        !ReadGameMemory(addresses.track, &track, 4) || track != 4000 ||
        !ReadGameMemory(addresses.raceManager, &actualManager, sizeof(actualManager)) || actualManager != manager ||
        !ReadGameMemory(addresses.firstView, &view, sizeof(view)) || !IsValidUserPtr(view) ||
        !ReadGameMemory(view + 0x250, &car, sizeof(car))) return false;
    if (ClientCar(car)) return port.clientOwned && port.clientOwned(car, ride, kind) && kind == 2;
    return Scope(coop::Role::Host, false) && port.identity && port.identity(car, ride, kind) &&
        kind == 2 && TypedIdentity(car, ride, kind);
}
static bool HoldOutrunResult(uintptr_t manager);
static bool BlockOutrun(uintptr_t manager) {
    if (starting.manager == manager && CurrentOutrun(starting) && port.outrunApproved &&
        port.outrunApproved(starting.car, starting.human)) return false;
    PendingOutrun selected{};
    if (!Selection(manager, selected)) {
        if (ManagedOutrunSelection(manager)) { ++outrunRefused; return true; }
        return false; // Unrelated/SP native path.
    }
    if (engagement.car || !selected.key.Valid() || !CoopSnapshot().fresh) { ++outrunRefused; return true; }
    const bool same = pending.car == selected.car && pending.human == selected.human &&
        pending.parameter == selected.parameter && pending.session == selected.session && pending.epoch == selected.epoch &&
        pending.key == selected.key;
    if (!same || static_cast<DWORD>(selected.requested - pending.requested) >= 1000) {
        pending = selected;
        if (port.requestOutrun) { port.requestOutrun(selected.car, selected.human); ++outrunQueued; }
    }
    ++outrunRefused; return true;
}
static bool BlockFollow(uintptr_t driver, uintptr_t human) {
    uintptr_t car = 0, ownedDriver = 0;
    if (!IsValidUserPtr(driver) || !ReadGameMemory(driver + 4, &car, sizeof(car))) return false;
    // The world scanner invokes Follow independently of the suppressed AI tick.
    // Its client follow allocation reaches 428490 with no local racer context
    // at driver+6E0 (live crash 428499). All replicated ambient steering belongs
    // to Host, including idle/approved racers and stale snapshots. Membership
    // uses the reserved manifest, not an active pose or a particular human.
    if (ClientCar(car)) return true;
    if (!Ambient(car, ownedDriver) || ownedDriver != driver) return false;
    if (!(Scope(coop::Role::Host) || Scope(coop::Role::Client)) || !port.outrunBusy || !port.outrunApproved ||
        !(Human(human, false) || Human(human, true)) || !port.outrunBusy(car)) return false;
    return !port.outrunApproved(car, human);
}

// The original palette controller reads CarTypeInfo's five regional density
// bytes (+854..858) and five retention bytes (+859..85D). Expand only those
// reads to the two real human regions; native reference counts, retirement,
// capacity limit and resource request/release calls are not replaced.
static int PaletteValue(uintptr_t node, uintptr_t modelInfo, uint32_t nativeRegion,
    bool retention) {
    const uintptr_t field = retention ? 0x859 : 0x854;
    int8_t original = 0;
    if (!ReadGameMemory(modelInfo + nativeRegion + field, &original, 1)) return 0;
    if (nativeRegion >= 5 || !Scope(coop::Role::Host) || !region) return original;
    uint8_t committedPaint = 0; uint32_t model = 0; uintptr_t table = 0;
    if (!ReadCoopFleetPalettePaint(node + 0x10, committedPaint) ||
        !ReadGameMemory(node + 0x10, &model, 4) || model >= 4096 ||
        !ReadGameMemory(addresses.modelTable, &table, sizeof(table)) ||
        !IsValidUserPtr(table) || modelInfo != table + static_cast<uintptr_t>(model) * 0x890)
        return original;
    int result = original;
    for (bool peer : {false, true}) {
        const uintptr_t human = CoopRaceCar(peer);
        float position[3]{}; uintptr_t found = 0; uint32_t offset = 0;
        if (!Human(human, peer) || !ReadGameMemory(human + 0x60, position, sizeof(position))) continue;
        __try { found = region(reinterpret_cast<void*>(addresses.regions), position, 0x10, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        int8_t value = 0;
        if (!IsValidUserPtr(found) || !ReadGameMemory(found + 0x3C, &offset, 4) || offset >= 5 ||
            !ReadGameMemory(modelInfo + offset + field, &value, 1)) continue;
        result = retention ? ((result != 0 || value != 0) ? 1 : 0) : (result > value ? result : value);
    }
    return result;
}
static uint32_t PalettePaint(uintptr_t modelInfo, uintptr_t node, uintptr_t random) {
    // Consume the stock RNG exactly once, even for a managed palette node.
    uint32_t result = paint ? paint(reinterpret_cast<void*>(modelInfo), random) : 0;
    uint8_t selected = 0;
    if ((Scope(coop::Role::Host, false) || Scope(coop::Role::Client, false)) &&
        ReadCoopFleetPalettePaint(node + 0x10, selected)) result = (result & ~0xFFU) | selected;
    return result;
}
}

static uintptr_t gCoopFleetCandidateContinue = 0x0042FA8B, gCoopFleetCandidateSkip = 0x0042FABA;
static uintptr_t gCoopFleetTrafficContinue = 0x00408826, gCoopFleetBeltContinue = 0x0059EC86;
static uintptr_t gCoopFleetOutrunContinue = 0x00607185, gCoopFleetFollowContinue = 0x00428635;
static uintptr_t gCoopFleetResetHumanViewContinue = 0x005F8523, gCoopFleetResetHumanViewNull = 0x005F8528;
static uintptr_t gCoopFleetResetHumanViewSkip = 0x005F8534;
static uintptr_t gCoopFleetResetAmbientContinue = 0x0042CD8F, gCoopFleetResetAmbientSkip = 0x0042CDA0;
static uintptr_t gCoopFleetScannerPlacementContinue = 0x0042A658, gCoopFleetScannerPlacementSkip = 0x0042A767;
static uintptr_t gCoopFleetScannerRefreshContinue = 0x0042A88B, gCoopFleetScannerRefreshSkip = 0x0042A925;
static uintptr_t gCoopFleetNativeMode = 0x00850078;
static uintptr_t gCoopFleetDensityContinue = 0x004129BD, gCoopFleetRetainContinue = 0x00412A20;
static uintptr_t gCoopFleetCreateContinue = 0x00412B34;

bool ShouldSuppressCoopFleetMover(uintptr_t wrapper) {
    if (!CoopFleetNative::enabled) return false;
    uintptr_t vtable = 0, car = 0;
    CoopWorldPhysicalMotionNative::Target target{};
    if (!ReadGameMemory(wrapper, &vtable, sizeof(vtable)) ||
        (vtable != CoopWorldPhysicalMotionNative::kTrafficMoverVtable && vtable != CoopWorldPhysicalMotionNative::kBeltMoverVtable) ||
        !ReadGameMemory(wrapper + 8, &car, sizeof(car)) || !ShouldSuppressCoopAIPlacement(car) ||
        !CoopWorldPhysicalMotionNative::ReadTarget(car, CoopRaceCar(false), target) || target.wrapper != wrapper) return false;
    return (target.kind == 3 && target.mode == 2) || target.mode == 4;
}

#define COOP_FLEET_SAVE_CPU() __asm { lea esp, [esp - 4] } __asm { pushfd } __asm { pushad } \
    __asm { mov ebp, esp } __asm { sub esp, 528 } __asm { lea ebx, [esp + 15] } \
    __asm { and ebx, -16 } __asm { fxsave [ebx] }
#define COOP_FLEET_RESULT() __asm { movzx eax, al } __asm { mov [ebp + 36], eax } \
    __asm { fxrstor [ebx] } __asm { mov esp, ebp } __asm { cmp dword ptr [ebp + 36], 0 }
#define COOP_FLEET_RESTORE_CPU() __asm { popad } __asm { popfd } __asm { lea esp, [esp + 4] }

__declspec(naked) static void HookedCoopFleetResetHumanView() {
    COOP_FLEET_SAVE_CPU()
    __asm { push edi }
    __asm { call CoopFleetNative::BlockResetHumanView }
    __asm { add esp, 4 }
    COOP_FLEET_RESULT()
    __asm { jne skip_peer_view }
    COOP_FLEET_RESTORE_CPU()
    __asm { mov eax, dword ptr [edi] }
    __asm { test eax, eax }
    __asm { jz null_human_car }
    __asm { jmp dword ptr [gCoopFleetResetHumanViewContinue] }
    __asm { null_human_car: }
    __asm { jmp dword ptr [gCoopFleetResetHumanViewNull] }
    __asm { skip_peer_view: }
    COOP_FLEET_RESTORE_CPU()
    __asm { jmp dword ptr [gCoopFleetResetHumanViewSkip] }
}
__declspec(naked) static void HookedCoopFleetResetAmbient() {
    COOP_FLEET_SAVE_CPU()
    __asm { push esi }
    __asm { call CoopFleetNative::ClientCar }
    __asm { add esp, 4 }
    COOP_FLEET_RESULT()
    __asm { jne skip_dormant_reset }
    COOP_FLEET_RESTORE_CPU()
    __asm { mov ecx, dword ptr [esi + 0x2C] }
    __asm { mov eax, dword ptr [ecx + 0x6FC] }
    __asm { jmp dword ptr [gCoopFleetResetAmbientContinue] }
    __asm { skip_dormant_reset: }
    COOP_FLEET_RESTORE_CPU()
    __asm { jmp dword ptr [gCoopFleetResetAmbientSkip] }
}
__declspec(naked) static void HookedCoopFleetScannerPlacement() {
    __asm { mov ecx, dword ptr [ecx] }
    __asm { test ecx, ecx }
    __asm { mov dword ptr [esp + 0x10], ecx }
    COOP_FLEET_SAVE_CPU()
    __asm { push ecx }
    __asm { call CoopFleetNative::BlockScannerCar }
    __asm { add esp, 4 }
    COOP_FLEET_RESULT()
    __asm { jne skip_scanner_placement }
    COOP_FLEET_RESTORE_CPU()
    __asm { jmp dword ptr [gCoopFleetScannerPlacementContinue] }
    __asm { skip_scanner_placement: inc dword ptr [CoopFleetNative::scannerPlacementSkipped] }
    COOP_FLEET_RESTORE_CPU()
    __asm { jmp dword ptr [gCoopFleetScannerPlacementSkip] }
}
__declspec(naked) static void HookedCoopFleetScannerRefresh() {
    __asm { mov edi, dword ptr [esi + 8] }
    __asm { test edi, edi }
    COOP_FLEET_SAVE_CPU()
    __asm { push edi }
    __asm { call CoopFleetNative::BlockScannerCar }
    __asm { add esp, 4 }
    COOP_FLEET_RESULT()
    __asm { jne skip_scanner_refresh }
    COOP_FLEET_RESTORE_CPU()
    __asm { jz null_scanner_refresh }
    __asm { jmp dword ptr [gCoopFleetScannerRefreshContinue] }
    __asm { skip_scanner_refresh: inc dword ptr [CoopFleetNative::scannerRefreshSkipped] }
    COOP_FLEET_RESTORE_CPU()
    __asm { null_scanner_refresh: }
    __asm { jmp dword ptr [gCoopFleetScannerRefreshSkip] }
}
__declspec(naked) static void HookedCoopFleetCandidate() {
    __asm { mov edi, dword ptr [eax + ecx * 4 + 0x1C] }
    COOP_FLEET_SAVE_CPU()
    __asm { push edi }
    __asm { call CoopFleetNative::ClientCar }
    __asm { add esp, 4 }
    COOP_FLEET_RESULT()
    __asm { jne skip_candidate }
    COOP_FLEET_RESTORE_CPU()
    __asm { push edi }
    __asm { jmp dword ptr [gCoopFleetCandidateContinue] }
    __asm { skip_candidate: inc dword ptr [CoopFleetNative::candidatesSkipped] }
    COOP_FLEET_RESTORE_CPU()
    __asm { jmp dword ptr [gCoopFleetCandidateSkip] }
}
__declspec(naked) static void HookedCoopFleetTrafficMover() {
    COOP_FLEET_SAVE_CPU()
    __asm { push ecx }
    __asm { call ShouldSuppressCoopFleetMover }
    __asm { add esp, 4 }
    COOP_FLEET_RESULT()
    __asm { jne skip_mover }
    COOP_FLEET_RESTORE_CPU()
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { and esp, -16 }
    __asm { jmp dword ptr [gCoopFleetTrafficContinue] }
    __asm { skip_mover: inc dword ptr [CoopFleetNative::moverSkipped] }
    COOP_FLEET_RESTORE_CPU()
    __asm { fld dword ptr [esp + 4] }
    __asm { ret 4 }
}
__declspec(naked) static void HookedCoopFleetBeltMover() {
    COOP_FLEET_SAVE_CPU()
    __asm { push ecx }
    __asm { call ShouldSuppressCoopFleetMover }
    __asm { add esp, 4 }
    COOP_FLEET_RESULT()
    __asm { jne skip_belt }
    COOP_FLEET_RESTORE_CPU()
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { and esp, -16 }
    __asm { jmp dword ptr [gCoopFleetBeltContinue] }
    __asm { skip_belt: inc dword ptr [CoopFleetNative::beltSkipped] }
    COOP_FLEET_RESTORE_CPU()
    __asm { fld dword ptr [esp + 4] }
    __asm { ret 4 }
}
__declspec(naked) static void HookedCoopFleetOutrunAdmission() {
    COOP_FLEET_SAVE_CPU()
    __asm { push ecx }
    __asm { call CoopFleetNative::BlockOutrun }
    __asm { add esp, 4 }
    COOP_FLEET_RESULT()
    __asm { jne refuse_outrun }
    COOP_FLEET_RESTORE_CPU()
    __asm { mov eax, dword ptr [gCoopFleetNativeMode] }
    __asm { mov eax, dword ptr [eax] }
    __asm { jmp dword ptr [gCoopFleetOutrunContinue] }
    __asm { refuse_outrun: }
    COOP_FLEET_RESTORE_CPU()
    __asm { ret }
}
__declspec(naked) static void HookedCoopFleetFollowTarget() {
    COOP_FLEET_SAVE_CPU()
    // Original ESP=EBP+40: arg1 at+44, original ECX saved at+24.
    __asm { push dword ptr [ebp + 44] }
    __asm { push dword ptr [ebp + 24] }
    __asm { call CoopFleetNative::BlockFollow }
    __asm { add esp, 8 }
    COOP_FLEET_RESULT()
    __asm { jne refuse_follow }
    COOP_FLEET_RESTORE_CPU()
    __asm { push ebx }
    __asm { push esi }
    __asm { push edi }
    __asm { mov edi, ecx }
    __asm { jmp dword ptr [gCoopFleetFollowContinue] }
    __asm { refuse_follow: inc dword ptr [CoopFleetNative::followRefused] }
    COOP_FLEET_RESTORE_CPU()
    __asm { ret 0x10 }
}
__declspec(naked) static void HookedCoopFleetPaletteDensity() {
    COOP_FLEET_SAVE_CPU()
    __asm { push 0 }
    __asm { push dword ptr [ebp + 28] }
    __asm { push dword ptr [ebp + 24] }
    __asm { push dword ptr [ebp + 20] }
    __asm { call CoopFleetNative::PaletteValue }
    __asm { add esp, 16 }
    // MOVSX changes ESI only and leaves flags/native x87/SSE intact.
    __asm { mov dword ptr [ebp + 4], eax }
    __asm { fxrstor [ebx] }
    __asm { mov esp, ebp }
    COOP_FLEET_RESTORE_CPU()
    __asm { jmp dword ptr [gCoopFleetDensityContinue] }
}
__declspec(naked) static void HookedCoopFleetPaletteRetain() {
    COOP_FLEET_SAVE_CPU()
    __asm { push 1 }
    __asm { push dword ptr [ebp + 28] }
    __asm { push dword ptr [ebp + 24] }
    __asm { push dword ptr [ebp + 20] }
    __asm { call CoopFleetNative::PaletteValue }
    __asm { add esp, 16 }
    __asm { mov dword ptr [ebp + 36], eax }
    __asm { fxrstor [ebx] }
    __asm { mov esp, ebp }
    COOP_FLEET_RESTORE_CPU()
    __asm { cmp byte ptr [esp - 4], bl }
    __asm { jmp dword ptr [gCoopFleetRetainContinue] }
}
__declspec(naked) static void HookedCoopFleetPaletteCreate() {
    COOP_FLEET_SAVE_CPU()
    __asm { push 0 }
    // Here EAX=modelInfo, ECX=region, ESI=palette node.
    __asm { push dword ptr [ebp + 24] }
    __asm { push dword ptr [ebp + 28] }
    __asm { push dword ptr [ebp + 4] }
    __asm { call CoopFleetNative::PaletteValue }
    __asm { add esp, 16 }
    __asm { mov dword ptr [ebp + 36], eax }
    __asm { fxrstor [ebx] }
    __asm { mov esp, ebp }
    COOP_FLEET_RESTORE_CPU()
    __asm { cmp byte ptr [esp - 4], bl }
    __asm { jmp dword ptr [gCoopFleetCreateContinue] }
}
__declspec(naked) static void HookedCoopFleetPalettePaint() {
    COOP_FLEET_SAVE_CPU()
    __asm { push dword ptr [ebp + 44] }
    __asm { push dword ptr [ebp + 4] }
    __asm { push dword ptr [ebp + 24] }
    __asm { call CoopFleetNative::PalettePaint }
    __asm { add esp, 12 }
    __asm { mov dword ptr [ebp + 28], eax }
    __asm { fxrstor [ebx] }
    __asm { mov esp, ebp }
    COOP_FLEET_RESTORE_CPU()
    __asm { ret 4 }
}
static uintptr_t gCoopFleetOutrunMode=0x0089E7B0;
static uintptr_t gCoopFleetOutrunResultContinue=0x00607CA3,gCoopFleetOutrunResultSkip=0x00608C58;
__declspec(naked) static void HookedCoopFleetOutrunResult() {
    COOP_FLEET_SAVE_CPU()
    __asm { push dword ptr [ebp + 8] } // Original EBP is the race manager.
    __asm { call CoopFleetNative::HoldOutrunResult }
    __asm { add esp, 4 }
    COOP_FLEET_RESULT()
    __asm { jne hold_outcome }
    COOP_FLEET_RESTORE_CPU()
    __asm { push eax }
    __asm { mov eax, dword ptr [gCoopFleetOutrunMode] }
    __asm { cmp dword ptr [eax], 7 }
    __asm { pop eax } // POP retains the displaced comparison's flags.
    __asm { jmp dword ptr [gCoopFleetOutrunResultContinue] }
    __asm { hold_outcome: }
    COOP_FLEET_RESTORE_CPU()
    __asm { jmp dword ptr [gCoopFleetOutrunResultSkip] }
}

#undef COOP_FLEET_SAVE_CPU
#undef COOP_FLEET_RESULT
#undef COOP_FLEET_RESTORE_CPU

static bool __fastcall HookedCoopFleetPlacement(void* controller, void*, uintptr_t car, const float* weights) {
    using namespace CoopFleetNative;
    if (ClientCar(car)) return true;
    uintptr_t focus = 0, ride = 0, manager = 0, actualController = 0; uint32_t kind = 0;
    if (Scope(coop::Role::Host) && port.hostFocus && port.identity &&
        port.identity(car, ride, kind) && kind == 3 && TypedIdentity(car, ride, kind) &&
        port.hostFocus(car, focus) && Human(focus, true) &&
        ReadGameMemory(addresses.carManager, &manager, sizeof(manager)) && IsValidUserPtr(manager) &&
        ReadGameMemory(manager + 0x118, &actualController, sizeof(actualController)) &&
        actualController == reinterpret_cast<uintptr_t>(controller) && nearPlacement && weightedPlacement && weight) {
        // These explicit-focus APIs accept a real Car*, not a viewport/player
        // root. Native route placement/resource accounting remain native.
        __try {
            if (nearPlacement(controller, car, focus)) { ++peerPlaced; return true; }
            const float density = weight(controller, focus);
            if (std::isfinite(density) && std::fabs(density) <= 1000000.0f &&
                weightedPlacement(controller, car, focus, density)) { ++peerPlaced; return true; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    return originalPlacement ? originalPlacement(controller, car, weights) : false;
}

// Visibility remains the native byte setter. The separately verified contact
// adapter activates only current Client world replicas, without changing their driver.
bool SetCoopFleetCarVisible(uintptr_t car, uintptr_t localCar, bool active) {
    using namespace CoopFleetNative;
    CoopWorldPhysicalMotionNative::Target target{};
    if (!visible || !ClientCar(car) || !CoopWorldPhysicalMotionNative::ReadTarget(car, localCar, target) ||
        !CoopPhysicalMotionNative::WritableRange(car + 0x550, 1)) return false;
    if (active) {
        uintptr_t visual = 0, visualRide = 0;
        if (!ReadGameMemory(car + 0x554, &visual, sizeof(visual)) || !IsValidUserPtr(visual) ||
            !ReadGameMemory(visual + 4, &visualRide, sizeof(visualRide)) || visualRide != target.ride) return false;
    }
    __try { visible(reinterpret_cast<void*>(car), active); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (target.kind == 2 || target.kind == 3) SetCoopAIReplicaContacts(car, localCar, active);
    return true;
}

#include "CoopFleetOutrunNative.inl"

bool ResumeCoopFleetOutrun() {
    using namespace CoopFleetNative;
    if (!pending.car || !enterOutrun || !port.outrunApproved) return false;
    PendingOutrun selected{};
    if (!Selection(pending.manager, selected) || selected.car != pending.car || selected.human != pending.human ||
        selected.parameter != pending.parameter || selected.session != pending.session || selected.epoch != pending.epoch || selected.key != pending.key ||
        static_cast<DWORD>(GetTickCount() - pending.requested) > 10000) { pending = {}; return false; }
    if (!port.outrunApproved(selected.car, selected.human)) return false;
    if (!EnterSelectedOutrun(selected)) return false;
    pending = {}; return true;
}

bool ApplyCoopFleetOutrunFollow(uintptr_t ambientCar, uintptr_t humanCar) {
    using namespace CoopFleetNative;
    uintptr_t driver = 0; float distance = 0, seconds = 0;
    if (!Scope(coop::Role::Host) || !follow || !port.outrunApproved ||
        !port.outrunApproved(ambientCar, humanCar) || !Ambient(ambientCar, driver) ||
        !(Human(humanCar, false) || Human(humanCar, true)) ||
        !ReadGameMemory(addresses.followDistance, &distance, 4) || !std::isfinite(distance) || distance <= 0 || distance > 1000000 ||
        !ReadGameMemory(addresses.followSeconds, &seconds, 4) || !std::isfinite(seconds) || seconds <= 0 || seconds > 3600) return false;
    // EXACT stock world-scanner arguments at 42A804..42A819. This follows a
    // real human target; it does not start a global Host OutRun or write score.
    __try { follow(reinterpret_cast<void*>(driver), humanCar, distance, 0.0f, seconds); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

bool InstallCoopFleetNativeHooks(const CoopFleetNative::Port& port) {
    using namespace CoopFleetNative;
    if (enabled) { CoopFleetNative::port = port; return true; }
    if (!CoopPhysicalMotionNative::enabled || !CoopAINativePlacementHooks::enabled) return false;
    struct Site { uintptr_t address, function; uint8_t size, opcode; uint8_t bytes[11]; DWORD protection; };
    Site sites[]{
        {addresses.candidate, reinterpret_cast<uintptr_t>(&HookedCoopFleetCandidate), 5, 0xE9, {0x8B,0x7C,0x88,0x1C,0x57}, 0},
        {addresses.trafficMover, reinterpret_cast<uintptr_t>(&HookedCoopFleetTrafficMover), 6, 0xE9, {0x55,0x8B,0xEC,0x83,0xE4,0xF0}, 0},
        {addresses.beltMover, reinterpret_cast<uintptr_t>(&HookedCoopFleetBeltMover), 6, 0xE9, {0x55,0x8B,0xEC,0x83,0xE4,0xF0}, 0},
        {addresses.placement, reinterpret_cast<uintptr_t>(&HookedCoopFleetPlacement), 5, 0xE8, {0xE8,0x5E,0xFD,0xFF,0xFF}, 0},
        {addresses.outrun, reinterpret_cast<uintptr_t>(&HookedCoopFleetOutrunAdmission), 5, 0xE9, {0xA1,0x78,0x00,0x85,0x00}, 0},
        {addresses.outrunMeasure, reinterpret_cast<uintptr_t>(&HookedCoopFleetOutrunMeasure), 5, 0xE8, {0xE8,0x5B,0xAE,0xFF,0xFF}, 0},
        {addresses.outrunSort, reinterpret_cast<uintptr_t>(&HookedCoopFleetOutrunSort), 5, 0xE8, {0xE8,0x61,0x73,0xFE,0xFF}, 0},
        {addresses.outrunResult, reinterpret_cast<uintptr_t>(&HookedCoopFleetOutrunResult), 7, 0xE9, {0x83,0x3D,0xB0,0xE7,0x89,0x00,0x07}, 0},
        {addresses.follow, reinterpret_cast<uintptr_t>(&HookedCoopFleetFollowTarget), 5, 0xE9, {0x53,0x56,0x57,0x8B,0xF9}, 0}
        ,{addresses.resetHumanView, reinterpret_cast<uintptr_t>(&HookedCoopFleetResetHumanView), 6, 0xE9, {0x8B,0x07,0x85,0xC0,0x74,0x05}, 0}
        ,{addresses.resetAmbient, reinterpret_cast<uintptr_t>(&HookedCoopFleetResetAmbient), 9, 0xE9, {0x8B,0x4E,0x2C,0x8B,0x81,0xFC,0x06,0x00,0x00}, 0}
        ,{addresses.paletteDensity, reinterpret_cast<uintptr_t>(&HookedCoopFleetPaletteDensity), 8, 0xE9, {0x0F,0xBE,0xB4,0x08,0x54,0x08,0x00,0x00}, 0}
        ,{addresses.paletteRetain, reinterpret_cast<uintptr_t>(&HookedCoopFleetPaletteRetain), 7, 0xE9, {0x38,0x9C,0x08,0x59,0x08,0x00,0x00}, 0}
        ,{addresses.paletteCreate, reinterpret_cast<uintptr_t>(&HookedCoopFleetPaletteCreate), 7, 0xE9, {0x38,0x9C,0x08,0x54,0x08,0x00,0x00}, 0}
        ,{addresses.palettePaint, reinterpret_cast<uintptr_t>(&HookedCoopFleetPalettePaint), 5, 0xE8, {0xE8,0xA5,0x90,0x20,0x00}, 0}
        ,{addresses.scannerPlacement, reinterpret_cast<uintptr_t>(&HookedCoopFleetScannerPlacement), 8, 0xE9, {0x8B,0x09,0x85,0xC9,0x89,0x4C,0x24,0x10}, 0}
        ,{addresses.scannerRefresh, reinterpret_cast<uintptr_t>(&HookedCoopFleetScannerRefresh), 11, 0xE9, {0x8B,0x7E,0x08,0x85,0xFF,0x0F,0x84,0x9A,0x00,0x00,0x00}, 0}
    };
    constexpr uint8_t visibleBytes[]{0x8A,0x44,0x24,0x04,0x88,0x81,0x50,0x05,0x00,0x00,0xC2,0x04,0x00};
    constexpr uint8_t nearBytes[]{0x83,0xEC,0x20,0xA1,0xC8,0x3D,0x88,0x00,0x85,0xC0,0x56,0x8B,0xF1};
    constexpr uint8_t weightedBytes[]{0x83,0xEC,0x64,0xA1,0xA8,0xE7,0x89,0x00,0x53,0x56,0x57,0x8B,0xF9};
    constexpr uint8_t weightBytes[]{0x51,0x53,0x8B,0x5C,0x24,0x0C,0x56,0x57,0x6A,0x00};
    constexpr uint8_t regionBytes[]{0x8B,0x44,0x24,0x08,0x53,0x8B,0x9C,0x81,0x44,0x04,0x00,0x00};
    constexpr uint8_t paintBytes[]{0x56,0x8B,0xF1,0x8B,0x4C,0x24,0x08,0x85,0xC9};
    constexpr uint8_t resetOutrunBytes[]{0x83,0xEC,0x0C,0x53,0x56,0x8B,0xF1,0xB8,0x01,0x00,0x00,0x00};
    using CoopAINativePlacementHooks::ReadSignature;
    for (const auto& site : sites) if (!ReadSignature(site.address, site.bytes, site.size)) return false;
    if (!originalPlacement || !nearPlacement || !weightedPlacement || !weight || !visible || !enterOutrun || !follow ||
        !ReadSignature(addresses.visible, visibleBytes, sizeof(visibleBytes)) ||
        !ReadSignature(addresses.nearPlacement, nearBytes, sizeof(nearBytes)) ||
        !ReadSignature(addresses.weightedPlacement, weightedBytes, sizeof(weightedBytes)) ||
        !ReadSignature(addresses.weight, weightBytes, sizeof(weightBytes)) ||
        !resetOutrun || !ReadSignature(addresses.resetOutrun, resetOutrunBytes, sizeof(resetOutrunBytes)) ||
        !region || !paint || !ReadSignature(addresses.findRegion, regionBytes, sizeof(regionBytes)) ||
        !ReadSignature(addresses.nativePaint, paintBytes, sizeof(paintBytes))) return false;
    const size_t count = sizeof(sites) / sizeof(sites[0]); size_t protectedCount = 0;
    for (; protectedCount != count; ++protectedCount) {
        auto& site = sites[protectedCount];
        if (!VirtualProtect(reinterpret_cast<void*>(site.address), site.size, PAGE_EXECUTE_READWRITE, &site.protection)) break;
    }
    if (protectedCount != count) {
        DWORD unused = 0;
        while (protectedCount) { auto& site = sites[--protectedCount]; VirtualProtect(reinterpret_cast<void*>(site.address), site.size, site.protection, &unused); }
        return false;
    }
    gCoopFleetCandidateContinue = addresses.candidateContinue; gCoopFleetCandidateSkip = addresses.candidateSkip;
    gCoopFleetTrafficContinue = addresses.trafficMoverContinue; gCoopFleetBeltContinue = addresses.beltMoverContinue;
    gCoopFleetOutrunContinue = addresses.outrunContinue; gCoopFleetFollowContinue = addresses.followContinue;
    gCoopFleetOutrunMode=addresses.gameMode;
    gCoopFleetOutrunResultContinue=addresses.outrunResultContinue;gCoopFleetOutrunResultSkip=addresses.outrunResultSkip;
    gCoopFleetResetHumanViewContinue = addresses.resetHumanViewContinue;
    gCoopFleetResetHumanViewNull = addresses.resetHumanViewNull; gCoopFleetResetHumanViewSkip = addresses.resetHumanViewSkip;
    gCoopFleetResetAmbientContinue = addresses.resetAmbientContinue; gCoopFleetResetAmbientSkip = addresses.resetAmbientSkip;
    gCoopFleetScannerPlacementContinue = addresses.scannerPlacementContinue;
    gCoopFleetScannerPlacementSkip = addresses.scannerPlacementSkip;
    gCoopFleetScannerRefreshContinue = addresses.scannerRefreshContinue;
    gCoopFleetScannerRefreshSkip = addresses.scannerRefreshSkip;
    gCoopFleetNativeMode = addresses.mode;
    gCoopFleetDensityContinue = addresses.paletteDensityContinue;
    gCoopFleetRetainContinue = addresses.paletteRetainContinue;
    gCoopFleetCreateContinue = addresses.paletteCreateContinue;
    size_t attempted = 0; bool okay = true;
    for (const auto& site : sites) {
        uint8_t patch[11]; std::memset(patch, 0x90, sizeof(patch)); patch[0] = site.opcode;
        const int32_t relative = static_cast<int32_t>(static_cast<intptr_t>(site.function) - (site.address + 5));
        std::memcpy(patch + 1, &relative, 4); ++attempted;
        if (!SafeWriteBytes(site.address, patch, site.size)) { okay = false; break; }
    }
    if (!okay) for (size_t i = 0; i != attempted; ++i) SafeWriteBytes(sites[i].address, sites[i].bytes, sites[i].size);
    DWORD unused = 0;
    for (auto& site : sites) {
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site.address), site.size);
        VirtualProtect(reinterpret_cast<void*>(site.address), site.size, site.protection, &unused);
    }
    if (!okay) return false;
    enterOutrun = reinterpret_cast<OutrunFn>(addresses.outrun);
    resetOutrun = reinterpret_cast<OutrunFn>(addresses.resetOutrun);
    CoopFleetNative::port = port; enabled = true;
    CoopAINativePlacementHooks::fleetPlacement = &ClientCar;
    LogMessage("[FLEET_NATIVE] Owned Client scanner/candidate/mover/pending/wake/integration guards and exclusive OutRun admission installed; Host/SP native lifecycle retained.\r\n");
    return true;
}

bool CoopFleetTrafficNativeReady() {
    return CoopFleetNative::enabled && CoopPhysicalMotionNative::enabled && CoopAINativePlacementHooks::enabled;
}
bool CoopFleetScannerNativeReady() {
    // Installation state, not a runtime setting: a legacy installer must never
    // overwrite either of the atomically verified scanner gates.
    return CoopFleetNative::enabled;
}
bool CoopFleetOutrunNativeReady() {
    const auto& port = CoopFleetNative::port;
    return CoopFleetTrafficNativeReady() && port.identity && port.worldEpoch && port.outrunApproved &&
        port.outrunBusy && port.requestOutrun && port.outrunKey && CoopFleetNative::resetOutrun;
}
bool GetCoopFleetNativeCapabilities(CoopTrafficRuntime::Capabilities& result) {
    result = {};
    if (!CoopFleetTrafficNativeReady()) return false;
    const auto& port = CoopFleetNative::port;
    result.trafficMover = result.trafficPlacement = result.trafficTransitions = true;
    result.ambientDriver = result.ambientTransitions = true;
    result.ambientEngagement = CoopFleetOutrunNativeReady();
    result.hostPeerFocus = port.hostFocus != nullptr;
    result.hostPeerStreaming = CoopDualRegionNative::enabled;
    result.hostPeerPalette = CoopFleetNative::region && CoopFleetNative::paint;
    return result.ambientEngagement && result.hostPeerFocus && result.hostPeerStreaming && result.hostPeerPalette;
}
