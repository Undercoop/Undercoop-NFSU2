// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once

#include "CoopMotion.h"
#include "CoopWorldWheel.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Portable authority/replication state. Native handles belong in an adapter;
// only keys, generations, descriptors and values cross the network boundary.
namespace coop_world {

constexpr std::size_t kMaxEntities = 64;
constexpr std::size_t kMaxDatagramBytes = 1200;
constexpr std::size_t kMaxPlannedEvents = 128;
constexpr std::uint32_t kPlayerTimeoutMs = 2500;
constexpr std::uint32_t kMaxLeaseMs = 5000;

inline std::int32_t SerialDelta(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t d = a - b;
    return d <= 0x7fffffffU ? static_cast<std::int32_t>(d) :
        static_cast<std::int32_t>(-1 - static_cast<std::int64_t>(0xffffffffU - d));
}
inline bool Newer(std::uint32_t a, std::uint32_t b) { return SerialDelta(a, b) > 0; }
inline bool NewerRequest(std::uint64_t a, std::uint64_t b) { const std::uint64_t d = a - b; return d && d < 0x8000000000000000ULL; }

struct Identity {
    std::uint64_t sessionId = 0;
    std::uint32_t trackId = 0, worldEpoch = 0, raceEpoch = 0;
    bool Valid() const { return sessionId != 0 && worldEpoch != 0; }
};
inline bool operator==(const Identity& a, const Identity& b) {
    return a.sessionId == b.sessionId && a.trackId == b.trackId &&
        a.worldEpoch == b.worldEpoch && a.raceEpoch == b.raceEpoch;
}
inline bool operator!=(const Identity& a, const Identity& b) { return !(a == b); }

struct EntityKey {
    std::uint64_t id = 0;
    std::uint32_t generation = 0;
    bool Valid() const { return id != 0 && generation != 0; }
};
inline bool operator==(const EntityKey& a, const EntityKey& b) { return a.id == b.id && a.generation == b.generation; }
inline bool operator!=(const EntityKey& a, const EntityKey& b) { return !(a == b); }

enum class Kind : std::uint8_t { Traffic = 1, AmbientRacer = 2, RaceRacer = 3 };
enum class Action : std::uint8_t { Spawn = 1, Update = 2, Remove = 3 };
enum class ApplyResult { Applied, Duplicate, Unauthorized, WrongIdentity, Invalid, Stale, Missing, Capacity, RosterFull };
enum class ClaimResult { Acquired, AlreadyOwned, Busy, Invalid, NotAuthority, Disconnected, ExpiredRequest };
constexpr std::uint8_t kFinishRacing = 0, kFinishComplete = 1, kFinishDnf = 2;

struct Descriptor {
    std::uint32_t modelId = 0, modelHash = 0;
    std::uint64_t appearanceHash = 0, handlingHash = 0;
    char carModel[16]{};
};
inline bool operator==(const Descriptor& a, const Descriptor& b) {
    return a.modelId == b.modelId && a.modelHash == b.modelHash && a.appearanceHash == b.appearanceHash &&
        a.handlingHash == b.handlingHash && std::memcmp(a.carModel, b.carModel, sizeof(a.carModel)) == 0;
}
inline bool ValidDescriptor(const Descriptor& descriptor) {
    if (!descriptor.modelHash || !descriptor.carModel[0] || !std::memchr(descriptor.carModel, 0, sizeof(descriptor.carModel))) return false;
    for (char c : descriptor.carModel) if (c && (c < 32 || c > 126)) return false;
    return true;
}

struct Motion {
    std::uint32_t sequence = 0, sentAtMs = 0, flowState = 6;
    std::uint64_t sessionId = 0;
    std::uint32_t trackId = 0, worldEpoch = 0;
    char carModel[16]{};
    float x = 0, y = 0, z = 0, speedKmh = 0, headingDeg = 0;
    float forwardX = 1, forwardY = 0, forwardZ = 0;
    std::int16_t steerAngle = 0;
    std::uint8_t brakeLight = 0, reverseLight = 0;
    float wheelPhaseRad[4]{}, wheelSpeedRad[4]{}, wheelRotation[4][9]{};
    std::uint32_t wheelRotationValidMask = 0;
    coop_world_wheel::State wheels{};
    float orientation[4]{0, 0, 0, 1}, linearVelocity[3]{}, angularVelocity[3]{};
    float totalProgress = 0, splineDistance = 0;
    std::uint8_t lap = 0, completedLaps = 0, finishStatus = kFinishRacing, finishRank = 0;
    std::uint8_t aiFrozen = 0;
    std::uint32_t finishTicks = 0;
    // Exact native 16-bit finish code; the portable semantic status above is
    // intentionally separate (native loss is 3, not semantic Dnf's 2).
    std::uint16_t nativeFinishStatus = 0;
    // Native mode 7 ranks Racer+0x24; other modes typically use progress+0x20.
    float nativeRaceScore = 0;
};
inline bool ValidMotion(const Motion& p) {
    const auto finite = [](float value, float limit) { return std::isfinite(value) && std::fabs(value) <= limit; };
    if (!finite(p.x, 1.0e7f) || !finite(p.y, 1.0e7f) || !finite(p.z, 1.0e7f) || !finite(p.speedKmh, 1500) ||
        !finite(p.headingDeg, 1.0e5f) || !finite(p.forwardX, 1.1f) || !finite(p.forwardY, 1.1f) || !finite(p.forwardZ, 1.1f) ||
        !coop_motion::ValidQuaternion(p.orientation) || !coop_world_wheel::Valid(p.wheels) || (p.wheelRotationValidMask & ~std::uint32_t(15)) ||
        !finite(p.totalProgress, 10000) || p.totalProgress < 0 || !finite(p.splineDistance, 1.0e7f) || p.splineDistance < 0 ||
        !finite(p.nativeRaceScore, 1.0e9f) ||
        p.lap > 127 || p.completedLaps > 127 || p.finishStatus > kFinishDnf || p.finishRank > kMaxEntities || p.aiFrozen > 1 ||
        p.brakeLight > 2 || p.reverseLight > 1) return false;
    if (p.finishStatus == kFinishRacing && (p.finishRank || p.finishTicks || p.nativeFinishStatus)) return false;
    if (p.finishStatus != kFinishRacing && (!p.nativeFinishStatus || p.nativeFinishStatus > 11)) return false;
    if (p.finishStatus == kFinishComplete && (!p.finishRank || !p.finishTicks)) return false;
    for (int i = 0; i != 3; ++i) if (!finite(p.linearVelocity[i], 400) || !finite(p.angularVelocity[i], 200)) return false;
    for (int wheel = 0; wheel != 4; ++wheel) {
        if (!finite(p.wheelPhaseRad[wheel], 1.0e8f) || !finite(p.wheelSpeedRad[wheel], 10000)) return false;
        for (float value : p.wheelRotation[wheel]) if (!finite(value, 2)) return false;
        float q[4];
        if ((p.wheelRotationValidMask & (1U << wheel)) && !coop_motion::WheelQuaternion(p.wheelRotation[wheel], q)) return false;
    }
    return true;
}
inline void CopyRaceProgress(const Motion& source, Motion& output) {
    output.totalProgress = source.totalProgress; output.splineDistance = source.splineDistance;
    output.lap = source.lap; output.completedLaps = source.completedLaps;
    output.finishStatus = source.finishStatus; output.finishRank = source.finishRank; output.finishTicks = source.finishTicks;
    output.nativeFinishStatus = source.nativeFinishStatus;
    output.nativeRaceScore = source.nativeRaceScore;
    output.aiFrozen = source.aiFrozen;
}

struct EntityState {
    EntityKey key;
    Kind kind = Kind::Traffic;
    std::uint64_t raceMembershipId = 0;
    Descriptor descriptor;
    Motion motion;
};
struct Event {
    Identity identity;
    // Global host event sequence; per-entity ordering allows packet reordering
    // between unrelated entities without dropping their independent spawns.
    std::uint32_t sequence = 0;
    Action action = Action::Update;
    EntityState state;
};
struct RacePolicy {
    std::uint64_t raceId = 0;
    std::uint16_t vehicleCapacity = 0;
    std::uint8_t playerCount = 2;
    bool trafficAllowed = true;
};
struct PlayerRegion {
    std::uint32_t playerId = 0;
    bool active = false;
    float x = 0, y = 0, z = 0, radius = 250;
};
struct InterestResult { std::size_t count = 0, total = 0; bool truncated = false; };
// sampledSnapshotAtMs is the actual representative input snapshot, NOT an
// invented precise playback timestamp. Source/receiver uptimes are unrelated.
struct MotionDiagnostics {
    std::size_t historyCount = 0;
    std::uint32_t delayMs = 0, extrapolationMs = 0, resetCount = 0, coalescedTimes = 0;
    std::uint32_t sourceAtMs = 0, receiveAtMs = 0, sampledSnapshotAtMs = 0, sampleAtMs = 0;
    std::int32_t sourceStepMs = 0, renderStepMs = 0;
    bool hasSample = false;
};
struct Lease {
    EntityKey key;
    std::uint32_t ownerPlayerId = 0, expiresAtMs = 0, sequence = 0;
    std::uint64_t requestId = 0;
};
struct LeaseMessage {
    Identity identity;
    EntityKey key;
    std::uint32_t sequence = 0, ownerPlayerId = 0, durationMs = 0;
    std::uint64_t requestId = 0;
};

struct DatagramPlan { std::uint16_t firstEvent = 0, eventCount = 0, bytes = 0; };
inline bool PlanDatagrams(const std::uint16_t* encodedEventBytes, std::size_t eventCount,
    DatagramPlan* plans, std::size_t planCapacity, std::size_t& planCount, std::size_t headerBytes = 40) {
    planCount = 0;
    if (eventCount > kMaxPlannedEvents || headerBytes >= kMaxDatagramBytes ||
        (eventCount && (!encodedEventBytes || !plans))) return false;
    DatagramPlan pending[kMaxPlannedEvents]{};
    std::size_t count = 0;
    for (std::size_t i = 0; i != eventCount; ++i) {
        const std::size_t bytes = encodedEventBytes[i];
        if (!bytes || bytes + headerBytes > kMaxDatagramBytes) return false;
        if (!count || pending[count - 1].bytes + bytes > kMaxDatagramBytes) {
            pending[count].firstEvent = static_cast<std::uint16_t>(i);
            pending[count].bytes = static_cast<std::uint16_t>(headerBytes);
            ++count;
        }
        ++pending[count - 1].eventCount;
        pending[count - 1].bytes = static_cast<std::uint16_t>(pending[count - 1].bytes + bytes);
    }
    if (count > planCapacity) return false;
    for (std::size_t i = 0; i != count; ++i) plans[i] = pending[i];
    planCount = count;
    return true;
}

template<std::size_t Capacity = 32>
class World {
    static_assert(Capacity > 0 && Capacity <= kMaxEntities, "World capacity must be 1..64");
public:
    bool Begin(const Identity& identity, std::uint32_t hostPlayerId, std::uint32_t localPlayerId,
        const RacePolicy& race = RacePolicy{}, std::uint32_t now = 0) {
        if (!identity.Valid() || !PlayerValid(hostPlayerId) || !PlayerValid(localPlayerId) || !RaceValid(identity, race)) return false;
        if (begun_ && identity == identity_ && hostPlayerId == hostPlayerId_ && localPlayerId == localPlayerId_) {
            return SetRacePolicy(race, hostPlayerId);
        }
        identity_ = identity; hostPlayerId_ = hostPlayerId; localPlayerId_ = localPlayerId;
        race_ = race; count_ = 0; hasFloor_ = false; tombstoneClock_ = 0; begun_ = true;
        for (auto& slot : slots_) { slot.occupied = false; slot.motion.Reset(); slot.lease = Lease{}; slot.hasLeaseSequence = false; }
        for (auto& tomb : tombstones_) tomb.used = false;
        for (auto& player : players_) player = Presence{};
        RefreshPlayer(hostPlayerId, now); RefreshPlayer(localPlayerId, now);
        return true;
    }
    bool Begin(const Identity& identity, std::uint32_t hostPlayerId, const RacePolicy& race = RacePolicy{}, std::uint32_t now = 0) {
        return Begin(identity, hostPlayerId, hostPlayerId, race, now);
    }
    bool IsAuthority() const { return begun_ && localPlayerId_ == hostPlayerId_; }
    const Identity& CurrentIdentity() const { return identity_; }
    std::size_t Count() const { return count_; }
    std::size_t RaceRacerCount() const {
        std::size_t count = 0;
        for (const auto& slot : slots_) if (slot.occupied && slot.state.kind == Kind::RaceRacer) ++count;
        return count;
    }
    std::size_t RaceAiCapacity() const { return race_.raceId ? race_.vehicleCapacity - race_.playerCount : 0; }
    bool SetRacePolicy(const RacePolicy& race, std::uint32_t sourcePlayerId) {
        if (!begun_ || sourcePlayerId != hostPlayerId_ || !RaceValid(identity_, race)) return false;
        for (const auto& slot : slots_)
            if (slot.occupied && slot.state.kind == Kind::RaceRacer && slot.state.raceMembershipId != race.raceId) return false;
        for (const auto& slot : slots_)
            if (slot.occupied && slot.state.kind == Kind::Traffic && !race.trafficAllowed) return false;
        if (RaceRacerCount() > (race.raceId ? std::size_t(race.vehicleCapacity - race.playerCount) : 0)) return false;
        race_ = race;
        return true;
    }

