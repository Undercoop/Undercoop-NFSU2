// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Included after CoopRaceLifecycle.inl. Prepare/Commit/Read run on the game
// thread; network Tick/Receive use only locked published transfer state.
#include "CoopAIRoster.h"
#include "CoopCarCatalog.h"
#include "CoopCheckpoint.h"
#include "CoopFleetTransfer.h"
#include "CoopUnlimiterRideCompat.h"
bool CoopUrlRetainedTransition(uint32_t oldTrack,uint32_t nextTrack,uint32_t oldEpoch,
    uint32_t nextEpoch,uint32_t eventHash);

namespace CoopAIRosterNative {
constexpr uint32_t kMagic = 0x5741324e; // N2AW, separate from all other protocols.
constexpr uint32_t kManifestMagic = 0x52414943;
constexpr uint16_t kRejected = 0xFFFE;
constexpr uint32_t Hash(const char* text) {
    uint32_t hash = 0xffffffffu;
    while (*text) hash = hash * 33u + static_cast<uint8_t>(*text++);
    return hash;
}
constexpr uint32_t kRandomPreset = Hash("RANDOM");
#pragma pack(push, 1)
struct Identity {
    uint64_t session = 0;
    uint32_t token = 0, track = 0, eventHash = 0;
};
struct Opponent {
    uint8_t sourceSlot = 0, playerIndex = 0, flags = 0, aiSkill = 0, aiStyle = 0;
    uint8_t descriptor[0x0C]{};
    char name[16]{};
    uint8_t portable[0x940]{};
};
struct Manifest {
    uint32_t magic = kManifestMagic;
    uint16_t version = 1, size = sizeof(Manifest);
    Identity identity{};
    uint8_t nativeCapacity = 0, effectiveCapacity = 0, opponentCount = 0, aiCount = 0;
    Opponent opponents[coop_ai_roster::kMaximumNativeOpponents]{};
};
struct Envelope {
    uint32_t magic = kMagic;
    uint16_t version = 1, size = sizeof(Envelope);
    Identity identity{};
    uint16_t senderPlayer = 0;
    coop_checkpoint::Packet block{};
};
#pragma pack(pop)
static_assert(sizeof(Envelope) <= 1200, "AI roster packets must not fragment");
static_assert(sizeof(Manifest) <= coop_checkpoint::kMaxFile, "Roster transfer is bounded");
static bool Same(const Identity& a, const Identity& b) {
    return a.session == b.session && a.token == b.token && a.track == b.track && a.eventHash == b.eventHash;
}
static bool Valid(const Identity& id) {
    return id.session && id.token && id.track && id.track != 4000 && id.eventHash;
}
static bool Nonzero(const uint8_t* bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) if (bytes[i]) return true;
    return false;
}
static coop_ai_roster::Plan ManifestPlan(const Manifest& manifest) {
    coop_ai_roster::NativeOpponent opponents[5]{};
    for (size_t i = 0; i < manifest.opponentCount && i < 5; ++i) {
        uint32_t preset = 0;
        std::memcpy(&preset, manifest.opponents[i].descriptor + 4, 4);
        opponents[i] = {manifest.opponents[i].sourceSlot, preset != kRandomPreset};
    }
    return coop_ai_roster::BuildPlan(manifest.nativeCapacity, opponents, manifest.opponentCount);
}
static bool ValidManifest(const Manifest& manifest, const Identity& identity) {
    if (manifest.magic != kManifestMagic || manifest.version != 1 || manifest.size != sizeof(Manifest) ||
        !Same(manifest.identity, identity) || !Valid(identity) ||
        manifest.opponentCount > 5) return false;
    const auto plan = ManifestPlan(manifest);
    if (!plan.Ready() || manifest.effectiveCapacity != plan.totalCount || manifest.aiCount != plan.aiCount) return false;
    for (size_t i = 0; i < manifest.opponentCount; ++i) {
        const auto& opponent = manifest.opponents[i];
        uint32_t model = 0;
        std::memcpy(&model, opponent.portable + 0x10, 4);
        // The manifest stores finalized native AI identities, never player IDs.
        if (opponent.sourceSlot != i + 1 || opponent.playerIndex != i + 100 ||
            !opponent.name[0] || !std::memchr(opponent.name, 0, sizeof(opponent.name)) || !coop_car_catalog::PortableModelIndex(model) ||
            Nonzero(opponent.portable, 0x10) ||
            Nonzero(opponent.portable + 0x10 + 0x38C, 8) ||
            Nonzero(opponent.portable + 0x10 + 0x3C0, 4) ||
            Nonzero(opponent.portable + 0x10 + 0x57C, 4) ||
            Nonzero(opponent.portable + 0x10 + 0x584, 12) ||
            Nonzero(opponent.portable + 0x8F2, 0x940 - 0x8F2)) return false;
    }
    // Unused wire entries must contain no hidden stale native data.
    for (size_t i = manifest.opponentCount; i < 5; ++i)
        if (Nonzero(reinterpret_cast<const uint8_t*>(&manifest.opponents[i]), sizeof(Opponent))) return false;
    return true;
}

