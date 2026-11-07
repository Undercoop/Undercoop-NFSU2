// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// SPEED2.EXE v1.2 native finish adapters. All state/network ownership is outside
// this file; every racer lookup is for the current coordinator on the game thread.
bool CoopFinishRaceActive();
bool CoopFinishRaceInterrupted();
bool CoopFinishIsLocalRacer(uintptr_t racer);
bool CoopFinishIsPeerRacer(uintptr_t racer);
void CoopFinishCaptureLocal(uint8_t rank, uint32_t elapsedTicks, uint16_t status);
bool CoopFinishReadFrozen(uintptr_t racer, uint8_t& rank, uint32_t& ticks, uint16_t& status);
bool CoopFinishReadTeamResult(uint8_t& rank, uint32_t& ticks, uint16_t& status);
bool CoopFinishCareerEventMatches(uint32_t eventHash);
bool CoopFinishCareerRecordMatches(uintptr_t destination);
bool CoopFinishSceneScopeMatches();
void CoopFinishNativeCareerCommitted();
void CoopSyncRaceProgressBeforeSort();
bool CoopFinishIsManagedAIRacer(uintptr_t racer);
bool CoopFinishReadManagedAI(uintptr_t racer,uint8_t& rank,uint32_t& ticks,uint16_t& status);
bool CoopFinishReadTimedResult(uintptr_t racer,uint8_t& rank,uint32_t& ticks,uint16_t& status);
bool CoopFinishReadDriftResult(uintptr_t racer,uint8_t& rank,uint32_t& ticks,uint16_t& status);
bool CoopFinishWriteDriftScore(uintptr_t row,uintptr_t racer);
bool CoopFinishHasOrderedResult();
bool CoopFinishHasTimedResult();
bool IsCoopUrlTournament();
bool PrepareCoopUrlResults();
bool CoopUrlHistoryCopied(uintptr_t destination);
bool CoopUrlResultGroupBuilt();
bool CoopUrlReadCareerOutcome(uint8_t& rank);
bool ReadCoopUrlFrozenRacer(uintptr_t,uint8_t&,uint32_t&,uint16_t&);

