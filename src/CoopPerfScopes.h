// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Game-thread-only, inclusive QPC telemetry. No engine reads, allocation, file
// probing or settings changes. A complete five-second report is one log write.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

namespace CoopPerf {
enum class Part : unsigned {
    Special, Session, CareerPresentation, Sponsor, Movies, Pause, RaceLifecycle,
    Url, WorldImpact, Fleet, AI, Traces, Results, Appearance, PeerMotion,
    PeerLabel, Announcements, Total, Count
};
using Counter = uint64_t (*)();
using Milliseconds = uint32_t (*)();
using Logger = void (*)(const char*);
struct Sample { uint64_t ticks = 0, maximum = 0, hits = 0; };
struct State {
    Logger logger = nullptr;
    Counter counter = nullptr;
    Milliseconds milliseconds = nullptr;
    uint64_t frequency = 0, frames = 0;
    uint32_t startedAt = 0;
    bool started = false;
    std::array<Sample, static_cast<size_t>(Part::Count)> samples{};
};
inline State& Data() { static State state; return state; }
#ifdef _WIN32
inline uint64_t NativeCounter() {
    LARGE_INTEGER now{};
    return QueryPerformanceCounter(&now) ? static_cast<uint64_t>(now.QuadPart) : 0;
}
inline uint32_t NativeMilliseconds() { return GetTickCount(); }
inline uint64_t NativeFrequency() {
    static const uint64_t frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0
            ? static_cast<uint64_t>(value.QuadPart) : uint64_t(0);
    }();
    return frequency;
}
#endif
// Injectable clocks make aggregation/wrap tests independent of wall time.
// Initialize once on the same game thread that owns Scope and EndFrame.
inline void Initialize(Logger logger, Counter counter = nullptr,
    uint64_t frequency = 0, Milliseconds milliseconds = nullptr) {
#ifdef _WIN32
    if (!counter) { counter = NativeCounter; frequency = NativeFrequency(); }
    if (!milliseconds) milliseconds = NativeMilliseconds;
#endif
    auto& state = Data(); state = {};
    state.logger = logger; state.counter = counter;
    state.frequency = frequency; state.milliseconds = milliseconds;
}
inline bool Enabled() {
    const auto& state = Data();
    return state.logger && state.counter && state.milliseconds && state.frequency;
}
inline uint64_t Microseconds(uint64_t ticks, uint64_t frequency) {
    if (!frequency) return 0;
    return ticks / frequency * 1000000ull + ticks % frequency * 1000000ull / frequency;
}
class Scope {
    Part part_;
    uint64_t start_ = 0;
    bool enabled_ = false;
public:
    explicit Scope(Part part) : part_(part), enabled_(
        static_cast<size_t>(part) < static_cast<size_t>(Part::Count) && Enabled()) {
        if (enabled_) start_ = Data().counter();
    }
    ~Scope() {
        if (!enabled_) return;
        auto& state = Data();
        const uint64_t end = state.counter();
        if (end < start_) return; // Clock reset/failure must not underflow.
        const uint64_t elapsed = end - start_;
        auto& sample = state.samples[static_cast<size_t>(part_)];
        sample.ticks += elapsed; ++sample.hits;
        if (elapsed > sample.maximum) sample.maximum = elapsed;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
inline const char* Name(Part part) {
    static const char* const names[] = {
        "special", "session", "career_presentation", "sponsor", "movies", "pause",
        "race_lifecycle", "url", "world_impact", "fleet", "ai", "traces", "results",
        "appearance", "peer_motion", "peer_label", "announcements", "total"
    };
    const auto index = static_cast<size_t>(part);
    return index < static_cast<size_t>(Part::Count) ? names[index] : "invalid";
}
// All scopes, including Total, must have ended BEFORE EndFrame. An exit guard
// declared before Total enforces this for all UiTick early returns.
inline void EndFrame(bool active) {
    if (!Enabled()) return;
    auto& state = Data();
    const uint32_t now = state.milliseconds();
    if (!active) {
        state.samples = {}; state.frames = 0; state.started = false;
        return; // Menus/loading/startup intro do not pollute gameplay reports.
    }
    ++state.frames;
    if (!state.started) { state.started = true; state.startedAt = now; return; }
    const uint32_t elapsed = now - state.startedAt;
    if (elapsed < 5000u) return;
    char report[4096]{};
    size_t used = 0;
    int count = std::snprintf(report, sizeof(report),
        "[PERF_SCOPES] frames=%llu intervalMs=%u inclusive=1 scopeOverheadIncluded=1\r\n",
        static_cast<unsigned long long>(state.frames), unsigned(elapsed));
    if (count > 0 && static_cast<size_t>(count) < sizeof(report)) used = static_cast<size_t>(count);
    for (size_t index = 0; index < state.samples.size(); ++index) {
        const auto& sample = state.samples[index];
        if (!sample.hits) continue;
        count = std::snprintf(report + used, sizeof(report) - used,
            "[PERF_SCOPE] part=%s avg=%lluus max=%lluus hits=%llu frameAvg=%lluus\r\n",
            Name(static_cast<Part>(index)),
            static_cast<unsigned long long>(Microseconds(sample.ticks / sample.hits, state.frequency)),
            static_cast<unsigned long long>(Microseconds(sample.maximum, state.frequency)),
            static_cast<unsigned long long>(sample.hits),
            static_cast<unsigned long long>(Microseconds(sample.ticks / state.frames, state.frequency)));
        if (count < 0 || static_cast<size_t>(count) >= sizeof(report) - used) {
            report[used] = '\0'; break;
        }
        used += static_cast<size_t>(count);
    }
    // Clear before callback: logging time belongs to neither a later sample
    // nor another report, and cannot accidentally trigger recursive reporting.
    state.samples = {}; state.frames = 0; state.startedAt = now;
    state.logger(report);
}
} // namespace CoopPerf