struct Addresses {
    uintptr_t array = 0x0089E810, count = 0x0089E814, track = 0x0089E7A0;
    uintptr_t participants = 0x0089E7C0, parameters = 0x0089E7A0, career = 0x0085AD40;
    uintptr_t partManager = 0x008A2B68;
};
static Addresses gAddresses{};
static void NativeRideCtor(uint8_t* ride) {
    reinterpret_cast<void*(__thiscall*)(void*)>(0x0040C910)(ride);
}
static void NativeCopy(uint8_t* output, const uint8_t* input) {
    reinterpret_cast<void*(__thiscall*)(void*, const void*)>(0x005FC130)(output, input);
}
static void NativeRideInit(uint8_t* ride, uint32_t model) {
    reinterpret_cast<void(__thiscall*)(void*, int, int, char, char)>(0x00610270)(ride, static_cast<int>(model), 0, 0, 0);
}
static void NativeStock(uint8_t* ride) {
    reinterpret_cast<void(__thiscall*)(void*, int)>(0x00637040)(ride, 0);
}
static void* NativePart(uint32_t model, uint32_t slot, uint32_t hash) {
    return reinterpret_cast<void*(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, void*, int)>(0x0061B980)(
        reinterpret_cast<void*>(gAddresses.partManager), model, slot, hash, nullptr, -1);
}
static void NativeParts(uint8_t* ride) {
    reinterpret_cast<void(__thiscall*)(void*)>(0x0061BCD0)(ride);
}
static void NativeTexture(uint8_t* ride, uint32_t slot) {
    reinterpret_cast<void(__thiscall*)(void*, int)>(0x0061C280)(ride, static_cast<int>(slot));
}
static uintptr_t NativeEvent(uint32_t hash) {
    return reinterpret_cast<uintptr_t>(reinterpret_cast<void*(__thiscall*)(void*, uint32_t)>(0x005003F0)(
        reinterpret_cast<void*>(gAddresses.career), hash));
}
static void NativeAdd(uint8_t* parameter) {
    reinterpret_cast<void(__thiscall*)(void*, void*)>(&TrampolineRaceParametersAddVehicle)(
        reinterpret_cast<void*>(gAddresses.parameters), parameter);
}
static void NativeRepair(uint8_t* parameter) {
    // 413440/4133D0 rebuild embedded tuning and spline self-pointers. Copying
    // onto itself is legal for these native copy loops and preserves local parts.
    NativeCopy(parameter, parameter);
}
static bool NativeSend(const Envelope& packet, const coop::Endpoint& endpoint) {
    if (gGameSocket == INVALID_SOCKET || !endpoint.Valid()) return false;
    return SendMeasuredCoopDatagram(&packet,sizeof(packet),endpoint)==sizeof(packet);
}
// Fixture overrides replace native calls, never the validation contract.
struct Api {
    void (*ctor)(uint8_t*) = NativeRideCtor;
    void (*copy)(uint8_t*, const uint8_t*) = NativeCopy;
    void (*init)(uint8_t*, uint32_t) = NativeRideInit;
    void (*stock)(uint8_t*) = NativeStock;
    void* (*part)(uint32_t, uint32_t, uint32_t) = NativePart;
    void (*parts)(uint8_t*) = NativeParts;
    void (*texture)(uint8_t*, uint32_t) = NativeTexture;
    uintptr_t (*event)(uint32_t) = NativeEvent;
    void (*add)(uint8_t*) = NativeAdd;
    void (*repair)(uint8_t*) = NativeRepair;
    bool (*send)(const Envelope&, const coop::Endpoint&) = NativeSend;
};
static Api gApi{};
struct Signature { uintptr_t address; uint8_t bytes[8]; uint8_t length; };
static Signature gSignatures[] = {
    {0x40C910,{0x6A,0xFF,0x68,0xEB,0x13,0x77,0x00},7},
    {0x5FC130,{0x55,0x56,0x8B,0xF1,0x57,0x8B,0x7C,0x24},8},
    {0x610270,{0x8B,0x44,0x24,0x04,0x8B,0xD1},6},
    {0x637040,{0x83,0xEC,0x10,0x53,0x55,0x56,0x57},7},
    {0x61B980,{0x51,0x8B,0x44,0x24,0x08,0x56},6},
    {0x61BCD0,{0x81,0xEC,0x88,0x00,0x00,0x00},6},
    {0x61C280,{0x83,0xEC,0x40,0x56,0x57},5},
    {0x5003F0,{0x8B,0x44,0x24,0x04,0x50},5}
};
static bool VerifyNative() {
    static uintptr_t lastFailure=0;
    for (const auto& signature : gSignatures) {
        uint8_t observed[8]{};
        const bool readable=ReadGameMemory(signature.address, observed, signature.length);
        if (!readable || std::memcmp(observed, signature.bytes, signature.length)) {
            // The same audited RideInfo parts adapter is used by fleet and
            // race AI. Authenticate its existing hook; do not remove it or
            // accept an arbitrary JMP merely because both peers have it.
            if(readable && signature.address==coop_unlimiter_ride::kEntry &&
                coop_unlimiter_ride::Approved(signature.address,observed,signature.length)) {
                static bool reported=false;
                if(!reported) {
                    LogMessage("[AI_ROSTER] Verified Unlimiter 3.1.1.1337 RideInfo parts adapter (SHA-256, PE identity, owned target and body). Existing mod hook preserved.\r\n");
                    reported=true;
                }
                continue;
            }
            if(lastFailure!=signature.address) {
                char message[192]{};
                _snprintf_s(message,sizeof(message),_TRUNCATE,
                    "[AI_ROSTER] Native signature %08X %s: %02X %02X %02X %02X %02X %02X %02X %02X; unsupported adapter, streaming held.\r\n",
                    static_cast<unsigned>(signature.address),readable?"differs":"unreadable",
                    observed[0],observed[1],observed[2],observed[3],observed[4],observed[5],observed[6],observed[7]);
                LogMessage(message);lastFailure=signature.address;
            }
            return false;
        }
    }
    lastFailure=0;
    return true;
}
struct NetworkState {
    Identity identity{};
    coop::Endpoint peer{};
    bool host = false, available = false, ackPending = false;
    uint32_t digest = 0;
    coop_fleet_transfer::Sender sender;
    coop_checkpoint::Receiver receiver;
    Envelope ack{};
    Envelope finalAck{};
    bool finalAckPending=false;
};
static SRWLOCK gLock = SRWLOCK_INIT;
static NetworkState gNetwork;
static uint32_t NativeClock(){return GetTickCount();}
static uint32_t (*gNetworkClock)()=NativeClock;
struct GameState {
    Identity identity{};
    uintptr_t sourceArray = 0;
    bool prepared = false, staged = false, committed = false, holdLogged = false, replay=false, host=false;
    coop::Endpoint peer{};
    uint8_t trafficCount = 0;
    Manifest manifest{};
    coop_ai_roster::Plan plan{};
    alignas(16) uint8_t backups[5][0x940]{};
    alignas(16) uint8_t stagedAi[4][0x940]{};
    alignas(16) uint8_t traffic[20][0x940]{};
    // Local read-only proof captured AFTER Fleet insertion and native Stage7.
    // Pre-stream manifests are reconstruction recipes, not loaded parameters:
    // native copy/parts/variant setup may legally normalize their bytes.
    bool restartSealed=false;
    bool restartArmed=false;
    Identity authorizedRestart{};
    uint32_t finalizedCount=0;
    uint8_t finalizedTailIds[20]{}, finalizedTailRemote[20]{};
    alignas(16) uint8_t finalizedAi[4][0x940]{};
    alignas(16) uint8_t authorizedAi[4][0x940]{};
};
static GameState gGame;
static bool Hold(const char* message) {
    if (!gGame.holdLogged) { LogMessage(message); gGame.holdLogged = true; }
    return false;
}
static bool RestartRefused(const char* reason,uint32_t slot=0,uint32_t offset=0,
    uint32_t expected=0,uint32_t actual=0) {
    if(!gGame.holdLogged) {
        char line[240]{};
        std::snprintf(line,sizeof(line),
            "[AI_RESTART_REFUSED] reason=%s parent=%u prior=%u slot=%u offset=%04X expected=%08X actual=%08X; streaming held.\r\n",
            reason,gPendingRaceEvent.parentEpoch,gGame.identity.token,slot,offset,expected,actual);
        LogMessage(line);gGame.holdLogged=true;
    }
    return false;
}
static bool CaptureCanonical(uintptr_t parameter,uint8_t* portable) {
    uint8_t probe[0x940]{};
    if(!ReadGameMemory(parameter,probe,sizeof(probe))) return false;
    for(unsigned slot=0;slot<170;++slot) {
        uintptr_t part=0;uint32_t hash=0;
        std::memcpy(&part,probe+0x10+0x590+slot*4,4);
        if(part && (!IsValidUserPtr(part) || !ReadGameMemory(part,&hash,4) || !hash)) return false;
    }
    // Serialization resolves local part pointers to hashes and omits native
    // resource/spline handles. The optional appearance cache is also local.
    SerializeVehicleParamPortable(portable,reinterpret_cast<const uint8_t*>(parameter));
    std::memset(portable+0x3D0,0,4);
    return true;
}
static bool CaptureRawCanonical(uintptr_t parameter,uint8_t* output) {
    uint8_t native[0x940]{};
    if(!ReadGameMemory(parameter,native,sizeof(native))) return false;
    // Exact same POD ranges as portable serialization, but part addresses
    // remain uninterpreted local identity bytes. Stage3 must never resolve a
    // part/cache allocation that native teardown might already have released.
    std::memset(output,0,0x940);
    std::memcpy(output+0x10,native+0x10,0x38C);
    std::memcpy(output+0x10+0x394,native+0x10+0x394,0x1E8);
    std::memcpy(output+0x10+0x580,native+0x10+0x580,4);
    std::memcpy(output+0x10+0x590,native+0x10+0x590,170*4);
    std::memcpy(output+0x10+0x838,native+0x10+0x838,0xAA);
    std::memset(output+0x3D0,0,4);
    return true;
}
static bool EqualBytes(const uint8_t* actual,const uint8_t* expected,size_t size,
    const char* reason,uint32_t slot,uint32_t base=0) {
    for(size_t offset=0;offset<size;++offset)
        if(actual[offset]!=expected[offset])
            return RestartRefused(reason,slot,base+static_cast<uint32_t>(offset),expected[offset],actual[offset]);
    return true;
}
static bool EqualLoadedRecipe(const uint8_t* actual,const uint8_t* staged,uint32_t slot) {
    // PARAMETER offsets, not RideInfo offsets. Original 601110 -> 639CE0 ->
    // 5B4FC0/5B4DC0 regenerates installed-upgrade performance outputs.
    // 5AC4F0 writes +50..2FF and derives ten upgrade levels at +300..327.
    // 5B4780 rebuilds ModelAgg outputs/counter +3D4..47B, not its input
    // sliders +3A8..3CF. 5B5AB0/4133D0 rebuild +47C..58F spline data.
    // +2E0 and +450 therefore are derived, not immutable visual bytes.
    uint32_t modelKey=0,performanceModel=0;
    std::memcpy(&modelKey,actual+0x394,4);
    std::memcpy(&performanceModel,actual+0x20,4);
    if(modelKey!=performanceModel)
        return RestartRefused("seal-ai-model-key",slot,0x394,performanceModel,modelKey);
    for(uint32_t offset=0;offset<0x940;++offset) {
        if(actual[offset]==staged[offset]) continue;
        // 610270 establishes loaded variant 1. No unrelated header byte is
        // authorized just because this one local normalization is legal.
        if(offset==0x17 && actual[offset]==1) continue;
        // Native grade and aggregate performance metrics: 5B4DC0,5B06A0,
        // 5B5AB0. Independently verify the repaired model key (59B060) above.
        if(offset==0x24 && actual[offset]<=3) continue;
        if((offset>=0x28 && offset<0x44) || (offset>=0x50 && offset<0x328) ||
            (offset>=0x394 && offset<0x398) || (offset>=0x3A4 && offset<0x3A8) ||
            (offset>=0x3D4 && offset<0x590)) continue;
        // 61BCD0 derives part style from part+5 >>5 and writes 170 boolean
        // visibility outputs. Installed part HASHES still match exactly.
        if(offset==0x591 && actual[offset]<=7) continue;
        if(offset>=0x848 && offset<0x8F2 && actual[offset]<=1) continue;
        return RestartRefused("seal-ai-recipe",slot,offset,staged[offset],actual[offset]);
    }
    return true;
}
static Identity CurrentIdentity(const coop::SessionSnapshot& session) {
    // A lost heartbeat stops network transfer, not the ownership of already
    // committed replicas. Otherwise the client could resume its own AI race.
    if (!IsCoopGameplayActive() || !session.joined || !session.peer.Valid() ||
        (session.role != coop::Role::Host && session.role != coop::Role::Client) ||
        gPendingRaceEvent.sessionId != session.sessionId ||
        (gCoopRaceState != COOP_RACE_STARTING && gCoopRaceState != COOP_RACE_HOST_WAITING_ACCEPT &&
         gCoopRaceState != COOP_RACE_CLIENT_INVITED && gCoopRaceState != COOP_RACE_IN_PROGRESS)) return {};
    return {session.sessionId, gPendingRaceEvent.seed, gPendingRaceEvent.trackId, gPendingRaceEvent.careerEventHash};
}
static void PublishScope(const coop::SessionSnapshot& session, const Identity& identity) {
    AcquireSRWLockExclusive(&gLock);
    if (!Same(gNetwork.identity, identity) || gNetwork.peer != session.peer ||
        gNetwork.host != (session.role == coop::Role::Host)) {
        gNetwork = NetworkState{};
        gNetwork.identity = identity; gNetwork.peer = session.peer;
        gNetwork.host = session.role == coop::Role::Host;
    }
    ReleaseSRWLockExclusive(&gLock);
}
static Envelope Wrap(const coop_checkpoint::Packet& block, const Identity& identity, bool host) {
    Envelope packet; packet.identity = identity; packet.senderPlayer = host ? 1 : 2; packet.block = block;
    return packet;
}
static bool ReadArray(uintptr_t& array, uint32_t& count) {
    return ReadGameMemory(gAddresses.array, &array, sizeof(array)) && IsValidUserPtr(array) &&
        ReadGameMemory(gAddresses.count, &count, 4) && count >= 1 && count <= 22;
}
static bool ReadHuman(uintptr_t parameter, bool remote) {
    uint32_t type = 0; uint8_t flag = 0, index = 0;
    return ReadGameMemory(parameter + 4, &type, 4) && type == 1 &&
        ReadGameMemory(parameter + 0x900, &flag, 1) && flag == (remote ? 1 : 0) &&
        ReadGameMemory(parameter + 1, &index, 1) && index == (remote ? 1 : 0);
}
static bool CaptureTraffic(uintptr_t array, uint32_t count, uint32_t raceCount, bool allowNativeAi = true, bool replay=false) {
    size_t traffic = 0;
    for (uint32_t index = 1; index < count; ++index) {
        uint32_t type = 0; const uintptr_t source = array + index * 0x940;
        if (!ReadGameMemory(source + 4, &type, 4)) return false;
        if(replay && index==1 && type==1 && ReadHuman(source,true)) continue;
        if (type == 2) { if (!allowNativeAi) return false; continue; }
        if (type != 3 || traffic >= 20 || raceCount + traffic + 1 > 22) return false;
        uint8_t probe[0x940]{};
        if (!ReadGameMemory(source, probe, sizeof(probe))) return false;
        gApi.ctor(gGame.traffic[traffic] + 0x10);
        uint32_t localCache=0;
        if(replay) std::memcpy(&localCache,gGame.traffic[traffic]+0x3D0,4);
        gApi.copy(gGame.traffic[traffic], reinterpret_cast<const uint8_t*>(source));
        if(replay) {
            // Native copy includes RideInfo's streaming cache. Restart may
            // already have released that old allocation: retain this new
            // constructor's cache, not the previous load's handle.
            const uint32_t clear=0;
            std::memcpy(gGame.traffic[traffic]+0x3D0,&localCache,4);
            std::memcpy(gGame.traffic[traffic]+0x8F8,&clear,4);
        }
        ++traffic;
    }
    gGame.trafficCount = static_cast<uint8_t>(traffic);
    return true;
}
static bool CaptureHost(const Identity& identity) {
    uintptr_t array = 0; uint32_t count = 0, eventHash = 0;
    const uintptr_t event = gApi.event(identity.eventHash);
    int8_t opponentCount = 0;
    if (!ReadArray(array, count) || !ReadHuman(array, false) || !IsValidUserPtr(event) ||
        !ReadGameMemory(event + 8, &eventHash, 4) || eventHash != identity.eventHash ||
        !ReadGameMemory(event + 0x7C, &opponentCount, 1) || opponentCount < 0 || opponentCount > 5 ||
        count < static_cast<uint32_t>(opponentCount + 1)) return false;
    Manifest manifest; manifest.identity = identity; manifest.nativeCapacity = static_cast<uint8_t>(opponentCount + 1);
    manifest.opponentCount = static_cast<uint8_t>(opponentCount);
    size_t i = 0;
    for (uint32_t index = 1; index < count; ++index) {
        const uintptr_t source = array + index * 0x940;
        uint8_t native[0x940]{};
        if (!ReadGameMemory(source, native, sizeof(native))) return false;
        uint32_t type = 0, model = 0;
        std::memcpy(&type, native + 4, 4); std::memcpy(&model, native + 0x10, 4);
        if (type == 3) continue;
        if (i >= manifest.opponentCount) return false;
        if (type != 2 || native[0x900] || !IsSupportedCarModel(model)) return false;
        auto& opponent = manifest.opponents[i];
        opponent.sourceSlot = static_cast<uint8_t>(i + 1); opponent.playerIndex = native[1];
        opponent.flags = native[2]; opponent.aiSkill = native[0x901]; opponent.aiStyle = native[0x902];
        if (!ReadGameMemory(event + 0x40 + i * 0x0C, opponent.descriptor, sizeof(opponent.descriptor))) return false;
        std::memcpy(opponent.name, native + 0x90C, sizeof(opponent.name));
        SerializeVehicleParamPortable(opponent.portable, reinterpret_cast<const uint8_t*>(source));
        // Never send the appearance adapter's optional process-local cache.
        std::memset(opponent.portable + 0x10 + 0x3C0, 0, 4);
        gApi.ctor(gGame.backups[i] + 0x10);
        gApi.copy(gGame.backups[i], reinterpret_cast<const uint8_t*>(source));
        ++i;
    }
    if (i != manifest.opponentCount) return false;
    const auto plan = ManifestPlan(manifest);
    manifest.effectiveCapacity = plan.totalCount; manifest.aiCount = plan.aiCount;
    if (!ValidManifest(manifest, identity) || !CaptureTraffic(array, count, plan.totalCount, !plan.extraHuman)) return false;
    gGame.manifest = manifest; gGame.plan = plan; gGame.sourceArray = array;
    return true;
}
static bool Stage(const Manifest& manifest, bool host, bool replay=false) {
    uintptr_t array = 0; uint32_t count = 0;
    if (!ReadArray(array, count) || !ReadHuman(array, false)) return false;
    const uintptr_t event = gApi.event(manifest.identity.eventHash);
    uint32_t eventHash = 0; int8_t expected = 0;
    if (!IsValidUserPtr(event) || !ReadGameMemory(event + 8, &eventHash, 4) || eventHash != manifest.identity.eventHash ||
        !ReadGameMemory(event + 0x7C, &expected, 1) || expected != manifest.opponentCount) return false;
    for (size_t i = 0; i < manifest.opponentCount; ++i) {
        uint8_t descriptor[0x0C]{};
        if (!ReadGameMemory(event + 0x40 + i * 0x0C, descriptor, sizeof(descriptor)) ||
            std::memcmp(descriptor, manifest.opponents[i].descriptor, sizeof(descriptor))) return false;
    }
    const auto plan = ManifestPlan(manifest);
    if ((!host || replay) && !CaptureTraffic(array, count, plan.totalCount, !plan.extraHuman, replay)) return false;
    for (size_t i = 0; i < plan.aiCount; ++i) {
        const auto& opponent = manifest.opponents[plan.sourceIndices[i]];
        uint32_t model = 0;
        std::memcpy(&model, opponent.portable + 0x10, 4);
        if (!IsSupportedCarModel(model)) return false;
        auto* output = gGame.stagedAi[i]; auto* ride = output + 0x10;
        std::memset(output, 0, 0x940); gApi.ctor(ride);
        if (host && !replay) gApi.copy(output, gGame.backups[plan.sourceIndices[i]]);
        else {
            // A finalized opponent is not a player-car variant. The native
            // constructor/init already provide this ride's local tuning and
            // spline members; copying human 0 first also imports any player
            // appearance/cache fields outside the portable ranges.
            gApi.init(ride, model); gApi.stock(ride);
            const uint8_t* input = opponent.portable + 0x10;
            uint8_t cache[4]{}; std::memcpy(cache, ride + 0x3C0, 4);
            std::memcpy(ride, input, 0x38C);
            std::memcpy(ride + 0x394, input + 0x394, 0x1E8);
            std::memcpy(ride + 0x3C0, cache, 4);
            std::memcpy(ride + 0x580, input + 0x580, 4);
            std::memcpy(ride + 0x838, input + 0x838, 0xAA);
            for (uint32_t slot = 0; slot < 170; ++slot) {
                uint32_t hash = 0; std::memcpy(&hash, input + 0x590 + slot * 4, 4);
                // This is a complete native roster, not a partial customization
                // packet. An absent host part must remain absent; retaining a
                // stock/default part changes presets and can alias player mods.
                if (!hash) {
                    const uint32_t empty = 0;
                    std::memcpy(ride + 0x590 + slot * 4, &empty, 4);
                    continue;
                }
                void* part = gApi.part(model, slot, hash); uint32_t actual = 0;
                if (!part || !ReadGameMemory(reinterpret_cast<uintptr_t>(part), &actual, 4) || actual != hash) return false;
                std::memcpy(ride + 0x590 + slot * 4, &part, 4);
            }
            gApi.parts(ride);
        }
        output[0] = 0; output[1] = opponent.playerIndex; output[2] = opponent.flags; output[3] = plan.grids[i];
        const uint32_t type = 2, zero = 0;
        std::memcpy(output + 4, &type, 4); std::memcpy(output + 0x8F8, &zero, 4);
        output[0x900] = host ? 0 : 1; output[0x901] = opponent.aiSkill; output[0x902] = opponent.aiStyle;
        std::memcpy(output + 0x904, &zero, 4); std::memcpy(output + 0x908, &model, 4);
        std::memcpy(output + 0x90C, opponent.name, 16); output[0x91C] = 0;
        gApi.repair(output);
        // The peer human occupies texture slot 2; each AI gets the same 3..6
        // texture identity in both processes without changing native driver IDs.
        gApi.texture(ride, plan.grids[i]);
    }
    gGame.plan = plan; gGame.sourceArray = array; gGame.staged = true;
    return true;
}
static bool AIHeader(uintptr_t parameter,size_t selected) {
    const auto& opponent=gGame.manifest.opponents[gGame.plan.sourceIndices[selected]];
    uint32_t type=0,model=0,expectedModel=0;uint8_t id=0,remote=0,metadata[3]{},name[16]{};
    std::memcpy(&expectedModel,opponent.portable+0x10,4);
    const uint32_t slot=static_cast<uint32_t>(selected+2);
    if(!ReadGameMemory(parameter+4,&type,4) || type!=2)
        return RestartRefused("ai-type",slot,4,2,type);
    if(!ReadGameMemory(parameter+1,&id,1) || id!=opponent.playerIndex)
        return RestartRefused("ai-driver",slot,1,opponent.playerIndex,id);
    if(!ReadGameMemory(parameter+0x900,metadata,3)) return RestartRefused("ai-metadata-read",slot);
    remote=metadata[0];
    if(remote!=(gGame.host?0:1)) return RestartRefused("ai-authority",slot,0x900,gGame.host?0:1,remote);
    if(metadata[1]!=opponent.aiSkill || metadata[2]!=opponent.aiStyle)
        return RestartRefused("ai-skill-style",slot,0x901,opponent.aiSkill,metadata[1]);
    if(!ReadGameMemory(parameter+0x10,&model,4) || model!=expectedModel)
        return RestartRefused("ai-model",slot,0x10,expectedModel,model);
    if(!ReadGameMemory(parameter+0x90C,name,sizeof(name))) return RestartRefused("ai-name-read",slot);
    return EqualBytes(name,reinterpret_cast<const uint8_t*>(opponent.name),sizeof(name),"ai-name",slot,0x90C);
}
static bool RetainedList(const Identity& identity,const coop::SessionSnapshot& session,uintptr_t& array) {
    if(!gGame.committed || !gGame.restartSealed) return RestartRefused("unsealed-final-roster");
    if(gGame.identity.token!=gPendingRaceEvent.parentEpoch)
        return RestartRefused("parent-epoch",0,0,gGame.identity.token,gPendingRaceEvent.parentEpoch);
    if(gGame.identity.session!=identity.session ||
        gGame.identity.eventHash!=identity.eventHash || gGame.host!=(session.role==coop::Role::Host) ||
        gGame.peer!=session.peer || !ValidManifest(gGame.manifest,gGame.identity)) return RestartRefused("session-track-event-peer");
    // A URL next heat keeps this exact finalized two-human/AI recipe but
    // changes route. Only the lifecycle's agreed adjacent-heat proof can
    // authorize that one track difference; ordinary Restart stays same-track.
    if(gGame.identity.track!=identity.track && !CoopUrlRetainedTransition(
        gGame.identity.track,identity.track,gGame.identity.token,identity.token,identity.eventHash))
        return RestartRefused("unapproved-route-transition");
    uint32_t count=0;
    if(!ReadArray(array,count) || array!=gGame.sourceArray) return RestartRefused("parameter-array");
    // Fleet's Stage4 commit replaces AI's provisional traffic tail. The proof
    // is sealed after that final commit, not using AI's old trafficCount.
    if(count!=gGame.finalizedCount) return RestartRefused("final-parameter-count",0,0,gGame.finalizedCount,count);
    if(!ReadHuman(array,false) || !ReadHuman(array+0x940,true)) return RestartRefused("human-identity");
    const uintptr_t event=gApi.event(identity.eventHash);uint32_t hash=0;int8_t opponents=0;
    if(!IsValidUserPtr(event) || !ReadGameMemory(event+8,&hash,4) || hash!=identity.eventHash ||
        !ReadGameMemory(event+0x7C,&opponents,1) || opponents!=gGame.manifest.opponentCount)
        return RestartRefused("native-event");
    for(size_t i=0;i<gGame.manifest.opponentCount;++i) {
        uint8_t descriptor[0xC]{};
        if(!ReadGameMemory(event+0x40+i*0xC,descriptor,sizeof(descriptor))) return RestartRefused("event-descriptor-read");
        if(!EqualBytes(descriptor,gGame.manifest.opponents[i].descriptor,sizeof(descriptor),
            "event-descriptor",static_cast<uint32_t>(i+1),0x40+static_cast<uint32_t>(i*0xC))) return false;
    }
    for(size_t i=0;i<gGame.plan.aiCount;++i) if(!AIHeader(array+(i+2)*0x940,i)) return false;
    const auto tail=count-gGame.plan.totalCount;
    for(uint32_t i=0;i<tail;++i) {
        const auto parameter=array+(gGame.plan.totalCount+i)*0x940;
        uint32_t type=0;uint8_t id=0,remote=0;
        if(!ReadGameMemory(parameter+4,&type,4) || type!=3 || !ReadGameMemory(parameter+1,&id,1) ||
            id!=gGame.finalizedTailIds[i] || !ReadGameMemory(parameter+0x900,&remote,1) || remote!=gGame.finalizedTailRemote[i])
            return RestartRefused("final-traffic-identity",gGame.plan.totalCount+i,1,gGame.finalizedTailIds[i],id);
    }
    return true;
}
static bool RebindRestart(const Identity& identity,const coop::SessionSnapshot& session) {
    // Approval validated part HASHES while the old world was still alive.
    // Native5322E0(false) preserves these parameters across Restart/message9.
    // At Stage3 inspect only owned local bytes, never dereference released
    // streaming/part handles; new staging resolves the pointer-free recipe.
    uintptr_t array=0;
    if(!gGame.restartArmed || !Same(gGame.authorizedRestart,identity)) return RestartRefused("restart-not-authorized");
    if(!RetainedList(identity,session,array)) return false;
    for(size_t i=0;i<gGame.plan.aiCount;++i) {
        uint8_t raw[0x940]{};
        if(!CaptureRawCanonical(array+(i+2)*0x940,raw)) return RestartRefused("retained-ai-read",static_cast<uint32_t>(i+2));
        if(!EqualBytes(raw,gGame.authorizedAi[i],sizeof(raw),"retained-ai-bytes",static_cast<uint32_t>(i+2))) return false;
    }
    gGame.identity=identity;gGame.manifest.identity=identity;
    gGame.prepared=gGame.staged=gGame.committed=gGame.holdLogged=false;gGame.replay=true;
    gGame.restartSealed=gGame.restartArmed=false;
    LogMessage("[AI_ROSTER] Restart finalized roster proof verified; fresh epoch/pointers will be restaged.\r\n");
    return true;
}
static void QueueAck(const Envelope& packet) {
    if(packet.block.index!=coop_checkpoint::kComplete && gNetwork.ackPending &&
        gNetwork.ack.block.index==coop_checkpoint::kComplete) {
        // Committed Receiver also ACKs reordered Data. Preserve the final
        // proof while delivering the prefix first, never erase completion.
        gNetwork.finalAck=gNetwork.ack;gNetwork.finalAckPending=true;
        gNetwork.ack=packet;
    } else if(packet.block.index==coop_checkpoint::kComplete && gNetwork.ackPending &&
        gNetwork.ack.block.index!=coop_checkpoint::kComplete) {
        gNetwork.finalAck=packet;gNetwork.finalAckPending=true;
    } else if(packet.block.index!=coop_checkpoint::kComplete && gNetwork.ackPending &&
        gNetwork.ack.block.index!=coop_checkpoint::kComplete && packet.block.index<gNetwork.ack.block.index) {
        // A delayed duplicate may not rewind the contiguous-prefix ACK.
    } else {gNetwork.ack=packet;gNetwork.ackPending=true;}
}
} // namespace CoopAIRosterNative