namespace CoopRaceFinishNative {
// Defaults are native; isolated adapter tests substitute private fixture pages.
static uintptr_t kCoordinator = 0x00890118;
static uintptr_t kResultGroup = 0x0085036C;
static uintptr_t kParticipantCounts = 0x0089E7C0;
constexpr size_t kResultStride = 0x224;
constexpr uint32_t kMaximumRacers = 6;

// 606CB0 selects [this+BD0+index*4], and returns with RET 10h. 608D70 is
// only a four-argument convenience wrapper; hooking it misses finish crossings.
using FinishFn = void(__thiscall*)(void*, uint32_t, int32_t, uint32_t, uint32_t);
using AllPlayersDoneFn = int(__thiscall*)(void*);
using SortFn = void(__thiscall*)(void*);
using SerializeResultFn = void(__thiscall*)(void*, void*, uint32_t);
using EnterResultFn = void(__thiscall*)(void*);
using CareerCommitFn = void(__thiscall*)(void*, void*);
using ResultGroupFn = void(__thiscall*)(void*, void*);
using OrdinaryCommitFn = void(__thiscall*)(void*);
using CopyResultFn = void*(__thiscall*)(void*,void*);
using RenderTimeFn = void(__thiscall*)(void*,void*,uint32_t);
static FinishFn gFinish = reinterpret_cast<FinishFn>(0x00606CB0);
static AllPlayersDoneFn gAllPlayersDone = reinterpret_cast<AllPlayersDoneFn>(0x005F85B0);
static SortFn gSort = reinterpret_cast<SortFn>(0x005EEC80);
static SerializeResultFn gSerializeResult = reinterpret_cast<SerializeResultFn>(0x00515DB0);
static EnterResultFn gEnterResult = reinterpret_cast<EnterResultFn>(0x0060AAA0);
static CareerCommitFn gCareerCommit = reinterpret_cast<CareerCommitFn>(0x0054CAF0);
static ResultGroupFn gResultGroup = reinterpret_cast<ResultGroupFn>(0x0054CCA0);
static OrdinaryCommitFn gOrdinaryCommit = reinterpret_cast<OrdinaryCommitFn>(0x0054C940);
static CopyResultFn gCopyResult = reinterpret_cast<CopyResultFn>(0x00525D10);
static RenderTimeFn gRenderTime = reinterpret_cast<RenderTimeFn>(0x004A4020);
static bool gInstalled = false;
static bool gResultEntered = false;
static bool gCareerCommitArmed = false;
static bool gCareerCommitApplied = false;
static bool gCareerHistoryReady = false;
static void* gAppliedCommitObject=nullptr;
static void* gAppliedCommitResult=nullptr;
static uint32_t gAppliedCommitHash=0;
static uintptr_t gResultCoordinator = 0;
static uintptr_t gLocalResultRow = 0;
static bool gParticipantCountsHoldLogged = false;
static uintptr_t gPeerWinnerPlayerOriginal=0x00431EE9;
static uintptr_t gPeerWinnerPlayerContinue=0x00431EEF;
static bool gPeerWinnerSceneLogged=false;

static bool ReadRacer(uintptr_t coordinator, uint32_t index, uintptr_t& racer) {
    uint32_t count = 0;
    return IsValidUserPtr(coordinator) &&
        ReadGameMemory(coordinator + 0x24, &count, sizeof(count)) &&
        count > 0 && count <= kMaximumRacers && index < count &&
        ReadGameMemory(coordinator + 0xBD0 + index * 4, &racer, sizeof(racer)) &&
        IsValidUserPtr(racer);
}

static bool ReconcileParticipantCounts(uintptr_t coordinator) {
    uint32_t count=0,actual[4]{},previous[4]{};
    uintptr_t seenCars[kMaximumRacers]{},seenRacers[kMaximumRacers]{};
    bool haveLocal=false,havePeer=false;
    if(!IsValidUserPtr(coordinator) ||
        !ReadGameMemory(coordinator+0x24,&count,sizeof(count)) || count<2 || count>kMaximumRacers) return false;
    for(uint32_t index=0;index<count;++index) {
        uintptr_t racer=0,car=0,param=0,backRacer=0;
        uint32_t type=0; uint8_t remote=0;
        if(!ReadRacer(coordinator,index,racer) || !ReadGameMemory(racer,&car,sizeof(car)) ||
            !IsValidUserPtr(car) || !ReadGameMemory(car+0x1C,&backRacer,sizeof(backRacer)) || backRacer!=racer ||
            !ReadGameMemory(car+0x14,&param,sizeof(param)) || !IsValidUserPtr(param) ||
            !ReadGameMemory(param+4,&type,sizeof(type)) || (type!=1 && type!=2) ||
            !ReadGameMemory(param+0x900,&remote,sizeof(remote))) return false;
        for(uint32_t before=0;before<index;++before)
            if(seenCars[before]==car || seenRacers[before]==racer) return false;
        seenCars[index]=car; seenRacers[index]=racer;
        const bool local=CoopFinishIsLocalRacer(racer),peer=CoopFinishIsPeerRacer(racer);
        if(type==1) {
            if(local && !peer && !remote && !haveLocal) haveLocal=true;
            else if(peer && !local && remote && !havePeer) havePeer=true;
            else return false; // Never invent another native/local Player.
        } else if(local || peer) return false;
        ++actual[(remote?2:0)+(type==2?1:0)];
    }
    if(!haveLocal || !havePeer ||
        !ReadGameMemory(kParticipantCounts,previous,sizeof(previous))) return false;
    // C0/C4/C8/CC are additive local-player/AI/remote-player/AI counts,
    // NOT a total-car field. Native 54CCA0 and 4BF290 sum all four when
    // copying winners/rendering rows; stale event AI counts create null rows.
    if(std::memcmp(actual,previous,sizeof(actual))==0) return true;
    if(!SafeWriteBytes(kParticipantCounts,actual,sizeof(actual))) return false;
    char line[180];
    std::snprintf(line,sizeof(line),
        "[RACE_RESULT] Participant counts reconciled [%u,%u,%u,%u] -> [%u,%u,%u,%u], native racers=%u.\r\n",
        previous[0],previous[1],previous[2],previous[3],actual[0],actual[1],actual[2],actual[3],count);
    LogMessage(line);
    return true;
}

static bool RestoreFrozenRacer(uintptr_t racer) {
    uint8_t rank = 0;
    uint32_t ticks = 0;
    uint16_t status = 0;
    const bool timed=CoopFinishReadTimedResult(racer,rank,ticks,status);
    const bool drift=!timed && CoopFinishReadDriftResult(racer,rank,ticks,status);
    if(drift && !status)status=2; // No terminal native status for this AI at the score cutoff.
    if(!timed && !drift) {
        const bool managedAI=CoopFinishIsManagedAIRacer(racer);
        if((!managedAI && !CoopFinishIsLocalRacer(racer) && !CoopFinishIsPeerRacer(racer)) ||
            (!ReadCoopUrlFrozenRacer(racer,rank,ticks,status) &&
             !(managedAI?CoopFinishReadManagedAI(racer,rank,ticks,status):CoopFinishReadFrozen(racer,rank,ticks,status))))return false;
    }
    if(!rank || rank>kMaximumRacers || !status)return false;
    // +C is immutable finish rank, +A is the live/display rank. Native result
    // serialization reads +A, +10 and +198, so preserve all four fields.
    return SafeWriteBytes(racer+0x0A,&rank,1) && SafeWriteBytes(racer+0x0C,&rank,1) &&
        SafeWriteBytes(racer+0x10,&status,2) && SafeWriteBytes(racer+0x198,&ticks,4);
}

static void RestoreFrozenRacers(uintptr_t coordinator) {
    uint32_t count = 0;
    if (!ReadGameMemory(coordinator + 0x24, &count, sizeof(count)) ||
        count == 0 || count > kMaximumRacers) return;
    for (uint32_t index = 0; index < count; ++index) {
        uintptr_t racer = 0;
        if (ReadRacer(coordinator, index, racer)) RestoreFrozenRacer(racer);
    }
}

static bool ReadTeam(uint8_t& rank, uint32_t& ticks, uint16_t& status) {
    return CoopFinishReadTeamResult(rank, ticks, status) &&
        rank > 0 && rank <= kMaximumRacers && status != 0;
}

static void __fastcall HookFinish(void* object, void*, uint32_t index,
    int32_t rankArgument, uint32_t postFinish, uint32_t statusArgument) {
    if (!CoopFinishRaceActive()) {
        if (CoopFinishRaceInterrupted()) return;
        gFinish(object, index, rankArgument, postFinish, statusArgument);
        return;
    }
    const uintptr_t coordinator = reinterpret_cast<uintptr_t>(object);
    uintptr_t racer = 0, car = 0;
    // Native 606CC2 and 60710C dereference these objects. A remote finish is
    // accepted from the peer's real game, never from its interpolated proxy.
    if (!ReadRacer(coordinator, index, racer) ||
        !ReadGameMemory(racer, &car, sizeof(car)) || !IsValidUserPtr(car)) return;
    if (CoopFinishIsManagedAIRacer(racer)) {
        // Only the host simulates and completes managed AI. Client rows use
        // the authoritative frozen finish/progress received from that host.
        CoopSyncRaceProgressBeforeSort();
        return;
    }
    if (CoopFinishIsPeerRacer(racer)) {
        RestoreFrozenRacer(racer);
        return;
    }
    if (CoopFinishIsLocalRacer(racer) && RestoreFrozenRacer(racer)) return;
    gFinish(object, index, rankArgument, postFinish, statusArgument);
    if (!CoopFinishIsLocalRacer(racer)) return;
    uint8_t rank = 0;
    uint32_t ticks = 0;
    uint16_t status = 0;
    if (ReadGameMemory(racer + 0x0C, &rank, sizeof(rank)) &&
        ReadGameMemory(racer + 0x198, &ticks, sizeof(ticks)) &&
        ReadGameMemory(racer + 0x10, &status, sizeof(status)) &&
        rank > 0 && rank <= kMaximumRacers && status != 0) {
        CoopFinishCaptureLocal(rank, ticks, status);
        RestoreFrozenRacer(racer);
    }
}

static int __fastcall HookAllPlayersDone(void* object, void*) {
    if (CoopFinishRaceActive()) {
        uint8_t rank = 0; uint32_t ticks = 0; uint16_t status = 0;
        if (!ReadTeam(rank, ticks, status)) return 0;
    } else if (CoopFinishRaceInterrupted()) return 0;
    return gAllPlayersDone(object);
}

static void __fastcall HookSort(void* object, void*) {
    const bool active = CoopFinishRaceActive();
    const uintptr_t coordinator = reinterpret_cast<uintptr_t>(object);
    if (active) {
        // The native sorter and HUD gap use the same lap-aware +20 progress.
        // Refresh it before the first comparison, not after ranks were assigned.
        CoopSyncRaceProgressBeforeSort();
        RestoreFrozenRacers(coordinator);
    }
    gSort(object);
    if (active) RestoreFrozenRacers(coordinator);
}

static void __fastcall HookSerializeResult(void* output, void*, void* racerObject,
    uint32_t fromNetwork) {
    if(CoopFinishRaceActive()) RestoreFrozenRacer(reinterpret_cast<uintptr_t>(racerObject));
    gSerializeResult(output, racerObject, fromNetwork);
    const uintptr_t racer = reinterpret_cast<uintptr_t>(racerObject);
    if (!CoopFinishRaceActive()) return;
    // Visible results retain each human's frozen finish. The team's scored
    // row is substituted only in the career-history COPY, never in this UI.
    const uintptr_t row = reinterpret_cast<uintptr_t>(output);
    if (row >= kResultGroup && row < kResultGroup + kMaximumRacers * kResultStride &&
        (row - kResultGroup) % kResultStride == 0) {
        if(gResultEntered)CoopFinishWriteDriftScore(row,racer);
        if(CoopFinishIsLocalRacer(racer))gLocalResultRow=row;
    }
}

static bool OrderVisibleResultRows() {
    uint32_t count=0;
    if(!ReadGameMemory(kResultGroup+0xCF0,&count,4) || count<2 || count>kMaximumRacers)return false;
    uintptr_t sorted[kMaximumRacers]{};uint32_t ids[kMaximumRacers]{};
    for(uint32_t i=0;i<count;++i) {
        const auto row=kResultGroup+i*kResultStride;
        uint32_t rank=0;
        if(!ReadGameMemory(row,&rank,4) || !rank || rank>count || sorted[rank-1] ||
            !ReadGameMemory(row+4,&ids[i],4))return false;
        for(uint32_t j=0;j<i;++j)if(ids[j]==ids[i])return false;
        sorted[rank-1]=row;
    }
    return SafeWriteBytes(kResultGroup+0xCD8,sorted,count*sizeof(uintptr_t));
}

static void __fastcall HookRenderTime(void* screen,void*,void* result,uint32_t position) {
    const auto row=reinterpret_cast<uintptr_t>(result);
    uint32_t count=0,ticks=0,status=0;
    uintptr_t package=0;
    if(gResultEntered && CoopFinishRaceActive() && (CoopFinishHasTimedResult() || IsCoopUrlTournament()) &&
        row>=kResultGroup && (row-kResultGroup)%kResultStride==0 &&
        ReadGameMemory(kResultGroup+0xCF0,&count,4) && count<=kMaximumRacers &&
        (row-kResultGroup)/kResultStride<count && ReadGameMemory(row+8,&status,4) &&
        ReadGameMemory(row+0xC,&ticks,4) && !coop_race::TimedSuccess(uint16_t(status),ticks) &&
        ReadGameMemory(reinterpret_cast<uintptr_t>(screen)+4,&package,4) && IsValidUserPtr(package)) {
        const char* label="DNF";
        if(status==8 || status==9) {
            const auto localized=reinterpret_cast<const char*(__cdecl*)(uint32_t)>(0x4FFA80)(
                status==8?0x01E66364u:0xB7B75185u);
            if(localized && *localized)label=localized;
        }
        // Same FE row-label hash and printf entry used by stock 4A40E8.
        const auto hash=reinterpret_cast<uint32_t(__cdecl*)(const char*,...)>(0x50B990)(
            reinterpret_cast<const char*>(0x7904EC),position);
        reinterpret_cast<int(__cdecl*)(uintptr_t,uint32_t,const char*,...)>(0x537B80)(package,hash,"%s",label);
        return;
    }
    gRenderTime(screen,result,position);
}

static bool HaveLocalResultRow() {
    uint32_t count = 0, localId = ~uint32_t(0);
    return ReadGameMemory(kResultGroup + 0xCF0, &count, sizeof(count)) &&
        count>0 && count<=kMaximumRacers && gLocalResultRow>=kResultGroup &&
        (gLocalResultRow-kResultGroup)%kResultStride==0 &&
        (gLocalResultRow-kResultGroup)/kResultStride<count &&
        ReadGameMemory(gLocalResultRow+4,&localId,4) && localId==0;
}

static void* __fastcall HookCopyResult(void* destination,void*,void* source) {
    void* result=gCopyResult(destination,source); // Preserve stock EAX / RET4.
    const uintptr_t target=reinterpret_cast<uintptr_t>(destination);
    if(IsCoopUrlTournament() && reinterpret_cast<uintptr_t>(source)==kResultGroup &&
        CoopFinishCareerRecordMatches(target)) {
        // Keep each human's actual heat rank. Taking the better human at every
        // heat creates a team total that neither driver earned individually.
        gCareerHistoryReady=CoopUrlHistoryCopied(target);
        return result;
    }
    const bool appliedCopy=gCareerCommitApplied &&
        target==reinterpret_cast<uintptr_t>(gAppliedCommitResult)+0x30;
    // A valid career acceptance may run after the UI/flow left the result
    // page. The scoped receipt survives that transition; gResultEntered does
    // not. Exact native record + event/session/epoch remains mandatory.
    if((!gCareerCommitArmed && !appliedCopy) ||
        reinterpret_cast<uintptr_t>(source)!=kResultGroup ||
        !CoopFinishCareerRecordMatches(target)) return result;
    gCareerHistoryReady=false;
    uint8_t rank=0;uint16_t status=0;uint32_t ticks=0,count=0;
    uintptr_t pointers[kMaximumRacers]{};
    if(!HaveLocalResultRow() || !ReadTeam(rank,ticks,status) ||
        !ReadGameMemory(kResultGroup+0xCF0,&count,4)) return result;
    uint32_t localIndex=kMaximumRacers;
    for(uint32_t i=0;i<count;++i) {
        uintptr_t row=0;
        if(!ReadGameMemory(kResultGroup+0xCD8+i*4,&row,4) || row<kResultGroup ||
            (row-kResultGroup)%kResultStride!=0 || (row-kResultGroup)/kResultStride>=count) return result;
        pointers[i]=target+(row-kResultGroup);
        for(uint32_t j=0;j<i;++j) if(pointers[j]==pointers[i]) return result;
        if(row==gLocalResultRow) localIndex=i;
    }
    if(localIndex==kMaximumRacers) return result;
    // Stock 525D10 copies pointers still aimed at the visible results group.
    // Rebase ONLY the exact current career history, not UI rows or identities.
    if(rank==1) std::swap(pointers[0],pointers[localIndex]);
    const uintptr_t local=target+(gLocalResultRow-kResultGroup);
    const uint32_t scoredRank=rank,scoredStatus=status;
    if(!SafeWriteBytes(local,&scoredRank,4) || !SafeWriteBytes(local+8,&scoredStatus,4) ||
        !SafeWriteBytes(local+0xC,&ticks,4)) return result;
    for(uint32_t i=0;i<count;++i) if(!SafeWriteBytes(target+0xCD8+i*4,&pointers[i],4)) return result;
    gCareerHistoryReady=true;
    return result;
}

static void __fastcall HookResultGroup(void* object,void*,void* coordinator) {
    uint8_t rank=0;uint32_t ticks=0;uint16_t status=0;
    const bool scoped=reinterpret_cast<uintptr_t>(object)==kResultGroup &&
        gResultEntered && reinterpret_cast<uintptr_t>(coordinator)==gResultCoordinator &&
        CoopFinishRaceActive() && ReadTeam(rank,ticks,status);
    if(scoped) gLocalResultRow=0;
    gResultGroup(object,coordinator);
    if(scoped && IsCoopUrlTournament() && !CoopUrlResultGroupBuilt()) {
        gCareerCommitArmed=false;gCareerHistoryReady=false;return;
    }
    if(scoped && CoopFinishHasOrderedResult() && !OrderVisibleResultRows()) {
        gCareerCommitArmed=false;gCareerHistoryReady=false;
        LogMessage("[RACE_RESULT] Sealed result row identities or ranks were incomplete.\r\n");
        return;
    }
    // Arm before 54CA60 copies history inside EnterResult. The history-copy
    // hook scores the team without changing any visible individual finish.
    if(scoped) {
        gCareerCommitArmed=HaveLocalResultRow();
        LogMessage(gCareerCommitArmed?
            "[RACE_RESULT] Local native identity verified before team career-history copy.\r\n":
            "[RACE_RESULT] Local native winner identity missing; reward receipt held.\r\n");
    }
}

static bool __cdecl MayClassifyPeerWinnerWithoutPlayer(uintptr_t car) {
    uint8_t rank=0,teamRank=0,remote=0;uint16_t status=0,teamStatus=0,id=0;
    uint32_t ticks=0,teamTicks=0,type=0,rowId=~uint32_t(0),count=0;
    uintptr_t player=0,param=0,racer=0,owner=0,coordinator=0,row=0;
    // Scene safety does not depend on a later cash/history persistence
    // receipt. The immutable paired finish and current native winner prove it.
    if((!gResultEntered && !gCareerCommitArmed && !gCareerCommitApplied) ||
        !HaveLocalResultRow() || !CoopFinishRaceActive() || !CoopFinishSceneScopeMatches() ||
        !ReadTeam(teamRank,teamTicks,teamStatus) || teamRank!=1 ||
        !ReadGameMemory(car+0x18,&player,4) || player ||
        !ReadGameMemory(car+0x14,&param,4) || !IsValidUserPtr(param) ||
        !ReadGameMemory(param+4,&type,4) || type!=1 ||
        !ReadGameMemory(param+0x900,&remote,1) || remote!=1 ||
        !ReadGameMemory(car+0x1C,&racer,4) || !CoopFinishIsPeerRacer(racer) ||
        !ReadGameMemory(racer,&owner,4) || owner!=car ||
        !CoopFinishReadFrozen(racer,rank,ticks,status) || rank!=1 || !status ||
        !ReadGameMemory(kCoordinator,&coordinator,4) || coordinator!=gResultCoordinator ||
        !ReadGameMemory(kResultGroup+0xCF0,&count,4) ||
        !ReadGameMemory(kResultGroup+0xCD8,&row,4) || row<kResultGroup ||
        (row-kResultGroup)%kResultStride!=0 || (row-kResultGroup)/kResultStride>=count ||
        !ReadGameMemory(row+4,&rowId,4) || !ReadGameMemory(car+0xA,&id,2) || rowId!=id) return false;
    bool present=false;
    for(uint32_t i=0;i<count;++i) {
        uintptr_t current=0;
        if(!ReadRacer(coordinator,i,current)) return false;
        if(current==racer) present=true;
    }
    if(present && !gPeerWinnerSceneLogged) {
        gPeerWinnerSceneLogged=true;
        LogMessage("[RACE_SCENE] Verified peer winner has no local Player; native offline-player role flags remain unset.\r\n");
    }
    return present;
}

// Stock NIS condition builder 431DF0 treats every Type1 winner as a LOCAL
// Player when native LAN is off. A co-op peer deliberately has no such object.
// Keep the original scene/count/winner logic, but do not assign that peer an
// offline local-player0/1 role. No fake Player, car swap or result rewrite.
__declspec(naked) static void HookPeerWinnerPlayerNaked() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,528
        and esp,0FFFFFFF0h
        mov [esp+512],eax
        fxsave [esp]
        push edi
        call MayClassifyPeerWinnerWithoutPlayer
        add esp,4
        test al,al
        jz original_player
        mov eax,[esp+512]
        mov dword ptr [eax+28],0FFFFFFFFh
        mov dword ptr [eax+20],0
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        jmp dword ptr [gPeerWinnerPlayerContinue]
    original_player:
        fxrstor [esp]
        mov esp,[esp+512]
        popad
        popfd
        jmp dword ptr [gPeerWinnerPlayerOriginal]
    }
}

