// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once

#include "CoopWorldSync.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

// Fixed-size, little-endian value packets. Encode/decode each field explicitly;
// never send the padding/layout of Event, Descriptor, Motion or native objects.
namespace coop_world_wire {

constexpr std::uint32_t kEventMagic = 0x5732434eU; // NC2W
constexpr std::uint32_t kLeaseMagic = 0x4c32434eU; // NC2L
constexpr std::uint16_t kVersion = 4;
constexpr std::uint8_t kEventKind = 1;
enum class LeaseMessageKind : std::uint8_t { ClaimRequest = 1, ReleaseRequest = 2, AuthorityState = 3, RenewRequest = 4 };
enum class LeaseDecision : std::uint8_t { State = 0, Granted = 1, Released = 2, Busy = 3, Rejected = 4 };

inline bool LittleEndianHost() { const std::uint16_t value = 1; std::uint8_t byte; std::memcpy(&byte, &value, 1); return byte == 1; }
inline std::uint16_t Swap16(std::uint16_t x) { return static_cast<std::uint16_t>((x >> 8) | (x << 8)); }
inline std::uint32_t Swap32(std::uint32_t x) { return (x >> 24) | ((x >> 8) & 0xff00U) | ((x << 8) & 0xff0000U) | (x << 24); }
inline std::uint64_t Swap64(std::uint64_t x) { return (std::uint64_t(Swap32(static_cast<std::uint32_t>(x))) << 32) | Swap32(static_cast<std::uint32_t>(x >> 32)); }
inline std::uint16_t LE16(std::uint16_t x) { return LittleEndianHost() ? x : Swap16(x); }
inline std::uint32_t LE32(std::uint32_t x) { return LittleEndianHost() ? x : Swap32(x); }
inline std::uint64_t LE64(std::uint64_t x) { return LittleEndianHost() ? x : Swap64(x); }
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "Wire motion requires IEEE-754 binary32");
inline std::uint32_t FloatBits(float value) { std::uint32_t bits; std::memcpy(&bits, &value, 4); return LE32(bits); }
inline float BitsFloat(std::uint32_t bits) { bits = LE32(bits); float value; std::memcpy(&value, &bits, 4); return value; }

#pragma pack(push, 1)
struct Header {
    std::uint32_t magic;
    std::uint16_t version, size;
    std::uint8_t kind, senderPlayerId, hostPlayerId, reserved;
};
struct IdentityFields { std::uint64_t sessionId; std::uint32_t trackId, worldEpoch, raceEpoch; };
struct KeyFields { std::uint64_t id; std::uint32_t generation; };
struct DescriptorFields {
    std::uint32_t modelId, modelHash;
    std::uint64_t appearanceHash, handlingHash;
    char carModel[16];
};
struct MotionFields {
    std::uint32_t sequence, sentAtMs, flowState;
    std::uint64_t sessionId;
    std::uint32_t trackId, worldEpoch;
    char carModel[16];
    std::uint32_t position[3], speedKmh, headingDeg, forward[3];
    std::uint16_t steerAngle;
    std::uint8_t brakeLight, reverseLight;
    std::uint32_t wheelPhaseRad[4], wheelSpeedRad[4], wheelRotation[4][9], wheelRotationValidMask;
    std::uint32_t orientation[4], linearVelocity[3], angularVelocity[3];
    std::uint32_t totalProgress, splineDistance;
    std::uint8_t lap, completedLaps, finishStatus, finishRank, aiFrozen, reserved[3];
    std::uint32_t finishTicks;
    std::uint16_t nativeFinishStatus;
    std::uint32_t nativeRaceScore;
    std::uint32_t tireValidMask;
    std::uint32_t tirePhase[4], tireSpeed[4], suspension[4], tireScalar50[4], tireSlip[4][2], tireLoad[4], tireSpring[4], tireRatio[4];
    std::uint8_t tireGround[4], tireSkid[4];
};
struct EventPacket {
    Header header;
    IdentityFields identity;
    std::uint32_t eventSequence;
    KeyFields key;
    std::uint8_t action, entityKind;
    std::uint16_t reserved;
    std::uint64_t raceMembershipId;
    DescriptorFields descriptor;
    std::uint32_t descriptorDigest;
    MotionFields motion;
};
struct LeasePacket {
    Header header;
    IdentityFields identity;
    KeyFields key;
    std::uint32_t leaseSequence, ownerPlayerId, durationMs;
    std::uint64_t eventId, claimId;
    std::uint8_t decision, reserved[3];
};
#pragma pack(pop)