void TickCoopAIRosterNetwork(DWORD now) {
    using namespace CoopAIRosterNative;
    const auto session = CoopSnapshot();
    std::array<Envelope,coop_fleet_transfer::kWindow+2> packets{};
    coop::Endpoint peer{};size_t outgoing=0;
    AcquireSRWLockExclusive(&gLock);
    // UI may publish a transfer after the outer network loop sampled time.
    // Sample after ownership, otherwise unsigned age expires it immediately.
    now=gNetworkClock();
    const Identity identity = gNetwork.identity;
    if (Valid(identity) && session.joined && session.fresh && session.sessionId == identity.session &&
        session.peer == gNetwork.peer && session.role == (gNetwork.host ? coop::Role::Host : coop::Role::Client)) {
        peer = gNetwork.peer;
        if (gNetwork.ackPending) { packets[outgoing++]=gNetwork.ack;gNetwork.ackPending=false; }
        if (gNetwork.finalAckPending) {packets[outgoing++]=gNetwork.finalAck;gNetwork.finalAckPending=false;}
        if (gNetwork.host) {
            coop_checkpoint::Packet block;
            while(outgoing<packets.size() && gNetwork.sender.Tick(now,block)) packets[outgoing++]=Wrap(block,identity,true);
        }
    }
    ReleaseSRWLockExclusive(&gLock);
    for(size_t i=0;i<outgoing;++i) gApi.send(packets[i],peer);
}