    ApplyResult Apply(const Event& event, std::uint32_t sourcePlayerId, std::uint32_t receiveMs) {
        if (!begun_ || sourcePlayerId != hostPlayerId_) return ApplyResult::Unauthorized;
        if (event.identity != identity_) return ApplyResult::WrongIdentity;
        if (!event.state.key.Valid() || (event.action != Action::Spawn && event.action != Action::Update && event.action != Action::Remove)) return ApplyResult::Invalid;
        if (hasFloor_ && !Newer(event.sequence, sequenceFloor_)) return ApplyResult::Stale;
        Slot* slot = FindId(event.state.key.id);
        Tombstone* tomb = FindTombstone(event.state.key.id);
        if (slot) {
            if (event.state.key.generation != slot->state.key.generation) {
                if (!Newer(event.state.key.generation, slot->state.key.generation) || event.action == Action::Update) return ApplyResult::Stale;
            } else if (event.sequence == slot->lastSequence) return ApplyResult::Duplicate;
            if (!Newer(event.sequence, slot->lastSequence)) return ApplyResult::Stale;
        } else if (tomb) {
            if (event.state.key.generation == tomb->key.generation) {
                if (event.action == Action::Remove && event.sequence == tomb->sequence) return ApplyResult::Duplicate;
                if (event.action != Action::Remove || !Newer(event.sequence, tomb->sequence)) return ApplyResult::Stale;
            } else if (!Newer(event.state.key.generation, tomb->key.generation) || !Newer(event.sequence, tomb->sequence)) return ApplyResult::Stale;
        }
        if (event.action == Action::Remove) {
            if (slot) {
                slot->occupied = false;
                slot->lease = Lease{};
                slot->motion.Reset();
                --count_;
            }
            RememberRemoval(event.state.key, event.sequence);
            return ApplyResult::Applied;
        }
        if (!StateValid(event.state)) return ApplyResult::Invalid;
        if (!slot && event.action == Action::Update) return ApplyResult::Missing;
        const bool replace = slot && event.state.key != slot->state.key;
        if (slot && !replace && (slot->state.kind != event.state.kind || slot->state.raceMembershipId != event.state.raceMembershipId ||
            !(slot->state.descriptor == event.state.descriptor))) return ApplyResult::Invalid;
        const bool alreadyRacer = slot && slot->state.kind == Kind::RaceRacer;
        if (event.state.kind == Kind::RaceRacer && RaceRacerCount() - (alreadyRacer ? 1 : 0) >= RaceAiCapacity()) return ApplyResult::RosterFull;
        if (!slot) {
            for (auto& candidate : slots_) if (!candidate.occupied) { slot = &candidate; break; }
            if (!slot) return ApplyResult::Capacity;
        }
        Motion motion = event.state.motion;
        if (slot->occupied && !replace && slot->state.motion.finishStatus != kFinishRacing) {
            const Motion& final = slot->state.motion;
            if (motion.finishStatus != kFinishRacing && (motion.finishStatus != final.finishStatus || motion.finishRank != final.finishRank ||
                motion.finishTicks != final.finishTicks || motion.nativeFinishStatus != final.nativeFinishStatus)) return ApplyResult::Invalid;
            motion.finishStatus = final.finishStatus; motion.finishRank = final.finishRank; motion.finishTicks = final.finishTicks;
            motion.nativeFinishStatus = final.nativeFinishStatus;
            motion.nativeRaceScore = final.nativeRaceScore;
            motion.aiFrozen = final.aiFrozen;
        }
        if (motion.finishStatus != kFinishRacing) motion.aiFrozen = 1;
        motion.sequence = event.sequence; motion.flowState = 6; motion.sessionId = identity_.sessionId;
        motion.worldEpoch = identity_.worldEpoch; motion.trackId = identity_.trackId;
        std::memcpy(motion.carModel, event.state.descriptor.carModel, sizeof(motion.carModel));
        if (!slot->occupied || replace) {
            if (replace) RememberRemoval(slot->state.key, slot->lastSequence);
            slot->motion.Reset(); slot->lease = Lease{}; slot->hasLeaseSequence = false;
            slot->diagnostics = MotionDiagnostics{};
            slot->lastReleasedPlayer = 0; slot->lastReleasedRequest = 0;
            for (int player = 0; player != 2; ++player) { slot->hasClaimRequest[player] = false; slot->latestClaimRequest[player] = 0; }
        }
        const auto sourceStep = slot->occupied && !replace ? SerialDelta(motion.sentAtMs, slot->state.motion.sentAtMs) : 0;
        const double dx = double(motion.x) - slot->state.motion.x, dy = double(motion.y) - slot->state.motion.y, dz = double(motion.z) - slot->state.motion.z;
        // GetTickCount and millisecond wire timestamps can repeat at high FPS.
        // A newer sequence in the SAME source tick is not a clock reset. Keep
        // authoritative progress/results latest but retain the existing visual
        // sample; the next positive source delta carries the next pose. A real
        // relocation (>35m even within one tick) still goes through Push/reset.
        const bool sameTick = slot->occupied && !replace && sourceStep == 0 && dx * dx + dy * dy + dz * dz <= 35.0 * 35.0;
        if (sameTick) ++slot->diagnostics.coalescedTimes;
        else if (!slot->motion.Push(motion, receiveMs)) return ApplyResult::Invalid;
        slot->diagnostics.sourceAtMs = motion.sentAtMs; slot->diagnostics.receiveAtMs = receiveMs;
        slot->diagnostics.sourceStepMs = sourceStep;
        if (!slot->occupied) ++count_;
        slot->state = event.state; slot->state.motion = motion;
        slot->lastSequence = event.sequence; slot->occupied = true;
        return ApplyResult::Applied;
    }