static_assert(sizeof(Header) == 12, "Wire header layout changed");
static_assert(sizeof(MotionFields) == 482, "Wire motion layout changed");
static_assert(sizeof(EventPacket) == 586, "Wire event layout changed");
static_assert(sizeof(LeasePacket) == 76, "Wire lease layout changed");
static_assert(sizeof(EventPacket) < coop_world::kMaxDatagramBytes && sizeof(LeasePacket) < coop_world::kMaxDatagramBytes, "Wire packets must fit the datagram budget");
static_assert(std::is_trivially_copyable<EventPacket>::value && std::is_trivially_copyable<LeasePacket>::value, "Wire packets must be plain values");

inline void EncodeIdentity(const coop_world::Identity& input, IdentityFields& output) {
    output.sessionId = LE64(input.sessionId); output.trackId = LE32(input.trackId);
    output.worldEpoch = LE32(input.worldEpoch); output.raceEpoch = LE32(input.raceEpoch);
}
inline coop_world::Identity DecodeIdentity(const IdentityFields& input) {
    return coop_world::Identity{LE64(input.sessionId), LE32(input.trackId), LE32(input.worldEpoch), LE32(input.raceEpoch)};
}
inline void EncodeKey(coop_world::EntityKey input, KeyFields& output) { output.id = LE64(input.id); output.generation = LE32(input.generation); }
inline coop_world::EntityKey DecodeKey(const KeyFields& input) { return coop_world::EntityKey{LE64(input.id), LE32(input.generation)}; }
inline void EncodeDescriptor(const coop_world::Descriptor& input, DescriptorFields& output) {
    output.modelId = LE32(input.modelId); output.modelHash = LE32(input.modelHash);
    output.appearanceHash = LE64(input.appearanceHash); output.handlingHash = LE64(input.handlingHash);
    std::memcpy(output.carModel, input.carModel, 16);
}
inline coop_world::Descriptor DecodeDescriptor(const DescriptorFields& input) {
    coop_world::Descriptor output;
    output.modelId = LE32(input.modelId); output.modelHash = LE32(input.modelHash);
    output.appearanceHash = LE64(input.appearanceHash); output.handlingHash = LE64(input.handlingHash);
    std::memcpy(output.carModel, input.carModel, 16);
    return output;
}
inline std::uint32_t DescriptorDigest(const coop_world::Descriptor& descriptor) {
    DescriptorFields fields{}; EncodeDescriptor(descriptor, fields);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&fields);
    std::uint32_t hash = 2166136261U;
    for (std::size_t i = 0; i != sizeof(fields); ++i) hash = (hash ^ bytes[i]) * 16777619U;
    return hash;
}
inline void EncodeMotion(const coop_world::Motion& input, MotionFields& output) {
    output.sequence = LE32(input.sequence); output.sentAtMs = LE32(input.sentAtMs); output.flowState = LE32(input.flowState);
    output.sessionId = LE64(input.sessionId); output.trackId = LE32(input.trackId); output.worldEpoch = LE32(input.worldEpoch);
    std::memcpy(output.carModel, input.carModel, 16);
    output.position[0] = FloatBits(input.x); output.position[1] = FloatBits(input.y); output.position[2] = FloatBits(input.z);
    output.speedKmh = FloatBits(input.speedKmh); output.headingDeg = FloatBits(input.headingDeg);
    output.forward[0] = FloatBits(input.forwardX); output.forward[1] = FloatBits(input.forwardY); output.forward[2] = FloatBits(input.forwardZ);
    output.steerAngle = LE16(static_cast<std::uint16_t>(input.steerAngle));
    output.brakeLight = input.brakeLight; output.reverseLight = input.reverseLight;
    output.wheelRotationValidMask = LE32(input.wheelRotationValidMask);
    for (int i = 0; i != 4; ++i) {
        output.orientation[i] = FloatBits(input.orientation[i]);
        output.wheelPhaseRad[i] = FloatBits(input.wheelPhaseRad[i]); output.wheelSpeedRad[i] = FloatBits(input.wheelSpeedRad[i]);
        for (int j = 0; j != 9; ++j) output.wheelRotation[i][j] = FloatBits(input.wheelRotation[i][j]);
    }
    for (int i = 0; i != 3; ++i) { output.linearVelocity[i] = FloatBits(input.linearVelocity[i]); output.angularVelocity[i] = FloatBits(input.angularVelocity[i]); }
    output.totalProgress = FloatBits(input.totalProgress); output.splineDistance = FloatBits(input.splineDistance);
    output.lap = input.lap; output.completedLaps = input.completedLaps; output.finishStatus = input.finishStatus;
    output.finishRank = input.finishRank; output.finishTicks = LE32(input.finishTicks); output.aiFrozen = input.aiFrozen;
    output.nativeFinishStatus = LE16(input.nativeFinishStatus);
    output.nativeRaceScore = FloatBits(input.nativeRaceScore);
    output.tireValidMask = LE32(input.wheels.validMask);
    for (unsigned i=0;i<4;++i) {
        output.tirePhase[i]=FloatBits(input.wheels.phase[i]); output.tireSpeed[i]=FloatBits(input.wheels.speed[i]);
        output.suspension[i]=FloatBits(input.wheels.suspension[i]); output.tireScalar50[i]=FloatBits(input.wheels.scalar50[i]);
        output.tireSlip[i][0]=FloatBits(input.wheels.slip[i][0]); output.tireSlip[i][1]=FloatBits(input.wheels.slip[i][1]);
        output.tireLoad[i]=FloatBits(input.wheels.load[i]); output.tireSpring[i]=FloatBits(input.wheels.spring[i]);
        output.tireRatio[i]=FloatBits(input.wheels.ratio[i]); output.tireGround[i]=input.wheels.ground[i]; output.tireSkid[i]=input.wheels.skid[i];
    }
    for (auto& value : output.reserved) value = 0;
}
inline coop_world::Motion DecodeMotion(const MotionFields& input) {
    coop_world::Motion output;
    output.sequence = LE32(input.sequence); output.sentAtMs = LE32(input.sentAtMs); output.flowState = LE32(input.flowState);
    output.sessionId = LE64(input.sessionId); output.trackId = LE32(input.trackId); output.worldEpoch = LE32(input.worldEpoch);
    std::memcpy(output.carModel, input.carModel, 16);
    output.x = BitsFloat(input.position[0]); output.y = BitsFloat(input.position[1]); output.z = BitsFloat(input.position[2]);
    output.speedKmh = BitsFloat(input.speedKmh); output.headingDeg = BitsFloat(input.headingDeg);
    output.forwardX = BitsFloat(input.forward[0]); output.forwardY = BitsFloat(input.forward[1]); output.forwardZ = BitsFloat(input.forward[2]);
    const std::uint16_t steer = LE16(input.steerAngle);
    output.steerAngle = steer <= 0x7fffU ? static_cast<std::int16_t>(steer) : static_cast<std::int16_t>(-1 - static_cast<std::int32_t>(0xffffU - steer));
    output.brakeLight = input.brakeLight; output.reverseLight = input.reverseLight;
    output.wheelRotationValidMask = LE32(input.wheelRotationValidMask);
    for (int i = 0; i != 4; ++i) {
        output.orientation[i] = BitsFloat(input.orientation[i]);
        output.wheelPhaseRad[i] = BitsFloat(input.wheelPhaseRad[i]); output.wheelSpeedRad[i] = BitsFloat(input.wheelSpeedRad[i]);
        for (int j = 0; j != 9; ++j) output.wheelRotation[i][j] = BitsFloat(input.wheelRotation[i][j]);
    }
    for (int i = 0; i != 3; ++i) { output.linearVelocity[i] = BitsFloat(input.linearVelocity[i]); output.angularVelocity[i] = BitsFloat(input.angularVelocity[i]); }
    output.totalProgress = BitsFloat(input.totalProgress); output.splineDistance = BitsFloat(input.splineDistance);
    output.lap = input.lap; output.completedLaps = input.completedLaps; output.finishStatus = input.finishStatus;
    output.finishRank = input.finishRank; output.finishTicks = LE32(input.finishTicks); output.aiFrozen = input.aiFrozen;
    output.nativeFinishStatus = LE16(input.nativeFinishStatus);
    output.nativeRaceScore = BitsFloat(input.nativeRaceScore);
    output.wheels.validMask = LE32(input.tireValidMask);
    for (unsigned i=0;i<4;++i) {
        output.wheels.phase[i]=BitsFloat(input.tirePhase[i]); output.wheels.speed[i]=BitsFloat(input.tireSpeed[i]);
        output.wheels.suspension[i]=BitsFloat(input.suspension[i]); output.wheels.scalar50[i]=BitsFloat(input.tireScalar50[i]);
        output.wheels.slip[i][0]=BitsFloat(input.tireSlip[i][0]); output.wheels.slip[i][1]=BitsFloat(input.tireSlip[i][1]);
        output.wheels.load[i]=BitsFloat(input.tireLoad[i]); output.wheels.spring[i]=BitsFloat(input.tireSpring[i]);
        output.wheels.ratio[i]=BitsFloat(input.tireRatio[i]); output.wheels.ground[i]=input.tireGround[i]; output.wheels.skid[i]=input.tireSkid[i];
    }
    return output;
}
inline bool PlayerValid(std::uint32_t player) { return player == 1 || player == 2; }
inline bool EventValid(const coop_world::Event& event) {
    if (!event.identity.Valid() || !event.state.key.Valid() || !coop_world::ValidMotion(event.state.motion)) return false;
    const auto action = event.action;
    if (action != coop_world::Action::Spawn && action != coop_world::Action::Update && action != coop_world::Action::Remove) return false;
    const auto kind = event.state.kind;
    if (kind != coop_world::Kind::Traffic && kind != coop_world::Kind::AmbientRacer && kind != coop_world::Kind::RaceRacer) return false;
    if (action == coop_world::Action::Remove) return true;
    if (!coop_world::ValidDescriptor(event.state.descriptor)) return false;
    return kind == coop_world::Kind::RaceRacer ? event.identity.raceEpoch != 0 && event.state.raceMembershipId != 0 : event.state.raceMembershipId == 0;
}
inline bool EncodeEvent(const coop_world::Event& event, std::uint32_t hostPlayerId, EventPacket& output) {
    if (!PlayerValid(hostPlayerId) || !EventValid(event)) return false;
    EventPacket packet{};
    packet.header = Header{LE32(kEventMagic), LE16(kVersion), LE16(static_cast<std::uint16_t>(sizeof(packet))), kEventKind,
        static_cast<std::uint8_t>(hostPlayerId), static_cast<std::uint8_t>(hostPlayerId), 0};
    EncodeIdentity(event.identity, packet.identity); EncodeKey(event.state.key, packet.key);
    packet.eventSequence = LE32(event.sequence); packet.action = static_cast<std::uint8_t>(event.action); packet.entityKind = static_cast<std::uint8_t>(event.state.kind);
    packet.raceMembershipId = LE64(event.state.raceMembershipId); EncodeDescriptor(event.state.descriptor, packet.descriptor);
    packet.descriptorDigest = LE32(DescriptorDigest(event.state.descriptor));
    coop_world::Motion motion = event.state.motion;
    motion.sequence = event.sequence; motion.flowState = 6; motion.sessionId = event.identity.sessionId;
    motion.trackId = event.identity.trackId; motion.worldEpoch = event.identity.worldEpoch;
    std::memcpy(motion.carModel, event.state.descriptor.carModel, 16);
    if (motion.finishStatus != coop_world::kFinishRacing) motion.aiFrozen = 1;
    EncodeMotion(motion, packet.motion);
    output = packet;
    return true;
}
inline bool DecodeEvent(const void* data, std::size_t size, coop_world::Event& output, std::uint32_t& senderPlayerId,
    std::uint32_t expectedHostPlayerId = 0) {
    if (!data || size != sizeof(EventPacket)) return false;
    EventPacket packet; std::memcpy(&packet, data, sizeof(packet));
    const Header& h = packet.header;
    if (LE32(h.magic) != kEventMagic || LE16(h.version) != kVersion || LE16(h.size) != size || h.kind != kEventKind || h.reserved || packet.reserved ||
        !PlayerValid(h.senderPlayerId) || !PlayerValid(h.hostPlayerId) || h.senderPlayerId != h.hostPlayerId ||
        (expectedHostPlayerId && h.hostPlayerId != expectedHostPlayerId)) return false;
    for (auto reserved : packet.motion.reserved) if (reserved) return false;
    coop_world::Event event;
    event.identity = DecodeIdentity(packet.identity); event.sequence = LE32(packet.eventSequence);
    event.action = static_cast<coop_world::Action>(packet.action); event.state.key = DecodeKey(packet.key);
    event.state.kind = static_cast<coop_world::Kind>(packet.entityKind); event.state.raceMembershipId = LE64(packet.raceMembershipId);
    event.state.descriptor = DecodeDescriptor(packet.descriptor); event.state.motion = DecodeMotion(packet.motion);
    if (!EventValid(event) || DescriptorDigest(event.state.descriptor) != LE32(packet.descriptorDigest)) return false;
    const auto& motion = event.state.motion;
    if (motion.sequence != event.sequence || motion.flowState != 6 || motion.sessionId != event.identity.sessionId || motion.trackId != event.identity.trackId ||
        motion.worldEpoch != event.identity.worldEpoch || std::memcmp(motion.carModel, event.state.descriptor.carModel, 16) != 0 ||
        motion.brakeLight > 2 || motion.reverseLight > 1 || (motion.finishStatus != coop_world::kFinishRacing && !motion.aiFrozen)) return false;
    output = event; senderPlayerId = h.senderPlayerId;
    return true;
}