static void __fastcall HookEnterResult(void* object, void*) {
    if (!CoopFinishRaceActive()) {
        if (!CoopFinishRaceInterrupted()) gEnterResult(object);
        return;
    }
    uint8_t rank = 0; uint32_t ticks = 0; uint16_t status = 0;
    if (!ReadTeam(rank, ticks, status)) return;
    uintptr_t liveCoordinator = 0; // Resolve the live global, not RaceFlow::this.
    if (!ReadGameMemory(kCoordinator, &liveCoordinator, sizeof(liveCoordinator)) ||
        !IsValidUserPtr(liveCoordinator)) return;
    if (gResultEntered && gResultCoordinator == liveCoordinator) return;
    if(IsCoopUrlTournament() && !PrepareCoopUrlResults())return;
    if (!ReconcileParticipantCounts(liveCoordinator)) {
        if(!gParticipantCountsHoldLogged) {
            LogMessage("[RACE_RESULT] Native participant list is not valid; result entry held safely.\r\n");
            gParticipantCountsHoldLogged=true;
        }
        return;
    }
    gParticipantCountsHoldLogged=false;
    if(CoopFinishHasOrderedResult()) {
        uint32_t count=0;
        if(!ReadGameMemory(liveCoordinator+0x24,&count,4) || count<2 || count>kMaximumRacers)return;
        for(uint32_t i=0;i<count;++i) {
            uintptr_t racer=0;
            if(!ReadRacer(liveCoordinator,i,racer) || !RestoreFrozenRacer(racer))return;
        }
    } else RestoreFrozenRacers(liveCoordinator);
    gLocalResultRow = 0;
    gResultCoordinator = liveCoordinator;
    gResultEntered = true; // Native result flow must run once, even if reentrant.
    gCareerCommitApplied = false;
    gCareerCommitArmed = false;
    gCareerHistoryReady = false;
    gPeerWinnerSceneLogged = false;
    gEnterResult(object);
    LogMessage(gCareerCommitArmed || gCareerCommitApplied ?
        "[RACE_FINISH] Both humans finished; native team result ready.\r\n" :
        "[RACE_FINISH] Native team winner identity was not available; checkpoint held.\r\n");
}

