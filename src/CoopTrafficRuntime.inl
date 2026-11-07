// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopWorldWire.h"
#include <array>
#include <new>

// Game-thread component, not a native vehicle factory. The caller supplies a
// host-issued Identity and a LOCAL post-Stage7 generation token, never infers
// an epoch from an array pointer. Network callers must first check the current
// session/peer endpoint and queue packets onto the game thread.
//
// This supports strict, already-loaded pooled replicas. It does NOT implement
// arbitrary creation, palette reference ownership, model materialization,
// native two-centre generation or the outrun approval hook. Those remain
// explicit integration requirements; all native mutation defaults OFF.
namespace CoopTrafficRuntime {
constexpr std::size_t kLimit = 22;
constexpr std::size_t kRetireLimit = kLimit * 2;
constexpr std::size_t kDatagramLimit = kLimit * 3 + kRetireLimit;
constexpr std::uint64_t kTrafficIdBase = 0x200000000ULL;
constexpr std::uint64_t kAmbientIdBase = 0x300000000ULL;

enum class ScopeKind { FreeRoam, Race };
enum class Readiness { Ready, Dormant, Pending };
struct NativeRow {
    // Handles are process-local only. None of this structure is serialized.
    std::uintptr_t car = 0, parameter = 0, ride = 0, mover = 0, witness = 0;
    std::uint32_t nativeKind = 0, mode = 0;
    std::uint8_t nativeId = 0;
    Readiness readiness = Readiness::Pending;
    coop_world::Descriptor descriptor{};
    coop_world::Motion motion{};
};
struct Catalog {
    std::uint64_t localGeneration = 0;
    std::uint32_t track = 0;
    std::size_t count = 0;
    bool stable = false;
    NativeRow rows[kLimit]{};
};
struct Capabilities {
    // Set ONLY after typed, signature-checked hooks are installed. Suppressing
    // the whole 42F910 controller would break 4111C0/412950 resource upkeep.
    bool trafficMover = false;        // 408820, float __thiscall(..., float)
    bool trafficPlacement = false;    // narrow 42FA9D -> 42F800 placement call
    bool trafficTransitions = false;  // 601369/601383 -> 409770/409960
    bool ambientDriver = false;
    bool ambientTransitions = false;
    bool ambientEngagement = false;   // native challenge approval consumes lease
    bool hostPeerFocus = false;       // 42F800 otherwise uses local view roots
    bool hostPeerStreaming = false;   // second 5D6040 centre AFTER 44F193
    bool hostPeerPalette = false;     // 412950 otherwise uses first view region
};
struct Port {
    void* context = nullptr;
    // Re-read native identity/model/appearance/witness AND resource readiness
    // on EVERY ownership query. Must exclude both human cars and never write
    // native memory. Catalog Ready alone is a readable pose, not an asset lease.
    bool (*matches)(void*, const NativeRow&) = nullptr;
    // Existing audited physical adapter, INCLUDING wheel/steer/light values.
    // This callback is reachable only on a client with matching loaded pool.
    bool (*apply)(void*, const NativeRow&, const coop_world::Motion&) = nullptr;
    // Manifest reservation is independent of the first pose/model lease. It
    // keeps native client activation suppressed during dormant/pending states.
    bool (*reserved)(void*, const NativeRow&) = nullptr;
    // Audited visibility port only: this does not claim a collider transition.
    bool (*visible)(void*, const NativeRow&, bool) = nullptr;
};
struct Datagram {
    std::uint16_t size = 0;
    std::uint8_t bytes[sizeof(coop_world_wire::EventPacket)]{};
};
struct Statistics {
    std::uint32_t received = 0, rejected = 0, applied = 0, applyRefused = 0;
    std::uint32_t pendingModels = 0, captureRefused = 0, catalogRefused = 0;
};
inline coop_world::Kind WorldKind(std::uint32_t type) {
    return type == 3 ? coop_world::Kind::Traffic : coop_world::Kind::AmbientRacer;
}
inline std::uint64_t EntityId(std::uint32_t type, std::uint8_t nativeId) {
    return (type == 3 ? kTrafficIdBase : kAmbientIdBase) + nativeId;
}
inline bool NativeIdValid(std::uint32_t type, std::uint8_t id) {
    return type == 3 ? id >= 50 && id < 66 :
        type == 2 && id >= 100 && id < 103;
}
inline bool DecodeId(std::uint64_t id, std::uint32_t& type, std::uint8_t& nativeId) {
    if (id >= kTrafficIdBase + 50 && id < kTrafficIdBase + 66) type = 3;
    else if (id >= kAmbientIdBase + 100 && id < kAmbientIdBase + 103) type = 2;
    else return false;
    nativeId = static_cast<std::uint8_t>(id - (type == 3 ? kTrafficIdBase : kAmbientIdBase));
    return true;
}

// Allocate statically or on the heap: World<22> includes bounded histories.
class Runtime {
public:
    bool Configure(const Port& port, const Capabilities& capabilities) {
        if (begun_) return false; // Never drop installed guards under a replica.
        port_ = port; capabilities_ = capabilities; return true;
    }
    bool Begin(const coop_world::Identity& identity, bool authority, ScopeKind scope,
        std::uint64_t localGeneration, bool trafficAllowed, std::uint32_t now) {
        if (!identity.Valid() || !identity.trackId || !localGeneration ||
            ((scope == ScopeKind::Race) != (identity.raceEpoch != 0))) return false;
        // Generic Begin must not reset counters under a live Host epoch.
        // Only explicit RebindNative preserves those counters during reload.
        if (begun_ && authority && authority_ && identity == identity_ && localGeneration != localGeneration_) return false;
        if (begun_ && identity == identity_ && authority == authority_ &&
            localGeneration == localGeneration_ && scope == scope_ && trafficAllowed == trafficAllowed_) return true;
        Reset();
        coop_world::RacePolicy policy{}; policy.raceId = identity.raceEpoch;
        policy.vehicleCapacity = scope == ScopeKind::Race ? 2 : 0;
        policy.trafficAllowed = trafficAllowed;
        if (!world_.Begin(identity, 1, authority ? 1 : 2, policy, now)) return false;
        identity_ = identity; authority_ = authority; scope_ = scope;
        localGeneration_ = localGeneration; trafficAllowed_ = trafficAllowed; begun_ = true;
        return true;
    }
    // Explicit local reload of an already-authorized live FreeRoam. Unlike
    // Reset/Begin, preserve all wire serials, tombstones and entity generation
    // high-water marks. Host native handles are retired even if the allocator
    // reuses every address; Client handles unbind until a fresh catalog matches.
    bool RebindNative(const coop_world::Identity& identity, std::uint64_t localGeneration, std::uint32_t now) {
        if (!begun_ || identity != identity_ || scope_ != ScopeKind::FreeRoam || identity.raceEpoch || !localGeneration) return false;
        if(localGeneration==localGeneration_) return true;
        if(!coop_world::NewerRequest(localGeneration, localGeneration_)) return false;
        if (authority_) {
            for (auto& slot : slots_) if (slot.present) Retire(slot, now);
            for (const auto& slot : slots_) if (slot.present) return false;
        } else {
            for (auto& slot : slots_) { slot.matched = false; slot.native = NativeRow{}; }
        }
        catalog_ = Catalog{};pendingCount_ = 0;localGeneration_ = localGeneration;
        return true;
    }
    void Reset() {
        // World::Begin deliberately preserves an existing equal identity.
        // Reconstruct in place so an explicit reset actually clears histories
        // without a half-megabyte temporary on the game's small x86 stack.
        world_.~WorldState(); ::new (static_cast<void*>(&world_)) WorldState{};
        begun_ = false; identity_ = {}; localGeneration_ = 0; catalog_ = {};
        for (auto& slot : slots_) slot = Slot{};
        for (auto& retirement : retirements_) retirement = Retirement{};
        for (auto& row : generations_) for (auto& generation : row) generation = 0;
        regions_[0] = {}; regions_[1] = {}; haveRegion_[0] = haveRegion_[1] = false;
        sequence_ = 0; pendingCount_ = 0; statistics_ = {};
    }
    bool Begun() const { return begun_; }
    const coop_world::Identity& Identity() const { return identity_; }
    const Statistics& Stats() const { return statistics_; }
    bool HostCoversBothRegions() const {
        return authority_ && capabilities_.hostPeerFocus && capabilities_.hostPeerStreaming && capabilities_.hostPeerPalette;
    }
    std::size_t EntityCount() const { return begun_ ? world_.Count() : 0; }
    std::size_t ManagedCount() const {
        std::size_t count = 0;
        for (std::size_t i=0;i<catalog_.count;++i) {
            std::uintptr_t ride=0;std::uint32_t kind=0;
            if(ReadManaged(catalog_.rows[i].car,ride,kind)) ++count;
        }
        return authority_ ? 0 : count;
    }
    bool SetRegion(const coop_world::PlayerRegion& region, std::uint32_t now) {
        if (!begun_ || region.playerId < 1 || region.playerId > 2 ||
            !std::isfinite(region.x) || !std::isfinite(region.y) || !std::isfinite(region.z) ||
            std::fabs(region.x) > 1e7f || std::fabs(region.y) > 1e7f || std::fabs(region.z) > 1e7f ||
            !std::isfinite(region.radius) || region.radius <= 0 || region.radius > 2000) return false;
        const unsigned index = region.playerId - 1;
        if (haveRegion_[index] && coop_world::SerialDelta(now, regionAt_[index]) < 0) return false;
        regions_[index] = region; regionAt_[index] = now; haveRegion_[index] = true;
        return region.active ? world_.RefreshPlayer(region.playerId, now) : world_.Disconnect(region.playerId);
    }
    bool Observe(const Catalog& catalog, std::uint32_t now) {
        if (!begun_ || !CatalogValid(catalog)) {
            ++statistics_.catalogRefused;
            // A transient native read must not return an already reserved
            // pool to local AI. The port revalidates the exact live manifest
            // on every query, including while the next catalog is pending.
            if (!authority_) for (auto& slot : slots_) slot.matched = false;
            return false;
        }
        catalog_ = catalog;
        if (!authority_) { MatchClient(); return true; }
        // Prune departures BEFORE additions: a full 22-slot pool may replace
        // its roster in one observation and must not need a second frame to
        // free the old slots. Pending reads of an existing pool are not removals.
        for (auto& slot : slots_) {
            if (!slot.present) continue;
            const NativeRow* current = nullptr;
            for (std::size_t i = 0; i < catalog.count; ++i) if (EntityId(catalog.rows[i].nativeKind, catalog.rows[i].nativeId) == slot.latest.key.id)
                current = &catalog.rows[i];
            if (!current || !Allowed(current->nativeKind)) Retire(slot, now);
        }
        bool seen[kLimit]{};
        for (std::size_t i = 0; i < catalog.count; ++i) {
            const auto& row = catalog.rows[i];
            Slot* slot = FindId(EntityId(row.nativeKind, row.nativeId));
            if (slot) seen[static_cast<std::size_t>(slot - slots_)] = true;
            if (row.readiness == Readiness::Pending) { ++statistics_.captureRefused; continue; }
            if (!Allowed(row.nativeKind)) {
                if (slot) Retire(*slot, now);
                continue;
            }
            if (!slot) {
                for (auto& candidate : slots_) if (!candidate.present) { slot = &candidate; break; }
                if (!slot) { ++statistics_.captureRefused; continue; }
            }
            seen[static_cast<std::size_t>(slot - slots_)] = true;
            const bool replacement = slot->present && (slot->native.car != row.car || slot->native.parameter != row.parameter ||
                !(slot->latest.descriptor == row.descriptor));
            if (replacement) Retire(*slot, now);
            if (!slot->present) {
                const unsigned category = row.nativeKind == 3 ? 0 : 1;
                auto& generation = generations_[category][row.nativeId];
                ++generation; if (!generation) ++generation;
                slot->latest.key = {EntityId(row.nativeKind, row.nativeId), generation};
            }
            coop_world::Event event{}; event.identity = identity_; event.action = coop_world::Action::Spawn;
            event.sequence = NextSequence(); event.state.key = slot->latest.key;
            event.state.kind = WorldKind(row.nativeKind); event.state.descriptor = row.descriptor;
            event.state.motion = row.motion; Stamp(event.state.motion, event.sequence, now, row.descriptor);
            if(row.readiness==Readiness::Dormant) {
                event.state.motion.aiFrozen=1;event.state.motion.speedKmh=0;
                for(auto& v:event.state.motion.linearVelocity) v=0;
                for(auto& v:event.state.motion.angularVelocity) v=0;
                for(auto& v:event.state.motion.wheelSpeedRad) v=0;
            }
            if (world_.Apply(event, 1, now) != coop_world::ApplyResult::Applied) { ++statistics_.captureRefused; continue; }
            slot->present = slot->hasLatest = true; slot->native = row;
            slot->latest = *world_.Find(event.state.key); slot->lastReceive = now;
            // Repeated newer Spawns repair the old model/generation without
            // needing its old Remove; keep tombstones only for absent pools.
            for (auto& retirement : retirements_) if (retirement.used && retirement.event.state.key.id == slot->latest.key.id)
                retirement.used = false;
        }
        for (std::size_t i = 0; i < kLimit; ++i) if (slots_[i].present && !seen[i]) Retire(slots_[i], now);
        return true;
    }
    // Return true only for successfully consumed traffic/ambient messages.
    // RaceRacer frames belong to CoopAIWorldRuntime, not this registry.
    bool Receive(const void* data, std::size_t size, std::uint32_t peerPlayerId, std::uint32_t now) {
        if (!begun_ || !data || size < 4) return Reject();
        std::uint32_t magic = 0; std::memcpy(&magic, data, 4); magic = coop_world_wire::LE32(magic);
        if (magic == coop_world_wire::kLeaseMagic) return ReceiveLease(data, size, peerPlayerId, now);
        coop_world::Event event{}; std::uint32_t sender = 0, type = 0; std::uint8_t nativeId = 0;
        if (authority_ || peerPlayerId != 1 || !coop_world_wire::DecodeEvent(data, size, event, sender, 1) ||
            event.identity != identity_ || !DecodeId(event.state.key.id, type, nativeId) || !Allowed(type) ||
            event.state.kind != WorldKind(type) || event.state.raceMembershipId) return Reject();
        const auto applied = world_.Apply(event, sender, now);
        world_.RefreshPlayer(1, now);
        if (applied == coop_world::ApplyResult::Duplicate || applied == coop_world::ApplyResult::Stale) return true;
        if (applied != coop_world::ApplyResult::Applied) return Reject();
        Slot* slot = FindId(event.state.key.id);
        if (event.action == coop_world::Action::Remove) {
            if (slot) *slot = Slot{};
        } else {
            if (!slot) for (auto& candidate : slots_) if (!candidate.present) { slot = &candidate; break; }
            if (!slot) return Reject();
            if (!slot->present || slot->latest.key != event.state.key) *slot = Slot{};
            slot->present = slot->hasLatest = true; slot->latest = *world_.Find(event.state.key); slot->lastReceive = now;
        }
        ++statistics_.received; MatchClient(); return true;
    }
    bool ReadManaged(std::uintptr_t car, std::uintptr_t& ride, std::uint32_t& kind) const {
        if (!begun_ || authority_ || !car) return false;
        for (std::size_t i=0;i<catalog_.count;++i) {
            const auto& row=catalog_.rows[i];
            if(row.car!=car || !Allowed(row.nativeKind) || !NativeEnabled(row.nativeKind)) continue;
            if(port_.reserved ? !port_.reserved(port_.context,row) :
                (!port_.matches || !port_.matches(port_.context,row))) continue;
            ride=row.ride;kind=row.nativeKind;return true;
        }
        return false;
    }
    bool NativeKey(std::uintptr_t car,coop_world::EntityKey& key) const {
        if(!begun_ || !car) return false;
        for(const auto& slot:slots_) if(slot.present && slot.native.car==car && slot.hasLatest) {key=slot.latest.key;return true;}
        return false;
    }
    bool NativeState(std::uintptr_t car,coop_world::EntityState& state,std::uint32_t& receivedAt) const {
        if (!begun_ || !car) return false;
        for (const auto& slot:slots_) if(slot.present && slot.native.car==car && slot.hasLatest &&
            (authority_ || slot.matched)) { state=slot.latest; receivedAt=slot.lastReceive; return true; }
        return false;
    }
    void Tick(std::uint32_t now) {
        if (!begun_) return;
        world_.RefreshPlayer(authority_ ? 1 : 2, now);
        world_.Expire(now);
        if (authority_ || !port_.apply) return;
        // Reserved pools without a usable first pose must remain hidden; a
        // missing packet must never hand the client back to vanilla generation.
        for(std::size_t i=0;i<catalog_.count;++i) {
            const auto& row=catalog_.rows[i];std::uintptr_t ride=0;std::uint32_t kind=0;
            if(!ReadManaged(row.car,ride,kind)) continue;
            Slot* slot=FindId(EntityId(row.nativeKind,row.nativeId));
            if((!slot || !slot->matched || !slot->hasLatest) && port_.visible) port_.visible(port_.context,row,false);
        }
        for (auto& slot : slots_) {
            std::uintptr_t ride = 0; std::uint32_t kind = 0;
            if (!slot.present || !slot.matched || !ReadManaged(slot.native.car, ride, kind) ||
                !port_.matches || !port_.matches(port_.context,slot.native)) continue;
            coop_world::EntityState rendered{};
            if (!world_.Sample(slot.latest.key, now, rendered) || !coop_world::ValidMotion(rendered.motion)) continue;
            // CoopMotion already stops extrapolation at 100ms. Never return
            // to independent client AI simply because the host is in a menu.
            if (coop_world::SerialDelta(now, slot.lastReceive) > 400) {
                // Wheel spin helpers must not keep integrating a cached speed
                // after the visual timeline is certainly beyond its cap.
                for (auto& speed : rendered.motion.wheelSpeedRad) speed = 0;
                rendered.motion.speedKmh = 0;
            }
            if (port_.apply(port_.context, slot.native, rendered.motion)) {
                ++statistics_.applied;
                if(port_.visible) port_.visible(port_.context,slot.native,!rendered.motion.aiFrozen);
            }
            else ++statistics_.applyRefused;
        }
    }
    // Caller sends each returned element as ONE datagram. Repeat Spawns repair
    // lost first packets; bounded tombstones repeat until a newer generation
    // or explicit scope reset, so a long loss cannot permanently lose Remove.
    std::size_t Outgoing(Datagram* output, std::size_t capacity, std::uint32_t now) {
        if (!begun_ || !output || !capacity) return 0;
        std::size_t count = 0;
        for (std::size_t i = 0; i < pendingCount_ && count < capacity; ++i) output[count++] = pending_[i];
        if (count < pendingCount_) {
            std::move(pending_.begin() + count, pending_.begin() + pendingCount_, pending_.begin()); pendingCount_ -= count;
            return count;
        }
        pendingCount_ = 0;
        if (!authority_) return count;
        for (auto& retirement : retirements_) {
            if (!retirement.used) continue;
            if (count < capacity && Encode(retirement.event, output[count])) ++count;
        }
        coop_world::PlayerRegion interested[2] = {regions_[0], regions_[1]};
        for (unsigned i = 0; i < 2; ++i) if (!haveRegion_[i] ||
            coop_world::SerialDelta(now, regionAt_[i]) > static_cast<std::int32_t>(coop_world::kPlayerTimeoutMs)) interested[i].active = false;
        coop_world::EntityKey keys[kLimit]{}; const auto selection = world_.SelectInterest(interested, keys, kLimit);
        for (std::size_t i = 0; i < selection.count && count < capacity; ++i) {
            const auto* state = world_.Find(keys[i]); if (!state) continue;
            coop_world::Event event{}; event.identity = identity_; event.action = coop_world::Action::Spawn;
            event.sequence = state->motion.sequence; event.state = *state;
            if (Encode(event, output[count])) ++count;
        }
        // Dormancy is a reliable reservation state, not an interest-based
        // removal. Repeat it even outside both current regions after loss.
        for(const auto& slot:slots_) {
            if(!slot.present || !slot.latest.motion.aiFrozen || count==capacity) continue;
            bool selected=false;for(std::size_t i=0;i<selection.count;++i) if(keys[i]==slot.latest.key) selected=true;
            if(selected) continue;
            coop_world::Event event{};event.identity=identity_;event.action=coop_world::Action::Spawn;
            event.sequence=slot.latest.motion.sequence;event.state=slot.latest;
            if(Encode(event,output[count])) ++count;
        }
        for (const auto& slot : slots_) {
            if (!slot.present || slot.latest.kind != coop_world::Kind::AmbientRacer || count == capacity) continue;
            coop_world::LeaseMessage state{}; if (!world_.GetLeaseMessage(slot.latest.key, now, state)) continue;
            coop_world_wire::LeaseEnvelope response{}; response.senderPlayerId = response.hostPlayerId = 1; response.state = state;
            if (Encode(response, output[count])) ++count;
        }
        return count;
    }
    bool RequestClaim(coop_world::EntityKey key, std::uint64_t requestId, std::uint32_t now, std::uint32_t durationMs = 1500) {
        return RequestLease(key, requestId, requestId, now, durationMs, false);
    }
    bool RequestRelease(coop_world::EntityKey key, std::uint64_t claimId, std::uint64_t releaseId, std::uint32_t now) {
        return RequestLease(key, claimId, releaseId, now, 0, true);
    }
    bool RequestRenew(coop_world::EntityKey key, std::uint64_t claimId, std::uint64_t renewalId, std::uint32_t now,
        std::uint32_t duration = 5000) {
        if (!begun_ || !capabilities_.ambientEngagement || !world_.Find(key)) return false;
        coop_world_wire::LeaseEnvelope envelope{}; envelope.kind = coop_world_wire::LeaseMessageKind::RenewRequest;
        envelope.senderPlayerId = authority_ ? 1 : 2; envelope.hostPlayerId = 1; envelope.eventId = renewalId;
        envelope.state.identity = identity_; envelope.state.key = key; envelope.state.requestId = claimId;
        envelope.state.ownerPlayerId = envelope.senderPlayerId; envelope.state.durationMs = duration;
        if (!coop_world_wire::LeaseValid(envelope)) return false;
        world_.RefreshPlayer(envelope.senderPlayerId, now);
        return authority_ ? HandleLease(envelope, now) : Queue(envelope);
    }
    bool EngagementApproved(coop_world::EntityKey key, std::uint32_t playerId, std::uint32_t now) const {
        coop_world::Lease lease{};
        return begun_ && capabilities_.ambientEngagement && world_.GetLease(key, now, lease) && lease.ownerPlayerId == playerId;
    }
    bool GetLease(coop_world::EntityKey key, std::uint32_t now, coop_world::Lease& lease) const {
        return begun_ && world_.GetLease(key, now, lease);
    }
    // Called only by the game-thread native engagement owner, not from UDP.
    // Native win/cancel may happen before the remote player's next update;
    // publish that end without impersonating a client ReleaseRequest.
    bool FinishAuthoritativeEngagement(coop_world::EntityKey key, std::uint32_t playerId, std::uint32_t now) {
        coop_world::Lease lease{};
        if (!begun_ || !authority_ || !capabilities_.ambientEngagement ||
            !world_.GetLease(key, now, lease) || lease.ownerPlayerId != playerId ||
            !world_.Release(key, playerId, lease.requestId, now)) return false;
        coop_world_wire::LeaseEnvelope response{};
        response.senderPlayerId=response.hostPlayerId=1;
        response.eventId=lease.requestId;
        response.decision=coop_world_wire::LeaseDecision::Released;
        if (!world_.GetLeaseMessage(key,now,response.state)) return false;
        return Queue(response);
    }
private:
    using WorldState = coop_world::World<kLimit>;
    struct Slot {
        bool present = false, hasLatest = false, matched = false;
        NativeRow native{}; coop_world::EntityState latest{};
        std::uint32_t lastReceive = 0;
    };
    struct Retirement { bool used = false; std::uint32_t at = 0; coop_world::Event event{}; };
    WorldState world_;
    coop_world::Identity identity_{};
    Slot slots_[kLimit]{}; Retirement retirements_[kRetireLimit]{};
    std::uint32_t generations_[2][256]{};
    Catalog catalog_{}; Port port_{}; Capabilities capabilities_{}; Statistics statistics_{};
    coop_world::PlayerRegion regions_[2]{};
    std::uint32_t regionAt_[2]{}, sequence_ = 0;
    bool haveRegion_[2]{}, begun_ = false, authority_ = false, trafficAllowed_ = true;
    ScopeKind scope_ = ScopeKind::FreeRoam;
    std::uint64_t localGeneration_ = 0;
    std::array<Datagram, kLimit * 2> pending_{}; std::size_t pendingCount_ = 0;
    std::uint32_t NextSequence() { ++sequence_; if (!sequence_) ++sequence_; return sequence_; }
    bool Reject() { ++statistics_.rejected; return false; }
    bool Allowed(std::uint32_t kind) const {
        return kind == 3 ? trafficAllowed_ : kind == 2 && scope_ == ScopeKind::FreeRoam;
    }
    bool NativeEnabled(std::uint32_t kind) const {
        return kind == 3 ? capabilities_.trafficMover && capabilities_.trafficPlacement && capabilities_.trafficTransitions :
            kind == 2 && capabilities_.ambientDriver && capabilities_.ambientTransitions;
    }
    Slot* FindId(std::uint64_t id) {
        for (auto& slot : slots_) if (slot.present && slot.latest.key.id == id) return &slot;
        return nullptr;
    }
    bool CatalogValid(const Catalog& catalog) const {
        if (!catalog.stable || catalog.count > kLimit || catalog.localGeneration != localGeneration_ || catalog.track != identity_.trackId) return false;
        for (std::size_t i = 0; i < catalog.count; ++i) {
            const auto& row = catalog.rows[i];
            if (!NativeIdValid(row.nativeKind, row.nativeId) ||
                (row.readiness != Readiness::Ready && row.readiness != Readiness::Dormant && row.readiness != Readiness::Pending)) return false;
            for (std::size_t j = 0; j < i; ++j) {
                const auto& prior = catalog.rows[j];
                if ((prior.nativeKind == row.nativeKind && prior.nativeId == row.nativeId) ||
                    (row.car && prior.car == row.car) || (row.parameter && prior.parameter == row.parameter)) return false;
            }
            if (row.readiness != Readiness::Pending && (!row.car || !row.parameter || row.parameter + 0x10 < row.parameter || row.ride != row.parameter + 0x10 ||
                (row.nativeKind == 3 ? row.mode != 0 && row.mode != 2 && row.mode != 3 && row.mode != 4 : row.mode != 0 && row.mode != 1 && row.mode != 3 && row.mode != 4) ||
                !row.mover || !row.witness || !coop_world::ValidDescriptor(row.descriptor) ||
                !row.descriptor.appearanceHash || !coop_world::ValidMotion(row.motion))) return false;
        }
        return true;
    }
    void Stamp(coop_world::Motion& motion, std::uint32_t sequence, std::uint32_t now, const coop_world::Descriptor& descriptor) const {
        motion.sequence = sequence; motion.sentAtMs = now; motion.flowState = 6;
        motion.sessionId = identity_.sessionId; motion.trackId = identity_.trackId; motion.worldEpoch = identity_.worldEpoch;
        std::memcpy(motion.carModel, descriptor.carModel, 16);
    }
    void Retire(Slot& slot, std::uint32_t now) {
        if (!slot.present) return;
        coop_world::Event event{}; event.identity = identity_; event.sequence = NextSequence(); event.action = coop_world::Action::Remove;
        event.state = slot.latest; Stamp(event.state.motion, event.sequence, now, event.state.descriptor);
        if (world_.Apply(event, 1, now) != coop_world::ApplyResult::Applied) return;
        Retirement* record = nullptr;
        for (auto& candidate : retirements_) if (candidate.used && candidate.event.state.key.id == event.state.key.id) { record = &candidate; break; }
        if (!record) for (auto& candidate : retirements_) if (!candidate.used) { record = &candidate; break; }
        if (!record) {
            record = &retirements_[0];
            for (auto& candidate : retirements_) if (coop_world::SerialDelta(candidate.at, record->at) < 0) record = &candidate;
        }
        *record = {true, now, event}; slot = Slot{};
    }
    void MatchClient() {
        if (authority_) return;
        statistics_.pendingModels = 0;
        for (auto& slot : slots_) {
            slot.matched = false;
            if (!slot.present) continue;
            for (std::size_t i = 0; i < catalog_.count; ++i) {
                const auto& row = catalog_.rows[i];
                if (row.readiness != Readiness::Pending && EntityId(row.nativeKind, row.nativeId) == slot.latest.key.id &&
                    row.descriptor == slot.latest.descriptor) { slot.native = row; slot.matched = true; break; }
            }
            if (!slot.matched) ++statistics_.pendingModels;
        }
    }
    static bool Encode(const coop_world::Event& event, Datagram& output) {
        coop_world_wire::EventPacket packet{};
        if (!coop_world_wire::EncodeEvent(event, 1, packet)) return false;
        output.size = static_cast<std::uint16_t>(sizeof(packet)); std::memcpy(output.bytes, &packet, sizeof(packet)); return true;
    }
    static bool Encode(const coop_world_wire::LeaseEnvelope& envelope, Datagram& output) {
        coop_world_wire::LeasePacket packet{};
        if (!coop_world_wire::EncodeLease(envelope, packet)) return false;
        output.size = static_cast<std::uint16_t>(sizeof(packet)); std::memcpy(output.bytes, &packet, sizeof(packet)); return true;
    }
    bool Queue(const coop_world_wire::LeaseEnvelope& envelope) {
        if (pendingCount_ == pending_.size() || !Encode(envelope, pending_[pendingCount_])) return false;
        ++pendingCount_; return true;
    }
    bool RequestLease(coop_world::EntityKey key, std::uint64_t claimId, std::uint64_t eventId, std::uint32_t now,
        std::uint32_t duration, bool release) {
        if (!begun_ || !capabilities_.ambientEngagement || !world_.Find(key) || world_.Find(key)->kind != coop_world::Kind::AmbientRacer) return false;
        coop_world_wire::LeaseEnvelope envelope{};
        envelope.kind = release ? coop_world_wire::LeaseMessageKind::ReleaseRequest : coop_world_wire::LeaseMessageKind::ClaimRequest;
        envelope.senderPlayerId = authority_ ? 1 : 2; envelope.hostPlayerId = 1; envelope.eventId = eventId;
        envelope.state.identity = identity_; envelope.state.key = key; envelope.state.requestId = claimId;
        envelope.state.ownerPlayerId = envelope.senderPlayerId; envelope.state.durationMs = duration;
        if (!coop_world_wire::LeaseValid(envelope)) return false;
        world_.RefreshPlayer(envelope.senderPlayerId, now);
        return authority_ ? HandleLease(envelope, now) : Queue(envelope);
    }
    bool ReceiveLease(const void* data, std::size_t size, std::uint32_t source, std::uint32_t now) {
        coop_world_wire::LeaseEnvelope envelope{};
        if (!coop_world_wire::DecodeLease(data, size, envelope, 1) || envelope.state.identity != identity_ ||
            envelope.senderPlayerId != source || source != (authority_ ? 2u : 1u)) return Reject();
        if (authority_) {
            if (envelope.kind == coop_world_wire::LeaseMessageKind::AuthorityState) return Reject();
            // A verified current-session packet refreshes presence, not an
            // unrelated process-wide clock or unauthenticated sender field.
            world_.RefreshPlayer(source, now); return HandleLease(envelope, now);
        }
        if (envelope.kind != coop_world_wire::LeaseMessageKind::AuthorityState) return Reject();
        world_.RefreshPlayer(1, now);
        const auto result = world_.ApplyLease(envelope.state, 1, now);
        return result == coop_world::ApplyResult::Applied || result == coop_world::ApplyResult::Duplicate || result == coop_world::ApplyResult::Stale;
    }
    bool HandleLease(const coop_world_wire::LeaseEnvelope& request, std::uint32_t now) {
        using namespace coop_world_wire;
        if (!authority_ || pendingCount_ == pending_.size()) return false;
        LeaseEnvelope response{}; response.senderPlayerId = response.hostPlayerId = 1;
        response.eventId = request.eventId; response.state.identity = identity_; response.state.key = request.state.key;
        response.decision = LeaseDecision::Rejected;
        if (capabilities_.ambientEngagement) {
            if (request.kind == LeaseMessageKind::ClaimRequest) {
                // Vanilla OutRun scoring has ONE process-wide engagement.
                // Serialize different ambient cars too, not only one key.
                bool busy=false;coop_world::Lease existing{};
                for(const auto& slot:slots_) if(slot.present && slot.latest.key!=request.state.key &&
                    world_.GetLease(slot.latest.key,now,existing)) busy=true;
                const auto result = busy ? coop_world::ClaimResult::Busy :
                    world_.TryClaim(request.state.key, request.senderPlayerId, request.state.requestId, now, request.state.durationMs);
                if (result == coop_world::ClaimResult::Acquired || result == coop_world::ClaimResult::AlreadyOwned) response.decision = LeaseDecision::Granted;
                else if (result == coop_world::ClaimResult::Busy) response.decision = LeaseDecision::Busy;
            } else if (request.kind == LeaseMessageKind::RenewRequest) {
                if (world_.RenewClaim(request.state.key, request.senderPlayerId, request.state.requestId, now, request.state.durationMs))
                    response.decision = LeaseDecision::Granted;
            } else if (world_.Release(request.state.key, request.senderPlayerId, request.state.requestId, now)) response.decision = LeaseDecision::Released;
        }
        world_.GetLeaseMessage(request.state.key, now, response.state);
        return Queue(response);
    }
};

// Read-only native catalog port. Stage7's 420030 completes palette RideInfo
// and 601110 rebuild; capture must run AFTER that call, on the game thread.
// The generation token comes from that typed observer, not pointer polling.
struct NativeAddresses {
    std::uintptr_t parameterArray = 0x0089E810, parameterCount = 0x0089E814;
    std::uintptr_t carSlots = 0x0089CCF8, track = 0x0089E7A0, modelTable = 0x008A1CCC;
};
struct NativeApi {
    bool (*read)(std::uintptr_t, void*, std::size_t) = nullptr;
    bool (*validPointer)(std::uintptr_t) = nullptr;
    bool (*validModel)(std::uint32_t) = nullptr;
    bool (*appearance)(std::uintptr_t, std::uint8_t*) = nullptr;
    std::uint64_t (*fingerprint)(const std::uint8_t*) = nullptr;
    // Optional verified portable handling identity. Zero means unknown, NOT
    // a serialized process-local tuning pointer or an invented stock hash.
    std::uint64_t (*handlingHash)(std::uintptr_t) = nullptr;
    bool (*physical)(std::uintptr_t, float*, float*, float*, float*) = nullptr;
};
inline std::uint32_t ModelNameHash(const char* name) {
    std::uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < 16 && name[i]; ++i) { hash ^= static_cast<std::uint8_t>(name[i]); hash *= 16777619u; }
    return hash ? hash : 1;
}
inline bool CaptureCatalog(const NativeApi& api, const NativeAddresses& addresses,
    std::uint64_t localGeneration, bool freeRoam, Catalog& output) {
    output = Catalog{}; output.localGeneration = localGeneration;
    if (!localGeneration || !api.read || !api.validPointer || !api.validModel || !api.appearance || !api.fingerprint || !api.physical) return false;
    std::uintptr_t array = 0, table = 0, cars[kLimit]{}; std::uint32_t count = 0;
    if (!api.read(addresses.parameterArray, &array, sizeof(array)) || !api.read(addresses.parameterCount, &count, 4) ||
        !api.read(addresses.track, &output.track, 4) || !count || count > kLimit || !api.validPointer(array) ||
        array + count * 0x940 < array || !api.validPointer(array + count * 0x940 - 1) ||
        !api.read(addresses.carSlots, cars, sizeof(cars)) || !api.read(addresses.modelTable, &table, sizeof(table)) || !api.validPointer(table)) return false;
    for (std::uint32_t ordinal = 0; ordinal < count; ++ordinal) {
        const std::uintptr_t parameter = array + ordinal * 0x940; std::uint32_t type = 0; std::uint8_t id = 0;
        if (!api.read(parameter + 4, &type, 4)) return false;
        if (type != 3 && !(freeRoam && type == 2)) continue;
        if (!api.read(parameter + 1, &id, 1) || !NativeIdValid(type, id)) return false;
        NativeRow& row = output.rows[output.count++]; row.parameter = parameter; row.ride = parameter + 0x10; row.nativeKind = type; row.nativeId = id;
        unsigned matches = 0;
        for (const auto car : cars) {
            std::uintptr_t owner = 0;
            if (car && api.validPointer(car) && api.read(car + 0x14, &owner, sizeof(owner)) && owner == parameter) { ++matches; row.car = car; }
        }
        if (matches != 1) { row.car = 0; continue; }
        std::uintptr_t ride = 0; std::uint8_t active = 0; std::uint32_t model = 0;
        if (!api.read(row.car + 0x10, &ride, sizeof(ride)) || ride != row.ride || !api.read(row.ride, &model, 4) ||
            !api.validModel(model) || !api.read(row.car + 0x28, &row.mode, 4) || !api.read(row.car + 0x550, &active, 1) ||
            !api.read(row.car + 0x34, &row.mover, sizeof(row.mover)) || !api.validPointer(row.mover) ||
            !api.read(row.car + 0x3C, &row.witness, sizeof(row.witness)) || !api.validPointer(row.witness)) continue;
        // Capture descriptor AND pose for dormant pools too. A live renderer
        // flag, not a guessed interpretation of mode zero, selects visibility.
        if (active > 1 || (type == 3 && row.mode != 0 && row.mode != 2 && row.mode != 3 && row.mode != 4) ||
            (type == 2 && row.mode != 0 && row.mode != 1 && row.mode != 3 && row.mode != 4)) continue;
        std::uintptr_t moverVtable = 0, witnessVtable = 0, witnessCar = 0, geometry = 0, embeddedGeometry = 0, terrain = 0;
        if (!api.read(row.mover, &moverVtable, sizeof(moverVtable)) ||
            moverVtable != (row.mode == 0 ? 0x007A5BB0u : row.mode == 2 ? 0x0078456Cu :
                row.mode == 1 ? 0x007A25E0u : row.mode == 3 ? 0x007A2538u : 0x007A2144u) ||
            !api.read(row.witness, &witnessVtable, sizeof(witnessVtable)) || witnessVtable != 0x007A2470u ||
            !api.read(row.witness + 0x130, &witnessCar, sizeof(witnessCar)) || witnessCar != row.car ||
            !api.read(row.car + 0x20, &geometry, sizeof(geometry)) || geometry != row.ride + 0x40 ||
            !api.read(row.car + 0x40, &embeddedGeometry, sizeof(embeddedGeometry)) || embeddedGeometry != geometry ||
            !api.read(row.car + 0x44, &terrain, sizeof(terrain)) || terrain != row.car + 0x5C4) continue;
        if (row.mode == 2 || row.mode == 0 || row.mode == 4) {
            std::uintptr_t owner = 0;
            if (!api.read(row.mover + 8, &owner, sizeof(owner)) || owner != (row.mode == 0 ? row.car + 0x40 : row.car)) continue;
        }
        std::uint8_t portable[0x940]{};
        if (!api.read(table + model * 0x890, row.descriptor.carModel, 16) || !std::memchr(row.descriptor.carModel, 0, 16) ||
            !api.appearance(row.ride, portable)) continue;
        row.descriptor.modelId = model; row.descriptor.modelHash = ModelNameHash(row.descriptor.carModel);
        row.descriptor.appearanceHash = api.fingerprint(portable);
        if (api.handlingHash) row.descriptor.handlingHash = api.handlingHash(row.ride);
        if (!coop_world::ValidDescriptor(row.descriptor) || !row.descriptor.appearanceHash) continue;
        float position[3]{}, matrix[16]{};
        auto& motion = row.motion;
        if (!api.physical(row.car, position, motion.orientation, motion.linearVelocity, motion.angularVelocity) ||
            !coop_motion::QuaternionToMatrix(motion.orientation, matrix)) continue;
        motion.x = position[0]; motion.y = position[1]; motion.z = position[2];
        motion.forwardX = matrix[0]; motion.forwardY = matrix[1]; motion.forwardZ = matrix[2];
        motion.headingDeg = std::atan2(matrix[1], matrix[0]) * 57.295779513f;
        if (motion.headingDeg < 0) motion.headingDeg += 360;
        const auto* velocity = motion.linearVelocity;
        motion.speedKmh = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]) * 3.6f;
        if (!api.read(row.car + 0x40E, &motion.steerAngle, 2) || !api.read(row.car + 0x4E8, motion.wheelPhaseRad, sizeof(motion.wheelPhaseRad)) ||
            !api.read(row.car + 0x4F8, motion.wheelSpeedRad, sizeof(motion.wheelSpeedRad))) continue;
        for (unsigned wheel = 0; wheel < 4; ++wheel) {
            float rotation[16]{};
            if (api.read(row.car + 0x130 + wheel * 0x40, rotation, sizeof(rotation)) && coop_motion::IsRotation(rotation)) {
                for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c) motion.wheelRotation[wheel][r * 3 + c] = rotation[r * 4 + c];
                motion.wheelRotationValidMask |= 1u << wheel;
            }
        }
        // Brake/reverse byte locations have not been established for these
        // pooled types. Default off is explicit; never borrow a human layout.
        if (coop_world::ValidMotion(motion)) row.readiness = active ? Readiness::Ready : Readiness::Dormant;
    }
    std::uintptr_t finalArray = 0; std::uint32_t finalCount = 0, finalTrack = 0;
    if (!api.read(addresses.parameterArray, &finalArray, sizeof(finalArray)) || !api.read(addresses.parameterCount, &finalCount, 4) ||
        !api.read(addresses.track, &finalTrack, 4) || finalArray != array || finalCount != count || finalTrack != output.track) return false;
    output.stable = true; return true;
}
} // namespace CoopTrafficRuntime