    const EntityState* Find(EntityKey key) const {
        const Slot* slot = FindId(key.id);
        return slot && slot->state.key == key ? &slot->state : nullptr;
    }
    bool Sample(EntityKey key, std::uint32_t now, EntityState& output) {
        Slot* slot = FindId(key.id);
        if (!slot || slot->state.key != key) return false;
        output = slot->state;
        if (!slot->motion.Sample(now, output.motion)) return false;
        // Progress/results follow the latest authority snapshot; only the
        // rendered transform waits on the visual jitter buffer.
        CopyRaceProgress(slot->state.motion, output.motion);
        if (output.motion.aiFrozen) {
            output.motion = slot->state.motion;
            for (int i = 0; i != 3; ++i) output.motion.linearVelocity[i] = output.motion.angularVelocity[i] = 0;
            for (float& speed : output.motion.wheelSpeedRad) speed = 0;
            output.motion.speedKmh = 0;
        }
        auto& diagnostic = slot->diagnostics;
        diagnostic.renderStepMs = diagnostic.hasSample ? SerialDelta(now, diagnostic.sampleAtMs) : 0;
        diagnostic.sampleAtMs = now; diagnostic.sampledSnapshotAtMs = output.motion.sentAtMs; diagnostic.hasSample = true;
        return true;
    }
    bool ReadMotionDiagnostics(EntityKey key, MotionDiagnostics& output) const {
        const Slot* slot = FindId(key.id);
        if (!slot || slot->state.key != key) return false;
        output = slot->diagnostics;
        output.historyCount = slot->motion.Count(); output.delayMs = slot->motion.DelayMs();
        output.extrapolationMs = slot->motion.ExtrapolationMs(); output.resetCount = slot->motion.ResetCount();
        return true;
    }