static void __fastcall HookCareerCommit(void* object, void*, void* result) {
    if (CoopFinishRaceInterrupted()) return;
    if(IsCoopUrlTournament()) {uint8_t rank=0;if(!CoopUrlReadCareerOutcome(rank)||!gCareerHistoryReady)return;}
    uint32_t eventHash=0;
    ReadGameMemory(reinterpret_cast<uintptr_t>(result)+0x38,&eventHash,4);
    const bool notify = gCareerCommitArmed && IsCoopGameplayActive() && CoopFinishCareerEventMatches(eventHash);
    if(IsCoopGameplayActive() && gCareerCommitApplied && object==gAppliedCommitObject &&
        result==gAppliedCommitResult && eventHash==gAppliedCommitHash) return;
    // Stock 541420 has no payout deduplication. Arm the receipt BEFORE calling
    // native code so a reentrant/repeated Continue cannot credit money twice.
    if(notify) {
        gCareerCommitApplied=true;gCareerCommitArmed=false;
        gAppliedCommitObject=object;gAppliedCommitResult=result;gAppliedCommitHash=eventHash;
    }
    // Scripted/onboarding career result path; normal races use 54C940 below.
    gCareerCommit(object, result);
    if (notify) {
        CoopFinishNativeCareerCommitted();
    }
}