bool ReceiveCoopAIRosterPacket(const char* data, int bytes, const sockaddr_in& sender, DWORD now) {
    using namespace CoopAIRosterNative;
    uint32_t magic = 0;
    if (!data || bytes < 4) return false;
    std::memcpy(&magic, data, 4); if (magic != kMagic) return false;
    if (bytes != sizeof(Envelope)) return true;
    Envelope packet; std::memcpy(&packet, data, sizeof(packet));
    const auto session = CoopSnapshot();
    const coop::Endpoint endpoint{sender.sin_addr.s_addr, ntohs(sender.sin_port)};
    if (sender.sin_family != AF_INET || !session.joined || !session.fresh || endpoint != session.peer ||
        packet.version != 1 || packet.size != sizeof(packet) || packet.identity.session != session.sessionId ||
        !Valid(packet.identity) || !coop_checkpoint::Valid(packet.block) ||
        packet.block.session != packet.identity.session || packet.block.ticket != packet.identity.token ||
        packet.block.transfer != packet.identity.token || packet.block.launchKind != packet.identity.eventHash ||
        packet.block.total != sizeof(Manifest) || std::strcmp(packet.block.name, "ai-roster") != 0) return true;
    AcquireSRWLockExclusive(&gLock);
    if (Same(packet.identity, gNetwork.identity) && endpoint == gNetwork.peer &&
        packet.senderPlayer == (gNetwork.host ? 2 : 1)) {
        if (gNetwork.host && packet.block.type == coop_checkpoint::Type::Ack) {
            if (packet.block.index == kRejected && packet.block.digest == gNetwork.digest && gNetwork.sender.Active())
                gNetwork.sender.Reset();
            else gNetwork.sender.Ack(packet.block, now);
        }
        else if (!gNetwork.host && packet.block.type != coop_checkpoint::Type::Ack) {
            coop_checkpoint::Packet ack;
            if (gNetwork.receiver.Receive(packet.block, ack)) {
                QueueAck(Wrap(ack,gNetwork.identity,false));
            }
        }
    }
    ReleaseSRWLockExclusive(&gLock);
    return true;
}