struct LeaseEnvelope {
    LeaseMessageKind kind = LeaseMessageKind::AuthorityState;
    LeaseDecision decision = LeaseDecision::State;
    std::uint32_t senderPlayerId = 0, hostPlayerId = 0;
    // Correlates a request/response; active claimId remains state.requestId.
    std::uint64_t eventId = 0;
    coop_world::LeaseMessage state;
};
inline bool LeaseValid(const LeaseEnvelope& envelope) {
    const auto& state = envelope.state;
    if (!PlayerValid(envelope.senderPlayerId) || !PlayerValid(envelope.hostPlayerId) || !state.identity.Valid() || !state.key.Valid()) return false;
    if (envelope.kind == LeaseMessageKind::AuthorityState) {
        if (envelope.senderPlayerId != envelope.hostPlayerId || static_cast<unsigned>(envelope.decision) > static_cast<unsigned>(LeaseDecision::Rejected) ||
            (envelope.decision != LeaseDecision::State && !envelope.eventId)) return false;
        if (envelope.decision == LeaseDecision::Granted && !state.ownerPlayerId) return false;
        if (envelope.decision == LeaseDecision::Released && state.ownerPlayerId) return false;
        return state.ownerPlayerId ? PlayerValid(state.ownerPlayerId) && state.requestId != 0 && state.durationMs != 0 && state.durationMs <= coop_world::kMaxLeaseMs :
            state.requestId == 0 && state.durationMs == 0;
    }
    if (envelope.kind != LeaseMessageKind::ClaimRequest && envelope.kind != LeaseMessageKind::ReleaseRequest &&
        envelope.kind != LeaseMessageKind::RenewRequest) return false;
    if (envelope.decision != LeaseDecision::State || !envelope.eventId || !state.requestId || state.ownerPlayerId != envelope.senderPlayerId) return false;
    if (envelope.kind == LeaseMessageKind::ReleaseRequest) return state.durationMs == 0;
    return (envelope.kind == LeaseMessageKind::RenewRequest || state.requestId == envelope.eventId) &&
        state.durationMs >= 100 && state.durationMs <= coop_world::kMaxLeaseMs;
}
inline bool EncodeLease(const LeaseEnvelope& envelope, LeasePacket& output) {
    if (!LeaseValid(envelope)) return false;
    LeasePacket packet{};
    packet.header = Header{LE32(kLeaseMagic), LE16(kVersion), LE16(static_cast<std::uint16_t>(sizeof(packet))), static_cast<std::uint8_t>(envelope.kind),
        static_cast<std::uint8_t>(envelope.senderPlayerId), static_cast<std::uint8_t>(envelope.hostPlayerId), 0};
    EncodeIdentity(envelope.state.identity, packet.identity); EncodeKey(envelope.state.key, packet.key);
    packet.leaseSequence = LE32(envelope.state.sequence); packet.ownerPlayerId = LE32(envelope.state.ownerPlayerId);
    packet.durationMs = LE32(envelope.state.durationMs); packet.eventId = LE64(envelope.eventId); packet.claimId = LE64(envelope.state.requestId);
    packet.decision = static_cast<std::uint8_t>(envelope.decision);
    output = packet;
    return true;
}
inline bool DecodeLease(const void* data, std::size_t size, LeaseEnvelope& output, std::uint32_t expectedHostPlayerId = 0) {
    if (!data || size != sizeof(LeasePacket)) return false;
    LeasePacket packet; std::memcpy(&packet, data, sizeof(packet));
    const Header& h = packet.header;
    if (LE32(h.magic) != kLeaseMagic || LE16(h.version) != kVersion || LE16(h.size) != size || h.reserved ||
        (expectedHostPlayerId && h.hostPlayerId != expectedHostPlayerId)) return false;
    for (auto reserved : packet.reserved) if (reserved) return false;
    LeaseEnvelope envelope;
    envelope.kind = static_cast<LeaseMessageKind>(h.kind); envelope.decision = static_cast<LeaseDecision>(packet.decision);
    envelope.senderPlayerId = h.senderPlayerId; envelope.hostPlayerId = h.hostPlayerId; envelope.eventId = LE64(packet.eventId);
    envelope.state.identity = DecodeIdentity(packet.identity); envelope.state.key = DecodeKey(packet.key);
    envelope.state.sequence = LE32(packet.leaseSequence); envelope.state.ownerPlayerId = LE32(packet.ownerPlayerId);
    envelope.state.durationMs = LE32(packet.durationMs); envelope.state.requestId = LE64(packet.claimId);
    if (!LeaseValid(envelope)) return false;
    output = envelope;
    return true;
}

} // namespace coop_world_wire