static void __fastcall HookOrdinaryCareerCommit(void* object,void*) {
    if (CoopFinishRaceInterrupted()) return;
    const uintptr_t career=reinterpret_cast<uintptr_t>(object);
    uintptr_t event=0,record=0;uint32_t eventHash=0,phase=0;uint8_t active=0,done=0;
    const bool candidate=IsCoopGameplayActive() &&
        ReadGameMemory(career+0x8730,&event,4) && IsValidUserPtr(event) &&
        ReadGameMemory(event+8,&eventHash,4) && CoopFinishCareerEventMatches(eventHash);
    if(candidate && IsCoopUrlTournament()) {
        uint8_t finalRank=0;
        // Native ordinary commit can also be reached by leaving a result
        // page. Intermediate heats have no final win/fail receipt to persist.
        if(!CoopUrlReadCareerOutcome(finalRank)||!gCareerHistoryReady)return;
    }
    if(candidate && gCareerCommitApplied && object==gAppliedCommitObject && eventHash==gAppliedCommitHash) return;
    const bool notify=candidate && gCareerCommitArmed && gCareerHistoryReady &&
        ReadGameMemory(career+0xD25,&active,1) && active &&
        ReadGameMemory(career+0xD28,&done,1) && !done &&
        ReadGameMemory(career+0x4208,&record,4) && IsValidUserPtr(record) &&
        ReadGameMemory(record+0x14,&phase,4) && (phase==3 || phase==4);
    if(notify) {
        // Receipt precedes native payout to cover reentry. Do not synthesize
        // cash, replay 541420 or clear personal event/garage state.
        gCareerCommitApplied=true;gCareerCommitArmed=false;
        gAppliedCommitObject=object;gAppliedCommitResult=reinterpret_cast<void*>(record);gAppliedCommitHash=eventHash;
    }
    gOrdinaryCommit(object); // stock 54C940: local success541420 / failure528500
    if(notify && gCareerHistoryReady) {
        LogMessage("[RACE_REWARD] Original ordinary career win/fail committed; saving this player's own profile.\r\n");
        CoopFinishNativeCareerCommitted();
    }
}