bool PrepareCoopAIRosterBeforeStreaming() {
    using namespace CoopAIRosterNative;
    uint32_t track = 0;
    if (!IsCoopGameplayActive()) return true;
    if (!ReadGameMemory(gAddresses.track, &track, 4)) return false;
    if (track == 4000) return true;
    // Drift now shares the validated native recipe/seat budget. Its separate
    // score bank is transported authoritatively, not inferred from lap progress.
    const auto session = CoopSnapshot(); const Identity identity = CurrentIdentity(session);
    if (!Valid(identity) || track != identity.track) return Hold("[AI_ROSTER] Event identity unavailable; streaming held.\r\n");
    PublishScope(session, identity);
    if (!Same(gGame.identity, identity)) {
        const bool retained=gPendingRaceEvent.parentEpoch && RebindRestart(identity,session);
        if(gPendingRaceEvent.parentEpoch && !retained)
            return Hold("[AI_ROSTER] Restart prior roster/epoch mismatch; streaming held.\r\n");
        if(!retained) {gGame=GameState{};gGame.identity=identity;}
        gGame.host=session.role==coop::Role::Host;gGame.peer=session.peer;
    }
    if (!VerifyNative()) return Hold("[AI_ROSTER] Native signature mismatch; streaming held.\r\n");
    const bool host = session.role == coop::Role::Host;
    if (host) {
        if (!gGame.prepared) {
            if ((!gGame.replay && !CaptureHost(identity)) || !Stage(gGame.manifest,true,gGame.replay))
                return Hold("[AI_ROSTER] Native event roster unsupported; streaming held.\r\n");
            std::vector<uint8_t> file(sizeof(Manifest)); std::memcpy(file.data(), &gGame.manifest, file.size());
            AcquireSRWLockExclusive(&gLock);
            const bool started = Same(gNetwork.identity, identity) && gNetwork.sender.Start(identity.session,
                identity.token, identity.token, "ai-roster", file, GetTickCount(), identity.eventHash);
            if (started) gNetwork.digest = coop_checkpoint::Digest(file);
            ReleaseSRWLockExclusive(&gLock);
            if (!started) return false;
            gGame.prepared = true;
            LogMessage("[AI_ROSTER] Finalized native host AI manifest published.\r\n");
        }
        AcquireSRWLockExclusive(&gLock);
        if (!gNetwork.sender.Active() && !gNetwork.sender.Done()) {
            std::vector<uint8_t> file(sizeof(Manifest)); std::memcpy(file.data(), &gGame.manifest, file.size());
            gNetwork.sender.Start(identity.session, identity.token, identity.token, "ai-roster", file,
                GetTickCount(), identity.eventHash);
            gNetwork.digest = coop_checkpoint::Digest(file);
        }
        const bool done = gNetwork.sender.Done(); ReleaseSRWLockExclusive(&gLock);
        return done;
    }
    if (gGame.prepared) return true;
    coop_checkpoint::Completed completed;
    AcquireSRWLockExclusive(&gLock); const bool received = gNetwork.receiver.Take(completed); ReleaseSRWLockExclusive(&gLock);
    if (!received) return false;
    Manifest manifest;
    if (completed.file.size() != sizeof(manifest)) return false;
    std::memcpy(&manifest, completed.file.data(), sizeof(manifest));
    const bool valid = ValidManifest(manifest, identity) && Stage(manifest,false,gGame.replay);
    AcquireSRWLockExclusive(&gLock);
    coop_checkpoint::Packet ack;
    if (Same(gNetwork.identity, identity) && gNetwork.receiver.Commit(valid, ack)) {
        QueueAck(Wrap(ack,identity,false));gNetwork.available=true;
    } else if (!valid && Same(gNetwork.identity, identity)) {
        // Restart the complete transfer after a validation/asset failure. The
        // generic sender otherwise remains at Commit while Receiver waits at 0.
        coop_checkpoint::Packet rejected;
        rejected.session = identity.session; rejected.transfer = identity.token; rejected.ticket = identity.token;
        rejected.total = sizeof(Manifest); rejected.count = (sizeof(Manifest) + coop_checkpoint::kBlock - 1) / coop_checkpoint::kBlock;
        rejected.launchKind = identity.eventHash; rejected.type = coop_checkpoint::Type::Ack; rejected.index = kRejected;
        rejected.digest = coop_checkpoint::Digest(completed.file);
        std::memcpy(rejected.name, "ai-roster", sizeof("ai-roster"));
        gNetwork.ack = Wrap(rejected, identity, false); gNetwork.ackPending = true;
    }
    ReleaseSRWLockExclusive(&gLock);
    if (!valid) return Hold("[AI_ROSTER] Host manifest or native AI parts unavailable; streaming held.\r\n");
    gGame.manifest = manifest; gGame.prepared = true;
    LogMessage("[AI_ROSTER] Host AI identities, models and parts validated locally.\r\n");
    return true;
}

