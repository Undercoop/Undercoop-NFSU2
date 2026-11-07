// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopSession.h"
#include "CoopWorldSync.h"
#include "CoopTrafficRuntime.inl"

// Include with the game's existing anonymous-namespace declarations. All
// identifiers are values; native handles and local load tokens are never
// serialized as object addresses.
enum class FleetLoadResult { Ready, Waiting, Refused };
FleetLoadResult BeginCoopFleetIdentityAndLoadingBarrier(std::uint32_t track,
    std::uint32_t raceEpoch, bool freeRoam, std::uint64_t localLoadToken,
    bool trafficAllowed, coop_world::Identity& identity);
bool CurrentCoopFleetIdentity(coop_world::Identity& identity);
bool SealCoopFleetLoadingIdentity(std::uint64_t localGeneration);
bool NotifyCoopFleetNativeLoadReady(const coop_world::Identity&, std::uint64_t localGeneration);
void ConfigureCoopFleetWorld(const CoopTrafficRuntime::Capabilities&);
void TickCoopFleetWorld(std::uint32_t flow);
bool ReceiveCoopFleetWorldPacket(const void*, std::size_t, const coop::Endpoint&, std::uint32_t);
void TickCoopFleetWorldNetwork(std::uint32_t now);
bool ReadCoopFleetIdentity(std::uintptr_t car, std::uintptr_t& ride, std::uint32_t& kind);
bool ReadManagedCoopFleet(std::uintptr_t car, std::uintptr_t& ride, std::uint32_t& kind);
std::uint64_t CoopFleetWorldEpoch();
std::uint64_t CoopFleetLocalGeneration();
bool CoopFleetOutrunApproved(std::uintptr_t ambientCar, std::uintptr_t humanCar);
bool CoopFleetOutrunBusy(std::uintptr_t ambientCar);
void RequestCoopFleetOutrunClaim(std::uintptr_t ambientCar, std::uintptr_t humanCar);
void ReleaseCoopFleetOutrunClaim(std::uintptr_t ambientCar, std::uintptr_t humanCar);
bool IsCoopFleetOutrunActive(std::uintptr_t ambientCar, std::uintptr_t humanCar);
bool StartCoopFleetHostOutrun(std::uintptr_t ambientCar, std::uintptr_t humanCar);
bool SetCoopFleetOutrunNativeSort(std::uintptr_t address);
void TickCoopFleetOutrunNative();

// The asset owner implements these loading barriers. Reservations are the
// committed host manifest, never a guessed range of native pool IDs.
bool PrepareCoopFleetBeforeStreaming(const coop_world::Identity&, bool freeRoam);
bool CommitCoopFleetAfterHumans(const coop_world::Identity&);
bool PrepareCoopFleetPaletteBeforeStreaming(const coop_world::Identity&);
bool CoopFleetAssetsReady(const coop_world::Identity&);
bool ReadCoopFleetParameter(std::uintptr_t parameter, std::uint32_t& kind,
    std::uint8_t& nativeId, std::uint32_t& model, std::uint64_t& appearanceHash);
bool ReadCoopFleetReservation(const coop_world::Identity&, std::uintptr_t parameter,
    std::uint32_t& kind, std::uint8_t& nativeId);
bool CoopFleetReservedNativeId(const coop_world::Identity&, std::uint32_t kind, std::uint8_t nativeId);
std::uint32_t CoopFleetCommittedParameterCount();
bool ShouldPreserveCoopFleetRideSetup(std::uintptr_t ride);
bool CoopFleetSelectionPlanReady(const coop_world::Identity&);
bool CaptureCoopFleetHostSelection(const coop_world::Identity&);
bool ApplyCoopFleetClientSelection(const coop_world::Identity&);
bool CoopFleetAssetsReadyForStage7(const coop_world::Identity&);
bool NotifyCoopFleetAssetsAfterStage7(const coop_world::Identity&, std::uint64_t generation);
bool CoopFleetWaitForPeerAssets();
bool ReadCoopFleetPalettePaint(std::uintptr_t ride, std::uint8_t& paint);
bool GetCoopFleetNativeCapabilities(CoopTrafficRuntime::Capabilities&);