struct Detour {
    uintptr_t address;
    const BYTE* expected;
    size_t length;
    void* hook;
    BYTE* trampoline = nullptr;
    bool installed = false;
};
static constexpr BYTE kFinishBytes[] = {0x51,0x8B,0x44,0x24,0x08,0x53,0x55,0x56,0x57,0x8B,0xF9};
static constexpr BYTE kDoneBytes[] = {0xA0,0x69,0xCF,0x89,0x00,0x84,0xC0};
static constexpr BYTE kSortBytes[] = {0x83,0xEC,0x08,0x53,0x55,0x56,0x57,0x8B,0xF9};
static constexpr BYTE kSerializeBytes[] = {0x53,0x55,0x8B,0x6C,0x24,0x0C,0x56,0x8B,0xF1,0x57};
static constexpr BYTE kResultBytes[] = {0x6A,0xFF,0x68,0x1F,0xA1,0x77,0x00};
static constexpr BYTE kCommitBytes[] = {0x55,0x8B,0xEC,0x53,0x56,0x57,0x8B,0x7D,0x08};
static constexpr BYTE kGroupBytes[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x6A,0xFF};
static constexpr BYTE kOrdinaryBytes[] = {0x57,0x8B,0xF9,0x8A,0x87,0x25,0x0D,0x00,0x00};
static constexpr BYTE kCopyBytes[] = {0x53,0x55,0x8B,0x6C,0x24,0x0C,0x56};
static constexpr BYTE kPeerWinnerBytes[] = {0x8B,0x57,0x18,0x8B,0x42,0x10};
static constexpr BYTE kRenderTimeBytes[] = {0x83,0xEC,0x20,0x56,0x8B,0xF1,0x57};
static Detour gDetours[] = {
    {0x606CB0,kFinishBytes,sizeof(kFinishBytes),reinterpret_cast<void*>(&HookFinish)},
    {0x5F85B0,kDoneBytes,sizeof(kDoneBytes),reinterpret_cast<void*>(&HookAllPlayersDone)},
    {0x5EEC80,kSortBytes,sizeof(kSortBytes),reinterpret_cast<void*>(&HookSort)},
    {0x515DB0,kSerializeBytes,sizeof(kSerializeBytes),reinterpret_cast<void*>(&HookSerializeResult)},
    {0x60AAA0,kResultBytes,sizeof(kResultBytes),reinterpret_cast<void*>(&HookEnterResult)},
    {0x54CAF0,kCommitBytes,sizeof(kCommitBytes),reinterpret_cast<void*>(&HookCareerCommit)},
    {0x54CCA0,kGroupBytes,sizeof(kGroupBytes),reinterpret_cast<void*>(&HookResultGroup)},
    {0x54C940,kOrdinaryBytes,sizeof(kOrdinaryBytes),reinterpret_cast<void*>(&HookOrdinaryCareerCommit)},
    {0x525D10,kCopyBytes,sizeof(kCopyBytes),reinterpret_cast<void*>(&HookCopyResult)},
    {0x431EE9,kPeerWinnerBytes,sizeof(kPeerWinnerBytes),reinterpret_cast<void*>(&HookPeerWinnerPlayerNaked)}
};

