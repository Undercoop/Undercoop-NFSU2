// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Passive game-thread diagnostics only. Preserve native EAX exactly; never
// change visibility, masks, poses, frusta, camera state or renderer decisions.
// Include after CoopFleetWorldRuntime.inl and the map entry-hook installer.
namespace CoopRearRenderTrace {
using HideFn = uint32_t(__cdecl*)(uintptr_t, uintptr_t);
using EligibleFn = uint32_t(__cdecl*)(uintptr_t, uintptr_t, uintptr_t);
using ReadFn = bool(*)(uintptr_t, void*, size_t);
using OwnedFn = bool(*)(uintptr_t, uintptr_t&, uint32_t&);
using ContextFn = bool(*)();
using ClockFn = DWORD(*)();
using KeyFn = bool(*)();
using LogFn = void(*)(const char*);
using InstallFn = bool(*)(uintptr_t, const BYTE*, size_t, void*, void**);
static HideFn originalHide = nullptr;
static EligibleFn originalEligible = nullptr;
static bool installed = false;
static bool ProductionRead(uintptr_t at, void* out, size_t bytes) { return ReadGameMemory(at, out, bytes); }
static bool ProductionContext() {
    const auto session = CoopSnapshot();
    return IsCoopGameplayActive() && session.joined && session.fresh && session.role == coop::Role::Client;
}
static bool ProductionOwned(uintptr_t car, uintptr_t& ride, uint32_t& kind) {
    return car && car != CoopRaceCar(false) && car != CoopRaceCar(true) &&
        ReadManagedCoopFleet(car, ride, kind) && (kind == 2 || kind == 3);
}
static DWORD ProductionClock() { return GetTickCount(); }
static bool ProductionKey() { return (GetAsyncKeyState('B') & 0x8000) != 0; }
struct Ports {
    ReadFn read = ProductionRead;
    OwnedFn owned = ProductionOwned;
    ContextFn context = ProductionContext;
    ClockFn clock = ProductionClock;
    KeyFn key = ProductionKey;
    LogFn log = LogMessage;
    InstallFn install = InstallMapEntryHook;
};
static Ports ports;
enum class Stage : uint8_t { Hide, Eligible };
enum class Window : uint8_t { Baseline, Edge, Settled };
constexpr size_t kDetails = 384, kKeys = 128, kWindowDetails = 128;
constexpr DWORD kCaptureMs = 150, kReportMs = 500, kSettledMs = 750;
struct Key { uintptr_t car = 0, view = 0; uint32_t window = 0, tick = 0; Stage stage = Stage::Hide; unsigned count = 0; };
struct Detail {
    uintptr_t car = 0, ride = 0, view = 0, cameraArg = 0, viewCamera = 0, ownTarget = 0, root = 0;
    uint32_t window = 0, kind = 0, result = 0, viewId = 0, poseMask = 0, viewMask = 0, ghostMask = 0, root234 = 0, known = 0;
    DWORD age = 0;
    uint8_t active = 255, hidden = 255;
    Stage stage = Stage::Hide;
    Window reason = Window::Baseline;
    bool held = false;
    float origin[3]{}, position[3]{}, alternate[3]{};
};
struct TraceState {
    bool active = false, held = false, capture = false, pending = false, reported = false, settlePending = false;
    DWORD windowAt = 0, pendingAt = 0, reportAt = 0, settleAt = 0;
    uint32_t window = 0, tick = 0;
    unsigned windows = 0, edges = 0, settled = 0, dropped = 0, readFailures = 0;
    Window reason = Window::Baseline;
    unsigned outcomes[2][3]{}; // Native zero, one and OTHER; no bool coercion.
    std::array<Key, kKeys> keys{};
    std::array<Detail, kDetails> details{};
    size_t detailCount = 0, windowDetails = 0;
};
static TraceState trace;
template<class T> static void Field(uintptr_t at, T& value, uint32_t flag, uint32_t& known) {
    if (at && ports.read && ports.read(at, &value, sizeof(value))) known |= flag;
    else ++trace.readFailures;
}
static void Begin(DWORD now, bool held, Window reason) {
    if (!trace.pending) { trace.pending = true; trace.pendingAt = now; }
    ++trace.window; if (!trace.window) ++trace.window;
    ++trace.windows;
    if (reason == Window::Edge) ++trace.edges;
    if (reason == Window::Settled) { ++trace.settled; trace.settlePending = false; }
    else { trace.settlePending = true; trace.settleAt = now; }
    trace.reason = reason;
    trace.held = held; trace.windowAt = now; trace.capture = true; trace.windowDetails = 0;
}
static bool Admit(uintptr_t car, uintptr_t view, Stage stage) {
    Key* free = nullptr;
    for (auto& key : trace.keys) {
        if (key.window == trace.window && key.car == car && key.view == view && key.stage == stage) {
            // Multiple native passes can revisit the same car this frame.
            // Reserve first-frame room for all 19 fleet entries before taking
            // their optional second sample on a later UiTick.
            if (key.count >= 2 || key.tick == trace.tick) return false;
            ++key.count; key.tick = trace.tick; return true;
        }
        if ((!key.window || key.window != trace.window) && !free) free = &key;
    }
    if (!free) { ++trace.dropped; return false; }
    *free = {car, view, trace.window, trace.tick, stage, 1}; return true;
}
static void Observe(Stage stage, uintptr_t car, uintptr_t cameraArg, uintptr_t view, uint32_t result) {
    // Outside a short input-edge window the hot-path cost is one bool check.
    if (!trace.capture) return;
    const DWORD now = ports.clock ? ports.clock() : 0;
    if (now - trace.windowAt > kCaptureMs) return;
    uintptr_t ride = 0; uint32_t kind = 0;
    if (!installed || !trace.active || !ports.context || !ports.context() ||
        !ports.owned || !ports.owned(car, ride, kind) || !ride || (kind != 2 && kind != 3)) return;
    ++trace.outcomes[unsigned(stage)][result == 0 ? 0 : result == 1 ? 1 : 2];
    if (trace.detailCount == trace.details.size() || trace.windowDetails == kWindowDetails) { ++trace.dropped; return; }
    if (!Admit(car, view, stage)) return;
    Detail detail{};
    detail.car = car; detail.ride = ride; detail.view = view; detail.cameraArg = cameraArg;
    detail.kind = kind; detail.window = trace.window; detail.stage = stage;
    detail.result = result; detail.held = trace.held; detail.age = now - trace.windowAt; detail.reason = trace.reason;
    Field(car + 0x550, detail.active, 1u << 0, detail.known);
    Field(car + 0x998, detail.hidden, 1u << 1, detail.known);
    Field(car + 0x5B0, detail.poseMask, 1u << 2, detail.known);
    Field(car + 0x5B4, detail.viewMask, 1u << 3, detail.known);
    Field(car + 0x5B8, detail.ghostMask, 1u << 4, detail.known);
    Field(car + 0x50, detail.origin, 1u << 5, detail.known);
    Field(car + 0x60, detail.position, 1u << 6, detail.known);
    Field(car + 0x5A0, detail.alternate, 1u << 7, detail.known);
    if (view) {
        Field(view + 4, detail.viewId, 1u << 8, detail.known);
        Field(view + 0x40, detail.viewCamera, 1u << 9, detail.known);
    }
    const uintptr_t camera = cameraArg ? cameraArg : detail.viewCamera;
    if (camera) Field(camera + 0x280, detail.ownTarget, 1u << 10, detail.known);
    // Exact 48B190 mapping for the ten native eView IDs, NOT process/player
    // identity. No native camera/projection routine is called by diagnostics.
    if ((detail.known & (1u << 8)) && detail.viewId >= 1 && detail.viewId <= 10) {
        const bool second = detail.viewId == 2 || detail.viewId == 5 || detail.viewId == 7 || detail.viewId == 10;
        Field(uintptr_t(0x8900AC) + (second ? 4 : 0), detail.root, 1u << 11, detail.known);
        if (detail.root) Field(detail.root + 0x234, detail.root234, 1u << 12, detail.known);
    }
    trace.details[trace.detailCount++] = detail; ++trace.windowDetails;
}
uint32_t __cdecl HookHide(uintptr_t car, uintptr_t view) {
    const uint32_t result = originalHide ? originalHide(car, view) : 0;
    Observe(Stage::Hide, car, 0, view, result);
    return result;
}
uint32_t __cdecl HookEligible(uintptr_t car, uintptr_t camera, uintptr_t view) {
    const uint32_t result = originalEligible ? originalEligible(car, camera, view) : 0;
    Observe(Stage::Eligible, car, camera, view, result);
    return result;
}
static void Flush(DWORD now) {
    if (!trace.pending || !ports.log || now - trace.windowAt < kCaptureMs ||
        (trace.reported ? now - trace.reportAt < kReportMs : now - trace.pendingAt < kReportMs)) return;
    static char report[196608];
    size_t used = 0;
    int wrote = std::snprintf(report, sizeof(report),
        "[REAR_RENDER] windows=%u edges=%u settled=%u details=%u dropped=%u readFailures=%u hide=zero:%u,one:%u,other:%u eligible=zero:%u,one:%u,other:%u; hide1=HIDDEN eligible1=PASS_NATIVE_CAR_GATES; B is input correlation, not an assumed eView.\r\n",
        trace.windows,trace.edges,trace.settled,unsigned(trace.detailCount),trace.dropped,trace.readFailures,
        trace.outcomes[0][0],trace.outcomes[0][1],trace.outcomes[0][2],trace.outcomes[1][0],trace.outcomes[1][1],trace.outcomes[1][2]);
    if (wrote > 0 && size_t(wrote) < sizeof(report)) used = size_t(wrote);
    for (size_t index = 0; index < trace.detailCount; ++index) {
        const auto& d = trace.details[index];
        wrote = std::snprintf(report + used, sizeof(report) - used,
            "[REAR_RENDER_DETAIL] win=%u B=%u capture=%s age=%u stage=%s original=%08X car=%08X kind=%u ride=%08X view=%08X id=%u cameraArg=%08X cameraView=%08X own=%08X root=%08X root234=%08X active=%u hidden=%u pose=%08X views=%08X ghost=%08X known=%08X origin=(%.2f,%.2f,%.2f) position=(%.2f,%.2f,%.2f) alternate=(%.2f,%.2f,%.2f)\r\n",
            d.window,unsigned(d.held),d.reason == Window::Settled ? "settled" : d.reason == Window::Edge ? "edge" : "baseline",
            unsigned(d.age),d.stage == Stage::Hide ? "hide" : "eligible",d.result,
            unsigned(d.car),d.kind,unsigned(d.ride),unsigned(d.view),d.viewId,unsigned(d.cameraArg),unsigned(d.viewCamera),
            unsigned(d.ownTarget),unsigned(d.root),d.root234,unsigned(d.active),unsigned(d.hidden),d.poseMask,d.viewMask,d.ghostMask,d.known,
            d.origin[0],d.origin[1],d.origin[2],d.position[0],d.position[1],d.position[2],d.alternate[0],d.alternate[1],d.alternate[2]);
        if (wrote < 0 || size_t(wrote) >= sizeof(report) - used) { report[used] = 0; break; }
        used += size_t(wrote);
    }
    trace.pending = false; trace.capture = false; trace.reported = true; trace.reportAt = now;
    trace.windows = trace.edges = trace.settled = trace.dropped = trace.readFailures = 0;
    trace.detailCount = 0; trace.keys = {};
    for (auto& stage : trace.outcomes) for (auto& count : stage) count = 0;
    ports.log(report); // Exactly one batched write, and only from UiTick.
}
void Tick(DWORD flow) {
    if (!installed || !ports.clock || !ports.key) return;
    ++trace.tick; if (!trace.tick) ++trace.tick;
    const DWORD now = ports.clock();
    const bool held = ports.key(); // One sample per UiTick, never from render hooks.
    const bool active = flow == 6 && ports.context && ports.context();
    if (active) {
        if (!trace.active) Begin(now, held, Window::Baseline);
        else if (held != trace.held) Begin(now, held, Window::Edge);
        else if (trace.settlePending && now - trace.settleAt >= kSettledMs) Begin(now, held, Window::Settled);
    }
    trace.active = active;
    if (!active) { trace.capture = false; trace.settlePending = false; }
    else if (now - trace.windowAt > kCaptureMs) trace.capture = false;
    Flush(now);
}
bool Install() {
    if (installed) return true;
    if (!ports.install) return false;
    // Complete non-relative instructions: push ebx; mov ebx,[esp+0C].
    static const BYTE hide[]{0x53,0x8B,0x5C,0x24,0x0C};
    // mov eax,[esp+08]; mov ecx,[eax+280].
    static const BYTE eligible[]{0x8B,0x44,0x24,0x08,0x8B,0x88,0x80,0x02,0x00,0x00};
    const bool first = ports.install(0x60D4F0,hide,sizeof(hide),reinterpret_cast<void*>(HookHide),reinterpret_cast<void**>(&originalHide));
    const bool second = first && ports.install(0x60D570,eligible,sizeof(eligible),reinterpret_cast<void*>(HookEligible),reinterpret_cast<void**>(&originalEligible));
    installed = first && second;
    if (ports.log) ports.log(installed ?
        "[REAR_RENDER] Passive client-owned traffic/AI render diagnostics ready; native return values unchanged.\r\n" :
        "[REAR_RENDER] Passive diagnostics unavailable/partial; any installed wrapper only forwards original native result.\r\n");
    return installed;
}
} // namespace CoopRearRenderTrace
