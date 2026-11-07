// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Native/UI-thread authority adapter. The UDP thread only decodes values and
// queues packets; native identities, assets and vehicle writes stay here.
#include "CoopFleetEpoch.h"
#include "CoopFleetNetworkOutbox.h"
namespace CoopFleetWorldRuntime {
static coop_fleet_load::State loading;
static CoopTrafficRuntime::Runtime runtime;
static CoopTrafficRuntime::Capabilities capabilities{};
static bool installed = false, ready = false;
struct Retained {uintptr_t car=0,parameter=0,ride=0;uint32_t kind=0,model=0;uint8_t id=0;};
static std::array<Retained,CoopTrafficRuntime::kLimit> retained{};
static uint32_t retainedTrack=0;static bool retainedLost=false;
static void ClearRetained(){retained={};retainedTrack=0;retainedLost=false;}
static bool RetainedClient(uintptr_t car,uintptr_t& ride,uint32_t& kind) {
    if(!car || !retainedTrack || (!retainedLost && CoopSnapshot().joined))return false;
    uint32_t flow=0,track=0,type=0,model=0;uintptr_t parameter=0,actualRide=0;uint8_t id=0,remote=0;
    if(!ReadGameMemory(kGameFlowState,&flow,4) || flow!=6 ||
        !ReadGameMemory(gRaceContextAddresses.track,&track,4) || track!=retainedTrack)return false;
    for(const auto& row:retained)if(row.car==car) {
        if(!ReadGameMemory(car+0x14,&parameter,4) || parameter!=row.parameter ||
            !ReadGameMemory(car+0x10,&actualRide,4) || actualRide!=row.ride ||
            !ReadGameMemory(parameter+4,&type,4) || type!=row.kind ||
            !ReadGameMemory(parameter+1,&id,1) || id!=row.id ||
            !ReadGameMemory(parameter+0x900,&remote,1) || remote!=1 ||
            !ReadGameMemory(actualRide,&model,4) || model!=row.model)return false;
        ride=actualRide;kind=type;return true;
    }
    return false;
}
// A completed COOP FreeRoam survives local garage/track-pool reloads, not a
// departure into SP/profile/frontend. Native readiness is false during those
// reloads, so it is not itself a persistent-world marker.
static coop_world::Identity establishedFreeRoam{};
static uint64_t establishedGeneration=0;
static uintptr_t establishedOwnCar=0;
static bool retainedWithoutPeer=false;
static uint32_t localFlow = 0, peerFlow = 0, peerPresenceAt = 0, focusCounter = 0;
static bool havePeerPresence = false;
static coop_world::Identity peerPresenceIdentity{};
static bool peerPresenceFreeRoam = false;
static uint32_t presenceSequence = 0, peerPresenceSequence = 0;
static coop::Endpoint peer{};
struct Received {
    CoopTrafficRuntime::Datagram datagram{};
    coop::Endpoint endpoint{}; uint32_t receivedAt = 0;
};
static SRWLOCK lock = SRWLOCK_INIT;
static std::array<Received, 256> inbox{};
static size_t inboxCount = 0;
static std::array<CoopTrafficRuntime::Datagram, CoopTrafficRuntime::kDatagramLimit> outgoing{};
static size_t outgoingCount = 0;
static uint32_t outgoingAt = 0, publication = 0, lastSent = 0;
static uint64_t outgoingSession = 0;
static coop::Endpoint outgoingPeer{};
static coop_fleet_net::Outbox networkOutbox;
static std::atomic<uint32_t> queueDropped{0};
struct LocalChallenge {
    uintptr_t car = 0, human = 0;
    coop_world::EntityKey key{};
    uint64_t claim = 0; uint32_t lastRequest = 0;
    bool entered = false;
};
static LocalChallenge challenge{};
static uint64_t claimSerial = 0;
static coop_world::EntityKey followed{};
static uintptr_t followedCar=0;
static uint32_t Now() { return CoopAIWorldRuntime::MotionNow(); }
static bool Session(const coop::SessionSnapshot& session) {
    // Session presence survives frontend screens; native work does not.
    return gCoopFleetEnabled && session.joined && session.peer.Valid() &&
        session.sessionId && (session.role == coop::Role::Host || session.role == coop::Role::Client);
}
static bool EstablishedCampaign(const coop::SessionSnapshot& session) {
    return establishedFreeRoam.Valid()&&session.sessionId==establishedFreeRoam.sessionId&&
        coop_fleet_load::SameCampaign(session.campaignName,loading.Campaign());
}
static bool ProvenLocalWorld(uint32_t flow) {
    uint32_t track=0;
    return flow==6&&IsCoopGameplayActive()&&ready&&loading.FreeRoam()&&
        establishedFreeRoam==loading.Identity()&&runtime.Begun()&&runtime.Identity()==establishedFreeRoam&&
        establishedGeneration&&establishedGeneration==loading.Generation()&&
        establishedOwnCar&&establishedOwnCar==CoopRaceCar(false)&&IsValidUserPtr(establishedOwnCar)&&
        ReadGameMemory(gRaceContextAddresses.track,&track,4)&&track==establishedFreeRoam.trackId;
}
static void FreezeTransport() {
    AcquireSRWLockExclusive(&lock);inboxCount=outgoingCount=0;outgoingSession=0;
    ++publication;networkOutbox.Reset();ReleaseSRWLockExclusive(&lock);
}
static bool RefreshRetainedTransport(const coop::SessionSnapshot& session,uint32_t flow) {
    if(!session.localRetainsNative||!EstablishedCampaign(session)||!ProvenLocalWorld(flow))return false;
    const bool changed=!loading.Matches(session)||peer!=session.peer;
    if(!loading.RebindRetained(session,establishedGeneration))return false;
    if(changed) {
        FreezeTransport();peerPresenceIdentity={};peerPresenceFreeRoam=false;
        havePeerPresence=false;peerPresenceAt=peerPresenceSequence=0;peer=session.peer;
        challenge={};followed={};followedCar=0;
    }
    retainedWithoutPeer=false;return true;
}
static void Reset() {
    loading.Reset(); runtime.Reset(); ready = false; peer = {}; challenge = {}; followed = {}; followedCar=0;
    establishedFreeRoam = {}; peerPresenceIdentity = {}; peerPresenceFreeRoam = false;
    establishedGeneration=0;establishedOwnCar=0;retainedWithoutPeer=false;
    havePeerPresence = false; peerPresenceAt = 0; peerPresenceSequence = 0;
    AcquireSRWLockExclusive(&lock); inboxCount = outgoingCount = 0; outgoingSession = 0; ++publication;networkOutbox.Reset();
    ReleaseSRWLockExclusive(&lock);
}
static bool NativeIdentity(uintptr_t car, uintptr_t& ride, uint32_t& kind,
    uint8_t* nativeId = nullptr, uint32_t* modelId = nullptr, uint64_t* hash = nullptr) {
    if (!installed || !loading.Ready() || !Session(CoopSnapshot()) || !IsCoopGameplayActive() || !IsValidUserPtr(car) ||
        car == CoopRaceCar(false) || car == CoopRaceCar(true)) return false;
    uintptr_t parameter = 0, actualRide = 0; uint32_t worldKind = 0, type = 0, model = 0;
    uint8_t id = 0; uint64_t appearance = 0;
    if (!ReadGameMemory(car + 0x14, &parameter, sizeof(parameter)) || !IsValidUserPtr(parameter) ||
        !ReadGameMemory(car + 0x10, &actualRide, sizeof(actualRide)) || actualRide != parameter + 0x10 ||
        !ReadGameMemory(parameter + 4, &type, 4) || (type != 3 && type != 2) ||
        !ReadCoopFleetParameter(parameter, worldKind, id, model, appearance) ||
        worldKind != static_cast<uint32_t>(CoopTrafficRuntime::WorldKind(type)) ||
        !CoopFleetReservedNativeId(loading.Identity(), worldKind, id) ||
        !CoopTrafficRuntime::NativeIdValid(type, id)) return false;
    ride = actualRide; kind = type;
    if (nativeId) *nativeId = id; if (modelId) *modelId = model; if (hash) *hash = appearance; return true;
}
static bool Reserved(void*, const CoopTrafficRuntime::NativeRow& row) {
    uintptr_t ride = 0; uint32_t kind = 0; uint8_t id = 0;
    if (CoopSnapshot().role==coop::Role::Client) {
        return ReadManagedCoopFleet(row.car,ride,kind) && ride==row.ride && kind==row.nativeKind &&
            ReadGameMemory(row.parameter+1,&id,1) && id==row.nativeId;
    }
    return NativeIdentity(row.car, ride, kind, &id) && ride == row.ride && kind == row.nativeKind && id == row.nativeId;
}
static bool Matches(void*, const CoopTrafficRuntime::NativeRow& row) {
    uintptr_t ride = 0, witness = 0, visual = 0, visualRide = 0;
    uint32_t kind = 0, model = 0; uint8_t id = 0; uint64_t hash = 0;
    // Never call ReadTarget here: it calls ReadManagedCoopAI and would recurse.
    return ready && CoopFleetAssetsReady(loading.Identity()) &&
        NativeIdentity(row.car, ride, kind, &id, &model, &hash) && ride == row.ride &&
        kind == row.nativeKind && id == row.nativeId && model == row.descriptor.modelId && hash == row.descriptor.appearanceHash &&
        ReadGameMemory(row.car + 0x3C, &witness, sizeof(witness)) && witness == row.witness &&
        ReadGameMemory(row.car + 0x554, &visual, sizeof(visual)) && IsValidUserPtr(visual) &&
        ReadGameMemory(visual + 4, &visualRide, sizeof(visualRide)) && visualRide == ride;
}
static bool Apply(void*, const CoopTrafficRuntime::NativeRow& row, const coop_world::Motion& incoming) {
    if (!Matches(nullptr, row) || CoopSnapshot().role != coop::Role::Client) return false;
    auto motion=incoming;
    ApplyCoopWorldImpactPrediction(row.car,motion);
    const float position[3]{motion.x, motion.y, motion.z};
    if (!ApplyCoopWorldPhysicalMotion(row.car, CoopRaceCar(false), position,
        motion.orientation, motion.linearVelocity, motion.angularVelocity)) return false;
    if (!SafeWriteBytes(row.car + 0x40E, &motion.steerAngle, 2) ||
        !SafeWriteBytes(row.car + 0x4E8, motion.wheelPhaseRad, sizeof(motion.wheelPhaseRad)) ||
        !SafeWriteBytes(row.car + 0x4F8, motion.wheelSpeedRad, sizeof(motion.wheelSpeedRad))) return false;
    for (unsigned wheel = 0; wheel < 4; ++wheel) if (motion.wheelRotationValidMask & (1u << wheel)) {
        if (!CoopAIWorldRuntime::ApplyWheelOrientation(row.car, wheel, motion.wheelRotation[wheel])) return false;
    }
    if (motion.wheels.validMask) ApplyCoopWorldWheelState(row.car,CoopRaceCar(false),motion.wheels);
    return true;
}
static bool Visible(void*, const CoopTrafficRuntime::NativeRow& row, bool visible) {
    return Reserved(nullptr, row) && SetCoopFleetCarVisible(row.car, CoopRaceCar(false), visible);
}
static bool HostFocus(uintptr_t car, uintptr_t& focus) {
    uintptr_t ride = 0; uint32_t kind = 0;
    if (CoopSnapshot().role != coop::Role::Host || !NativeIdentity(car, ride, kind) || kind != 3) return false;
    // Alternate actual placement attempts, so even a one-car traffic pool can
    // serve both people. Returning false preserves stock local-camera focus.
    if ((++focusCounter & 1u) == 0) return false;
    RemotePlayerState remote{};
    if (!GetRemotePlayerState(remote, false) || remote.current.sessionId != loading.Identity().sessionId ||
        remote.current.trackId != loading.Identity().trackId || remote.current.flowState != 6) return false;
    focus = CoopRaceCar(true); return IsValidUserPtr(focus) && focus != CoopRaceCar(false);
}
static bool Capture(CoopTrafficRuntime::Catalog& catalog) {
    const CoopTrafficRuntime::NativeApi api{[](uintptr_t address, void* output, size_t bytes) {
        return ReadGameMemory(address, output, static_cast<SIZE_T>(bytes)); }, &IsValidUserPtr, &IsSupportedCarModel,
        [](uintptr_t ride, uint8_t* output) { return output && ReadNativeAppearancePortable(ride,
            *reinterpret_cast<uint8_t (*)[0x940]>(output)); }, &ComputePeerAppearanceFingerprint, nullptr, &ReadCoopWorldPhysicalMotion};
    if (!CoopTrafficRuntime::CaptureCatalog(api, {}, loading.Generation(), loading.FreeRoam(), catalog)) return false;
    if (catalog.track != loading.Identity().trackId) return false;
    for (size_t i = 0; i < catalog.count; ++i) {
        auto& row = catalog.rows[i]; uint32_t kind = 0, model = 0; uint8_t id = 0; uint64_t hash = 0;
        if (CoopSnapshot().role==coop::Role::Host) ReadCoopWorldWheelState(row.car,row.motion.wheels);
        if (!ReadCoopFleetReservation(loading.Identity(),row.parameter,kind,id) ||
            kind != static_cast<uint32_t>(CoopTrafficRuntime::WorldKind(row.nativeKind)) || id != row.nativeId ||
            !CoopFleetReservedNativeId(loading.Identity(), kind, id)) return false;
        if (!ReadCoopFleetParameter(row.parameter,kind,id,model,hash) ||
            (row.readiness != CoopTrafficRuntime::Readiness::Pending &&
                (model != row.descriptor.modelId || hash != row.descriptor.appearanceHash))) {
            // One vehicle loading a model must not stall every other vehicle.
            // Its exact reservation remains owned; poses wait for local assets.
            row.readiness=CoopTrafficRuntime::Readiness::Pending;
        }
    }
    return true;
}
static void Regions(uint32_t now) {
    const auto session = CoopSnapshot(); const bool host = session.role == coop::Role::Host;
    uintptr_t humans[]{CoopRaceCar(false), CoopRaceCar(true)};
    RemotePlayerState remote{}; const bool peerActive = GetRemotePlayerState(remote, false) && remote.current.flowState == 6 &&
        remote.current.sessionId == loading.Identity().sessionId && remote.current.trackId == loading.Identity().trackId &&
        !(havePeerPresence && coop_world::SerialDelta(now, peerPresenceAt) <= 1500 && peerFlow == 3);
    for (unsigned i = 0; i < 2; ++i) {
        coop_world::PlayerRegion region{}; region.playerId = i == 0 ? (host ? 1 : 2) : (host ? 2 : 1); region.radius = 600;
        float position[3]{}; uint8_t active = 0;
        region.active = (i == 0 || peerActive) && IsValidUserPtr(humans[i]) &&
            ReadGameMemory(humans[i] + 0x550, &active, 1) && active == 1 &&
            ReadGameMemory(humans[i] + 0x60, position, sizeof(position));
        region.x = position[0]; region.y = position[1]; region.z = position[2]; runtime.SetRegion(region, now);
    }
}
static void Drain(const coop::SessionSnapshot& session) {
    // Keep this bounded stack well below the native game thread's stack size.
    std::array<Received, 32> batch{};
    for (unsigned pass = 0; pass < 8; ++pass) {
        size_t count = 0;
        AcquireSRWLockExclusive(&lock); count = std::min(batch.size(), inboxCount);
        std::copy_n(inbox.begin(), count, batch.begin());
        if (count < inboxCount) std::move(inbox.begin() + count, inbox.begin() + inboxCount, inbox.begin());
        inboxCount -= count; ReleaseSRWLockExclusive(&lock);
        if (!count) break;
        for (size_t i = 0; i < count; ++i) {
            const auto& packet = batch[i]; if (packet.endpoint != session.peer) continue;
            coop_fleet_load::Message message{};
            if (coop_fleet_load::Decode(packet.datagram.bytes, packet.datagram.size, message)) {
                if (message.identity.sessionId != session.sessionId ||message.ticket!=session.ticket||message.membership!=session.membershipNonce) continue;
                // Presence is independently ordered across native reloads.
                // Old same-session/menu packets cannot release a newer load.
                if (message.presenceSequence && (!havePeerPresence ||
                    coop_world::Newer(message.presenceSequence, peerPresenceSequence))) {
                    peerFlow = message.flowState; peerPresenceAt = packet.receivedAt;
                    peerPresenceSequence = message.presenceSequence; havePeerPresence = true;
                    peerPresenceIdentity = message.identity; peerPresenceFreeRoam = message.freeRoam;
                }
                // Do not replace a live native pool when a delayed/newer Host
                // offer arrives. A fresh local loading token is required.
                if (!ready || message.identity == loading.Identity() || session.role == coop::Role::Host) loading.Receive(message);
            } else if (ready) runtime.Receive(packet.datagram.bytes, packet.datagram.size,
                session.role == coop::Role::Host ? 2 : 1, packet.receivedAt);
        }
    }
}
static bool HoldRemoteOutrunGrant(const CoopTrafficRuntime::Datagram& datagram) {
    coop_world_wire::LeaseEnvelope lease{};
    return coop_world_wire::DecodeLease(datagram.bytes,datagram.size,lease,1) &&
        lease.kind==coop_world_wire::LeaseMessageKind::AuthorityState && lease.state.ownerPlayerId==2 &&
        (!followedCar || followed!=lease.state.key);
}
static void Publish(const coop::SessionSnapshot& session, uint32_t now) {
    std::array<CoopTrafficRuntime::Datagram, CoopTrafficRuntime::kDatagramLimit> frames{};
    size_t count = 0; coop_fleet_load::Message message{}; coop_fleet_load::Packet packet{};
    if (loading.Outgoing(message)) {
        message.flowState = localFlow;
        message.presenceSequence = ++presenceSequence; if (!message.presenceSequence) message.presenceSequence = ++presenceSequence;
        if (coop_fleet_load::Encode(message, packet)) {
            frames[count].size = sizeof(packet); std::memcpy(frames[count++].bytes, &packet, sizeof(packet));
        }
    }
    AcquireSRWLockExclusive(&lock);
    networkOutbox.Bind(session.sessionId,session.peer,loading.Identity());
    // Runtime drains urgent claim/release replies before snapshots. Reserve
    // FIFO capacity for every returned datagram so network backpressure never
    // discards one of those transactions at the source publication boundary.
    if (ready) count += runtime.Outgoing(frames.data() + count,
        std::min(frames.size()-count,networkOutbox.ControlSpace()), now);
    // A lease reservation is not proof that the native Host AI started.
    // Suppress both the first grant and periodic grant state until it has.
    if(session.role==coop::Role::Host)
        count=static_cast<size_t>(std::remove_if(frames.begin(),frames.begin()+count,&HoldRemoteOutrunGrant)-frames.begin());
    for(size_t i=0;i<count;++i)networkOutbox.Push(frames[i].bytes,frames[i].size,now);
    std::copy_n(frames.begin(), count, outgoing.begin()); outgoingCount = count;
    outgoingAt = now; outgoingSession = session.sessionId; outgoingPeer = session.peer; ++publication;
    ReleaseSRWLockExclusive(&lock);
}
}