static void Jump(BYTE* location, const void* destination) {
    const int32_t offset = static_cast<int32_t>(reinterpret_cast<intptr_t>(destination) -
        (reinterpret_cast<intptr_t>(location) + 5));
    location[0] = 0xE9; std::memcpy(location + 1, &offset, sizeof(offset));
}
static void Publish(const Detour& detour, bool trampoline) {
    const uintptr_t address = trampoline ? reinterpret_cast<uintptr_t>(detour.trampoline) : detour.address;
    switch (detour.address) {
    case 0x606CB0: gFinish = reinterpret_cast<FinishFn>(address); break;
    case 0x5F85B0: gAllPlayersDone = reinterpret_cast<AllPlayersDoneFn>(address); break;
    case 0x5EEC80: gSort = reinterpret_cast<SortFn>(address); break;
    case 0x515DB0: gSerializeResult = reinterpret_cast<SerializeResultFn>(address); break;
    case 0x60AAA0: gEnterResult = reinterpret_cast<EnterResultFn>(address); break;
    case 0x54CAF0: gCareerCommit = reinterpret_cast<CareerCommitFn>(address); break;
    case 0x54CCA0: gResultGroup = reinterpret_cast<ResultGroupFn>(address); break;
    case 0x54C940: gOrdinaryCommit = reinterpret_cast<OrdinaryCommitFn>(address); break;
    case 0x525D10: gCopyResult = reinterpret_cast<CopyResultFn>(address); break;
    case 0x431EE9: gPeerWinnerPlayerOriginal=address; break;
    case 0x4A4020: gRenderTime=reinterpret_cast<RenderTimeFn>(address); break;
    }
}
static void RollBack(Detour& detour) {
    if (detour.installed) {
        DWORD oldProtection = 0;
        BYTE* target = reinterpret_cast<BYTE*>(detour.address);
        if (!VirtualProtect(target, detour.length, PAGE_EXECUTE_READWRITE, &oldProtection)) return;
        std::memcpy(target, detour.expected, detour.length);
        FlushInstructionCache(GetCurrentProcess(), target, detour.length);
        DWORD ignored = 0; VirtualProtect(target, detour.length, oldProtection, &ignored);
        detour.installed = false;
    }
    Publish(detour, false);
    if (detour.trampoline) VirtualFree(detour.trampoline, 0, MEM_RELEASE);
    detour.trampoline = nullptr;
}
static bool Install(Detour& detour) {
    detour.trampoline = reinterpret_cast<BYTE*>(VirtualAlloc(nullptr, 32,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!detour.trampoline) return false;
    // Every displaced block ends on an instruction boundary and has no relative
    // branch/call. SEH PUSH immediates at 60AAA0 retain their original addresses.
    std::memcpy(detour.trampoline, detour.expected, detour.length);
    Jump(detour.trampoline + detour.length, reinterpret_cast<void*>(detour.address + detour.length));
    FlushInstructionCache(GetCurrentProcess(), detour.trampoline, detour.length + 5);
    DWORD oldProtection = 0;
    BYTE* target = reinterpret_cast<BYTE*>(detour.address);
    if (!VirtualProtect(target, detour.length, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        VirtualFree(detour.trampoline, 0, MEM_RELEASE); detour.trampoline = nullptr; return false;
    }
    Publish(detour, true);
    Jump(target, detour.hook);
    if (detour.length > 5) std::memset(target + 5, 0x90, detour.length - 5);
    FlushInstructionCache(GetCurrentProcess(), target, detour.length);
    DWORD ignored = 0; VirtualProtect(target, detour.length, oldProtection, &ignored);
    detour.installed = true;
    return true;
}
} // namespace CoopRaceFinishNative

bool InstallCoopRaceFinishHooks() {
    using namespace CoopRaceFinishNative;
    if (gInstalled) return true;
    for (const Detour& detour : gDetours) {
        BYTE bytes[16]{};
        if (!ReadGameMemory(detour.address, bytes, detour.length) ||
            std::memcmp(bytes, detour.expected, detour.length) != 0) {
            LogMessage("[RACE_FINISH] Native signature mismatch; finish adapters were not installed.\r\n");
            return false;
        }
    }
    for (Detour& detour : gDetours) {
        if (!Install(detour)) {
            for (Detour& installed : gDetours) RollBack(installed);
            LogMessage("[RACE_FINISH] Finish adapter installation failed and was rolled back.\r\n");
            return false;
        }
    }
    gInstalled = true;
    static Detour timeLabel{0x4A4020,kRenderTimeBytes,sizeof(kRenderTimeBytes),reinterpret_cast<void*>(&HookRenderTime)};
    BYTE timeBytes[sizeof(kRenderTimeBytes)]{};
    if(!ReadGameMemory(timeLabel.address,timeBytes,sizeof(timeBytes)) || std::memcmp(timeBytes,timeLabel.expected,sizeof(timeBytes)) ||
        !Install(timeLabel))LogMessage("[RACE_RESULT] Native time label differs; stock formatting retained.\r\n");
    LogMessage("[RACE_FINISH] Verified native finish, two-human result barrier and career-commit adapters installed.\r\n");
    return true;
}

void TickCoopRaceFinish(DWORD flow) {
    using namespace CoopRaceFinishNative;
    if (!IsCoopGameplayActive()) {
        gResultEntered = false; gCareerCommitArmed = false;gCareerCommitApplied=false;
        gCareerHistoryReady=false;
        gPeerWinnerSceneLogged=false;
        gResultCoordinator = 0; gLocalResultRow = 0;
        gParticipantCountsHoldLogged = false;
        return;
    }
    if (flow != 6 || !CoopFinishRaceActive()) {
        gResultEntered = false; gResultCoordinator = 0;
        gParticipantCountsHoldLogged = false;
        return; // Keep the armed commit through native result acceptance/return.
    }
    uintptr_t coordinator = 0;
    if (ReadGameMemory(kCoordinator, &coordinator, sizeof(coordinator)) &&
        IsValidUserPtr(coordinator)) {
        ReconcileParticipantCounts(coordinator);
        RestoreFrozenRacers(coordinator);
    }
}