bool SealCoopAIRosterAfterStreaming() {
    using namespace CoopAIRosterNative;
    if(!gCoopAIEnabled || !IsCoopGameplayActive()) return true;
    uint32_t track=0;
    if(!ReadGameMemory(gAddresses.track,&track,4)) return false;
    if(track==4000) return true;
    const auto session=CoopSnapshot();const auto identity=CurrentIdentity(session);
    uintptr_t array=0;uint32_t count=0;
    if(!Valid(identity) || !Same(identity,gGame.identity) || !gGame.committed ||
        !ReadArray(array,count) || array!=gGame.sourceArray || !ReadHuman(array,false) || !ReadHuman(array+0x940,true))
        return RestartRefused("seal-committed-roster");
    const uint32_t expectedCount=gCoopFleetEnabled?CoopFleetCommittedParameterCount():gGame.plan.totalCount+gGame.trafficCount;
    if(count!=expectedCount || count<gGame.plan.totalCount || count-gGame.plan.totalCount>20)
        return RestartRefused("seal-final-count",0,0,expectedCount,count);
    alignas(16) uint8_t finalized[4][0x940]{};
    uint8_t tailIds[20]{},tailRemote[20]{};
    for(size_t i=0;i<gGame.plan.aiCount;++i) {
        const auto parameter=array+(i+2)*0x940;uint8_t recipe[0x940]{};
        if(!AIHeader(parameter,i) || !CaptureCanonical(parameter,finalized[i]) ||
            !CaptureCanonical(reinterpret_cast<uintptr_t>(gGame.stagedAi[i]),recipe))
            return RestartRefused("seal-ai-parts-read",static_cast<uint32_t>(i+2));
        // Native Stage7 may set the loaded variant and normalize derived
        // visibility/tuning. It may NOT substitute another car or installed
        // parts: verify every finalized part hash against this staged recipe.
        if(!EqualBytes(finalized[i]+0x10+0x590,recipe+0x10+0x590,170*4,
            "seal-ai-part-hashes",static_cast<uint32_t>(i+2),0x10+0x590)) return false;
        if(!EqualLoadedRecipe(finalized[i],recipe,static_cast<uint32_t>(i+2))) return false;
    }
    coop_world::Identity fleetIdentity{};
    if(gCoopFleetEnabled && !CurrentCoopFleetIdentity(fleetIdentity)) return RestartRefused("seal-fleet-scope");
    for(uint32_t i=0;i<count-gGame.plan.totalCount;++i) {
        const auto parameter=array+(gGame.plan.totalCount+i)*0x940;uint32_t type=0,kind=0;uint8_t reserved=0;
        if(!ReadGameMemory(parameter+4,&type,4) || type!=3 || !ReadGameMemory(parameter+1,&tailIds[i],1) ||
            !ReadGameMemory(parameter+0x900,&tailRemote[i],1)) return RestartRefused("seal-traffic-identity",gGame.plan.totalCount+i);
        if(gCoopFleetEnabled && (!ReadCoopFleetReservation(fleetIdentity,parameter,kind,reserved) ||
            kind!=static_cast<uint32_t>(coop_world::Kind::Traffic) || reserved!=tailIds[i]))
            return RestartRefused("seal-fleet-reservation",gGame.plan.totalCount+i);
        for(uint32_t j=0;j<i;++j) if(tailIds[j]==tailIds[i]) return RestartRefused("seal-duplicate-traffic",gGame.plan.totalCount+i);
    }
    std::memcpy(gGame.finalizedAi,finalized,sizeof(finalized));
    std::memcpy(gGame.finalizedTailIds,tailIds,sizeof(tailIds));
    std::memcpy(gGame.finalizedTailRemote,tailRemote,sizeof(tailRemote));
    gGame.finalizedCount=count;gGame.restartSealed=true;gGame.restartArmed=false;gGame.holdLogged=false;
    char line[160]{};std::snprintf(line,sizeof(line),
        "[AI_ROSTER] Final post-stream restart proof sealed: token=%u race=%u total=%u fleet=%u.\r\n",
        identity.token,gGame.plan.totalCount,count,gCoopFleetEnabled?1:0);LogMessage(line);
    return true;
}