    InterestResult SelectInterest(const PlayerRegion (&regions)[2], EntityKey* output, std::size_t outputCapacity) const {
        struct Candidate { const Slot* slot; float score; };
        Candidate candidates[Capacity]{};
        std::size_t count = 0;
        for (const auto& slot : slots_) {
            if (!slot.occupied) continue;
            bool interested = slot.state.kind == Kind::RaceRacer && slot.state.raceMembershipId == race_.raceId;
            float score = interested ? -1.0f : 1.0e30f;
            for (const auto& region : regions) {
                if (!RegionValid(region)) continue;
                const float dx = slot.state.motion.x - region.x, dy = slot.state.motion.y - region.y, dz = slot.state.motion.z - region.z;
                const float relative = (dx * dx + dy * dy + dz * dz) / (region.radius * region.radius);
                if (relative <= 1.0f) { interested = true; score = std::min(score, relative); }
            }
            if (interested) candidates[count++] = Candidate{&slot, score};
        }
        std::sort(candidates, candidates + count, [](const Candidate& a, const Candidate& b) {
            if (a.score != b.score) return a.score < b.score;
            return a.slot->state.key.id < b.slot->state.key.id;
        });
        const std::size_t written = output ? std::min(count, outputCapacity) : 0;
        for (std::size_t i = 0; i != written; ++i) output[i] = candidates[i].slot->state.key;
        return InterestResult{written, count, written != count};
    }

