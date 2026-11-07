// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <psapi.h>
#ifdef small
#undef small
#endif
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <vector>
#include <string>
#include <array>
#include <set>
#include <intrin.h>
#include "CoopSession.h"
#include "CoopNetworkGeneration.h"
#include "CoopNetworkHealth.h"
#include "CoopTime.h"
#include "CoopMotionClock.h"
#include "CoopTransport.h"
#if defined(_MSC_VER) && !defined(UNDERCOOP_CMAKE_TRANSPORT)
#pragma comment(lib,"UndercoopTransport.lib")
#pragma comment(lib,"winmm.lib")
#endif
#include "CoopCarCatalog.h"
#include "CoopModInventory.h"
#include "CoopModScanWin.h"
#include "CoopUnlimiterRideCompat.h"
#include "CoopStockCatalog.h"
#include "CoopProfileContext.h"
#include "CoopCareerHistoryWin.h"
#include "CoopCareerRestoreWin.h"
#include "CoopCrashReporterWin.h"
#include "CoopLobbyText.h"
#include "CoopSaveNamespace.h"
#include "CoopSpecialSaveFence.h"
#include "CoopSpecial.h"
#include "CoopSpecialLifecycle.h"
#include "CoopSpecialRecoveryReceipt.h"
#include "CoopCheckpoint.h"
#include "CoopCareerProgress.h"
#include "CoopRaceLifecycle.h"
#include "CoopUrlTournament.h"
#include "CoopMotion.h"
#include "CoopWorldWire.h"
#include "CoopNetworkPolicy.h"
#include "CoopTrafficRuntime.inl"
#include "CoopStartupIntro.inl"
#include "CoopAnnouncements.inl"
#include "CoopDiscordPublicConfig.h"
#ifdef UNDERCOOP_MOD_MANAGER
#include "CoopModManagerWin.h"
#endif
#include "CoopPeerLabelOverlay.inl"
#include "CoopPerfScopes.h"
namespace { namespace CoopPeerLabelNative { static void Tick(HWND game,bool focused); } }
namespace { namespace CoopRearRenderTrace { bool Install(); void Tick(DWORD flow); } }
namespace { namespace CoopDiscordPresence { static void Observe(DWORD flow,uint32_t track); } }

// ============================================================================
// Target: Need for Speed Underground 2 v1.2 NTSC (SPEED2.EXE, 4,800,512 bytes)
// Milestone 4.0: Dedicated Remote Vehicle Despawn Immunity & Free Roam Co-op
// ============================================================================

constexpr uintptr_t kGameFlowState = 0x8654A4;
constexpr uintptr_t kPlayerPositionRoot = 0x8900B4;
constexpr uintptr_t kCarArray = 0x0089CCF8;
constexpr uintptr_t kVehicleTypeArray = 0x0089CE00;
constexpr int kMaxVehicleSlots = 22;
constexpr uintptr_t kMultiInstancePatch = 0x580E11;

constexpr uintptr_t kUiTickHook = 0x5663F9;
constexpr uintptr_t kAddFngToUiObject = 0x555E80;
constexpr uintptr_t kSetUiLabel = 0x537B80;
constexpr uintptr_t kFindUiElementByHash = 0x5379C0;
constexpr unsigned int kScreenPrintfLabel = 0xB9D45799;

constexpr uint32_t kPacketMagic = 0x5032434E; // "NC2P"
constexpr uint32_t kCarCustomizationMagic = 0x4332434E; // "NC2C"
constexpr uint32_t kRaceEventMagic = 0x5232434E; // "NC2R"
constexpr uint16_t kPacketVersion = 14; // Membership/ticket-bound appearance; connected gameplay transport.
constexpr uint8_t kPeerRenderPlayerIndex = 1;
constexpr int TextureSlotForPlayerIndex(uint8_t playerIndex) { return playerIndex + 1; }

enum RaceEventType : uint16_t {
    RACE_EVENT_NONE = 0,
    RACE_EVENT_PROPOSE = 1,
    RACE_EVENT_ACCEPT = 2,
    RACE_EVENT_DECLINE = 3,
    RACE_EVENT_START = 4,
    RACE_EVENT_ABORT = 5,
    RACE_EVENT_READY_COUNTDOWN = 6,
    RACE_EVENT_TRIGGER_COUNTDOWN = 7,
    RACE_EVENT_REQUEST = 8, // Client asks Host to approve a native Career event/restart.
    RACE_EVENT_CANCEL_ACK = 9,
};

enum CoopSessionMode : uint32_t {
    COOP_SESSION_INACTIVE = 0, // Vanilla Singleplayer (no waiting, no 2nd car, native saves)
    COOP_SESSION_LOBBY = 1,    // In Co-op lobby / searching for hosts
    COOP_SESSION_HOST = 2,     // Hosting a Co-op session
    COOP_SESSION_CLIENT = 3    // Connected to a host as client
};

constexpr uint32_t kBeaconMagic = 0x4232434E; // "NC2B"
constexpr uint32_t kJoinMagic   = 0x4A32434E; // "NC2J"
constexpr uint32_t kAckMagic    = 0x4132434E; // "NC2A"

#pragma pack(push, 1)
struct SessionBeaconPacket {
    uint32_t magic;         // "NC2B"
    uint16_t version;       // kPacketVersion
    uint16_t size;          // sizeof(SessionBeaconPacket)
    uint32_t hostPlayerNum; // 1 or 2
    uint32_t flowState;     // 6 = gameplay, 3 = menu
    uint32_t trackId;       // 4000 = free roam, etc.
    char hostName[32];      // Host profile name or computer name
    char carModel[16];      // e.g. "COROLLA", "240SX"
    uint32_t sentAtMs;
};

struct SessionJoinPacket {
    uint32_t magic;         // "NC2J"
    uint16_t version;
    uint32_t clientPlayerNum;
    char clientName[32];
    char carModel[16];
    uint32_t sentAtMs;
};
#pragma pack(pop)

static std::atomic<CoopSessionMode> gCoopSessionMode{COOP_SESSION_INACTIVE};
static bool gCoopLobbyOpen = false;
static bool gHaveDiscoveredHost = false;
static bool gHaveDiscoveredClient = false;
static SessionBeaconPacket gLatestDiscoveredHost{};
static DWORD gDiscoveredHostReceivedAtMs = 0;
static DWORD gDiscoveredClientReceivedAtMs = 0;
static std::atomic<bool> gTriggerLobbyCareerStart{false};
static char gRemoteProfileName[32]{};
static char gCoopSaveProfileName[64] = "";
// Optional Host campaign selection. Empty retains the existing latest-save
// behavior; an invalid/missing explicit name must never silently start another.
static char gConfiguredCareerProfile[128]{};
static bool gCoopAutoBackup=true;
// Session presence is independent of the career currently loaded in the engine.
// Browsing/hosting must never enable vehicle hooks or redirect a SP save.
static volatile LONG gCoopGameplayActive = 0;
static bool gCoopUiActive = false; // main-thread-only native menu context
static void* gNativeCoopLobby = nullptr;
static std::atomic<bool> gReturnToFreeroam{false};
static std::atomic<bool> gClearNetworkBanner{false};
bool IsCoopGameplayActive() { return InterlockedCompareExchange(&gCoopGameplayActive, 0, 0) != 0; }

enum CoopRaceState {
    COOP_RACE_IDLE = 0,
    COOP_RACE_HOST_WAITING_ACCEPT = 1,
    COOP_RACE_CLIENT_INVITED = 2,
    COOP_RACE_STARTING = 3,
    COOP_RACE_IN_PROGRESS = 4,
    COOP_RACE_REQUESTING = 5,
};

// Function Pointer Types
using AddFngToUiObjectFn = void(__cdecl*)(char*, void*);
using SetUiLabelFn = int(__cdecl*)(char*, unsigned int, char*, int);
using FindUiElementByHashFn = void*(__cdecl*)(char*, unsigned int);
using KeyboardPollFn = int(__thiscall*)(void* device);

constexpr uintptr_t kKeyboardPollHook = 0x005BF860;
constexpr uintptr_t kGameHwndAddr = 0x00870990;

// UI & Feature State
bool gOverlayEnabled = false;
bool gOverlayCreated = false;
bool gRemoteSyncEnabled = true; // Enabled by default: toggled via F9
// World AI is a separate, opt-in experimental rollout until its native pooled
// traffic lifecycle has been verified. Never silently enable partial traffic.
bool gCoopAIEnabled = false;
bool gCoopFleetEnabled = false;
int gSelectedCandidateIndex = 0;
uintptr_t gBoundProxyCar = 0;
static uintptr_t gPeerModelSwapCar = 0; // game-thread, exact authorized rebuild only
float gBoundProxyDist = 0.0f;
bool gIsSecondInstance = false;
bool gReceivedExternalPeer = false;
uintptr_t gVehicleVtable = 0;

// Network Configuration
char gPeerAddress[64] = "127.0.0.1";
unsigned short gLocalPort = 48930;
unsigned short gTargetPort = 48931;
unsigned int gConfiguredInstance = 0; // 0 = automatic; 1/2 = explicit local test role
float gLoopbackOffsetX = 4.0f;
float gLoopbackOffsetY = 0.0f;

char gLogFilePath[MAX_PATH] = {};
char gScriptsDirectory[MAX_PATH] = {}; // ASI directory; never depend on launch working directory

void LogMessage(const char* msg) {
    CoopAnnouncements::FromLog(msg);
    if (gLogFilePath[0] != '\0' && msg != nullptr) {
        HANDLE hLog = CreateFileA(gLogFilePath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hLog != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(hLog, msg, static_cast<DWORD>(std::strlen(msg)), &written, nullptr);
            CloseHandle(hLog);
        }
    }
}

#pragma pack(push, 1)
// Native event 3: CARFX_SKID_SMOKE. Only generated visual effects are shared.
struct SkidEffectSample {
    float intensity = 0.0f;
    uint32_t surface = 0;
    float velocity[3]{};
    uint32_t hasVelocity = 0;
};

struct PositionPacket {
    uint32_t magic;       // "NC2P"
    uint16_t version;     // kPacketVersion
    uint16_t size;        // sizeof(PositionPacket)
    uint32_t sequence;    // Sequence ID
    uint32_t flowState;   // 6 = gameplay
    uint32_t sentAtMs;    // Timestamp
    float x;
    float y;
    float z;
    float speedKmh;       // Speed in km/h
    float headingDeg;     // Heading / yaw angle in degrees (0-360)
    float forwardX;       // Forward vector X
    float forwardY;       // Forward vector Y
    float forwardZ;       // Forward vector Z
    int16_t steerAngle;   // Front wheel steer angle (-32768 to 32767)
    uint8_t brakeLight;   // Brake light state (0 = off, 1 = tail, 2 = brake)
    uint8_t reverseLight; // Reverse light state (0 = off, 1 = reverse on)
    char carModel[16];    // e.g. "COROLLA", "240SX"
    float wheelPhaseRad[4]; // Native Car + 0x4E8: optional spinner phase, radians.
    float wheelSpeedRad[4]; // Native Car + 0x4F8: filtered spinner angular speed.
    float wheelRotation[4][9]; // Ordinary tire/rim 3x3 matrices: Car + 0x130 + wheel*0x40.
    uint32_t wheelRotationValidMask; // Low four bits: invalid/startup wheel caches remain native.
    SkidEffectSample skidEffects[4];
    float totalProgress;      // Racer + 0x20
    float splineDistance;     // Vehicle + 0x8fc (Centerline track distance in meters)
    uint8_t currentLap;       // Racer + 0x08
    uint8_t completedLaps;    // Racer + 0x09
    uint8_t padding[2];
    uint64_t sessionId;
    uint32_t membershipNonce,ticket;
    uint32_t trackId;
    uint32_t worldEpoch; // Per-sender generation; never compare clocks across computers.
    float orientation[4]; // Normalized quaternion XYZW, native row-vector convention.
    float linearVelocity[3];
    float angularVelocity[3];
    float driftScore;         // Own native drift bank, never computed from peer proxy physics.
    uint32_t driftEpoch;      // Zero outside an approved drift event; prevents stale score reuse.
};
static_assert(sizeof(PositionPacket) < 1200, "Motion snapshots must not fragment UDP datagrams");

struct CarCustomizationPacket {
    uint32_t magic;         // "NC2C"
    uint16_t version;       // 5
    uint16_t size;          // sizeof(CarCustomizationPacket) = 824
    uint32_t playerIndex;   // 1 = Host, 2 = Client
    uint64_t sessionId;
    uint32_t membershipNonce,ticket,sequence;
    uint32_t carModelID;    // e.g. 0 = 240SX, 3 = AE86, etc.
    uint8_t  visualData[0x270];   // RideInfo + 0x2E0..0x550 (All visual customization)
    uint8_t  partFlags[0x18];    // RideInfo + 0x838..0x850 (Installed flags)
    uint8_t  visualTuning[0xA0];  // RideInfo + 0x850..0x8EF (Stance, ride height, camber)
};

struct CarSyncPacket {
    uint32_t magic;       // "NC2P"
    uint16_t version;     // kPacketVersion
    uint16_t size;        // sizeof(CarSyncPacket) = 20 + 0x940
    uint32_t playerIndex; // 1 = Host, 2 = Client
    uint32_t sentAtMs;
    uint32_t selectionStage; // 0 = preview, 1 = finalized loading, 2 = gameplay
    uint64_t sessionId;
    uint32_t membershipNonce,ticket,sequence;
    uint8_t vehicleParam[0x940];
};

struct RaceEventPacket {
    uint32_t magic;         // "NC2R" (0x5232434E)
    uint16_t version;       // kRacePacketVersion
    uint16_t eventType;     // RaceEventType
    uint32_t gameMode;      // 0x0083AAB4 (2=Circuit, 3=Sprint, 4=Drag, 5=Drift, 8=URL, etc.)
    uint32_t trackId;       // Explicit Track ID (e.g. 4607)
    uint32_t laps;          // Explicit Lap count (e.g. 4)
    uint32_t direction;     // Explicit Direction (0=Forward, 1=Reverse)
    uint32_t senderPlayer;  // 1=Host, 2=Client
    uint32_t seed;          // Random seed
    uint64_t sessionId;
    uint32_t membershipNonce, ticket; // Exact joined incarnation and career selection.
    uint32_t careerEventHash;
    uint32_t requestId;     // Client nonce echoed by Host, zero for Host initiation.
    uint32_t parentEpoch;   // Zero for a new event; current race epoch for restart.
    uint8_t timedResult;   // Ordinary circuit, sprint, drag or Street X.
    uint8_t nativeContext[0x50]; // Authoritative native WorldEvent, not synthetic mode flags.
    uint8_t  raceParams[0x70]; // 112 bytes from 0x0089E7A0
};
#pragma pack(pop)

constexpr uint16_t kRacePacketVersion = 6;

inline uint32_t CleanGameMode(uint32_t mode) {
    uint32_t m = mode & 0xFFFF;
    if (m == 0) return 0; // Mode 0 = Free Roam
    return (m >= 2 && m <= 15) ? m : 2;
}

void ApplyRaceParameters(uint32_t trackId, uint32_t mode, uint32_t laps, uint32_t direction);

static SRWLOCK gPeerCarLock = SRWLOCK_INIT;
static CarCustomizationPacket gLatestPeerCar{};
static bool gHavePeerCar = false;
static SRWLOCK gPeerSyncLock = SRWLOCK_INIT;
static CarSyncPacket gLatestPeerSyncPkt{};
static bool gHavePeerSyncPkt = false;
static coop_network::SerialGate gPeerCarSerial,gPeerSelectionSerial;
static std::atomic<uint32_t> gLocalAppearanceSerial{0};
static DWORD gPeerSyncReceivedAtMs = 0;
static uintptr_t gPendingTrackLoader = 0; // main-thread-only native stage callback
static uint32_t gPendingTrackLoaderStage = 0;
static DWORD gPendingTrackLoaderStartedAt = 0;
static uint64_t gCoopFleetLocalLoadToken = 0;
static bool gLocalSelectionFinalized = false;
static SOCKET gGameSocket = INVALID_SOCKET;
static sockaddr_in gTargetEndpoint{};
static float gDedicatedCoopTable1DBuffer[1024]{};
static int16_t gProxySteerAngle = 0;
static bool gCar1LoadedForRace = false;

static CoopRaceState gCoopRaceState = COOP_RACE_IDLE;
static RaceEventPacket gPendingRaceEvent{};
static DWORD gRaceInviteReceivedAtMs = 0;
static DWORD gRaceStateTimeoutMs = 0;
static bool gTriggerRaceLoading = false;
static bool gRaceProposalPrepared = false;

// ---------------------------------------------------------------------------
// Synchronized Grid Countdown State (Phase 11)
// ---------------------------------------------------------------------------
static std::atomic<bool> gLocalReadyForCountdown{false};
static std::atomic<bool> gPeerReadyForCountdown{false};
static std::atomic<bool> gCountdownTriggered{false};
static std::atomic<bool> gCountdownActive{false};
static std::atomic<uint32_t> gCountdownStartAtMs{0};

void ResetCountdownSyncState() {
    gLocalReadyForCountdown.store(false);
    gPeerReadyForCountdown.store(false);
    gCountdownTriggered.store(false);
    gCountdownActive.store(false);
    gCountdownStartAtMs.store(0);
}

void BroadcastLocalCarCustomization();
void ResumePendingCoopLoading(DWORD flowState);

struct LocalVehicleState {
    uintptr_t playerInstance = 0;
    uintptr_t vtable = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float speedKmh = 0.0f;
    float headingDeg = 0.0f;
    float forwardX = 0.0f;
    float forwardY = 0.0f;
    float forwardZ = 0.0f;
    int16_t steerAngle = 0;
    uint8_t brakeLight = 0;
    uint8_t reverseLight = 0;
    char carModel[16] = {};
    float wheelPhaseRad[4] = {};
    float wheelSpeedRad[4] = {};
    float wheelRotation[4][9] = {};
    uint32_t wheelRotationValidMask = 0;
    float orientation[4] = {0,0,0,1};
    float linearVelocity[3]{};
    float angularVelocity[3]{};
    bool physicalMotionValid = false;
};

struct RemotePlayerState {
    bool valid = false;
    PositionPacket previous{};
    PositionPacket current{};
    PositionPacket rendered{};
    DWORD receivedAtMs = 0;
    DWORD previousReceivedAtMs = 0;
    bool isFromPeer = false;
};

SRWLOCK gRemoteStateLock = SRWLOCK_INIT;
RemotePlayerState gRemoteState{};
coop_motion::Buffer<PositionPacket> gRemoteMotion;
struct PublishedMotion {
    bool valid = false;
    uint32_t publication = 0;
    DWORD publishedAtMs = 0; // Local uptime age, independent of the precise wire timeline.
    LocalVehicleState local{};
    PositionPacket packet{};
};
static SRWLOCK gPublishedMotionLock = SRWLOCK_INIT;
static PublishedMotion gPublishedMotion;
static std::atomic<uint32_t> gMotionApplied{0},gMotionRefused{0};
namespace { static coop::SessionSnapshot CoopSnapshot(); }

struct WorldVehicleCandidate {
    uintptr_t entityAddress = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float distance = 0.0f;
    int carType = 0; // 1 = Player, 2 = AI Racer, 3 = Traffic
    char modelName[16] = {};
};

constexpr int kMaxCandidates = 16;
WorldVehicleCandidate gCandidates[kMaxCandidates]{};
int gCandidateCount = 0;

static wchar_t gEarlyCrashLogPath[MAX_PATH]{};
static wchar_t gRuntimeCrashLogPath[MAX_PATH]{};
static volatile LONG gRuntimeCrashPlayer=0;

static void CrashImage(const void* address,char (&path)[MAX_PATH],uintptr_t& offset) {
    MEMORY_BASIC_INFORMATION mapping{};
    if(!VirtualQuery(address,&mapping,sizeof(mapping)) || mapping.Type!=MEM_IMAGE)return;
    // Resolve mapped images without taking the loader's module-list lock.
    const DWORD size=K32GetMappedFileNameA(GetCurrentProcess(),mapping.AllocationBase,path,MAX_PATH);
    if(!size || size>=MAX_PATH) {
        std::strcpy(path,"Unknown");
        return;
    }
    offset=reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(mapping.AllocationBase);
}

static const char* CrashImageName(const char* path) {
    const auto* slash=std::strrchr(path,'\\');
    return slash?slash+1:path;
}

static void PrepareCrashReport(coop_crash::Report& report,const EXCEPTION_POINTERS& fault) {
    const auto* pExceptionInfo=&fault;
    const DWORD code=pExceptionInfo->ExceptionRecord->ExceptionCode;
    report.player=static_cast<unsigned>(InterlockedCompareExchange(&gRuntimeCrashPlayer,0,0));
    lstrcpynW(report.logPath,report.player?gRuntimeCrashLogPath:gEarlyCrashLogPath,MAX_PATH);
    char crashLog[2048]{};
    void* addr = pExceptionInfo->ExceptionRecord->ExceptionAddress;
    const bool accessFault=(code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_IN_PAGE_ERROR) &&
        pExceptionInfo->ExceptionRecord->NumberParameters>=2;
    ULONG_PTR target = accessFault?pExceptionInfo->ExceptionRecord->ExceptionInformation[1]:0;
    DWORD type = accessFault?DWORD(pExceptionInfo->ExceptionRecord->ExceptionInformation[0]):0;

    uintptr_t stackVals[8] = {};
    if (pExceptionInfo->ContextRecord && pExceptionInfo->ContextRecord->Esp >= 0x00010000) {
        SIZE_T readBytes = 0;
        ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(pExceptionInfo->ContextRecord->Esp),
            stackVals, sizeof(stackVals), &readBytes);
    }

    char faultModPath[MAX_PATH] = "Unknown";
    uintptr_t faultModOffset = 0;
    CrashImage(addr,faultModPath,faultModOffset);
    const char* faultModName=CrashImageName(faultModPath);
    MultiByteToWideChar(CP_ACP,0,faultModName,-1,report.module,MAX_PATH);

    char callStackBuf[1024] = "  Call Stack:\r\n";
    if (pExceptionInfo->ContextRecord) {
        uintptr_t curEbp = pExceptionInfo->ContextRecord->Ebp;
        for (int f = 0; f < 8; ++f) {
            if (curEbp < 0x00010000 || curEbp >= 0x7FFFF000) break;
            uintptr_t frameMem[2] = {};
            SIZE_T rb = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(curEbp),
                frameMem, sizeof(frameMem), &rb) || rb < sizeof(frameMem)) {
                break;
            }
            uintptr_t nextEbp = frameMem[0];
            uintptr_t retAddr = frameMem[1];
            if (retAddr < 0x00010000 || retAddr >= 0x7FFFFFFF) break;

            char callerMod[MAX_PATH] = "Unknown";
            uintptr_t callerOffset = 0;
            CrashImage(reinterpret_cast<const void*>(retAddr),callerMod,callerOffset);
            char frameLine[128];
            std::snprintf(frameLine, sizeof(frameLine), "    [%d] 0x%08X (%s + 0x%X)\r\n",
                f, static_cast<unsigned int>(retAddr), CrashImageName(callerMod), static_cast<unsigned int>(callerOffset));
            std::strncat(callStackBuf, frameLine, sizeof(callStackBuf) - std::strlen(callStackBuf) - 1);

            if (nextEbp <= curEbp) break;
            curEbp = nextEbp;
        }
    }

    const char* rootCause = "Unhandled Windows exception";
    if(code==EXCEPTION_BREAKPOINT) {
        rootCause="Native engine assertion/breakpoint; see caller at stack top (not an access violation)";
    } else if (accessFault && (std::strstr(faultModName, "nvd3dum") || std::strstr(faultModName, "d3d9") || std::strstr(faultModName, "AcLayers"))) {
        rootCause = "Graphics module memory fault";
    } else if (accessFault && _stricmp(faultModName,"SPEED2.EXE")==0) {
        if (faultModOffset == 0x1ACB21 || (faultModOffset >= 0x1ACB00 && faultModOffset <= 0x1ACB40)) {
            rootCause = "Frontend car slot out of bounds access (VehicleCount > 1 during frontend menus)";
        } else if (faultModOffset == 0x1B5C70 || (faultModOffset >= 0x1B5AB0 && faultModOffset <= 0x1B5C80)) {
            rootCause = "Table1D physics spline curve store fault";
        } else if (faultModOffset >= 0x1930D0 && faultModOffset <= 0x193150) {
            rootCause = "SimEntity mesh visibility access fault";
        } else if (faultModOffset >= 0x1F6E20 && faultModOffset <= 0x1F6E70) {
            rootCause = "Car deactivation/hide routine fault";
        } else if (target < 0x00010000) {
            rootCause = "Engine null pointer dereference";
        } else {
            rootCause = "SPEED2.EXE game engine memory fault";
        }
    } else if (accessFault && target < 0x00010000) {
        rootCause = "Null or near-null pointer dereference";
    }

    std::snprintf(crashLog, sizeof(crashLog),
        "\r\n========================================\r\n"
        "[CRASH DETECTED]\r\n"
        "  Root Cause Analysis: %s\r\n"
        "  Code: 0x%08lX (%s)\r\n"
        "  Instruction Address: 0x%p (%s + 0x%X)\r\n"
        "  Module Path: %s\r\n"
        "  Attempted to %s: 0x%p\r\n"
        "  Registers:\r\n"
        "    EAX: 0x%08lX  EBX: 0x%08lX  ECX: 0x%08lX  EDX: 0x%08lX\r\n"
        "    ESI: 0x%08lX  EDI: 0x%08lX  ESP: 0x%08lX  EBP: 0x%08lX\r\n"
        "    EIP: 0x%08lX\r\n"
        "  Stack Top:\r\n"
        "    [ESP+0x00]: 0x%08lX  [ESP+0x04]: 0x%08lX\r\n"
        "    [ESP+0x08]: 0x%08lX  [ESP+0x0C]: 0x%08lX\r\n"
        "    [ESP+0x10]: 0x%08lX  [ESP+0x14]: 0x%08lX\r\n"
        "    [ESP+0x18]: 0x%08lX  [ESP+0x1C]: 0x%08lX\r\n"
        "%s"
        "========================================\r\n",
        rootCause,
        static_cast<unsigned long>(code),code==EXCEPTION_BREAKPOINT?"Breakpoint/assertion":accessFault?"Memory access":"Unhandled exception",
        addr, faultModName, static_cast<unsigned int>(faultModOffset), faultModPath,
        accessFault?(type==8?"EXECUTE":type==1?"WRITE":"READ"):"ACCESS (not applicable)",
        reinterpret_cast<void*>(target),
        static_cast<unsigned long>(pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Eax : 0),
        static_cast<unsigned long>(pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Ebx : 0),
        static_cast<unsigned long>(pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Ecx : 0),
        static_cast<unsigned long>(pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Edx : 0),
        static_cast<unsigned long>(pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Esi : 0),
        static_cast<unsigned long>(pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Edi : 0),
        static_cast<unsigned long>(pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Esp : 0),
        static_cast<unsigned long>(pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Ebp : 0),
        static_cast<unsigned long>(pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Eip : 0),
        static_cast<unsigned long>(stackVals[0]),
        static_cast<unsigned long>(stackVals[1]),
        static_cast<unsigned long>(stackVals[2]),
        static_cast<unsigned long>(stackVals[3]),
        static_cast<unsigned long>(stackVals[4]),
        static_cast<unsigned long>(stackVals[5]),
        static_cast<unsigned long>(stackVals[6]),
        static_cast<unsigned long>(stackVals[7]),
        callStackBuf);

    if (report.logPath[0]) {
        HANDLE hLog = CreateFileW(report.logPath, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hLog != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            const auto bytes=static_cast<DWORD>(std::strlen(crashLog));
            if(!WriteFile(hLog,crashLog,bytes,&written,nullptr) || written!=bytes)report.logPath[0]=L'\0';
            CloseHandle(hLog);
        } else report.logPath[0]=L'\0';
    }
}

static void StartCoopCrashReporter(HMODULE module) {
    if(!gEarlyCrashLogPath[0]) {
        wchar_t path[MAX_PATH]{};
        const DWORD length=GetModuleFileNameW(module,path,MAX_PATH);
        if(length && length<MAX_PATH) {
            auto* name=std::wcsrchr(path,L'\\');
            constexpr wchar_t early[]=L"NFSU2Coop-crash.log";
            if(name && static_cast<size_t>(name-path)+1+std::size(early)<=MAX_PATH) {
                std::wcscpy(name+1,early);
                lstrcpynW(gEarlyCrashLogPath,path,MAX_PATH);
            }
        }
    }
    coop_crash::Install(&PrepareCrashReport);
}

namespace { static void NoteAcceptedCoopMotion(const coop::SessionSnapshot& accepted); }
bool UpdateRemotePlayerState(const PositionPacket& packet, bool fromExternalPeer) {
    const auto session = CoopSnapshot();
    if (!session.joined || !session.fresh || packet.sessionId != session.sessionId ||
        packet.membershipNonce!=session.membershipNonce || packet.ticket!=session.ticket) return false;
    AcquireSRWLockExclusive(&gRemoteStateLock);
    const DWORD now = GetTickCount();
    const bool newGeneration=!gRemoteState.valid||gRemoteState.current.sessionId!=packet.sessionId||
        gRemoteState.current.membershipNonce!=packet.membershipNonce||gRemoteState.current.ticket!=packet.ticket;
    const bool newWorld=newGeneration||packet.worldEpoch!=gRemoteState.current.worldEpoch||packet.trackId!=gRemoteState.current.trackId;
    if(newGeneration)gRemoteMotion=coop_motion::Buffer<PositionPacket>{};
    if (!gRemoteMotion.Push(packet, coop_motion::MotionMilliseconds())) {
        ReleaseSRWLockExclusive(&gRemoteStateLock);
        return false;
    }
    if (!newWorld) {
        gRemoteState.previous = gRemoteState.current;
        gRemoteState.previousReceivedAtMs = gRemoteState.receivedAtMs;
    } else {
        gRemoteState.previous = packet;
        gRemoteState.previousReceivedAtMs = now;
    }
    if (newWorld)
        gRemoteState.rendered=packet;
    gRemoteState.current = packet;
    gRemoteState.receivedAtMs = now;
    gRemoteState.valid = true;
    if (fromExternalPeer) {
        gRemoteState.isFromPeer = true;
        gReceivedExternalPeer = true;
    }
    ReleaseSRWLockExclusive(&gRemoteStateLock);
    if(fromExternalPeer) {
        NoteAcceptedCoopMotion(session);
    }
    return true;
}

bool GetRemotePlayerState(RemotePlayerState& state,bool sampleMotion=true) {
    AcquireSRWLockExclusive(&gRemoteStateLock);
    state = gRemoteState;
    const DWORD now = GetTickCount();
    const bool sampled = state.valid && (sampleMotion ? gRemoteMotion.Sample(coop_motion::MotionMilliseconds(),state.rendered) :
        state.rendered.sessionId==state.current.sessionId&&state.rendered.membershipNonce==state.current.membershipNonce&&
        state.rendered.ticket==state.current.ticket&&state.rendered.worldEpoch==state.current.worldEpoch&&state.rendered.trackId==state.current.trackId);
    if (sampled && sampleMotion) gRemoteState.rendered=state.rendered;
    ReleaseSRWLockExclusive(&gRemoteStateLock);
    AcquireSRWLockShared(&gPublishedMotionLock);
    const bool sameWorld = gPublishedMotion.valid &&
        coop_time::Elapsed(now,gPublishedMotion.publishedAtMs) <= 250 &&
        state.current.sessionId == gPublishedMotion.packet.sessionId &&
        state.current.membershipNonce == gPublishedMotion.packet.membershipNonce &&
        state.current.ticket == gPublishedMotion.packet.ticket &&
        state.current.trackId == gPublishedMotion.packet.trackId;
    ReleaseSRWLockShared(&gPublishedMotionLock);
    return sampled && sameWorld && state.current.flowState == 6 &&
        coop_time::Elapsed(now,state.receivedAtMs) <= 2000;
}

void GetInterpolatedRemotePosition(const RemotePlayerState& state,
    float& x, float& y, float& z, float& speed, float& heading,
    int16_t* outSteer = nullptr, uint8_t* outBrake = nullptr, uint8_t* outReverse = nullptr,
    float* outWheelPhase = nullptr, float* outWheelSpeed = nullptr) {
    const auto& pose = state.rendered;
    x=pose.x; y=pose.y; z=pose.z; speed=pose.speedKmh; heading=pose.headingDeg;
    if (outSteer) *outSteer=pose.steerAngle;
    if (outBrake) *outBrake=pose.brakeLight;
    if (outReverse) *outReverse=pose.reverseLight;
    for (size_t i = 0; i < 4; ++i) {
        if (outWheelSpeed) outWheelSpeed[i]=pose.wheelSpeedRad[i];
        if (outWheelPhase) outWheelPhase[i]=pose.wheelPhaseRad[i];
    }
}

namespace {
#include "CoopFleetInterfaces.h"
static coop_transport::Transport gCoopTransport;
static std::atomic<uint32_t> gPublishedGameFlow{0};
static std::atomic<uint32_t> gPublishedGameTrack{0};
static std::atomic<bool> gCoopTransportOpened{false};
static int SendMeasuredCoopDatagram(const void*,size_t,coop::Endpoint);
static bool ActualCoopTransportReady(const coop::SessionSnapshot& state){
    return gCoopTransport.Ready(state.sessionId,state.membershipNonce,state.peer);
}
// Private native-adapter fixtures supply a typed verified-link substitute.
// Production always uses ActualCoopTransportReady; no INI can bypass it.
static bool (*gCoopTransportReadyProbe)(const coop::SessionSnapshot&)=ActualCoopTransportReady;
static bool CoopConnectedTransportReady(const coop::SessionSnapshot& state){
    return state.joined&&state.fresh&&gCoopTransportReadyProbe&&gCoopTransportReadyProbe(state);
}
static coop_network::Generation CurrentPeerGeneration(){
    const auto state=CoopSnapshot();return state.joined&&state.fresh?
        coop_network::Generation{state.sessionId,state.membershipNonce,state.ticket}:coop_network::Generation{};
}
template<class Packet> static coop_network::Generation AppearanceGeneration(const Packet& p){return {p.sessionId,p.membershipNonce,p.ticket};}
template<class Packet> static bool ScopeLocalAppearance(Packet& p){
    const auto generation=CurrentPeerGeneration();if(!generation.Valid())return false;
    p.sessionId=generation.session;p.membershipNonce=generation.membership;p.ticket=generation.ticket;
    p.sequence=gLocalAppearanceSerial.fetch_add(1,std::memory_order_relaxed)+1;
    if(!p.sequence)p.sequence=gLocalAppearanceSerial.fetch_add(1,std::memory_order_relaxed)+1;
    return true;
}
static void ResetCoopPeerSelectionGeneration(){
    const auto live=CurrentPeerGeneration();static coop_network::Generation observed{};
    if(observed==live)return;observed=live;
    AcquireSRWLockExclusive(&gPeerSyncLock);
    if(!live.Valid()||AppearanceGeneration(gLatestPeerSyncPkt)!=live){
        gHavePeerSyncPkt=false;gPeerSyncReceivedAtMs=0;gLatestPeerSyncPkt={};gPeerSelectionSerial.Clear();
    }
    ReleaseSRWLockExclusive(&gPeerSyncLock);
    AcquireSRWLockExclusive(&gPeerCarLock);
    if(!live.Valid()||AppearanceGeneration(gLatestPeerCar)!=live){gHavePeerCar=false;gLatestPeerCar={};gPeerCarSerial.Clear();}
    ReleaseSRWLockExclusive(&gPeerCarLock);
    AcquireSRWLockExclusive(&gRemoteStateLock);
    if(!live.Valid()||gRemoteState.current.sessionId!=live.session||
        gRemoteState.current.membershipNonce!=live.membership||gRemoteState.current.ticket!=live.ticket){
        gRemoteState={};gRemoteMotion=coop_motion::Buffer<PositionPacket>{};gReceivedExternalPeer=false;
    }
    ReleaseSRWLockExclusive(&gRemoteStateLock);
}
static coop_special_save::Fence gSpecialSaveFence;
static std::wstring WideText(const char*);
void TickCoopFleetAssetsNetwork(DWORD now);
bool ReceiveCoopFleetAssetsPacket(const char*,int,const sockaddr_in&,DWORD);
bool InstallCoopFleetAssetsHooks();
bool InstallCoopFleetWorldNative();
bool ProbeCoopVehicleAssetAdaptersReady();
bool RefreshCoopVehicleAssetAdaptersReady();
void TraceCoopAddonMenus(DWORD flow);
bool InstallCoopStockCatalogReaderHook();
bool CoopStockCatalogReaderReady();
void TickCoopStockCatalog(DWORD flow);
uint32_t CoopFleetCommittedParameterCount();
bool ShouldPreserveCoopFleetRideSetup(uintptr_t ride);
bool CoopFleetSelectionPlanReady(const coop_world::Identity&);
bool CoopFleetAssetsReadyForStage7(const coop_world::Identity&);
bool NotifyCoopFleetAssetsAfterStage7(const coop_world::Identity&,uint64_t generation);
uint64_t CoopFleetLocalLoadToken() { return gCoopFleetLocalLoadToken; }
void OnCoopFleetNativeStage7Completed();
// Internal native boundary reads; defaults are the verified game addresses.
// Isolated adapter tests replace these with their own allocated data pages.
struct CoopRaceContextAddresses {
    uintptr_t flow=kGameFlowState,track=0x0089E7A0;
    uintptr_t nativeFlow=0x008900D8,race=0x00890118;
};
static CoopRaceContextAddresses gRaceContextAddresses;
struct CoopRaceProgressAddresses {
    uintptr_t carArray=kCarArray,routeLength=0x0088F2B0,localRoot=kPlayerPositionRoot;
};
static CoopRaceProgressAddresses gRaceProgressAddresses;
uintptr_t CoopRaceCar(bool peer);
bool ReadCoopPhysicalMotion(uintptr_t car,float* orientation,float* velocity,float* angular);
bool SyncNativePeerRaceProgress(uintptr_t car,const PositionPacket& packet);
void TickPeerAppearanceRefresh(DWORD flowState);
void RetirePeerAppearanceOutsideWorld(DWORD flowState);
bool InstallPeerTextureAllocationHooks();
bool ReadPeerSetupTarget(uintptr_t car,uintptr_t& ride);
bool IsRemotePeerSetupMetadata(uint8_t playerIndex,uint8_t remoteFlag,uint32_t carType);
void NativeGameFlowSwitchTrack(uint32_t trackId, uint32_t mode, uint32_t laps, uint32_t direction);
void TickCoopSessionUi(DWORD flowState);
void TickCoopCareerPresentation(DWORD flowState);
uint32_t CoopCameraShieldCount();
void TickCoopSessionNetwork(DWORD now);
bool CoopModsReady(const coop::SessionSnapshot& state);
const char* CoopModStatus();
void ResetCoopModPeer();
void TickCoopModNetwork(DWORD now);
bool ReceiveCoopModPacket(const char*,int,const sockaddr_in&,DWORD);
void TickCoopModUi(DWORD flow);
void StartCoopModScan();
void TickCoopRaceLifecycle(DWORD flow);
void TickCoopRaceNetwork(DWORD now);
void TickCoopRaceFinish(DWORD flow);
bool StartNativePeerRace();
bool ReceiveCoopRacePacket(const char*,int,const sockaddr_in&,DWORD);
bool IsCoopUrlTournament();
bool ReadCoopUrlFrozenRacer(uintptr_t,uint8_t&,uint32_t&,uint16_t&);
bool CoopUrlReadHeatResult(uint8_t&,uint32_t&,uint16_t&);
bool CoopUrlReadFinalResult(uint8_t&,uint32_t&,uint16_t&);
bool CoopUrlReadCareerOutcome(uint8_t&);
bool CoopUrlOwnsReceipt();
bool CoopUrlRetainsNativeEvent();
bool CoopUrlConsumeFinalizedStart(const RaceEventPacket&,DWORD);
void CoopUrlNativePrepared();
void TickCoopUrlTournament(DWORD);
void TickCoopUrlNetwork(DWORD);
bool ReceiveCoopUrlPacket(const char*,int,const sockaddr_in&,DWORD);
bool CoopFinishRaceActive();
bool CoopFinishIsPeerRacer(uintptr_t racer);
bool CoopRaceHooksReady();
bool CoopRaceReturnedToFreeRoam(DWORD flow,uint32_t track);
void CaptureCoopRaceReturnOrigin();
void ApplyCoopRaceReturnOrigin(uintptr_t localParameter);
void SaveCoopRaceReturnOrigin(DWORD flow);
void CaptureCoopRaceReturnBackup();
void RestoreCoopRaceReturnBackup();
bool ReadCoopLocalDriftScore(uintptr_t car,float& score,uint32_t& epoch);
bool InstallCoopDriftHooks();
bool InstallCoopCareerOfferHooks();
static coop_race::State RaceStateSnapshot();
void TickCoopDriftResults(DWORD flow);
void TickCoopMovies(DWORD flow);
void TickCoopMovieNetwork(DWORD now);
bool ReceiveCoopMoviePacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now);
bool InstallCoopMovieSyncHooks();
bool InstallCoopSponsorHooks();
void TickCoopSponsorUi();
void TickCoopSponsorNetwork(DWORD now);
bool ReceiveCoopSponsorPacket(const char*,int,const sockaddr_in&,DWORD);
bool HandleCoopSponsorDecisionKey(unsigned key);
bool InstallCoopVisualGate();
void TickCoopVisualUi(DWORD flow);
void TickCoopVisualNetwork(DWORD now);
bool ReceiveCoopVisualPacket(const char*,int,const sockaddr_in&,DWORD);
bool CoopVisualMayMerge(const std::vector<uint8_t>&,const coop_career::Progress&);
bool CoopVisualNeedsRefresh();
bool CoopVisualRefreshGate();
bool CoopSpecialHooksReady();
bool CoopSpecialFactsReady();
bool CoopSpecialFilterUnpaired(coop_career::Progress&,const coop_career::Progress&);
bool CoopSpecialClassify(uint32_t hash,uint32_t& kind,uint32_t& stage);
bool CoopSpecialMayImport();
bool InstallCoopSpecialHooks();
void TickCoopSpecialUi(DWORD flow);
void TickCoopSpecialNetwork(DWORD now);
bool ReceiveCoopSpecialPacket(const char*,int,const sockaddr_in&,DWORD);
bool HandleCoopSpecialDecisionKey(unsigned key);
bool CoopSpecialBusy();
bool CoopSpecialClockPaused();
bool CoopSpecialPrepareInitial(const char*,const char*,uint32_t,bool,std::vector<uint8_t>&,coop_career::Progress*);
bool CoopSpecialInitialHeld();
bool CoopSpecialBeforeNewCareer(const char*);
void TickCoopSpecialController(DWORD);
bool ReceiveCoopSpecialRecoveryPacket(const char*,int,const sockaddr_in&,DWORD);
void TickCoopSpecialRecoveryNetwork(DWORD);
bool ConfigureCoopSpecialDriver();
bool CoopFleetAnyOutrunBusy();
bool CoopSpecialBothCarsForRace(uint32_t);
static bool RaceIntentPending();
bool PrepareCoopAIRosterBeforeStreaming();
bool CommitCoopAIRosterAfterPeer();
bool SealCoopAIRosterAfterStreaming();
bool AuthorizeCoopAIRosterRestartBeforeTeardown();
uint32_t CoopAIRosterCommittedCount();
uint32_t CoopAIRosterRaceCount();
bool ReadCoopAIRosterParam(uintptr_t parameter,uint32_t& ordinal,uint32_t& model);
bool ReadManagedCoopAI(uintptr_t car,uintptr_t& ride,uint32_t& kind);
bool ShouldSuppressCoopAITick(uintptr_t object);
bool ShouldSuppressCoopAIController(uintptr_t object);
bool CoopFinishIsManagedAIRacer(uintptr_t racer);
void TickCoopAIWorld(DWORD flow);
void ApplyCoopWorldImpactPrediction(uintptr_t car,coop_world::Motion& motion);
void TickCoopWorldImpact(DWORD flow);
void TickCoopWorldImpactNetwork(DWORD now);
bool ReceiveCoopWorldImpactPacket(const char*,int,const sockaddr_in&,DWORD);
void SyncCoopAIRaceProgress();
void TraceCoopAIWorld(DWORD flow);
void TraceCoopAIVisual(DWORD flow);
static void TraceCoopAIRepair(DWORD flow);
void TraceCoopTrafficInventory(DWORD flow);
bool CoopFleetScannerNativeReady();
void TickCoopAIRosterNetwork(DWORD now);
void TickCoopAIWorldNetwork(DWORD now);
bool ReceiveCoopAIRosterPacket(const char*,int,const sockaddr_in&,DWORD);
bool ReceiveCoopAIWorldPacket(const char*,int,const sockaddr_in&,DWORD);
void PrepareNativeCoopRaceIntent();
bool PopulateNativeCoopRaceProposal();
bool HoldNativeCoopRaceIntent(void* gameFlow);
bool HoldNativeCoopRaceRestart(void* nativeFlow);
bool StartApprovedNativeCoopRace();
bool FinalizeCoopNativeRaceLoading(void* loader);
const char* CoopRaceLoadingFailureMessage();
bool HandleCoopRaceInviteKey(WPARAM key);
void TickCoopChatUi();
bool SendCoopChatMessage(const char*);
void BindCoopConsoleForUi(const coop::SessionSnapshot&);
void TraceCoopConsoleToggle(const char* source,bool handled);
void PollCoopConsoleToggle(bool down);
bool ReceiveCoopRetiredPausePacket(const char*,int);
bool InstallCoopLivePauseHook();
void CancelCoopRaceInvitation(bool notifyPeer);
bool ReceiveCoopSessionPacket(const char* data, int bytes, const sockaddr_in& sender, DWORD now);
bool IsJoinedCoopSender(const sockaddr_in& sender);
bool InitialCoopLoadingBarrier();
void BrowseCoopServers();
void OnCoopNativeScreen(const char* name);
bool RouteContextSavePath(const char* input, char* output, size_t capacity);
void TraceCoopProfileEnumeration(const char*,const char*,HANDLE,const WIN32_FIND_DATAA*,DWORD);
void TrackCoopWriteHandle(HANDLE handle,const char* path,DWORD access);
bool RouteClientCoopWrite(const char* path,char* output,size_t capacity,DWORD access);
bool InstallCoopSaveCompletionHooks();
bool CoopSharedCareerCanApply(DWORD flow);
bool SaveLocalCoopRaceProgress(DWORD now);

inline bool ReadGameMemory(uintptr_t address, void* output, SIZE_T bytes) {
    if (address < 0x00400000 || address >= 0x7FFE0000 || !output || bytes == 0) return false;
    __try {
        std::memcpy(output, reinterpret_cast<const void*>(address), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool SafeWriteBytes(uintptr_t address, const void* src, size_t size) {
    if (address < 0x00400000 || address >= 0x7FFE0000 || !src || size == 0) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(address), src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool IsValidUserPtr(uintptr_t address) {
    return (address >= 0x00400000 && address < 0x7FFE0000);
}

#include "CoopCarCatalogNative.inl"

inline float SanitizeFloat(float v, float fallback = 0.0f) {
    return std::isfinite(v) ? v : fallback;
}

constexpr DWORD kPeerMapMaxAgeMs = 2000;

struct PeerMapPosition {
    float x = 0;
    float y = 0;
    float headingDeg = 0;
};

bool IsPeerMapStateVisible(DWORD localFlow, const RemotePlayerState& remote, DWORD now) {
    // Map visibility is independent of the F9 physics and F10 observer toggles.
    return localFlow == 6 && remote.valid && remote.isFromPeer &&
        remote.current.flowState == 6 && now - remote.receivedAtMs <= kPeerMapMaxAgeMs &&
        std::isfinite(remote.current.x) && std::fabs(remote.current.x) < 1000000.0f &&
        std::isfinite(remote.current.y) && std::fabs(remote.current.y) < 1000000.0f &&
        std::isfinite(remote.current.headingDeg);
}

bool TryGetPeerMapPosition(PeerMapPosition& position) {
    DWORD flow = 0;
    RemotePlayerState remote{};
    if (!ReadGameMemory(kGameFlowState, &flow, sizeof(flow)) ||
        !GetRemotePlayerState(remote) || !IsPeerMapStateVisible(flow, remote, GetTickCount()))
        return false;
    float z = 0, speed = 0;
    GetInterpolatedRemotePosition(remote, position.x, position.y, z, speed, position.headingDeg);
    return std::isfinite(position.x) && std::fabs(position.x) < 1000000.0f &&
        std::isfinite(position.y) && std::fabs(position.y) < 1000000.0f &&
        std::isfinite(position.headingDeg);
}

bool IsUsableWheelRotation(const float* rotation) {
    if (!rotation) return false;
    // Native meshes can include model-specific scale. Require a finite,
    // nondegenerate bounded basis, rather than incorrectly rejecting scale.
    for (int row = 0; row < 3; ++row) {
        float lengthSquared = 0;
        for (int col = 0; col < 3; ++col) {
            const float value = rotation[row * 3 + col];
            if (!std::isfinite(value) || std::fabs(value) > 4.0f) return false;
            lengthSquared += value * value;
        }
        if (lengthSquared < 0.01f || lengthSquared > 16.0f) return false;
    }
    const float determinant = rotation[0] * (rotation[4]*rotation[8] - rotation[5]*rotation[7]) -
        rotation[1] * (rotation[3]*rotation[8] - rotation[5]*rotation[6]) +
        rotation[2] * (rotation[3]*rotation[7] - rotation[4]*rotation[6]);
    return std::isfinite(determinant) && std::fabs(determinant) > 0.001f;
}

// Performance Profiler and Fast Hook Caches
static uintptr_t gCachedProxyRideInfo = 0;
static uintptr_t gCachedRaceParamCar1Ride = 0;
static LARGE_INTEGER gQpcFreq{};
static LARGE_INTEGER gLastFrameQpc{};
static float gCurrentFps = 0.0f;
static float gAvgFps = 0.0f;
static float gMinFps = 999.0f;
static uint32_t gUiTickDurationUs = 0;
static uint32_t gMaxUiTickDurationUs = 0;
static uint32_t gFrameCounter = 0;
static bool gIsGameFocused = true;
static DWORD gLastFocusCheckMs = 0;

// 3D VehicleVisual Render Diagnostics
static uint32_t gProxyRenderDurationUs = 0;
static uint32_t gProxyRenderMaxUs = 0;
static uint32_t gProxyRenderFrameCount = 0;
static bool gProxyRenderedThisFrame = false;

static uint32_t gPlayerRenderDurationUs = 0;
static uint32_t gPlayerRenderFrameCount = 0;
static bool gPlayerRenderedThisFrame = false;

const char* DescribeFlowState(DWORD state) {
    switch (state) {
    case 3: return "front_end";
    case 4:
    case 5: return "loading";
    case 6: return "gameplay";
    default: return "other";
    }
}

void ApplyMultiInstancePatch() {
    auto* patchAddr = reinterpret_cast<BYTE*>(kMultiInstancePatch);
    DWORD oldProtect = 0;
    if (VirtualProtect(patchAddr, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *patchAddr = 0xEB; // JMP opcode, skips single-instance mutex check
        VirtualProtect(patchAddr, 1, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), patchAddr, 1);
    }
}

void LoadConfiguration(const wchar_t* iniPath) {
    char iniPathA[MAX_PATH]{};
    WideCharToMultiByte(CP_ACP, 0, iniPath, -1, iniPathA, MAX_PATH, nullptr, nullptr);

    GetPrivateProfileStringA("Network", "PeerAddress", "127.0.0.1",
        gPeerAddress, sizeof(gPeerAddress), iniPathA);
    gLocalPort = static_cast<unsigned short>(
        GetPrivateProfileIntA("Network", "Port", 48930, iniPathA));
    gTargetPort = static_cast<unsigned short>(
        GetPrivateProfileIntA("Network", "PeerPort", 48931, iniPathA));
    gRemoteSyncEnabled = (GetPrivateProfileIntA("GhostCar", "EnableSync", 1, iniPathA) != 0);
    gCoopAIEnabled = (GetPrivateProfileIntA("Experimental", "RaceAI", 0, iniPathA) != 0);
    gCoopFleetEnabled = gCoopAIEnabled && (GetPrivateProfileIntA("Experimental", "WorldFleet", 0, iniPathA) != 0);
    gConfiguredInstance = GetPrivateProfileIntA("Network", "Instance", 0, iniPathA);
    if (gConfiguredInstance > 2) gConfiguredInstance = 0;
    gIsSecondInstance = (gConfiguredInstance == 2);
    GetPrivateProfileStringA("Career", "Profile", "", gConfiguredCareerProfile,
        sizeof(gConfiguredCareerProfile), iniPathA);
    gCoopAutoBackup=GetPrivateProfileIntW(L"Career",L"AutoBackup",1,iniPath)!=0;
    CoopAnnouncements::Configure(
        GetPrivateProfileIntA("Announcements", "AutoShow", 1, iniPathA),
        GetPrivateProfileIntA("Announcements", "BackgroundOpacity", 70, iniPathA));
    CoopAnnouncements::SetChatSender(&SendCoopChatMessage);

    char offsetBuf[32]{};
    if (GetPrivateProfileStringA("GhostCar", "LoopbackOffsetX", "4.0", offsetBuf, sizeof(offsetBuf), iniPathA)) {
        gLoopbackOffsetX = static_cast<float>(std::atof(offsetBuf));
    }

    // Startup always remains vanilla SP. Membership requires an explicit UI join.
    gCoopSessionMode.store(COOP_SESSION_INACTIVE);
}

bool GetCarModelName(uintptr_t carPtr, char* outName, size_t maxLen) {
    if (carPtr < 0x01000000 || outName == nullptr || maxLen == 0) return false;
    outName[0] = '\0';

    // Resolve the real Car::RideInfo model through the validated installed
    // catalog. A truncated addon name must never become a network identity.
    uintptr_t nativeRide = 0; uint32_t nativeModel = UINT32_MAX;
    if (ReadGameMemory(carPtr + 0x10, &nativeRide, sizeof(nativeRide)) && IsValidUserPtr(nativeRide) &&
        ReadGameMemory(nativeRide, &nativeModel, sizeof(nativeModel)))
        return CoopCarCatalogNative::ReadName(nativeModel, outName, maxLen);

    // 1. Try via CarRenderInfo (carPtr + 0x14)
    uintptr_t renderInfo = 0;
    if (ReadGameMemory(carPtr + 0x14, &renderInfo, sizeof(renderInfo)) && renderInfo >= 0x01000000) {
        // CarRenderInfo + 0x0C is CarTypeInfo*
        uintptr_t carTypeInfo = 0;
        if (ReadGameMemory(renderInfo + 0x0C, &carTypeInfo, sizeof(carTypeInfo)) && carTypeInfo >= 0x00400000 && carTypeInfo < 0x7FFFFFFF) {
            if (ReadGameMemory(carTypeInfo, outName, maxLen) && outName[0] != '\0' && std::memchr(outName, 0, maxLen)) {
                bool valid = true;
                for (size_t i = 0; i < maxLen && outName[i] != '\0'; ++i) {
                    if (outName[i] < 32 || outName[i] > 126) { valid = false; break; }
                }
                if (valid && outName[0] != '\0') return true;
            }
        }
        // Fallback: CarRenderInfo + 0x10 is RideInfo*. RideInfo + 0 is CarTypeID
        uintptr_t rideInfo = 0;
        if (ReadGameMemory(renderInfo + 0x10, &rideInfo, sizeof(rideInfo)) && rideInfo >= 0x01000000) {
            uint32_t carTypeId = 0;
            if (ReadGameMemory(rideInfo, &carTypeId, sizeof(carTypeId)) && CoopCarCatalogNative::Supported(carTypeId)) {
                uintptr_t carTypeArray = 0;
                if (ReadGameMemory(0x008A1CCC, &carTypeArray, sizeof(carTypeArray)) && carTypeArray >= 0x00400000) {
                    uintptr_t entry = carTypeArray + (carTypeId * 0x890);
                    if (ReadGameMemory(entry, outName, maxLen) && outName[0] != '\0' && std::memchr(outName, 0, maxLen)) {
                        bool valid = true;
                        for (size_t i = 0; i < maxLen && outName[i] != '\0'; ++i) {
                            if (outName[i] < 32 || outName[i] > 126) { valid = false; break; }
                        }
                        if (valid && outName[0] != '\0') return true;
                    }
                }
            }
        }
    }

    // 2. Direct fallback via Car + 0x0C (CarTypeInfo*)
    uintptr_t directTypeInfo = 0;
    if (ReadGameMemory(carPtr + 0x0C, &directTypeInfo, sizeof(directTypeInfo)) && directTypeInfo >= 0x00400000 && directTypeInfo < 0x7FFFFFFF) {
        if (ReadGameMemory(directTypeInfo, outName, maxLen) && outName[0] != '\0' && std::memchr(outName, 0, maxLen)) {
            bool valid = true;
            for (size_t i = 0; i < maxLen && outName[i] != '\0'; ++i) {
                if (outName[i] < 32 || outName[i] > 126) { valid = false; break; }
            }
            if (valid && outName[0] != '\0') return true;
        }
    }

    outName[0] = '\0';
    return false;
}

// 0x00601110 / 0x0063A5A0 create the native visual at Car + 0x554.
// Visual + 0 owns the Car, and Visual + 4 references its RideInfo.
uintptr_t GetVehicleVisual(uintptr_t carPtr) {
    if (!IsValidUserPtr(carPtr)) return 0;
    uintptr_t visual = 0, owner = 0;
    if (!ReadGameMemory(carPtr + 0x554, &visual, sizeof(visual)) || !IsValidUserPtr(visual) ||
        !ReadGameMemory(visual, &owner, sizeof(owner)) || owner != carPtr) return 0;
    return visual;
}

bool ReadPlayerVehicleState(LocalVehicleState& state) {
    DWORD root = 0;
    DWORD playerInstance = 0;
    if (!ReadGameMemory(gRaceProgressAddresses.localRoot, &root, sizeof(root)) || root == 0 ||
        !ReadGameMemory(static_cast<uintptr_t>(root) + 4, &playerInstance,
            sizeof(playerInstance)) || playerInstance == 0) {
        return false;
    }

    state.playerInstance = static_cast<uintptr_t>(playerInstance);

    // Read and verify Vehicle vtable
    uintptr_t vtbl = 0;
    if (ReadGameMemory(state.playerInstance, &vtbl, sizeof(vtbl))) {
        state.vtable = vtbl;
        gVehicleVtable = vtbl;
    }

    // 1. Position Vector (X, Y, Z at playerInstance + 0x60)
    float pos[3]{};
    if (!ReadGameMemory(state.playerInstance + 0x60, pos, sizeof(pos))) {
        return false;
    }
    state.x = pos[0];
    state.y = pos[1];
    state.z = pos[2];

    // 2. Heading / Car Angle (at playerInstance + 0x40C short, 65536 = 360 degrees)
    uint16_t angleRaw = 0;
    if (ReadGameMemory(state.playerInstance + 0x40C, &angleRaw, sizeof(angleRaw))) {
        state.headingDeg = (static_cast<float>(angleRaw) / 65536.0f) * 360.0f;
    }
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float rad = state.headingDeg * kDegToRad;
    state.forwardX = std::cos(rad);
    state.forwardY = std::sin(rad);
    state.forwardZ = 0.0f;

    // 3. Linear Velocity Vector (Vx, Vy, Vz at playerInstance + 0x70)
    float vel[3]{};
    if (ReadGameMemory(state.playerInstance + 0x70, vel, sizeof(vel))) {
        float speedMs = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
        state.speedKmh = speedMs * 3.6f;
    }
    state.physicalMotionValid = ReadCoopPhysicalMotion(state.playerInstance,
        state.orientation, state.linearVelocity, state.angularVelocity);
    if (state.physicalMotionValid) {
        float rotation[16]{};
        coop_motion::QuaternionToMatrix(state.orientation, rotation);
        state.forwardX=rotation[0]; state.forwardY=rotation[1]; state.forwardZ=rotation[2];
        state.headingDeg=std::atan2(rotation[1],rotation[0])*180.0f/coop_motion::kPi;
        if (state.headingDeg<0) state.headingDeg+=360.0f;
    }

    // 4. Front Wheel Steer Angle (Car + 0x40E is authoritative: 0 = wheels straight ahead)
    int16_t steer = 0;
    ReadGameMemory(state.playerInstance + 0x40E, &steer, sizeof(steer));
    state.steerAngle = steer;

    // 0x5F6820 copies these ordinary tire/rim matrices into renderer argument 5.
    // Send only their rotations: each receiver retains its own model-specific
    // wheel positions, suspension and separate hub/caliper matrices.
    for (size_t wheel = 0; wheel < 4; ++wheel) {
        float wheelMatrix[16]{};
        if (!ReadGameMemory(state.playerInstance + 0x130 + wheel * 0x40,
            wheelMatrix, sizeof(wheelMatrix))) continue;
        for (size_t row = 0; row < 3; ++row) {
            for (size_t column = 0; column < 3; ++column) {
                state.wheelRotation[wheel][row * 3 + column] = wheelMatrix[row * 4 + column];
            }
        }
        if (IsUsableWheelRotation(state.wheelRotation[wheel])) {
            state.wheelRotationValidMask |= uint32_t{1} << wheel;
        } else {
            std::memset(state.wheelRotation[wheel], 0, sizeof(state.wheelRotation[wheel]));
        }
    }

    // Native 0x5ECAC0 phase/speed controls optional spinner meshes (renderer
    // argument 7), not the ordinary wheel matrices above.
    ReadGameMemory(state.playerInstance + 0x4E8, state.wheelPhaseRad, sizeof(state.wheelPhaseRad));
    ReadGameMemory(state.playerInstance + 0x4F8, state.wheelSpeedRad, sizeof(state.wheelSpeedRad));
    for (size_t i = 0; i < 4; ++i) {
        state.wheelPhaseRad[i] = std::remainder(SanitizeFloat(state.wheelPhaseRad[i]), 6.283185307f);
        state.wheelSpeedRad[i] = std::clamp(SanitizeFloat(state.wheelSpeedRad[i]), -5000.0f, 5000.0f);
    }
    // Do not interpret geometry/resource members as lamp controllers.
    // Derive reverse motion from native velocity until lamp state is verified.
    state.reverseLight = (vel[0] * state.forwardX + vel[1] * state.forwardY < -0.05f) ? 1 : 0;

    // 6. Read Car Model Name (e.g. "COROLLA", "240SX")
    GetCarModelName(state.playerInstance, state.carModel, sizeof(state.carModel));

    return true;
}

bool IsTrafficModel(const char* modelName) {
    if (modelName == nullptr || modelName[0] == '\0') return false;
    static const char* kTrafficModels[] = {
        "TAXI", "MINIVAN", "WAGON", "PICKUP", "SUV", "HATCH", "COURIER",
        "AMBULANCE", "POLICE", "SEMI", "TRUCK", "VAN", "BUS", "TRAF"
    };
    for (const char* t : kTrafficModels) {
        if (_strnicmp(modelName, t, std::strlen(t)) == 0) {
            return true;
        }
    }
    return false;
}

// Scans world vehicles directly from the engine's authoritative Car array at 0x0089CCF8
int ScanWorldVehicles(uintptr_t localPlayerInstance, float localX, float localY, float localZ, const char* targetModel = nullptr) {
    gCandidateCount = 0;
    if (localPlayerInstance < 0x01000000) {
        return 0;
    }

    int found = 0;

    for (int i = 0; i < kMaxVehicleSlots; ++i) {
        uintptr_t candidate = 0;
        if (!ReadGameMemory(kCarArray + (i * sizeof(uintptr_t)), &candidate, sizeof(candidate)) ||
            candidate < 0x01000000 || candidate == localPlayerInstance) {
            continue;
        }

        // Avoid duplicate entries
        bool duplicate = false;
        for (int k = 0; k < found; ++k) {
            if (gCandidates[k].entityAddress == candidate) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        // Ensure vehicle has an active physics instance (PVehicle) and is not being destroyed
        uintptr_t pVehicle = 0;
        if (!ReadGameMemory(candidate + 0x34, &pVehicle, sizeof(pVehicle)) || pVehicle < 0x01000000) {
            continue;
        }

        // Read car type: 1 = Player, 2 = AI Racer (Outrun), 3 = Traffic
        int carType = 0;
        ReadGameMemory(kVehicleTypeArray + (i * sizeof(int)), &carType, sizeof(carType));
        if (carType == 1) continue; // Skip player vehicles

        // Read coordinates at Car + 0x60
        float pos[3]{};
        if (!ReadGameMemory(candidate + 0x60, pos, sizeof(pos))) {
            continue;
        }

        if (std::isnan(pos[0]) || std::isnan(pos[1]) || std::isnan(pos[2])) {
            continue;
        }

        const float dx = pos[0] - localX;
        const float dy = pos[1] - localY;
        const float dz = pos[2] - localZ;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        // Maximum streaming distance: never pick cars beyond 300m!
        // Cars beyond 300m are outside LOD streaming bubble and cause visual disappearance.
        if (dist > 300.0f) {
            continue;
        }

        if (found < kMaxCandidates) {
            gCandidates[found].entityAddress = candidate;
            gCandidates[found].x = pos[0];
            gCandidates[found].y = pos[1];
            gCandidates[found].z = pos[2];
            gCandidates[found].distance = dist;
            gCandidates[found].carType = carType;
            GetCarModelName(candidate, gCandidates[found].modelName, sizeof(gCandidates[found].modelName));

            found++;
        }
    }

    // Score-based candidate ranking:
    // Heavy proximity weight so nearby cars (within visual bubble) are ALWAYS preferred.
    // Matching car model gets a massive priority bonus (+1200 pts).
    // AI Racers (sports cars) get a +800 pts bonus to strongly prefer sports tuners over taxis.
    // The currently bound proxy car gets a +1500 pts retention bonus to eliminate flickering.
    std::sort(gCandidates, gCandidates + found, [targetModel](const WorldVehicleCandidate& a, const WorldVehicleCandidate& b) {
        float scoreA = 1000.0f - (a.distance * 5.0f);
        float scoreB = 1000.0f - (b.distance * 5.0f);

        if (targetModel != nullptr && targetModel[0] != '\0') {
            if (a.modelName[0] != '\0' && _stricmp(a.modelName, targetModel) == 0) scoreA += 1200.0f;
            if (b.modelName[0] != '\0' && _stricmp(b.modelName, targetModel) == 0) scoreB += 1200.0f;
        }

        if (a.carType == 2) scoreA += 800.0f;
        if (b.carType == 2) scoreB += 800.0f;

        if (a.entityAddress == gBoundProxyCar) scoreA += 1500.0f;
        if (b.entityAddress == gBoundProxyCar) scoreB += 1500.0f;

        return scoreA > scoreB; // Highest score first
    });

    gCandidateCount = found;
    return found;
}

static float gProxyWheelRollAngle = 0.0f;
static float gProxyWheelRollRad = 0.0f;
static float gProxyRenderWheelPhase[4]{};
static float gProxyRenderOrdinaryWheelRotation[4][9]{};
static uint32_t gProxyRenderOrdinaryWheelRotationMask = 0;
static uintptr_t gProxyRenderWheelCar = 0;
static DWORD gProxyRenderWheelAtMs = 0;
static volatile LONG gProxyWheelRenderSamples = 0;
static volatile LONG gProxyOrdinaryWheelRenderSamples = 0;

using NativeOrdinaryWheelMatricesFn = void(__thiscall*)(void*, float*, float*, uint32_t);
static NativeOrdinaryWheelMatricesFn gOriginalOrdinaryWheelMatrices = nullptr;

bool WriteNativeRendererOutput(float* destination, const float* source, size_t bytes) {
    // Native 0x6316D0 supplies stack matrices, commonly below 0x00400000.
    // SafeWriteBytes intentionally rejects such addresses for game objects;
    // do not weaken that guard. Only this native getter's output uses this path.
    if (!destination || !source || bytes == 0) return false;
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Native map layouts, verified in 4EC950/4AC2B0 and 4E0440/4B09E0.
struct NativeMapLinks { uintptr_t next, previous; };
struct NativeMinimapMarker {
    NativeMapLinks links;
    uint32_t id;
    float x, y, heading;
    uint32_t flags;
    uintptr_t icon;
};
struct NativeWorldMapMarker {
    NativeMapLinks links;
    uint32_t type;
    uintptr_t element;
    float x, y, headingRadians;
    float screenX, screenY, screenHeading;
    uint32_t extra;
};
static_assert(sizeof(NativeMinimapMarker) == 0x20, "Native minimap layout");
static_assert(sizeof(NativeWorldMapMarker) == 0x2C, "Native world map layout");
using NativeMinimapPositionFn = void(__thiscall*)(void*, float*, float*);
using NativeMinimapVisibilityFn = bool(__thiscall*)(void*);
using NativeMapAcquireIconFn = uintptr_t(__cdecl*)(uint32_t);
using NativeWorldMapDrawFn = void(__thiscall*)(void*);
using NativeMapVisibilityFn = void(__cdecl*)(void*);
using PeerMapPositionFn = bool(*)(PeerMapPosition&);
static NativeMinimapPositionFn gOriginalMinimapPosition = nullptr;
static NativeMinimapVisibilityFn gOriginalMinimapVisibility = nullptr;
static NativeMapAcquireIconFn gMapAcquireIcon = reinterpret_cast<NativeMapAcquireIconFn>(0x4EC730);
static NativeWorldMapDrawFn gOriginalWorldMapDraw = nullptr;
static FindUiElementByHashFn gMapFindElement = reinterpret_cast<FindUiElementByHashFn>(kFindUiElementByHash);
static NativeMapVisibilityFn gMapHideElement = reinterpret_cast<NativeMapVisibilityFn>(0x50CA00);
static PeerMapPositionFn gMapGetPeerPosition = &TryGetPeerMapPosition;
static uintptr_t gMinimapManagerPointer = 0x8364D4;
static volatile LONG gMinimapPeerSamples = 0;
static volatile LONG gWorldMapPeerSamples = 0;
static volatile LONG gMinimapResolverCalls = 0;
static volatile LONG gMinimapOwnedHits = 0;
static volatile LONG gMinimapBorrowedIcons = 0;

bool IsOwnedPeerMinimapMarker(uintptr_t marker) {
    NativeMinimapMarker item{};
    uintptr_t manager = 0;
    uint32_t iconType = 0;
    if (!ReadGameMemory(marker, &item, sizeof(item)) || item.id != 1 ||
        !ReadGameMemory(gMinimapManagerPointer, &manager, sizeof(manager))) return false;
    NativeMapLinks head{};
    if (!ReadGameMemory(manager, &head, sizeof(head))) return false;
    if (item.icon) ReadGameMemory(item.icon + 4, &iconType, sizeof(iconType));
    if (iconType != 4) {
        // PC HUD assets can omit PLAYERCARINDICATOR_2. The constructor still
        // creates its node immediately after the local-player node. Match that
        // verified role, never an unrelated opponent with world ID 1.
        NativeMinimapMarker first{};
        uint32_t firstType = 0;
        return (item.icon == 0 || iconType == 3) &&
            ReadGameMemory(head.next, &first, sizeof(first)) && first.id == 0 &&
            ReadGameMemory(first.icon + 4, &firstType, sizeof(firstType)) && firstType == 4 &&
            first.links.previous == manager && first.links.next == marker &&
            item.links.previous == head.next;
    }
    uintptr_t node = head.next;
    // The native pools contain 102 entries. Do not follow a damaged/cyclic list.
    for (unsigned n = 0; node != manager && n < 102; ++n) {
        NativeMapLinks links{};
        if (!ReadGameMemory(node, &links, sizeof(links))) return false;
        if (node == marker) return true;
        node = links.next;
    }
    return false;
}

void __fastcall HookedMinimapPosition(void* object, void*, float* xy, float* direction) {
    InterlockedIncrement(&gMinimapResolverCalls);
    const uintptr_t marker = reinterpret_cast<uintptr_t>(object);
    if (!IsOwnedPeerMinimapMarker(marker)) {
        if (gOriginalMinimapPosition) gOriginalMinimapPosition(object, xy, direction);
        return;
    }
    InterlockedIncrement(&gMinimapOwnedHits);
    PeerMapPosition position{};
    bool visible = gMapGetPeerPosition && gMapGetPeerPosition(position);
    uintptr_t icon = 0;
    ReadGameMemory(marker + 0x1C, &icon, sizeof(icon));
    if (visible && !icon && gMapAcquireIcon && gOriginalMinimapVisibility &&
        SafeWriteBytes(marker + 0x1C, &icon, sizeof(icon))) {
        // Take only an UNUSED opponent template from the game's free list.
        // The native minimap destructor returns this lease to that same pool.
        icon = gMapAcquireIcon(3);
        if (icon && SafeWriteBytes(marker + 0x1C, &icon, sizeof(icon))) {
            InterlockedIncrement(&gMinimapBorrowedIcons);
            LogMessage("[MAP] Missing Player 2 HUD icon replaced with an unused native opponent icon.\r\n");
        }
    }
    visible = visible && icon != 0;
    if (!visible) position = {};
    uint32_t flags = 0;
    ReadGameMemory(marker + 0x18, &flags, sizeof(flags));
    flags = visible ? (flags & ~uint32_t{2}) : (flags | uint32_t{2});
    SafeWriteBytes(marker + 0x18, &flags, sizeof(flags));
    // Native Player 2 normally has no local-player object; its original getter
    // leaves outputs uninitialized. Supply finite outputs even while hidden.
    constexpr float toRadians = 0.017453292519943295f;
    const float coordinates[2]{position.x, position.y};
    const float forward[2]{std::cos(position.headingDeg * toRadians),
        std::sin(position.headingDeg * toRadians)};
    WriteNativeRendererOutput(xy, coordinates, sizeof(coordinates));
    WriteNativeRendererOutput(direction, forward, sizeof(forward));
    if (visible) InterlockedIncrement(&gMinimapPeerSamples);
}

bool __fastcall HookedMinimapVisibility(void* object, void*) {
    if (!IsOwnedPeerMinimapMarker(reinterpret_cast<uintptr_t>(object)))
        return gOriginalMinimapVisibility ? gOriginalMinimapVisibility(object) : false;
    uintptr_t icon = 0;
    PeerMapPosition position{};
    return ReadGameMemory(reinterpret_cast<uintptr_t>(object) + 0x1C, &icon, sizeof(icon)) &&
        icon != 0 && gMapGetPeerPosition && gMapGetPeerPosition(position);
}

uint32_t MapElementHash(const char* name) {
    uint32_t hash = 0xFFFFFFFF;
    for (; *name; ++name) {
        const unsigned char c = static_cast<unsigned char>(*name);
        hash = hash * 33 + ((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
    }
    return hash;
}

bool ReadWorldMapElements(uintptr_t manager, uintptr_t* elements, unsigned& count,
    NativeMapLinks& head, uintptr_t* localElement = nullptr) {
    count = 0;
    if (localElement) *localElement = 0;
    const uintptr_t sentinel = manager + 4;
    if (!ReadGameMemory(sentinel, &head, sizeof(head))) return false;
    uintptr_t node = head.next, previous = sentinel;
    for (; node != sentinel && count < 512; ++count) {
        NativeWorldMapMarker item{};
        if (!ReadGameMemory(node, &item, sizeof(item)) || item.links.previous != previous)
            return false;
        elements[count] = item.element;
        if (localElement && item.type == 0x10 && !*localElement) *localElement = item.element;
        previous = node;
        node = item.links.next;
    }
    return node == sentinel && previous == head.previous;
}

#include "CoopPeerMapAppearance.inl"

void __fastcall HookedWorldMapDraw(void* object, void*) {
    if (!gOriginalWorldMapDraw) return;
    const uintptr_t manager = reinterpret_cast<uintptr_t>(object);
    uintptr_t package = 0, usedElements[512]{}, localElement = 0;
    unsigned count = 0;
    NativeMapLinks head{};
    char packageName[64]{};
    if (!ReadGameMemory(manager + 0x28, &package, sizeof(package)) ||
        !ReadGameMemory(package, packageName, sizeof(packageName) - 1) ||
        (std::strcmp(packageName, "UI_InGame_WorldMap.fng") != 0 &&
         std::strcmp(packageName, "UI_CareerWorldMap.fng") != 0) ||
        !ReadWorldMapElements(manager, usedElements, count, head, &localElement)) {
        gOriginalWorldMapDraw(object);
        return;
    }
    using namespace CoopPeerMapAppearance;
    // A previous spare can become an AI racer on a later frame. Return its
    // original appearance BEFORE native rendering; never turn real AI green.
    if (worldMap.element && (std::strcmp(packageName, worldMapPackage) ||
        std::find(usedElements, usedElements + count, worldMap.element) != usedElements + count ||
        reinterpret_cast<uintptr_t>(gMapFindElement(packageName, worldMapElementHash)) != worldMap.element))
        RestoreWorldMap();
    // Borrow only an unused native AI icon. Never steal the local player,
    // a shop/race/GPS marker, or an icon already in the manager-owned list.
    uintptr_t peerElement = 0; uint32_t peerElementHash = 0;
    for (unsigned index = 0; index < 8; ++index) {
        char name[32]{};
        std::snprintf(name, sizeof(name), "AIRACERINDICATOR_%u", index);
        const uintptr_t element = reinterpret_cast<uintptr_t>(
            gMapFindElement(packageName, MapElementHash(name)));
        if (!IsValidUserPtr(element) ||
            std::find(usedElements, usedElements + count, element) != usedElements + count) continue;
        gMapHideElement(reinterpret_cast<void*>(element));
        if (!peerElement) { peerElement = element; peerElementHash = MapElementHash(name); }
    }
    PeerMapPosition position{};
    if (!peerElement || !gMapGetPeerPosition || !gMapGetPeerPosition(position)) {
        RestoreWorldMap();
        gOriginalWorldMapDraw(object);
        return;
    }
    if (worldMap.element && worldMap.element != peerElement) RestoreWorldMap();
    if (Apply(worldMap, peerElement, package, localElement)) {
        worldMapElementHash = peerElementHash;
        std::strcpy(worldMapPackage, packageName);
    }
    const uintptr_t sentinel = manager + 4;
    NativeWorldMapMarker peer{};
    peer.links = {sentinel, head.previous};
    peer.type = 0x0E; // Native opponent: bypass event/shop discovery filters.
    peer.element = peerElement;
    peer.x = position.x;
    peer.y = position.y;
    peer.headingRadians = position.headingDeg * 0.017453292519943295f;
    const uintptr_t node = reinterpret_cast<uintptr_t>(&peer);
    // The renderer is synchronous and does not allocate/remove list entries.
    // Own the temporary node only for this call; native destruction never sees it.
    if (!SafeWriteBytes(head.previous, &node, sizeof(node))) {
        gOriginalWorldMapDraw(object);
        return;
    }
    if (!SafeWriteBytes(sentinel + 4, &node, sizeof(node))) {
        SafeWriteBytes(head.previous, &sentinel, sizeof(sentinel));
        gOriginalWorldMapDraw(object);
        return;
    }
    __try {
        gOriginalWorldMapDraw(object);
        InterlockedIncrement(&gWorldMapPeerSamples);
    } __finally {
        SafeWriteBytes(head.previous, &sentinel, sizeof(sentinel));
        SafeWriteBytes(sentinel + 4, &head.previous, sizeof(head.previous));
    }
}

bool InstallMapEntryHook(uintptr_t address, const BYTE* expected, size_t size,
    void* replacement, void** original) {
    auto* entry = reinterpret_cast<BYTE*>(address);
    BYTE actual[16]{};
    if (size > sizeof(actual) || !ReadGameMemory(address, actual, size) ||
        std::memcmp(actual, expected, size) != 0) return false;
    // Both verified prologues contain only complete, non-relative instructions.
    auto* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, size + 5,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    std::memcpy(trampoline, expected, size);
    trampoline[size] = 0xE9;
    const int32_t back = static_cast<int32_t>(address + size -
        (reinterpret_cast<uintptr_t>(trampoline) + size + 5));
    std::memcpy(trampoline + size + 1, &back, sizeof(back));
    FlushInstructionCache(GetCurrentProcess(), trampoline, size + 5);
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    *original = trampoline;
    const int32_t jump = static_cast<int32_t>(reinterpret_cast<uintptr_t>(replacement) - address - 5);
    entry[0] = 0xE9;
    std::memcpy(entry + 1, &jump, sizeof(jump));
    for (size_t i = 5; i < size; ++i) entry[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), entry, size);
    DWORD unused = 0;
    VirtualProtect(entry, size, oldProtect, &unused);
    return true;
}

void InstallPeerMapHooks() {
    const BYTE minimap[]{0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x1C};
    const BYTE worldmap[]{0x83, 0xEC, 0x50, 0x53, 0x56, 0x57};
    const BYTE appearance[]{0x83, 0xEC, 0x10, 0x56, 0x8B, 0xF1};
    const bool small = InstallMapEntryHook(0x4AC2B0, minimap, sizeof(minimap),
        reinterpret_cast<void*>(&HookedMinimapPosition), reinterpret_cast<void**>(&gOriginalMinimapPosition));
    const bool visibility = small && InstallMapEntryHook(0x4CA6D0, minimap, sizeof(minimap),
        reinterpret_cast<void*>(&HookedMinimapVisibility), reinterpret_cast<void**>(&gOriginalMinimapVisibility));
    const bool large = InstallMapEntryHook(0x4B09E0, worldmap, sizeof(worldmap),
        reinterpret_cast<void*>(&HookedWorldMapDraw), reinterpret_cast<void**>(&gOriginalWorldMapDraw));
    const bool green = small && visibility && InstallMapEntryHook(0x4AC460, appearance, sizeof(appearance),
        reinterpret_cast<void*>(&HookedMinimapAppearance), reinterpret_cast<void**>(&gOriginalMinimapAppearance));
    LogMessage(small ? "[MAP] Native minimap peer marker enabled.\r\n" :
        "[MAP] Minimap hook unavailable; existing map left untouched.\r\n");
    LogMessage(visibility ? "[MAP] Peer icon visibility override enabled.\r\n" :
        "[MAP] Peer icon visibility override unavailable.\r\n");
    LogMessage(large ? "[MAP] Native world-map peer marker enabled.\r\n" :
        "[MAP] World-map hook unavailable; existing map left untouched.\r\n");
    LogMessage(green ? "[MAP] Human peer arrow uses local player appearance; AI colors preserved.\r\n" :
        "[MAP] Peer appearance hook unavailable; existing minimap appearance preserved.\r\n");
}

// Native enum 0x8027BC: type 3 is CARFX_SKID_SMOKE (string at 0x7A8338).
// Sync generated visual requests, not guessed slip/contact physics fields.
using NativeSkidEffectFn = void(__thiscall*)(void*, int, float, uint32_t, const float*);
static NativeSkidEffectFn gOriginalSkidEffect = nullptr;
static SRWLOCK gSkidEffectLock = SRWLOCK_INIT;
static SkidEffectSample gLocalSkidEffects[4]{};
static DWORD gLocalSkidEffectAtMs[4]{};
static uintptr_t gLocalSkidEffectCar = 0;
static volatile LONG gLocalSkidEffectCaptures = 0;
static volatile LONG gPeerSkidEffectDispatches = 0;
static volatile LONG gPeerSkidEffectBlocked = 0;

bool IsValidSkidEffectSample(const SkidEffectSample& sample) {
    if (!std::isfinite(sample.intensity) || sample.intensity < 0.0f || sample.intensity > 1.0f ||
        sample.surface >= 29 || sample.hasVelocity > 1) return false;
    for (float component : sample.velocity)
        if (!std::isfinite(component) || std::fabs(component) > 500.0f) return false;
    return true;
}

bool CopyNativeSkidVelocity(const float* velocity, float* destination) {
    // Native optional velocity can be on a low-address stack, not a game heap.
    if (!velocity || !destination) return false;
    __try { std::memcpy(destination, velocity, sizeof(float) * 3); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    for (size_t component = 0; component < 3; ++component)
        if (!std::isfinite(destination[component]) || std::fabs(destination[component]) > 500.0f)
            return false;
    return true;
}

void __fastcall HookedSkidEffect(void* thisCar, void*, int effectType, float intensity,
    uint32_t packedWheelSurface, const float* velocity) {
    uintptr_t localCar = 0;
    const uintptr_t car = reinterpret_cast<uintptr_t>(thisCar);
    const uint32_t wheelMarker = packedWheelSurface >> 16;
    const uint32_t surface = packedWheelSurface & 0xFFFF;
    if (effectType == 3 && std::isfinite(intensity) && intensity > 0.0f &&
        wheelMarker >= 5 && wheelMarker <= 8 && surface < 29 &&
        ReadGameMemory(kCarArray, &localCar, sizeof(localCar)) && car == localCar) {
        SkidEffectSample sample{};
        sample.intensity = std::clamp(intensity, 0.0f, 1.0f);
        sample.surface = surface;
        if (CopyNativeSkidVelocity(velocity, sample.velocity)) sample.hasVelocity = 1;
        else std::memset(sample.velocity, 0, sizeof(sample.velocity));
        AcquireSRWLockExclusive(&gSkidEffectLock);
        if (gLocalSkidEffectCar != car) {
            std::memset(gLocalSkidEffects, 0, sizeof(gLocalSkidEffects));
            std::memset(gLocalSkidEffectAtMs, 0, sizeof(gLocalSkidEffectAtMs));
            gLocalSkidEffectCar = car;
        }
        gLocalSkidEffects[wheelMarker - 5] = sample;
        gLocalSkidEffectAtMs[wheelMarker - 5] = GetTickCount();
        ReleaseSRWLockExclusive(&gSkidEffectLock);
        InterlockedIncrement(&gLocalSkidEffectCaptures);
    }
    // All native effects still run with their original four-argument ABI.
    gOriginalSkidEffect(thisCar, effectType, intensity, packedWheelSurface, velocity);
}

bool CopyLocalSkidEffectSamples(SkidEffectSample destination[4]) {
    if (!destination) return false;
    std::memset(destination, 0, sizeof(SkidEffectSample) * 4);
    uintptr_t localCar = 0;
    if (!ReadGameMemory(kCarArray, &localCar, sizeof(localCar)) || !localCar) return false;
    const DWORD now = GetTickCount();
    bool haveActive = false;
    AcquireSRWLockShared(&gSkidEffectLock);
    if (localCar == gLocalSkidEffectCar) {
        for (size_t wheel = 0; wheel < 4; ++wheel) {
            if (gLocalSkidEffectAtMs[wheel] && now - gLocalSkidEffectAtMs[wheel] <= 100) {
                destination[wheel] = gLocalSkidEffects[wheel];
                haveActive |= destination[wheel].intensity > 0.0f;
            }
        }
    }
    ReleaseSRWLockShared(&gSkidEffectLock);
    return haveActive;
}

void ReplayPeerSkidEffects(uintptr_t proxyCar, const SkidEffectSample samples[4]) {
    if (!gOriginalSkidEffect || !samples || !gRemoteSyncEnabled ||
        !proxyCar || proxyCar != gBoundProxyCar) return;
    uintptr_t visual = 0;
    uint32_t poseMask = 0;
    if (!ReadGameMemory(proxyCar + 0x554, &visual, sizeof(visual)) || !visual ||
        !ReadGameMemory(proxyCar + 0x5B0, &poseMask, sizeof(poseMask)) || poseMask) {
        InterlockedIncrement(&gPeerSkidEffectBlocked);
        return;
    }
    for (size_t wheel = 0; wheel < 4; ++wheel) {
        if (!IsValidSkidEffectSample(samples[wheel]) || samples[wheel].intensity <= 0.0f) continue;
        const uint32_t packed = (static_cast<uint32_t>(wheel + 5) << 16) | samples[wheel].surface;
        gOriginalSkidEffect(reinterpret_cast<void*>(proxyCar), 3, samples[wheel].intensity,
            packed, samples[wheel].hasVelocity ? samples[wheel].velocity : nullptr);
        InterlockedIncrement(&gPeerSkidEffectDispatches);
    }
}

bool InstallSkidEffectHook() {
    auto* entry = reinterpret_cast<BYTE*>(0x0061FB00);
    // push ecx; mov eax,[ecx+554]. Native original returns RET 0x10.
    const BYTE original[] = {0x51, 0x8B, 0x81, 0x54, 0x05, 0x00, 0x00};
    if (std::memcmp(entry, original, sizeof(original)) != 0) {
        LogMessage("[ERROR] Native skid-effect signature differs; smoke sync hook not installed.\r\n");
        return false;
    }
    auto* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, sizeof(original) + 5,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    std::memcpy(trampoline, original, sizeof(original));
    trampoline[sizeof(original)] = 0xE9;
    const int32_t returnJump = static_cast<int32_t>(reinterpret_cast<intptr_t>(entry + sizeof(original)) -
        (reinterpret_cast<intptr_t>(trampoline) + sizeof(original) + 5));
    std::memcpy(trampoline + sizeof(original) + 1, &returnJump, sizeof(returnJump));
    FlushInstructionCache(GetCurrentProcess(), trampoline, sizeof(original) + 5);
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry, sizeof(original), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    gOriginalSkidEffect = reinterpret_cast<NativeSkidEffectFn>(trampoline);
    const int32_t jump = static_cast<int32_t>(reinterpret_cast<intptr_t>(&HookedSkidEffect) -
        (reinterpret_cast<intptr_t>(entry) + 5));
    entry[0] = 0xE9;
    std::memcpy(entry + 1, &jump, sizeof(jump));
    std::memset(entry + 5, 0x90, sizeof(original) - 5);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(original));
    DWORD unused = 0;
    VirtualProtect(entry, sizeof(original), oldProtect, &unused);
    LogMessage("[HOOK] Native CARFX_SKID_SMOKE requests captured; physics remains unchanged.\r\n");
    return true;
}

void __fastcall HookedOrdinaryWheelMatrices(void* thisCar, void*, float* tireMatrices,
    float* hubMatrices, uint32_t viewport) {
    // Preserve the complete native getter, including this receiver's wheel
    // translations, suspension, viewport handling and hub/caliper matrices.
    gOriginalOrdinaryWheelMatrices(thisCar, tireMatrices, hubMatrices, viewport);
    const uintptr_t car = reinterpret_cast<uintptr_t>(thisCar);
    if (!gRemoteSyncEnabled || gProxyRenderOrdinaryWheelRotationMask == 0 ||
        car != gBoundProxyCar || car != gProxyRenderWheelCar || tireMatrices == nullptr ||
        GetTickCount() - gProxyRenderWheelAtMs > 250) return;
    for (size_t wheel = 0; wheel < 4; ++wheel) {
        if ((gProxyRenderOrdinaryWheelRotationMask & (uint32_t{1} << wheel)) == 0) continue;
        for (size_t row = 0; row < 3; ++row) {
            if (!WriteNativeRendererOutput(tireMatrices + wheel * 16 + row * 4,
                gProxyRenderOrdinaryWheelRotation[wheel] + row * 3, sizeof(float) * 3)) return;
        }
    }
    InterlockedIncrement(&gProxyOrdinaryWheelRenderSamples);
}

bool InstallOrdinaryWheelMatrixHook() {
    auto* entry = reinterpret_cast<BYTE*>(0x005F6820);
    // push ebx; mov ebx,ecx; mov ecx,[esp+10]. Native getter returns RET 0xC.
    const BYTE original[] = {0x53, 0x8B, 0xD9, 0x8B, 0x4C, 0x24, 0x10};
    if (std::memcmp(entry, original, sizeof(original)) != 0) {
        LogMessage("[ERROR] Ordinary wheel-matrix signature differs; hook not installed.\r\n");
        return false;
    }
    auto* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, sizeof(original) + 5,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) return false;
    std::memcpy(trampoline, original, sizeof(original));
    trampoline[sizeof(original)] = 0xE9;
    const int32_t returnJump = static_cast<int32_t>(reinterpret_cast<intptr_t>(entry + sizeof(original)) -
        (reinterpret_cast<intptr_t>(trampoline) + sizeof(original) + 5));
    std::memcpy(trampoline + sizeof(original) + 1, &returnJump, sizeof(returnJump));
    FlushInstructionCache(GetCurrentProcess(), trampoline, sizeof(original) + 5);
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry, sizeof(original), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    gOriginalOrdinaryWheelMatrices = reinterpret_cast<NativeOrdinaryWheelMatricesFn>(trampoline);
    const int32_t jump = static_cast<int32_t>(reinterpret_cast<intptr_t>(&HookedOrdinaryWheelMatrices) -
        (reinterpret_cast<intptr_t>(entry) + 5));
    entry[0] = 0xE9;
    std::memcpy(entry + 1, &jump, sizeof(jump));
    std::memset(entry + 5, 0x90, sizeof(original) - 5);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(original));
    DWORD unused = 0;
    VirtualProtect(entry, sizeof(original), oldProtect, &unused);
    LogMessage("[HOOK] Ordinary peer wheel rotations applied after native tire/rim matrix getter.\r\n");
    return true;
}

void __cdecl ApplyProxyWheelRenderSnapshot(uintptr_t car) {
    if (!gRemoteSyncEnabled || car != gBoundProxyCar || car != gProxyRenderWheelCar ||
        GetTickCount() - gProxyRenderWheelAtMs > 250) return;
    SafeWriteBytes(car + 0x4E8, gProxyRenderWheelPhase, sizeof(gProxyRenderWheelPhase));
    InterlockedIncrement(&gProxyWheelRenderSamples);
}

// Verified 0x5EC7F0 constructs optional spinner wheel matrices for the renderer.
// Its indexed read is (Car+380+wheel*4)+168 == Car+4E8+wheel*4.
// Re-assert the network phase at that actual consumer, after the native remote
// simulation has had a chance to overwrite the earlier UiTick injection.
__declspec(naked) void HookedProxyWheelMatrices() {
    __asm {
        pushad
        push ecx
        call ApplyProxyWheelRenderSnapshot
        add esp, 4
        popad
        push ecx
        mov edx, ecx
        mov ecx, [esp + 0x0c]
        push 0x005EC7F7
        ret
    }
}

bool InstallProxyWheelMatrixHook() {
    InstallOrdinaryWheelMatrixHook();
    auto* entry = reinterpret_cast<BYTE*>(0x005EC7F0);
    const BYTE original[] = {0x51, 0x8B, 0xD1, 0x8B, 0x4C, 0x24, 0x0C};
    if (std::memcmp(entry, original, sizeof(original)) != 0) {
        LogMessage("[ERROR] Wheel matrix entry differs; render hook not installed.\r\n");
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry, sizeof(original), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    const int32_t jump = static_cast<int32_t>(reinterpret_cast<intptr_t>(&HookedProxyWheelMatrices) -
        (0x005EC7F0 + 5));
    entry[0] = 0xE9;
    std::memcpy(entry + 1, &jump, sizeof(jump));
    std::memset(entry + 5, 0x90, 2);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(original));
    DWORD unused = 0;
    VirtualProtect(entry, sizeof(original), oldProtect, &unused);
    LogMessage("[HOOK] Peer spinner phase applied at native spinner-matrix consumer.\r\n");
    return true;
}

void ReloadCarVisualsFromPacket(uintptr_t carPtr, const CarCustomizationPacket& pkt) {
    // Note: Live SetupRide calls mid-frame cause D3D crashes (nvd3dum.dll / AcLayers.DLL).
    // All car models, paint, and parts are loaded cleanly during RaceLoadingLoop.
}

static void CopyPortableRideValues(uint8_t* destination, const uint8_t* source) {
    // Keep this process's spline, performance cache and texture identities.
    // +3C0 is a native cache pointer even though it sits among value fields.
    std::memcpy(destination, source, 0x38C);
    std::memcpy(destination + 0x394, source + 0x394, 0x2C);
    std::memcpy(destination + 0x3C4, source + 0x3C4, 0x1B8);
    std::memcpy(destination + 0x580, source + 0x580, 4);
    std::memcpy(destination + 0x838, source + 0x838, 0xAA);
}

void SerializeVehicleParamPortable(uint8_t* outParam, const uint8_t* inParam) {
    if (!outParam || !inParam) return;
    // Never send process-local RideInfo, streaming or spline pointers to another
    // game process.  These POD ranges are verified against MenuCarInstance's
    // constructor and CopyFrom in this executable (0x40C910 / 0x413440).
    std::memset(outParam, 0, 0x940);
    const uint8_t* pRide = inParam + 0x10;
    uint8_t* pRideOut = outParam + 0x10;
    CopyPortableRideValues(pRideOut, pRide);
    // +584/+588/+58C are local DUMMY_SKIN/WHEEL/SPINNER texture identities.
    // Both senders use their own slot 0: copying these aliases the local paint.
    for (uint32_t slot = 0; slot < 170; ++slot) {
        uintptr_t pPart = 0;
        uint32_t partHash = 0;
        ReadGameMemory(reinterpret_cast<uintptr_t>(pRide + 0x590 + slot * 4), &pPart, sizeof(pPart));
        if (IsValidUserPtr(pPart)) ReadGameMemory(pPart, &partHash, sizeof(partHash));
        std::memcpy(pRideOut + 0x590 + slot * 4, &partHash, sizeof(partHash));
    }
}

bool IsSupportedCarModel(uint32_t modelID) {
    return CoopCarCatalogNative::Supported(modelID);
}

bool IsReadyPeerSelection(const CarSyncPacket& pkt, uint32_t expectedPlayer,
    DWORD receivedAtMs, DWORD nowMs) {
    uint32_t model = UINT32_MAX;
    std::memcpy(&model, pkt.vehicleParam + 0x10, sizeof(model));
    return pkt.magic == kPacketMagic && pkt.version == kPacketVersion &&
        pkt.size == sizeof(pkt) && pkt.playerIndex == expectedPlayer &&
        (pkt.selectionStage == 1 || pkt.selectionStage == 2) &&
        coop_time::Elapsed(nowMs,receivedAtMs) <= 5000 &&
        AppearanceGeneration(pkt)==CurrentPeerGeneration()&&pkt.sequence&&
        coop_car_catalog::PortableModelIndex(model);
}

bool HaveReadyPeerSelection() {
    AcquireSRWLockShared(&gPeerSyncLock);
    const bool ready = gHavePeerSyncPkt && IsReadyPeerSelection(gLatestPeerSyncPkt,
        gIsSecondInstance ? 1 : 2, gPeerSyncReceivedAtMs, GetTickCount());
    ReleaseSRWLockShared(&gPeerSyncLock);
    return ready;
}

static void QueueLocalPeerSelection(CarSyncPacket& packet){
    if(!ScopeLocalAppearance(packet))return;
    static SRWLOCK outputLock=SRWLOCK_INIT;static CarSyncPacket last{};static DWORD lastAt=0;static bool sent=false;
    const DWORD now=GetTickCount();const auto endpoint=CoopSnapshot().peer;
    AcquireSRWLockExclusive(&outputLock);
    const bool unchanged=sent&&AppearanceGeneration(last)==AppearanceGeneration(packet)&&last.selectionStage==packet.selectionStage&&
        !std::memcmp(last.vehicleParam,packet.vehicleParam,sizeof(packet.vehicleParam));
    // Loading callbacks can run at rendering frequency. Repeated identical
    // 2.4 KiB reliable messages must not become a delayed bulk-data backlog.
    if(!unchanged||coop_time::Elapsed(now,lastAt)>=400){
        if(SendMeasuredCoopDatagram(&packet,sizeof(packet),endpoint)==sizeof(packet)){last=packet;lastAt=now;sent=true;}
    }
    ReleaseSRWLockExclusive(&outputLock);
}
void SendPortableCarSelection(const uint8_t* param, uint32_t stage) {
    if (!param || gGameSocket == INVALID_SOCKET) return;
    CarSyncPacket pkt{};
    pkt.magic = kPacketMagic;
    pkt.version = kPacketVersion;
    pkt.size = sizeof(pkt);
    pkt.playerIndex = gIsSecondInstance ? 2 : 1;
    pkt.sentAtMs = GetTickCount();
    pkt.selectionStage = stage;
    SerializeVehicleParamPortable(pkt.vehicleParam, param);
    QueueLocalPeerSelection(pkt);
}

void SendRaceEvent(uint16_t eventType, uint32_t seed = 0) {
    if (gGameSocket != INVALID_SOCKET) {
        const auto session=CoopSnapshot();
        if(!session.joined || !session.fresh || gPendingRaceEvent.sessionId!=session.sessionId ||
            gPendingRaceEvent.membershipNonce!=session.membershipNonce || gPendingRaceEvent.ticket!=session.ticket)return;
        RaceEventPacket pkt=gPendingRaceEvent;
        pkt.magic = kRaceEventMagic;
        pkt.version = kRacePacketVersion;
        pkt.eventType = eventType;
        pkt.gameMode = gPendingRaceEvent.gameMode;
        pkt.trackId = gPendingRaceEvent.trackId;
        pkt.laps = gPendingRaceEvent.laps;
        pkt.direction = gPendingRaceEvent.direction;
        pkt.senderPlayer = gIsSecondInstance ? 2 : 1;
        pkt.seed = seed;
        SendMeasuredCoopDatagram(&pkt,sizeof(pkt),session.peer);
    }
}

void ApplyRaceParameters(uint32_t trackId, uint32_t mode, uint32_t laps, uint32_t direction) {
    if (trackId == 4000) {
        // Free Roam Career: Bayview open world
        *reinterpret_cast<uint32_t*>(0x0089E7A0) = 4000;
        *reinterpret_cast<uint32_t*>(0x0089E7A4) = 0;
        *reinterpret_cast<uint32_t*>(0x0089E7B0) = 0; // Free Roam mode
        *reinterpret_cast<uint32_t*>(0x0089E7B4) = 0;
        *reinterpret_cast<uint32_t*>(0x0089E7BC) = 1;
        *reinterpret_cast<uint32_t*>(0x0089E7C0) = 1;
        *reinterpret_cast<uint32_t*>(0x0089E7C4) = 1;
        *reinterpret_cast<uint8_t*>(0x0089E7E4) = 0;  // Inactive race flag in free roam

        *reinterpret_cast<uint32_t*>(0x00850078) = 1;    // Career mode! (1 = Career, 0x200 = LAN, 8 = Online)
        *reinterpret_cast<uint32_t*>(0x00850088) = 0;
        *reinterpret_cast<uint16_t*>(0x008500B8) = 4000;
        *reinterpret_cast<uint16_t*>(0x008500BA) = 1;
        *reinterpret_cast<uint32_t*>(0x008500BC) = 0;

        *reinterpret_cast<uint32_t*>(0x0083AAB4) = 0;
        return;
    }

    mode = CleanGameMode(mode);
    if (laps == 0) laps = 2;

    // 1. RaceParameters at 0x0089E7A0
    *reinterpret_cast<uint32_t*>(0x0089E7A0) = trackId;
    *reinterpret_cast<uint32_t*>(0x0089E7A4) = direction;
    *reinterpret_cast<uint32_t*>(0x0089E7B0) = mode; // GameMode
    // 0x0089E7B4: IsPointToPoint / Sprint. MUST be 0 for Circuit!
    // If non-zero, native function 0x0053FD30 forces laps = 1!
    *reinterpret_cast<uint32_t*>(0x0089E7B4) = (mode == 2) ? 0 : 1;
    *reinterpret_cast<uint32_t*>(0x0089E7BC) = laps;
    *reinterpret_cast<uint32_t*>(0x0089E7C0) = 1; // 1 local player (NOT split-screen!)
    *reinterpret_cast<uint32_t*>(0x0089E7C4) = 2; // Legacy local-AI budget, NOT total racers; co-op stage4 reconciles the actual list.
    *reinterpret_cast<uint8_t*>(0x0089E7E4) = 1;  // RaceActive flag!

    // 2. WorldEvent at 0x008500A0
    *reinterpret_cast<uint32_t*>(0x00850078) = 8; // Career / Circuit event
    *reinterpret_cast<uint32_t*>(0x00850088) = mode;
    *reinterpret_cast<uint16_t*>(0x008500B8) = static_cast<uint16_t>(trackId); // TrackID is 0x008500B8!
    *reinterpret_cast<uint16_t*>(0x008500BA) = static_cast<uint16_t>(laps);    // Laps is 0x008500BA!
    *reinterpret_cast<uint32_t*>(0x008500BC) = direction;                      // Direction is 0x008500BC!

    // 3. Engine Global GameMode at 0x0083AAB4
    *reinterpret_cast<uint32_t*>(0x0083AAB4) = mode;
}

void NativeGameFlowSwitchTrack(uint32_t trackId, uint32_t mode, uint32_t laps, uint32_t direction) {
    ApplyRaceParameters(trackId, mode, laps, direction);
    *reinterpret_cast<uint32_t*>(0x007FAA08) = (mode == 0 && trackId == 4000) ? 0x10 : 0;

    // Reset FxManager (0x008357E0) active nodes cleanly before switching track - only if particles initialized!
    void* pFxPool = *reinterpret_cast<void**>(0x0082DA3C);
    if (pFxPool != nullptr) {
        reinterpret_cast<void(__thiscall*)(void*)>(0x00491E10)(reinterpret_cast<void*>(0x008357E0));
    }

    // Native track switches start in RaceFlow state 0 (0x006037C0), which
    // schedules 0x005819F0. State 1 ticks traffic via 0x00603A4E and cannot
    // remain active while 0x0060A9A0 releases the racer list at 0x00890080.
    // Use the native state transition before a helper-initiated gameplay switch.
    DWORD currentFlow = 0;
    ReadGameMemory(kGameFlowState, &currentFlow, sizeof(currentFlow));
    if (currentFlow == 6) {
        uintptr_t pRaceFlow = 0;
        if (ReadGameMemory(0x008900D8, &pRaceFlow, sizeof(pRaceFlow)) && IsValidUserPtr(pRaceFlow)) {
            uint32_t currentState = 0;
            ReadGameMemory(pRaceFlow, &currentState, sizeof(currentState));
            if (currentState != 0) {
                reinterpret_cast<void(__thiscall*)(void*, int)>(0x00603630)(
                    reinterpret_cast<void*>(pRaceFlow), 0);
                LogMessage("[RACE] Transitioned RaceFlow to native switch state 0 before track switch.\r\n");
            }
        }
    }

    void* pGameFlow = reinterpret_cast<void*>(0x00865480);
    if (pGameFlow) {
        // 0x005819F0 is GameFlow::BeginGameFlowSwitchTrack (__thiscall on 0x00865480)
        reinterpret_cast<void(__thiscall*)(void*)>(0x005819F0)(pGameFlow);
    }
}

// 0x57F2D0 is a void main-thread loading callback. It increments its stage
// first. Gate before native streaming, palette selection, and palette commit;
// resume the same callback from UiTick, never by a blocking network wait.
// No blocking wait, window-message pump, or mid-race model replacement.
static bool PrepareCoopFleetLoadingStage(uint32_t stage) {
    if (!gCoopFleetEnabled) return true;
    uint32_t track = 0; uint8_t trafficDisabled = 0;
    if (!ReadGameMemory(0x0089E7A0, &track, 4) || !track ||
        !ReadGameMemory(0x0089E7E4, &trafficDisabled, 1) || trafficDisabled > 1) return false;
    coop_world::Identity identity{};
    if (stage == 3) {
        if (!gCoopFleetLocalLoadToken) ++gCoopFleetLocalLoadToken;
        const bool freeRoam = track == 4000;
        const uint32_t raceEpoch = freeRoam ? 0 : gPendingRaceEvent.seed;
        if (!freeRoam && (gPendingRaceEvent.trackId != track || !raceEpoch)) return false;
        if (BeginCoopFleetIdentityAndLoadingBarrier(track, raceEpoch, freeRoam,
            gCoopFleetLocalLoadToken, trafficDisabled == 0, identity) != FleetLoadResult::Ready) return false;
        return PrepareCoopFleetBeforeStreaming(identity, freeRoam);
    }
    if (!CurrentCoopFleetIdentity(identity) || identity.trackId != track) return false;
    if (stage == 5) {
        return CoopSnapshot().role != coop::Role::Client || CoopFleetSelectionPlanReady(identity);
    }
    return stage != 6 || CoopFleetAssetsReadyForStage7(identity);
}

static bool ReadCoopFinalizedLoadingSelection(uintptr_t& params) {
    uint32_t model=UINT32_MAX;
    return ReadGameMemory(0x0089E810,&params,sizeof(params)) && IsValidUserPtr(params) &&
        ReadGameMemory(params+0x10,&model,sizeof(model)) && IsSupportedCarModel(model);
}

static void TraceCoopNativeLoadingWait(const char* reason) {
    // Game-thread diagnostic only. Never advances a native stage or relaxes a
    // readiness predicate; do not infer RTT from a locally observed packet age.
    static uintptr_t lastLoader=0;static uint32_t lastStage=0,lastTicket=0;
    static uint64_t lastSession=0;static DWORD lastAt=0;static char lastReason[80]{};
    const DWORD now=GetTickCount();const auto session=CoopSnapshot();
    if(lastLoader==gPendingTrackLoader && lastStage==gPendingTrackLoaderStage &&
        lastTicket==session.ticket && lastSession==session.sessionId &&
        !std::strcmp(lastReason,reason) && now-lastAt<3000)return;
    lastLoader=gPendingTrackLoader;lastStage=gPendingTrackLoaderStage;
    lastTicket=session.ticket;lastSession=session.sessionId;lastAt=now;
    std::snprintf(lastReason,sizeof(lastReason),"%s",reason);
    uint32_t flow=UINT32_MAX,stage=UINT32_MAX,track=0,model=UINT32_MAX;uintptr_t params=0;
    ReadGameMemory(kGameFlowState,&flow,4);ReadGameMemory(0x89E7A0,&track,4);
    if(gPendingTrackLoader)ReadGameMemory(gPendingTrackLoader,&stage,4);
    if(ReadGameMemory(0x89E810,&params,sizeof(params)) && IsValidUserPtr(params))ReadGameMemory(params+0x10,&model,4);
    bool peerHave=false;uint32_t peerStage=0,peerModel=UINT32_MAX;DWORD peerAge=UINT32_MAX;
    AcquireSRWLockShared(&gPeerSyncLock);
    peerHave=gHavePeerSyncPkt;
    if(peerHave) {
        peerStage=gLatestPeerSyncPkt.selectionStage;peerAge=now-gPeerSyncReceivedAtMs;
        std::memcpy(&peerModel,gLatestPeerSyncPkt.vehicleParam+0x10,4);
    }
    ReleaseSRWLockShared(&gPeerSyncLock);
    char line[720]{};
    std::snprintf(line,sizeof(line),
        "[LOAD_WAIT] reason=%s loader=%08X heldStage=%u actualStage=%u flow=%u track=%u wait=%ums role=%u session=%llu ticket=%u joined=%u fresh=%u phase=%u initial=%u/%u localReady=%u peerReady=%u params=%08X model=%u peerSelection=%u stage=%u model=%u age=%ums AI=%u Fleet=%u raceState=%u.\r\n",
        reason,static_cast<unsigned>(gPendingTrackLoader),gPendingTrackLoaderStage,stage,flow,track,
        now-gPendingTrackLoaderStartedAt,static_cast<unsigned>(session.role),static_cast<unsigned long long>(session.sessionId),
        session.ticket,session.joined?1u:0u,session.fresh?1u:0u,static_cast<unsigned>(session.phase),
        session.initialBarrierStarted?1u:0u,session.initialBarrierComplete?1u:0u,
        session.localInitialReady?1u:0u,session.peerInitialReady?1u:0u,static_cast<unsigned>(params),model,
        peerHave?1u:0u,peerStage,peerModel,peerAge,gCoopAIEnabled?1u:0u,gCoopFleetEnabled?1u:0u,
        static_cast<unsigned>(gCoopRaceState));
    LogMessage(line);
}

static void HoldCoopNativeLoader(void* loader, uint32_t stage,const char* reason="assets") {
    if (gPendingTrackLoader != reinterpret_cast<uintptr_t>(loader) || gPendingTrackLoaderStage != stage) {
        gPendingTrackLoaderStartedAt = GetTickCount();
        char line[144]{};
        std::snprintf(line,sizeof(line),"[COOP_READY] Native loading stage %lu held for current-session assets.\r\n",
            static_cast<unsigned long>(stage));
        LogMessage(line);
    }
    gPendingTrackLoader = reinterpret_cast<uintptr_t>(loader);
    gPendingTrackLoaderStage = stage;
    TraceCoopNativeLoadingWait(reason);
}

bool __cdecl DelayTrackLoadingForPeer(void* loader) {
    if (!IsCoopGameplayActive()) {
        return false;
    }
    uint32_t stage = 0;
    if (!ReadGameMemory(reinterpret_cast<uintptr_t>(loader), &stage, sizeof(stage))) return false;
    if ((stage==3 || stage==5 || stage==6) && CoopRaceLoadingFailureMessage()) {
        HoldCoopNativeLoader(loader,stage,"race-validation");
        return true;
    }
    if (stage != 3) {
        if (gCoopFleetEnabled && (stage == 5 || stage == 6) && !PrepareCoopFleetLoadingStage(stage)) {
            HoldCoopNativeLoader(loader, stage,stage==5?"fleet-palette":"fleet-resources");
            return true;
        }
        return false;
    }
    if (!FinalizeCoopNativeRaceLoading(loader)) {
        HoldCoopNativeLoader(loader, stage,"native-race-finalization");
        return true;
    }
    uintptr_t params = 0;
    if(!ReadCoopFinalizedLoadingSelection(params)) {
        const auto session=CoopSnapshot();
        // This callback precedes native Stage4; that body consumes, but does
        // not initialize, the finalized selection. Letting it through here
        // also omits this ticket's InitialReady and can strand the other peer.
        if(session.initialBarrierActive && session.joined && session.fresh && session.ticket) {
            HoldCoopNativeLoader(loader,stage,"local-selection");return true;
        }
        return false; // Preserve standalone/unarmed native loading behavior.
    }
    gLocalSelectionFinalized = true;
    SendPortableCarSelection(reinterpret_cast<const uint8_t*>(params), 1);

    // Publish only finalized native parameters, after Career initialized them.
    if (gCoopRaceState == COOP_RACE_HOST_WAITING_ACCEPT) {
        HoldCoopNativeLoader(loader, stage,"race-consent");
        if (!gRaceProposalPrepared && PopulateNativeCoopRaceProposal()) {
            gRaceProposalPrepared = true;
            LogMessage("[RACE] Finalized native Career event published; waiting for acceptance.\r\n");
        }
        return true;
    }

    if (!gCoopFleetEnabled && gCoopRaceState == COOP_RACE_IDLE && !InitialCoopLoadingBarrier()) {
        gPendingTrackLoader = 0;
        return false;
    }
    const bool aiReady=!gCoopAIEnabled || PrepareCoopAIRosterBeforeStreaming();
    // AI and ambient/traffic manifests only stage private data. Their transfers
    // are independent and may overlap; neither mutates the list before Stage4.
    const bool fleetReady = PrepareCoopFleetLoadingStage(stage);
    const bool initialBarrier = InitialCoopLoadingBarrier();
    const bool humanReady = (gCoopRaceState == COOP_RACE_IDLE && !initialBarrier) || HaveReadyPeerSelection();
    if (humanReady && !initialBarrier && aiReady && fleetReady) {
        if (gCoopFleetEnabled && !SealCoopFleetLoadingIdentity(CoopFleetLocalGeneration())) {
            HoldCoopNativeLoader(loader, stage,"fleet-identity-seal");
            return true;
        }
        gPendingTrackLoader = 0;
        return false;
    }
    HoldCoopNativeLoader(loader, stage,!aiReady?"ai-roster":!fleetReady?"fleet-roster":
        initialBarrier?"initial-peer-ready":"human-selection");
    return true;
}

void ResumePendingCoopLoading(DWORD flowState) {
    if (!gPendingTrackLoader) return;
    if (flowState != 4 && flowState != 5) {
        TraceCoopNativeLoadingWait("retired-world-changed");
        gPendingTrackLoader = 0;
        return;
    }
    uint32_t stage = 0;
    if (!ReadGameMemory(gPendingTrackLoader, &stage, sizeof(stage)) ||
        stage != gPendingTrackLoaderStage || (stage != 3 && stage != 5 && stage != 6)) {
        TraceCoopNativeLoadingWait("retired-callback-changed");
        gPendingTrackLoader = 0;
        return;
    }
    if (CoopRaceLoadingFailureMessage()) {TraceCoopNativeLoadingWait("race-validation");return;}
    static DWORD lastSend = 0;
    if (stage == 3 && GetTickCount() - lastSend >= 250) {
        uintptr_t params = 0;
        if (ReadCoopFinalizedLoadingSelection(params))
            SendPortableCarSelection(reinterpret_cast<const uint8_t*>(params), 1);
        lastSend = GetTickCount();
    }

    // Consent now completes before native teardown; loading never starts a
    // second track switch to cancel a held invitation.
    if (stage == 3 && (gCoopRaceState == COOP_RACE_HOST_WAITING_ACCEPT ||
        gCoopRaceState == COOP_RACE_REQUESTING || gCoopRaceState == COOP_RACE_CLIENT_INVITED)) {
        TraceCoopNativeLoadingWait("race-consent");return;
    }

    if (stage == 3) {
        if (!FinalizeCoopNativeRaceLoading(reinterpret_cast<void*>(gPendingTrackLoader))) {
            TraceCoopNativeLoadingWait("native-race-finalization");return;
        }
        const auto session=CoopSnapshot();uintptr_t params=0;
        if(session.initialBarrierActive && session.joined && session.fresh && session.ticket &&
            !ReadCoopFinalizedLoadingSelection(params)) {
            TraceCoopNativeLoadingWait("local-selection");return;
        }
        if (InitialCoopLoadingBarrier()) {TraceCoopNativeLoadingWait("initial-peer-ready");return;}
        if (gCoopRaceState != COOP_RACE_IDLE && !HaveReadyPeerSelection()) {
            TraceCoopNativeLoadingWait("human-selection");return;
        }
        const bool aiReady=!gCoopAIEnabled || PrepareCoopAIRosterBeforeStreaming();
        const bool fleetReady=PrepareCoopFleetLoadingStage(stage);
        if(!aiReady || !fleetReady) {TraceCoopNativeLoadingWait(!aiReady?"ai-roster":"fleet-roster");return;}
    }
    if (!PrepareCoopFleetLoadingStage(stage)) {
        TraceCoopNativeLoadingWait(stage==3?"fleet-roster":stage==5?"fleet-palette":"fleet-resources");return;
    }
    if (stage == 3 && gCoopFleetEnabled && !SealCoopFleetLoadingIdentity(CoopFleetLocalGeneration())) {
        TraceCoopNativeLoadingWait("fleet-identity-seal");return;
    }
    const uintptr_t loader = gPendingTrackLoader;
    gPendingTrackLoader = 0;
    char timing[128]{};
    std::snprintf(timing,sizeof(timing),"[LOAD_TIMING] stage=%u network/assets wait=%lu ms.\r\n",
        stage,static_cast<unsigned long>(GetTickCount()-gPendingTrackLoaderStartedAt));
    LogMessage(timing);
    LogMessage("[COOP_READY] Current loading barrier cleared; native streaming resumed.\r\n");
    reinterpret_cast<void(__thiscall*)(void*)>(0x0057F2D0)(reinterpret_cast<void*>(loader));
}

__declspec(naked) void HookedTrackLoadingReadyCheck() {
    __asm {
        pushad
        push ecx
        call DelayTrackLoadingForPeer
        add esp, 4
        // MSVC x86 bool defines AL only. Stale upper EAX bits must not
        // suppress every loading stage when the callback returns false.
        movzx eax, al
        mov [esp + 0x1c], eax
        popad
        test eax, eax
        jnz pending_peer
        sub esp, 0x248
        push 0x0057F2D6
        ret
    pending_peer:
        ret
    }
}

bool InstallTrackLoadingReadyCheck() {
    auto* entry = reinterpret_cast<BYTE*>(0x0057F2D0);
    const BYTE original[] = {0x81, 0xEC, 0x48, 0x02, 0x00, 0x00};
    if (std::memcmp(entry, original, sizeof(original)) != 0) {
        LogMessage("[ERROR] Track loading entry differs; readiness hook not installed.\r\n");
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry, sizeof(original), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    const int32_t jump = static_cast<int32_t>(reinterpret_cast<intptr_t>(&HookedTrackLoadingReadyCheck) -
        (0x0057F2D0 + 5));
    entry[0] = 0xE9;
    std::memcpy(entry + 1, &jump, sizeof(jump));
    entry[5] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(original));
    DWORD unused = 0;
    VirtualProtect(entry, sizeof(original), oldProtect, &unused);
    LogMessage("[HOOK] Nonblocking peer-car readiness check installed before native streaming.\r\n");
    return true;
}

void BroadcastLocalCarCustomization() {
    uintptr_t carArrayPtr = 0;
    uint8_t* pRide = nullptr;
    if (ReadGameMemory(0x0089E810, &carArrayPtr, sizeof(carArrayPtr)) && carArrayPtr >= 0x00400000) {
        pRide = reinterpret_cast<uint8_t*>(carArrayPtr + 0x10);
    } else return; // 0x890160 is VehicleParam storage, NOT a frontend RideInfo.
    if (!pRide) return;
    uint32_t modelID = *reinterpret_cast<uint32_t*>(pRide);
    if (!IsSupportedCarModel(modelID)) return;

    CarCustomizationPacket pkt{};
    pkt.magic = kCarCustomizationMagic;
    pkt.version = kPacketVersion;
    pkt.size = sizeof(CarCustomizationPacket);
    pkt.playerIndex = gIsSecondInstance ? 2 : 1;
    pkt.carModelID = modelID;
    if(!ScopeLocalAppearance(pkt))return;
    uint8_t portableParam[0x940]{};
    if (carArrayPtr >= 0x00400000) SerializeVehicleParamPortable(portableParam, reinterpret_cast<const uint8_t*>(carArrayPtr));
    else SerializeVehicleParamPortable(portableParam, pRide - 0x10);
    // Legacy diagnostic packet retains layout but no raw nested pointers.
    std::memcpy(pkt.visualData, portableParam + 0x10 + 0x2E0, sizeof(pkt.visualData));
    std::memcpy(pkt.partFlags, pRide + 0x838, sizeof(pkt.partFlags));
    std::memcpy(pkt.visualTuning, pRide + 0x850, sizeof(pkt.visualTuning));

    char localPath[MAX_PATH];
    std::snprintf(localPath, sizeof(localPath), "%sPlayer%dCar.v9.bin", gScriptsDirectory, pkt.playerIndex);
    FILE* fp = std::fopen(localPath, "wb");
    if (fp) {
        std::fwrite(&pkt, 1, sizeof(pkt), fp);
        std::fclose(fp);
    }

    if (gGameSocket != INVALID_SOCKET) {
        SendMeasuredCoopDatagram(&pkt,sizeof(pkt),CoopSnapshot().peer);

        if (carArrayPtr >= 0x00400000) {
            CarSyncPacket syncPkt{};
            syncPkt.magic = kPacketMagic;
            syncPkt.version = kPacketVersion;
            syncPkt.size = sizeof(CarSyncPacket);
            syncPkt.playerIndex = pkt.playerIndex;
            syncPkt.sentAtMs = GetTickCount();
            DWORD flowState = 0;
            ReadGameMemory(kGameFlowState, &flowState, sizeof(flowState));
            syncPkt.selectionStage = flowState == 6 ? 2 : (gLocalSelectionFinalized ? 1 : 0);
            std::memcpy(syncPkt.vehicleParam, portableParam, sizeof(syncPkt.vehicleParam));
            QueueLocalPeerSelection(syncPkt);
        }
    }
}

void ReloadCarVisuals(uintptr_t carPtr, const void* newVehicleParam) {
    // Note: Live SetupRide calls mid-frame cause D3D crashes. Disabled for stability.
}

#include "CoopMotionNative.inl"
#include "CoopWorldMotionNative.inl"
#include "CoopWorldWheelNative.inl"
bool SetCoopAIReplicaContacts(uintptr_t car, uintptr_t localCar, bool active);
#include "CoopDualRegionNative.inl"

// Full native body state, plus existing dedicated-peer wheel rendering caches.
void InjectVehicleTransform(uintptr_t carPtr, uintptr_t localPlayerCar, float x, float y, float z,
    float headingDeg, float speedKmh, int16_t steerAngle = 0, uint8_t brakeLight = 0, uint8_t reverseLight = 0,
    const float* nativeWheelPhase = nullptr, const float* nativeWheelSpeed = nullptr,
    const float (*nativeWheelRotation)[9] = nullptr, uint32_t wheelRotationValidMask = 0,
    const PositionPacket* physicalPose = nullptr) {
    if (!IsValidUserPtr(carPtr) || carPtr == localPlayerCar) return;

    __try {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float rad = headingDeg * kDegToRad;
        const float fx = std::cos(rad);
        const float fy = std::sin(rad);

        // Canonical NFSU2 4x4 Orientation Matrix:
        float matrix[16] = {
             fx,    fy,  0.0f, 0.0f,
            -fy,    fx,  0.0f, 0.0f,
           0.0f,  0.0f,  1.0f, 0.0f,
           0.0f,  0.0f,  0.0f, 1.0f
        };

        float speedMs = (reverseLight ? -speedKmh : speedKmh) / 3.6f;
        if (physicalPose) {
            float fullRotation[16]{};
            if (!coop_motion::QuaternionToMatrix(physicalPose->orientation,fullRotation)) return;
            speedMs=physicalPose->linearVelocity[0]*fullRotation[0]+
                physicalPose->linearVelocity[1]*fullRotation[1]+
                physicalPose->linearVelocity[2]*fullRotation[2];
        }
        const float vx = fx * speedMs;
        const float vy = fy * speedMs;
        const float vz = 0.0f;

        // AI suppression is performed by the typed AIDriver hook only. Human
        // and remote drivers do not share AIDriver's +0x77C/+0x77D layout.

        float carPos[3] = { SanitizeFloat(x), SanitizeFloat(y), SanitizeFloat(z) };
        float vel[3] = { SanitizeFloat(vx), SanitizeFloat(vy), SanitizeFloat(vz) };

        float quaternion[4]{};
        coop_motion::MatrixToQuaternion(matrix, quaternion);
        const float noSpin[3]{};
        const float* orientation = physicalPose ? physicalPose->orientation : quaternion;
        const float* velocity = physicalPose ? physicalPose->linearVelocity : vel;
        const float* angular = physicalPose ? physicalPose->angularVelocity : noSpin;
        if (!ApplyCoopPhysicalMotion(carPtr,localPlayerCar,carPos,orientation,velocity,angular)) {
            ++gMotionRefused; return;
        }
        ++gMotionApplied;

        const uint16_t headingRaw = static_cast<uint16_t>((headingDeg / 360.0f) * 65536.0f);
        SafeWriteBytes(carPtr + 0x40C, &headingRaw, sizeof(headingRaw));
        SafeWriteBytes(carPtr + 0x40E, &steerAngle, sizeof(steerAngle));
        SafeWriteBytes(carPtr + 0x42C, &speedMs, sizeof(speedMs));

        // Wheel phase is owned by this proxy, not by the previous race/vehicle.
        // Integrate only short, continuous samples so teleport/spawn corrections
        // and a stalled peer cannot produce an enormous rotation step.
        static float sProxyWheelTurn = 0.0f;
        static DWORD sLastWheelTickMs = 0;
        static float sLastPosX = 0.0f, sLastPosY = 0.0f;
        static uintptr_t sWheelCar = 0;
        static bool sHaveWheelSample = false;
        const DWORD nowMs = GetTickCount();
        const DWORD elapsedMs = nowMs - sLastWheelTickMs; // DWORD wrap is intentional.
        const bool resetWheel = !sHaveWheelSample || sWheelCar != carPtr || elapsedMs > 250;
        const float dt = resetWheel ? 0.0f : std::min(elapsedMs * 0.001f, 0.1f);
        float signedSpeedMs = std::clamp(SanitizeFloat(speedMs), -200.0f, 200.0f);
        if (reverseLight != 0) signedSpeedMs = -std::fabs(signedSpeedMs);
        if (resetWheel) {
            sProxyWheelTurn = 0.0f;
        } else if (dt > 0.0f) {
            const float dx = carPos[0] - sLastPosX;
            const float dy = carPos[1] - sLastPosY;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float measuredSpeed = distance / dt;
            if (distance > 20.0f || measuredSpeed > 200.0f) {
                signedSpeedMs = 0.0f; // A relocation is not wheel travel.
            } else if (distance > 0.002f) {
                // Displacement supplies the true reverse direction even when
                // the peer's speed is a positive magnitude.
                const float forwardTravel = dx * fx + dy * fy;
                if (std::fabs(forwardTravel) > distance * 0.25f) {
                    signedSpeedMs = std::copysign(measuredSpeed, forwardTravel);
                } else if (measuredSpeed > std::fabs(signedSpeedMs)) {
                    signedSpeedMs = std::copysign(measuredSpeed, signedSpeedMs);
                }
            }
        }
        sWheelCar = carPtr;
        sLastWheelTickMs = nowMs;
        sLastPosX = carPos[0];
        sLastPosY = carPos[1];
        sHaveWheelSample = true;

        // Cosmetic approximation until actual per-wheel phase is sent by peer.
        constexpr float kCircumference = 2.07345115f;
        const float revPerSec = (std::fabs(signedSpeedMs) > 0.05f) ? (signedSpeedMs / kCircumference) : 0.0f;
        if (dt > 0.0f && revPerSec != 0.0f) {
            sProxyWheelTurn += revPerSec * dt;
            sProxyWheelTurn -= std::floor(sProxyWheelTurn);
        }

        float rollRad = sProxyWheelTurn * 6.2831853f;
        float wheelAnglesRad[4] = { rollRad, rollRad, rollRad, rollRad };
        const float fallbackWheelSpeed = revPerSec * 6.2831853f;
        float wheelSpeedsRad[4] = { fallbackWheelSpeed, fallbackWheelSpeed, fallbackWheelSpeed, fallbackWheelSpeed };
        if (nativeWheelPhase && nativeWheelSpeed) {
            for (size_t i = 0; i < 4; ++i) {
                wheelAnglesRad[i] = std::remainder(SanitizeFloat(nativeWheelPhase[i]), 6.283185307f);
                wheelSpeedsRad[i] = std::clamp(SanitizeFloat(nativeWheelSpeed[i]), -5000.0f, 5000.0f);
            }
            rollRad = wheelAnglesRad[0];
        }

        // Native player-car construction owns collision body activation.
        // Do not alter unverified Havok internal flags from the network path.

        // 0x5F5010 deliberately NOT called: it projects yaw onto wheel terrain,
        // destroying pitch/roll and airborne height. Native body state above
        // instead updates momentum, model origin and full render orientation.

        // Re-assert front wheel steer angle
        SafeWriteBytes(carPtr + 0x40E, &steerAngle, sizeof(steerAngle));

        // Optional spinner animation is separate from the ordinary tire/rim
        // rotation caches consumed by 0x5F6820. Never replace a wheel-force getter.
        SafeWriteBytes(carPtr + 0x4E8, wheelAnglesRad, sizeof(wheelAnglesRad));
        SafeWriteBytes(carPtr + 0x4F8, wheelSpeedsRad, sizeof(wheelSpeedsRad));
        SafeWriteBytes(carPtr + 0x330, wheelSpeedsRad, sizeof(wheelSpeedsRad));
        gProxyWheelRollAngle = rollRad / 6.283185307f;
        gProxyWheelRollRad = rollRad;
        gProxySteerAngle = steerAngle;
        std::memcpy(gProxyRenderWheelPhase, wheelAnglesRad, sizeof(gProxyRenderWheelPhase));
        gProxyRenderOrdinaryWheelRotationMask = 0;
        if (nativeWheelRotation != nullptr) {
            std::memcpy(gProxyRenderOrdinaryWheelRotation, nativeWheelRotation,
                sizeof(gProxyRenderOrdinaryWheelRotation));
            for (size_t wheel = 0; wheel < 4; ++wheel) {
                if ((wheelRotationValidMask & (uint32_t{1} << wheel)) != 0 &&
                    IsUsableWheelRotation(gProxyRenderOrdinaryWheelRotation[wheel])) {
                    gProxyRenderOrdinaryWheelRotationMask |= uint32_t{1} << wheel;
                }
            }
        }
        gProxyRenderWheelCar = carPtr;
        gProxyRenderWheelAtMs = GetTickCount();

        // The legacy havok+0x19C write was removed: 0x005ECD30 reads
        // wheel load/traction state (dashboard comparisons use a 500-unit
        // threshold), not the mesh rotation angle.

        // Ensure Car is kept active and visible
        BYTE carActive = 1;
        SafeWriteBytes(carPtr + 0x550, &carActive, sizeof(carActive));

        // Legacy writes to visual+0x954/0x958/0x95C were not lighting
        // controllers. Preserve the native visual/streaming data untouched.
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Exception safely caught and ignored during dynamic spawn/streaming transitions
    }
}

bool IsValidPacket(const PositionPacket& packet) {
    if(!std::isfinite(packet.driftScore) || packet.driftScore<0 || packet.driftScore>2000000000.0f ||
        (!packet.driftEpoch && packet.driftScore!=0)) return false;
    if (!packet.sessionId || !packet.trackId || !packet.worldEpoch ||
        !coop_motion::ValidQuaternion(packet.orientation)) return false;
    for (size_t i=0;i<3;++i)
        if (!std::isfinite(packet.linearVelocity[i]) || std::fabs(packet.linearVelocity[i])>400.0f ||
            !std::isfinite(packet.angularVelocity[i]) || std::fabs(packet.angularVelocity[i])>100.0f) return false;
    for (const auto& sample : packet.skidEffects)
        if (!IsValidSkidEffectSample(sample)) return false;
    if ((packet.wheelRotationValidMask & ~uint32_t{0xF}) != 0) return false;
    for (size_t wheel = 0; wheel < 4; ++wheel) {
        for (float value : packet.wheelRotation[wheel])
            if (!std::isfinite(value) || std::fabs(value) > 4.0f) return false;
        if ((packet.wheelRotationValidMask & (uint32_t{1} << wheel)) != 0 &&
            !IsUsableWheelRotation(packet.wheelRotation[wheel])) return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(packet.wheelPhaseRad[i]) || std::fabs(packet.wheelPhaseRad[i]) > 100.0f ||
            !std::isfinite(packet.wheelSpeedRad[i]) || std::fabs(packet.wheelSpeedRad[i]) > 5000.0f) return false;
    }
    return packet.magic == kPacketMagic && packet.version == kPacketVersion &&
        packet.size == sizeof(PositionPacket) && packet.flowState <= 6 &&
        packet.carModel[sizeof(packet.carModel) - 1] == '\0' &&
        std::isfinite(packet.x) && std::fabs(packet.x) < 1000000.0f &&
        std::isfinite(packet.y) && std::fabs(packet.y) < 1000000.0f &&
        std::isfinite(packet.z) && std::fabs(packet.z) < 1000000.0f &&
        std::isfinite(packet.headingDeg) && packet.headingDeg >= 0.0f && packet.headingDeg <= 360.0f &&
        std::isfinite(packet.forwardX) && std::fabs(packet.forwardX) <= 1.1f &&
        std::isfinite(packet.forwardY) && std::fabs(packet.forwardY) <= 1.1f &&
        std::isfinite(packet.forwardZ) && std::fabs(packet.forwardZ) <= 1.1f &&
        std::isfinite(packet.speedKmh) && packet.speedKmh >= 0.0f && packet.speedKmh < 1000.0f;
}

// Only the main game thread reads a complete pose. UDP never samples mutable
// Car/Racer objects halfway through a physics update or during destruction.
void InvalidatePublishedMotion() {
    AcquireSRWLockExclusive(&gPublishedMotionLock);
    gPublishedMotion.valid = false;
    ReleaseSRWLockExclusive(&gPublishedMotionLock);
}

void PublishLocalMotion(const LocalVehicleState& local,DWORD now) {
    static bool hadWorld=false;
    static uintptr_t lastCar=0;
    static uint32_t lastTrack=0,epoch=0,publication=0,lastTime=0,lastMembership=0,lastTicket=0;
    static uint64_t lastSession=0;
    const auto session=CoopSnapshot();
    uint32_t track=0,flow=0;
    if (!IsCoopGameplayActive() || !session.joined || !session.fresh ||
        !ReadGameMemory(gRaceContextAddresses.flow,&flow,sizeof(flow)) || flow!=6 ||
        !local.physicalMotionValid || !ReadGameMemory(gRaceContextAddresses.track,&track,sizeof(track)) || !track) {
        hadWorld=false; InvalidatePublishedMotion(); return;
    }
    AcquireSRWLockShared(&gPublishedMotionLock);
    const bool previouslyPublished=gPublishedMotion.valid;
    ReleaseSRWLockShared(&gPublishedMotionLock);
    const DWORD sourceNow=coop_motion::MotionMilliseconds();
    if (!hadWorld || !previouslyPublished || lastCar!=local.playerInstance ||
        lastTrack!=track || lastSession!=session.sessionId || lastMembership!=session.membershipNonce || lastTicket!=session.ticket) {
        if (++epoch==0) ++epoch;
        hadWorld=true; lastCar=local.playerInstance; lastTrack=track; lastSession=session.sessionId;
        lastMembership=session.membershipNonce;lastTicket=session.ticket;
    } else if (sourceNow==lastTime) return;
    lastTime=sourceNow;
    PositionPacket packet{};
    packet.magic=kPacketMagic; packet.version=kPacketVersion; packet.size=sizeof(packet);
    packet.sequence=++publication; packet.flowState=6; packet.sentAtMs=sourceNow;
    packet.x=local.x; packet.y=local.y; packet.z=local.z;
    packet.speedKmh=local.speedKmh; packet.headingDeg=local.headingDeg;
    packet.forwardX=local.forwardX; packet.forwardY=local.forwardY; packet.forwardZ=local.forwardZ;
    packet.steerAngle=local.steerAngle; packet.brakeLight=local.brakeLight; packet.reverseLight=local.reverseLight;
    std::memcpy(packet.carModel,local.carModel,sizeof(packet.carModel));
    std::memcpy(packet.wheelPhaseRad,local.wheelPhaseRad,sizeof(packet.wheelPhaseRad));
    std::memcpy(packet.wheelSpeedRad,local.wheelSpeedRad,sizeof(packet.wheelSpeedRad));
    std::memcpy(packet.wheelRotation,local.wheelRotation,sizeof(packet.wheelRotation));
    packet.wheelRotationValidMask=local.wheelRotationValidMask;
    packet.sessionId=session.sessionId;packet.membershipNonce=session.membershipNonce;packet.ticket=session.ticket;
    packet.trackId=track; packet.worldEpoch=epoch;
    std::memcpy(packet.orientation,local.orientation,sizeof(packet.orientation));
    std::memcpy(packet.linearVelocity,local.linearVelocity,sizeof(packet.linearVelocity));
    std::memcpy(packet.angularVelocity,local.angularVelocity,sizeof(packet.angularVelocity));
    CopyLocalSkidEffectSamples(packet.skidEffects);
    ReadGameMemory(local.playerInstance+0x8FC,&packet.splineDistance,sizeof(float));
    uintptr_t racer=0,owner=0;
    if (ReadGameMemory(local.playerInstance+0x1C,&racer,sizeof(racer)) && IsValidUserPtr(racer) &&
        ReadGameMemory(racer,&owner,sizeof(owner)) && owner==local.playerInstance) {
        ReadGameMemory(racer+8,&packet.currentLap,1);
        ReadGameMemory(racer+9,&packet.completedLaps,1);
        ReadGameMemory(racer+0x20,&packet.totalProgress,sizeof(float));
    }
    ReadCoopLocalDriftScore(local.playerInstance,packet.driftScore,packet.driftEpoch);
    if (!IsValidPacket(packet)) { InvalidatePublishedMotion(); return; }
    AcquireSRWLockExclusive(&gPublishedMotionLock);
    gPublishedMotion.local=local; gPublishedMotion.packet=packet;
    gPublishedMotion.publication=publication;gPublishedMotion.publishedAtMs=now;gPublishedMotion.valid=true;
    ReleaseSRWLockExclusive(&gPublishedMotionLock);
}

bool ReadPublishedMotion(PublishedMotion& output,DWORD now) {
    AcquireSRWLockShared(&gPublishedMotionLock);
    output=gPublishedMotion;
    ReleaseSRWLockShared(&gPublishedMotionLock);
    return output.valid && coop_time::Elapsed(now,output.publishedAtMs)<=250;
}

void LogMotionDiagnostics(uintptr_t car,const RemotePlayerState& remote,DWORD now) {
    static DWORD lastLog=0;
    if (now-lastLog<5000) return;
    lastLog=now;
    size_t history=0; uint32_t delay=0,extrapolation=0,resets=0;
    AcquireSRWLockShared(&gRemoteStateLock);
    history=gRemoteMotion.Count(); delay=gRemoteMotion.DelayMs();
    extrapolation=gRemoteMotion.ExtrapolationMs(); resets=gRemoteMotion.ResetCount();
    ReleaseSRWLockShared(&gRemoteStateLock);
    uintptr_t wrapper=0,wrapperType=0,body=0,bodyType=0;
    uint32_t mode=UINT32_MAX;
    CoopPhysicalMotionNative::ReadVerifiedMode(car,mode);
    float mass=0;
    ReadGameMemory(car+0x34,&wrapper,sizeof(wrapper));
    if (IsValidUserPtr(wrapper)) {
        ReadGameMemory(wrapper,&wrapperType,sizeof(wrapperType));
        if (mode==1 || mode==3) ReadGameMemory(wrapper+0x20,&body,sizeof(body));
        if ((mode==1 || mode==3) && IsValidUserPtr(body)) {
            ReadGameMemory(body,&bodyType,sizeof(bodyType));
            ReadGameMemory(body+0x164,&mass,sizeof(mass));
        }
    }
    char line[520]{};
    std::snprintf(line,sizeof(line),
        "[MOTION] applied=%u refused=%u history=%u delay=%ums extrap=%ums resets=%u age=%ums epoch=%u track=%u q=(%.3f,%.3f,%.3f,%.3f) v=(%.2f,%.2f,%.2f) wrapper=%08X body=%08X mass=%.1f mode=%u kinematic=%u physical=%u\r\n",
        gMotionApplied.load(),gMotionRefused.load(),static_cast<unsigned>(history),delay,extrapolation,resets,
        now-remote.receivedAtMs,remote.current.worldEpoch,remote.current.trackId,
        remote.rendered.orientation[0],remote.rendered.orientation[1],remote.rendered.orientation[2],remote.rendered.orientation[3],
        remote.rendered.linearVelocity[0],remote.rendered.linearVelocity[1],remote.rendered.linearVelocity[2],
        static_cast<unsigned>(wrapperType),static_cast<unsigned>(bodyType),mass,mode,
        CoopPhysicalMotionNative::kinematicApplied,CoopPhysicalMotionNative::bodyApplied);
    LogMessage(line);
}

SOCKET OpenSocket(unsigned short port, bool isNonBlocking = true) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    BOOL bcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));
    // Fleet publications contain many datagrams in one frame. The small OS
    // default drops burst traffic on a VPN even when average bandwidth fits.
    const int bufferBytes=1024*1024;
    setsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&bufferBytes),sizeof(bufferBytes));
    setsockopt(s,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&bufferBytes),sizeof(bufferBytes));

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = INADDR_ANY;
    endpoint.sin_port = htons(port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    if (isNonBlocking) {
        u_long mode = 1;
        ioctlsocket(s, FIONBIO, &mode);
    }
    return s;
}

void SetOverlayText(const char* text) {
    CoopAnnouncements::SetLive(text);
    gOverlayCreated=true;
}

void ConfigureOverlayLayout(bool isRaceBanner = false) {
    // Presentation lives in the owned announcement panel, never FEng linked lists.
}

HWND gGameHwnd = nullptr;
WNDPROC gOldWndProc = nullptr;

bool IsGameWindowFocused() {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) return false;
    HWND gameHwnd = gGameHwnd;
    if (gameHwnd == nullptr) ReadGameMemory(kGameHwndAddr, &gameHwnd, sizeof(gameHwnd));
    if (gameHwnd == nullptr || !IsWindow(gameHwnd)) return true;
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(foreground, &foregroundPid);
    if (foregroundPid == GetCurrentProcessId()) return true;
    return (foreground == gameHwnd || GetAncestor(foreground, GA_ROOT) == gameHwnd);
}

#include "CoopResultsTrace.inl"
#include "CoopResultsFocusNative.inl"

bool InstallKeyboardFocusHook() {
    // The keyboard COM object is created later. UiTick hooks its native methods.
    return true;
}

constexpr uintptr_t kKeyboardDevicePtr = 0x0086E5C0;
constexpr DWORD kBufferedInputPeek = 1; // DIGDD_PEEK
constexpr size_t kDirectInputDeviceVtableSlots = 32;

using GetDeviceStateFn = HRESULT(__stdcall*)(void* This, DWORD cbData, LPVOID lpvData);
using GetDeviceDataFn = HRESULT(__stdcall*)(void* This, DWORD cbObjectData, void* rgdod, LPDWORD pdwInOut, DWORD dwFlags);

bool gNeedSetupProxyBody = false;

GetDeviceStateFn gOriginalGetDeviceState = nullptr;
GetDeviceDataFn gOriginalGetDeviceData = nullptr;
void* gKeyboardDevice = nullptr;
// Native DirectInput checks both vtable[-1] and the vtable's exact identity.
// Keep the original table address and remember originals per shared A/W table.
struct NativeInputHook {
    uintptr_t table = 0;
    GetDeviceStateFn state = nullptr;
    GetDeviceDataFn data = nullptr;
};
NativeInputHook gNativeInputHooks[4]{};
SRWLOCK gNativeInputHooksLock = SRWLOCK_INIT;

bool GetNativeInputMethods(void* device, GetDeviceStateFn& state, GetDeviceDataFn& data) {
    uintptr_t table = 0;
    if (!ReadGameMemory(reinterpret_cast<uintptr_t>(device), &table, sizeof(table))) return false;
    AcquireSRWLockShared(&gNativeInputHooksLock);
    for (const auto& hook : gNativeInputHooks) {
        if (hook.table == table) {
            state = hook.state;
            data = hook.data;
            ReleaseSRWLockShared(&gNativeInputHooksLock);
            return state != nullptr && data != nullptr;
        }
    }
    ReleaseSRWLockShared(&gNativeInputHooksLock);
    return false;
}

bool IsCommittedNativeInputAddress(uintptr_t address, bool requireExecutable = false) {
    MEMORY_BASIC_INFORMATION memory{};
    if (address == 0 || VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    if (!requireExecutable) return true;
    const DWORD protection = memory.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool CopyNativeKeyboardVtable(uintptr_t source,
    uintptr_t (&copy)[kDirectInputDeviceVtableSlots + 1]) {
    if (source < sizeof(uintptr_t) ||
        !ReadGameMemory(source - sizeof(uintptr_t), copy, sizeof(copy))) return false;
    for (size_t index = 1; index <= kDirectInputDeviceVtableSlots; ++index) {
        if (!IsCommittedNativeInputAddress(copy[index], true)) return false;
    }
    return true;
}

struct BufferedKeyboardEvent {
    DWORD offset;
    DWORD data;
    DWORD timestamp;
    DWORD sequence;
    UINT_PTR action;
};
static_assert(sizeof(BufferedKeyboardEvent) == 20, "DirectInput8 x86 event layout");

struct HeldKeyboardEvent {
    BufferedKeyboardEvent event{};
    bool held = false;
    bool releasePending = false;
};
HeldKeyboardEvent gHeldKeyboardEvents[256]{};
bool gKeyboardWasFocused = true;
bool gFlushKeyboardOnFocusGain = false;

void UpdateKeyboardFocus(bool focused) {
    if (!focused && gKeyboardWasFocused) {
        for (auto& key : gHeldKeyboardEvents) {
            if (key.held) {
                key.held = false;
                key.releasePending = true;
            }
        }
        gFlushKeyboardOnFocusGain = true;
    }
    gKeyboardWasFocused = focused;
}

void RememberKeyboardEvent(const BufferedKeyboardEvent& event) {
    HeldKeyboardEvent* empty = nullptr;
    for (auto& key : gHeldKeyboardEvents) {
        if ((key.held || key.releasePending) && key.event.offset == event.offset &&
            key.event.action == event.action) {
            key.event = event;
            key.held = event.data != 0;
            key.releasePending = false;
            return;
        }
        if (!key.held && !key.releasePending && empty == nullptr) empty = &key;
    }
    if (event.data != 0 && empty != nullptr) {
        empty->event = event;
        empty->held = true;
    }
}

void FlushBufferedKeyboard(void* device, DWORD eventSize, GetDeviceDataFn original) {
    // Microsoft documents NULL + INFINITE + flags=0 as a complete buffer flush.
    DWORD discarded = INFINITE;
    original(device, eventSize, nullptr, &discarded, 0);
}

DWORD WritePendingKeyboardReleases(void* output, DWORD eventSize, DWORD capacity, bool peek) {
    DWORD count = 0;
    for (auto& key : gHeldKeyboardEvents) {
        if (!key.releasePending || count >= capacity) continue;
        if (output != nullptr) {
            BufferedKeyboardEvent release = key.event;
            release.data = 0;
            release.timestamp = GetTickCount();
            std::memcpy(static_cast<BYTE*>(output) + count * eventSize, &release, eventSize);
            if (!peek) key.releasePending = false;
        }
        ++count;
    }
    return count;
}

HRESULT __stdcall HookedGetDeviceState(void* This, DWORD cbData, LPVOID lpvData) {
    GetDeviceStateFn originalState = nullptr;
    GetDeviceDataFn originalData = nullptr;
    if (!GetNativeInputMethods(This, originalState, originalData)) return E_FAIL;
    const HRESULT hr = originalState(This, cbData, lpvData);
    if (This != gKeyboardDevice) return hr;
    const bool focused = IsGameWindowFocused();
    CoopResultsTrace::KeyboardState(hr,cbData,lpvData,focused);
    const bool composing=CoopAnnouncements::CapturesKeyboard();
    UpdateKeyboardFocus(focused&&!composing);
    if(composing&&SUCCEEDED(hr)&&lpvData&&cbData&&cbData<=4096){std::memset(lpvData,0,cbData);return S_OK;}
    if (!focused && lpvData != nullptr && cbData != 0 && cbData <= 4096) {
        if (gGameHwnd && IsIconic(gGameHwnd)) {
            // Only zero keyboard state if the game window is actually minimized
            std::memset(lpvData, 0, cbData);
            return S_OK;
        }
    }
    // Gameplay uses SetActionMap: offsets are packed action DWORDs, NOT DIK
    // scan codes. In particular 0x14 is Left/A, not T. Keep native mappings
    // byte-exact; panel keys use WndProc/poll and pause uses native arbitration.
    return hr;
}

HRESULT __stdcall HookedGetDeviceData(void* This, DWORD cbObjectData, void* rgdod, LPDWORD pdwInOut, DWORD dwFlags) {
    GetDeviceStateFn originalState = nullptr;
    GetDeviceDataFn originalData = nullptr;
    if (!GetNativeInputMethods(This, originalState, originalData)) return E_FAIL;
    if (This != gKeyboardDevice) return originalData(This, cbObjectData, rgdod, pdwInOut, dwFlags);
    if (pdwInOut == nullptr || (cbObjectData != 16 && cbObjectData != sizeof(BufferedKeyboardEvent))) {
        return originalData(This, cbObjectData, rgdod, pdwInOut, dwFlags);
    }

    const DWORD capacity = *pdwInOut;
    const bool focused = IsGameWindowFocused()&&!CoopAnnouncements::CapturesKeyboard();
    const bool peek = (dwFlags & kBufferedInputPeek) != 0;
    UpdateKeyboardFocus(focused);
    if (!focused || gFlushKeyboardOnFocusGain) {
        FlushBufferedKeyboard(This, cbObjectData, originalData);
        if (focused) gFlushKeyboardOnFocusGain = false;
    }

    const DWORD releases = WritePendingKeyboardReleases(rgdod, cbObjectData, capacity, peek);
    if (!focused || releases != 0) {
        *pdwInOut = releases;
        CoopResultsTrace::BufferStatus(S_OK,releases,dwFlags,focused,true);
        return S_OK;
    }

    const HRESULT hr = originalData(This, cbObjectData, rgdod, pdwInOut, dwFlags);
    CoopResultsTrace::BufferStatus(hr,*pdwInOut,dwFlags,focused,false);
    // Real mappings, including uAppData, remain exactly as the game configured them.
    if (SUCCEEDED(hr) && rgdod != nullptr && !peek) {
        const DWORD count = (std::min)(*pdwInOut, capacity);
        for (DWORD i = 0; i < count; ++i) {
            BufferedKeyboardEvent event{};
            std::memcpy(&event, static_cast<BYTE*>(rgdod) + i * cbObjectData, cbObjectData);
            CoopResultsTrace::BufferedEvent(event.offset,event.data,event.timestamp,event.sequence,event.action);
            RememberKeyboardEvent(event);
        }
    }
    return hr;
}

bool InstallDirectInputFocusHookForDevice(uintptr_t device) {
    uintptr_t originalVtable = 0;
    if (device == 0 || !IsCommittedNativeInputAddress(device) ||
        !ReadGameMemory(device, &originalVtable, sizeof(originalVtable))) return false;
    // UiTick checks each frame so device replacement is noticed. Once installed,
    // this path performs no table writes and avoids revalidating 32 functions.
    AcquireSRWLockShared(&gNativeInputHooksLock);
    if (gKeyboardDevice == reinterpret_cast<void*>(device)) {
        for (const auto& hook : gNativeInputHooks) {
            uintptr_t stateAndData[2]{};
            if (hook.table == originalVtable &&
                ReadGameMemory(originalVtable + sizeof(uintptr_t) * 9, stateAndData, sizeof(stateAndData)) &&
                stateAndData[0] == reinterpret_cast<uintptr_t>(HookedGetDeviceState) &&
                stateAndData[1] == reinterpret_cast<uintptr_t>(HookedGetDeviceData)) {
                ReleaseSRWLockShared(&gNativeInputHooksLock);
                return true;
            }
        }
    }
    ReleaseSRWLockShared(&gNativeInputHooksLock);
    uintptr_t nativeTable[kDirectInputDeviceVtableSlots + 1]{};
    if (!CopyNativeKeyboardVtable(originalVtable, nativeTable) || nativeTable[0] > device ||
        !IsCommittedNativeInputAddress(device - nativeTable[0])) return false;
    const uintptr_t* methods = nativeTable + 1;

    AcquireSRWLockExclusive(&gNativeInputHooksLock);
    NativeInputHook* registered = nullptr;
    NativeInputHook* empty = nullptr;
    for (auto& hook : gNativeInputHooks) {
        if (hook.table == originalVtable) registered = &hook;
        if (hook.table == 0 && empty == nullptr) empty = &hook;
    }
    if (registered == nullptr) {
        if (empty == nullptr || methods[9] == reinterpret_cast<uintptr_t>(HookedGetDeviceState) ||
            methods[10] == reinterpret_cast<uintptr_t>(HookedGetDeviceData)) {
            ReleaseSRWLockExclusive(&gNativeInputHooksLock);
            return false;
        }
        registered = empty;
    }

    auto* slots = reinterpret_cast<uintptr_t*>(originalVtable);
    const bool installed = slots[9] == reinterpret_cast<uintptr_t>(HookedGetDeviceState) &&
        slots[10] == reinterpret_cast<uintptr_t>(HookedGetDeviceData);
    if (!installed) {
        DWORD protection = 0;
        if (!VirtualProtect(slots + 9, sizeof(uintptr_t) * 2, PAGE_EXECUTE_READWRITE, &protection)) {
            ReleaseSRWLockExclusive(&gNativeInputHooksLock);
            return false;
        }
        if (registered->table == 0) {
            registered->table = originalVtable;
            registered->state = reinterpret_cast<GetDeviceStateFn>(methods[9]);
            registered->data = reinterpret_cast<GetDeviceDataFn>(methods[10]);
        }
        InterlockedExchange(reinterpret_cast<volatile LONG*>(slots + 9),
            static_cast<LONG>(reinterpret_cast<uintptr_t>(HookedGetDeviceState)));
        InterlockedExchange(reinterpret_cast<volatile LONG*>(slots + 10),
            static_cast<LONG>(reinterpret_cast<uintptr_t>(HookedGetDeviceData)));
        DWORD unused = 0;
        VirtualProtect(slots + 9, sizeof(uintptr_t) * 2, protection, &unused);
    }
    const bool sameDevice = gKeyboardDevice == reinterpret_cast<void*>(device);
    gOriginalGetDeviceState = registered->state;
    gOriginalGetDeviceData = registered->data;
    gKeyboardDevice = reinterpret_cast<void*>(device);
    ReleaseSRWLockExclusive(&gNativeInputHooksLock);
    if (sameDevice && installed) return true;
    for (auto& key : gHeldKeyboardEvents) key = HeldKeyboardEvent{};
    // The explicit-device entry point also supports a standalone native COM
    // smoke test without touching any game's absolute addresses.
    gKeyboardWasFocused = gGameHwnd != nullptr && IsGameWindowFocused();
    gFlushKeyboardOnFocusGain = !gKeyboardWasFocused;
    LogMessage("[INPUT] Native keyboard vtable identity and ABI preserved; foreground filter installed.\r\n");
    return true;
}

bool RestoreDirectInputFocusHooks() {
    bool restored = true;
    AcquireSRWLockExclusive(&gNativeInputHooksLock);
    for (auto& hook : gNativeInputHooks) {
        if (hook.table == 0) continue;
        auto* slots = reinterpret_cast<uintptr_t*>(hook.table);
        DWORD protection = 0;
        if (!VirtualProtect(slots + 9, sizeof(uintptr_t) * 2, PAGE_EXECUTE_READWRITE, &protection)) {
            restored = false;
            continue;
        }
        if (slots[9] == reinterpret_cast<uintptr_t>(HookedGetDeviceState))
            InterlockedExchange(reinterpret_cast<volatile LONG*>(slots + 9),
                static_cast<LONG>(reinterpret_cast<uintptr_t>(hook.state)));
        if (slots[10] == reinterpret_cast<uintptr_t>(HookedGetDeviceData))
            InterlockedExchange(reinterpret_cast<volatile LONG*>(slots + 10),
                static_cast<LONG>(reinterpret_cast<uintptr_t>(hook.data)));
        DWORD unused = 0;
        VirtualProtect(slots + 9, sizeof(uintptr_t) * 2, protection, &unused);
        hook = NativeInputHook{};
    }
    if (restored) {
        gKeyboardDevice = nullptr;
        gOriginalGetDeviceState = nullptr;
        gOriginalGetDeviceData = nullptr;
        for (auto& key : gHeldKeyboardEvents) key = HeldKeyboardEvent{};
        gKeyboardWasFocused = true;
        gFlushKeyboardOnFocusGain = false;
    }
    ReleaseSRWLockExclusive(&gNativeInputHooksLock);
    return restored;
}

bool InstallDirectInputFocusHook() {
    uintptr_t device = 0;
    return ReadGameMemory(kKeyboardDevicePtr, &device, sizeof(device)) &&
        InstallDirectInputFocusHookForDevice(device);
}

LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CoopResultsTrace::Window(msg,wParam,lParam);
    switch (msg) {
    case WM_ACTIVATE:
        UpdateKeyboardFocus(LOWORD(wParam) != WA_INACTIVE && IsGameWindowFocused());
        // MainWndProc 0x5CCD60 uses this message to pause simulation and set
        // lost-focus byte 0x8709E0. Forward active to keep background simulation.
        wParam = MAKEWPARAM(WA_ACTIVE, HIWORD(wParam));
        break;
    case WM_ACTIVATEAPP:
        wParam = TRUE;
        break;
    case WM_KILLFOCUS:
        CoopAnnouncements::FocusLost();
        UpdateKeyboardFocus(false);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        CoopAnnouncements::Release(wParam);
        // NEVER drop WM_KEYUP! Key releases must always reach the game so steering keys
        // (such as A/D/Left/Right) never get stuck held down when switching windows!
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (gGameHwnd && IsIconic(gGameHwnd)) {
            return 0;
        }

        const bool repeated=(lParam&(1L<<30))!=0;
        const bool composing=CoopAnnouncements::CapturesKeyboard();
        const bool consoleHandled=CoopAnnouncements::Key(wParam,repeated);
        if(wParam==CoopAnnouncements::kToggleKey&&!repeated&&!composing)
            TraceCoopConsoleToggle("window",consoleHandled);
        if(consoleHandled) return 0;
        if (HandleCoopSponsorDecisionKey(unsigned(wParam))) return 0;
        if (HandleCoopSpecialDecisionKey(unsigned(wParam))) return 0;
        if (HandleCoopRaceInviteKey(wParam)) return 0;
        // Stock mapped Pause/Resume is the sole pause initiator. A second
        // Windows-key toggle would undo the same physical DirectInput press.
        break;
    }
    case WM_CHAR:
        if (gGameHwnd && IsIconic(gGameHwnd)) {
            return 0;
        }
        if(CoopAnnouncements::Character(wParam,IsWindowUnicode(hWnd)!=FALSE))return 0;
        break;
    case WM_UNICHAR:
        if(wParam==UNICODE_NOCHAR)return TRUE;
        if(CoopAnnouncements::Character(wParam,true))return 0;
        break;
    }
    return gOldWndProc != nullptr
        ? CallWindowProcA(gOldWndProc, hWnd, msg, wParam, lParam)
        : DefWindowProcA(hWnd, msg, wParam, lParam);
}

struct UiTickProfiler {
    LARGE_INTEGER start;
    UiTickProfiler() {
        if (gQpcFreq.QuadPart == 0) {
            QueryPerformanceFrequency(&gQpcFreq);
        }
        QueryPerformanceCounter(&start);

        if (gLastFrameQpc.QuadPart > 0) {
            double frameDt = static_cast<double>(start.QuadPart - gLastFrameQpc.QuadPart) / static_cast<double>(gQpcFreq.QuadPart);
            if (frameDt > 0.0001 && frameDt < 2.0) {
                float instantFps = static_cast<float>(1.0 / frameDt);
                gCurrentFps = instantFps;
                if (gAvgFps <= 0.0f) gAvgFps = instantFps;
                else gAvgFps = gAvgFps * 0.95f + instantFps * 0.05f;
                if (instantFps < gMinFps && gFrameCounter > 60) gMinFps = instantFps;
            }
        }
        gLastFrameQpc = start;
        gFrameCounter++;
    }

    ~UiTickProfiler() {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        if (gQpcFreq.QuadPart > 0) {
            uint32_t dur = static_cast<uint32_t>(
                (end.QuadPart - start.QuadPart) * 1000000 / gQpcFreq.QuadPart);
            gUiTickDurationUs = dur;
            if (dur > gMaxUiTickDurationUs) gMaxUiTickDurationUs = dur;
        }
    }
};

// In-game UI & Render Tick Callback (invoked on main game thread)
struct CoopFrontendRosterAddresses {
    uintptr_t callback = 0x865484, count = 0x89E814;
};
static CoopFrontendRosterAddresses gCoopFrontendRosterAddresses{};
bool MaintainCoopFrontendRoster(uint32_t flow) {
    if (flow != 3 || !IsCoopGameplayActive()) return false;
    uint32_t callback = 0;
    if (!ReadGameMemory(gCoopFrontendRosterAddresses.callback, &callback, 4)) return true;
    // Native578320 queues UnloadingFrontEnd BEFORE flow changes from3.
    // Native5404A0 has already appended its ambient/traffic pool at this point.
    if (callback == 0x581A20) return true;
    const uint32_t previewCount = 1;
    SafeWriteBytes(gCoopFrontendRosterAddresses.count, &previewCount, 4);
    return false;
}

void __cdecl UiTick() {
    static const bool perfInitialized = [] { CoopPerf::Initialize(LogMessage); return true; }();
    (void)perfInitialized;
    bool perfGameplay = false;
    struct PerfFrameEnd { bool& active; ~PerfFrameEnd() { CoopPerf::EndFrame(active); } } perfEnd{perfGameplay};
    CoopPerf::Scope perfTotal(CoopPerf::Part::Total);
    UiTickProfiler profiler;
    // Destruction runs on every banner/loading/menu early return as well.
    struct NoticeTick { ~NoticeTick() {const bool focused=IsGameWindowFocused();
        { CoopPerf::Scope perf(CoopPerf::Part::PeerLabel); CoopPeerLabelNative::Tick(gGameHwnd,focused); }
        { CoopPerf::Scope perf(CoopPerf::Part::Announcements); CoopAnnouncements::Tick(gGameHwnd,focused); }
    } } noticeTick;
    BindCoopConsoleForUi(CoopSnapshot());
    TickCoopChatUi();

    static bool sWndProcHooked = false;
    if (!sWndProcHooked) {
        HWND hGame = nullptr;
        ReadGameMemory(kGameHwndAddr, &hGame, sizeof(hGame));
        if (hGame == nullptr || !IsWindow(hGame)) EnumThreadWindows(GetCurrentThreadId(), [](HWND hWnd, LPARAM lParam) -> BOOL {
            if (IsWindowVisible(hWnd) && GetWindow(hWnd, GW_OWNER) == nullptr) {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&hGame));

        if (hGame != nullptr) {
            SetLastError(0);
            const LONG_PTR previous = SetWindowLongPtrA(hGame, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc));
            if (previous != 0) {
                gGameHwnd = hGame;
                gOldWndProc = reinterpret_cast<WNDPROC>(previous);
                sWndProcHooked = true;
                SetWindowTextA(hGame, gIsSecondInstance ? "NFSU2 Co-op - Player 2" : "NFSU2 Co-op - Player 1");
                // Resume an instance that lost focus before this subclass existed.
                CallWindowProcA(gOldWndProc, hGame, WM_ACTIVATE, WA_ACTIVE, 0);
                LogMessage("[WINDOW] Background simulation enabled; window focus controls keyboard input.\r\n");
            }
        }
    }

    const int startupIntro = CoopStartupIntro::OnFirstGameFrame(gGameHwnd);
    if (startupIntro == 1) {
        LogMessage("[STARTUP_INTRO] UNDERCOOP displayed for 3000ms; normal startup resumed.\r\n");
    } else if (startupIntro == -1) {
        char introFailure[160]{};
        std::snprintf(introFailure, sizeof(introFailure),
            "[STARTUP_INTRO] Display failed (code=%lu); startup continued safely.\r\n",
            static_cast<unsigned long>(CoopStartupIntro::gLastError));
        LogMessage(introFailure);
    }

    // Retry if the engine creates/replaces the keyboard device after loading.
    // 0x870780 is IDirectInput8 itself, never a joystick device vtable.
    InstallDirectInputFocusHook();
    PollCoopConsoleToggle(IsGameWindowFocused() && (GetAsyncKeyState(CoopAnnouncements::kToggleKey)&0x8000));

    if (IsGameWindowFocused()) {
        if (GetAsyncKeyState(VK_F10) & 1) {
            gOverlayEnabled = !gOverlayEnabled;
            if(!gOverlayEnabled) CoopAnnouncements::SetDebug("");
            if (!gOverlayEnabled && gOverlayCreated) {
                SetOverlayText("");
            }
        }

        if (GetAsyncKeyState(VK_F9) & 1) {
            gRemoteSyncEnabled = !gRemoteSyncEnabled;
        }

        if (GetAsyncKeyState(VK_F8) & 1) {
            if (gCandidateCount > 0) {
                gSelectedCandidateIndex = (gSelectedCandidateIndex + 1) % gCandidateCount;
                gBoundProxyCar = gCandidates[gSelectedCandidateIndex].entityAddress;
                gBoundProxyDist = gCandidates[gSelectedCandidateIndex].distance;
            }
        }
    }

    DWORD flowState = 0;
    const bool hasFlowState = ReadGameMemory(kGameFlowState, &flowState, sizeof(flowState));
    gPublishedGameFlow.store(hasFlowState?flowState:UINT32_MAX,std::memory_order_release);
    uint32_t publishedTrack=0;if(hasFlowState)ReadGameMemory(0x0089E7A0,&publishedTrack,sizeof(publishedTrack));
    gPublishedGameTrack.store(publishedTrack,std::memory_order_release);
    if(hasFlowState) CoopDiscordPresence::Observe(flowState,publishedTrack);
    perfGameplay = hasFlowState && flowState == 6 && IsCoopGameplayActive();
    // This precedes every frontend/loading/banner early return.
    if (!hasFlowState || flowState!=6 || !IsCoopGameplayActive()) InvalidatePublishedMotion();
    // Frontend/shop/banner returns below must not skip lease retirement.
    RetirePeerAppearanceOutsideWorld(hasFlowState?flowState:UINT32_MAX);
    if (hasFlowState) {
        { CoopPerf::Scope perf(CoopPerf::Part::CareerPresentation); TickCoopVisualUi(flowState); }
        { CoopPerf::Scope perf(CoopPerf::Part::Special); TickCoopSpecialController(flowState); }
        { CoopPerf::Scope perf(CoopPerf::Part::Session); TickCoopSessionUi(flowState); }
        { CoopPerf::Scope perf(CoopPerf::Part::Movies); TickCoopMovies(flowState); }
        { CoopPerf::Scope perf(CoopPerf::Part::Sponsor); TickCoopSponsorUi(); }
        { CoopPerf::Scope perf(CoopPerf::Part::RaceLifecycle); TickCoopRaceLifecycle(flowState); }
        { CoopPerf::Scope perf(CoopPerf::Part::Url); TickCoopUrlTournament(flowState); }
        { CoopPerf::Scope perf(CoopPerf::Part::WorldImpact); TickCoopWorldImpact(flowState); }
        if (gCoopFleetEnabled) { CoopPerf::Scope perf(CoopPerf::Part::Fleet); TickCoopFleetWorld(flowState); }
        { CoopPerf::Scope perf(CoopPerf::Part::AI); TickCoopAIWorld(flowState); }
        { CoopPerf::Scope perf(CoopPerf::Part::Traces);
            TraceCoopAIWorld(flowState); TraceCoopAIVisual(flowState);
            TraceCoopAIRepair(flowState); TraceCoopTrafficInventory(flowState); }
        { CoopPerf::Scope perf(CoopPerf::Part::Results);
            CoopResultsFocusNative::Tick(); CoopResultsTrace::Tick(); }
        { CoopPerf::Scope perf(CoopPerf::Part::CareerPresentation); TickCoopCareerPresentation(flowState); }
    }
    { CoopPerf::Scope perf(CoopPerf::Part::Traces);
        CoopRearRenderTrace::Tick(hasFlowState ? flowState : UINT32_MAX); }
    if (gClearNetworkBanner.exchange(false)) SetOverlayText("");
    if (gReturnToFreeroam.exchange(false)) CancelCoopRaceInvitation(false);
    if (hasFlowState) ResumePendingCoopLoading(flowState);

    if (flowState == 6) {
        *reinterpret_cast<uint8_t*>(0x008709E0) = 0; // Clear window lost-focus byte
    }

    // Phase 11: Co-op Race Synchronized Loading Trigger
    if (gTriggerRaceLoading) {
        gTriggerRaceLoading = false;
        gCar1LoadedForRace = false;
        gCoopRaceState = COOP_RACE_STARTING;
        SetOverlayText("");

        if (!StartApprovedNativeCoopRace()) {
            LogMessage("[RACE] Native event preparation failed; race not launched.\r\n");
        }
    }

    // Phase 11: Render active Co-op Race invitation / waiting HUD banner
    bool hasRaceBanner = false;
    if (const char* loadingFailure=CoopRaceLoadingFailureMessage()) {
        SetOverlayText(loadingFailure);
        hasRaceBanner=true;
    } else if (gCoopRaceState == COOP_RACE_CLIENT_INVITED) {
        if (GetTickCount() > gRaceStateTimeoutMs) {
            CancelCoopRaceInvitation(true);
            LogMessage("[RACE] Invite timed out.\r\n");
            SetOverlayText("");
        } else {
            uint32_t trackId = gPendingRaceEvent.trackId;
            if (trackId == 0) {
                std::memcpy(&trackId, gPendingRaceEvent.raceParams + 0x00, sizeof(trackId));
            }
            uint32_t laps = gPendingRaceEvent.laps;
            if (laps == 0) {
                laps = *reinterpret_cast<const uint32_t*>(gPendingRaceEvent.raceParams + 0x1C);
            }
            if (laps == 0) laps = gPendingRaceEvent.raceParams[0x54];
            uint32_t secondsLeft = (gRaceStateTimeoutMs - GetTickCount()) / 1000;

            const char* modeName = "Race";
            switch (CleanGameMode(gPendingRaceEvent.gameMode)) {
            case 2: modeName = "Circuit"; break;
            case 3: modeName = "Sprint"; break;
            case 4: modeName = "Drag"; break;
            case 5: modeName = "Drift"; break;
            case 8: modeName = "URL"; break;
            case 9: modeName = "Street X"; break;
            default: modeName = "Circuit"; break;
            }

            char inviteBanner[300];
            std::snprintf(inviteBanner, sizeof(inviteBanner),
                "[ CO-OP RACE INVITATION ]^"
                "Your teammate requests: %s (Track %lu, %lu Laps)^"
                "[F5] Accept  |  [F6] Decline (%us)",
                modeName,
                static_cast<unsigned long>(trackId),
                static_cast<unsigned long>(laps),
                secondsLeft);
            SetOverlayText(inviteBanner);
            hasRaceBanner = true;
        }
    } else if (gCoopRaceState == COOP_RACE_HOST_WAITING_ACCEPT || gCoopRaceState == COOP_RACE_REQUESTING) {
        if (GetTickCount() > gRaceStateTimeoutMs) {
            CancelCoopRaceInvitation(true);
            LogMessage("[RACE] Host wait timed out.\r\n");
            SetOverlayText("");
        } else {
            uint32_t secondsLeft = (gRaceStateTimeoutMs - GetTickCount()) / 1000;
            char waitBanner[250];
            std::snprintf(waitBanner, sizeof(waitBanner),
                "[ CO-OP RACE: INVITATION SENT ]^"
                "Waiting for your teammate to accept... (%us)^"
                "[F6] Cancel invitation",
                secondsLeft);
            SetOverlayText(waitBanner);
            hasRaceBanner = true;
        }
    }

    static bool sPrevHadRaceBanner = false;
    if (hasRaceBanner) {
        sPrevHadRaceBanner = true;
        if (!gOverlayCreated) {
            gOverlayCreated = true;
        }
        ConfigureOverlayLayout(true);
        if (hasFlowState && flowState == 7) {
            return;
        }
    } else if (sPrevHadRaceBanner) {
        sPrevHadRaceBanner = false;
        SetOverlayText("");
    }

    // Ensure HUD overlay is cleared when co-op race is in progress
    static bool sRaceInProgressCleared = false;
    if (gCoopRaceState == COOP_RACE_IN_PROGRESS) {
        if (!sRaceInProgressCleared) {
            SetOverlayText("");
            sRaceInProgressCleared = true;
        }
    } else {
        sRaceInProgressCleared = false;
    }

    if (hasFlowState && flowState == 3) {
        if (!MaintainCoopFrontendRoster(flowState)) gLocalSelectionFinalized = false;
        if (gOverlayCreated) SetOverlayText(""); // no ScreenPrintf lobby banner
        return;
    }
    static DWORD sLastCarBroadcastTick = 0;
    const DWORD nowTick = GetTickCount();
    if (hasFlowState && (sLastCarBroadcastTick == 0 || nowTick - sLastCarBroadcastTick >= 1500)) {
        if (IsCoopGameplayActive()) {
            BroadcastLocalCarCustomization();
        }
        sLastCarBroadcastTick = nowTick;
    }
    if (!hasFlowState || flowState != 6) {
        InvalidatePublishedMotion();
        { CoopPerf::Scope perf(CoopPerf::Part::Appearance);
            TickPeerAppearanceRefresh(hasFlowState ? flowState : UINT32_MAX); }
        gBoundProxyCar = 0;
        if (flowState == 3 || flowState == 1) {
            gCar1LoadedForRace = false;
            gCoopRaceState = COOP_RACE_IDLE;
            ResetCountdownSyncState();
        }
        return;
    }

    if (!IsCoopGameplayActive() && flowState == 6) {
        InvalidatePublishedMotion();
        // Pure singleplayer: do not spawn proxy car, do not refresh peer, do not show co-op overlay!
        SetOverlayText("");
        return;
    }

    uint32_t currentTrack = 0;
    ReadGameMemory(0x0089E7A0, &currentTrack, sizeof(currentTrack));
    if ((gCoopRaceState == COOP_RACE_IN_PROGRESS || gCoopRaceState == COOP_RACE_STARTING) && CoopRaceReturnedToFreeRoam(flowState,currentTrack)) {
        gCoopRaceState = COOP_RACE_IDLE;
        ResetCountdownSyncState();
        LogMessage("[RACE] Returned to Free Roam (flow=6, track=4000); reset gCoopRaceState to COOP_RACE_IDLE.\r\n");
    }
    if (gCoopRaceState == COOP_RACE_IDLE && flowState == 6) {
        ResetCountdownSyncState();
    }

    CoopPerf::Scope perfMotion(CoopPerf::Part::PeerMotion);
    LocalVehicleState localState{};
    const bool hasLocal = ReadPlayerVehicleState(localState);
    if (hasLocal) PublishLocalMotion(localState,nowTick);
    else InvalidatePublishedMotion();

    RemotePlayerState remote{};
    const bool hasRemote = GetRemotePlayerState(remote);
    const char* targetModel = (hasRemote && remote.current.carModel[0] != '\0') ? remote.current.carModel : nullptr;

    // Smart Proxy Vehicle Management:
    // Native car slots are not player identities: either slot can own the
    // local Player. Use the same root-based identity as standings and finishes.
    uintptr_t proxyCar = 0;
    const char* carTypeStr = "NONE";

    const uintptr_t dedicatedCar = hasLocal ? CoopRaceCar(true) : 0;

    bool dedicatedActive = false;
    if (dedicatedCar >= 0x01000000 && dedicatedCar != localState.playerInstance) {
        uintptr_t pVehicle = 0,peerRide = 0;
        if (ReadPeerSetupTarget(dedicatedCar,peerRide) &&
            ReadGameMemory(dedicatedCar + 0x34, &pVehicle, sizeof(pVehicle)) && pVehicle >= 0x01000000) {
            dedicatedActive = true;
            gBoundProxyCar = dedicatedCar;
            gSelectedCandidateIndex = 0;
            carTypeStr = "DEDICATED";
            if (hasLocal) {
                float cpos[3]{};
                if (ReadGameMemory(dedicatedCar + 0x60, cpos, sizeof(cpos))) {
                    float dx = cpos[0] - localState.x;
                    float dy = cpos[1] - localState.y;
                    float dz = cpos[2] - localState.z;
                    gBoundProxyDist = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
            }
        }
    }

    // Never commandeer unrelated traffic when a dedicated peer car is absent.
    if (!dedicatedActive) {
        gBoundProxyCar = 0;
        gBoundProxyDist = 0.0f;
        gCandidateCount = 0;
    }
    proxyCar = gBoundProxyCar;
    { CoopPerf::Scope perf(CoopPerf::Part::Appearance); TickPeerAppearanceRefresh(flowState); }

    if (IsValidUserPtr(proxyCar)) {
        // Keep active flag asserted
        BYTE active = 1;
        SafeWriteBytes(proxyCar + 0x550, &active, sizeof(active));

        // Ensure SimEntity mesh is unhidden
        uintptr_t simEntity = 0;
        if (ReadGameMemory(proxyCar + 0x3C, &simEntity, sizeof(simEntity)) && IsValidUserPtr(simEntity)) {
            BYTE zero = 0;
            SafeWriteBytes(simEntity + 0x17, &zero, sizeof(zero));
        }

        // Native route fraction, meters and lap-aware progress are distinct.
        if ((gCoopRaceState == COOP_RACE_IN_PROGRESS || gCoopRaceState == COOP_RACE_STARTING) && hasRemote) {
            SyncNativePeerRaceProgress(proxyCar,remote.current);

            // Ensure proxy car's VehicleParam has remote flag asserted and peer name populated
            // so HUD standings (0x004C93B8) displays 'Player 2' (or 'Player 1') instead of blank!
            uintptr_t pVehParam = 0;
            if (ReadGameMemory(proxyCar + 0x14, &pVehParam, sizeof(pVehParam)) && IsValidUserPtr(pVehParam)) {
                BYTE remoteFlag = 1;
                SafeWriteBytes(pVehParam + 0x900, &remoteFlag, sizeof(remoteFlag));
                const char* peerName = gIsSecondInstance ? "Player 1" : "Player 2";
                char nameBuf[16] = {0};
                std::strncpy(nameBuf, peerName, 15);
                SafeWriteBytes(pVehParam + 0x90C, nameBuf, 16);
            }
        }
    }

    // Cache RideInfo pointers for ultra-fast zero-overhead wheel renderer hook
    gCachedProxyRideInfo = 0;
    if (proxyCar >= 0x01000000) {
        ReadGameMemory(proxyCar + 0x10, &gCachedProxyRideInfo, sizeof(gCachedProxyRideInfo));
    }
    uintptr_t raceParamCars = 0;
    gCachedRaceParamCar1Ride = 0;
    if (ReadGameMemory(0x0089E810, &raceParamCars, sizeof(raceParamCars)) && raceParamCars >= 0x01000000) {
        gCachedRaceParamCar1Ride = raceParamCars + 0x950;
    }

    // Apply Real-time Transform Synchronization or One-time Initial Standby Placement
    static bool sInitialStandbyParked = false;
    if (proxyCar != 0 && hasLocal) {
        if (gRemoteSyncEnabled && hasRemote) {
            float remoteX = 0.0f, remoteY = 0.0f, remoteZ = 0.0f, remoteSpeed = 0.0f, remoteHeading = 0.0f;
            int16_t remoteSteer = 0;
            uint8_t remoteBrake = 0, remoteReverse = 0;
            float remoteWheelPhase[4]{}, remoteWheelSpeed[4]{};
            GetInterpolatedRemotePosition(remote, remoteX, remoteY, remoteZ, remoteSpeed, remoteHeading,
                &remoteSteer, &remoteBrake, &remoteReverse, remoteWheelPhase, remoteWheelSpeed);

            // Anti-penetration clearance:
            // When both cars are stationary (< 1.5 km/h) and at virtually the same spawn coordinates (< 2.5m apart),
            // offset the remote proxy laterally to prevent Havok collision solver from entering an intense penetration loop (5 FPS lag)!
            float dx = remoteX - localState.x;
            float dy = remoteY - localState.y;
            float dist2D = std::sqrt(dx * dx + dy * dy);
            static uint64_t clearanceSession=0;
            static uint32_t clearanceEpoch=0;
            static DWORD clearanceUntil=0;
            if (clearanceSession!=remote.current.sessionId || clearanceEpoch!=remote.current.worldEpoch) {
                clearanceSession=remote.current.sessionId; clearanceEpoch=remote.current.worldEpoch;
                clearanceUntil=nowTick+750;
            }
            if (static_cast<int32_t>(clearanceUntil-nowTick)>0 &&
                remoteSpeed < 1.5f && localState.speedKmh < 1.5f && dist2D < 2.5f &&
                std::fabs(remoteZ-localState.z)<1.0f) {
                constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
                const float rad = localState.headingDeg * kDegToRad;
                const float rx = -std::sin(rad);
                const float ry = std::cos(rad);
                float side = gIsSecondInstance ? -3.5f : 3.5f;
                remoteX = localState.x + rx * side;
                remoteY = localState.y + ry * side;
            }

            InjectVehicleTransform(proxyCar, localState.playerInstance, remoteX, remoteY, remoteZ,
                remoteHeading, remoteSpeed, remoteSteer, remoteBrake, remoteReverse, remoteWheelPhase, remoteWheelSpeed,
                remote.rendered.wheelRotation, remote.rendered.wheelRotationValidMask, &remote.rendered);
            LogMotionDiagnostics(proxyCar,remote,nowTick);
            static uintptr_t lastSkidCar = 0;
            static uint32_t lastSkidSequence = 0;
            if (lastSkidCar != proxyCar) {
                lastSkidCar = proxyCar;
                lastSkidSequence = 0;
            }
            if (nowTick - remote.receivedAtMs <= 150 &&
                lastSkidSequence != remote.rendered.sequence) {
                ReplayPeerSkidEffects(proxyCar, remote.rendered.skidEffects);
                lastSkidSequence = remote.rendered.sequence;
            }
        } else if (!sInitialStandbyParked && dedicatedActive) {
            // Place Car 1 ONCE beside player at startup so it is on the road in the parking lot:
            constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
            const float rad = localState.headingDeg * kDegToRad;
            const float rx = -std::sin(rad);
            const float ry = std::cos(rad);
            const float standbyX = localState.x + rx * 3.5f;
            const float standbyY = localState.y + ry * 3.5f;
            const float standbyZ = localState.z;
            InjectVehicleTransform(proxyCar, localState.playerInstance, standbyX, standbyY, standbyZ,
                localState.headingDeg, 0.0f, 0, 1, 0);
            sInitialStandbyParked = true;
        }
    } else {
        sInitialStandbyParked = false;
    }

    if (!gOverlayCreated) {
        gOverlayCreated = true;
        ConfigureOverlayLayout();
    }


    if (hasRaceBanner) {
        ConfigureOverlayLayout();
        return;
    }

    if (!gOverlayEnabled) {
        if (sPrevHadRaceBanner) {
            sPrevHadRaceBanner = false;
            SetOverlayText("");
        }
        return;
    }

    char proxyModelName[16] = {};
    if (proxyCar != 0) {
        GetCarModelName(proxyCar, proxyModelName, sizeof(proxyModelName));
    }

    char panel[600]{};
    if (hasLocal) {
        const int carIndexDisplay = (gCandidateCount > 0) ? (gSelectedCandidateIndex + 1) : 0;
        if (hasRemote) {
            float remoteX = 0.0f, remoteY = 0.0f, remoteZ = 0.0f, remoteSpeed = 0.0f, remoteHeading = 0.0f;
            GetInterpolatedRemotePosition(remote, remoteX, remoteY, remoteZ, remoteSpeed, remoteHeading);
            std::snprintf(panel, sizeof(panel),
                "COOP OBSERVER [F10] | GHOST SYNC [F9]: %s^LOCAL [%s]: Pos: %.1f, %.1f, %.1f | Spd: %.0f km/h | Yaw: %.0f deg^REMOTE [%s] #%lu: Pos: %.1f, %.1f, %.1f | Spd: %.0f km/h | Yaw: %.0f deg (age %lums)^PROXY: 0x%08X [%s] (Dist: %.1fm, %s) [F8: Car %d/%d] | Status: %s",
                gRemoteSyncEnabled ? "ON" : "OFF (Press F9)",
                localState.carModel[0] ? localState.carModel : "CAR",
                localState.x, localState.y, localState.z, localState.speedKmh, localState.headingDeg,
                remote.current.carModel[0] ? remote.current.carModel : "PEER",
                static_cast<unsigned long>(remote.current.sequence),
                remoteX, remoteY, remoteZ, remoteSpeed, remoteHeading,
                static_cast<unsigned long>(GetTickCount() - remote.receivedAtMs),
                static_cast<unsigned int>(gBoundProxyCar),
                proxyModelName[0] ? proxyModelName : carTypeStr,
                gBoundProxyDist, carTypeStr,
                carIndexDisplay, gCandidateCount,
                (proxyCar != 0) ? (gRemoteSyncEnabled ? "SYNCING [ACTIVE]" : "READY (Press F9)") : "SEARCHING");
        } else {
            std::snprintf(panel, sizeof(panel),
                "COOP OBSERVER [F10] | GHOST SYNC [F9]: %s^LOCAL [%s]: Pos: %.1f, %.1f, %.1f | Spd: %.0f km/h | Yaw: %.0f deg^REMOTE: waiting for peer packets...^PROXY: 0x%08X [%s] (%s) [F8: Car %d/%d]",
                gRemoteSyncEnabled ? "ON" : "OFF (Press F9)",
                localState.carModel[0] ? localState.carModel : "CAR",
                localState.x, localState.y, localState.z, localState.speedKmh, localState.headingDeg,
                static_cast<unsigned int>(gBoundProxyCar),
                proxyModelName[0] ? proxyModelName : carTypeStr,
                carTypeStr,
                carIndexDisplay, gCandidateCount);
        }
    } else {
        std::snprintf(panel, sizeof(panel),
            "COOP OBSERVER [F10]^Vehicle: unavailable (enter Free Roam)");
    }
    SetOverlayText(panel);
    ConfigureOverlayLayout();
}

bool InstallUiTickHook() {
    auto* hook = reinterpret_cast<BYTE*>(kUiTickHook);
    if (*hook != 0xC3) {
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(hook, 5, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&UiTick) -
        (static_cast<intptr_t>(kUiTickHook) + 5);
    hook[0] = 0xE9;
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), hook, 5);

    DWORD unused = 0;
    VirtualProtect(hook, 5, oldProtection, &unused);
    return true;
}

#include "CoopContactRosterNative.inl"

// Prevents 0xC0000005 crash at 0x0041C113 during vehicle-to-vehicle collision
__declspec(naked) void SafeCollisionHook() {
    __asm {
        mov eax, [esi + 0x6E0]
        cmp eax, 0x01000000
        jb skipCol
        cmp eax, 0x7FFFFFFF
        ja skipCol
        push edi
        mov edi, [eax]
        push 0x0041C115
        ret

    skipCol:
        push edi
        mov byte ptr [esp + 0x13], 0
        push 0x0041C253
        ret
    }
}

bool InstallCollisionPatch() {
    auto* hook = reinterpret_cast<BYTE*>(0x0041C10C);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&SafeCollisionHook) -
        (0x0041C10C + 5);
    hook[0] = 0xE9;
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP the remaining byte
    FlushInstructionCache(GetCurrentProcess(), hook, 6);

    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    return true;
}

// Prevents 0xC0000005 crash at 0x004022E1 when pVehicle at [Car + 0x34] is NULL or despawned
__declspec(naked) void SafeCarUpdateHook() {
    __asm {
        mov ecx, [eax + 0x34]
        cmp ecx, 0x01000000
        jb skipCall
        mov edx, [ecx]
        push 0x004022E3
        ret

    skipCall:
        push 0x00402321
        ret
    }
}

bool InstallCarUpdateCrashPatch() {
    auto* hook = reinterpret_cast<BYTE*>(0x004022DE);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&SafeCarUpdateHook) -
        (0x004022DE + 5);
    hook[0] = 0xE9;
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), hook, 5);

    DWORD unused = 0;
    VirtualProtect(hook, 5, oldProtect, &unused);
    return true;
}

// This entry changes physics mode, not just despawning. Never suppress native
// nonzero activation requests: doing so strands the peer in its mode-0 shell.
#ifdef COOP_PHYSICS_MODE_TEST_FIXTURE
// Private fixture addresses only; production keeps the verified native sites.
static uintptr_t gPhysicsModeFixtureEntry = 0x005F46A0;
static uintptr_t gPhysicsModeFixtureResume = 0x005F46A7;
static uintptr_t gPhysicsModeFixtureFlow = 0x008654A4;
#endif
__declspec(naked) void SafeCarDespawnHook() {
    __asm {
        cmp dword ptr [esp + 4], 0
        jne doDespawn
        cmp dword ptr [gCoopGameplayActive], 0
        je doDespawn
        // Ensure proxy car is actively bound
        cmp gBoundProxyCar, 0
        je doDespawn

        // ECX is Car* being checked for despawn
        cmp ecx, gBoundProxyCar
        jne doDespawn
        // SetupRide must retire the old mover before replacing its witness.
        // Only that exact verified game-thread transaction may release it.
        cmp ecx, gPeerModelSwapCar
        je doDespawn

        // Check if we are currently in gameplay (flow == 6)
#ifdef COOP_PHYSICS_MODE_TEST_FIXTURE
        mov eax, dword ptr [gPhysicsModeFixtureFlow]
        cmp dword ptr [eax], 6
#else
        cmp dword ptr ds:[0x008654A4], 6
#endif
        jne doDespawn

        // Only hold a bound CO-OP peer's mode-zero deactivation in gameplay.
        ret 4

    doDespawn:
        push -1
        push 0x00779BA2
#ifdef COOP_PHYSICS_MODE_TEST_FIXTURE
        push dword ptr [gPhysicsModeFixtureResume]
#else
        push 0x005F46A7
#endif
        ret
    }
}

bool InstallCarDespawnPatch() {
#ifdef COOP_PHYSICS_MODE_TEST_FIXTURE
    const uintptr_t entry = gPhysicsModeFixtureEntry;
#else
    constexpr uintptr_t entry = 0x005F46A0;
#endif
    auto* hook = reinterpret_cast<BYTE*>(entry);
    const BYTE original[]{0x6A,0xFF,0x68,0xA2,0x9B,0x77,0x00};
    BYTE actual[sizeof(original)]{};
    if (!ReadGameMemory(entry,actual,sizeof(actual)) ||
        std::memcmp(actual,original,sizeof(original))!=0) {
        LogMessage("[ERROR] Native physics-mode entry differs; peer deactivation guard not installed.\r\n");
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&SafeCarDespawnHook) -
        (entry + 5);
    hook[0] = 0xE9;
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP byte 6
    hook[6] = 0x90; // NOP byte 7
    FlushInstructionCache(GetCurrentProcess(), hook, 7);

    DWORD unused = 0;
    VirtualProtect(hook, 7, oldProtect, &unused);
    return true;
}

// Telemetry counter for intercepted collision null pointers
volatile uint32_t gCollisionWitnessInterceptCount = 0;

// Prevents 0xC0000005 crash at 0x00593A5E in CollisionBodyWitness (narrow-phase vehicle contact)
// Target: 0x00593A5B (9 bytes: mov eax, [ecx + 0x68]; mov edx, [eax + 0x158])
__declspec(naked) void SafeCollisionWitnessHook() {
    __asm {
        mov eax, dword ptr [ecx + 0x68]
        test eax, eax
        jz handleNullWitness

        // Normal case: eax is valid body pointer
        mov edx, dword ptr [eax + 0x158]
        push 0x00593A64
        ret

    handleNullWitness:
        inc dword ptr [gCollisionWitnessInterceptCount]

        // Pop float loaded onto FPU stack at 0x593a4d / 0x593a55
        fstp st(0)

        // Store 4x4 identity matrix directly into [ecx + 0x150] (64 bytes / 16 dwords)
        mov dword ptr [ecx + 0x150], 0x3F800000 // (0,0) = 1.0f
        mov dword ptr [ecx + 0x154], 0
        mov dword ptr [ecx + 0x158], 0
        mov dword ptr [ecx + 0x15C], 0

        mov dword ptr [ecx + 0x160], 0
        mov dword ptr [ecx + 0x164], 0x3F800000 // (1,1) = 1.0f
        mov dword ptr [ecx + 0x168], 0
        mov dword ptr [ecx + 0x16C], 0

        mov dword ptr [ecx + 0x170], 0
        mov dword ptr [ecx + 0x174], 0
        mov dword ptr [ecx + 0x178], 0x3F800000 // (2,2) = 1.0f
        mov dword ptr [ecx + 0x17C], 0

        mov dword ptr [ecx + 0x180], 0
        mov dword ptr [ecx + 0x184], 0
        mov dword ptr [ecx + 0x188], 0
        mov dword ptr [ecx + 0x18C], 0x3F800000 // (3,3) = 1.0f

        // Pop saved ecx pushed at 0x00593A40
        pop ecx

        // Function returns eax = pointer to 4x4 matrix at [ecx + 0x150]
        lea eax, [ecx + 0x150]
        ret
    }
}

// Prevents 0xC0000005 crash at 0x005A0B27 in CollisionBodyWitness Method 20
// Target: 0x005A0B1F (8 bytes: mov ecx, [esi + 0x68]; push edi; fstp [esp + 0x18])
__declspec(naked) void SafeCollisionWitnessMethod20Hook() {
    __asm {
        mov ecx, dword ptr [esi + 0x68]
        test ecx, ecx
        jz handleNullMethod20

        // Normal path: ecx is valid
        push edi
        fstp dword ptr [esp + 0x18]
        push 0x005A0B27
        ret

    handleNullMethod20:
        inc dword ptr [gCollisionWitnessInterceptCount]

        // Pop 2 floats currently on FPU stack
        fstp st(0)
        fstp st(0)

        // Clear output vector at [ebp + 0x0c] if valid
        mov eax, dword ptr [ebp + 0x0C]
        test eax, eax
        jz finishMethod20
        mov dword ptr [eax], 0
        mov dword ptr [eax + 4], 0
        mov dword ptr [eax + 8], 0

    finishMethod20:
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        ret 8
    }
}

bool InstallCollisionWitnessPatch() {
    // 1. Hook Method 24 at 0x00593A5B (9 bytes)
    auto* hook1 = reinterpret_cast<BYTE*>(0x00593A5B);
    DWORD oldProtect1 = 0;
    if (VirtualProtect(hook1, 9, PAGE_EXECUTE_READWRITE, &oldProtect1)) {
        const intptr_t rel1 = reinterpret_cast<intptr_t>(&SafeCollisionWitnessHook) -
            (0x00593A5B + 5);
        hook1[0] = 0xE9;
        const int32_t jump1 = static_cast<int32_t>(rel1);
        std::memcpy(hook1 + 1, &jump1, sizeof(jump1));
        std::memset(hook1 + 5, 0x90, 4); // 4 NOPs
        FlushInstructionCache(GetCurrentProcess(), hook1, 9);
        DWORD unused = 0;
        VirtualProtect(hook1, 9, oldProtect1, &unused);
    } else {
        return false;
    }

    // 2. Hook Method 20 at 0x005A0B1F (8 bytes)
    auto* hook2 = reinterpret_cast<BYTE*>(0x005A0B1F);
    DWORD oldProtect2 = 0;
    if (VirtualProtect(hook2, 8, PAGE_EXECUTE_READWRITE, &oldProtect2)) {
        const intptr_t rel2 = reinterpret_cast<intptr_t>(&SafeCollisionWitnessMethod20Hook) -
            (0x005A0B1F + 5);
        hook2[0] = 0xE9;
        const int32_t jump2 = static_cast<int32_t>(rel2);
        std::memcpy(hook2 + 1, &jump2, sizeof(jump2));
        std::memset(hook2 + 5, 0x90, 3); // 3 NOPs
        FlushInstructionCache(GetCurrentProcess(), hook2, 8);
        DWORD unused = 0;
        VirtualProtect(hook2, 8, oldProtect2, &unused);
    } else {
        return false;
    }

    return true;
}

// Prevents 0xC0000005 crash at 0x004CA89D when FindTrackInfo returns NULL (e.g. invalid track ID or race transition)
void* __cdecl SafeFindTrackInfoForMap(int trackId) {
    void* info = reinterpret_cast<void*(__cdecl*)(int)>(0x005D3E40)(trackId);
    if (!info) {
        // Fallback to Free Roam track (4000) so minimap / world map renderer never dereferences NULL!
        info = reinterpret_cast<void*(__cdecl*)(int)>(0x005D3E40)(4000);
    }
    return info;
}

bool InstallTrackMapCrashShield() {
    auto* hook = reinterpret_cast<BYTE*>(0x004CA889);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&SafeFindTrackInfoForMap) -
        (0x004CA889 + 5);
    hook[0] = 0xE8; // CALL
    const int32_t rel32 = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &rel32, sizeof(rel32));
    FlushInstructionCache(GetCurrentProcess(), hook, 5);

    DWORD unused = 0;
    VirtualProtect(hook, 5, oldProtect, &unused);
    return true;
}

// Prevents 0xC0000005 crashes at 0x00403C6C and 0x004DA4ED by neutralizing split-screen code branches
// (NFSU2 PC has no local split-screen; local player 2 viewport at 0x008900B0 is always NULL)
bool InstallSplitScreenCrashShield() {
    // 1. Patch 0x00403C48: jne 0x403d19 -> jmp 0x403d19 (E9 CC 00 00 00 90)
    auto* patch1 = reinterpret_cast<BYTE*>(0x00403C48);
    DWORD oldProtect1 = 0;
    if (VirtualProtect(patch1, 6, PAGE_EXECUTE_READWRITE, &oldProtect1)) {
        patch1[0] = 0xE9; // JMP rel32
        const int32_t rel = 0x00403D19 - (0x00403C48 + 5);
        std::memcpy(patch1 + 1, &rel, sizeof(rel));
        patch1[5] = 0x90; // NOP
        FlushInstructionCache(GetCurrentProcess(), patch1, 6);
        DWORD unused = 0;
        VirtualProtect(patch1, 6, oldProtect1, &unused);
    }

    // 2. Patch 0x004DA23B: jne 0x4da38f -> 6 NOPs (stay on single-player HUD path)
    auto* patch2 = reinterpret_cast<BYTE*>(0x004DA23B);
    DWORD oldProtect2 = 0;
    if (VirtualProtect(patch2, 6, PAGE_EXECUTE_READWRITE, &oldProtect2)) {
        std::memset(patch2, 0x90, 6);
        FlushInstructionCache(GetCurrentProcess(), patch2, 6);
        DWORD unused = 0;
        VirtualProtect(patch2, 6, oldProtect2, &unused);
    }

    return true;
}


// ---------------------------------------------------------------------------
// Universal Dedicated Co-op Car Spawner Hook:
// 1. RaceParameters::AddVehicle (0x00601600):
//    Saves local player garage car to scripts\Player%dCar.bin whenever selected.
//    Leaves menus in single-player state (VehicleCount = 1) so menus never freeze!
// 2. RaceLoadingLoop (0x006065B0):
//    Executes ONLY during 3D race/Free Roam loading screen (flowState == 4).
//    Loads peer's garage car into Car 1, sets Grid=2, PlayerIndex=1, CarType=2,
//    and sets VehicleCount = 2 so the engine spawns both cars natively!
// ---------------------------------------------------------------------------
bool ResetPeerSpawnForLocalSelection(uint32_t carType,uint8_t remote) {
    if(!IsCoopGameplayActive() || carType!=1 || remote!=0) return false;
    // Local AddVehicle starts a NEW native parameter list, including flow 7
    // race -> Free Roam reloads. Do not carry the previous list's spawn latch.
    gCar1LoadedForRace=false;
    ++gCoopFleetLocalLoadToken;
    if (!gCoopFleetLocalLoadToken) ++gCoopFleetLocalLoadToken;
    return true;
}

bool CoopSpawnUsesRaceGrid(uint32_t track) {
    return track!=0 && track!=4000 &&
        (gCoopRaceState==COOP_RACE_STARTING || gCoopRaceState==COOP_RACE_IN_PROGRESS);
}

void __cdecl OnAfterRaceParametersAddVehicle(void* pRaceParams, void* pAddedVehParam) {
    if (!IsCoopGameplayActive()) return;
    if (!pAddedVehParam) return;
    auto* pParam = reinterpret_cast<uint8_t*>(pAddedVehParam);
    uint32_t carType = *reinterpret_cast<uint32_t*>(pParam + 4);

    // We only care when the game adds the local human player car (CarType == 1)
    if (!ResetPeerSpawnForLocalSelection(carType,pParam[0x900])) return;

    DWORD flowState = 0;
    ReadGameMemory(kGameFlowState, &flowState, sizeof(flowState));

    // When in frontend (flowState == 3), ALWAYS keep VehicleCount = 1!
    // This completely prevents frontend transmission / stats menus from accessing uninitialized Slot 1 (crash 0x005ACB21)!
    *reinterpret_cast<uint32_t*>(0x0089E814) = 1;

    int localPlayerNum = gIsSecondInstance ? 2 : 1;
    char localPath[MAX_PATH];
    std::snprintf(localPath, sizeof(localPath), "%sPlayer%dParam.v9.bin", gScriptsDirectory, localPlayerNum);

    // Save preliminary local car param to file & broadcast over UDP
    uint8_t portableParam[0x940]{};
    SerializeVehicleParamPortable(portableParam, pParam);
    FILE* fpLocal = std::fopen(localPath, "wb");
    if (fpLocal) {
        std::fwrite(portableParam, 1, sizeof(portableParam), fpLocal);
        std::fclose(fpLocal);
        char logBuf[160];
        std::snprintf(logBuf, sizeof(logBuf), "[SPAWN] AddVehicle: Saved local garage car preview (Player %d) to %s (flow=%lu, count=1)\r\n",
            localPlayerNum, localPath, static_cast<unsigned long>(flowState));
        LogMessage(logBuf);
    }

    if (gGameSocket != INVALID_SOCKET) {
        CarSyncPacket syncPkt{};
        syncPkt.magic = kPacketMagic;
        syncPkt.version = kPacketVersion;
        syncPkt.size = sizeof(CarSyncPacket);
        syncPkt.playerIndex = localPlayerNum;
        syncPkt.sentAtMs = GetTickCount();
        std::memcpy(syncPkt.vehicleParam, portableParam, sizeof(syncPkt.vehicleParam));
        QueueLocalPeerSelection(syncPkt);
    }
}

__declspec(naked) void TrampolineRaceParametersAddVehicle() {
    __asm {
        push esi
        push edi
        mov edi, dword ptr [esp + 0x0C]
        push 0x00601606
        ret
    }
}

__declspec(naked) void HookedRaceParametersAddVehicle() {
    __asm {
        // [esp] = caller return address
        // [esp + 4] = pVehicleParam
        // ecx = this (RaceParameters* = 0x0089E7A0)

        // 1. Call original AddVehicle via trampoline
        push [esp + 4]
        call TrampolineRaceParametersAddVehicle

        // 2. Call our post-add handler
        pushad
        push [esp + 0x24]   // pVehicleParam (shifted by 0x20 from pushad)
        push 0x0089E7A0     // RaceParameters*
        call OnAfterRaceParametersAddVehicle
        add esp, 8
        popad

        ret 4
    }
}

// ---------------------------------------------------------------------------
// RaceLoadingLoop Hook (0x006065B0):
// Executes ONLY during 3D race/Free Roam loading screen (flowState == 4).
// At this point, frontend car customization & 0x5260b0 has fully completed.
// We save finalized Car 0, load peer's garage car into Car 1, configure
// grid & racer properties, and set VehicleCount = 2 right before spawn!
// ---------------------------------------------------------------------------
static uint32_t CoopCommittedPeerSpawnCount(uint8_t role,uint8_t remote,uint32_t id) {
    if(!gCar1LoadedForRace || !IsRemotePeerSetupMetadata(role,remote,id)) return 0;
    // Fleet owns the final AI+traffic tail when enabled. Its missing commit
    // cannot be substituted with the AI layer's provisional smaller count.
    const uint32_t committed=gCoopFleetEnabled?CoopFleetCommittedParameterCount():
        (gCoopAIEnabled?CoopAIRosterCommittedCount():0);
    if(committed || (!gCoopAIEnabled && !gCoopFleetEnabled)) return committed?committed:2;
    // A retained human is not a current-generation AI/Fleet commitment.
    gCar1LoadedForRace=false;
    return 0;
}
void __cdecl SetupCoopCar1BeforeRaceLoading() {
    if (!IsCoopGameplayActive()) {
        return;
    }
    uintptr_t carArrayPtr = 0;
    if (!ReadGameMemory(0x0089E810, &carArrayPtr, sizeof(carArrayPtr)) || !IsValidUserPtr(carArrayPtr)) {
        return;
    }

    uint8_t* pCar0 = reinterpret_cast<uint8_t*>(carArrayPtr);
    uint8_t* pCar1 = pCar0 + 0x940;
    ApplyCoopRaceReturnOrigin(carArrayPtr);
    // The latch is valid only for a prepared remote-player slot, never a
    // stock opponent left in freshly recreated parameter storage.
    if(const uint32_t committed=CoopCommittedPeerSpawnCount(pCar1[1],pCar1[0x900],
        *reinterpret_cast<uint32_t*>(pCar1+4))) {
        *reinterpret_cast<uint32_t*>(0x0089E814)=committed;
        return;
    }
    uint32_t localModel = 0;
    if (!ReadGameMemory(carArrayPtr + 0x10, &localModel, sizeof(localModel)) ||
        !IsSupportedCarModel(localModel)) return;

    int localPlayerNum = gIsSecondInstance ? 2 : 1;
    int peerPlayerNum = gIsSecondInstance ? 1 : 2;

    char localPath[MAX_PATH];
    std::snprintf(localPath, sizeof(localPath), "%sPlayer%dParam.v9.bin", gScriptsDirectory, localPlayerNum);

    // 1. Save finalized local garage car (with complete visual customization, paint, vinyls, stance from 0x5260b0!)
    uint8_t localSaveBuf[0x940]{};
    SerializeVehicleParamPortable(localSaveBuf, pCar0);

    FILE* fpLocal = std::fopen(localPath, "wb");
    if (fpLocal) {
        std::fwrite(localSaveBuf, 1, 0x940, fpLocal);
        std::fclose(fpLocal);
        char logBuf[160];
        std::snprintf(logBuf, sizeof(logBuf), "[SPAWN] Stage4CarLoader: Saved finalized local garage car (Player %d) to %s (0x940 bytes)\r\n",
            localPlayerNum, localPath);
        LogMessage(logBuf);
    }

    // Broadcast over UDP
    if (gGameSocket != INVALID_SOCKET) {
        CarSyncPacket syncPkt{};
        syncPkt.magic = kPacketMagic;
        syncPkt.version = kPacketVersion;
        syncPkt.size = sizeof(CarSyncPacket);
        syncPkt.playerIndex = localPlayerNum;
        syncPkt.sentAtMs = GetTickCount();
        std::memcpy(syncPkt.vehicleParam, localSaveBuf, 0x940);
        syncPkt.selectionStage = 1;
        QueueLocalPeerSelection(syncPkt);
    }

    // Only current-session, current-protocol UDP data is eligible for a peer.
    // A saved binary from the previous executable/version may contain raw
    // process pointers or a different player's old selection: never spawn it.
    bool loadedFromPeer = false;
    uint8_t peerBuf[0x940]{};
    AcquireSRWLockShared(&gPeerSyncLock);
    if (gHavePeerSyncPkt && IsReadyPeerSelection(gLatestPeerSyncPkt,
        static_cast<uint32_t>(peerPlayerNum), gPeerSyncReceivedAtMs, GetTickCount())) {
        uint32_t peerModel = 0;
        std::memcpy(&peerModel, gLatestPeerSyncPkt.vehicleParam + 0x10, sizeof(peerModel));
        if (IsSupportedCarModel(peerModel)) {
            std::memcpy(peerBuf, gLatestPeerSyncPkt.vehicleParam, sizeof(peerBuf));
            loadedFromPeer = true;
        }
    }
    ReleaseSRWLockShared(&gPeerSyncLock);

    // Clone ONLY this process's own race parameter through its native copier;
    // unlike memcpy this repairs the embedded spline/garage self-pointers.
    using CopyVehicleParamFn = void*(__thiscall*)(void*, const void*);
    using RideInfoCtorFn = void*(__thiscall*)(void*);
    using RideInfoInitFn = void(__thiscall*)(void*, int, int, char, char);
    using InitAllStockPartsFn = void(__thiscall*)(void*, int);
    using GetPartFn = void*(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, void*, int);
    using SetupPartsFn = void(__thiscall*)(void*);
    auto pCopyVehicleParam = reinterpret_cast<CopyVehicleParamFn>(0x005FC130);
    auto pRideCtor = reinterpret_cast<RideInfoCtorFn>(0x0040C910);
    auto pRideInit = reinterpret_cast<RideInfoInitFn>(0x00610270);
    auto pInitAllStockParts = reinterpret_cast<InitAllStockPartsFn>(0x00637040);
    auto pGetPart = reinterpret_cast<GetPartFn>(0x0061B980);
    auto pSetupParts = reinterpret_cast<SetupPartsFn>(0x0061BCD0);
    std::memset(pCar1, 0, 0x940);
    uint8_t* pRide1 = pCar1 + 0x10;
    pRideCtor(pRide1);
    pCopyVehicleParam(pCar1, pCar0);

    uint32_t peerModel = loadedFromPeer
        ? *reinterpret_cast<uint32_t*>(peerBuf + 0x10)
        : localModel;
    int resolvedCount = 0;
    int unresolvedCount = 0;
    if (loadedFromPeer) {
        // Build a complete stock peer model first; if an optional custom part
        // is absent locally, retain that model's stock mesh, never another car.
        pRideInit(pRide1, static_cast<int>(peerModel), 0, 0, 0);
        pInitAllStockParts(pRide1, 0);
        const uint8_t* srcRide = peerBuf + 0x10;
        CopyPortableRideValues(pRide1, srcRide);
        void* pPartManager = reinterpret_cast<void*>(0x008A2B68);
        for (uint32_t slot = 0; slot < 170; ++slot) {
            uint32_t partHash = 0;
            std::memcpy(&partHash, srcRide + 0x590 + slot * 4, sizeof(partHash));
            if (partHash == 0) continue;
            // 0x6101E0 returns a POINTER to a slot-specific hash pair, not a
            // car hash! Native 0x61B980 performs this mapping internally and
            // handles both car-specific and shared part databases correctly.
            void* part = pGetPart(pPartManager, peerModel, slot, partHash, nullptr, -1);
            uint32_t resolvedHash = 0;
            if (part != nullptr && ReadGameMemory(reinterpret_cast<uintptr_t>(part),
                &resolvedHash, sizeof(resolvedHash)) && resolvedHash == partHash) {
                *reinterpret_cast<void**>(pRide1 + 0x590 + slot * 4) = part;
                ++resolvedCount;
            } else {
                ++unresolvedCount;
            }
        }
        // Recompute dependent body/widebody parts and visibility flags BEFORE
        // native CarLoader requests geometry and textures. Never mutate parts
        // from a rendering (or collision) callback.
        pSetupParts(pRide1);
    }
    // Native 0x5F02B0 passes VehicleParam.PlayerIndex + 1 to 0x61C280.
    // Slot 1 is LOCAL DUMMY_SKIN1, not the partner: sharing it makes the peer
    // inherit local body/rim paint and vinyls. PlayerIndex 1 needs texture slot 2.
    pCar1[1] = kPeerRenderPlayerIndex;
    const int peerTextureSlot = TextureSlotForPlayerIndex(pCar1[1]);
    reinterpret_cast<void(__thiscall*)(void*, int)>(0x0061C280)(pRide1, peerTextureSlot);
    *reinterpret_cast<uint32_t*>(pCar1 + 0x8F8) = 0; // no cloned local streaming handle
    char logBuf[240];
    std::snprintf(logBuf, sizeof(logBuf),
        "[SPAWN] CarLoader peer model=%lu local=%lu source=%s custom_parts=%d stock_fallbacks=%d texture_slot=%d.\r\n",
        static_cast<unsigned long>(peerModel), static_cast<unsigned long>(localModel),
        loadedFromPeer ? "finalized_udp_v9" : "local_preview_waiting_peer", resolvedCount, unresolvedCount, peerTextureSlot);
    LogMessage(logBuf);

    // 7. Configure Car 1 properties for native dedicated loading
    pCar1[0] = 0;
    pCar1[1] = kPeerRenderPlayerIndex;
    pCar1[2] = 0;
    *reinterpret_cast<uint32_t*>(pCar1 + 4) = 1; // CarType = 1 (CAR_TYPE_PLAYER: keeps 3D mesh & SimEntity active, never hidden by 0x5F6E20!)
    pCar1[0x900] = 1; // Remote player flag (prevents 0x00601197 from overwriting LocalPlayerModel / LocalPlayerRideInfo)

    // Overwrite Car 1 driver name so it does not clone the local profile name ("ASHAN").
    // Native SPEED2.EXE reads VehicleParam + 0x90C when VehicleParam + 0x900 != 0
    // (verified at 0x004C93B8 for HUD standings and 0x004E9C40 for race ranking).
    char* pCar1Name = reinterpret_cast<char*>(pCar1 + 0x90C);
    std::memset(pCar1Name, 0, 16);
    if (gIsSecondInstance) {
        std::strncpy(pCar1Name, "Player 1", 15);
    } else {
        std::strncpy(pCar1Name, "Player 2", 15);
    }

    uint32_t spawnTrack=0;
    ReadGameMemory(gRaceContextAddresses.track,&spawnTrack,sizeof(spawnTrack));
    if (CoopSpawnUsesRaceGrid(spawnTrack)) {
        // ACTIVE CO-OP RACE MODE:
        // Use native track starting grid slots (SetupGridPositions 0x005264A0)
        pCar0[0x91C] = 0;
        pCar1[0x91C] = 0;

        if (!gIsSecondInstance) {
            // HOST (Player 1): Grid slot 1, Client on Grid slot 2
            pCar0[3] = 1;
            pCar1[3] = 2;
        } else {
            // CLIENT (Player 2): Grid slot 2, Host on Grid slot 1
            pCar0[3] = 2;
            pCar1[3] = 1;
        }

        char logBuf[160];
        std::snprintf(logBuf, sizeof(logBuf),
            "[SPAWN] Active Co-op Race: Native grid slots set (Car0=Slot %u, Car1=Slot %u, Driver1='%s')\r\n",
            pCar0[3], pCar1[3], pCar1Name);
        LogMessage(logBuf);
    } else {
        // FREE ROAM MODE:
        // Unconditionally keep local player as default Car 0 slot, and spawn Car 1 at +4.0m lateral offset!
        pCar1[3] = 2; // GridPosition = 2
        pCar1[0x91C] = 1;
        float origX = 1695.2f, origY = -883.6f, origZ = 24.1f;
        uint16_t origHeading = 0;
        if (pCar0[0x91C] != 0) {
            origX = *reinterpret_cast<float*>(pCar0 + 0x920);
            origY = *reinterpret_cast<float*>(pCar0 + 0x924);
            origZ = *reinterpret_cast<float*>(pCar0 + 0x928);
            origHeading = *reinterpret_cast<uint16_t*>(pCar0 + 0x930);
        }
        *reinterpret_cast<float*>(pCar1 + 0x920) = origX + 4.0f;
        *reinterpret_cast<float*>(pCar1 + 0x924) = origY;
        *reinterpret_cast<float*>(pCar1 + 0x928) = origZ;
        *reinterpret_cast<uint16_t*>(pCar1 + 0x930) = origHeading;
    }

    // Use the native AddVehicle copier/counters, not a guessed count-only
    // insertion. The original trampoline avoids our local-preview callback.
    if (*reinterpret_cast<uint32_t*>(0x0089E814) == 1) {
        using AddVehicleFn = void(__thiscall*)(void*, void*);
        auto pAddVehicle = reinterpret_cast<AddVehicleFn>(&TrampolineRaceParametersAddVehicle);
        pAddVehicle(reinterpret_cast<void*>(0x0089E7A0), pCar1);
    }
    *reinterpret_cast<uint32_t*>(0x0089E814) = 2;
    // Native frontend 5260E7 keeps the event's AI budget even though this
    // streaming list contains only the local human and the dedicated peer.
    // AddVehicle has already run: these are FINAL additive categories, not
    // pre-insertion counts (601600 increments the remote-player category).
    if(*reinterpret_cast<uint32_t*>(pCar0+4)==1 && pCar0[0x900]==0 &&
        *reinterpret_cast<uint32_t*>(pCar1+4)==1 && pCar1[0x900]!=0) {
        const uint32_t participants[4]={1,0,1,0};
        SafeWriteBytes(0x0089E7C0,participants,sizeof(participants));
    }
    gCar1LoadedForRace = true;

    if(gCoopAIEnabled && !CommitCoopAIRosterAfterPeer()) {
        LogMessage("[AI_ROSTER] Commit refused; no unverified AI inserted.\r\n");
    }
    if (gCoopFleetEnabled) {
        coop_world::Identity identity{};
        if (!CurrentCoopFleetIdentity(identity) || !CommitCoopFleetAfterHumans(identity))
            LogMessage("[FLEET_ASSETS] Commit refused; no unmatched pooled vehicles inserted.\r\n");
    }

    LogMessage("[SPAWN] Stage4CarLoader: Dedicated Co-op Car 1 added to RaceParameters (CarType=1, Grid=2, count=2, offset=+4m) BEFORE native streaming!\r\n");
}

// Same-model appearance refresh uses the native live-traffic streaming path.
// Keep the existing physics Car and its RideInfo address; never edit a RideInfo
// that an old Visual is using before its replacement assets finish loading.
constexpr uintptr_t kNativeCarLoader = 0x008A3288;
static uintptr_t gPeerAppearanceLoader=kNativeCarLoader;
using PeerAppearanceReadyFn=bool(__thiscall*)(void*,uint32_t);
using PeerAppearanceReleaseFn=void(__thiscall*)(void*,uint32_t);
static PeerAppearanceReadyFn gPeerAppearanceReady=reinterpret_cast<PeerAppearanceReadyFn>(0x61CA10);
static PeerAppearanceReleaseFn gPeerAppearanceRelease=reinterpret_cast<PeerAppearanceReleaseFn>(0x63AE00);
struct PeerAppearanceRefreshState {
    alignas(16) uint8_t stagingRide[0x8F0]{};
    uintptr_t baselineCar = 0;
    uintptr_t pendingCar = 0;
    uintptr_t pendingRide = 0;
    uint64_t baselineHash = 0;
    uint64_t pendingHash = 0;
    uint32_t retainedResource = 0; // only a lease allocated by this helper
    uint32_t pendingResource = 0;
    DWORD pendingSince = 0;
    bool baselineSet = false;
    bool timeoutLogged = false;
};
static PeerAppearanceRefreshState gPeerAppearanceRefresh{};

uint64_t ComputePeerAppearanceFingerprint(const uint8_t* portableParam) {
    if (!portableParam) return 0;
    const uint8_t* ride = portableParam + 0x10;
    uint64_t hash = 14695981039346656037ULL;
    auto add = [&](const uint8_t* bytes, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
    };
    add(ride, 4); // model
    add(ride + 5, 3); // native appearance metadata; not local player index +4
    add(ride + 0x580, 2);
    add(ride + 0x590, 170 * 4); // portable part IDs, not local pointers
    add(ride + 0x838, 0xAA); // native installed/visibility bytes
    // Performance curves, native cache pointers, DUMMY texture identities,
    // timestamps are deliberately excluded.
    return hash;
}

static uintptr_t FindPeerAppearanceResourceNode(uint32_t resourceID) {
    if (!resourceID) return 0;
    const uintptr_t head = gPeerAppearanceLoader + 0x58;
    uintptr_t node = 0;
    if (!ReadGameMemory(head, &node, sizeof(node))) return false;
    for (unsigned i = 0; node != head && i < 128; ++i) {
        uint32_t id = 0, state = 0;
        uintptr_t next = 0;
        if (!IsValidUserPtr(node) ||
            !ReadGameMemory(node + 0x123C, &id, sizeof(id)) ||
            !ReadGameMemory(node + 0x0C, &state, sizeof(state)) ||
            !ReadGameMemory(node, &next, sizeof(next))) return false;
        if (id == resourceID) {
            return node;
        }
        if (next == node) return false;
        node = next;
    }
    return false;
}
bool FindPeerAppearanceResource(uint32_t resourceID, bool& loaded) {
    loaded=false;const uintptr_t node=FindPeerAppearanceResourceNode(resourceID);
    uint32_t state=0;int32_t refs=0;
    if(!node || !ReadGameMemory(node+0x0C,&state,4) || !ReadGameMemory(node+8,&refs,4)) return false;
    loaded=state==2 && refs>0 && gPeerAppearanceReady(reinterpret_cast<void*>(gPeerAppearanceLoader),resourceID);
    return true;
}

void ReleasePeerAppearanceLease(uint32_t& resourceID) {
    bool loaded = false;
    if (resourceID && FindPeerAppearanceResource(resourceID, loaded)) {
        // 63BC70 gives this caller one reference; 63AE00 releases exactly it.
        // Never release the initial Stage4/physics streaming handle.
        int32_t refs=0;const uintptr_t node=FindPeerAppearanceResourceNode(resourceID);
        // Native track teardown63B070 may already have consumed this ref.
        // Never turn an already-retired zero reference into a negative count.
        if(node && ReadGameMemory(node+8,&refs,4) && refs>0)
            gPeerAppearanceRelease(reinterpret_cast<void*>(gPeerAppearanceLoader),resourceID);
    }
    resourceID = 0;
}

bool IsRemotePeerSetupMetadata(uint8_t playerIndex,uint8_t remoteFlag,uint32_t carType) {
    return playerIndex==kPeerRenderPlayerIndex && remoteFlag==1 && carType==1;
}

bool ReadPeerSetupTarget(uintptr_t car,uintptr_t& ride) {
    uintptr_t params=0,actualRide=0;
    uint8_t index=0,remote=0; uint32_t type=0;
    if(!IsValidUserPtr(car) ||
        !ReadGameMemory(car+0x14,&params,sizeof(params)) || !IsValidUserPtr(params) ||
        !ReadGameMemory(car+0x10,&actualRide,sizeof(actualRide)) || actualRide!=params+0x10 ||
        !ReadGameMemory(params+1,&index,sizeof(index)) ||
        !ReadGameMemory(params+4,&type,sizeof(type)) ||
        !ReadGameMemory(params+0x900,&remote,sizeof(remote)) ||
        !IsRemotePeerSetupMetadata(index,remote,type)) return false;
    ride=actualRide;
    return true;
}

bool ReadDedicatedAppearanceTarget(uintptr_t& car,uintptr_t& ride,uint32_t& model) {
    car=CoopRaceCar(true);
    return ReadPeerSetupTarget(car,ride) &&
        ReadGameMemory(ride, &model, sizeof(model)) && IsSupportedCarModel(model);
}

bool ReadNativeAppearancePortable(uintptr_t ride, uint8_t* param) {
    std::memset(param, 0, 0x940);
    if (!ReadGameMemory(ride, param + 0x10, 4) ||
        !ReadGameMemory(ride + 5, param + 0x10 + 5, 3) ||
        !ReadGameMemory(ride + 0x580, param + 0x10 + 0x580, 2) ||
        !ReadGameMemory(ride + 0x838, param + 0x10 + 0x838, 0xAA)) return false;
    for (unsigned slot = 0; slot < 170; ++slot) {
        uintptr_t part = 0;
        uint32_t hash = 0;
        if (!ReadGameMemory(ride + 0x590 + slot * 4, &part, sizeof(part))) return false;
        if (part && (!IsValidUserPtr(part) || !ReadGameMemory(part, &hash, sizeof(hash)))) return false;
        std::memcpy(param + 0x10 + 0x590 + slot * 4, &hash, sizeof(hash));
    }
    return true;
}

bool NativeAppearanceQueueIdle() {
    uint32_t nativeBusy = 1;
    uintptr_t callback = 1, deferred = 1;
    return ReadGameMemory(gPeerAppearanceLoader + 0x14, &nativeBusy, sizeof(nativeBusy)) && !nativeBusy &&
        ReadGameMemory(gPeerAppearanceLoader, &callback, sizeof(callback)) && !callback &&
        ReadGameMemory(0x00864F58, &deferred, sizeof(deferred)) && !deferred;
}

bool CanRebuildPeerModel(uintptr_t car, uintptr_t ride) {
    uintptr_t actualRide=0;
    return car==gBoundProxyCar && ReadPeerSetupTarget(car,actualRide) && ride==actualRide;
}

#include "CoopPeerTextureNative.inl"

using PeerModelModeFn=void(__thiscall*)(void*,uint32_t);
using PeerModelSetupFn=void(__thiscall*)(void*,const void*);
using PeerModelCaptureFn=bool(*)(uintptr_t,float*,float*,float*);
using PeerModelPoseFn=bool(*)(uintptr_t,uintptr_t,const float*,const float*,const float*,const float*);
static PeerModelModeFn gPeerModelMode=reinterpret_cast<PeerModelModeFn>(0x005F46A0);
static PeerModelSetupFn gPeerModelSetup=reinterpret_cast<PeerModelSetupFn>(0x00601110);
static PeerModelCaptureFn gPeerModelCapture=ReadCoopPhysicalMotion;
static PeerModelPoseFn gPeerModelPose=ApplyCoopPhysicalMotion;
static bool(*gPeerModelSupported)(uint32_t)=IsSupportedCarModel;
struct PeerModelPoseRestore {
    uintptr_t car=0,ride=0;uint32_t model=0,mode=0;
    float position[3]{},orientation[4]{},velocity[3]{},angular[3]{};
};
static PeerModelPoseRestore gPeerModelPoseRestore;
static bool RestorePendingPeerModelPose() {
    const auto saved=gPeerModelPoseRestore;
    if(!saved.car) return true;
    uint32_t model=0,mode=0;uintptr_t ride=0;
    if(gPeerModelSwapCar || saved.car!=CoopRaceCar(true) || !ReadPeerSetupTarget(saved.car,ride) ||
        ride!=saved.ride || !ReadGameMemory(ride,&model,4) || model!=saved.model ||
        !ReadGameMemory(saved.car+0x28,&mode,4) || mode!=saved.mode) return false;
    gPeerModelSwapCar=saved.car;
    const bool okay=gPeerModelPose(saved.car,CoopRaceCar(false),saved.position,saved.orientation,saved.velocity,saved.angular);
    gPeerModelSwapCar=0;
    if(okay) gPeerModelPoseRestore={};
    return okay;
}
bool RebuildLivePeerModel(uintptr_t car,uintptr_t ride,const void* staged) {
    uint32_t flow=0,mode=0,targetModel=UINT32_MAX,actualModel=UINT32_MAX,afterMode=UINT32_MAX;
    uintptr_t local=CoopRaceCar(false),verifiedRide=0,mover=0;
    float position[3]{},orientation[4]{},velocity[3]{},angular[3]{};
    if(gPeerModelSwapCar || gPeerModelPoseRestore.car || !IsCoopGameplayActive() || car!=CoopRaceCar(true) || car==local ||
        !CanRebuildPeerModel(car,ride) || !ReadGameMemory(gRaceContextAddresses.flow,&flow,4) || flow!=6 ||
        !ReadGameMemory(car+0x28,&mode,4) || (mode!=0 && mode!=1 && mode!=3) ||
        !ReadGameMemory(reinterpret_cast<uintptr_t>(staged),&targetModel,4) || !gPeerModelSupported(targetModel) ||
        !ReadGameMemory(car+0x60,position,sizeof(position)) ||
        !CoopPhysicalMotionNative::FiniteVector(position,1000000.0f) ||
        !gPeerModelCapture(car,orientation,velocity,angular)) return false;
    struct Scope {
        explicit Scope(uintptr_t value){gPeerModelSwapCar=value;}
        ~Scope(){gPeerModelSwapCar=0;}
    } scope(car);
    // Native callers are constructor/Stage7 shells. Make a live peer the
    // same mode-zero shell BEFORE SetupRide destroys SimEntity/car geometry.
    gPeerModelMode(reinterpret_cast<void*>(car),0);
    if(!ReadGameMemory(car+0x28,&afterMode,4) || afterMode!=0) return false;
    gPeerModelSetup(reinterpret_cast<void*>(car),staged);
    gPeerModelMode(reinterpret_cast<void*>(car),mode);
    if(car!=CoopRaceCar(true) || !ReadPeerSetupTarget(car,verifiedRide) || verifiedRide!=ride ||
        !ReadGameMemory(ride,&actualModel,4) || actualModel!=targetModel ||
        !ReadGameMemory(car+0x28,&afterMode,4) || afterMode!=mode ||
        !ReadGameMemory(car+0x34,&mover,4) || !IsValidUserPtr(mover)) return false;
    // Resolve the NEW mover/body/witness in the existing physical adapter.
    // Do not restore cached pointers, old mass or the old model's geometry.
    if(gPeerModelPose(car,local,position,orientation,velocity,angular)) return true;
    gPeerModelPoseRestore.car=car;gPeerModelPoseRestore.ride=ride;
    gPeerModelPoseRestore.model=targetModel;gPeerModelPoseRestore.mode=mode;
    std::memcpy(gPeerModelPoseRestore.position,position,sizeof(position));
    std::memcpy(gPeerModelPoseRestore.orientation,orientation,sizeof(orientation));
    std::memcpy(gPeerModelPoseRestore.velocity,velocity,sizeof(velocity));
    std::memcpy(gPeerModelPoseRestore.angular,angular,sizeof(angular));
    return false;
}

void TickPeerAppearanceRefresh(DWORD flowState) {
    auto& refresh = gPeerAppearanceRefresh;
    uintptr_t car = 0, ride = 0;
    uint32_t model = UINT32_MAX;
    const bool haveTarget = flowState == 6 && ReadDedicatedAppearanceTarget(car, ride, model);
    if (!haveTarget) {
        gPeerModelPoseRestore={};
        ReleasePeerAppearanceLease(refresh.pendingResource);
        ReleasePeerAppearanceLease(refresh.retainedResource);
        refresh.baselineSet = false;
        refresh.baselineCar = 0;
        return;
    }
    if(gPeerModelPoseRestore.car && gPeerModelPoseRestore.car!=car) gPeerModelPoseRestore={};
    // A temporarily unavailable NEW body must restore its retained old pose
    // before a now-equal model can take the same-model visual commit path.
    if(!RestorePendingPeerModelPose()) return;
    if (refresh.baselineCar != car) {
        ReleasePeerAppearanceLease(refresh.pendingResource);
        ReleasePeerAppearanceLease(refresh.retainedResource);
        uint8_t portable[0x940]{};
        if (!ReadNativeAppearancePortable(ride, portable)) return;
        refresh.baselineHash = ComputePeerAppearanceFingerprint(portable);
        refresh.baselineCar = car;
        refresh.baselineSet = true;
    }
    CarSyncPacket peer{};
    DWORD receivedAt = 0;
    AcquireSRWLockShared(&gPeerSyncLock);
    const bool havePeer = gHavePeerSyncPkt;
    if (havePeer) {
        peer = gLatestPeerSyncPkt;
        receivedAt = gPeerSyncReceivedAtMs;
    }
    ReleaseSRWLockShared(&gPeerSyncLock);
    const DWORD now = GetTickCount();
    if (!havePeer || peer.selectionStage != 2 || !IsReadyPeerSelection(peer,
        gIsSecondInstance ? 1 : 2, receivedAt, now)) return;
    uint32_t peerModel = UINT32_MAX;
    std::memcpy(&peerModel, peer.vehicleParam + 0x10, sizeof(peerModel));
    if (!IsSupportedCarModel(peerModel)) return;
    const uint64_t appearance = ComputePeerAppearanceFingerprint(peer.vehicleParam);
    if (refresh.pendingResource) {
        if (refresh.pendingCar != car || refresh.pendingRide != ride || refresh.pendingHash != appearance) {
            ReleasePeerAppearanceLease(refresh.pendingResource);
            return; // a newer selection superseded the staged one
        }
        bool loaded = false;
        if (!FindPeerAppearanceResource(refresh.pendingResource, loaded)) {
            refresh.pendingResource = 0;
            LogMessage("[APPEARANCE] Native staged resource disappeared; keeping existing visual.\r\n");
            return;
        }
        if (!loaded || !NativeAppearanceQueueIdle()) {
            if (!refresh.timeoutLogged && now - refresh.pendingSince > 20000) {
                LogMessage("[APPEARANCE] Native streaming still busy; existing peer visual retained.\r\n");
                refresh.timeoutLogged = true;
            }
            return;
        }
        if (peerModel != model) {
            // Native Car::SetupRide rebuilds model table, SimEntity/collision,
            // visual, lamps and transforms while preserving the Car object.
            // +900 prevents it from overwriting LOCAL player/model globals.
            if (!CanRebuildPeerModel(car, ride)) {
                ReleasePeerAppearanceLease(refresh.pendingResource);
                LogMessage("[MODEL] Remote-slot guard failed; existing car retained.\r\n");
                return;
            }
            if(!RebuildLivePeerModel(car,ride,refresh.stagingRide)) {
                LogMessage("[MODEL] Scoped live model rebuild could not validate native mode/pose; refresh remains pending.\r\n");
                return;
            }
            gProxyRenderWheelAtMs = 0; // next fresh network tick restages wheel poses
        } else {
            // Preserve the already-tested same-model appearance path.
            reinterpret_cast<void(__thiscall*)(void*)>(0x0063A610)(reinterpret_cast<void*>(car));
            reinterpret_cast<void*(__thiscall*)(void*, const void*)>(0x00413440)(
                reinterpret_cast<void*>(ride), refresh.stagingRide);
            reinterpret_cast<void(__thiscall*)(void*, void*)>(0x0063A5A0)(
                reinterpret_cast<void*>(car), reinterpret_cast<void*>(ride));
        }
        ReleasePeerAppearanceLease(refresh.retainedResource);
        refresh.retainedResource = refresh.pendingResource;
        refresh.pendingResource = 0;
        refresh.baselineHash = appearance;
        LogMessage(peerModel != model
            ? "[MODEL] Peer model, native physics and appearance rebuilt after streaming.\r\n"
            : "[APPEARANCE] Same-model peer parts, paint and vinyls refreshed after native streaming.\r\n");
        return;
    }
    if (!refresh.baselineSet || appearance == refresh.baselineHash || !NativeAppearanceQueueIdle()) return;
    if(!CoopPeerTextureNative::enabled) return;
    if(!CoopPeerTextureNative::EnsureSlot()) return;
    auto* staged = refresh.stagingRide;
    reinterpret_cast<void*(__thiscall*)(void*)>(0x0040C910)(staged);
    reinterpret_cast<void*(__thiscall*)(void*, const void*)>(0x00413440)(staged, reinterpret_cast<void*>(ride));
    if (peerModel != model) {
        // Init the NEW model's native performance/part defaults, retaining only
        // locally constructed pointers, never another process's tuning caches.
        reinterpret_cast<void(__thiscall*)(void*, int, int, char, char)>(0x00610270)(
            staged, static_cast<int>(peerModel), 0, 0, 0);
    }
    // Retain every LOCAL tuning/cache pointer, including +3C0. The sender's
    // performance POD and process pointers are not appearance data.
    std::memset(staged + 0x590, 0, 170 * 4);
    reinterpret_cast<void(__thiscall*)(void*, int)>(0x00637040)(staged, 0);
    const uint8_t* src = peer.vehicleParam + 0x10;
    // Native CarRenderInfo copy (633A40) copies these exact appearance fields,
    // including the 170 installed/visibility bytes; all cache pointers stay local.
    std::memcpy(staged + 5, src + 5, 3);
    std::memcpy(staged + 0x580, src + 0x580, 2);
    std::memcpy(staged + 0x838, src + 0x838, 0xAA);
    int resolved = 0, missing = 0;
    auto resolve = reinterpret_cast<void*(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, void*, int)>(0x0061B980);
    for (unsigned slot = 0; slot < 170; ++slot) {
        uint32_t hash = 0, actual = 0;
        std::memcpy(&hash, src + 0x590 + slot * 4, sizeof(hash));
        if (!hash) continue;
        void* part = resolve(reinterpret_cast<void*>(0x008A2B68), peerModel, slot, hash, nullptr, -1);
        if (part && ReadGameMemory(reinterpret_cast<uintptr_t>(part), &actual, sizeof(actual)) && actual == hash) {
            std::memcpy(staged + 0x590 + slot * 4, &part, sizeof(part));
            ++resolved;
        } else ++missing;
    }
    reinterpret_cast<void(__thiscall*)(void*)>(0x0061BCD0)(staged);
    reinterpret_cast<void(__thiscall*)(void*, int)>(0x0061C280)(staged, TextureSlotForPlayerIndex(kPeerRenderPlayerIndex));
    // Existing resident resources own their own references. This request owns
    // exactly one additional lease, retained only until replaced or race exit.
    const uint32_t resource = reinterpret_cast<uint32_t(__thiscall*)(void*, void*, int, int)>(0x0063BC70)(
        reinterpret_cast<void*>(kNativeCarLoader), staged, 1, 1);
    if (!resource) {
        LogMessage("[APPEARANCE] Native streaming rejected the appearance request; old visual retained.\r\n");
        return;
    }
    if(!MarkOwnedPeerTextureResource(resource)) {
        uint32_t rejected=resource;ReleasePeerAppearanceLease(rejected);
        LogMessage("[APPEARANCE] Private texture allocation scope unavailable; request retired before pumping.\r\n");
        return;
    }
    refresh.pendingResource = resource;
    refresh.pendingCar = car;
    refresh.pendingRide = ride;
    refresh.pendingHash = appearance;
    refresh.pendingSince = now;
    refresh.timeoutLogged = false;
    // Native live traffic uses this null-callback pump. Poll its resource state
    // instead of taking ownership of the engine's singleton deferred callback.
    reinterpret_cast<void(__thiscall*)(void*, void*, void*)>(0x0063BCD0)(
        reinterpret_cast<void*>(kNativeCarLoader), nullptr, nullptr);
    char message[160];
    std::snprintf(message, sizeof(message), "[APPEARANCE] Native refresh queued: model=%lu->%lu resource=%lu parts=%d missing=%d.\r\n",
        static_cast<unsigned long>(model), static_cast<unsigned long>(peerModel),
        static_cast<unsigned long>(resource), resolved, missing);
    LogMessage(message);
}
void RetirePeerAppearanceOutsideWorld(DWORD flowState) {
    if(flowState==6 && IsCoopGameplayActive()) return;
    auto& refresh=gPeerAppearanceRefresh;
    ReleasePeerAppearanceLease(refresh.pendingResource);
    ReleasePeerAppearanceLease(refresh.retainedResource);
    refresh.baselineSet=false;refresh.baselineCar=0;gPeerModelPoseRestore={};
}

__declspec(naked) void TrampolineRaceLoadingLoop() {
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0xfffffff0
        push 0x006065B6
        ret
    }
}

void __cdecl OnRaceLoadingLoop() {
    if (!IsCoopGameplayActive()) {
        return;
    }
    if (*reinterpret_cast<uint32_t*>(0x0089E814) < 2) {
        *reinterpret_cast<uint32_t*>(0x0089E814) = 2;
    }
}

__declspec(naked) void HookedRaceLoadingLoop() {
    __asm {
        pushad
        call OnRaceLoadingLoop
        popad
        jmp TrampolineRaceLoadingLoop
    }
}

// ---------------------------------------------------------------------------
// Stage 4 CarLoader Native Streaming Hook (0x0057F673):
// In SPEED2.EXE, 0x0057F673 is the loop that registers each vehicle in CarLoader
// and streams its 3D geometry chunks (GEOMETRY.BIN, TEXTURES.BIN) into D3D memory.
// Instruction at 0x0057F673: mov eax, [0x0089E814] (5 bytes).
// By executing SetupCoopCar1BeforeRaceLoading right here, Car 1 is fully added to
// RaceParameters with count=2 BEFORE the streaming loop begins!
// The engine then natively streams BOTH Car 0 and Car 1 in the background!
// ---------------------------------------------------------------------------
__declspec(naked) void HookedStage4CarLoader() {
    __asm {
        pushad
        call SetupCoopCar1BeforeRaceLoading
        popad

        // Execute original instruction at 0x0057F673:
        // A1 14 E8 89 00 = mov eax, dword ptr [0x0089E814]
        mov eax, dword ptr ds:[0x0089E814]

        // Jump to 0x0057F678 (xor edi, edi)
        push 0x0057F678
        ret
    }
}

bool InstallStage4CarLoaderHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x0057F673);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedStage4CarLoader) -
        (0x0057F673 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), hook, 5);

    DWORD unused = 0;
    VirtualProtect(hook, 5, oldProtect, &unused);
    LogMessage("[HOOK] Stage 4 CarLoader hooked at 0x0057F673 (Co-op Car 1 geometry & textures natively streamed!)\r\n");
    return true;
}



// ---------------------------------------------------------------------------
// RenderMode Dummy Branch Patch (0x0060661B):
// By default, the game spawner sets Car 1's RenderMode to 0 (Invisible/Dummy) 
// if pCar[0x900] != 0. We patch this `je` to a `jmp` so the proxy car 
// always gets RenderMode 1 (Full 3D Geometry) and becomes visible.
void InstallRenderModePatch() {
    auto* hook = reinterpret_cast<BYTE*>(0x0060661B);
    DWORD oldProtect = 0;
    if (VirtualProtect(hook, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        hook[0] = 0xEB; // JMP
        FlushInstructionCache(GetCurrentProcess(), hook, 1);
        DWORD unused = 0;
        VirtualProtect(hook, 1, oldProtect, &unused);
        LogMessage("[HOOK] RenderMode branch patched at 0x0060661B (Proxy car body will be fully visible!)\r\n");
    }
}

// ---------------------------------------------------------------------------
// Table1D Spline Store Crash Guard Hook (0x005B5C70):
// In SPEED2.EXE, 0x005B5AB0 evaluates physics spline curves during loading.
// Instruction at 0x005B5C70: mov eax, [esi + 0x10]; fstp [eax + edi*4]
// We intercept this 6-byte sequence and redirect out-of-bounds/invalid writes
// to a dedicated 1024-float static buffer, preventing any 0xC0000005 crash!
// ---------------------------------------------------------------------------
static float gSafeTable1DBuffer[1024];

__declspec(naked) void HookedTable1DStore() {
    __asm {
        push ebx
        mov eax, dword ptr [esi + 0x10]
        test al, 3
        jnz SafeFallback
        cmp eax, 0x00400000
        jb SafeFallback
        cmp eax, 0x7FFFFFFF
        ja SafeFallback
        cmp edi, 512
        jae SafeFallback

        fstp dword ptr [eax + edi*4]
        pop ebx
        ret

    SafeFallback:
        mov ebx, edi
        and ebx, 511
        fstp dword ptr [gSafeTable1DBuffer + ebx*4]
        pop ebx
        ret
    }
}

bool InstallTable1DStoreHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x005B5C70);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedTable1DStore) -
        (0x005B5C70 + 5);
    hook[0] = 0xE8; // CALL
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 6);
    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    LogMessage("[HOOK] Table1D spline store crash guard hooked at 0x005B5C70 (Permanent physics crash fix)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// 3D VehicleVisual Render Diagnostics Hook (0x005B3390):
// In SPEED2.EXE, 0x005B3390 is VehicleVisual::RenderBody(void* this).
// It renders the 3D meshes, shaders, and geometry of the vehicle when in frustum.
// We intercept this call to profile rendering execution time (in microseconds)
// and track frustum visibility for Player and Co-op Proxy cars.
// ---------------------------------------------------------------------------
static BYTE sVehicleVisualRenderTrampoline[24] = {
    0x55,                               // push ebp
    0x8B, 0xEC,                         // mov ebp, esp
    0x83, 0xE4, 0xF0,                   // and esp, 0xfffffff0
    0x81, 0xEC, 0xB4, 0x01, 0x00, 0x00, // sub esp, 0x1b4
    0xE9, 0, 0, 0, 0                    // jmp 0x005B339C
};

void __fastcall HookedVehicleVisualRender(void* pThis, void* /*edx*/) {
    if (!pThis || !IsValidUserPtr(reinterpret_cast<uintptr_t>(pThis))) return;

    LARGE_INTEGER tStart{}, tEnd{};
    QueryPerformanceCounter(&tStart);

    using OrigFn = void(__thiscall*)(void*);
    auto pOrig = reinterpret_cast<OrigFn>(static_cast<void*>(sVehicleVisualRenderTrampoline));
    pOrig(pThis);

    QueryPerformanceCounter(&tEnd);
    if (gQpcFreq.QuadPart > 0) {
        uint32_t dur = static_cast<uint32_t>(
            (tEnd.QuadPart - tStart.QuadPart) * 1000000 / gQpcFreq.QuadPart);

        uintptr_t visual = reinterpret_cast<uintptr_t>(pThis);
        uintptr_t proxyVis = (gBoundProxyCar != 0) ? GetVehicleVisual(gBoundProxyCar) : 0;
        if (proxyVis != 0 && visual == proxyVis) {
            gProxyRenderDurationUs = dur;
            if (dur > gProxyRenderMaxUs) gProxyRenderMaxUs = dur;
            gProxyRenderedThisFrame = true;
            gProxyRenderFrameCount++;
            
        } else {
            gPlayerRenderDurationUs = dur;
            gPlayerRenderedThisFrame = true;
            gPlayerRenderFrameCount++;
        }
    }
}

bool InstallVehicleVisualRenderHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x005B3390);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 12, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    // Initialize trampoline jump back to 0x005B339C
    DWORD trampProtect = 0;
    VirtualProtect(sVehicleVisualRenderTrampoline, sizeof(sVehicleVisualRenderTrampoline), PAGE_EXECUTE_READWRITE, &trampProtect);
    const intptr_t trampRel = (0x005B3390 + 12) -
        (reinterpret_cast<intptr_t>(sVehicleVisualRenderTrampoline + 12) + 5);
    sVehicleVisualRenderTrampoline[12] = 0xE9;
    const int32_t jumpTarget = static_cast<int32_t>(trampRel);
    std::memcpy(sVehicleVisualRenderTrampoline + 13, &jumpTarget, sizeof(jumpTarget));
    DWORD unused = 0;
    VirtualProtect(sVehicleVisualRenderTrampoline, sizeof(sVehicleVisualRenderTrampoline), trampProtect, &unused);

    // Install hook at 0x005B3390
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedVehicleVisualRender) -
        (0x005B3390 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    std::memset(hook + 5, 0x90, 7); // NOP remaining 7 bytes
    FlushInstructionCache(GetCurrentProcess(), hook, 12);
    VirtualProtect(hook, 12, oldProtect, &unused);
    LogMessage("[HOOK] VehicleVisual::RenderBody hooked at 0x005B3390 (Render diagnostics active)\r\n");

    return true;
}

// ---------------------------------------------------------------------------
// Legacy wheel hook intentionally disabled after executable verification.
// 0x005ECD30 is used by force/traction diagnostics, not the wheel mesh renderer.
// ---------------------------------------------------------------------------
bool InstallWheelAngularPositionHook() {
    LogMessage("[INFO] Incorrect legacy wheel-force hook disabled; physics code preserved.\r\n");
    return false;
}

bool InstallFreeRoamCarSpawnHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x00601600);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedRaceParametersAddVehicle) -
        (0x00601600 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 6);

    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    LogMessage("[HOOK] RaceParameters::AddVehicle hooked at 0x00601600 (Universal Co-op Car Spawner active)\r\n");
    return true;
}

bool InstallRaceLoadingLoopHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x006065B0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedRaceLoadingLoop) -
        (0x006065B0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 6);

    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    LogMessage("[HOOK] RaceLoadingLoop hooked at 0x006065B0 (Dedicated Co-op Car 1 loader active, native AI racer branch preserved)\r\n");

    return true;
}

// ---------------------------------------------------------------------------
// Selective RideInfo Randomizer Protection (0x00639CE8):
// Preserve the finalized human/authority roster, not arbitrary array slots.
// Before Stage4, slot 1 still belongs to a native opponent. Unmanaged cars and
// singleplayer retain the original randomize argument and native setup.
// Hooked at 0x00639CE8 inside RideInfo::Setup (0x00639CE0) - 9 bytes.
// For co-op cars, jumps directly to 0x00639CF9 (skipping RideInfo::Randomize entirely).
// ---------------------------------------------------------------------------
static uintptr_t gCoopRideParameterArrayAddress = 0x0089E810;
#ifdef COOP_RIDE_SETUP_TEST_FIXTURE
static uintptr_t gRideSetupFixtureRandomize = 0x006399D0;
static uintptr_t gRideSetupFixtureResume = 0x00639CF9;
#endif
static bool ShouldPreserveCoopRideSetup(uintptr_t ride) {
    if (!IsCoopGameplayActive() || !IsValidUserPtr(ride) || ride < 0x10) return false;
    uintptr_t array = 0;
    if (!ReadGameMemory(gCoopRideParameterArrayAddress, &array, sizeof(array)) || !IsValidUserPtr(array)) return false;
    const uintptr_t parameter = ride - 0x10;
    if (parameter == array || parameter == array + 0x940) {
        uint32_t type = 0; uint8_t index = 0, remote = 0;
        if (ReadGameMemory(parameter + 4, &type, 4) && type == 1 &&
            ReadGameMemory(parameter + 1, &index, 1) &&
            ReadGameMemory(parameter + 0x900, &remote, 1) &&
            index == (parameter == array ? 0 : kPeerRenderPlayerIndex) &&
            remote == (parameter == array ? 0 : 1)) return true;
    }
    uint32_t ordinal = 0, model = 0;
    if (gCoopFleetEnabled && ShouldPreserveCoopFleetRideSetup(ride)) return true;
    return gCoopAIEnabled && ReadCoopAIRosterParam(parameter, ordinal, model);
}

__declspec(naked) void HookedRideInfoRandomizeCheck() {
    __asm {
        pushad
        push esi
        call ShouldPreserveCoopRideSetup
        add esp, 4
        movzx eax, al
        mov [esp + 0x1c], eax
        popad
        test eax, eax
        jnz skip_randomize

    check_car_table:
        // Preserve the caller's native argument, including false in SP.
        mov edx, dword ptr [esp + 8]
        xor eax, eax
        test edx, edx
        setne al
        push 0
        push eax
        mov ecx, esi
#ifdef COOP_RIDE_SETUP_TEST_FIXTURE
        mov edx, dword ptr [gRideSetupFixtureRandomize]
#else
        mov edx, 0x006399D0
#endif
        call edx

    skip_randomize:
#ifdef COOP_RIDE_SETUP_TEST_FIXTURE
        mov edx, dword ptr [gRideSetupFixtureResume]
#else
        mov edx, 0x00639CF9
#endif
        jmp edx
    }
}

bool InstallRideInfoRandomizerPatch() {
    // Leave the vanilla global paint-randomization flag untouched.
    // 2. Install hook at 0x00639CE8 to protect co-op cars from AI racer randomization
    auto* hook1 = reinterpret_cast<BYTE*>(0x00639CE8);
    DWORD oldProt1 = 0;
    if (!VirtualProtect(hook1, 9, PAGE_EXECUTE_READWRITE, &oldProt1)) {
        return false;
    }
    const intptr_t rel1 = reinterpret_cast<intptr_t>(&HookedRideInfoRandomizeCheck) -
        (0x00639CE8 + 5);
    hook1[0] = 0xE9; // JMP
    const int32_t jmp1 = static_cast<int32_t>(rel1);
    std::memcpy(hook1 + 1, &jmp1, sizeof(jmp1));
    std::memset(hook1 + 5, 0x90, 4); // 4 NOPs
    FlushInstructionCache(GetCurrentProcess(), hook1, 9);
    DWORD unused = 0;
    VirtualProtect(hook1, 9, oldProt1, &unused);

    LogMessage("[HOOK] RideInfo setup protected by finalized roster identity; unmanaged/SP arguments preserved.\r\n");
    return true;
}

__declspec(naked) void HookedAiDriverUpdate() {
    __asm {
        // Replicas are identified by current manifest and native ownership,
        // never by array slot or by disabling all AI in singleplayer/host.
        pushad
        push ecx
        call ShouldSuppressCoopAIController
        add esp, 4
        movzx eax, al
        mov [esp + 0x1c], eax
        popad
        test eax, eax
        jnz null_aidriver
        // ecx is AIDriver*
        test ecx, ecx
        jz null_aidriver
        cmp dword ptr [gBoundProxyCar], 0
        je regular_ai
        mov eax, dword ptr [ecx + 4]
        cmp eax, dword ptr [gBoundProxyCar]
        jne regular_ai
        // If it's our co-op proxy car, do not let AI driver run!
        // Do not write guessed driver-layout fields or discard owned state.
        // Native teardown owns follow-state unlinking and destruction.
        ret
    null_aidriver:
        ret
    regular_ai:
        push ecx
        push esi
        mov esi, ecx
        mov eax, dword ptr [esi + 4]
        push 0x0040D557
        ret
    }
}

bool InstallAiDriverHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x0040D550);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedAiDriverUpdate) -
        (0x0040D550 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    hook[6] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 7);

    DWORD unused = 0;
    VirtualProtect(hook, 7, oldProtect, &unused);
    LogMessage("[HOOK] AIDriver::Update hooked at 0x0040D550 (proxy AI silenced)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Free Roam AI Opponent Proximity Scanner Guard (0x0042A880)
// Prevents singleplayer Outrun challenge engagement from targeting co-op player car
// ---------------------------------------------------------------------------
__declspec(naked) void SafeWorldOpponentScanHook() {
    __asm {
        mov edi, dword ptr [esi + 8]
        test edi, edi
        je skip_opponent
        cmp dword ptr [gBoundProxyCar], 0
        je normal_opponent
        // If this opponent car is our dedicated co-op proxy car, skip it!
        cmp edi, dword ptr [gBoundProxyCar]
        je skip_opponent
    normal_opponent:
        // Normal roaming AI racer: proceed to call 0x005ED430
        push 0x0042A88B
        ret
    skip_opponent:
        push 0x0042A925
        ret
    }
}

bool InstallWorldOpponentScanHook() {
    if (CoopFleetScannerNativeReady()) return true;
    auto* hook = reinterpret_cast<BYTE*>(0x0042A880);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 11, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&SafeWorldOpponentScanHook) -
        (0x0042A880 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    for (int i = 5; i < 11; ++i) {
        hook[i] = 0x90; // NOP
    }
    FlushInstructionCache(GetCurrentProcess(), hook, 11);

    DWORD unused = 0;
    VirtualProtect(hook, 11, oldProtect, &unused);
    LogMessage("[HOOK] World opponent scanner guarded at 0x0042A880 (co-op car shielded from Outrun AI engagement)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Part Animation Pointer Guard:
// Prevents 0xC0000005 crash at 0x0061BD13 when RideInfo part cache pointer in
// eax is NULL or invalid (< 0x00400000) during car parts resolution.
// ---------------------------------------------------------------------------
__declspec(naked) void SafePartAnimationPointerHook() {
    __asm {
        mov dword ptr [esp + 0x10], eax
        cmp eax, 0x00400000
        jb skip_entry
        cmp eax, 0x7FFFFFFF
        ja skip_entry
        push 0x0061BD13
        ret

    skip_entry:
        push 0x0061BDFA
        ret
    }
}

bool InstallPartAnimationCrashShield() {
    auto* hook = reinterpret_cast<BYTE*>(0x0061BD07);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 12, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&SafePartAnimationPointerHook) -
        (0x0061BD07 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    std::memset(hook + 5, 0x90, 7); // 7 NOPs
    FlushInstructionCache(GetCurrentProcess(), hook, 12);
    DWORD unused = 0;
    VirtualProtect(hook, 12, oldProtect, &unused);
    LogMessage("[HOOK] Part animation pointer shield installed at 0x0061BD07 (0x0061BD13 crash shielded)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// bStringHash Crash Shield (0x0043DB80):
// In SPEED2.EXE, 0x0043DB80 is bStringHash(const char* str, uint32_t seed).
// At 0x0043DB84, it executes `mov cl, byte ptr [edx]`. If edx is an invalid pointer,
// it triggers a fatal 0xC0000005 crash.
// The caller at 0x006338FB tests eax: if bStringHash returns 0, the engine skips the part!
// We hook 0x0043DB80 with a 5-byte JMP + 1 NOP, returning 0 on any invalid pointer or exception.
// ---------------------------------------------------------------------------
uint32_t __cdecl SafeStringHash(const char* str, uint32_t seed) {
    __try {
        if (!str || reinterpret_cast<uintptr_t>(str) < 0x00010000 || reinterpret_cast<uintptr_t>(str) >= 0x7FFFFFFF) {
            return 0;
        }
        uint32_t hash = seed;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(str);
        while (*p) {
            hash = hash * 33 + *p;
            p++;
        }
        return hash;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool InstallStringHashCrashShield() {
    auto* hook = reinterpret_cast<BYTE*>(0x0043DB80);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&SafeStringHash) - (0x0043DB80 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // 1 NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 6);
    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    LogMessage("[HOOK] SafeStringHash crash shield installed at 0x0043DB80 (0x0043DB84 crash shielded)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 11: Native BeginGameFlowSwitchTrack Hook (0x005819F0)
// 0x005819F0 is the true GameFlow track-switch entry point in SPEED2.EXE.
// When a player accepts a race from Free Roam (flowState == 6), the native game
// calls 0x005819F0 on the GameFlow singleton (0x00865480).
// In Co-op, either human can initiate consent, and the old native world is
// retained until approval. Pause-menu Restart has its own message9 hook below.
// ---------------------------------------------------------------------------
alignas(16) static BYTE gTrampolineBeginGameFlowSwitchTrack[32] = {};

bool __cdecl HandleBeginGameFlowSwitchTrack(void* This) {
    return HoldNativeCoopRaceIntent(This);
}

__declspec(naked) void HookedBeginGameFlowSwitchTrackNaked() {
    __asm {
        pushad
        push ecx
        call HandleBeginGameFlowSwitchTrack
        add esp, 4
        movzx eax, al                    // x86 bool only defines AL.
        mov [esp + 0x1c], eax
        popad
        test eax, eax
        jnz consent_pending

        // Execute original trampoline: ecx must be This!
        lea eax, gTrampolineBeginGameFlowSwitchTrack
        jmp eax
    consent_pending:
        ret
    }
}

bool InstallRaceTriggerHook() {
    auto* target = reinterpret_cast<BYTE*>(0x005819F0);
    const BYTE expected[]={0x8B,0x51,0x04,0x33,0xC0};
    if(std::memcmp(target,expected,sizeof(expected))!=0) {
        LogMessage("[ERROR] Native track-switch entry differs; race intent hook not installed.\r\n");
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    // Build trampoline for 0x005819F0 (8B 51 04 33 C0 = 5 bytes)
    DWORD trampProtect = 0;
    VirtualProtect(gTrampolineBeginGameFlowSwitchTrack, sizeof(gTrampolineBeginGameFlowSwitchTrack), PAGE_EXECUTE_READWRITE, &trampProtect);
    std::memcpy(gTrampolineBeginGameFlowSwitchTrack, target, 5);
    gTrampolineBeginGameFlowSwitchTrack[5] = 0xE9; // JMP
    const intptr_t jmpBack = static_cast<intptr_t>(0x005819F5) - (reinterpret_cast<intptr_t>(gTrampolineBeginGameFlowSwitchTrack + 5) + 5);
    const int32_t jumpBackOffset = static_cast<int32_t>(jmpBack);
    std::memcpy(gTrampolineBeginGameFlowSwitchTrack + 6, &jumpBackOffset, sizeof(jumpBackOffset));
    FlushInstructionCache(GetCurrentProcess(), gTrampolineBeginGameFlowSwitchTrack, sizeof(gTrampolineBeginGameFlowSwitchTrack));

    // Install JMP to HookedBeginGameFlowSwitchTrackNaked at 0x005819F0
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedBeginGameFlowSwitchTrackNaked) - (0x005819F0 + 5);
    target[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(target + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    DWORD unused = 0;
    VirtualProtect(target, 5, oldProtect, &unused);

    LogMessage("[HOOK] Native BeginGameFlowSwitchTrack hooked at 0x005819F0 (synchronized race trigger active).\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Native pause-menu Restart is a separate void __thiscall entry. Its message9
// path must obtain consent BEFORE resetting the profile/event or native RF0.
alignas(16) static BYTE gTrampolineNativeRaceRestart[32] = {};
bool __cdecl HandleNativeRaceRestart(void* This) {
    return HoldNativeCoopRaceRestart(This);
}
__declspec(naked) void HookedNativeRaceRestartNaked() {
    __asm {
        pushad
        push ecx
        call HandleNativeRaceRestart
        add esp, 4
        movzx eax, al
        mov [esp + 0x1c], eax
        popad
        test eax, eax
        jnz consent_pending
        lea eax, gTrampolineNativeRaceRestart
        jmp eax
    consent_pending:
        ret
    }
}
bool InstallNativeRaceRestartHook() {
    auto* target=reinterpret_cast<BYTE*>(0x00603DE0);
    // Whole instructions: push esi; mov esi,ecx; mov ecx,83A9D0.
    const BYTE expected[]={0x56,0x8B,0xF1,0xB9,0xD0,0xA9,0x83,0x00};
    if(std::memcmp(target,expected,sizeof(expected))!=0) {
        LogMessage("[ERROR] Native Restart entry differs; restart consent hook not installed.\r\n");return false;
    }
    DWORD trampolineProtect=0,targetProtect=0;
    if(!VirtualProtect(gTrampolineNativeRaceRestart,sizeof(gTrampolineNativeRaceRestart),PAGE_EXECUTE_READWRITE,&trampolineProtect)) return false;
    std::memcpy(gTrampolineNativeRaceRestart,expected,sizeof(expected));
    gTrampolineNativeRaceRestart[8]=0xE9;
    const int32_t back=static_cast<int32_t>(reinterpret_cast<intptr_t>(target+8)-
        (reinterpret_cast<intptr_t>(gTrampolineNativeRaceRestart+8)+5));
    std::memcpy(gTrampolineNativeRaceRestart+9,&back,4);
    FlushInstructionCache(GetCurrentProcess(),gTrampolineNativeRaceRestart,sizeof(gTrampolineNativeRaceRestart));
    if(!VirtualProtect(target,sizeof(expected),PAGE_EXECUTE_READWRITE,&targetProtect)) return false;
    target[0]=0xE9;
    const int32_t jump=static_cast<int32_t>(reinterpret_cast<intptr_t>(&HookedNativeRaceRestartNaked)-
        (reinterpret_cast<intptr_t>(target)+5));
    std::memcpy(target+1,&jump,4);std::memset(target+5,0x90,3);
    FlushInstructionCache(GetCurrentProcess(),target,sizeof(expected));
    DWORD ignored=0;VirtualProtect(target,sizeof(expected),targetProtect,&ignored);
    LogMessage("[HOOK] Native Restart/message9 hooked at 0x00603DE0 before profile reset/world teardown.\r\n");
    return true;
}

// Native GetCountdownState Trampoline & Synchronized Hook (0x005EEA40)
// Original 6 bytes:
//   51             push ecx
//   8B 51 04       mov edx, dword ptr [ecx + 4]
//   31 C0          xor eax, eax
// ---------------------------------------------------------------------------
// RaceFlow Crash Shield at 0x0060B49F
// Original 11 bytes:
//   8B 15 18 01 89 00    mov edx, dword ptr [0x890118]
//   83 3A 04             cmp dword ptr [edx], 4
//   74 32                je 0x0060B4DC
// If edx (0x890118) is NULL during track switches/transitions, cmp [edx], 4
// causes a null pointer dereference crash (0xC0000005).
// This hook intercepts the check and safely jumps to 0x0060B4DC if edx is NULL.
// ---------------------------------------------------------------------------
__declspec(naked) void RaceFlowCrashShield_Naked() {
    __asm {
        mov edx, dword ptr ds:[0x00890118]
        test edx, edx
        jz skip_active_race_check
        cmp dword ptr [edx], 4
        je skip_active_race_check

        // edx != 0 && *edx != 4: jump to 0x0060B4AA
        push 0x0060B4AA
        ret

    skip_active_race_check:
        // edx == 0 || *edx == 4: jump to 0x0060B4DC
        push 0x0060B4DC
        ret
    }
}

bool InstallRaceFlowCrashShield() {
    auto* target = reinterpret_cast<BYTE*>(0x0060B49F);
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 11, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect RaceFlow at 0x0060B49F.\r\n");
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&RaceFlowCrashShield_Naked) - (0x0060B49F + 5);
    target[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(target + 1, &jump, sizeof(jump));
    for (int i = 5; i < 11; ++i) {
        target[i] = 0x90; // NOP remaining 6 bytes
    }
    FlushInstructionCache(GetCurrentProcess(), target, 11);
    DWORD unused = 0;
    VirtualProtect(target, 11, oldProtect, &unused);

    LogMessage("[HOOK] RaceFlow crash shield installed at 0x0060B49F (null-safe Race pointer).\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// RaceFlow Handler 4 (RF_STATE_RACE) Safety Guard at 0x0080215C
// Table 0x0080214C dispatches RaceFlow messages. Slot [4] is Handler 4 (0x0060B3A0).
// In native game, Handler 4 assumes RaceCoordinator (0x00890118) is allocated.
// If a tick (msgType 3) arrives while 0x00890118 is NULL (e.g. during track transition),
// this guard safely resets state to 0 and returns 1, completely preventing crashes!
// ---------------------------------------------------------------------------
typedef int(__thiscall* RaceFlowHandlerFn)(void* This, int msgType, int param);
static RaceFlowHandlerFn gOriginalRaceFlowHandler4 = reinterpret_cast<RaceFlowHandlerFn>(0x0060B3A0);

int __stdcall SafeRaceFlowHandler4_Cpp(void* This, int msgType, int param) {
    if (msgType == 3) { // Tick message
        uintptr_t pRace = 0;
        if (!ReadGameMemory(0x00890118, &pRace, sizeof(pRace)) || !IsValidUserPtr(pRace)) {
            if (This && IsValidUserPtr(reinterpret_cast<uintptr_t>(This))) {
                *reinterpret_cast<uint32_t*>(This) = 0; // Reset RaceFlow to State 0 (Idle/Loading)
            }
            return 1;
        }
    }
    return gOriginalRaceFlowHandler4(This, msgType, param);
}

__declspec(naked) void SafeRaceFlowHandler4_Naked() {
    __asm {
        push dword ptr [esp + 8] // push param
        push dword ptr [esp + 8] // push msgType
        push ecx                 // push This
        call SafeRaceFlowHandler4_Cpp
        ret 8
    }
}

bool InstallRaceFlowHandlerGuard() {
    auto* pHandler4Slot = reinterpret_cast<void**>(0x0080215C);
    DWORD oldProtect = 0;
    if (!VirtualProtect(pHandler4Slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect RaceFlow Handler 4 slot at 0x0080215C.\r\n");
        return false;
    }
    *pHandler4Slot = reinterpret_cast<void*>(&SafeRaceFlowHandler4_Naked);
    DWORD unused = 0;
    VirtualProtect(pHandler4Slot, sizeof(void*), oldProtect, &unused);
    LogMessage("[HOOK] RaceFlow Handler 4 guard installed at 0x0080215C (crash-immune).\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Native RaceCoordinator::CompareRacers Hook (0x005EEC00)
// Stolen 7 bytes: 83 3D B0 E7 89 00 07 (cmp dword ptr [0x89e7b0], 7)
// Fixes ranking inversion where physically leading car was assigned 2nd / 2.
// Returns 1 if racerB is ahead of racerA (SWAP), 0 if racerA is ahead or tied (NO SWAP).
// ---------------------------------------------------------------------------
static uint8_t gTrampolineCompareRacers[16]{};

using NativeCompareRacersFn=uint8_t(__thiscall*)(void*,uintptr_t,uintptr_t);
static NativeCompareRacersFn gNativeCompareRacers=
    reinterpret_cast<NativeCompareRacersFn>(&gTrampolineCompareRacers[0]);

uintptr_t CoopRaceCar(bool peer) {
    uintptr_t root=0,local=0,cars[2]{};
    if(!ReadGameMemory(gRaceProgressAddresses.localRoot,&root,sizeof(root)) ||
        !IsValidUserPtr(root) || !ReadGameMemory(root+4,&local,sizeof(local)) ||
        !IsValidUserPtr(local) ||
        !ReadGameMemory(gRaceProgressAddresses.carArray,cars,sizeof(cars)) ||
        !IsValidUserPtr(cars[0]) || !IsValidUserPtr(cars[1]) || cars[0]==cars[1]) return 0;
    // Slot order can differ between processes and loads. Never replace the
    // actual local Player's progress with a remote packet, even after sorting.
    if(local==cars[0]) return peer?cars[1]:local;
    if(local==cars[1]) return peer?cars[0]:local;
    return 0; // Teardown/unbound identity: no guessing or traffic takeover.
}

bool SyncNativePeerRaceProgress(uintptr_t car,const PositionPacket& packet) {
    if(!CoopFinishRaceActive() || packet.flowState!=6 || !IsValidUserPtr(car)) return false;
    uintptr_t racer=0,owner=0;
    if(car!=CoopRaceCar(true) ||
        !ReadGameMemory(car+0x1C,&racer,sizeof(racer)) || !IsValidUserPtr(racer) ||
        !CoopFinishIsPeerRacer(racer) ||
        !ReadGameMemory(racer,&owner,sizeof(owner)) || owner!=car) return false;
    float length=0;
    if(!ReadGameMemory(gRaceProgressAddresses.routeLength,&length,sizeof(length)) ||
        !std::isfinite(length) || length<=100 || length>1000000 ||
        !std::isfinite(packet.splineDistance) || packet.splineDistance<0 ||
        packet.splineDistance>length+1.0f ||
        !std::isfinite(packet.totalProgress) || packet.totalProgress < -1.1f ||
        packet.totalProgress>22.1f ||
        static_cast<int8_t>(packet.currentLap)<-1 || static_cast<int8_t>(packet.currentLap)>22 ||
        static_cast<int8_t>(packet.completedLaps)<-1 || static_cast<int8_t>(packet.completedLaps)>22) return false;
    const float fraction=std::clamp(packet.splineDistance/length,0.0f,1.0f);
    // Car's embedded route locator: +8FC meters, +8F8 fraction. The native
    // racer update multiplies the latter by 100; meters here corrupt progress.
    // VehicleParam +8F8 is a DIFFERENT structure's streaming handle.
    return SafeWriteBytes(car+0x8FC,&packet.splineDistance,4) &&
        SafeWriteBytes(car+0x8F8,&fraction,4) &&
        SafeWriteBytes(racer+8,&packet.currentLap,1) &&
        SafeWriteBytes(racer+9,&packet.completedLaps,1) &&
        SafeWriteBytes(racer+0x14,&packet.totalProgress,4) &&
        SafeWriteBytes(racer+0x20,&packet.totalProgress,4);
}

void CoopSyncRaceProgressBeforeSort() {
    if(gCoopAIEnabled) SyncCoopAIRaceProgress();
    if(CoopFinishRaceActive()) {
        RemotePlayerState peer{};
        if(GetRemotePlayerState(peer)) {
            SyncNativePeerRaceProgress(CoopRaceCar(true),peer.current);
        }
    }
}

uint8_t __cdecl HookedCompareRacers_Cpp(void* coordinator,uintptr_t a,uintptr_t b) {
    CoopSyncRaceProgressBeforeSort();
    // Native +20 is already lap-aware and understands the event's finish
    // semantics. Do not unsigned-compare lap -1 or shortest-wrap meter gaps.
    return gNativeCompareRacers(coordinator,a,b);
}

__declspec(naked) void HookedCompareRacers_Naked() {
    __asm {
        push dword ptr [esp + 8] // push racerB (arg2)
        push dword ptr [esp + 8] // push racerA (arg1)
        push ecx                 // push pRaceCoordinator (this)
        call HookedCompareRacers_Cpp
        add esp, 12
        ret 8                    // __thiscall: callee cleans 8 bytes (arg1, arg2)
    }
}

bool InstallCompareRacersHook() {
    auto* target = reinterpret_cast<BYTE*>(0x005EEC00);
    const BYTE expected[]{0x83,0x3D,0xB0,0xE7,0x89,0x00,0x07};
    if(std::memcmp(target,expected,sizeof(expected))!=0) {
        LogMessage("[HOOK] Native race progress comparator refused: signature differs.\r\n");
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect CompareRacers at 0x005EEC00.\r\n");
        return false;
    }

    // Build trampoline (stolen 7 bytes: 83 3D B0 E7 89 00 07 -> cmp dword ptr [0x89e7b0], 7)
    DWORD trampProtect = 0;
    VirtualProtect(gTrampolineCompareRacers, sizeof(gTrampolineCompareRacers), PAGE_EXECUTE_READWRITE, &trampProtect);
    std::memcpy(gTrampolineCompareRacers, target, 7);
    gTrampolineCompareRacers[7] = 0xE9; // JMP back to 0x005EEC07
    const intptr_t jmpBack = static_cast<intptr_t>(0x005EEC07) - (reinterpret_cast<intptr_t>(gTrampolineCompareRacers + 7) + 5);
    const int32_t jumpBackOffset = static_cast<int32_t>(jmpBack);
    std::memcpy(gTrampolineCompareRacers + 8, &jumpBackOffset, sizeof(jumpBackOffset));
    FlushInstructionCache(GetCurrentProcess(), gTrampolineCompareRacers, sizeof(gTrampolineCompareRacers));

    // Install JMP to HookedCompareRacers_Naked at 0x005EEC00
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedCompareRacers_Naked) - (0x005EEC00 + 5);
    target[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(target + 1, &jump, sizeof(jump));
    target[5] = 0x90; // NOP
    target[6] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), target, 7);
    DWORD unused = 0;
    VirtualProtect(target, 7, oldProtect, &unused);

    LogMessage("[HOOK] Native lap-aware comparator retained; peer progress synchronized before sorting.\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Native GetCountdownState Trampoline & Synchronized Hook (0x005EEA40)
// Original 6 bytes:
//   51             push ecx
//   8B 51 04       mov edx, dword ptr [ecx + 4]
//   33 C0          xor eax, eax
// ---------------------------------------------------------------------------
static uint8_t gTrampolineGetCountdownState[16]{};
using NativeGetCountdownFn = int(__thiscall*)(void*);
static NativeGetCountdownFn gNativeGetCountdownState =
    reinterpret_cast<NativeGetCountdownFn>(&gTrampolineGetCountdownState[0]);

// The reliable race controller maps the host barrier into this PC's clock.
void CoopRaceGridReady();
bool CoopRaceCountdownDeadline(uint32_t& localDeadlineMs);
void CoopRaceNativeCountdownReady();
bool CoopRaceHudCountdownDeadline(uint32_t& localDeadlineMs);
void CoopRaceCountdownStarted();
static DWORD (WINAPI *gCountdownNow)()=&GetTickCount;
static uintptr_t gCountdownSimTickAddress=0x0086518C;

static bool CoopCountdownDeadlineReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0; // GetTickCount rollover safe.
}

static bool ReadCoopCountdownRaceContext(uintptr_t& race) {
    if (!IsCoopGameplayActive() ||
        (gCoopRaceState != COOP_RACE_STARTING && gCoopRaceState != COOP_RACE_IN_PROGRESS) ||
        !gCar1LoadedForRace) return false;
    DWORD flow = 0;
    uint32_t track = 0;
    return ReadGameMemory(gRaceContextAddresses.flow, &flow, sizeof(flow)) && flow == 6 &&
        ReadGameMemory(gRaceContextAddresses.track, &track, sizeof(track)) && track != 0 && track != 4000 &&
        track == gPendingRaceEvent.trackId &&
        ReadGameMemory(gRaceContextAddresses.race, &race, sizeof(race)) && IsValidUserPtr(race);
}

static bool ReadVerifiedCountdownControls(uintptr_t car, uintptr_t& controls,
    uintptr_t expectedVtable = 0x00784FC8) {
    uintptr_t vtable = 0, setter = 0, owner = 0;
    return IsValidUserPtr(car) &&
        ReadGameMemory(car + 0x30, &controls, sizeof(controls)) && IsValidUserPtr(controls) &&
        ReadGameMemory(controls, &vtable, sizeof(vtable)) && vtable == expectedVtable &&
        ReadGameMemory(vtable + 0x1C, &setter, sizeof(setter)) && setter == 0x00410430 &&
        ReadGameMemory(controls + 0x2A8, &owner, sizeof(owner)) && owner == car;
}

static bool (*gCountdownVehicleReader)(LocalVehicleState&)=&ReadPlayerVehicleState;
static bool VerifiedControlsSetCountdownLocked(bool locked) {
    LocalVehicleState local{};
    uintptr_t controls = 0;
    if (!gCountdownVehicleReader(local) ||
        !ReadVerifiedCountdownControls(local.playerInstance, controls)) return false;
    uint8_t current = 0;
    if (!ReadGameMemory(controls + 0x2B4, &current, sizeof(current)) || current > 1) return false;
    if ((current != 0) == locked) return true;
    // Native type-1 Controls virtual +0x1C: __thiscall(bool), RET 4. This sets
    // +0x2B4; Controls::Tick then applies 100% brake (+0x214), preserving revs.
    // Avoid repeating false: the native setter also resets the starting gear.
    using SetCountdownLockedFn = void(__thiscall*)(void*, bool);
    reinterpret_cast<SetCountdownLockedFn>(0x00410430)(reinterpret_cast<void*>(controls), locked);
    return true;
}

static bool SetNativeCountdownOrigin(uintptr_t race, uint32_t tick) {
    // +4 is GO time, NOT the countdown origin. +8 is the 3/2/1 start time.
    const uint32_t unsetGo = 0x7FFFFFFF;
    if(!tick || tick==unsetGo)--tick; // Native unset sentinels are not valid origins.
    return SafeWriteBytes(race + 4, &unsetGo, sizeof(unsetGo)) &&
        SafeWriteBytes(race + 8, &tick, sizeof(tick));
}

// RF2's msg3 and msg6 both enter RF3, whose ENTER starts the NPC starter NIS.
// Hold before that transition, so the entire native scene starts at the shared
// barrier instead of attempting to repair only the later HUD countdown.
static RaceFlowHandlerFn gOriginalRaceFlowHandler2 = reinterpret_cast<RaceFlowHandlerFn>(0x00603AF0);

int __stdcall CoopRaceIntroBarrier_Cpp(void* This, int msgType, int param) {
    uintptr_t race = 0, nativeFlow = 0;
    uint32_t flowState = 0;
    if ((msgType == 3 || msgType == 6) && ReadCoopCountdownRaceContext(race) &&
        ReadGameMemory(gRaceContextAddresses.nativeFlow, &nativeFlow, sizeof(nativeFlow)) &&
        nativeFlow == reinterpret_cast<uintptr_t>(This) &&
        ReadGameMemory(nativeFlow, &flowState, sizeof(flowState)) && flowState == 2) {
        CoopRaceGridReady();
        uint32_t deadline = 0;
        if (!CoopRaceCountdownDeadline(deadline) ||
            !CoopCountdownDeadlineReached(gCountdownNow(), deadline)) {
            VerifiedControlsSetCountdownLocked(true);
            return 1;
        }
    }
    return gOriginalRaceFlowHandler2(This, msgType, param);
}

__declspec(naked) void CoopRaceIntroBarrier_Naked() {
    __asm {
        push dword ptr [esp + 8] // param
        push dword ptr [esp + 8] // msgType
        push ecx                // This
        call CoopRaceIntroBarrier_Cpp
        ret 8
    }
}

bool InstallCoopRaceIntroBarrier() {
    const uintptr_t slot = 0x00802154;
    uintptr_t current = 0;
    if (!ReadGameMemory(slot, &current, sizeof(current))) return false;
    if (current == reinterpret_cast<uintptr_t>(&CoopRaceIntroBarrier_Naked)) return true;
    const uint8_t nativePrefix[]{0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x08};
    uint8_t prefix[sizeof(nativePrefix)]{};
    if (current != 0x00603AF0 ||
        !ReadGameMemory(current, prefix, sizeof(prefix)) ||
        std::memcmp(prefix, nativePrefix, sizeof(prefix)) != 0) {
        LogMessage("[HOOK] Race starter barrier refused: native RF2 signature/slot differs.\r\n");
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
    const uintptr_t replacement = reinterpret_cast<uintptr_t>(&CoopRaceIntroBarrier_Naked);
    const bool installed = SafeWriteBytes(slot, &replacement, sizeof(replacement));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), oldProtect, &unused);
    if (installed) LogMessage("[HOOK] Native race starter barrier installed (RF2 before NPC NIS).\r\n");
    return installed;
}

int __stdcall HookedGetCountdownState_Cpp(void* pRace) {
    // SP/free roam and every non-current coordinator use the exact native call.
    uintptr_t race = 0;
    uint32_t nativeState = 0;
    if (!ReadCoopCountdownRaceContext(race) || race != reinterpret_cast<uintptr_t>(pRace) ||
        !ReadGameMemory(race, &nativeState, sizeof(nativeState)) || nativeState != 3)
        return gNativeGetCountdownState(pRace);

    // Some race types skip the NPC starter path. A real native countdown is
    // also valid grid readiness, but it still needs BOTH negotiated barriers.
    CoopRaceGridReady();
    CoopRaceNativeCountdownReady();
    uint32_t deadline = 0, simTick = 0;
    if (!ReadGameMemory(gCountdownSimTickAddress, &simTick, sizeof(simTick))) return gNativeGetCountdownState(pRace);
    VerifiedControlsSetCountdownLocked(true);
    const uint32_t now=gCountdownNow();
    if (!CoopRaceHudCountdownDeadline(deadline) ||
        !CoopCountdownDeadlineReached(now, deadline)) {
        // Keep native state 3 and its HUD alpha coherent. Returning 0 would
        // mean "countdown over"; setting +4 to now would instead fabricate GO.
        SetNativeCountdownOrigin(race, simTick);
        return gNativeGetCountdownState(pRace);
    }
    // The native simulation counter uses quarter milliseconds (784268=1/4000).
    // Project the same committed wall-clock origin on EVERY query, so different
    // starter lengths, late callbacks and different FPS cannot add local delay.
    if(!SetNativeCountdownOrigin(race,coop_race::CountdownOrigin(simTick,now,deadline)))return gNativeGetCountdownState(pRace);
    if (!gCountdownActive.load()) {
        gCountdownActive.store(true);
        CoopRaceCountdownStarted();
        LogMessage("[RACE] Native 3-2-1 countdown started after synchronized starter barrier.\r\n");
    }
    const int state = gNativeGetCountdownState(pRace);
    if (state == 4) {
        // Native Tick observes this same GO result and calls SetRaceState(4),
        // which starts race/lap clocks. Never start those clocks at READY.
        VerifiedControlsSetCountdownLocked(false);
        gCoopRaceState = COOP_RACE_IN_PROGRESS;
    }
    return state;
}

__declspec(naked) void HookedGetCountdownState_Naked() {
    __asm {
        push ecx                         // push this (pRace)
        call HookedGetCountdownState_Cpp // returns result in eax
        ret
    }
}

bool InstallCountdownSyncHook() {
    auto* target = reinterpret_cast<BYTE*>(0x005EEA40);
    const BYTE expected[]{0x51,0x8B,0x51,0x04,0x33,0xC0};
    if (std::memcmp(target,expected,sizeof(expected)) != 0) {
        LogMessage("[HOOK] Countdown sync refused: native signature differs.\r\n");
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect GetCountdownState at 0x005EEA40.\r\n");
        return false;
    }

    // Build trampoline (stolen 6 bytes: 51 8B 51 04 33 C0)
    DWORD trampProtect = 0;
    VirtualProtect(gTrampolineGetCountdownState, sizeof(gTrampolineGetCountdownState), PAGE_EXECUTE_READWRITE, &trampProtect);
    std::memcpy(gTrampolineGetCountdownState, target, 6);
    gTrampolineGetCountdownState[6] = 0xE9; // JMP back to 0x005EEA46
    const intptr_t jmpBack = static_cast<intptr_t>(0x005EEA46) - (reinterpret_cast<intptr_t>(gTrampolineGetCountdownState + 6) + 5);
    const int32_t jumpBackOffset = static_cast<int32_t>(jmpBack);
    std::memcpy(gTrampolineGetCountdownState + 7, &jumpBackOffset, sizeof(jumpBackOffset));
    FlushInstructionCache(GetCurrentProcess(), gTrampolineGetCountdownState, sizeof(gTrampolineGetCountdownState));

    // Install JMP to HookedGetCountdownState_Naked at 0x005EEA40
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedGetCountdownState_Naked) - (0x005EEA40 + 5);
    target[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(target + 1, &jump, sizeof(jump));
    target[5] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), target, 6);
    DWORD unused = 0;
    VirtualProtect(target, 6, oldProtect, &unused);

    LogMessage("[HOOK] Native GetCountdownState hooked at 0x005EEA40 (millisecond countdown sync active).\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Dedicated Co-op Save Path Redirection Hooks (IAT Hooking):
// Redirects %LOCALAPPDATA%\NFS Underground 2 -> %LOCALAPPDATA%\NFS Underground 2 Co-op
// whenever gCoopSessionMode != COOP_SESSION_INACTIVE.
// ---------------------------------------------------------------------------
using CreateFileAFn = HANDLE(WINAPI*)(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile);

using CreateDirectoryAFn = BOOL(WINAPI*)(
    LPCSTR lpPathName,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes);

using FindFirstFileAFn = HANDLE(WINAPI*)(
    LPCSTR lpFileName,
    LPWIN32_FIND_DATAA lpFindFileData);

using DeleteFileAFn = BOOL(WINAPI*)(LPCSTR lpFileName);
using RemoveDirectoryAFn = BOOL(WINAPI*)(LPCSTR lpPathName);

static CreateFileAFn g_pfnOriginalCreateFileA = nullptr;
static CreateDirectoryAFn g_pfnOriginalCreateDirectoryA = nullptr;
static FindFirstFileAFn g_pfnOriginalFindFirstFileA = nullptr;
static DeleteFileAFn g_pfnOriginalDeleteFileA = nullptr;
static RemoveDirectoryAFn g_pfnOriginalRemoveDirectoryA = nullptr;

bool RedirectSavePath(const char* inPath, char* outPath, size_t outSize) {
    return RouteContextSavePath(inPath, outPath, outSize);
}

HANDLE WINAPI HookedCreateFileA(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
{
    char redirected[MAX_PATH];
    const char* pPath = lpFileName;
    if (RedirectSavePath(lpFileName, redirected, sizeof(redirected))) {
        pPath = redirected;
    }
    char clientWrite[MAX_PATH]{};
    if(RouteClientCoopWrite(pPath,clientWrite,sizeof(clientWrite),dwDesiredAccess)) pPath=clientWrite;
    auto specialIo=gSpecialSaveFence.Lock();const auto specialPath=WideText(pPath);
    if(!specialIo.MayOpen(specialPath,dwDesiredAccess,dwCreationDisposition)) {SetLastError(ERROR_BUSY);return INVALID_HANDLE_VALUE;}
    HANDLE result=g_pfnOriginalCreateFileA(pPath, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    if(!specialIo.Opened(uintptr_t(result),specialPath,dwDesiredAccess,dwCreationDisposition)) {
        CloseHandle(result);SetLastError(ERROR_TOO_MANY_OPEN_FILES);return INVALID_HANDLE_VALUE;
    }
    TrackCoopWriteHandle(result,pPath,dwDesiredAccess);
    return result;
}

BOOL WINAPI HookedCreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    char redirected[MAX_PATH];
    const char* pPath = lpPathName;
    if (RedirectSavePath(lpPathName, redirected, sizeof(redirected))) {
        pPath = redirected;
    }
    auto specialIo=gSpecialSaveFence.Lock();
    if(!specialIo.MayMutate(WideText(pPath))) {SetLastError(ERROR_BUSY);return FALSE;}
    return g_pfnOriginalCreateDirectoryA(pPath, lpSecurityAttributes);
}

HANDLE WINAPI HookedFindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData) {
    char redirected[MAX_PATH];
    const char* pPath = lpFileName;
    if (RedirectSavePath(lpFileName, redirected, sizeof(redirected))) {
        pPath = redirected;
    }
    const HANDLE found=g_pfnOriginalFindFirstFileA(pPath, lpFindFileData);
    const DWORD error=GetLastError();
    TraceCoopProfileEnumeration(lpFileName,pPath,found,lpFindFileData,error);
    SetLastError(error);return found;
}

BOOL WINAPI HookedDeleteFileA(LPCSTR lpFileName) {
    char redirected[MAX_PATH];
    const char* pPath = lpFileName;
    if (RedirectSavePath(lpFileName, redirected, sizeof(redirected))) {
        pPath = redirected;
    }
    auto specialIo=gSpecialSaveFence.Lock();
    if(!specialIo.MayMutate(WideText(pPath))) {SetLastError(ERROR_BUSY);return FALSE;}
    return g_pfnOriginalDeleteFileA(pPath);
}

BOOL WINAPI HookedRemoveDirectoryA(LPCSTR lpPathName) {
    char redirected[MAX_PATH];
    const char* pPath = lpPathName;
    if (RedirectSavePath(lpPathName, redirected, sizeof(redirected))) {
        pPath = redirected;
    }
    auto specialIo=gSpecialSaveFence.Lock();
    if(!specialIo.MayMutate(WideText(pPath))) {SetLastError(ERROR_BUSY);return FALSE;}
    return g_pfnOriginalRemoveDirectoryA(pPath);
}

bool InstallSavePathRedirectionHooks() {
    auto pCreateFileA = reinterpret_cast<CreateFileAFn*>(0x007831D0);
    auto pCreateDirectoryA = reinterpret_cast<CreateDirectoryAFn*>(0x0078312C);
    auto pFindFirstFileA = reinterpret_cast<FindFirstFileAFn*>(0x00783128);
    auto pDeleteFileA = reinterpret_cast<DeleteFileAFn*>(0x0078322C);
    auto pRemoveDirectoryA = reinterpret_cast<RemoveDirectoryAFn*>(0x007832C4);

    g_pfnOriginalCreateFileA = *pCreateFileA;
    g_pfnOriginalCreateDirectoryA = *pCreateDirectoryA;
    g_pfnOriginalFindFirstFileA = *pFindFirstFileA;
    g_pfnOriginalDeleteFileA = *pDeleteFileA;
    g_pfnOriginalRemoveDirectoryA = *pRemoveDirectoryA;

    DWORD oldProtect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(0x00783120), 0x200, PAGE_READWRITE, &oldProtect)) {
        *pCreateFileA = &HookedCreateFileA;
        *pCreateDirectoryA = &HookedCreateDirectoryA;
        *pFindFirstFileA = &HookedFindFirstFileA;
        *pDeleteFileA = &HookedDeleteFileA;
        *pRemoveDirectoryA = &HookedRemoveDirectoryA;
        DWORD unused = 0;
        VirtualProtect(reinterpret_cast<void*>(0x00783120), 0x200, oldProtect, &unused);
        LogMessage("[HOOK] Save Path Redirection hooks installed in IAT (Dedicated Co-op Saves active)\r\n");
        return true;
    }
    LogMessage("[WARN] Failed to unprotect IAT for Save Path Redirection.\r\n");
    return false;
}

uint32_t ComputeSpeed2Crc(const void* buf, size_t len) {
    if (!buf || len < 4) return 0;
    auto pfnCrc = reinterpret_cast<uint32_t(__cdecl*)(const void*, size_t)>(0x006FE173);
    return pfnCrc(buf, len);
}

#if 0 // Removed unsafe Gemini save mutation/template cloning; retained only as historical reference.
void SyncSaveDeviceBasePath() {
    char coopDir[MAX_PATH]{};
    if (SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, coopDir) == S_OK) {
        std::strcat(coopDir, "\\NFS Underground 2 Co-op");
        CreateDirectoryA(coopDir, nullptr);

        uintptr_t pDevRoot = *reinterpret_cast<uintptr_t*>(0x00836498);
        if (pDevRoot) {
            uintptr_t pSub = *reinterpret_cast<uintptr_t*>(pDevRoot + 0x5C);
            if (pSub) {
                uintptr_t pDev = *reinterpret_cast<uintptr_t*>(pSub);
                if (pDev) {
                    char* pBasePath = reinterpret_cast<char*>(pDev + 0x174);
                    std::strncpy(pBasePath, coopDir, MAX_PATH);
                    pBasePath[MAX_PATH - 1] = '\0';
                    LogMessage("[SAVE] Updated SaveDevice base path to Co-op folder!\r\n");
                }
            }
        }
    }
}

bool SetupDedicatedCoopSaveProfile() {
    const char* activeProfile = reinterpret_cast<const char*>(0x0083A9E0);
    const char* coopProfileName = (activeProfile && activeProfile[0] != '\0') ? activeProfile : (gIsSecondInstance ? "Player2" : "Player1");
    std::strncpy(gCoopSaveProfileName, coopProfileName, sizeof(gCoopSaveProfileName) - 1);
    gCoopSaveProfileName[sizeof(gCoopSaveProfileName) - 1] = '\0';

    char localApp[MAX_PATH]{};
    if (SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, localApp) != S_OK) {
        LogMessage("[SAVE] Failed to get LOCALAPPDATA path.\r\n");
        return false;
    }

    char coopBaseDir[MAX_PATH]{};
    std::snprintf(coopBaseDir, sizeof(coopBaseDir), "%s\\NFS Underground 2 Co-op", localApp);
    CreateDirectoryA(coopBaseDir, nullptr);

    char coopProfileDir[MAX_PATH]{};
    std::snprintf(coopProfileDir, sizeof(coopProfileDir), "%s\\%s", coopBaseDir, coopProfileName);
    CreateDirectoryA(coopProfileDir, nullptr);

    char coopSavePath[MAX_PATH]{};
    std::snprintf(coopSavePath, sizeof(coopSavePath), "%s\\%s", coopProfileDir, coopProfileName);

    // Sync Game Engine base path and global pointer
    *reinterpret_cast<const char**>(0x007F66A0) = "NFS Underground 2 Co-op";
    SyncSaveDeviceBasePath();

    std::vector<uint8_t> saveBuf;
    bool needCreateNew = true;

    // 1. Try reading existing Co-op save
    HANDLE hFile = CreateFileA(coopSavePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD fsize = GetFileSize(hFile, nullptr);
        if (fsize == 54966) {
            saveBuf.resize(fsize);
            DWORD bytesRead = 0;
            if (ReadFile(hFile, saveBuf.data(), fsize, &bytesRead, nullptr) && bytesRead == fsize) {
                if (*reinterpret_cast<uint32_t*>(saveBuf.data()) == 0x4D433032) {
                    needCreateNew = false;
                    LogMessage("[SAVE] Loaded existing dedicated Co-op save file from disk!\r\n");
                }
            }
        }
        CloseHandle(hFile);
    }

    // 2. If no Co-op save exists, clone from available singleplayer profile and initialize starter state
    if (needCreateNew) {
        LogMessage("[SAVE] No existing Co-op save found; creating fresh Co-op Career save...\r\n");
        const char* templateProfiles[] = { "Player", "Batuhan", "Ashan", "NAME" };
        bool templateFound = false;
        for (const char* tpl : templateProfiles) {
            char tplPath[MAX_PATH]{};
            std::snprintf(tplPath, sizeof(tplPath), "%s\\NFS Underground 2\\%s\\%s", localApp, tpl, tpl);
            HANDLE hTpl = CreateFileA(tplPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hTpl != INVALID_HANDLE_VALUE) {
                DWORD fsize = GetFileSize(hTpl, nullptr);
                if (fsize == 54966) {
                    saveBuf.resize(fsize);
                    DWORD bytesRead = 0;
                    if (ReadFile(hTpl, saveBuf.data(), fsize, &bytesRead, nullptr) && bytesRead == fsize) {
                        if (*reinterpret_cast<uint32_t*>(saveBuf.data()) == 0x4D433032) {
                            templateFound = true;
                            CloseHandle(hTpl);
                            char tLog[128];
                            std::snprintf(tLog, sizeof(tLog), "[SAVE] Using template profile '%s' for Co-op save initialization.\r\n", tpl);
                            LogMessage(tLog);
                            break;
                        }
                    }
                }
                CloseHandle(hTpl);
            }
        }

        if (!templateFound) {
            LogMessage("[SAVE] No singleplayer template save found! Aborting save generation.\r\n");
            return false;
        }

        // Customize the template:
        // Set Profile Name at 0xD225 (16 bytes, null-padded)
        std::memset(saveBuf.data() + 0xD225, 0, 16);
        std::strncpy(reinterpret_cast<char*>(saveBuf.data() + 0xD225), coopProfileName, 15);

        // Set starter cash to $10,000 at offset 0xA16A
        *reinterpret_cast<uint32_t*>(saveBuf.data() + 0xA16A) = 10000;

        // Recalculate Block 2 CRC (offset 0x24, length 54,930 / 0xD692)
        uint32_t b2_crc = ComputeSpeed2Crc(saveBuf.data() + 0x24, 0xD692);
        *reinterpret_cast<uint32_t*>(saveBuf.data() + 0x14) = b2_crc;

        // Recalculate Header CRC (offset 0x00, length 0x18)
        uint32_t h_crc = ComputeSpeed2Crc(saveBuf.data(), 0x18);
        *reinterpret_cast<uint32_t*>(saveBuf.data() + 0x18) = h_crc;

        // Write the fresh save file to disk
        HANDLE hNew = CreateFileA(coopSavePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hNew != INVALID_HANDLE_VALUE) {
            DWORD bytesWritten = 0;
            WriteFile(hNew, saveBuf.data(), static_cast<DWORD>(saveBuf.size()), &bytesWritten, nullptr);
            CloseHandle(hNew);
            LogMessage("[SAVE] Created fresh dedicated Co-op save file on disk with $10,000 starter cash!\r\n");
        } else {
            LogMessage("[SAVE] Failed to create dedicated Co-op save file on disk!\r\n");
        }
    }

    // 3. Update in-RAM headers and career flags
    std::memset(reinterpret_cast<void*>(0x0083A9E0), 0, 16);
    std::strncpy(reinterpret_cast<char*>(0x0083A9E0), coopProfileName, 15);
    std::memset(reinterpret_cast<void*>(0x00850114), 0, 16);
    std::strncpy(reinterpret_cast<char*>(0x00850114), coopProfileName, 15);

    *reinterpret_cast<uint32_t*>(0x0083A9D8) = 0; // Local player index 0
    *reinterpret_cast<uint8_t*>(0x0083AA00) = 1;  // Profile valid / loaded
    *reinterpret_cast<uint8_t*>(0x00844AD8) = 1;  // AutoSave enabled
    *reinterpret_cast<uint8_t*>(0x00844AD9) = 1;  // AutoSave prompt seen
    *reinterpret_cast<uint32_t*>(0x00850078) = 1; // Career mode active
    *reinterpret_cast<uint32_t*>(0x00863480) = 0; // Active Car slot 0

    // Set Career cash in RAM
    uint32_t cash = *reinterpret_cast<const uint32_t*>(saveBuf.data() + 0xA16A);
    *reinterpret_cast<uint32_t*>(0x00861E74) = cash;

    // Deserialization: UserProfile is at UserProfileManager + 0x10 (0x0083A9E0)
    void* pUserProfile = reinterpret_cast<void*>(0x0083A9D0 + 0x10);
    const uint8_t* pPayload = saveBuf.data() + 0x24;
    const uint32_t payloadLen = 0xD692;

    __asm {
        mov ecx, pUserProfile       // 0x0083A9E0 (&UserProfileManager->mProfiles[0])
        push 0                      // bApplyControls = 0 (skips sound reset in menu)
        push payloadLen            // 0xD692 (54,930 bytes)
        push pPayload              // buffer + 0x24
        mov eax, 0x005344D0        // UserProfile::Deserialize
        call eax
    }

    // Also update in-memory save buffer pointer if allocated
    uintptr_t pSaveBufMem = *reinterpret_cast<uintptr_t*>(0x0083A9D0 + 0x16778);
    if (pSaveBufMem) {
        std::memcpy(reinterpret_cast<void*>(pSaveBufMem), saveBuf.data() + 0x24, 0xD692);
    }

    char sLog[180];
    std::snprintf(sLog, sizeof(sLog),
        "[SAVE] Co-op profile '%s' active! Cash=$%lu. Singleplayer is 100%% isolated.\r\n",
        coopProfileName, static_cast<unsigned long>(cash));
    LogMessage(sLog);
    return true;
}

// ---------------------------------------------------------------------------
// Native String Translation Hook (0x004FF9D0 & 0x004FFA80):
// Intercepts LookupLocalizedString calls and translates "LAN" -> "CO-OP" natively!
// 0x004FF9D0 is the core binary-search lookup called by FEng, Dialogs, and menus.
// ---------------------------------------------------------------------------
#endif
#include "CoopNativeSession.inl"
// The error/help shortcut belongs to the visible COOP lobby, not to the
// successful Join handshake. Never take Z from native profile/name entry.
void BindCoopConsoleForUi(const coop::SessionSnapshot& session) {
    const bool lobby=gCoopUiActive&&gCurrentNativeScreen=="PC_OL_Lobby.fng";
    const bool active=lobby||IsCoopGameplayActive()||session.joined;
    const bool allow=lobby||(session.phase!=coop::Phase::CareerSelection&&session.phase!=coop::Phase::ProfileSync);
    CoopAnnouncements::Bind(active,session.sessionId,allow);
}
void TraceCoopConsoleToggle(const char* source,bool handled) {
    bool enabled=false,context=false,expanded=false,composing=false;
    AcquireSRWLockShared(&CoopAnnouncements::lock);
    enabled=CoopAnnouncements::enabled;context=CoopAnnouncements::keyContext;
    expanded=CoopAnnouncements::model.expanded;composing=CoopAnnouncements::editor.active;
    ReleaseSRWLockShared(&CoopAnnouncements::lock);
    char line[256]{};
    std::snprintf(line,sizeof(line),"[CONSOLE_KEY] source=%s handled=%u enabled=%u context=%u expanded=%u composing=%u coopUi=%u screen=%.80s.\r\n",
        source,handled,enabled,context,expanded,composing,gCoopUiActive,gCurrentNativeScreen.c_str());
    LogMessage(line);
}
void PollCoopConsoleToggle(bool down) {
    static bool held=false;
    const bool composing=CoopAnnouncements::CapturesKeyboard();
    const bool before=CoopAnnouncements::Expanded();
    CoopAnnouncements::PollToggle(down);
    if(down&&!held&&!composing)TraceCoopConsoleToggle("poll",before!=CoopAnnouncements::Expanded());
    held=down;
}
#include "CoopModSupportNative.inl"
#include "CoopMotionNetwork.inl"
#include "CoopLobbyNative.inl"
#include "CoopCareerNative.inl"
#include "CoopCareerOfferNative.inl"
#include "CoopMovieSurfaceNative.inl"
#include "CoopMovieSyncNative.inl"
#include "CoopSponsorNative.inl"
#include "CoopVisualGateNative.inl"
#include "CoopSpecialNative.inl"
#include "CoopPeerLabelNative.inl"
static_assert(coop_sponsor::kMagic!=coop::kSessionMagic && coop_sponsor::kMagic!=kPacketMagic &&
    coop_sponsor::kMagic!=coop_movie::kMagic,"Sponsor datagrams must not be consumed by the session/motion/movie dispatcher");
static_assert(coop_visual::kMagic!=coop_sponsor::kMagic&&coop_visual::kMagic!=coop::kSessionMagic&&
    coop_visual::kMagic!=kPacketMagic&&coop_visual::kMagic!=coop_movie::kMagic,"Visual-star observations need their own dispatcher identity");
#include "CoopFrameTrace.inl"
#include "CoopRaceFinish.inl"
#include "CoopRaceLifecycle.inl"
#include "CoopDiscordPresence.inl"
#include "CoopUrlTournamentNative.inl"
#include "CoopRaceReturn.inl"
#include "CoopAIRosterNative.inl"
#include "CoopDriftNative.inl"
#include "CoopLivePauseNative.inl"
#include "CoopAIWorldRuntime.inl"
#include "CoopAIVisualTrace.inl"
#include "CoopAINativeHooks.inl"
#include "CoopWorldContactNative.inl"
#include "CoopFleetAssetsNative.inl"
#include "CoopFleetNative.inl"
#include "CoopFleetWorldRuntime.inl"
bool ProbeCoopVehicleAssetAdaptersReady() {
    // Called only on the game/UI thread. Network admission consumes the
    // published atomic result, never executable bytes or native game objects.
    if(CoopCarCatalogNative::Count()>coop_car_catalog::kStockCount&&!CoopStockCatalogReaderReady())return false;
    if(!gCoopAIEnabled&&!gCoopFleetEnabled)return true;
    return CoopFleetAssetHooks::enabled&&CoopFleetAssetsNative::VerifyNative()&&
        (!gCoopFleetEnabled||CoopFleetWorldRuntime::installed);
}
#include "CoopRearRenderTrace.inl"
#include "CoopAddonMenuTrace.inl"
#include "CoopStockCatalogNative.inl"
#include "CoopWorldImpactRuntime.inl"
#include "CoopTrafficInventory.inl"

#include "CoopPostStreamNative.inl"
#include "CoopUrlTournamentRuntime.inl"

void ShowCoopChatMessage(uint16_t player,const char* name,const char* text){CoopAnnouncements::AddChat(player,name,text);}
void ShowCoopChatStatus(const char* text){CoopAnnouncements::Post(text);}
void SetCoopSponsorPrompt(const char* text){CoopAnnouncements::SetSponsorPrompt(text);}
#include "CoopChatRuntime.inl"
static_assert(coop_console::kMessageBytes==coop_chat::kMaxTextBytes,"Console and chat wire budgets must match");

static void TraceCoopAIRepair(DWORD flow) {
    static DWORD previous = 0;
    if (flow != 6 || !gCoopAIEnabled || !IsCoopGameplayActive()) return;
    const auto session = CoopSnapshot();
    if (!session.joined || !session.fresh ||
        (session.role != coop::Role::Host && session.role != coop::Role::Client)) return;
    const DWORD now = GetTickCount();
    if (previous && static_cast<DWORD>(now - previous) < 5000) return;
    previous = now;
    uint8_t centres[2]{};
    ReadGameMemory(CoopDualRegionNative::addresses.streamer + 0xC4, &centres[0], 1);
    ReadGameMemory(CoopDualRegionNative::addresses.streamer + 0x120, &centres[1], 1);
    char line[256]{};
    std::snprintf(line, sizeof(line),
        "[AI_REPAIR] peer_region_enabled=%u registered=%lu refused=%lu centres=%u/%u placement_enabled=%u route_skipped=%lu wake_skipped=%lu integration_skipped=%lu.\r\n",
        CoopDualRegionNative::enabled ? 1u : 0u,
        static_cast<unsigned long>(CoopDualRegionNative::registered),
        static_cast<unsigned long>(CoopDualRegionNative::refused), centres[0], centres[1],
        CoopAINativePlacementHooks::enabled ? 1u : 0u,
        static_cast<unsigned long>(CoopAINativePlacementHooks::routeSkipped),
        static_cast<unsigned long>(CoopAINativePlacementHooks::wakeSkipped),
        static_cast<unsigned long>(CoopAINativePlacementHooks::integrationSkipped));
    LogMessage(line);
    if (!gCoopFleetEnabled) return;
    char ambientLine[512]{};
    std::snprintf(ambientLine, sizeof(ambientLine),
        "[AMBIENT_GUARD] scanners=%u placement_skipped=%u refresh_skipped=%u follow_refused=%u epoch=%llu generation=%llu\r\n",
        CoopFleetScannerNativeReady(), CoopFleetNative::scannerPlacementSkipped,
        CoopFleetNative::scannerRefreshSkipped, CoopFleetNative::followRefused,
        static_cast<unsigned long long>(CoopFleetWorldEpoch()),
        static_cast<unsigned long long>(CoopFleetLocalGeneration()));
    LogMessage(ambientLine);
    uintptr_t cars[kMaxVehicleSlots]{};
    if (!ReadGameMemory(kCarArray, cars, sizeof(cars))) return;
    for (uintptr_t car : cars) {
        uintptr_t parameter = 0, driver = 0, vtable = 0, owner = 0, reservedRide = 0, identityRide = 0;
        uint32_t type = 0, mode = 0, role = 0, reservedKind = 0, identityKind = 0;
        uint8_t id = 0, remote = 0, active = 0;
        if (!IsValidUserPtr(car) || !ReadGameMemory(car + 0x14, &parameter, sizeof(parameter)) ||
            !IsValidUserPtr(parameter) || !ReadGameMemory(parameter + 4, &type, 4) || type != 2 ||
            !ReadGameMemory(parameter + 1, &id, 1) || id < 100 || id > 102) continue;
        ReadGameMemory(parameter + 0x900, &remote, 1);
        ReadGameMemory(car + 0x24, &role, 4); ReadGameMemory(car + 0x28, &mode, 4);
        ReadGameMemory(car + 0x550, &active, 1); ReadGameMemory(car + 0x2C, &driver, sizeof(driver));
        // Even dormant drivers have these first two words. Never sample full-AI
        // offsets such as +6F8/+6FC on a 0x54-byte dormant/unknown driver.
        const bool driverReadable = IsValidUserPtr(driver) && ReadGameMemory(driver, &vtable, sizeof(vtable)) &&
            ReadGameMemory(driver + 4, &owner, sizeof(owner));
        const bool reserved = ReadManagedCoopFleet(car, reservedRide, reservedKind);
        const bool fullIdentity = ReadCoopFleetIdentity(car, identityRide, identityKind);
        std::snprintf(ambientLine, sizeof(ambientLine),
            "[AMBIENT_INV] id=%u car=%08X param=%08X remote=%u mode=%u active=%u role=%u driver=%08X vt=%08X backlink=%u readable=%u reserved=%u full_identity=%u scanner_blocked=%u\r\n",
            static_cast<unsigned>(id), static_cast<unsigned>(car), static_cast<unsigned>(parameter),
            static_cast<unsigned>(remote), mode, static_cast<unsigned>(active), role,
            static_cast<unsigned>(driver), static_cast<unsigned>(vtable), owner == car, driverReadable,
            reserved, fullIdentity, CoopFleetNative::BlockScannerCar(car));
        LogMessage(ambientLine);
    }
}

constexpr uint32_t kCoopStringKey = 0x00C00C00;

static uint8_t gTrampolineLookupString[16]{};

const char* CallOriginalLookupString(uint32_t stringKey) {
    const char* pRetStr = nullptr;
    __asm {
        mov edx, stringKey
        lea eax, gTrampolineLookupString
        call eax
        mov pRetStr, eax
    }
    return pRetStr;
}

const char* __cdecl HookedLookupStringCore(uint32_t stringKey) {
    if (stringKey == kCoopStringKey) {
        return "CO-OP";
    }

    if (gCoopSessionMode.load() != COOP_SESSION_INACTIVE) {
        switch (stringKey) {
        case 0x7294434C: // "LAN"
            return "CO-OP";
        case 0x4DE319CC: // "LAN Server Select"
            return "CO-OP Server Select";
        case 0x80AF4FC7: // "Host LAN Server"
            return "Host CO-OP Server";
        case 0x43003B5B: // "Looking for LAN servers ..."
            return "Looking for CO-OP servers ...";
        case 0xB13D6A9B: // "Starting LAN Server ..."
            return "Starting CO-OP Server ...";
        case 0x03F6F2BF: // "LAN Main"
            return "CO-OP Main";
        case 0xE50258B1: // "LAN Lobby"
            return "CO-OP Lobby";
        case 0x19889765: // "LAN Options"
            return "CO-OP Options";
        case 0x84E79DFF: // "Online LAN"
            return "Online CO-OP";
        case 0xF39723A1: // "Quit to LAN Main Menu"
            return "Quit to CO-OP Main Menu";
        case 0xD63B6982: // "Are you sure you want to quit to the LAN main menu?"
            return "Are you sure you want to quit to the CO-OP main menu?";
        case 0xC2E59520: // "Are you sure you want to exit LAN mode?"
            return "Are you sure you want to exit CO-OP mode?";
        case 0x309B91BA: // "You are hosting a LAN server..."
            return "You are hosting a CO-OP server. If you exit CO-OP mode, any users will be disconnected. Continue?";
        case 0xA8CD6FD3: // "There was a problem trying to start the LAN server..."
            return "There was a problem trying to start the CO-OP server. Please check your network settings.";
        case 0xA2717C2A: // "Your connection to the LAN server has been lost."
            return "Your connection to the CO-OP server has been lost.";
        case 0xC91A3FDE: // "You have lost your connection to the LAN Server..."
            return "You have lost your connection to the CO-OP Server. You will be returned to the main menu.";
        case 0xB5074AB4: // "Select Car"
        case 0xC0BEDF15: // "Select Car"
        case 0x063D57A0: // "Join Game"
        case 0x82CDBA8B: // "Join Game"
        case 0x3E56DED3: // "Join Game"
            if (!gIsSecondInstance && gCoopSessionMode.load() != COOP_SESSION_CLIENT) {
                return "START THE CAREER";
            } else {
                return "WAITING FOR HOST TO START...";
            }
        default:
            break;
        }

        const char* orig = CallOriginalLookupString(stringKey);
        if (orig) {
            if (std::strcmp(orig, "Select Car") == 0 || std::strcmp(orig, "Join Game") == 0) {
                if (!gIsSecondInstance && gCoopSessionMode.load() != COOP_SESSION_CLIENT) {
                    return "START THE CAREER";
                } else {
                    return "WAITING FOR HOST TO START...";
                }
            }
            if (std::strstr(orig, "LAN")) {
                static thread_local char sDynamicBuf[256];
                const char* pIn = orig;
                char* pOut = sDynamicBuf;
                char* pEnd = sDynamicBuf + sizeof(sDynamicBuf) - 10;
                while (*pIn && pOut < pEnd) {
                    if (pIn[0] == 'L' && pIn[1] == 'A' && pIn[2] == 'N') {
                        std::memcpy(pOut, "CO-OP", 5);
                        pOut += 5;
                        pIn += 3;
                    } else {
                        *pOut++ = *pIn++;
                    }
                }
                *pOut = '\0';
                return sDynamicBuf;
            }
            return orig;
        }
    }

    return CallOriginalLookupString(stringKey);
}

__declspec(naked) void HookedLookupStringAsm() {
    __asm {
        push edx // native512920 returns this key after lookup; C++ may clobber it
        push edx // stringKey
        call HookedLookupStringCore
        add esp, 4
        pop edx
        ret
    }
}

const char* __cdecl HookedGetLocalizedString(uint32_t stringKey) {
    return HookedLookupStringCore(stringKey);
}

bool InstallStringTranslationHook() {
    // 1. Hook 0x004FF9D0 (LookupLocalizedString - primary string lookup used by FEng and Dialogs)
    auto* hook9D0 = reinterpret_cast<BYTE*>(0x004FF9D0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook9D0, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x004FF9D0 for string hook.\r\n");
        return false;
    }

    DWORD trampProtect = 0;
    VirtualProtect(gTrampolineLookupString, sizeof(gTrampolineLookupString), PAGE_EXECUTE_READWRITE, &trampProtect);
    std::memcpy(gTrampolineLookupString, hook9D0, 7);
    gTrampolineLookupString[7] = 0xE9; // JMP
    const intptr_t jmpBack = static_cast<intptr_t>(0x004FF9D7) - (reinterpret_cast<intptr_t>(gTrampolineLookupString + 7) + 5);
    const int32_t jumpBackOffset = static_cast<int32_t>(jmpBack);
    std::memcpy(gTrampolineLookupString + 8, &jumpBackOffset, sizeof(jumpBackOffset));
    FlushInstructionCache(GetCurrentProcess(), gTrampolineLookupString, sizeof(gTrampolineLookupString));

    const intptr_t rel9D0 = reinterpret_cast<intptr_t>(&HookedLookupStringAsm) - (0x004FF9D0 + 5);
    hook9D0[0] = 0xE9; // JMP
    const int32_t jmp9D0 = static_cast<int32_t>(rel9D0);
    std::memcpy(hook9D0 + 1, &jmp9D0, sizeof(jmp9D0));
    hook9D0[5] = 0x90; // NOP
    hook9D0[6] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook9D0, 7);
    DWORD unused = 0;
    VirtualProtect(hook9D0, 7, oldProtect, &unused);

    // Leave stock4FFA80 intact: it calls our core and retains its native
    // missing-key fallbackC01A6F63. A second wrapper replacement loses that.
    LogMessage("[HOOK] Localized core4FF9D0 preserves EDX; native wrapper/fallback and shop body keys retained.\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Dedicated CO-OP Button on Main Menu Carousel (0x004AEF68):
// In SPEED2.EXE, 0x004AEE30 builds the Main Menu carousel (Career, Quick Race,
// Online, LAN, Customize, SaveLoad, Options).
// Right after adding the LAN item at 0x004AEF68, we add our dedicated CO-OP item!
// This gives the player BOTH "LAN" and "COOP" as native, distinct carousel buttons!
// ---------------------------------------------------------------------------
static uintptr_t gCoopItemVTable[2] = {
    0x004AEB70, // method 0: base scroller item query / update
    0           // method 1: set at runtime to HookedCoopButtonClick
};

void __cdecl OnCoopButtonClicked() {
    LogMessage("[UI] COOP browser opened; singleplayer profile unchanged.\r\n");
    BrowseCoopServers();
}

__declspec(naked) void HookedCoopButtonClick() {
    __asm {
        cmp dword ptr [esp + 8], 0x0C407210
        jne not_click

        pushad
        call OnCoopButtonClicked
        popad

        // Execute native transition to LAN / CO-OP Lobby (0x004AEDEA)
        push 0x004AEDEA
        ret

    not_click:
        ret 0x14
    }
}

void __cdecl AddCoopMenuItem(void* scroller) {
    if (!scroller) return;

    // Constructing a menu must not destroy a persistent UDP session.

    using MallocFn = void*(*)(size_t);
    auto pMalloc = reinterpret_cast<MallocFn>(0x00575620);
    void* pCoopItem = pMalloc(0x48);
    if (!pCoopItem) return;

    // 0x0051F670: ItemInit(this, iconHash, stringHash, 0)
    // 0xD13A2E66 = LAN / Network icon (two monitors with arrow)
    using ItemInitFn = void(__thiscall*)(void*, uint32_t, uint32_t, int);
    auto pItemInit = reinterpret_cast<ItemInitFn>(0x0051F670);
    pItemInit(pCoopItem, 0xD13A2E66, kCoopStringKey, 0);

    // Set custom vtable for COOP item
    gCoopItemVTable[1] = reinterpret_cast<uintptr_t>(&HookedCoopButtonClick);
    *reinterpret_cast<uintptr_t*>(pCoopItem) = reinterpret_cast<uintptr_t>(&gCoopItemVTable[0]);
    // Mark item as selectable/enabled
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pCoopItem) + 0x45) = 1;

    // Add item to scroller using Scroller::AddItem (vtable[6] at 0x0050DAB0)
    uintptr_t* vtbl = *reinterpret_cast<uintptr_t**>(scroller);
    using AddItemFn = void(__thiscall*)(void*, void*);
    auto pAddItem = reinterpret_cast<AddItemFn>(vtbl[6]);
    pAddItem(scroller, pCoopItem);

    LogMessage("[UI] Successfully inserted dedicated COOP button into Main Menu carousel!\r\n");
}

__declspec(naked) void HookedAfterLanItemAdded() {
    __asm {
        // Original instruction at 0x004AEF68:
        mov dword ptr [esp + 0x1c], ebp
        call dword ptr [edx + 0x18] // Adds LAN item

        // esi is scroller. Add our CO-OP item right after LAN!
        pushad
        push esi
        call AddCoopMenuItem
        add esp, 4
        popad

        // Return to 0x004AEF6F (push 0x48)
        push 0x004AEF6F
        ret
    }
}

void __cdecl HookedScrollerSetTitle(void* textNode, uint32_t stringHash) {
    if (stringHash == kCoopStringKey || stringHash == 0x02935CFD || stringHash == 0x0013FF70) {
        LogMessage("[UI] Carousel active item: CO-OP! Setting label text to 'CO-OP'.\r\n");
        reinterpret_cast<void(__cdecl*)(void*, uint32_t)>(0x0050C900)(textNode, kCoopStringKey);
    } else {
        reinterpret_cast<void(__cdecl*)(void*, uint32_t)>(0x0050C900)(textNode, stringHash);
    }
}

bool InstallScrollerTitleHook() {
    auto* callSite = reinterpret_cast<BYTE*>(0x0050DA69);
    DWORD oldProtect = 0;
    if (!VirtualProtect(callSite, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x0050DA69 for carousel title hook.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedScrollerSetTitle) - (0x0050DA69 + 5);
    callSite[0] = 0xE8; // CALL
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(callSite + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), callSite, 5);
    DWORD unused = 0;
    VirtualProtect(callSite, 5, oldProtect, &unused);
    LogMessage("[HOOK] Carousel title hook installed at 0x0050DA69 (Displays 'CO-OP' on Main Menu)\r\n");
    return true;
}

bool InstallMainMenuCoopButtonHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x004AEF68);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x004AEF68 for Main Menu CO-OP button.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedAfterLanItemAdded) - (0x004AEF68 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    hook[6] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 7);
    DWORD unused = 0;
    VirtualProtect(hook, 7, oldProtect, &unused);
    LogMessage("[HOOK] Main Menu CO-OP button hook installed at 0x004AEF68 (Dedicated COOP carousel item active)\r\n");
    InstallScrollerTitleHook();
    return true;
}

// ---------------------------------------------------------------------------
// Main Menu Vanilla LAN Button Hook (0x004AEDE0):
// Ensures that clicking the vanilla LAN button leaves session mode INACTIVE!
// ---------------------------------------------------------------------------
void __cdecl OnMainMenuLanButtonClicked() {
    LogMessage("[UI] Main Menu vanilla LAN button clicked! Session inactive.\r\n");
    gCoopSessionMode.store(COOP_SESSION_INACTIVE);
    gCoopUiActive=false;
}

__declspec(naked) void HookedMainMenuLanButton() {
    __asm {
        cmp dword ptr [esp + 8], 0x0C407210
        jne jmp_mismatch

        pushad
        call OnMainMenuLanButtonClicked
        popad

        push 0x004AEDEA
        ret

    jmp_mismatch:
        push 0x004AEE2C
        ret
    }
}

bool InstallMainMenuLanButtonHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x004AEDE0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 10, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect Main Menu LAN button at 0x004AEDE0.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedMainMenuLanButton) - (0x004AEDE0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    for (int i = 5; i < 10; ++i) hook[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), hook, 10);
    DWORD unused = 0;
    VirtualProtect(hook, 10, oldProtect, &unused);
    LogMessage("[HOOK] Main Menu LAN button hooked at 0x004AEDE0 (Vanilla LAN guard active)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Safe Strcmp Crash Shield (0x0043DC30):
// Prevents 0xC0000005 crash at 0x0043DC40 when any function in SPEED2 passes
// a near-NULL pointer (< 0x10000, such as 0x00000040) to strcmp!
// ---------------------------------------------------------------------------
__declspec(naked) void SafeStrcmpHook() {
    __asm {
        mov edx, dword ptr [esp + 4]
        cmp edx, 0x00010000
        jb invalid_ptr

        mov eax, dword ptr [esp + 8]
        cmp eax, 0x00010000
        jb invalid_ptr

        push esi
        mov esi, eax

    compare_loop:
        mov al, byte ptr [edx]
        mov cl, byte ptr [esi]
        inc edx
        inc esi
        test al, al
        je check_done
        test cl, cl
        je check_done
        cmp al, cl
        je compare_loop

    check_done:
        movsx ecx, cl
        movsx eax, al
        sub eax, ecx
        pop esi
        ret

    invalid_ptr:
        mov eax, 1
        ret
    }
}

bool InstallStrcmpCrashShield() {
    auto* hook = reinterpret_cast<BYTE*>(0x0043DC30);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&SafeStrcmpHook) - (0x0043DC30 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), hook, 5);
    DWORD unused = 0;
    VirtualProtect(hook, 5, oldProtect, &unused);
    LogMessage("[HOOK] Safe strcmp crash shield installed at 0x0043DC30 (0x40 NULL dereference eliminated)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Server.dll bind() Hook:
// Forces server.dll listener socket to bind to INADDR_ANY (0.0.0.0) with SO_REUSEADDR.
// This enables simultaneous listening on loopback (127.0.0.1), LAN (192.168.x.x),
// and VPN adapters (e.g. Radmin VPN 26.x.x.x), preventing connection refusal.
// ---------------------------------------------------------------------------
using BindFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
static BindFn gOriginalServerDllBind = nullptr;

int WSAAPI HookedServerDllBind(SOCKET s, const sockaddr* name, int namelen) {
    if (name && namelen >= sizeof(sockaddr_in)) {
        sockaddr_in sin = *reinterpret_cast<const sockaddr_in*>(name);
        const uint16_t reqPort = ntohs(sin.sin_port);

        // Always enable SO_REUSEADDR on the server listener socket
        BOOL reuse = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        char oldIp[32]{};
        inet_ntop(AF_INET, &sin.sin_addr, oldIp, sizeof(oldIp));

        // For the game server port (9900), bind to INADDR_ANY (0.0.0.0)
        // so it accepts connections from loopback (127.0.0.1), LAN (192.168.x.x), and VPN (26.x.x.x)!
        if (reqPort == 9900) {
            sin.sin_addr.s_addr = INADDR_ANY;
        }

        int res = gOriginalServerDllBind ? gOriginalServerDllBind(s, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin))
                                         : bind(s, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin));

        char logBuf[192];
        std::snprintf(logBuf, sizeof(logBuf),
            "[SERVER_DLL] Hooked bind(sock=%u, port=%u, originalIp=%s -> actualIp=%s) res=%d (err=%d)\r\n",
            static_cast<unsigned>(s), reqPort, oldIp, (reqPort == 9900) ? "0.0.0.0" : oldIp,
            res, (res != 0) ? WSAGetLastError() : 0);
        LogMessage(logBuf);
        return res;
    }
    return gOriginalServerDllBind ? gOriginalServerDllBind(s, name, namelen) : bind(s, name, namelen);
}

void InstallServerDllBindHook(HMODULE hServer) {
    if (!hServer) return;
    auto* pBindIat = reinterpret_cast<BindFn*>(reinterpret_cast<uintptr_t>(hServer) + 0x391B8);
    DWORD oldProtect = 0;
    if (VirtualProtect(pBindIat, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (*pBindIat != &HookedServerDllBind) {
            gOriginalServerDllBind = *pBindIat;
            *pBindIat = &HookedServerDllBind;
            LogMessage("[HOOK] Installed server.dll bind() IAT hook (forces INADDR_ANY 0.0.0.0:9900)!\r\n");
        }
        DWORD unused = 0;
        VirtualProtect(pBindIat, sizeof(void*), oldProtect, &unused);
    }
}

// ---------------------------------------------------------------------------
// Server Name Capture Hook (0x004A2F5A):
// Intercepts the call to StartServerDll (0x005FA300) when the host submits
// the server name ("RONIBABA") at the "Enter the Name" dialog!
// Captures the entered server name as the Co-op Save Profile name, sets COOP_SESSION_HOST,
// and immediately broadcasts a host beacon over UDP.
// ---------------------------------------------------------------------------
static int CallOriginalStartServer(void* thisPtr, const char* serverName) {
    int res = 0;
    __asm {
        mov ecx, thisPtr
        push serverName
        mov eax, 0x005FA300
        call eax
        mov res, eax
    }
    return res;
}

int __cdecl OnStartServerDll(void* thisPtr, const char* serverName) {
#if 0 // Obsolete beacon-driven membership and server-name-as-save-name coupling.
    if (serverName && serverName[0] != '\0') {
        std::strncpy(gCoopSaveProfileName, serverName, sizeof(gCoopSaveProfileName) - 1);
        gCoopSaveProfileName[sizeof(gCoopSaveProfileName) - 1] = '\0';
        gCoopSessionMode.store(COOP_SESSION_HOST);

        const char* localApp = std::getenv("LOCALAPPDATA");
        if (localApp) {
            char saveDir[MAX_PATH];
            std::snprintf(saveDir, sizeof(saveDir), "%s\\NFS Underground 2\\NFS Underground 2 Co-op\\%s", localApp, gCoopSaveProfileName);
            DWORD attr = GetFileAttributesA(saveDir);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                LogMessage("[SAVE] Existing Co-op Save Profile detected! Loading existing save progress.\r\n");
            } else {
                LogMessage("[SAVE] New Co-op Save Profile specified. Fresh save will be created.\r\n");
            }
        }

        char logBuf[160];
        std::snprintf(logBuf, sizeof(logBuf), "[LOBBY] Host Server Name captured: '%s'! COOP_SESSION_HOST active.\r\n", gCoopSaveProfileName);
        LogMessage(logBuf);

        // Immediate beacon broadcast so client discovers host right away
        SessionBeaconPacket beacon{};
        beacon.magic = kBeaconMagic;
        beacon.version = kPacketVersion;
        beacon.size = sizeof(SessionBeaconPacket);
        beacon.hostPlayerNum = 1;
        beacon.flowState = 3;
        beacon.trackId = 4000;
        std::snprintf(beacon.hostName, sizeof(beacon.hostName), "%s", gCoopSaveProfileName);
        beacon.sentAtMs = GetTickCount();
        if (gGameSocket != INVALID_SOCKET) {
            // Unicast to target
            sendto(gGameSocket, reinterpret_cast<const char*>(&beacon), sizeof(beacon), 0,
                reinterpret_cast<const sockaddr*>(&gTargetEndpoint), sizeof(gTargetEndpoint));
            // Loopback to client on same PC (port 48931)
            sockaddr_in loopbackClient{};
            loopbackClient.sin_family = AF_INET;
            loopbackClient.sin_port = htons(48931);
            loopbackClient.sin_addr.s_addr = inet_addr("127.0.0.1");
            sendto(gGameSocket, reinterpret_cast<const char*>(&beacon), sizeof(beacon), 0,
                reinterpret_cast<const sockaddr*>(&loopbackClient), sizeof(loopbackClient));
            // Broadcast to LAN
            sockaddr_in bcastClient{};
            bcastClient.sin_family = AF_INET;
            bcastClient.sin_port = htons(48931);
            bcastClient.sin_addr.s_addr = INADDR_BROADCAST;
            sendto(gGameSocket, reinterpret_cast<const char*>(&beacon), sizeof(beacon), 0,
                reinterpret_cast<const sockaddr*>(&bcastClient), sizeof(bcastClient));
        }
    }

#endif
    if(gCoopUiActive) {
        const bool hosted=HostNativeCoopSession(serverName);
        LogMessage(hosted?"[SESSION] UDP host opened; native LAN server/wait dialog bypassed.\r\n":
            "[SESSION] Host request rejected by session state.\r\n");
        return hosted?1:0;
    }
    // Keep vanilla native discovery/server initialization, independent of profile selection.
    HMODULE hServer = LoadLibraryA("server.dll");
    if (hServer) {
        InstallServerDllBindHook(hServer);
    }

    return CallOriginalStartServer(thisPtr, serverName);
}

__declspec(naked) void HookedStartServerDll() {
    __asm {
        push dword ptr [esp + 4] // serverName
        push ecx                 // thisPtr
        call OnStartServerDll
        add esp, 8
        cmp byte ptr [gCoopUiActive], 0
        je original_host_return
        test al, al
        je original_host_return
        // Skip the native server waiting-dialog postlude; keep its epilogue.
        mov dword ptr [esp], 0x004A2FC6
    original_host_return:
        ret 4
    }
}

bool InstallServerNameCaptureHook() {
    auto* callSite = reinterpret_cast<BYTE*>(0x004A2F5A);
    DWORD oldProtect = 0;
    if (!VirtualProtect(callSite, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedStartServerDll) - (0x004A2F5A + 5);
    callSite[0] = 0xE8; // CALL
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(callSite + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), callSite, 5);
    DWORD unused = 0;
    VirtualProtect(callSite, 5, oldProtect, &unused);
    LogMessage("[HOOK] Server Name Capture hook installed at 0x004A2F5A (Active on Host Server creation)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Lobby 'Create Game' Removal Hook in PC_OL_Lobby Constructor (0x004F1574):
// 1. Completely hides and disables the Button 1 container (UI_PC_Generic_Button_sm_1)
//    and text node (0x626EB826) via FEngSetInvisible (0x0050CA00).
// 2. Clears pointers at [esi + 0x144] and [esi + 0x154] so mouse hit-testing never triggers Button 0.
// 3. Sets active button to 1 (Join Game) instead of 0 (Create Game).
// ---------------------------------------------------------------------------
void __fastcall OnLobbyButtonSetup(void* lobby) {
#if 0 // Do not null native FE handles or move nodes off-screen.
    auto* pLobby = reinterpret_cast<uint8_t*>(lobby);
    if (gCoopSessionMode.load() != COOP_SESSION_INACTIVE) {
        void* btn1Obj = *reinterpret_cast<void**>(pLobby + 0x144);
        void* text1Obj = *reinterpret_cast<void**>(pLobby + 0x154);

        using FEngSetInvisibleFn = void(__cdecl*)(void*);
        auto FEngSetInvisible = reinterpret_cast<FEngSetInvisibleFn>(0x0050CA00);

        if (btn1Obj) {
            FEngSetInvisible(btn1Obj);
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(btn1Obj) + 0x1C) |= 0x01; // hidden
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(btn1Obj) + 0x1C) &= ~0x02; // disabled
            *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(btn1Obj) + 0x24) = -9999.0f;
            *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(btn1Obj) + 0x28) = -9999.0f;
            *reinterpret_cast<void**>(pLobby + 0x144) = nullptr;
        }
        if (text1Obj) {
            FEngSetInvisible(text1Obj);
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(text1Obj) + 0x1C) |= 0x01;
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(text1Obj) + 0x1C) &= ~0x02;
            *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(text1Obj) + 0x24) = -9999.0f;
            *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(text1Obj) + 0x28) = -9999.0f;
            *reinterpret_cast<void**>(pLobby + 0x154) = nullptr;
        }

        // Set active button to 1 (Join Game) instead of 0!
        *reinterpret_cast<uint32_t*>(pLobby + 0x118) = 1;

        // Explicitly update Button 1 label in PC_OL_Lobby
        using SetUiLabelFn = int(__cdecl*)(char*, unsigned int, char*, int);
        auto setLabel = reinterpret_cast<SetUiLabelFn>(0x00537B80);
        char* pkg = *reinterpret_cast<char**>(pLobby + 4);
        if (pkg && setLabel) {
            const char* btn1Text = (!gIsSecondInstance && gCoopSessionMode.load() != COOP_SESSION_CLIENT)
                ? "START THE CAREER"
                : "WAITING FOR HOST TO START...";
            setLabel(pkg, 0xD4D9D042, const_cast<char*>(btn1Text), 0);
        }

        LogMessage("[UI] PC_OL_Lobby: 'Create Game' button removed, active button set to START THE CAREER (1)!\r\n");
    } else {
        *reinterpret_cast<uint32_t*>(pLobby + 0x118) = 0;
    }
#endif
    if(gCoopUiActive) {
        gNativeCoopLobby=lobby;
        UpdateNativeCoopLobby(lobby,CoopSnapshot());
    }
}

__declspec(naked) void HookedLobbyButtonSetupTrampoline() {
    __asm {
        pushad
        mov ecx, esi
        call OnLobbyButtonSetup
        popad

        mov ecx, esi
        mov eax, 0x0049ED30
        call eax

        push 0x004F157F
        ret
    }
}

bool InstallLobbyCreateGameNodeHideHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x004F1574);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 11, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x004F1574 for Lobby button setup hook.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedLobbyButtonSetupTrampoline) - (0x004F1574 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    for (int i = 5; i < 11; ++i) hook[i] = 0x90; // NOP out remaining bytes up to 0x004F157F
    FlushInstructionCache(GetCurrentProcess(), hook, 11);
    DWORD unused = 0;
    VirtualProtect(hook, 11, oldProtect, &unused);
    LogMessage("[HOOK] Lobby button setup hooked at 0x004F1574 (Create Game removed from PC_OL_Lobby)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Lobby Navigation Hook (0x004FD11E):
// In Co-op mode, clamps the minimum button index to 1 (Join Game), so pressing
// Left arrow will never select Button 0 (Create Game).
// ---------------------------------------------------------------------------
__declspec(naked) void HookedLobbyNavLeft() {
    __asm {
        mov eax, dword ptr [esi + 0x118]
        cmp dword ptr [gCoopSessionMode], 0
        je vanilla_left

        // Co-op mode: clamp minimum index to 1 (Join Game)
        cmp eax, 1
        jle exit_nav
        dec eax
        mov ecx, esi
        mov dword ptr [esi + 0x118], eax
        push 0x0049ED30
        call [esp]
        add esp, 4
        push 0x004FD3F2
        ret

    vanilla_left:
        cmp eax, ebp
        jle exit_nav
        dec eax
        mov ecx, esi
        mov dword ptr [esi + 0x118], eax
        push 0x0049ED30
        call [esp]
        add esp, 4
    exit_nav:
        push 0x004FD3F2
        ret
    }
}

bool InstallLobbyNavLeftHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x004FD11E);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x004FD11E for Lobby Nav Left hook.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedLobbyNavLeft) - (0x004FD11E + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), hook, 6);
    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    LogMessage("[HOOK] Lobby Nav Left hooked at 0x004FD11E (Left arrow clamped to Join Game)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Server Select Discovery Synthesis Hooks (0x004F307B & 0x004F31EF):
// Intercepts server list population in UI_PC_LAN_ServerSelect.fng.
// When in Co-op mode, if native LAN discovery returns 0 servers, synthesizes
// the Host's server entry (Name, 1/2 players, Host IP/Port) so Player 2 sees
// Player 1's server instantly without relying on broadcast packets!
// ---------------------------------------------------------------------------
using QueryLanServersFn = int(__cdecl*)(void*, void*, char*, int);
static QueryLanServersFn gOriginalQueryLanServers = reinterpret_cast<QueryLanServersFn>(0x00743AA0);

// Native 4F31DD pushes the copied server NAME, not a numeric list index.
using GetServerConnectRecordFn = void*(__cdecl*)(void*, void*, const char*);
static GetServerConnectRecordFn gOriginalGetServerConnectRecord = reinterpret_cast<GetServerConnectRecordFn>(0x00743A50);

#pragma pack(push, 1)
struct LanConnectRecord {
    uint8_t pad[0x18];
    uint32_t hostIp;   // offset 0x18: 32-bit IP in network byte order
    uint16_t hostPort; // offset 0x1c: 16-bit Port (3658)
    uint16_t pad2;
    uint32_t flags;    // offset 0x20
};
#pragma pack(pop)
static LanConnectRecord gCoopConnectRecord{};

void BuildNativeCoopBrowserKey(const char* name, char (&key)[13]) {
    size_t length=0;
    // Native browser names are capped at 12 bytes and used as printf formats.
    // Keep discovery identity in SessionCore; render '%' as a harmless '_'.
    for(; name && name[length] && length<12; ++length)
        key[length]=name[length]=='%'?'_':name[length];
    key[length]='\0';
}

int __cdecl HookedQueryLanServers(void* ctx, void* query, char* outBuffer, int maxLen) {
#if 0
    int serverCount = gOriginalQueryLanServers(ctx, query, outBuffer, maxLen);

    static DWORD sLastQueryLogMs = 0;
    const DWORD now = GetTickCount();
    const bool shouldLog = (now - sLastQueryLogMs >= 2000);
    if (shouldLog) {
        sLastQueryLogMs = now;
    }

    if (gCoopSessionMode.load() != COOP_SESSION_INACTIVE) {
        // Player 1 (Host instance): Searching for OTHER servers.
        // Never synthesize a server for itself!
        if (!gIsSecondInstance) {
            if (shouldLog) {
                char qLog[160];
                std::snprintf(qLog, sizeof(qLog), "[SERVER_SELECT] Host instance searching: nativeCount=%d (no self-synthesis)\r\n", serverCount);
                LogMessage(qLog);
            }
            return serverCount;
        }

        // Player 2 (Client instance):
        // ONLY synthesize IF an active beacon from a remote host was received within the last 4 seconds!
        const bool hostActive = gHaveDiscoveredHost && ((now - gDiscoveredHostReceivedAtMs) < 4000);
        if (hostActive && gLatestDiscoveredHost.hostName[0] != '\0') {
            if (outBuffer && maxLen > 64) {
                const char* hostName = gLatestDiscoveredHost.hostName;
                int curRacers = (gTargetEndpoint.sin_addr.s_addr != 0 && gHavePeerSyncPkt) ? 2 : 1;
                int written = std::snprintf(outBuffer, maxLen, "%s\t%d|2\t0\n", hostName, curRacers);
                if (written > 0) {
                    serverCount = 1;
                    if (shouldLog) {
                        char logBuf[160];
                        std::snprintf(logBuf, sizeof(logBuf),
                            "[SERVER_SELECT] Client synthesized Co-op Server: '%s' (%d/2) (beacon age=%ums)\r\n",
                            hostName, curRacers, now - gDiscoveredHostReceivedAtMs);
                        LogMessage(logBuf);
                    }
                }
            }
        } else {
            if (shouldLog) {
                LogMessage("[SERVER_SELECT] Client searching: no active host beacon received. Count=0 (Searching...)\r\n");
            }
        }
    }
    return serverCount;
}

#endif
    if(!gCoopUiActive) return gOriginalQueryLanServers(ctx,query,outBuffer,maxLen);
    const auto state=CoopSnapshot();
    static int lastDiscovery=-1;
    const int discovery=state.haveDiscoveredHost?1:0;
    if(lastDiscovery!=discovery) {
        lastDiscovery=discovery;
        LogMessage(discovery?"[COOP_BROWSER] Live host found; native server row refreshed.\r\n":
            "[COOP_BROWSER] No live host beacon; browser empty.\r\n");
    }
    if(!state.haveDiscoveredHost || !outBuffer || maxLen<64) {
        if(outBuffer && maxLen>0) outBuffer[0]='\0';
        return 0;
    }
    char browserKey[13]{};
    BuildNativeCoopBrowserKey(state.discoveredHostName,browserKey);
    int length=std::snprintf(outBuffer,maxLen,"%s\t1|2\t0\n",browserKey);
    return length>0 && length<maxLen?1:0;
}

void* __cdecl HookedGetServerConnectRecord(void* ctx, void* query, const char* serverName) {
#if 0
    void* rec = gOriginalGetServerConnectRecord(ctx, query, serverIndex);
    if (gCoopSessionMode.load() != COOP_SESSION_INACTIVE) {
        std::memset(&gCoopConnectRecord, 0, sizeof(gCoopConnectRecord));
        if (gIsSecondInstance || std::strcmp(gPeerAddress, "127.0.0.1") == 0) {
            gCoopConnectRecord.hostIp = inet_addr("127.0.0.1");
        } else if (gTargetEndpoint.sin_addr.s_addr != 0 && gTargetEndpoint.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
            gCoopConnectRecord.hostIp = gTargetEndpoint.sin_addr.s_addr;
        } else {
            gCoopConnectRecord.hostIp = inet_addr(gPeerAddress);
        }
        gCoopConnectRecord.hostPort = 9900; // Real LAN server.dll port!
        if (!rec) {
            rec = &gCoopConnectRecord;
        } else {
            auto* pLanRec = reinterpret_cast<LanConnectRecord*>(rec);
            pLanRec->hostIp = gCoopConnectRecord.hostIp;
            pLanRec->hostPort = 9900;
        }

        char ipStr[32]{};
        inet_ntop(AF_INET, &gCoopConnectRecord.hostIp, ipStr, sizeof(ipStr));

        // Directly populate BOTH LAN and Online connection buffers in SPEED2.EXE
        // so native connect routines have valid Co-op host targets whichever branch is taken:
        std::strncpy(reinterpret_cast<char*>(0x007FBDDC), ipStr, 31);
        *reinterpret_cast<uint16_t*>(0x007FBE04) = 9900;
        std::strncpy(reinterpret_cast<char*>(0x007FBDBC), ipStr, 31);
        *reinterpret_cast<uint16_t*>(0x007FBE00) = 9900;

        // Force GameMode to LAN (0x200)
        *reinterpret_cast<uint32_t*>(0x00850078) = 0x200;

        char logBuf[160];
        std::snprintf(logBuf, sizeof(logBuf), "[CONNECT_REC] Client connect record provided: Target IP=%s, Port=9900\r\n", ipStr);
        LogMessage(logBuf);
    }
    return rec;
}

#endif
    if(!gCoopUiActive) return gOriginalGetServerConnectRecord(ctx,query,serverName);
    auto state=CoopSnapshot();
    if(!serverName || !state.haveDiscoveredHost || !state.discoveredHost.Valid()) return nullptr;
    // Native parser at 4F314A caps the displayed key at twelve characters.
    char key[13]{}, expected[13]{};
    if(!ReadGameMemory(reinterpret_cast<uintptr_t>(serverName), key, sizeof(key))) return nullptr;
    BuildNativeCoopBrowserKey(state.discoveredHostName,expected);
    // Short names only initialize through their terminator, not all 13 bytes.
    if(std::strncmp(key,expected,sizeof(key))!=0) return nullptr;
    std::memset(&gCoopConnectRecord,0,sizeof(gCoopConnectRecord));
    gCoopConnectRecord.hostIp=state.discoveredHost.address;
    gCoopConnectRecord.hostPort=9900;
    return &gCoopConnectRecord;
}

bool InstallServerSelectHooks() {
    auto* callSite1 = reinterpret_cast<BYTE*>(0x004F307B);
    DWORD oldProtect = 0;
    if (VirtualProtect(callSite1, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        const intptr_t relative = reinterpret_cast<intptr_t>(&HookedQueryLanServers) - (0x004F307B + 5);
        callSite1[0] = 0xE8;
        const int32_t jump = static_cast<int32_t>(relative);
        std::memcpy(callSite1 + 1, &jump, sizeof(jump));
        FlushInstructionCache(GetCurrentProcess(), callSite1, 5);
        DWORD unused = 0;
        VirtualProtect(callSite1, 5, oldProtect, &unused);
    }

    auto* callSite2 = reinterpret_cast<BYTE*>(0x004F31EF);
    if (VirtualProtect(callSite2, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        const intptr_t relative = reinterpret_cast<intptr_t>(&HookedGetServerConnectRecord) - (0x004F31EF + 5);
        callSite2[0] = 0xE8;
        const int32_t jump = static_cast<int32_t>(relative);
        std::memcpy(callSite2 + 1, &jump, sizeof(jump));
        FlushInstructionCache(GetCurrentProcess(), callSite2, 5);
        DWORD unused = 0;
        VirtualProtect(callSite2, 5, oldProtect, &unused);
    }

    LogMessage("[HOOK] Server Select hooks installed at 0x004F307B & 0x004F31EF (Co-op server synthesis active)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Winsock LAN Discovery Hooks (0x007833E0 bind, 0x007833F0 sendto):
// 1. Forces SO_REUSEADDR and SO_BROADCAST on every socket before bind.
// 2. Fallback to dynamic port if port 9999 returns WSAEACCES (10013).
// 3. Duplicates LAN discovery broadcast packets to 127.0.0.1:9999 and the peer IP.
// ---------------------------------------------------------------------------
using SendToFn = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using ConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);

static BindFn gOriginalBind = nullptr;
static SendToFn gOriginalSendTo = nullptr;
static ConnectFn gOriginalConnect = nullptr;

int WSAAPI HookedConnect(SOCKET s, const sockaddr* name, int namelen) {
    if(!gCoopUiActive) return gOriginalConnect?gOriginalConnect(s,name,namelen):connect(s,name,namelen);
    if (name && name->sa_family == AF_INET && namelen >= sizeof(sockaddr_in)) {
        sockaddr_in sin = *reinterpret_cast<const sockaddr_in*>(name);
        const uint16_t originalPort = ntohs(sin.sin_port);

        char originalIpStr[32]{};
        inet_ntop(AF_INET, &sin.sin_addr, originalIpStr, sizeof(originalIpStr));

        bool redirected = false;

        // In Co-op mode, ensure all lobby connection attempts target the Co-op host:
        if (gCoopSessionMode.load() != COOP_SESSION_INACTIVE) {
            const bool isClient = gIsSecondInstance || (gCoopSessionMode.load() == COOP_SESSION_CLIENT) ||
                                  (*reinterpret_cast<uint8_t*>(0x008500c7) == 0);
            if (isClient) {
                // Client instance: redirect any connect attempt (such as port 1 or wrong IP) to the Host port 9900!
                sin.sin_port = htons(9900);
                if (gIsSecondInstance || std::strcmp(gPeerAddress, "127.0.0.1") == 0) {
                    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
                } else if (gTargetEndpoint.sin_addr.s_addr != 0 && gTargetEndpoint.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
                    sin.sin_addr.s_addr = gTargetEndpoint.sin_addr.s_addr;
                } else {
                    sin.sin_addr.s_addr = inet_addr(gPeerAddress);
                }
                redirected = true;
            } else {
                // Host instance connecting to its own server: ensure loopback if 0.0.0.0
                if (sin.sin_addr.s_addr == INADDR_ANY || sin.sin_addr.s_addr == 0) {
                    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
                    redirected = true;
                }
            }
        } else {
            // SP / Inactive fallback
            if (sin.sin_addr.s_addr == INADDR_ANY || sin.sin_addr.s_addr == 0) {
                sin.sin_addr.s_addr = inet_addr("127.0.0.1");
                redirected = true;
            }
        }

        char finalIpStr[32]{};
        inet_ntop(AF_INET, &sin.sin_addr, finalIpStr, sizeof(finalIpStr));
        const uint16_t finalPort = ntohs(sin.sin_port);

        int res = gOriginalConnect ? gOriginalConnect(s, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin))
                                  : connect(s, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin));

        char logBuf[256];
        if (redirected) {
            std::snprintf(logBuf, sizeof(logBuf),
                "[NET_LAN] connect(sock=%u, target=%s:%u [REDIRECTED from %s:%u]) -> res=%d (err=%d)\r\n",
                static_cast<unsigned>(s), finalIpStr, finalPort, originalIpStr, originalPort, res, (res != 0) ? WSAGetLastError() : 0);
        } else {
            std::snprintf(logBuf, sizeof(logBuf),
                "[NET_LAN] connect(sock=%u, target=%s:%u) -> res=%d (err=%d)\r\n",
                static_cast<unsigned>(s), finalIpStr, finalPort, res, (res != 0) ? WSAGetLastError() : 0);
        }
        LogMessage(logBuf);
        return res;
    }
    return gOriginalConnect ? gOriginalConnect(s, name, namelen) : connect(s, name, namelen);
}

int WSAAPI HookedBind(SOCKET s, const sockaddr* name, int namelen) {
    if(!gCoopUiActive) return gOriginalBind?gOriginalBind(s,name,namelen):bind(s,name,namelen);
    if (name && name->sa_family == AF_INET) {
        BOOL reuse = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        BOOL bcast = TRUE;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));
    }

    int res = gOriginalBind ? gOriginalBind(s, name, namelen) : bind(s, name, namelen);
    if (name && name->sa_family == AF_INET) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(name);
        uint16_t port = ntohs(sin->sin_port);

        // Fallback for WSAEACCES (10013) on port 9999: bind to ephemeral port so socket stays valid!
        if (res == SOCKET_ERROR && WSAGetLastError() == WSAEACCES && port == 9999) {
            sockaddr_in altSin = *sin;
            altSin.sin_port = htons(0); // dynamic port
            res = gOriginalBind ? gOriginalBind(s, reinterpret_cast<const sockaddr*>(&altSin), namelen)
                                : bind(s, reinterpret_cast<const sockaddr*>(&altSin), namelen);
            if (res == 0) {
                LogMessage("[NET_LAN] bind fallback to dynamic port succeeded (bypassed WSAEACCES 10013)!\r\n");
            }
        }

        uint16_t actualPort = port;
        sockaddr_in boundAddr{};
        int boundLen = sizeof(boundAddr);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&boundAddr), &boundLen) == 0) {
            actualPort = ntohs(boundAddr.sin_port);
        }

        char logBuf[160];
        std::snprintf(logBuf, sizeof(logBuf), "[NET_LAN] bind(sock=%u, reqPort=%u, actualPort=%u) -> res=%d (err=%d)\r\n",
            static_cast<unsigned>(s), port, actualPort, res, res == SOCKET_ERROR ? WSAGetLastError() : 0);
        LogMessage(logBuf);
    }
    return res;
}

int WSAAPI HookedSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
    if(!gCoopUiActive) return gOriginalSendTo?gOriginalSendTo(s,buf,len,flags,to,tolen):sendto(s,buf,len,flags,to,tolen);
    int res = gOriginalSendTo ? gOriginalSendTo(s, buf, len, flags, to, tolen) : sendto(s, buf, len, flags, to, tolen);

    if (to && to->sa_family == AF_INET) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(to);
        uint16_t port = ntohs(sin->sin_port);
        // LAN discovery port in NFSU2
        if (port == 9999) {
            // Forward discovery packet to 127.0.0.1:9999
            sockaddr_in loopback{};
            loopback.sin_family = AF_INET;
            loopback.sin_port = htons(9999);
            loopback.sin_addr.s_addr = inet_addr("127.0.0.1");
            if (sin->sin_addr.s_addr != loopback.sin_addr.s_addr) {
                if (gOriginalSendTo) {
                    gOriginalSendTo(s, buf, len, flags, reinterpret_cast<const sockaddr*>(&loopback), sizeof(loopback));
                }
            }

            // Also forward to configured peer IP
            if (gTargetEndpoint.sin_addr.s_addr != 0 &&
                gTargetEndpoint.sin_addr.s_addr != inet_addr("127.0.0.1") &&
                sin->sin_addr.s_addr != gTargetEndpoint.sin_addr.s_addr) {
                sockaddr_in peerTarget{};
                peerTarget.sin_family = AF_INET;
                peerTarget.sin_port = htons(9999);
                peerTarget.sin_addr = gTargetEndpoint.sin_addr;
                if (gOriginalSendTo) {
                    gOriginalSendTo(s, buf, len, flags, reinterpret_cast<const sockaddr*>(&peerTarget), sizeof(peerTarget));
                }
            }
        }
    }
    return res;
}

bool InstallWinsockLanDiscoveryHooks() {
    auto* pBindIat = reinterpret_cast<BindFn*>(0x007833E0);
    auto* pSendToIat = reinterpret_cast<SendToFn*>(0x007833F0);

    DWORD oldProtect = 0;
    if (VirtualProtect(pBindIat, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        gOriginalBind = *pBindIat;
        *pBindIat = &HookedBind;
        DWORD unused = 0;
        VirtualProtect(pBindIat, sizeof(void*), oldProtect, &unused);
    }

    if (VirtualProtect(pSendToIat, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        gOriginalSendTo = *pSendToIat;
        *pSendToIat = &HookedSendTo;
        DWORD unused = 0;
        VirtualProtect(pSendToIat, sizeof(void*), oldProtect, &unused);
    }

    auto* pConnectIat = reinterpret_cast<ConnectFn*>(0x007833E4);
    if (VirtualProtect(pConnectIat, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        gOriginalConnect = *pConnectIat;
        *pConnectIat = &HookedConnect;
        DWORD unused = 0;
        VirtualProtect(pConnectIat, sizeof(void*), oldProtect, &unused);
    }

    HMODULE hServer = GetModuleHandleA("server.dll");
    if (hServer) {
        InstallServerDllBindHook(hServer);
    }

    // Patch 0x00588690: je 0x5886a5 (74 13) -> 90 90 (NOP NOP)
    // Prevents lobby connection routine from ever branching to defunct EA Online server path!
    // Forces native SPEED2.EXE to ALWAYS execute the LAN connect branch (0x00588692)!
    // Keep native Online/LAN selection byte-for-byte; COOP joins use the verified UI callback.
#if 0
    auto* pJeBranch = reinterpret_cast<BYTE*>(0x00588690);
    if (VirtualProtect(pJeBranch, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        pJeBranch[0] = 0x90;
        pJeBranch[1] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), pJeBranch, 2);
        DWORD unused = 0;
        VirtualProtect(pJeBranch, 2, oldProtect, &unused);
        LogMessage("[HOOK] Patched 0x00588690 (je 0x5886a5 -> NOP NOP): Forced LAN lobby connection branch!\r\n");
    }

#endif
    LogMessage("[HOOK] Winsock LAN discovery hooks installed; native mode branch retained.\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Lobby Start Career Hook (0x004BC71C):
// In UI_OLMAIN.fng, 0x004BC6F0 is the "Select Car" click handler.
// At 0x004BC71C, it normally calls 0x00537980 to push UI_CarSelect.fng.
// In Co-op mode, if peer is present, host clicks "START THE CAREER" and launches Co-op Career!
// ---------------------------------------------------------------------------
#if 0 // Replaced by ticketed, acknowledged native career-menu selection.
void StartCoopCareerFromLobby() {
    LogMessage("[COOP] Starting Co-op Career from Lobby!\r\n");
    // Cleanly tear down native LAN/Online engine state and server.dll (nullptr skips UI transition)
    reinterpret_cast<void(__cdecl*)(void*)>(0x004A0140)(nullptr);
    *reinterpret_cast<uint32_t*>(0x00850078) = 1;    // Career mode flag (1 = Career)
    *reinterpret_cast<uint8_t*>(0x0089E7D8) = 0;     // Clear online race flag
    *reinterpret_cast<uint8_t*>(0x00836324) = 0;     // Crib active = false
    *reinterpret_cast<uint8_t*>(0x0085EF6C) = 0;     // Disable pre-race camera cutscene for Free Roam
    *reinterpret_cast<uint8_t*>(0x0085EF6D) = 0;     // Disable post-race camera cutscene for Free Roam

    // Initialize Career Free Roam ActiveEvent (0xDD60E402)
    void* eventObj = reinterpret_cast<void*(__cdecl*)(uint32_t)>(0x005003F0)(0xDD60E402);
    if (eventObj) {
        reinterpret_cast<void(__thiscall*)(void*, void*)>(0x005410E0)(reinterpret_cast<void*>(0x0085AD40), eventObj);
    }

    if (gGameSocket != INVALID_SOCKET) {
        RaceEventPacket pkt{};
        pkt.magic = kRaceEventMagic;
        pkt.version = kRacePacketVersion;
        pkt.eventType = RACE_EVENT_START;
        pkt.gameMode = 0;
        pkt.trackId = 4000;
        pkt.senderPlayer = gIsSecondInstance ? 2 : 1;
        for (int i = 0; i < 3; ++i) {
            sendto(gGameSocket, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
                reinterpret_cast<const sockaddr*>(&gTargetEndpoint), sizeof(gTargetEndpoint));
            sockaddr_in loopbackClient{};
            loopbackClient.sin_family = AF_INET;
            loopbackClient.sin_port = htons(48931);
            loopbackClient.sin_addr.s_addr = inet_addr("127.0.0.1");
            sendto(gGameSocket, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
                reinterpret_cast<const sockaddr*>(&loopbackClient), sizeof(loopbackClient));
        }
    }

    *reinterpret_cast<uint8_t*>(0x00863484) = 0;     // Standard native Career spawn

    // Launch Career Free Roam via native engine launcher (0x005404A0)
    reinterpret_cast<void(*)()>(0x005404A0)();
}

#endif
void __cdecl HookedSelectCarCall(void* screen, const char* screenName, int flags) {
#if 0
    if (gCoopSessionMode.load() != COOP_SESSION_INACTIVE) {
        if (!gIsSecondInstance && gCoopSessionMode.load() != COOP_SESSION_CLIENT) {
            const DWORD nowTick = GetTickCount();
            if (!gHaveDiscoveredClient || (nowTick - gDiscoveredClientReceivedAtMs > 5000)) {
                LogMessage("[LOBBY] Host clicked START THE CAREER but 2nd player is not connected yet!\r\n");
                SetOverlayText("[ CO-OP ] Player 2 has not joined the lobby yet. Please wait...");
                return;
            }
            LogMessage("[LOBBY] START THE CAREER clicked in CO-OP Main! Launching Co-op Career from Lobby...\r\n");
            StartCoopCareerFromLobby();
            return;
        } else {
            LogMessage("[LOBBY] Client clicked START THE CAREER in CO-OP Main; waiting for host.\r\n");
            SetOverlayText("[ CO-OP ] Waiting for the Host to start the career...");
            return;
        }
    }
    // Vanilla behavior: push UI_CarSelect.fng
    reinterpret_cast<void(__cdecl*)(void*, const char*, int)>(0x00537980)(screen, screenName, flags);
}

#endif
    if(gCoopUiActive) {
        PushNativeCoopScreen("PC_OL_Lobby.fng");
        return;
    }
    reinterpret_cast<void(__cdecl*)(void*,const char*,int)>(0x537980)(screen,screenName,flags);
}

bool InstallLobbyStartCareerHook() {
    auto* callSite = reinterpret_cast<BYTE*>(0x004BC71C);
    DWORD oldProtect = 0;
    if (!VirtualProtect(callSite, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x004BC71C for Lobby Start Career.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedSelectCarCall) - (0x004BC71C + 5);
    callSite[0] = 0xE8; // CALL
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(callSite + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), callSite, 5);
    DWORD unused = 0;
    VirtualProtect(callSite, 5, oldProtect, &unused);
    LogMessage("[HOOK] Lobby Start Career hooked at 0x004BC71C (Real Select Car handler active)\r\n");
    return true;
}



// ---------------------------------------------------------------------------
// Lobby 'Create Game' Button Blocker (0x004FCE9E):
// In PC_OL_Lobby.fng, clicking 'Create Game' normally pushes UI_QRModeSelect.fng.
// In Co-op mode, we block this click so it never conflicts with START THE CAREER!
// ---------------------------------------------------------------------------
void __cdecl DispatchNativeCoopLobbyButton(void* lobby,uint32_t hash) {
    uint32_t selected=0xffffffff;
    ReadGameMemory(reinterpret_cast<uintptr_t>(lobby)+0x118,&selected,sizeof(selected));
    const bool knownButton=hash==0x626eb826 || hash==0xd4d9d042 ||
        hash==0x14b76941 || hash==0x68cf7b18;
    char message[128]{};
    std::snprintf(message,sizeof(message),"[LOBBY_CLICK] focusHash=%08X selected=%u -> %s\r\n",
        hash,selected,!knownButton || selected>1?"IGNORED":selected==1?"CAREER":"CLOSE");
    LogMessage(message);
    // 51CD60 provides PACKAGE FOCUS, not the identity of a clicked button.
    // Native confirmations route every recognized button by the selected index.
    if(!knownButton) return;
    if(selected==1) StartCoopCareerFromLobby();
    else if(selected==0) CloseNativeCoopSession();
}

__declspec(naked) void HookedLobbyCreateGameClick() {
    __asm {
        cmp byte ptr [gCoopUiActive], 0
        je vanilla_lobby_button
        cmp eax, 0xd4d9d042
        je coop_lobby_button
        cmp eax, 0x626eb826
        je coop_lobby_button
        cmp eax, 0x14b76941
        je coop_lobby_button
        cmp eax, 0x68cf7b18
        jne vanilla_lobby_button
    coop_lobby_button:
        // Keep native selection semantics; focused label metadata can lag behind.
        pushad
        push eax
        push esi
        call DispatchNativeCoopLobbyButton
        add esp, 8
        popad
        push 0x004FD3F2
        ret
    vanilla_lobby_button:
        cmp eax, 0x626eb826
        jne not_create_game
        push 0x004FCEEC
        ret

    not_create_game:
        cmp eax, 0xd4d9d042
        push 0x004FCEAA
        ret
    }
}

bool InstallLobbyCreateGameHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x004FCE9E);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x004FCE9E for Lobby Create Game hook.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedLobbyCreateGameClick) - (0x004FCE9E + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90;
    hook[6] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), hook, 7);
    DWORD unused = 0;
    VirtualProtect(hook, 7, oldProtect, &unused);
    LogMessage("[HOOK] Lobby Create Game blocked at 0x004FCE9E (No quick race conflict)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Lobby 'Join Game' / START THE CAREER Click Hook (0x004FCF02):
// In PC_OL_Lobby.fng, clicking Button 1 normally executes 0x0049EDD0.
// In Co-op mode, Button 1 is "START THE CAREER"!
// When clicked, Host launches Co-op Career directly from the Lobby!
// ---------------------------------------------------------------------------
void __cdecl OnLobbyJoinButtonClick() {
#if 0
    if (gCoopSessionMode.load() != COOP_SESSION_INACTIVE) {
        if (!gIsSecondInstance && gCoopSessionMode.load() != COOP_SESSION_CLIENT) {
            LogMessage("[LOBBY] START THE CAREER clicked inside PC_OL_Lobby! Launching Co-op Career from Lobby...\r\n");
            StartCoopCareerFromLobby();
        } else {
            LogMessage("[LOBBY] Client clicked START THE CAREER inside PC_OL_Lobby; waiting for host.\r\n");
        }
    }
}

#endif
    if(gCoopUiActive) StartCoopCareerFromLobby();
}

__declspec(naked) void HookedLobbyJoinClickTrampoline() {
    __asm {
        cmp dword ptr [gCoopSessionMode], 0
        je vanilla_join

        pushad
        call OnLobbyJoinButtonClick
        popad
        push 0x004FD3F2
        ret

    vanilla_join:
        mov ecx, esi
        mov eax, 0x0049EDD0
        call eax
        push 0x004FD3F2
        ret
    }
}

bool InstallLobbyJoinClickHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x004FCF02);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x004FCF02 for Lobby Join Click hook.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedLobbyJoinClickTrampoline) - (0x004FCF02 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90;
    hook[6] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), hook, 7);
    DWORD unused = 0;
    VirtualProtect(hook, 7, oldProtect, &unused);
    LogMessage("[HOOK] Lobby Join Click hooked at 0x004FCF02 (Button 1 launches START THE CAREER in Co-op mode)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// RaceFlow Cleanup Null-Guard (0x0060A9A0):
// In GameFlow::SwitchTrack, the engine calls 0x0060A9A0 with ecx = [0x008900D8] (RaceFlow).
// In menus / front_end, [0x008900D8] is NULL, which causes 0x0060AA50 to crash reading [0+0x40].
// This null-guard ensures that if ecx == 0, the function returns immediately.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedRaceFlowCleanup() {
    __asm {
        test ecx, ecx
        jz rf_null_exit
        push ebx
        push esi
        push edi
        mov edi, ecx
        push 0x0060A9A5
        ret
    rf_null_exit:
        ret
    }
}

bool InstallRaceFlowCleanupGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x0060A9A0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x0060A9A0 for RaceFlow cleanup null-guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedRaceFlowCleanup) - (0x0060A9A0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), hook, 5);
    DWORD unused = 0;
    VirtualProtect(hook, 5, oldProtect, &unused);
    LogMessage("[HOOK] RaceFlow cleanup null-guard installed at 0x0060A9A0 (Shield against 0x0060AA50 crash)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Waypoint Distance Query Null-Guard (0x005E68D0):
// In SPEED2.EXE, 0x005E68D0 is called by AI traffic / waypoint navigation.
// It assumes ecx is a valid object pointer and reads [ecx + 0xC + 4] at 0x005E68DE.
// When called from uninitialized or menu-to-track transitions, ecx is derived from NULL (e.g. 0x000005C4),
// causing a crash reading 0x000005D4.
// This null-guard safely returns 0 if ecx < 0x10000.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedWaypointDistanceQuery() {
    __asm {
        cmp ecx, 0x10000
        jb wp_null_exit

        push esi
        push edi
        mov edi, ecx
        mov ecx, dword ptr [esp + 0x0C]
        push 0x005E68D8
        ret

    wp_null_exit:
        xor eax, eax
        ret 8
    }
}

bool InstallWaypointNullGuardHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x005E68D0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x005E68D0 for Waypoint null-guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedWaypointDistanceQuery) - (0x005E68D0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    hook[6] = 0x90; // NOP
    hook[7] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 8);
    DWORD unused = 0;
    VirtualProtect(hook, 8, oldProtect, &unused);
    LogMessage("[HOOK] Waypoint distance query null-guard installed at 0x005E68D0 (Shield against 0x005E68DE crash)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Traffic Update Query Null-Guard (0x005FBBE0):
// In SPEED2.EXE, 0x005FBBE0 is called by 0x0060A017 during AI updates.
// If ecx is NULL or < 0x10000, calling it passes NULL + 0x5C4 to 0x005E68D0.
// This null-guard safely returns if ecx < 0x10000.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedTrafficUpdateQuery() {
    __asm {
        cmp ecx, 0x10000
        jb traf_null_exit

        push ecx
        fld dword ptr [esp + 8]
        push esi
        push 0x005FBBE6
        ret

    traf_null_exit:
        ret 4
    }
}

bool InstallTrafficUpdateNullGuardHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x005FBBE0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x005FBBE0 for Traffic update null-guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedTrafficUpdateQuery) - (0x005FBBE0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 6);
    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    LogMessage("[HOOK] Traffic update null-guard installed at 0x005FBBE0 (Shield against AI traffic crash)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// AI Traffic Waypoint Check Null-Guard (0x005F6CD0):
// In SPEED2.EXE, 0x005F6CD0 is called by 0x00609FF5.
// If ecx < 0x10000, this guard safely returns immediately.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedAITrafficWaypointCheck() {
    __asm {
        cmp ecx, 0x10000
        jb ai_null_exit

        push ecx
        push esi
        push edi
        mov edi, ecx
        push 0x005F6CD5
        ret

    ai_null_exit:
        ret
    }
}

bool InstallAITrafficNullGuardHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x005F6CD0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x005F6CD0 for AI traffic waypoint null-guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedAITrafficWaypointCheck) - (0x005F6CD0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), hook, 5);
    DWORD unused = 0;
    VirtualProtect(hook, 5, oldProtect, &unused);
    LogMessage("[HOOK] AI traffic waypoint null-guard installed at 0x005F6CD0\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// AIManager Update Guard (0x00609CE0):
// In SPEED2.EXE, 0x00609CE0 is called by the main frame tick (0x00581407).
// During track loading (flowState == 7) or menus (flowState == 3), world AI
// and traffic structures are unallocated, causing null dereferences.
// This guard ensures AIManager::Update ONLY runs when actively in-game (flowState == 6).
// ---------------------------------------------------------------------------
__declspec(naked) void HookedAIManagerUpdate() {
    __asm {
        // MSVC inline assembler treats a bare numeric MOV operand as an
        // immediate address. ds: forces the actual game-flow memory load.
        mov eax, dword ptr ds:[0x008654A4] // kGameFlowState
        cmp eax, 6                      // Only in-game!
        jne skip_ai_update

        cmp ecx, 0x10000
        jb skip_ai_update

        push ebp
        mov ebp, esp
        sub esp, 0x0C
        push 0x00609CE6
        ret

    skip_ai_update:
        ret 4
    }
}

bool InstallAIManagerUpdateGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x00609CE0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x00609CE0 for AIManager update guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedAIManagerUpdate) - (0x00609CE0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 6);
    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    LogMessage("[HOOK] AIManager update guard installed at 0x00609CE0 (Shield against loading screen AI crash)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Waypoint Path Query Guard (0x005E72E0):
// In SPEED2.EXE, 0x005E72E0 reads [edi + 8] where edi == ecx at 0x005E7301.
// If ecx < 0x10000 (e.g. 0x000005C4), it crashes reading 0x000005CC.
// This guard safely returns 0.0f if ecx < 0x10000.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedWaypointPathQuery() {
    __asm {
        cmp ecx, 0x10000
        jb wp_path_null_exit

        sub esp, 0x234
        push 0x005E72E6
        ret

    wp_path_null_exit:
        fldz
        ret 0x10
    }
}

bool InstallWaypointPathQueryGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x005E72E0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x005E72E0 for Waypoint path query guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedWaypointPathQuery) - (0x005E72E0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 6);
    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    LogMessage("[HOOK] Waypoint path query guard installed at 0x005E72E0 (Shield against 0x005E7301 crash)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Waypoint Path Search Guard (0x005E7E80):
// In SPEED2.EXE, 0x005E7E80 calls 0x005E72E0.
// If ecx < 0x10000, returns 0 cleanly.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedWaypointPathSearch() {
    __asm {
        cmp ecx, 0x10000
        jb wp_search_null_exit

        push ebp
        mov ebp, esp
        and esp, 0xfffffff0
        push 0x005E7E86
        ret

    wp_search_null_exit:
        xor eax, eax
        ret 4
    }
}

bool InstallWaypointPathSearchGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x005E7E80);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x005E7E80 for Waypoint path search guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedWaypointPathSearch) - (0x005E7E80 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 6);
    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    LogMessage("[HOOK] Waypoint path search guard installed at 0x005E7E80\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Waypoint Nav Update Guard (0x005E6FA0):
// If ecx < 0x10000, returns cleanly.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedWaypointNavUpdate() {
    __asm {
        cmp ecx, 0x10000
        jb wp_nav_null_exit

        sub esp, 0x0C
        push ebx
        mov ebx, dword ptr [esp + 0x14]
        push 0x005E6FA8
        ret

    wp_nav_null_exit:
        ret 0x0C
    }
}

bool InstallWaypointNavUpdateGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x005E6FA0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x005E6FA0 for Waypoint nav update guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedWaypointNavUpdate) - (0x005E6FA0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    hook[6] = 0x90; // NOP
    hook[7] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 8);
    DWORD unused = 0;
    VirtualProtect(hook, 8, oldProtect, &unused);
    LogMessage("[HOOK] Waypoint nav update guard installed at 0x005E6FA0\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Game Manager Teardown Field 3C Guard (0x00480F4F):
// In SPEED2.EXE, 0x00480EB0 is called when exiting/switching flow states.
// At 0x00480F4F, it reads [ebp + 0x3c] and calls [edx + 8] at 0x00480F52.
// When transitioning from menus/lobby directly into Career Free Roam, field 0x3C is NULL,
// causing a crash reading 0x00000000.
// This guard checks if field 0x3C >= 0x10000 before dereferencing.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedGameManagerTeardownField3C() {
    __asm {
        cmp ebp, 0x10000
        jb skip_teardown_field3c

        mov ecx, dword ptr [ebp + 0x3c]
        cmp ecx, 0x10000
        jb skip_teardown_field3c

        mov edx, dword ptr [ecx]
        call dword ptr [edx + 8]

    skip_teardown_field3c:
        push 0x00480F57
        ret
    }
}

bool InstallGameManagerTeardownField3CGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x00480F4F);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x00480F4F for GameManager teardown field 3C guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedGameManagerTeardownField3C) - (0x00480F4F + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    hook[6] = 0x90; // NOP
    hook[7] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 8);
    DWORD unused = 0;
    VirtualProtect(hook, 8, oldProtect, &unused);
    LogMessage("[HOOK] GameManager teardown field 3C guard installed at 0x00480F4F (Shield against 0x00480F52 crash)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Game Manager Cleanup Field 3C Guard (0x0048144C):
// In SPEED2.EXE, 0x00481440 is called during race cleanup.
// At 0x0048144C, it reads [esi + 0x3c] and unconditionally calls [eax + 8] at 0x00481451.
// If field 0x3C is NULL, it crashes reading 0x00000000.
// This guard checks if field 0x3C >= 0x10000 before dereferencing.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedGameManagerCleanupField3C() {
    __asm {
        cmp esi, 0x10000
        jb skip_cleanup_field3c

        mov ecx, dword ptr [esi + 0x3c]
        cmp ecx, 0x10000
        jb skip_cleanup_field3c

        mov eax, dword ptr [ecx]
        call dword ptr [eax + 8]

    skip_cleanup_field3c:
        push 0x00481454
        ret
    }
}

bool InstallGameManagerCleanupField3CGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x0048144C);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x0048144C for GameManager cleanup field 3C guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedGameManagerCleanupField3C) - (0x0048144C + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    hook[6] = 0x90; // NOP
    hook[7] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 8);
    DWORD unused = 0;
    VirtualProtect(hook, 8, oldProtect, &unused);
    LogMessage("[HOOK] GameManager cleanup field 3C guard installed at 0x0048144C\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Game Manager Dispatch Field 3C Guard (0x004814E0):
// In SPEED2.EXE, 0x004814E0 jumps to 0x004797B0 using [ecx + 0x3c].
// If ecx or [ecx + 0x3c] is NULL, it crashes in 0x004797B0.
// This guard safely returns if ecx or field 0x3C < 0x10000.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedGameManagerDispatchField3C() {
    __asm {
        mov eax, dword ptr ds:[0x7faaa8]
        test eax, eax
        je exit_dispatch_field3c

        cmp ecx, 0x10000
        jb exit_dispatch_field3c

        mov ecx, dword ptr [ecx + 0x3c]
        cmp ecx, 0x10000
        jb exit_dispatch_field3c

        push 0x004797B0
        ret

    exit_dispatch_field3c:
        ret 4
    }
}

bool InstallGameManagerDispatchField3CGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x004814E0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x004814E0 for GameManager dispatch field 3C guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedGameManagerDispatchField3C) - (0x004814E0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), hook, 5);
    DWORD unused = 0;
    VirtualProtect(hook, 5, oldProtect, &unused);
    LogMessage("[HOOK] GameManager dispatch field 3C guard installed at 0x004814E0\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Game Manager Subsystem Field 3C Guard (0x004792D0):
// In SPEED2.EXE, 0x004792D0 jumps to 0x00469DE0 using [ecx + 0x3c].
// If ecx or [ecx + 0x3c] < 0x10000, it crashes reading 0x24.
// This guard safely returns if ecx or field 0x3C < 0x10000.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedGameManagerSubsystemField3C() {
    __asm {
        mov eax, dword ptr ds:[0x7faaa8]
        test eax, eax
        je exit_subsystem_field3c

        cmp ecx, 0x10000
        jb exit_subsystem_field3c

        mov ecx, dword ptr [ecx + 0x3c]
        cmp ecx, 0x10000
        jb exit_subsystem_field3c

        push 0x00469DE0
        ret

    exit_subsystem_field3c:
        ret 4
    }
}

bool InstallGameManagerSubsystemField3CGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x004792D0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x004792D0 for GameManager subsystem field 3C guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedGameManagerSubsystemField3C) - (0x004792D0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), hook, 5);
    DWORD unused = 0;
    VirtualProtect(hook, 5, oldProtect, &unused);
    LogMessage("[HOOK] GameManager subsystem field 3C guard installed at 0x004792D0\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Game Manager Iterate Field 3C Guard (0x00482970):
// In SPEED2.EXE, 0x00482970 iterates the linked list head at [ecx + 0x3c].
// If [ecx + 0x3c] is NULL, it crashes reading [0].
// This guard returns cleanly if ecx or [ecx + 0x3c] < 0x10000.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedGameManagerIterField3C() {
    __asm {
        cmp ecx, 0x10000
        jb null_iter_field3c

        mov eax, dword ptr [ecx + 0x3c]
        cmp eax, 0x10000
        jb null_iter_field3c

        sub esp, 0x10
        push ebx
        mov ebx, ecx
        push 0x00482976
        ret

    null_iter_field3c:
        ret 4
    }
}

bool InstallGameManagerIterField3CGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x00482970);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x00482970 for GameManager iter field 3C guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedGameManagerIterField3C) - (0x00482970 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 6);
    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    LogMessage("[HOOK] GameManager iter field 3C guard installed at 0x00482970\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// World Entity Enum Field 3C Guard (0x0046ABC2):
// In SPEED2.EXE, 0x0046ABC2 has a logic bug: "add ebx, 0x24; je 0x46aded".
// When field 0x3C is NULL, 0 + 0x24 != 0, so it fails the null check and
// crashes reading [0x00000024] at 0x0046ABCE.
// This guard jumps directly to 0x0046ADED if field 0x3C < 0x10000.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedWorldEntityEnumField3C() {
    __asm {
        mov ebx, dword ptr [eax + 0x3C]
        cmp ebx, 0x10000
        jb skip_enum_field3c

        add ebx, 0x24
        push 0x0046ABCE
        ret

    skip_enum_field3c:
        push 0x0046ADED
        ret
    }
}

bool InstallWorldEntityEnumField3CGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x0046ABC2);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x0046ABC2 for World Entity Enum field 3C guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedWorldEntityEnumField3C) - (0x0046ABC2 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 6);
    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    LogMessage("[HOOK] World entity enum field 3C guard installed at 0x0046ABC2\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Entity Lookup Field 3C Guard (0x0046A687):
// In SPEED2.EXE, 0x0046A687 reads [eax + 0x3c] into edi and reads [edi + 0x24].
// If field 0x3C is NULL, it crashes reading [0x00000024].
// This guard returns false (0) cleanly if GameManager or field 0x3C < 0x10000.
// ---------------------------------------------------------------------------
__declspec(naked) void HookedEntityLookupField3C() {
    __asm {
        mov eax, dword ptr ds:[0x0082B884]
        cmp eax, 0x10000
        jb null_lookup_exit

        mov eax, dword ptr [eax + 0x3C]
        cmp eax, 0x10000
        jb null_lookup_exit

        mov eax, dword ptr ds:[0x0082B884]
        push ebp
        push esi
        push edi
        push 0x0046A68F
        ret

    null_lookup_exit:
        xor al, al
        pop ebx // Native 0x0046A676 saved EBX before this mid-function hook.
        ret
    }
}

bool InstallEntityLookupField3CGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x0046A687);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x0046A687 for Entity lookup field 3C guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedEntityLookupField3C) - (0x0046A687 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    hook[6] = 0x90; // NOP
    hook[7] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 8);
    DWORD unused = 0;
    VirtualProtect(hook, 8, oldProtect, &unused);
    LogMessage("[HOOK] Entity lookup field 3C guard installed at 0x0046A687\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// FxManager / Particle / Flare Engine Crash Shields:
// When transitioning from menus/lobby to Free Roam (flowState 3 -> 7 -> 4),
// 3D world visual FX nodes (0x8357E0) contain unallocated or freed memory blocks
// filled with 0xAAAAAAAA.
// 1. FxManager::Update Guard (0x00492E10): Skips FX updates during loading (4) and transitions (7).
// 2. FxManager::UnlinkNode Guard (0x004932DB): Validates prev/next pointers, preventing 0xAAAAAAAA write crashes.
// 3. FxManager::EmitterUnlink Guard (0x00493349): Validates emitter prev/next pointers.
// 4. FxManager::ResetActiveNodes Guard (0x00491E20): Validates active nodes when clearing the manager.
// ---------------------------------------------------------------------------
alignas(16) static BYTE gTrampolineFxManagerUpdate[32] = {};

void __cdecl HandleFxManagerUpdate(void* pThis, float dt) {
    DWORD flowState = 0;
    ReadGameMemory(kGameFlowState, &flowState, sizeof(flowState));
    // During loading (4), transition (7), or any non-simulation state,
    // FX particles / flares should NOT update!
    if (flowState != 6 && flowState != 3) {
        return;
    }

    __try {
        reinterpret_cast<void(__thiscall*)(void*, float)>(reinterpret_cast<uintptr_t>(gTrampolineFxManagerUpdate))(pThis, dt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Silently shield against any particle / emitter corrupt pointer crashes
    }
}

__declspec(naked) void HookedFxManagerUpdateNaked() {
    __asm {
        push dword ptr [esp + 4] // dt
        push ecx                 // pThis
        call HandleFxManagerUpdate
        add esp, 8
        ret 4
    }
}

bool InstallFxManagerUpdateGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x00492E10);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x00492E10 for FxManager update guard.\r\n");
        return false;
    }

    DWORD trampProtect = 0;
    VirtualProtect(gTrampolineFxManagerUpdate, sizeof(gTrampolineFxManagerUpdate), PAGE_EXECUTE_READWRITE, &trampProtect);
    std::memcpy(gTrampolineFxManagerUpdate, hook, 6);
    gTrampolineFxManagerUpdate[6] = 0xE9; // JMP
    const intptr_t jmpBack = static_cast<intptr_t>(0x00492E16) - (reinterpret_cast<intptr_t>(gTrampolineFxManagerUpdate + 6) + 5);
    const int32_t jumpBackOffset = static_cast<int32_t>(jmpBack);
    std::memcpy(gTrampolineFxManagerUpdate + 7, &jumpBackOffset, sizeof(jumpBackOffset));
    FlushInstructionCache(GetCurrentProcess(), gTrampolineFxManagerUpdate, sizeof(gTrampolineFxManagerUpdate));

    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedFxManagerUpdateNaked) - (0x00492E10 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 6);
    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);
    LogMessage("[HOOK] FxManager update guard installed at 0x00492E10 (Shield against loading screen FX crash)\r\n");
    return true;
}

__declspec(naked) void HookedFxUnlinkNodeNaked() {
    __asm {
        mov eax, dword ptr [esi + 4] // eax = prev

        // Validate eax (prev)
        cmp eax, 0x10000
        jb skip_bad_unlink
        cmp eax, 0x7FFE0000
        jae skip_bad_unlink
        cmp eax, 0xAAAAAAAA
        je skip_bad_unlink

        // Validate edi (next)
        cmp edi, 0x10000
        jb skip_bad_unlink
        cmp edi, 0x7FFE0000
        jae skip_bad_unlink
        cmp edi, 0xAAAAAAAA
        je skip_bad_unlink

        // Both pointers valid: unlink cleanly
        mov dword ptr [eax], edi
        mov dword ptr [edi + 4], eax
        push 0x004932E3
        ret

    skip_bad_unlink:
        // Skip invalid writes and node pooling.
        // If edi is also corrupted, break out of this sub-list loop safely!
        cmp edi, 0x10000
        jb exit_sublist
        cmp edi, 0x7FFE0000
        jae exit_sublist
        cmp edi, 0xAAAAAAAA
        je exit_sublist

        push 0x004932F9 // advance to next node
        ret

    exit_sublist:
        push 0x00493309 // break to next emitter list
        ret
    }
}

bool InstallFxUnlinkNodeGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x004932DB);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x004932DB for Fx unlink node guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedFxUnlinkNodeNaked) - (0x004932DB + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP
    hook[6] = 0x90; // NOP
    hook[7] = 0x90; // NOP
    FlushInstructionCache(GetCurrentProcess(), hook, 8);
    DWORD unused = 0;
    VirtualProtect(hook, 8, oldProtect, &unused);
    LogMessage("[HOOK] Fx unlink node guard installed at 0x004932DB (Shield against 0xAAAAAAAA crash)\r\n");
    return true;
}

__declspec(naked) void HookedFxEmitterUnlinkNaked() {
    __asm {
        mov ecx, dword ptr [ebx + 4]
        mov eax, dword ptr [ebx]

        cmp ecx, 0x10000
        jb skip_emitter_unlink
        cmp ecx, 0x7FFE0000
        jae skip_emitter_unlink
        cmp ecx, 0xAAAAAAAA
        je skip_emitter_unlink

        cmp eax, 0x10000
        jb skip_emitter_unlink
        cmp eax, 0x7FFE0000
        jae skip_emitter_unlink
        cmp eax, 0xAAAAAAAA
        je skip_emitter_unlink

        mov dword ptr [ecx], eax
        mov dword ptr [eax + 4], ecx
        push 0x00493353
        ret

    skip_emitter_unlink:
        push 0x00493369 // skip unlinking and free call, jump to next emitter
        ret
    }
}

bool InstallFxEmitterUnlinkGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x00493349);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 10, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x00493349 for Fx emitter unlink guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedFxEmitterUnlinkNaked) - (0x00493349 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90;
    hook[6] = 0x90;
    hook[7] = 0x90;
    hook[8] = 0x90;
    hook[9] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), hook, 10);
    DWORD unused = 0;
    VirtualProtect(hook, 10, oldProtect, &unused);
    LogMessage("[HOOK] Fx emitter unlink guard installed at 0x00493349\r\n");
    return true;
}

__declspec(naked) void HookedFxResetUnlinkNaked() {
    __asm {
        mov ecx, dword ptr [esi + 4]
        mov eax, dword ptr [esi]

        cmp ecx, 0x10000
        jb reset_bad_list
        cmp ecx, 0x7FFE0000
        jae reset_bad_list
        cmp ecx, 0xAAAAAAAA
        je reset_bad_list

        cmp eax, 0x10000
        jb reset_bad_list
        cmp eax, 0x7FFE0000
        jae reset_bad_list
        cmp eax, 0xAAAAAAAA
        je reset_bad_list

        mov dword ptr [ecx], eax
        mov dword ptr [eax + 4], ecx
        push 0x00491E2A
        ret

    reset_bad_list:
        mov dword ptr [edi], edi // Mark list as empty: [edi] = edi
        push 0x00491E46          // jump to pop edi; pop esi; ret
        ret
    }
}

bool InstallFxResetUnlinkGuard() {
    auto* hook = reinterpret_cast<BYTE*>(0x00491E20);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 10, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x00491E20 for Fx reset unlink guard.\r\n");
        return false;
    }
    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedFxResetUnlinkNaked) - (0x00491E20 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90;
    hook[6] = 0x90;
    hook[7] = 0x90;
    hook[8] = 0x90;
    hook[9] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), hook, 10);
    DWORD unused = 0;
    VirtualProtect(hook, 10, oldProtect, &unused);
    LogMessage("[HOOK] Fx reset unlink guard installed at 0x00491E20\r\n");
    return true;
}

// ============================================================================
// World Sound Subsystem Guard (0x004793D0, 0x0047946B, 0x004794F5):
// In WorldSubsystem[0x74]::Update (0x004793D0), the engine updates 3D sound
// tracking relative to the player car ([esi + 0xE4]).
// During track loading (flowState 4), the player car entity has not spawned yet
// and [esi + 0xE4] is NULL (0x00000000). The engine forgets to check for NULL
// and dereferences ecx at 0x00479471 and 0x004794FB, crashing with 0xC0000005!
// 1. Prologue Guard at 0x004793D0: If [ecx + 0xE4] is NULL or invalid, returns ret 4.
// 2. Call-Site Guard at 0x0047946B: If [esi + 0xE4] is NULL/invalid, sets eax = 0.
// 3. Call-Site Guard at 0x004794F5: If [esi + 0xE4] is NULL/invalid, sets eax = 0.
// ============================================================================

__declspec(naked) void HookedWorldSoundSubsystemUpdateNaked() {
    __asm {
        // Skip audio evaluation during loading (4) and unloading (7)
        cmp dword ptr ds:[0x8654A4], 4
        je skip_sound_update
        cmp dword ptr ds:[0x8654A4], 7
        je skip_sound_update

        // ecx is 'this' (WorldSubsystem[0x74])
        test ecx, ecx
        jz skip_sound_update
        cmp ecx, 0x10000
        jb skip_sound_update
        cmp ecx, 0x7FFE0000
        jae skip_sound_update

        // Validate player vehicle entity [ecx + 0xE4]
        mov eax, dword ptr [ecx + 0xE4]
        test eax, eax
        jz skip_sound_update
        cmp eax, 0x10000
        jb skip_sound_update
        cmp eax, 0x7FFE0000
        jae skip_sound_update

        // Player vehicle is valid: execute original prologue (5 bytes: 51 53 56 8B F1)
        push ecx
        push ebx
        push esi
        mov esi, ecx
        push 0x004793D5
        ret

    skip_sound_update:
        ret 4
    }
}

__declspec(naked) void HookedWorldSoundQueryCar6Naked() {
    __asm {
        mov ecx, dword ptr [esi + 0xE4]
        test ecx, ecx
        jz invalid_car6
        cmp ecx, 0x10000
        jb invalid_car6
        cmp ecx, 0x7FFE0000
        jae invalid_car6

        mov edx, dword ptr [ecx]
        test edx, edx
        jz invalid_car6
        push 6
        call dword ptr [edx + 0x38]
        push 0x00479478
        ret

    invalid_car6:
        xor eax, eax
        push 0x00479478
        ret
    }
}

__declspec(naked) void HookedWorldSoundQueryCar5Naked() {
    __asm {
        mov ecx, dword ptr [esi + 0xE4]
        test ecx, ecx
        jz invalid_car5
        cmp ecx, 0x10000
        jb invalid_car5
        cmp ecx, 0x7FFE0000
        jae invalid_car5

        mov edx, dword ptr [ecx]
        test edx, edx
        jz invalid_car5
        push 5
        call dword ptr [edx + 0x38]
        push 0x00479502
        ret

    invalid_car5:
        xor eax, eax
        push 0x00479502
        ret
    }
}

bool InstallWorldSoundSubsystemGuard() {
    // 1. Prologue hook at 0x004793D0 (5 bytes)
    {
        auto* hook = reinterpret_cast<BYTE*>(0x004793D0);
        DWORD oldProtect = 0;
        if (!VirtualProtect(hook, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LogMessage("[HOOK] Failed to unprotect 0x004793D0 for WorldSoundSubsystem prologue guard.\r\n");
            return false;
        }
        const intptr_t relative = reinterpret_cast<intptr_t>(&HookedWorldSoundSubsystemUpdateNaked) - (0x004793D0 + 5);
        hook[0] = 0xE9; // JMP
        const int32_t jump = static_cast<int32_t>(relative);
        std::memcpy(hook + 1, &jump, sizeof(jump));
        FlushInstructionCache(GetCurrentProcess(), hook, 5);
        DWORD unused = 0;
        VirtualProtect(hook, 5, oldProtect, &unused);
    }

    // 2. Call-site hook at 0x0047946B (13 bytes: 5 byte JMP + 8 NOPs)
    {
        auto* hook = reinterpret_cast<BYTE*>(0x0047946B);
        DWORD oldProtect = 0;
        if (!VirtualProtect(hook, 13, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LogMessage("[HOOK] Failed to unprotect 0x0047946B for WorldSound QueryCar6 guard.\r\n");
            return false;
        }
        const intptr_t relative = reinterpret_cast<intptr_t>(&HookedWorldSoundQueryCar6Naked) - (0x0047946B + 5);
        hook[0] = 0xE9; // JMP
        const int32_t jump = static_cast<int32_t>(relative);
        std::memcpy(hook + 1, &jump, sizeof(jump));
        for (int i = 5; i < 13; ++i) {
            hook[i] = 0x90; // NOP
        }
        FlushInstructionCache(GetCurrentProcess(), hook, 13);
        DWORD unused = 0;
        VirtualProtect(hook, 13, oldProtect, &unused);
    }

    // 3. Call-site hook at 0x004794F5 (13 bytes: 5 byte JMP + 8 NOPs)
    {
        auto* hook = reinterpret_cast<BYTE*>(0x004794F5);
        DWORD oldProtect = 0;
        if (!VirtualProtect(hook, 13, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LogMessage("[HOOK] Failed to unprotect 0x004794F5 for WorldSound QueryCar5 guard.\r\n");
            return false;
        }
        const intptr_t relative = reinterpret_cast<intptr_t>(&HookedWorldSoundQueryCar5Naked) - (0x004794F5 + 5);
        hook[0] = 0xE9; // JMP
        const int32_t jump = static_cast<int32_t>(relative);
        std::memcpy(hook + 1, &jump, sizeof(jump));
        for (int i = 5; i < 13; ++i) {
            hook[i] = 0x90; // NOP
        }
        FlushInstructionCache(GetCurrentProcess(), hook, 13);
        DWORD unused = 0;
        VirtualProtect(hook, 13, oldProtect, &unused);
    }

    LogMessage("[HOOK] WorldSoundSubsystem guards installed at 0x004793D0, 0x0047946B, 0x004794F5 (Shield against 0x00479471 null vehicle crash)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Curve::Evaluate Crash Shield (0x005764D0):
// Prevents 0xC0000005 crash at 0x00576552 in Curve::Evaluate when evaluating
// vehicle audio/physics curves if [curve + 0x10] (samples pointer) is invalid (e.g. 0x88000100).
// ---------------------------------------------------------------------------
static bool __cdecl IsValidCurve(const void* pCurve) {
    if (!pCurve) return false;
    const uintptr_t uCurve = reinterpret_cast<uintptr_t>(pCurve);
    if (uCurve < 0x10000 || uCurve >= 0x7FFE0000) return false;
    if (IsBadReadPtr(pCurve, 0x14)) return false;

    const int count = *reinterpret_cast<const int*>(uCurve);
    if (count <= 0 || count > 100000) return false;

    const uintptr_t uSamples = *reinterpret_cast<const uintptr_t*>(uCurve + 0x10);
    if (uSamples < 0x10000 || uSamples >= 0x7FFE0000) return false;
    if (IsBadReadPtr(reinterpret_cast<const void*>(uSamples), sizeof(float))) return false;

    return true;
}

__declspec(naked) void SafeCurveEvaluateHook() {
    __asm {
        // ECX = this (pCurve)
        // [esp + 4] = float x
        cmp ecx, 0x10000
        jb invalid_curve
        cmp ecx, 0x7FFE0000
        jae invalid_curve

        push ecx            // Preserve ECX (this)
        push ecx            // Argument pCurve for IsValidCurve
        call IsValidCurve
        add esp, 4          // Pop argument
        pop ecx             // Restore ECX
        test al, al
        jz invalid_curve

        // Valid curve: execute original 6 displaced bytes
        push ecx
        fld dword ptr [esp + 8]
        push esi
        push 0x005764D6
        ret

    invalid_curve:
        fldz
        ret 4
    }
}

bool InstallCurveEvaluateShieldHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x005764D0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x005764D0 for Curve::Evaluate shield.\r\n");
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&SafeCurveEvaluateHook) - (0x005764D0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90; // NOP (6th byte)
    FlushInstructionCache(GetCurrentProcess(), hook, 6);
    DWORD unused = 0;
    VirtualProtect(hook, 6, oldProtect, &unused);

    LogMessage("[HOOK] Curve::Evaluate shield installed at 0x005764D0 (Prevents 0x00576552 access violation)\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Camera Cutscene Bypass & Shields (0x005012C0, 0x00445C20, 0x0044DE90):
// 1. Hook 0x005012C0: Bypass legacy co-op race route cameras, but retain the
//    native singleplayer and new-career story-camera flag (including Bayview).
// 2. Hook 0x00445C20: Null-guard against 0x00445C2C null crash if node is NULL.
// 3. Hook 0x0044DE90: Null-guard against null camera node in track sampling.
// ---------------------------------------------------------------------------
bool __fastcall HookedShouldPlayIntroCamera(void* raceCoord, void* /*edx*/) {
    // Preserve the game's NPC starter, countdown scene and SP/story cameras.
    return *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(raceCoord) + 0x1c) != 0;
}

// Native 16-byte CameraKeyframe descriptor (aligned to 16 bytes).
struct alignas(16) CameraKeyframe {
    int16_t type = 0;       // +0x00: 0 = standard bumper/chase view
    int16_t subType = 0;    // +0x02: 0 = default heading
    float posX = 0.0f;     // +0x04: World coordinate X
    float posY = 0.0f;     // +0x08: World coordinate Y
    float posZ = 0.0f;     // +0x0C: World coordinate Z
};
static CameraKeyframe gSafeDummyCameraKeyframe{};
static std::atomic<uint32_t> gCameraKeyframeShieldCount{0};
uint32_t CoopCameraShieldCount() { return gCameraKeyframeShieldCount.load(); }

// Shield 1: Root Keyframe Resolver Guard (0x005ECCA0)
// Intercepts CameraDirector::FindKeyframe(cameraIndex)
static void* __cdecl CameraDirectorFindKeyframeImpl(void* pThis, uint32_t cameraIndex) {
    if (!pThis) {
        gCameraKeyframeShieldCount++;
        return &gSafeDummyCameraKeyframe;
    }
    uintptr_t pVehicleInfo = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(pThis) + 0x0c);
    if (!pVehicleInfo) {
        gCameraKeyframeShieldCount++;
        return &gSafeDummyCameraKeyframe;
    }
    void* pTable = reinterpret_cast<void*>(pVehicleInfo + 0x720);
    void* result = nullptr;
    __asm {
        mov ecx, pTable
        push cameraIndex
        mov eax, 0x00441140
        call eax
        mov result, eax
    }
    if (!result) {
        __asm {
            mov ecx, pTable
            push 0
            mov eax, 0x00441140
            call eax
            mov result, eax
        }
    }
    if (!result) {
        gCameraKeyframeShieldCount++;
        return &gSafeDummyCameraKeyframe;
    }
    return result;
}

__declspec(naked) void HookedCameraDirectorFindKeyframe() {
    __asm {
        push dword ptr [esp + 4] // cameraIndex
        push ecx                 // pThis
        call CameraDirectorFindKeyframeImpl
        add esp, 8
        ret 4
    }
}

// Shield 2: CameraDirector::InitKeyframePosition Guard (0x00445C20)
// Defense-in-depth shield directly protecting 0x00445C2C.
// If arg1 ([esp + 4]) is NULL, replaces it with &gSafeDummyCameraKeyframe on stack.
__declspec(naked) void SafeCameraNodeEvalHook() {
    __asm {
        // [esp + 4] = arg1: CameraKeyframe*
        cmp dword ptr [esp + 4], 0
        jne valid_node

        // Null detected: substitute safe static dummy keyframe pointer
        lock inc gCameraKeyframeShieldCount
        lea eax, gSafeDummyCameraKeyframe
        mov dword ptr [esp + 4], eax

    valid_node:
        // Execute 6 displaced prologue bytes
        push ebp
        mov ebp, esp
        and esp, 0xfffffff0
        push 0x00445C26
        ret
    }
}

// Shield 3: CameraDirector::SampleTrackCamera Guard (0x0044DE90)
// Defense-in-depth shield protecting 0x0044DEAE and caller 0x0045183B.
// If arg2 ([esp + 8]) is NULL, replaces it with &gSafeDummyCameraKeyframe on stack.
__declspec(naked) void SafeCameraTrackSampleHook() {
    __asm {
        // [esp + 8] = arg2: CameraKeyframe*
        cmp dword ptr [esp + 8], 0
        jne valid_camera_node

        // Null detected: substitute safe static dummy keyframe pointer
        lock inc gCameraKeyframeShieldCount
        lea eax, gSafeDummyCameraKeyframe
        mov dword ptr [esp + 8], eax

    valid_camera_node:
        // Execute 6 displaced prologue bytes
        push ebp
        mov ebp, esp
        and esp, 0xfffffff0
        push 0x0044DE96
        ret
    }
}

bool InstallIntroCameraBypassHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x005012C0);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 16, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x005012C0 for IntroCamera bypass hook.\r\n");
        return false;
    }

    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedShouldPlayIntroCamera) - (0x005012C0 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    for (int i = 5; i < 16; ++i) {
        hook[i] = 0x90; // NOP
    }
    FlushInstructionCache(GetCurrentProcess(), hook, 16);
    DWORD unused = 0;
    VirtualProtect(hook, 16, oldProtect, &unused);

    LogMessage("[HOOK] IntroCamera bypass installed at 0x005012C0 (Bypasses Mode 0x14 on Track 4000 Free Roam)\r\n");
    return true;
}

bool InstallCameraDirectorShieldHooks() {
    // 1. Hook 0x005ECCA0 (5 bytes: 8B 44 24 04 56) - Root Keyframe Resolver
    {
        auto* hook = reinterpret_cast<BYTE*>(0x005ECCA0);
        DWORD oldProtect = 0;
        if (VirtualProtect(hook, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            const intptr_t relative = reinterpret_cast<intptr_t>(&HookedCameraDirectorFindKeyframe) - (0x005ECCA0 + 5);
            hook[0] = 0xE9; // JMP
            const int32_t jump = static_cast<int32_t>(relative);
            std::memcpy(hook + 1, &jump, sizeof(jump));
            FlushInstructionCache(GetCurrentProcess(), hook, 5);
            DWORD unused = 0;
            VirtualProtect(hook, 5, oldProtect, &unused);
        }
    }

    // 2. Hook 0x00445C20 (6 bytes: 55 8B EC 83 E4 F0) - 0x00445C2C Crash Shield
    {
        auto* hook = reinterpret_cast<BYTE*>(0x00445C20);
        DWORD oldProtect = 0;
        if (VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            const intptr_t relative = reinterpret_cast<intptr_t>(&SafeCameraNodeEvalHook) - (0x00445C20 + 5);
            hook[0] = 0xE9; // JMP
            const int32_t jump = static_cast<int32_t>(relative);
            std::memcpy(hook + 1, &jump, sizeof(jump));
            hook[5] = 0x90; // NOP
            FlushInstructionCache(GetCurrentProcess(), hook, 6);
            DWORD unused = 0;
            VirtualProtect(hook, 6, oldProtect, &unused);
        }
    }

    // 3. Hook 0x0044DE90 (6 bytes: 55 8B EC 83 E4 F0) - 0x0044DEAE Caller Shield
    {
        auto* hook = reinterpret_cast<BYTE*>(0x0044DE90);
        DWORD oldProtect = 0;
        if (VirtualProtect(hook, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            const intptr_t relative = reinterpret_cast<intptr_t>(&SafeCameraTrackSampleHook) - (0x0044DE90 + 5);
            hook[0] = 0xE9; // JMP
            const int32_t jump = static_cast<int32_t>(relative);
            std::memcpy(hook + 1, &jump, sizeof(jump));
            hook[5] = 0x90; // NOP
            FlushInstructionCache(GetCurrentProcess(), hook, 6);
            DWORD unused = 0;
            VirtualProtect(hook, 6, oldProtect, &unused);
        }
    }

    LogMessage("[HOOK] Complete Camera Director & Keyframe shields installed at 0x005ECCA0, 0x00445C20, and 0x0044DE90.\r\n");
    return true;
}



// ---------------------------------------------------------------------------
// Screen Push Hook & Singleplayer Isolation (0x00537980):
// Intercepts all PushScreen calls to track active UI screens and cleanly
// isolate Singleplayer modes whenever returning to Main Menu or entering Career.
// ---------------------------------------------------------------------------
static uint8_t gTrampolinePushScreen[16]{};

void __cdecl HookedPushScreen(void* screenManager, const char* screenName, int flags) {
#if 0 // Menu recreation no longer destroys membership or guesses role from instance number.
    if (screenName) {
        char scrBuf[160];
        std::snprintf(scrBuf, sizeof(scrBuf), "[UI_SCREEN] PushScreen: %s (flags=%d, sessionMode=%d)\r\n",
            screenName, flags, static_cast<int>(gCoopSessionMode.load()));
        LogMessage(scrBuf);
        if (std::strcmp(screenName, "UI_Main.fng") == 0) {
            // Main menu entry: isolate singleplayer
            if (gCoopSessionMode.load() != COOP_SESSION_INACTIVE) {
                LogMessage("[SP] Returning to Main Menu: resetting to Singleplayer Inactive.\r\n");
                gCoopSessionMode.store(COOP_SESSION_INACTIVE);
                *reinterpret_cast<uint8_t*>(0x0089E7D8) = 0;
                gCoopRaceState = COOP_RACE_IDLE;
                gCar1LoadedForRace = false;
                SetOverlayText("");
            }
        }
    }
    using PushScreenFn = void(__cdecl*)(void*, const char*, int);
    auto original = reinterpret_cast<PushScreenFn>(&gTrampolinePushScreen[0]);
    original(screenManager, screenName, flags);

    if (screenName && gCoopSessionMode.load() != COOP_SESSION_INACTIVE) {
        if (!gIsSecondInstance && (std::strstr(screenName, "PC_OL_Lobby.fng") || std::strstr(screenName, "UI_OLMAIN.fng") || std::strstr(screenName, "UI_OLMain.fng"))) {
            gCoopSessionMode.store(COOP_SESSION_HOST);
            LogMessage("[LOBBY] Entered Co-op Lobby screen: Active COOP_SESSION_HOST set!\r\n");
        } else if (gIsSecondInstance && (std::strstr(screenName, "PC_OL_Lobby.fng") || std::strstr(screenName, "UI_OLMAIN.fng") || std::strstr(screenName, "UI_OLMain.fng"))) {
            gCoopSessionMode.store(COOP_SESSION_CLIENT);
            LogMessage("[LOBBY] Entered Co-op Lobby screen: Active COOP_SESSION_CLIENT set!\r\n");
        }

        // On any lobby screen transition, ensure GameMode is LAN (0x200) and buffers are primed
        if (std::strstr(screenName, "UI_OLMAIN.fng") || std::strstr(screenName, "UI_OLMain.fng") ||
            std::strstr(screenName, "UI_PC_LAN.fng") || std::strstr(screenName, "PC_OL_Lobby.fng")) {
            *reinterpret_cast<uint32_t*>(0x00850078) = 0x200; // NET_MODE_LAN

            const char* targetIp = (gIsSecondInstance || std::strcmp(gPeerAddress, "127.0.0.1") == 0) ? "127.0.0.1" : gPeerAddress;
            std::strncpy(reinterpret_cast<char*>(0x007FBDDC), targetIp, 31);
            *reinterpret_cast<uint16_t*>(0x007FBE04) = 9900;
            std::strncpy(reinterpret_cast<char*>(0x007FBDBC), targetIp, 31);
            *reinterpret_cast<uint16_t*>(0x007FBE00) = 9900;
        }
    }
}

#endif
    const char* target=screenName;
    if(gCoopUiActive && screenName && gCurrentNativeScreen=="PC_OL_Lobby.fng" &&
        std::strcmp(screenName,"UI_QRModeSelect.fng")==0) {
        CloseNativeCoopSession(); return;
    }
    if(gCoopUiActive && screenName && (_stricmp(screenName,"UI_OLMAIN.fng")==0))
        target="PC_OL_Lobby.fng";
    // The caller may have passed our tracked string; preserve it before updating the tracker.
    std::string sourceCopy;
    if(screenManager==gCurrentNativeScreen.c_str()) { sourceCopy=gCurrentNativeScreen; screenManager=const_cast<char*>(sourceCopy.c_str()); }
    OnCoopNativeScreen(target);
    using PushScreenFn=void(__cdecl*)(void*,const char*,int);
    reinterpret_cast<PushScreenFn>(&gTrampolinePushScreen[0])(screenManager,target,flags);
}

bool InstallPushScreenHook() {
    auto* hook = reinterpret_cast<BYTE*>(0x00537980);
    DWORD oldProtect = 0;
    if (!VirtualProtect(hook, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogMessage("[HOOK] Failed to unprotect 0x00537980 for PushScreen hook.\r\n");
        return false;
    }

    // Build trampoline (8 bytes: mov ecx, [0x8384c4]; test ecx, ecx)
    DWORD trampProtect = 0;
    VirtualProtect(gTrampolinePushScreen, sizeof(gTrampolinePushScreen), PAGE_EXECUTE_READWRITE, &trampProtect);
    std::memcpy(gTrampolinePushScreen, hook, 8);
    gTrampolinePushScreen[8] = 0xE9; // JMP
    const intptr_t jmpBack = static_cast<intptr_t>(0x00537988) - (reinterpret_cast<intptr_t>(gTrampolinePushScreen + 8) + 5);
    const int32_t jumpBackOffset = static_cast<int32_t>(jmpBack);
    std::memcpy(gTrampolinePushScreen + 9, &jumpBackOffset, sizeof(jumpBackOffset));
    FlushInstructionCache(GetCurrentProcess(), gTrampolinePushScreen, sizeof(gTrampolinePushScreen));

    const intptr_t relative = reinterpret_cast<intptr_t>(&HookedPushScreen) - (0x00537980 + 5);
    hook[0] = 0xE9; // JMP
    const int32_t jump = static_cast<int32_t>(relative);
    std::memcpy(hook + 1, &jump, sizeof(jump));
    hook[5] = 0x90;
    hook[6] = 0x90;
    hook[7] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), hook, 8);

    DWORD unused = 0;
    VirtualProtect(hook, 8, oldProtect, &unused);
    LogMessage("[HOOK] PushScreen hooked at 0x00537980 (Screen state tracker & SP isolation active)\r\n");
    return true;
}

#include "CoopNetworkRuntime.inl"
DWORD WINAPI RunObserver(void* moduleHandle) {
    coop_crash::ReserveExceptionStack();
    wchar_t modulePath[MAX_PATH]{};
    const auto length = GetModuleFileNameW(
        static_cast<HMODULE>(moduleHandle), modulePath, MAX_PATH);

    if (length == 0 || length == MAX_PATH) {
        return 1;
    }

    wchar_t* filename = wcsrchr(modulePath, L'\\');
    if (filename == nullptr) {
        return 1;
    }
    wchar_t scriptsDirectory[MAX_PATH]{};
    lstrcpynW(scriptsDirectory, modulePath, static_cast<int>(filename - modulePath) + 2);
    WideCharToMultiByte(CP_ACP, 0, scriptsDirectory, -1,
        gScriptsDirectory, MAX_PATH, nullptr, nullptr);

    // Read scripts\NFSU2Coop.ini
    lstrcpyW(filename + 1, L"NFSU2Coop.ini");
    LoadConfiguration(modulePath);
    StartCoopModScan();

    // Initialize WinSock
    WSADATA networkData{};
    const bool networkReady = WSAStartup(MAKEWORD(2, 2), &networkData) == 0;

    // Dynamic port fallback for seamless local multi-instance
    SOCKET gameSocket = networkReady ? OpenSocket(gLocalPort, true) : INVALID_SOCKET;
    if (gameSocket == INVALID_SOCKET && networkReady && gConfiguredInstance == 0) {
        unsigned short temp = gLocalPort;
        gLocalPort = gTargetPort;
        gTargetPort = temp;
        gameSocket = OpenSocket(gLocalPort, true);
        gIsSecondInstance = true;
    }

    // Prepare distinct log file per instance: NFSU2Coop.log (Host) / NFSU2Coop_2.log (Client)
    if (gIsSecondInstance) {
        lstrcpyW(filename + 1, L"NFSU2Coop_2.log");
    } else {
        lstrcpyW(filename + 1, L"NFSU2Coop.log");
    }
    WideCharToMultiByte(CP_ACP, 0, modulePath, -1, gLogFilePath, MAX_PATH, nullptr, nullptr);

    HANDLE log = CreateFileW(modulePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        return 1;
    }
    lstrcpynW(gRuntimeCrashLogPath,modulePath,MAX_PATH);
    InterlockedExchange(&gRuntimeCrashPlayer,gIsSecondInstance?2:1);

    // Apply single-instance bypass patch
    ApplyMultiInstancePatch();
    gGameSocket = gameSocket;
    const bool connectedTransport=gGameSocket!=INVALID_SOCKET&&gCoopTransport.Open(gLocalPort);
    gCoopTransportOpened.store(connectedTransport);
    LogMessage(connectedTransport?
        "[NETWORK613] Connected ENet gameplay transport opened; discovery remains on the configured base UDP port.\r\n":
        "[NETWORK613] Gameplay transport could not bind base port + 1000. CO-OP start is blocked; singleplayer is available.\r\n");
    gTargetEndpoint.sin_family = AF_INET;
    gTargetEndpoint.sin_port = htons(gTargetPort);
    inet_pton(AF_INET, gPeerAddress, &gTargetEndpoint.sin_addr);

    const bool uiHookInstalled = InstallUiTickHook();
    InstallCoopPhysicalMotion();
    InstallCoopDualRegionHook();
    InstallCoopAIPlacementHooks();
    LogMessage(CoopWorldContactNative::Install()
        ? "[AI_CONTACT] Verified Client AI replica native-body/contact adapter ready.\r\n"
        : "[AI_CONTACT] Native signature/placement guard mismatch; contact activation disabled safely.\r\n");
    LogMessage(InstallCoopWorldImpact()
        ? "[AI_IMPACT] Verified two-witness contact relay and prediction ready.\r\n"
        : "[AI_IMPACT] Solver signatures differ; contact relay disabled safely.\r\n");
    if (gCoopFleetEnabled || gCoopAIEnabled) {
        const bool assetsInstalled=InstallCoopFleetAssetsHooks();
        const bool worldInstalled=!gCoopFleetEnabled || (assetsInstalled && InstallCoopFleetWorldNative());
        if (gCoopFleetEnabled && !worldInstalled) {
            gCoopFleetEnabled=false;
            LogMessage(assetsInstalled?
                "[FLEET] World adapter verification failed; configured WorldFleet=1 disabled safely.\r\n":
                "[FLEET] Asset adapter verification failed; configured WorldFleet=1 disabled safely.\r\n");
        }
    }
    ConfigureCoopProfileContext();
    InstallCollisionPatch();
    LogMessage(CoopContactRosterNative::Install()
        ? "[CONTACT] Verified COOP empty contact-registry guard installed.\r\n"
        : "[ERROR] Contact-registry signature differs; guard not installed.\r\n");
    InstallCarUpdateCrashPatch();
    InstallCarDespawnPatch();
    InstallCollisionWitnessPatch();
    InstallKeyboardFocusHook();
    const bool spawnerHookInstalled = InstallFreeRoamCarSpawnHook();
    SetCoopRenderBranch(false); // preserve vanilla SP until an isolated COOP career is selected
    InstallStage4CarLoaderHook();
    InstallTrackLoadingReadyCheck();
    InstallWheelAngularPositionHook();
    InstallProxyWheelMatrixHook();
    InstallSkidEffectHook();
    InstallPeerMapHooks();
    CoopRearRenderTrace::Install();
    InstallRaceLoadingLoopHook();
    InstallTable1DStoreHook();
    InstallRideInfoRandomizerPatch();
    InstallAiDriverHook();
    // Fleet's atomic scanner gates include the peer exclusion. Do not overwrite
    // its refresh gate with the older peer-only patch at the same address.
    if (!CoopFleetScannerNativeReady()) InstallWorldOpponentScanHook();
    // Leave 428635 native: the old untyped guard accessed full-driver fields
    // on dormant 0x54-byte objects and could itself write beyond allocation.
    InstallPartAnimationCrashShield();
    InstallStringHashCrashShield();
    InstallTrackMapCrashShield();
    InstallSplitScreenCrashShield();
    const bool raceIntentReady = InstallRaceTriggerHook();
    const bool restartIntentReady = InstallNativeRaceRestartHook();
    const bool countdownReady = InstallCountdownSyncHook();
    const bool introBarrierReady = InstallCoopRaceIntroBarrier();
    const bool consentAdvanceReady = InstallCoopRaceConsentNativeHooks();
    const bool finishReady=InstallCoopRaceFinishHooks() && InstallCoopDriftHooks();
    const bool urlReady=finishReady && InstallCoopUrlTournamentHooks();
    gRaceFinishHooksReady = finishReady && urlReady && countdownReady && introBarrierReady && raceIntentReady && restartIntentReady && consentAdvanceReady;
    if (!SetCoopFleetOutrunNativeSort(reinterpret_cast<uintptr_t>(CoopRaceFinishNative::gSort)))
        LogMessage("[FLEET_OUTRUN] Verified native scorer trampoline unavailable; admission stays closed.\r\n");
    CoopResultsTrace::Install();
    CoopResultsFocusNative::Install();
    CoopResultsTrace::gCaptureReader=[]()->uint32_t{return (CoopAnnouncements::Expanded()?1u:0u)|(CoopAnnouncements::CapturesKeyboard()?2u:0u);};
    InstallCoopSponsorHooks();
    InstallCoopVisualGate();
    InstallCompareRacersHook();
    InstallRaceFlowCrashShield();
    InstallRaceFlowHandlerGuard();
    InstallRaceFlowCleanupGuard();
    InstallWaypointNullGuardHook();
    InstallTrafficUpdateNullGuardHook();
    InstallAITrafficNullGuardHook();
    InstallAIManagerUpdateGuard();
    InstallWaypointPathQueryGuard();
    InstallWaypointPathSearchGuard();
    InstallWaypointNavUpdateGuard();
    InstallGameManagerTeardownField3CGuard();
    InstallGameManagerCleanupField3CGuard();
    InstallGameManagerDispatchField3CGuard();
    InstallGameManagerSubsystemField3CGuard();
    InstallGameManagerIterField3CGuard();
    InstallWorldEntityEnumField3CGuard();
    InstallEntityLookupField3CGuard();
    InstallFxManagerUpdateGuard();
    InstallFxUnlinkNodeGuard();
    InstallFxEmitterUnlinkGuard();
    InstallFxResetUnlinkGuard();
    InstallWorldSoundSubsystemGuard();
    InstallCurveEvaluateShieldHook();
    InstallIntroCameraBypassHook();
    InstallCameraDirectorShieldHooks();
    InstallSavePathRedirectionHooks();
    InstallCoopSaveCompletionHooks();
    const bool specialReady=ConfigureCoopSpecialDriver();
    LogMessage(specialReady?"[SPECIAL] All-event native controller ready.\r\n":"[SPECIAL] Native verification failed; shared career entry remains disabled.\r\n");
    InstallMainMenuCoopButtonHook();
    InstallMainMenuLanButtonHook();
    InstallStringTranslationHook();
    InstallPeerTextureAllocationHooks();
    InstallServerNameCaptureHook();
    InstallPushScreenHook();
    InstallLobbyStartCareerHook();
    InstallLobbyCreateGameHook();
    InstallLobbyCreateGameNodeHideHook();
    InstallLobbyJoinClickHook();
    InstallServerSelectHooks();
    InstallWinsockLanDiscoveryHooks();
    const bool nativeLobbyReady=InstallNativeCoopLobbyHook(CoopUiSnapshot);
    gCoopCareerHooksReady=InstallNativeCoopCareerHooks() && InstallCoopCareerOfferHooks() && InstallCoopMovieSyncHooks() && nativeLobbyReady && specialReady;
    CoopFrameTrace::Install();
    InstallCoopLivePauseHook();
    InstallStrcmpCrashShield();
    // 0x5B3390 is a physics/contact distance function, not a renderer.
    // Hooking it as void RenderBody discarded its x87 float return and could
    // corrupt the car. Keep it untouched; setup is complete before streaming.
    const char* hookStatus = uiHookInstalled
        ? "[INFO] In-game observer & transform injector ready: press F10 for HUD.\r\n[INFO] Dedicated vehicle spawner, collision witness & despawn immunity shields active.\r\n"
        : "[WARN] In-game observer hook was not installed.\r\n";
    const char* initialMessage = gIsSecondInstance
        ? "[OK] Undercoop: NFSU2 " UNDERCOOP_BUILD_VERSION " Beta Pre-release loaded (Instance 2)\r\n[INFO] Z console, Enter chat, Tab channel. F5 accepts and F6 declines race, sponsor or special-event proposals.\r\n"
        : "[OK] Undercoop: NFSU2 " UNDERCOOP_BUILD_VERSION " Beta Pre-release loaded (Instance 1)\r\n[INFO] Z console, Enter chat, Tab channel. F5 accepts and F6 declines race, sponsor or special-event proposals.\r\n";
    DWORD bytesWritten = 0;
    SetFilePointer(log, 0, nullptr, FILE_END); // installers append their diagnostic lines
    WriteFile(log, initialMessage, static_cast<DWORD>(std::strlen(initialMessage)), &bytesWritten, nullptr);
    WriteFile(log, hookStatus, static_cast<DWORD>(std::strlen(hookStatus)), &bytesWritten, nullptr);
    LogMessage(gCoopAIEnabled?
        "[AI_WORLD] Experimental RaceAI=1: native event opponents, host motion/progress/results.\r\n":
        "[AI_WORLD] Experimental RaceAI=0: established two-human gameplay retained; world inventory is read-only.\r\n");
    LogMessage(gCoopFleetEnabled ?
        "[FLEET] WorldFleet=1: verified pooled traffic/ambient manifests, host authority and dual-player interest enabled.\r\n" :
        "[FLEET] Shared traffic/ambient rollout inactive; see configuration and adapter checks above.\r\n");

    char netLog[256]{};
    if (gameSocket != INVALID_SOCKET) {
        std::snprintf(netLog, sizeof(netLog),
            "[INFO] UDP Socket bound on port %u -> Target %s:%u (Instance %s)\r\n",
            gLocalPort, gPeerAddress, gTargetPort, gIsSecondInstance ? "2 (Client)" : "1 (Host)");
    } else {
        std::snprintf(netLog, sizeof(netLog), "[WARN] UDP socket could not be opened.\r\n");
    }
    WriteFile(log, netLog, static_cast<DWORD>(std::strlen(netLog)), &bytesWritten, nullptr);
    CloseHandle(log);

    gGameSocket = gameSocket;
    gTargetEndpoint.sin_family = AF_INET;
    gTargetEndpoint.sin_port = htons(gTargetPort);
    inet_pton(AF_INET, gPeerAddress, &gTargetEndpoint.sin_addr);

    // Allow game initialization
    Sleep(2500);

    uint32_t sequence = 0;
    uint32_t lastMotionPublication = 0;
    uint32_t lastLoggedShieldCount = 0;
    DWORD previousFlowState = 0xFFFFFFFF;
    DWORD coopLoadStartedAt = 0;
    bool coopLoadMeasured = false;
    for (;;) {
        // Immediate log when collision shield intercepts a null witness
        if (gCollisionWitnessInterceptCount != lastLoggedShieldCount) {
            char shieldLine[160]{};
            std::snprintf(shieldLine, sizeof(shieldLine),
                "[SHIELD] Intercepted null collision witness in narrow-phase! Crash prevented (total intercepts=%lu)\r\n",
                static_cast<unsigned long>(gCollisionWitnessInterceptCount));
            HANDLE appendLog = CreateFileW(modulePath, FILE_APPEND_DATA, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (appendLog != INVALID_HANDLE_VALUE) {
                WriteFile(appendLog, shieldLine, static_cast<DWORD>(std::strlen(shieldLine)),
                    &bytesWritten, nullptr);
                CloseHandle(appendLog);
            }
            lastLoggedShieldCount = gCollisionWitnessInterceptCount;
        }

        const DWORD flowState=gPublishedGameFlow.load(std::memory_order_acquire);

        // Immediate log when game flow state changes
        if (flowState!=UINT32_MAX) {
            if (flowState != previousFlowState) {
                char flowLog[160]{};
                std::snprintf(flowLog, sizeof(flowLog),
                    "[FLOW] State transition: %s (%lu) -> %s (%lu)\r\n",
                    DescribeFlowState(previousFlowState), static_cast<unsigned long>(previousFlowState),
                    DescribeFlowState(flowState), static_cast<unsigned long>(flowState));
                LogMessage(flowLog);
                if (IsCoopGameplayActive() && (flowState == 4 || flowState == 5) && !coopLoadMeasured) {
                    coopLoadStartedAt=GetTickCount();coopLoadMeasured=true;
                } else if (coopLoadMeasured && flowState == 6) {
                    char loadingTime[128]{};const uint32_t loadedTrack=gPublishedGameTrack.load(std::memory_order_acquire);
                    std::snprintf(loadingTime,sizeof(loadingTime),"[LOAD_TIMING] track=%u total native/network load=%lu ms.\r\n",
                        loadedTrack,static_cast<unsigned long>(GetTickCount()-coopLoadStartedAt));
                    LogMessage(loadingTime);coopLoadMeasured=false;
                } else if (flowState == 3) coopLoadMeasured=false;
                previousFlowState = flowState;
            }
        }

        // Process queued heartbeats before checking membership deadlines.
        // Raw UDP stays limited to discovery and membership.
        char rxBuf[4096]{};
        sockaddr_in senderAddr{};
        for (int drained = 0; gGameSocket != INVALID_SOCKET && drained < 256; ++drained) {
            int senderLen = sizeof(senderAddr);
            const int bytes = recvfrom(gGameSocket, rxBuf, sizeof(rxBuf), 0,
                reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);
            if (bytes == SOCKET_ERROR) break;
            uint32_t magic = 0;
            if (bytes >= 4) std::memcpy(&magic, rxBuf, 4);
            if (magic == coop::kSessionMagic)
                ReceiveCoopSessionPacket(rxBuf, bytes, senderAddr, GetTickCount());
            else
                ++gCoopRawGameplayRejected;
        }
        const DWORD nowTick = GetTickCount();

        CoopDiscordPresence::Poll(nowTick);
        TickCoopSessionNetwork(nowTick); // persists across main-menu/SP screen changes
        TickCoopModNetwork(nowTick);
        SendLatestCoopHumanMotion(sequence,lastMotionPublication,nowTick);
        TickCoopRaceNetwork(nowTick);
        TickCoopUrlNetwork(nowTick);
        TickCoopChatNetwork(nowTick);
        TickCoopMovieNetwork(nowTick);
        TickCoopSponsorNetwork(nowTick);
        TickCoopVisualNetwork(nowTick);
        TickCoopSpecialNetwork(nowTick);
        TickCoopSpecialRecoveryNetwork(nowTick);
        TickCoopWorldImpactNetwork(nowTick);
        if(gCoopAIEnabled) TickCoopAIRosterNetwork(nowTick);
        if(gCoopAIEnabled) TickCoopAIWorldNetwork(nowTick);
        if(gCoopFleetEnabled) TickCoopFleetAssetsNetwork(nowTick);
        if(gCoopFleetEnabled) TickCoopFleetWorldNetwork(nowTick);
        TickCoopConnectedTransport(GetTickCount());
        DrainCoopConnectedTransport(GetTickCount());
        gCoopTransport.Poll(GetTickCount());
        TraceCoopNetworkBudget(GetTickCount());
        TraceCoopNetworkState(GetTickCount());

        Sleep(16); // 60 Hz tick rate
    }
    return 0;
}

} // namespace

static HMODULE gCoopStartupModule=nullptr;
static volatile LONG gCoopStartupOnce=0;
static void StartCoopObserver(HMODULE module){
        StartCoopCrashReporter(module);
        CoopStartupIntro::Initialize(module);
        CoopAnnouncements::Initialize(module);
        CoopPeerLabelOverlay::Initialize(module);

        SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
        HANDLE worker = CreateThread(nullptr, 0, RunObserver, module, 0, nullptr);
        if (worker != nullptr) {
            CloseHandle(worker);
        }
}

extern "C" __declspec(dllexport) void InitializeASI(){
#ifdef UNDERCOOP_MOD_MANAGER
    if(InterlockedCompareExchange(&gCoopStartupOnce,1,0)!=0)return;
    // The tested loader calls this synchronously after LoadLibrary returns,
    // outside DllMain. Do not start our asynchronous observer before selection.
    wchar_t executable[MAX_PATH]{};DWORD n=GetModuleFileNameW(nullptr,executable,MAX_PATH);
    if(!n||n>=MAX_PATH)ExitProcess(ERROR_BAD_PATHNAME);
    std::wstring root(executable,n);root.resize(root.find_last_of(L'\\'));
    if(GetPrivateProfileIntW(L"GlobalSets",L"DontLoadFromDllMain",0,(root+L"\\global.ini").c_str())!=1)return;
    StartCoopCrashReporter(gCoopStartupModule);
    if(!CoopModManager::Show(gCoopStartupModule,root)){
        if(!CoopModManager::state.error.empty())MessageBoxW(nullptr,CoopModManager::state.error.c_str(),L"Undercoop Mod Manager",MB_OK|MB_ICONERROR);
        ExitProcess(0);
    }
    // Bind BOTH native single-player and COOP to this pack before the game
    // initializes its save device; never let addon garages reach stock saves.
    static std::string spRootName=coop_mod_scan::Utf8(CoopModManager::state.spFolder);
    const char* oldName=nullptr;char expected[18]{};
    const char* newName=spRootName.c_str();
    const bool original=ReadGameMemory(0x7F66A0,&oldName,4)&&oldName&&
        ReadGameMemory(reinterpret_cast<uintptr_t>(oldName),expected,sizeof(expected))&&!std::memcmp(expected,"NFS Underground 2",18);
    const auto& folder=CoopModManager::state.saveFolder;
    const auto ansi=coop_mod_scan::Utf8(folder);
    if(!original||ansi.size()>=std::size(coop_build::kCoopSaveFolder)||folder.size()>=std::size(coop_build::kCoopSaveFolderWide)||
        !SafeWriteBytes(0x7F66A0,&newName,4)){
        MessageBoxW(nullptr,L"Cannot isolate this pack's save domain. The game will not start.",L"Undercoop",MB_OK|MB_ICONERROR);ExitProcess(ERROR_INVALID_DATA);
    }
    std::strcpy(coop_build::kCoopSaveFolder,ansi.c_str());std::wcscpy(coop_build::kCoopSaveFolderWide,folder.c_str());
    StartCoopObserver(gCoopStartupModule);
#endif
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);gCoopStartupModule=module;
#ifndef UNDERCOOP_MOD_MANAGER
        StartCoopObserver(module);
#endif
    }
    return TRUE;
}