bool AuthorizeCoopAIRosterRestartBeforeTeardown() {
    using namespace CoopAIRosterNative;
    if(!gCoopAIEnabled || !IsCoopGameplayActive() || !gPendingRaceEvent.parentEpoch ||
        gPendingRaceEvent.trackId==4000) return true;
    const auto session=CoopSnapshot();const auto identity=CurrentIdentity(session);
    gGame.restartArmed=false;
    if(!Valid(identity) || !session.fresh || identity.token==gPendingRaceEvent.parentEpoch)
        return RestartRefused("restart-intent");
    uintptr_t array=0;
    if(!RetainedList(identity,session,array)) return false;
    alignas(16) uint8_t authorized[4][0x940]{};
    for(size_t i=0;i<gGame.plan.aiCount;++i) {
        const auto parameter=array+(i+2)*0x940;uint8_t canonical[0x940]{};
        if(!CaptureCanonical(parameter,canonical) || !CaptureRawCanonical(parameter,authorized[i]))
            return RestartRefused("restart-live-parts-read",static_cast<uint32_t>(i+2));
        if(!EqualBytes(canonical,gGame.finalizedAi[i],sizeof(canonical),
            "restart-finalized-ai",static_cast<uint32_t>(i+2))) return false;
    }
    std::memcpy(gGame.authorizedAi,authorized,sizeof(authorized));
    gGame.authorizedRestart=identity;gGame.restartArmed=true;gGame.holdLogged=false;
    return true;
}