    bool RefreshPlayer(std::uint32_t playerId, std::uint32_t now) {
        if (!begun_ || !PlayerValid(playerId)) return false;
        Presence& player = players_[playerId - 1];
        if (player.connected && SerialDelta(now, player.lastSeen) < 0) return false;
        player.connected = true; player.lastSeen = now;
        return true;
    }
    bool Disconnect(std::uint32_t playerId) {
        if (!PlayerValid(playerId)) return false;
        players_[playerId - 1].connected = false;
        for (auto& slot : slots_) if (slot.occupied && slot.lease.ownerPlayerId == playerId) ClearLease(slot, IsAuthority());
        return true;
    }
    void Expire(std::uint32_t now) {
        for (auto& player : players_)
            if (player.connected && SerialDelta(now, player.lastSeen) > static_cast<std::int32_t>(kPlayerTimeoutMs)) player.connected = false;
        for (auto& slot : slots_)
            if (slot.occupied && slot.lease.ownerPlayerId && (!Connected(slot.lease.ownerPlayerId, now) || SerialDelta(now, slot.lease.expiresAtMs) >= 0)) ClearLease(slot, IsAuthority());
    }
    ClaimResult TryClaim(EntityKey key, std::uint32_t playerId, std::uint64_t requestId, std::uint32_t now, std::uint32_t durationMs = 1500) {
        if (!IsAuthority()) return ClaimResult::NotAuthority;
        if (!PlayerValid(playerId) || !requestId || durationMs < 100 || durationMs > kMaxLeaseMs) return ClaimResult::Invalid;
        Expire(now);
        if (!Connected(playerId, now)) return ClaimResult::Disconnected;
        Slot* slot = FindId(key.id);
        if (!slot || slot->state.key != key || slot->state.kind != Kind::AmbientRacer) return ClaimResult::Invalid;
        if (slot->lease.ownerPlayerId) {
            if (slot->lease.ownerPlayerId == playerId && slot->lease.requestId == requestId) return ClaimResult::AlreadyOwned;
            return ClaimResult::Busy;
        }
        // Request IDs are monotonic serials per player/key. High-water marks
        // make delayed retries idempotent even after another player's lease.
        const std::size_t playerIndex = playerId - 1;
        if (slot->hasClaimRequest[playerIndex] && !NewerRequest(requestId, slot->latestClaimRequest[playerIndex])) return ClaimResult::ExpiredRequest;
        for (const auto& other : slots_) if (other.occupied && other.lease.ownerPlayerId == playerId) return ClaimResult::Busy;
        slot->lease.key = key; slot->lease.ownerPlayerId = playerId; slot->lease.requestId = requestId;
        slot->lease.expiresAtMs = now + durationMs; ++slot->lease.sequence; slot->hasLeaseSequence = true;
        slot->hasClaimRequest[playerIndex] = true; slot->latestClaimRequest[playerIndex] = requestId;
        return ClaimResult::Acquired;
    }
    bool Release(EntityKey key, std::uint32_t playerId, std::uint64_t requestId, std::uint32_t now) {
        if (!IsAuthority() || !PlayerValid(playerId) || !requestId) return false;
        Expire(now);
        Slot* slot = FindId(key.id);
        if (!slot || slot->state.key != key) return false;
        if (!slot->lease.ownerPlayerId) {
            const std::size_t player = playerId - 1;
            return slot->hasClaimRequest[player] && !NewerRequest(requestId, slot->latestClaimRequest[player]);
        }
        if (slot->lease.ownerPlayerId != playerId || slot->lease.requestId != requestId) return false;
        ClearLease(*slot);
        return true;
    }
    // Renewal cannot acquire an unowned/expired lease or change its owner.
    // Only a fresh heartbeat of the exact granted challenge may extend it.
    bool RenewClaim(EntityKey key, std::uint32_t playerId, std::uint64_t requestId,
        std::uint32_t now, std::uint32_t durationMs) {
        if (!IsAuthority() || durationMs < 100 || durationMs > kMaxLeaseMs) return false;
        Lease existing{};
        if (!GetLease(key, now, existing) || existing.ownerPlayerId != playerId || existing.requestId != requestId) return false;
        Slot* slot = FindId(key.id);
        slot->lease.expiresAtMs = now + durationMs; ++slot->lease.sequence; slot->hasLeaseSequence = true; return true;
    }
    bool GetLease(EntityKey key, std::uint32_t now, Lease& output) const {
        const Slot* slot = FindId(key.id);
        if (!slot || slot->state.key != key || !slot->lease.ownerPlayerId || !Connected(slot->lease.ownerPlayerId, now) || SerialDelta(now, slot->lease.expiresAtMs) >= 0) return false;
        output = slot->lease;
        return true;
    }
    bool GetLeaseMessage(EntityKey key, std::uint32_t now, LeaseMessage& output) {
        if (!IsAuthority()) return false;
        Expire(now);
        const Slot* slot = FindId(key.id);
        if (!slot || slot->state.key != key || slot->state.kind != Kind::AmbientRacer || !slot->hasLeaseSequence) return false;
        output = LeaseMessage{}; output.identity = identity_; output.key = key; output.sequence = slot->lease.sequence;
        Lease lease;
        if (GetLease(key, now, lease)) {
            output.ownerPlayerId = lease.ownerPlayerId; output.requestId = lease.requestId;
            output.durationMs = static_cast<std::uint32_t>(SerialDelta(lease.expiresAtMs, now));
        }
        return true;
    }
    ApplyResult ApplyLease(const LeaseMessage& message, std::uint32_t sourcePlayerId, std::uint32_t receiveMs) {
        if (!begun_ || sourcePlayerId != hostPlayerId_) return ApplyResult::Unauthorized;
        if (message.identity != identity_) return ApplyResult::WrongIdentity;
        Slot* slot = FindId(message.key.id);
        if (!slot || slot->state.key != message.key || slot->state.kind != Kind::AmbientRacer) return ApplyResult::Missing;
        if (slot->hasLeaseSequence) {
            if (message.sequence == slot->lease.sequence) return ApplyResult::Duplicate;
            if (!Newer(message.sequence, slot->lease.sequence)) return ApplyResult::Stale;
        }
        if ((message.ownerPlayerId && (!PlayerValid(message.ownerPlayerId) || !message.requestId || !message.durationMs || message.durationMs > kMaxLeaseMs)) ||
            (!message.ownerPlayerId && (message.requestId || message.durationMs))) return ApplyResult::Invalid;
        slot->lease = Lease{message.key, message.ownerPlayerId, receiveMs + message.durationMs, message.sequence, message.requestId};
        slot->hasLeaseSequence = true;
        if (message.ownerPlayerId) {
            RefreshPlayer(message.ownerPlayerId, receiveMs);
            slot->hasClaimRequest[message.ownerPlayerId - 1] = true;
            slot->latestClaimRequest[message.ownerPlayerId - 1] = message.requestId;
        }
        return ApplyResult::Applied;
    }

private:
    struct Presence { bool connected = false; std::uint32_t lastSeen = 0; };
    struct Slot {
        bool occupied = false, hasLeaseSequence = false;
        EntityState state;
        coop_motion::Buffer<Motion> motion;
        MotionDiagnostics diagnostics;
        std::uint32_t lastSequence = 0, lastReleasedPlayer = 0;
        std::uint64_t lastReleasedRequest = 0;
        bool hasClaimRequest[2]{};
        std::uint64_t latestClaimRequest[2]{};
        Lease lease;
    };
    struct Tombstone { bool used = false; EntityKey key; std::uint32_t sequence = 0; std::uint64_t age = 0; };
    Slot slots_[Capacity]{};
    Tombstone tombstones_[Capacity * 2]{};
    Presence players_[2]{};
    Identity identity_;
    RacePolicy race_;
    std::uint32_t hostPlayerId_ = 0, localPlayerId_ = 0, sequenceFloor_ = 0;
    std::uint64_t tombstoneClock_ = 0;
    std::size_t count_ = 0;
    bool begun_ = false, hasFloor_ = false;