FleetLoadResult BeginCoopFleetIdentityAndLoadingBarrier(uint32_t track, uint32_t raceEpoch,
    bool freeRoam, uint64_t localLoadToken, bool trafficAllowed, coop_world::Identity& identity) {
    using namespace CoopFleetWorldRuntime; const auto session = CoopSnapshot();
    if (!installed || !Session(session)) return FleetLoadResult::Refused;
    Drain(session);const auto now = Now();
    const auto before = loading.Identity(); const auto generation = loading.Generation();
    const bool established = IsCoopGameplayActive() && establishedFreeRoam==before && before.Valid() &&
        runtime.Begun() && runtime.Identity()==before;
    const bool retainedTicket=session.fresh&&session.careerLaunched&&EstablishedCampaign(session)&&
        (session.localRetainsNative||session.peerRetainsNative);
    // Both people may independently leave the garage at overlapping times.
    // A peer already streaming Stage5/6 has consumed this world's manifest;
    // rotating the wire epoch here would strand that native load. Flow3 is
    // allowed too: an ordered menu presence can precede its fresh load nonce.
    const bool retainLive = session.role==coop::Role::Host && session.fresh && established &&
        loading.Ready() && loading.FreeRoam() && freeRoam && !raceEpoch && before.trackId==track &&
        (retainedTicket || (session.initialBarrierComplete&&havePeerPresence && peer==session.peer && peerFlow>=3 && peerFlow<=6 && peerPresenceFreeRoam &&
        peerPresenceIdentity.sessionId==before.sessionId && peerPresenceIdentity.trackId==before.trackId &&
        !peerPresenceIdentity.raceEpoch &&
        // Native streaming may stall UI publication beyond 1500 ms while the
        // authenticated UDP session heartbeat remains fresh. Aging an ordered
        // matching presence must not rotate a world beneath its sealed peer.
        coop_world::SerialDelta(now,peerPresenceAt)>=0));
    const bool keepRuntime = established && (session.initialBarrierComplete||retainedTicket) &&
        loading.Role()==session.role && loading.FreeRoam() && freeRoam && !raceEpoch &&
        before.sessionId==session.sessionId && before.trackId==track && peer==session.peer;
    if (!loading.Begin(session, track, raceEpoch, freeRoam, localLoadToken, trafficAllowed, retainLive)) return FleetLoadResult::Refused;
    if (before != loading.Identity() || generation != loading.Generation()) {
        if(!keepRuntime || (loading.Ready() && runtime.Identity()!=loading.Identity())) runtime.Reset();
        if (before!=loading.Identity()) establishedFreeRoam = {};
        ready = false; challenge = {};followed={};followedCar=0;
    }
    peer = session.peer; Drain(session); Publish(session, now);
    if (!loading.Ready()) {
        static uint32_t lastWait = 0;
        if (coop_world::SerialDelta(now, lastWait) >= 5000) {
            lastWait = now; char line[160]{};
            std::snprintf(line, sizeof(line), "[FLEET_LOAD] Waiting for Host identity track=%u race=%u generation=%llu\r\n",
                track, raceEpoch, localLoadToken); LogMessage(line);
        }
        return FleetLoadResult::Waiting;
    }
    if (before != loading.Identity() || generation != loading.Generation()) {
        char line[180]{};
        std::snprintf(line, sizeof(line), "[FLEET_LOAD] Identity ready role=%s track=%u epoch=%u race=%u generation=%llu\r\n",
            session.role == coop::Role::Host ? "host" : "client", track,
            loading.Identity().worldEpoch, raceEpoch, localLoadToken); LogMessage(line);
    }
    identity = loading.Identity(); return FleetLoadResult::Ready;
}
bool CurrentCoopFleetIdentity(coop_world::Identity& identity) {
    using namespace CoopFleetWorldRuntime;
    if (!installed || !loading.Ready() || !Session(CoopSnapshot()) ||
        loading.Identity().sessionId != CoopSnapshot().sessionId ||
        (!loading.Matches(CoopSnapshot())&&!(CoopSnapshot().localRetainsNative&&EstablishedCampaign(CoopSnapshot())))) return false;
    identity = loading.Identity(); return true;
}
bool SealCoopFleetLoadingIdentity(uint64_t generation) {
    using namespace CoopFleetWorldRuntime; const auto session=CoopSnapshot();
    return installed && Session(session) && IsCoopGameplayActive() &&
        loading.Identity().sessionId==session.sessionId && peer==session.peer && loading.SealIdentity(generation);
}
uint64_t CoopFleetLocalGeneration() { return CoopFleetWorldRuntime::loading.Generation(); }
uint64_t CoopFleetWorldEpoch() {
    coop_world::Identity identity{}; return CurrentCoopFleetIdentity(identity) ? identity.sessionId ^ (uint64_t(identity.worldEpoch) << 32) ^ identity.raceEpoch : 0;
}
bool CoopFleetWaitForPeerAssets() {
    using namespace CoopFleetWorldRuntime; const auto session = CoopSnapshot();
    if (!loading.FreeRoam() || !session.initialBarrierComplete || !havePeerPresence || peer != session.peer ||
        coop_world::SerialDelta(Now(), peerPresenceAt) > 1500) return true;
    return peerFlow != 3;
}
bool NotifyCoopFleetNativeLoadReady(const coop_world::Identity& identity, uint64_t generation) {
    using namespace CoopFleetWorldRuntime;
    if (!installed || !loading.Ready() || loading.Identity() != identity || loading.Generation() != generation ||
        !CoopFleetAssetsReady(identity)) return false;
    const auto now=Now();
    ready = runtime.Begun() && runtime.Identity()==identity && loading.FreeRoam() ?
        runtime.RebindNative(identity,generation,now) :
        runtime.Begin(identity, loading.Role() == coop::Role::Host,
            loading.FreeRoam() ? CoopTrafficRuntime::ScopeKind::FreeRoam : CoopTrafficRuntime::ScopeKind::Race,
            generation, loading.TrafficAllowed(), now);
    if (ready) {
        establishedFreeRoam = loading.FreeRoam() ? identity : coop_world::Identity{};
        establishedGeneration=loading.FreeRoam()?generation:0;
        establishedOwnCar=loading.FreeRoam()?CoopRaceCar(false):0;
        char message[192]{}; std::snprintf(message, sizeof(message),
            "[FLEET_WORLD] Ready track=%u epoch=%u traffic=%u freeroam=%u generation=%llu\r\n",
            identity.trackId, identity.worldEpoch, loading.TrafficAllowed(), loading.FreeRoam(), generation); LogMessage(message);
    }
    return ready;
}
void ConfigureCoopFleetWorld(const CoopTrafficRuntime::Capabilities& hooks) {
    using namespace CoopFleetWorldRuntime; capabilities = hooks;
    const CoopTrafficRuntime::Port port{nullptr, &Matches, &Apply, &Reserved, &Visible}; runtime.Configure(port, hooks);
}
bool ReadCoopFleetIdentity(uintptr_t car, uintptr_t& ride, uint32_t& kind) {
    return CoopFleetWorldRuntime::NativeIdentity(car, ride, kind);
}
bool ReadManagedCoopFleet(uintptr_t car, uintptr_t& ride, uint32_t& kind) {
    using namespace CoopFleetWorldRuntime;
    // Reservation must survive pending Stage7 assets and missing first poses.
    const auto session=CoopSnapshot();
    if (!installed || !loading.Ready() || session.role!=coop::Role::Client || !Session(session) ||
        !IsCoopGameplayActive() || !IsValidUserPtr(car) || car==CoopRaceCar(false) || car==CoopRaceCar(true)) return false;
    uintptr_t parameter=0,actualRide=0;uint32_t worldKind=0,type=0;uint8_t id=0;
    if (!ReadGameMemory(car+0x14,&parameter,sizeof(parameter)) || !IsValidUserPtr(parameter) ||
        !ReadGameMemory(car+0x10,&actualRide,sizeof(actualRide)) || actualRide!=parameter+0x10 ||
        !ReadGameMemory(parameter+4,&type,4) || (type!=2 && type!=3) ||
        !ReadCoopFleetReservation(loading.Identity(),parameter,worldKind,id) ||
        worldKind!=static_cast<uint32_t>(CoopTrafficRuntime::WorldKind(type)) ||
        !CoopTrafficRuntime::NativeIdValid(type,id)) return false;
    ride=actualRide;kind=type;
    return kind == 3 ? capabilities.trafficMover && capabilities.trafficPlacement && capabilities.trafficTransitions :
        capabilities.ambientDriver && capabilities.ambientTransitions;
}
bool CoopFleetOutrunApproved(uintptr_t ambientCar, uintptr_t humanCar) {
    if(CoopSpecialBusy())return false;
    using namespace CoopFleetWorldRuntime; coop_world::EntityKey key{}; const auto session = CoopSnapshot();
    if (!ready || !runtime.NativeKey(ambientCar, key)) return false;
    const uint32_t localPlayer = session.role == coop::Role::Host ? 1 : 2;
    const uint32_t player = humanCar == CoopRaceCar(false) ? localPlayer : humanCar == CoopRaceCar(true) ? 3 - localPlayer : 0;
    return player && runtime.EngagementApproved(key, player, Now());
}
bool CoopFleetOutrunBusy(uintptr_t ambientCar) {
    using namespace CoopFleetWorldRuntime; coop_world::EntityKey key{}; coop_world::Lease lease{};
    return ready && runtime.NativeKey(ambientCar, key) && runtime.GetLease(key, Now(), lease);
}
bool CoopFleetAnyOutrunBusy(){
    using namespace CoopFleetWorldRuntime;
    return ready&&(challenge.car||followedCar);
}
static bool ReadCoopFleetOutrunKey(uintptr_t car,coop_world::EntityKey& key) {
    using namespace CoopFleetWorldRuntime;
    return ready && runtime.NativeKey(car,key);
}
void RequestCoopFleetOutrunClaim(uintptr_t ambientCar, uintptr_t humanCar) {
    if(CoopSpecialBusy())return;
    using namespace CoopFleetWorldRuntime; coop_world::EntityKey key{};
    if (!ready || humanCar != CoopRaceCar(false) || !runtime.NativeKey(ambientCar, key)) return;
    const auto now = Now();
    if (challenge.car != ambientCar || challenge.key != key) {
        ++claimSerial; if (!claimSerial) ++claimSerial;
        challenge = {ambientCar, humanCar, key, claimSerial, now, false};
    }
    runtime.RequestClaim(key, challenge.claim, now, 5000); challenge.lastRequest = now;
}
void ReleaseCoopFleetOutrunClaim(uintptr_t ambientCar, uintptr_t humanCar) {
    using namespace CoopFleetWorldRuntime;
    if (challenge.car != ambientCar || challenge.human != humanCar || !ready) return;
    runtime.RequestRelease(challenge.key, challenge.claim, ++claimSerial, Now()); challenge = {};
}
void TickCoopFleetWorld(uint32_t flow) {
    using namespace CoopFleetWorldRuntime; const auto session = CoopSnapshot(); const auto now = Now();
    // A still-open lobby does not make the original SP world a COOP world.
    localFlow = IsCoopGameplayActive() ? flow : 3;
    if(flow!=6)ClearRetained();
    if (!installed || !Session(session)) {
        if(retainedTrack && flow==6)retainedLost=true;
        // A departed Client does not destroy an unchanged Host-owned native
        // FreeRoam. Freeze all wire work until explicit new membership proof.
        if(installed&&session.role==coop::Role::Host&&session.sessionId==loading.Identity().sessionId&&
            ProvenLocalWorld(flow)) {retainedWithoutPeer=true;FreezeTransport();return;}
        if (loading.Identity().sessionId) Reset(); return;
    }
    if (!IsCoopGameplayActive()) {
        const bool returningMenu=flow==3&&session.fresh&&session.careerLaunched&&session.localInMainMenu&&
            !session.peerInMainMenu&&EstablishedCampaign(session);
        if(!returningMenu){establishedFreeRoam={};establishedGeneration=0;establishedOwnCar=0;}
    }
    if(loading.Identity().sessionId==session.sessionId&&session.role==coop::Role::Host&&
       ProvenLocalWorld(flow)&&(retainedWithoutPeer||(!loading.Matches(session)&&!session.ticket))&&
       !session.localRetainsNative) {retainedWithoutPeer=true;FreezeTransport();return;}
    RefreshRetainedTransport(session,flow);
    if (loading.Identity().sessionId && (loading.Identity().sessionId != session.sessionId || peer != session.peer)) { Reset(); return; }
    Drain(session);
    if (ready && flow == 6 && IsCoopGameplayActive()) {
        CoopTrafficRuntime::Catalog catalog{};
        if (Capture(catalog)) {
            runtime.Observe(catalog, now);
            if(session.role==coop::Role::Client && catalog.stable) {
                ClearRetained();retainedTrack=catalog.track;size_t saved=0;
                for(size_t i=0;i<catalog.count && saved<retained.size();++i) {
                    const auto& row=catalog.rows[i];uintptr_t ride=0;uint32_t kind=0;uint8_t remote=0;
                    if(!ReadManagedCoopFleet(row.car,ride,kind) || ride!=row.ride || kind!=row.nativeKind ||
                        !ReadGameMemory(row.parameter+0x900,&remote,1) || remote!=1)continue;
                    retained[saved++]={row.car,row.parameter,row.ride,row.nativeKind,row.descriptor.modelId,row.nativeId};
                }
            }
        }
        Regions(now); runtime.Tick(now); TickCoopFleetOutrunNative(); ResumeCoopFleetOutrun();
        if (challenge.car) {
            if (IsCoopFleetOutrunActive(challenge.car,challenge.human)) challenge.entered = true;
            if (challenge.entered && !IsCoopFleetOutrunActive(challenge.car,challenge.human))
                ReleaseCoopFleetOutrunClaim(challenge.car, challenge.human);
            else if (coop_world::SerialDelta(now, challenge.lastRequest) >= 500) {
                if (challenge.entered) runtime.RequestRenew(challenge.key, challenge.claim, ++claimSerial, now, 5000);
                else runtime.RequestClaim(challenge.key, challenge.claim, now, 5000);
                challenge.lastRequest = now;
            }
        }
        if (session.role == coop::Role::Host) {
            if (followedCar && !IsCoopFleetOutrunActive(followedCar,CoopRaceCar(true))) {
                runtime.FinishAuthoritativeEngagement(followed,2,now);
                followed={}; followedCar=0;
            }
            for (size_t i = 0; i < catalog.count; ++i) {
                const auto& row = catalog.rows[i]; if (row.nativeKind != 2) continue;
                coop_world::EntityKey key{}; coop_world::Lease lease{};
                if (runtime.NativeKey(row.car, key) && runtime.GetLease(key, now, lease) && lease.ownerPlayerId == 2 &&
                    followed != key && StartCoopFleetHostOutrun(row.car, CoopRaceCar(true))) {
                    followed = key; followedCar=row.car;
                }
            }
        }
        static uint32_t lastTrace=0;
        if(coop_world::SerialDelta(now,lastTrace)>=2000) {
            lastTrace=now;const auto& stats=runtime.Stats();char line[240]{};
            std::snprintf(line,sizeof(line),"[FLEET_TRACE] role=%s epoch=%u pool=%u rx=%u applied=%u pending=%u captureRefused=%u applyRefused=%u dropped=%u\r\n",
                session.role==coop::Role::Host?"host":"client",loading.Identity().worldEpoch,
                static_cast<unsigned>(catalog.count),stats.received,stats.applied,stats.pendingModels,
                stats.captureRefused,stats.applyRefused,queueDropped.load(std::memory_order_relaxed));LogMessage(line);
        }
    }
    Publish(session, now);
}
bool ReceiveCoopFleetWorldPacket(const void* bytes, size_t size, const coop::Endpoint& source, uint32_t) {
    using namespace CoopFleetWorldRuntime;
    if (!bytes || size < 4) return false;
    uint32_t magic = 0; std::memcpy(&magic, bytes, 4); magic = coop_world_wire::LE32(magic);
    bool fleet = magic == coop_fleet_load::kMagic;
    coop_world::Event event{}; uint32_t sender = 0;
    if (magic == coop_world_wire::kEventMagic) {
        if (!coop_world_wire::DecodeEvent(bytes, size, event, sender, 1) || event.state.kind == coop_world::Kind::RaceRacer) return false;
        fleet = event.state.kind == coop_world::Kind::Traffic || event.state.kind == coop_world::Kind::AmbientRacer;
    } else if (magic == coop_world_wire::kLeaseMagic) {
        coop_world_wire::LeaseEnvelope envelope{};
        if (!coop_world_wire::DecodeLease(bytes, size, envelope, 1)) return true;
        uint32_t kind = 0; uint8_t id = 0; fleet = CoopTrafficRuntime::DecodeId(envelope.state.key.id, kind, id) && kind == 2;
    }
    if (!fleet) return false;
    const auto session = CoopSnapshot();
    if (!gCoopFleetEnabled || !session.joined || source != session.peer || size > sizeof(CoopTrafficRuntime::Datagram::bytes)) return true;
    coop_fleet_load::Message message{};
    if (magic == coop_fleet_load::kMagic && (!coop_fleet_load::Decode(bytes, size, message) ||
        message.identity.sessionId != session.sessionId ||message.ticket!=session.ticket||message.membership!=session.membershipNonce||
        message.sender != (session.role == coop::Role::Host ? 2 : 1))) return true;
    if (magic == coop_world_wire::kEventMagic && (session.role != coop::Role::Client || sender != 1)) return true;
    AcquireSRWLockExclusive(&lock);
    if (inboxCount < inbox.size()) {
        auto& item = inbox[inboxCount++]; item = {}; item.datagram.size = static_cast<uint16_t>(size);
        std::memcpy(item.datagram.bytes, bytes, size); item.endpoint = source; item.receivedAt = Now();
    } else ++queueDropped;
    ReleaseSRWLockExclusive(&lock); return true;
}
void TickCoopFleetWorldNetwork(uint32_t) {
    using namespace CoopFleetWorldRuntime; const auto session = CoopSnapshot(); const auto now = Now();
    if (!gCoopFleetEnabled || !session.joined || !session.peer.Valid() || coop_world::SerialDelta(now, lastSent) < 15) return;
    std::array<coop_fleet_net::Datagram,coop_fleet_net::kBurstLimit> frames{}; size_t count = 0;
    AcquireSRWLockExclusive(&lock);
    if(networkOutbox.Matches(session.sessionId,session.peer))count=networkOutbox.PopDue(frames.data(),frames.size(),now);
    ReleaseSRWLockExclusive(&lock);
    if (!count) return;
    lastSent = now; for (size_t i = 0; i < count; ++i) SendSessionBytes(frames[i].bytes, frames[i].size, session.peer);
}
bool InstallCoopFleetWorldNative() {
    using namespace CoopFleetWorldRuntime;
    const CoopFleetNative::Port native{&ReadManagedCoopFleet, &ReadCoopFleetIdentity, &CoopFleetWorldEpoch,
        &HostFocus, &CoopFleetOutrunApproved, &CoopFleetOutrunBusy, &RequestCoopFleetOutrunClaim, &ReadCoopFleetOutrunKey, &RetainedClient};
    if (!InstallCoopFleetNativeHooks(native)) return false;
    CoopTrafficRuntime::Capabilities hooks{};
    if (!GetCoopFleetNativeCapabilities(hooks)) return false;
    ConfigureCoopFleetWorld(hooks); installed = true;
    LogMessage("[FLEET_WORLD] Host-authoritative traffic and ambient snapshots enabled; queue-only UDP, strict native manifests.\r\n");
    return true;
}
