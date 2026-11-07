// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// RF0 message4 is the stock advance-to-loading notification, not a harmless
// idle frame. BeginSwitch consent alone cannot hold RF0: its next message4
// calls 603630(1), allowing RF1's stale substate5 to prepare a Free Roam NIS.
// No scene-condition or winner-pointer instructions are changed here.
namespace CoopRaceConsentNative {
static RaceFlowHandlerFn gOriginal=reinterpret_cast<RaceFlowHandlerFn>(0x6037C0);
static bool gInstalled=false;
static uintptr_t gSlot=0x80214C,gEntry=0x6037C0,gAdvance=0x60380B;
static constexpr BYTE kEntry[]={0x55,0x8B,0xEC,0x56,0x8B,0xF1,0x8B,0x4D,0x08};
static constexpr BYTE kAdvance[]={0x6A,0x01,0x8B,0xCE,0xE8,0x1C,0xFE,0xFF,0xFF,
    0xB8,0x01,0x00,0x00,0x00,0x5E,0x5D,0xC2,0x08,0x00};
static bool HoldsAdvance(void* object,int message) {
    const auto& held=gRaceNativeConsentWindow;
    if(message!=4 || !IsCoopGameplayActive() || !CoopRaceHooksReady() ||
        !(RaceIntentPending() || gCoopRaceState==COOP_RACE_STARTING) ||
        !held.session || !held.epoch || held.session!=gRaceIntentSession ||
        held.session!=gPendingRaceEvent.sessionId || held.epoch!=gPendingRaceEvent.seed ||
        !gPendingRaceEvent.careerEventHash ||
        held.gameFlow!=CoopPeerRaceNative::addresses.gameFlowObject ||
        held.gameFlow!=reinterpret_cast<uintptr_t>(gRaceDeferredNativeSwitch) ||
        held.nativeFlow!=reinterpret_cast<uintptr_t>(object)) return false;
    uint32_t flow=0,state=0,track=0;uintptr_t live=0;
    const auto& address=CoopPeerRaceNative::addresses;
    // The retained window owns this exact old native world. Peer freshness is
    // deliberately irrelevant: hold it through timeout until cancellation
    // changes RF0 back through the native RF7/RF4 handler.
    return ReadGameMemory(address.gameFlow,&flow,4) && flow==6 &&
        ReadGameMemory(address.raceFlow,&live,sizeof(live)) && live==held.nativeFlow &&
        IsValidUserPtr(live) && ReadGameMemory(live,&state,4) && state==0 &&
        ReadGameMemory(address.track,&track,4) && track==held.track &&
        ((!gPendingRaceEvent.parentEpoch && track==4000) ||
         (gPendingRaceEvent.parentEpoch && gRaceBeforeInvitation==COOP_RACE_IN_PROGRESS &&
          track==gPendingRaceEvent.trackId));
}
static int __fastcall Handler(void* object,void*,int message,int argument) {
    if(HoldsAdvance(object,message)) return 1;
    return gOriginal(object,message,argument);
}
}
bool InstallCoopRaceConsentNativeHooks() {
    using namespace CoopRaceConsentNative;
    if(gInstalled) return true;
    uintptr_t original=0;BYTE entry[sizeof(kEntry)]{},advance[sizeof(kAdvance)]{};
    if(!ReadGameMemory(gSlot,&original,sizeof(original)) || original!=gEntry ||
        !ReadGameMemory(gEntry,entry,sizeof(entry)) || std::memcmp(entry,kEntry,sizeof(entry)) ||
        !ReadGameMemory(gAdvance,advance,sizeof(advance)) || std::memcmp(advance,kAdvance,sizeof(advance))) {
        LogMessage("[RACE_CONSENT] RF0 native signature mismatch; synchronized race consent disabled.\r\n");
        return false;
    }
    const uintptr_t handler=reinterpret_cast<uintptr_t>(&Handler);
    gOriginal=reinterpret_cast<RaceFlowHandlerFn>(original);
    if(!SafeWriteBytes(gSlot,&handler,sizeof(handler))) {
        LogMessage("[RACE_CONSENT] RF0 dispatch installation failed; synchronized race consent disabled.\r\n");
        return false;
    }
    gInstalled=true;
    LogMessage("[RACE_CONSENT] Verified RF0 message4 advance held for the exact pending native world until launch/cancel.\r\n");
    return true;
}