    static bool PlayerValid(std::uint32_t player) { return player == 1 || player == 2; }
    bool Connected(std::uint32_t playerId, std::uint32_t now) const {
        if (!PlayerValid(playerId)) return false;
        const Presence& player = players_[playerId - 1];
        const auto elapsed = SerialDelta(now, player.lastSeen);
        return player.connected && elapsed >= 0 && elapsed <= static_cast<std::int32_t>(kPlayerTimeoutMs);
    }
    static bool RaceValid(const Identity& identity, const RacePolicy& policy) {
        if (policy.playerCount < 1 || policy.playerCount > 2) return false;
        return policy.raceId ? identity.raceEpoch != 0 && policy.vehicleCapacity >= policy.playerCount && policy.vehicleCapacity <= kMaxEntities : policy.vehicleCapacity == 0;
    }
    static bool Finite(float value, float limit) { return std::isfinite(value) && std::fabs(value) <= limit; }
    static bool RegionValid(const PlayerRegion& region) {
        return region.active && PlayerValid(region.playerId) && Finite(region.x, 1.0e7f) && Finite(region.y, 1.0e7f) && Finite(region.z, 1.0e7f) &&
            std::isfinite(region.radius) && region.radius >= 1.0f && region.radius <= 2000.0f;
    }
    bool StateValid(const EntityState& state) const {
        if (state.kind != Kind::Traffic && state.kind != Kind::AmbientRacer && state.kind != Kind::RaceRacer) return false;
        if (state.kind == Kind::Traffic && !race_.trafficAllowed) return false;
        if (state.kind == Kind::RaceRacer ? !race_.raceId || state.raceMembershipId != race_.raceId : state.raceMembershipId != 0) return false;
        return ValidDescriptor(state.descriptor) && ValidMotion(state.motion);
    }
    Slot* FindId(std::uint64_t id) {
        for (auto& slot : slots_) if (slot.occupied && slot.state.key.id == id) return &slot;
        return nullptr;
    }
    const Slot* FindId(std::uint64_t id) const {
        for (const auto& slot : slots_) if (slot.occupied && slot.state.key.id == id) return &slot;
        return nullptr;
    }
    Tombstone* FindTombstone(std::uint64_t id) {
        for (auto& tomb : tombstones_) if (tomb.used && tomb.key.id == id) return &tomb;
        return nullptr;
    }
    void RememberRemoval(EntityKey key, std::uint32_t sequence) {
        Tombstone* chosen = FindTombstone(key.id);
        if (!chosen) for (auto& tomb : tombstones_) if (!tomb.used) { chosen = &tomb; break; }
        if (!chosen) {
            chosen = &tombstones_[0];
            for (auto& tomb : tombstones_) if (tomb.age < chosen->age) chosen = &tomb;
            // Bounded eviction advances a global late-event floor. An old,
            // unseen spawn cannot resurrect a removal whose tomb was evicted.
            if (!hasFloor_ || Newer(chosen->sequence, sequenceFloor_)) sequenceFloor_ = chosen->sequence;
            hasFloor_ = true;
        }
        *chosen = Tombstone{true, key, sequence, ++tombstoneClock_};
    }
    static void ClearLease(Slot& slot, bool advanceSequence = true) {
        if (slot.lease.ownerPlayerId) {
            slot.lastReleasedPlayer = slot.lease.ownerPlayerId; slot.lastReleasedRequest = slot.lease.requestId;
        }
        const std::uint32_t sequence = slot.lease.sequence + (advanceSequence ? 1 : 0);
        slot.lease = Lease{}; slot.lease.key = slot.state.key; slot.lease.sequence = sequence;
        slot.hasLeaseSequence = true;
    }
};

} // namespace coop_world
