// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Each player returns to their OWN last reachable free-roam race entrance.
// Native Career::Write/Read map 8750/8760 to saved AD60/AD70. These are
// personal fields, not shared progress. No post-spawn raw physics teleport.
namespace CoopRaceReturn {
struct Origin {
    uint64_t session=0;uint32_t epoch=0,track=0;
    float position[4]{};uint16_t heading=0;
    bool valid=false,applied=false,saved=false;
};
static Origin origin;
static Origin beforeLaunch;
static uintptr_t career=0x85AD40;
static bool Finite(const float* p) {
    for(size_t i=0;i<3;++i) if(!std::isfinite(p[i]) || std::fabs(p[i])>100000) return false;
    return true;
}
static bool Current() {
    const auto state=RaceStateSnapshot();const auto session=CoopSnapshot();
    return IsCoopGameplayActive() && session.joined && session.fresh && origin.valid && state.ready && state.token==origin.epoch &&
        state.session==origin.session && session.sessionId==origin.session && state.track==origin.track &&
        gPendingRaceEvent.sessionId==origin.session && gPendingRaceEvent.seed==origin.epoch &&
        gPendingRaceEvent.trackId==origin.track;
}
}
void CaptureCoopRaceReturnBackup() { CoopRaceReturn::beforeLaunch=CoopRaceReturn::origin; }
void RestoreCoopRaceReturnBackup() { CoopRaceReturn::origin=CoopRaceReturn::beforeLaunch; }
void CaptureCoopRaceReturnOrigin() {
    using namespace CoopRaceReturn;
    const auto session=CoopSnapshot();uint32_t flow=0,track=0;
    if(!IsCoopGameplayActive() || !session.joined || !session.fresh ||
        gPendingRaceEvent.sessionId!=session.sessionId || !gPendingRaceEvent.seed ||
        !ReadGameMemory(gRaceContextAddresses.flow,&flow,4) || flow!=6 ||
        !ReadGameMemory(gRaceContextAddresses.track,&track,4)) return;
    if(gPendingRaceEvent.parentEpoch) {
        if(origin.valid && !origin.applied && origin.session==session.sessionId &&
            origin.epoch==gPendingRaceEvent.parentEpoch && origin.track==track && track==gPendingRaceEvent.trackId)
            origin.epoch=gPendingRaceEvent.seed;
        return; // A restart must never capture a finish line in a locked region.
    }
    if(track!=4000) return;
    Origin next{};const uintptr_t local=CoopRaceCar(false);
    if(!IsValidUserPtr(local) || !ReadGameMemory(local+0x60,next.position,12) || !Finite(next.position) ||
        !ReadGameMemory(local+0x40C,&next.heading,2)) return;
    next.session=session.sessionId;next.epoch=gPendingRaceEvent.seed;next.track=gPendingRaceEvent.trackId;
    next.position[3]=1;next.valid=true;origin=next;
}
void ApplyCoopRaceReturnOrigin(uintptr_t localParameter) {
    using namespace CoopRaceReturn;
    uint32_t track=0,type=0;uint8_t remote=1;
    if(origin.applied || !Current() || !IsValidUserPtr(localParameter) ||
        !ReadGameMemory(gRaceContextAddresses.track,&track,4) || track!=4000 ||
        !ReadGameMemory(localParameter+4,&type,4) || type!=1 ||
        !ReadGameMemory(localParameter+0x900,&remote,1) || remote) return;
    const uint8_t enabled=1;
    if(!SafeWriteBytes(localParameter+0x920,origin.position,12) ||
        !SafeWriteBytes(localParameter+0x930,&origin.heading,2) ||
        !SafeWriteBytes(localParameter+0x91C,&enabled,1) ||
        !SafeWriteBytes(career+0x8750,origin.position,16) ||
        !SafeWriteBytes(career+0x8760,&origin.heading,2)) return;
    origin.applied=true;
    LogMessage("[RACE_RETURN] Own pre-race reachable origin restored through native spawn parameters; personal saved position updated.\r\n");
}
void SaveCoopRaceReturnOrigin(DWORD flow) {
    using namespace CoopRaceReturn;
    if(flow!=6 || !origin.applied || origin.saved || !Current() ||
        !CoopRaceReturnedToFreeRoam(flow,4000) || !gProfileContext.IsCoopActive()) return;
    uint32_t track=0,state=0;uintptr_t native=0;
    if(!ReadGameMemory(gRaceContextAddresses.track,&track,4) || track!=4000 ||
        !ReadGameMemory(gRaceContextAddresses.nativeFlow,&native,4) || !IsValidUserPtr(native) ||
        !ReadGameMemory(native,&state,4) || state!=4) return;
    if(CoopPersonalSave::CaptureAndQueue(GetTickCount(),true)) {
        origin.saved=true;
        LogMessage("[RACE_RETURN] Own corrected return position persisted; no peer garage/cash imported.\r\n");
    }
}