bool CommitCoopAIRosterAfterPeer() {
    using namespace CoopAIRosterNative;
    uint32_t track = 0;
    if (!IsCoopGameplayActive()) return true;
    if (!ReadGameMemory(gAddresses.track, &track, 4)) return false;
    if (track == 4000) return true;
    const auto session = CoopSnapshot(); const Identity identity = CurrentIdentity(session);
    uintptr_t array = 0; uint32_t count = 0;
    if (!Valid(identity) || !Same(identity, gGame.identity) || !gGame.prepared || !gGame.staged ||
        !ReadArray(array, count) || array != gGame.sourceArray || !ReadHuman(array, false) ||
        !ReadHuman(array + 0x940, true) || !VerifyNative())
        return Hold("[AI_ROSTER] Prepared two-human roster unavailable; streaming held.\r\n");
    const uint32_t finalCount = gGame.plan.totalCount + gGame.trafficCount;
    if (gGame.committed) return count == finalCount;
    if (count != 2) return Hold("[AI_ROSTER] Human insertion count invalid; streaming held.\r\n");
    // Preflight every destination before invoking native AddVehicle. Unsupported
    // layouts do not partially modify the live race parameter list.
    uint32_t previousCounters[4]{};
    if (!ReadGameMemory(gAddresses.participants, previousCounters, sizeof(previousCounters))) return false;
    auto restoreHumanList = [&]() {
        const uint32_t humans = 2;
        SafeWriteBytes(gAddresses.count, &humans, sizeof(humans));
        SafeWriteBytes(gAddresses.participants, previousCounters, sizeof(previousCounters));
    };
    uint8_t probe[0x940]{};
    for (size_t i = 0; i < static_cast<size_t>(gGame.plan.aiCount) + gGame.trafficCount; ++i)
        if (!ReadGameMemory(array + (i + 2) * 0x940, probe, sizeof(probe))) return false;
    for (size_t i = 0; i < gGame.plan.aiCount; ++i) {
        gApi.add(gGame.stagedAi[i]);
        uint32_t added = 0;
        if (!ReadGameMemory(gAddresses.count, &added, 4) || added != i + 3) {
            restoreHumanList();
            return Hold("[AI_ROSTER] Native AI insertion failed; streaming held.\r\n");
        }
    }
    for (size_t i = 0; i < gGame.trafficCount; ++i) {
        // Traffic is outside the race grids and additive human/AI categories.
        // Preserve its original local native parameters for the world adapter.
        gApi.add(gGame.traffic[i]);
        uint32_t added = 0;
        if (!ReadGameMemory(gAddresses.count, &added, 4) || added != gGame.plan.totalCount + i + 1) {
            restoreHumanList();
            return Hold("[AI_ROSTER] Native traffic insertion failed; streaming held.\r\n");
        }
    }
    const auto counters = coop_ai_roster::ParticipantCounts(gGame.plan, session.role == coop::Role::Host);
    if (!SafeWriteBytes(gAddresses.participants, counters.data(), sizeof(counters))) {
        restoreHumanList(); return false;
    }
    gGame.committed = true; gGame.holdLogged = false;
    char line[160]; std::snprintf(line, sizeof(line),
        "[AI_ROSTER] Native roster committed: humans=2 AI=%u capacity=%u vanilla=%u special_exception=%u extra_human=%u.\r\n",
        gGame.plan.aiCount, gGame.plan.totalCount, gGame.plan.nativeCapacity,
        gGame.plan.protectedCapacityException ? 1 : 0, gGame.plan.extraHuman ? 1 : 0);
    LogMessage(line);
    return true;
}

uint32_t CoopAIRosterCommittedCount() {
    using namespace CoopAIRosterNative;
    return gGame.committed && Same(CurrentIdentity(CoopSnapshot()), gGame.identity) ?
        gGame.plan.totalCount + gGame.trafficCount : 0;
}

uint32_t CoopAIRosterRaceCount() {
    using namespace CoopAIRosterNative;
    return gGame.committed && Same(CurrentIdentity(CoopSnapshot()), gGame.identity) ? gGame.plan.totalCount : 0;
}

static bool ReadCommittedCoopAIParam(uintptr_t parameter,const CoopAIRosterNative::Identity& identity,
    coop::Role role,uint32_t& ordinal,uint32_t& model) {
    using namespace CoopAIRosterNative;
    uintptr_t array = 0; uint32_t count = 0;
    if (!gGame.committed || !Valid(identity) || !Same(identity, gGame.identity) ||
        !ReadArray(array, count) || array != gGame.sourceArray ||
        count < static_cast<uint32_t>(gGame.plan.totalCount) || count > 22 || parameter < array + 2 * 0x940 ||
        parameter >= array + gGame.plan.totalCount * 0x940 || (parameter - array) % 0x940) return false;
    const size_t selected = (parameter - array) / 0x940 - 2;
    const auto& opponent = gGame.manifest.opponents[gGame.plan.sourceIndices[selected]];
    uint32_t actualType = 0, actualModel = 0, expectedModel = 0;
    uint8_t driver = 0, remote = 0;
    std::memcpy(&expectedModel, opponent.portable + 0x10, 4);
    if (!ReadGameMemory(parameter + 4, &actualType, 4) || actualType != 2 ||
        !ReadGameMemory(parameter + 0x10, &actualModel, 4) || actualModel != expectedModel ||
        !ReadGameMemory(parameter + 1, &driver, 1) || driver != opponent.playerIndex ||
        !ReadGameMemory(parameter + 0x900, &remote, 1) || remote != (role == coop::Role::Host ? 0 : 1)) return false;
    ordinal = opponent.sourceSlot; // Stable original event opponent ordinal 1..5.
    model = actualModel;
    return true;
}

bool ReadCoopAIRosterParam(uintptr_t parameter,uint32_t& ordinal,uint32_t& model) {
    const auto session=CoopSnapshot();
    return ReadCommittedCoopAIParam(parameter,CoopAIRosterNative::CurrentIdentity(session),session.role,ordinal,model);
}

