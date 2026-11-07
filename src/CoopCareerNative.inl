// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Native career entry and introductory movie adapters for SPEED2.EXE v1.2.
// Included by NFSU2Coop.cpp after the shared memory/log helpers are available.
// The native routines retain ownership of career progress and movie cleanup.

bool DeferNativeCoopCareerLaunch(uint32_t kind, const char* source);
void RequestCoopSkipVote();
bool IsCoopSkipApproved();
bool CoopMovieCaptureStart(uint32_t,const char*,const char*,uint32_t);
bool CoopMovieHandleMessage(void*,uint32_t,uintptr_t,uintptr_t,uintptr_t);
using ::IsCoopGameplayActive;

namespace CoopCareerNative {

constexpr uintptr_t kFreeRoamEntry = 0x005404A0;
constexpr uintptr_t kResumeEntry = 0x00495A30;
constexpr uintptr_t kCareerIntroEntry = 0x0053D710;
constexpr uintptr_t kMovieHandlerEntry = 0x0055AFE0;
constexpr uint32_t kFrameUpdate = 0xC98356BA;
constexpr uint32_t kSkipInputA = 0x406415E3;
constexpr uint32_t kSkipInputB = 0xB5AF2461;

using FreeRoamFn = void(__cdecl*)();
using ResumeFn = void(__cdecl*)(uint32_t);
using CareerIntroFn = void(__cdecl*)(const char*);
using MovieHandlerFn = void(__thiscall*)(void*, uint32_t, uintptr_t, uintptr_t, uintptr_t);

static FreeRoamFn gFreeRoam = reinterpret_cast<FreeRoamFn>(kFreeRoamEntry);
static ResumeFn gResume = reinterpret_cast<ResumeFn>(kResumeEntry);
using CompletedEventFn = bool(__thiscall*)(void*, uint32_t);
using PlayerIndexFn = int(__cdecl*)(uint32_t);
static CompletedEventFn gCompletedEvent = reinterpret_cast<CompletedEventFn>(0x00500B80);
static PlayerIndexFn gPlayerIndex = reinterpret_cast<PlayerIndexFn>(0x0050D2A0);
static bool (*gDeferCareer)(uint32_t, const char*) = &DeferNativeCoopCareerLaunch;
struct ResumeGlobals {
    uintptr_t flow = 0x8654A4, manager = 0x85AD40, mode = 0x850078;
    uintptr_t playerIndex = 0x83A9D8, crib = 0x836324, savedSpawn = 0x863484;
};
static ResumeGlobals gResumeGlobals;
using PrepareEventFn = void(__thiscall*)(void*, void*);
static PrepareEventFn gPrepareCareerEvent = reinterpret_cast<PrepareEventFn>(0x00532EA0);
static PrepareEventFn gPrepareRaceEvent = reinterpret_cast<PrepareEventFn>(0x00524240);
struct CareerManagerGlobals {
    uintptr_t career = 0x85AD40, race = 0x8635BC;
};
static CareerManagerGlobals gCareerManagerGlobals;
static CareerIntroFn gCareerIntro = reinterpret_cast<CareerIntroFn>(kCareerIntroEntry);
static MovieHandlerFn gMovieHandler = reinterpret_cast<MovieHandlerFn>(kMovieHandlerEntry);
static bool gReplayingCareer = false; // Career launch and replay run on the game thread.
static bool gHooksInstalled = false;
using NewCareerButtonFn=void(__thiscall*)(void*,uintptr_t,uint32_t,uintptr_t,uintptr_t,uintptr_t);
static NewCareerButtonFn gNewCareerButton=reinterpret_cast<NewCareerButtonFn>(0x4AF090);
static NewCareerButtonFn gResumeCareerButton=reinterpret_cast<NewCareerButtonFn>(0x4AF0C0);
static NewCareerButtonFn gLoadCareerButton=reinterpret_cast<NewCareerButtonFn>(0x4AF0E0);
static uint32_t gNewCareerIntentTicket=0;
using CareerDialogFn=void(__thiscall*)(void*,uint32_t,uintptr_t,uintptr_t,uintptr_t);
static CareerDialogFn gCareerDialog=reinterpret_cast<CareerDialogFn>(0x4FD570);
struct NewChoiceGlobals {uintptr_t config=0x836514,owner=0x83651C,ui=0x8384C4;};
static NewChoiceGlobals gNewChoiceGlobals;
struct NewChoiceProof {uint64_t session=0;uint32_t ticket=0;uintptr_t dialog=0;bool yes=false,approved=false,cancelled=false;uint32_t menu=0;};
static NewChoiceProof gNewChoice;
static bool NewChoiceName(uintptr_t address,const char* expected){
    if(!IsValidUserPtr(address))return false;
    for(unsigned i=0;i<40;++i){uint8_t actual=0,wanted=uint8_t(expected[i]);
        if(!ReadGameMemory(address+i,&actual,1))return false;
        if(actual>='a'&&actual<='z')actual-=32;if(wanted>='a'&&wanted<='z')wanted-=32;
        if(actual!=wanted)return false;if(!actual)return true;}
    return false;
}
static bool SameNewChoice(const coop::SessionSnapshot& s){return gNewChoice.session==s.sessionId&&gNewChoice.ticket==s.ticket&&
    gNewChoice.menu==gCoopCareerMenuGeneration&&s.sessionId;}
static bool CareerMenuScope() {return IsCoopGameplayActive()&&gProfileContext.IsCoopActive();}
static bool AllowCareerMenuInput(bool create) {
    const auto s=CoopSnapshot();uint32_t flow=0;
    return CareerMenuScope()&&ReadGameMemory(gResumeGlobals.flow,&flow,4)&&flow==3&&s.joined&&s.fresh&&
        s.localInMainMenu&&!s.resumeRequestPending&&!gCareerLaunchPending&&CoopModsReady(s)&&
        (s.phase==coop::Phase::Lobby||s.phase==coop::Phase::Playing)&&
        (s.role==coop::Role::Host||(!create&&s.role==coop::Role::Client&&s.careerLaunched));
}
static bool SkipClientNewCareerRow() {return CareerMenuScope()&&CoopSnapshot().role==coop::Role::Client;}
static uintptr_t gNewRowAllocate=0x575620,gNewRowContinue=0x4AF249,gNewRowSkip=0x4AF28B;
// Audited constructor block: New row allocation/binding only. Skip before
// allocation, not by hiding a live FE object or corrupting its scroller links.
static __declspec(naked) void HookNewCareerRow() {
    __asm {
        pushfd
        pushad
        call SkipClientNewCareerRow
        test al,al
        jz original
        popad
        popfd
        jmp dword ptr [gNewRowSkip]
    original:
        popad
        popfd
        push 48h
        call dword ptr [gNewRowAllocate]
        jmp dword ptr [gNewRowContinue]
    }
}
static bool OwnedNewChoiceDialog(uintptr_t object){
    uint32_t config=0;uintptr_t owner=0,vt=0,name=0,ui=0,engine=0,cursor=0;
    if(!ReadGameMemory(gNewChoiceGlobals.config,&config,4)||(config&0xFFFFFu)!=0x21961u||
        !ReadGameMemory(gNewChoiceGlobals.owner,&owner,4)||!NewChoiceName(owner,"UI_StartCareer.fng")||
        !IsValidUserPtr(object)||!ReadGameMemory(object,&vt,4)||vt!=0x797DF4||
        !ReadGameMemory(object+4,&name,4)||!NewChoiceName(name,"MC_Main.fng")||
        !ReadGameMemory(gNewChoiceGlobals.ui,&ui,4)||!IsValidUserPtr(ui)||
        !ReadGameMemory(ui+8,&engine,4)||!IsValidUserPtr(engine)||!ReadGameMemory(engine+0xE4,&cursor,4))return false;
    std::array<uintptr_t,64> seen{};unsigned count=0,matches=0;
    while(cursor){
        if(!IsValidUserPtr(cursor)||count==seen.size()||std::find(seen.begin(),seen.begin()+count,cursor)!=seen.begin()+count)return false;
        seen[count++]=cursor;uintptr_t next=0,packageName=0;uint8_t active=0,removing=0;
        if(!ReadGameMemory(cursor+4,&next,4)||!ReadGameMemory(cursor+0xC,&packageName,4)||
            !ReadGameMemory(cursor+0x14,&active,1)||!ReadGameMemory(cursor+0x15,&removing,1)||active>1||removing>1)return false;
        if(NewChoiceName(packageName,"MC_Main.fng")){
            uintptr_t descriptor=0,handler=0,bound=0;
            if(active!=1||removing||!ReadGameMemory(cursor+0x40,&descriptor,4)||!IsValidUserPtr(descriptor)||
                !ReadGameMemory(descriptor+0x14,&handler,4)||handler!=object||
                !ReadGameMemory(descriptor+0x18,&bound,4)||bound!=cursor)return false;
            ++matches;
        }
        cursor=next;
    }
    return matches==1;
}
static bool InitialCareerChoiceScope(const coop::SessionSnapshot& state) {
    uint32_t flow=0;
    return IsCoopGameplayActive() && gProfileContext.IsCoopActive() && state.role==coop::Role::Host &&
        state.joined && state.fresh &&
        ReadGameMemory(gResumeGlobals.flow,&flow,4) && flow==3;
}
static void __fastcall HookNewCareerButton(void* object,void*,uintptr_t a1,uint32_t message,
    uintptr_t a3,uintptr_t a4,uintptr_t a5) {
    if(message==0x0C407210&&CareerMenuScope()&&!AllowCareerMenuInput(true))return;
    const auto state=CoopSnapshot();
    if(message==0x0C407210 && InitialCareerChoiceScope(state)){
        gNewCareerIntentTicket=state.ticket?state.ticket:UINT32_MAX;gNewChoice={state.sessionId,state.ticket};gNewChoice.menu=gCoopCareerMenuGeneration;
    }
    // Its real profile-name/confirmation dialog owns cancellation and commits
    // the typed name. Do not reset the loaded career when opening the dialog.
    gNewCareerButton(object,a1,message,a3,a4,a5);
}
static void __fastcall HookResumeCareerButton(void* object,void*,uintptr_t a1,uint32_t message,uintptr_t a3,uintptr_t a4,uintptr_t a5) {
    if(message==0x0C407210&&CareerMenuScope()&&(!AllowCareerMenuInput(false)||!gCoopResumePreviewAvailable))return;
    gResumeCareerButton(object,a1,message,a3,a4,a5);
}
static void __fastcall HookLoadCareerButton(void* object,void*,uintptr_t a1,uint32_t message,uintptr_t a3,uintptr_t a4,uintptr_t a5) {
    if(message==0x0C407210&&CareerMenuScope()&&!AllowCareerMenuInput(false))return;
    gLoadCareerButton(object,a1,message,a3,a4,a5);
}
static void __fastcall HookCareerDialog(void* object,void*,uint32_t message,uintptr_t widget,uintptr_t b,uintptr_t c){
    const auto state=CoopSnapshot();
    const bool owned=SameNewChoice(state)&&InitialCareerChoiceScope(state)&&OwnedNewChoiceDialog(uintptr_t(object));
    if(owned&&(message==0x0C407210||message==0x8867412D||message==0x911AB364||message==0xC9D30688||message==0x461A18EE)){
        uint32_t config=0,button=0;ReadGameMemory(gNewChoiceGlobals.config,&config,4);
        const bool buttonKnown=IsValidUserPtr(widget)&&ReadGameMemory(widget+0x10,&button,4);
        char line[320];std::snprintf(line,sizeof(line),
            "[CAREER_DIALOG] session=%llu ticket=%u menu=%u message=%08X config=%08X prompt=%u button=%08X known=%u intent=%u yes=%u approved=%u cancelled=%u.\r\n",
            static_cast<unsigned long long>(state.sessionId),state.ticket,gCoopCareerMenuGeneration,message,config,
            unsigned((config>>24)&15),button,unsigned(buttonKnown),gNewCareerIntentTicket,unsigned(gNewChoice.yes),unsigned(gNewChoice.approved),unsigned(gNewChoice.cancelled));LogMessage(line);
    }
    if(gNewCareerIntentTicket&&owned){
        uint32_t button=0,config=0;const bool click=message==0x0C407210;
        const bool readable=!click||(IsValidUserPtr(widget)&&ReadGameMemory(widget+0x10,&button,4));
        const bool completedSaveAck=click&&readable&&button==0xB8A7C6CE&&
            ReadGameMemory(gNewChoiceGlobals.config,&config,4)&&(config&0x0F000000u)==0&&
            gNewChoice.yes&&!gNewChoice.cancelled&&gNewChoice.dialog==uintptr_t(object);
        // No/ESC's stock path can forward461A18EE and reach exactly the SAME
        //4AF32A Resume continuation as Yes. Invalidate BEFORE that reentry.
        // The final prompt0 ACK is a different button, not a No. It may keep
        // an already verified Yes until the native save-success continuation;
        // it can never grant Yes on its own or approve an error notice.
        if(completedSaveAck){
            LogMessage("[CAREER_CHOICE] Owned final save acknowledgement retained the existing New approval; native success is still required.\r\n");
        }else if(message==0x8867412D||message==0x911AB364||message==0xC9D30688||(click&&(!readable||button!=0xB8A7C6CC))){
            gNewChoice.cancelled=true;gNewChoice.yes=gNewChoice.approved=false;gNewCareerIntentTicket=0;
            LogMessage("[CAREER_CHOICE] Native dialog closed/declined this New approval; any forwarded Resume must not reset the profile. See CAREER_DIALOG for its subprompt.\r\n");
        }else if(click){
            gNewChoice.dialog=uintptr_t(object);gNewChoice.yes=true;
        }else if(message==0x461A18EE&&gNewChoice.yes&&!gNewChoice.cancelled&&gNewChoice.dialog==uintptr_t(object)){
            gNewChoice.approved=true;
            LogMessage("[CAREER_CHOICE] Explicit owned native Yes verified before New Career continuation.\r\n");
        }
    }
    gCareerDialog(object,message,widget,b,c);
}
static bool ApplyNewCareerChoice(uintptr_t caller) {
    if(caller==0x4AF0D4) {gNewCareerIntentTicket=0;gNewChoice={};return true;} // Explicit Resume.
    if(caller!=0x4AF32A)return true;
    const auto state=CoopSnapshot();
    if(!InitialCareerChoiceScope(state)) {gNewCareerIntentTicket=0;gNewChoice={};return true;}
    if(SameNewChoice(state)&&gNewChoice.cancelled){gNewChoice={};gNewCareerIntentTicket=0;return true;}
    if(!CoopModsReady(state)) {
        CoopAnnouncements::Post(CoopModStatus());
        LogMessage("[MODS] New career choice held before profile reset: current mod check is not approved.\r\n");return false;
    }
    // The ordinary Load dialog0C11 also converges on4AF32A. A caller address
    // never makes an unrelated Load/Resume into a New request.
    uint32_t config=0;
    if(!gNewCareerIntentTicket||(ReadGameMemory(gNewChoiceGlobals.config,&config,4)&&(config&0xFFFFFu)==0xC11u)){
        gNewCareerIntentTicket=0;gNewChoice={};return true;
    }
    if(!SameNewChoice(state)){gNewCareerIntentTicket=0;gNewChoice={};return true;}
    if(!gNewChoice.approved){
        LogMessage("[CAREER_CHOICE] New continuation held: no current owned explicit Yes; saved careers are unchanged.\r\n");return false;
    }
    // Native dialog acceptance 4AF310 -> 495A30 is distinct from the Resume
    // button 4AF0C0 -> 495A30. Reset only at accepted New, before the old
    // profile's DD60E403 flag can incorrectly route this request to its garage.
    std::vector<uint8_t> before(coop_career::kNativePayloadSize);char selectedName[32]{};uint8_t loaded=0,started=0;
    if(!NativeSerializeProfile(before.data())||!ReadGameMemory(gFreshCareerAddresses.loaded,&loaded,1)||
        !ReadGameMemory(gFreshCareerAddresses.started,&started,1)||loaded>1)return false;
    std::memcpy(selectedName,before.data()+coop_career::kPersonalNameOffset,31);
    if(!CoopPersonalSave::Name(selectedName)||(CoopSpecialHooksReady()&&!CoopSpecialBeforeNewCareer(selectedName)))return false;
    // Accepted New always owns a distinct paired transaction. Even an old
    // selection action not yet consumed on this thread must not reload NAME
    // over the native dialog's newly committed name. No menu open/cancel or
    // explicit Resume reaches this opt-in replacement.
    bool selected=false,prepared=false;coop::SessionSnapshot next;
    {
        struct SessionLock {SessionLock(){AcquireSRWLockExclusive(&gSessionLock);}~SessionLock(){ReleaseSRWLockExclusive(&gSessionLock);}} lock;
        const DWORD now=GetTickCount();const auto current=gSession.Snapshot(now);
        // Reserve in a PRIVATE SessionCore copy first: a full reliable queue,
        // stale peer or changed ticket must not reset RAM or publish anything.
        auto proposed=gSession;
        selected=SameNewChoice(current)&&current.fresh&&proposed.RequestCareer(now,true,selectedName);
        // This ONE audited port is profile-only532830: scalar/reset/hash
        // routines531800/576090/516480/500700, no UI pump/IO/session callback.
        // Never put deserialization, menus or reward/bootstrap work under this lock.
        if(selected) {
            prepared=CoopPersonalSave::fresh();
            // Native MC has already selected/saved the typed profile. Its
            // loaded bit belongs to UserProfile identity, not Career's DDAY
            // starter progress. The narrow career reset clears this bit;
            // retain the exact accepted native selection before publishing.
            // Leaving it zero makes EVERY later vanilla Save open Create
            // Profile (55E5CD), and5124A6 can replace the name with a default.
            if(prepared)prepared=SafeWriteBytes(gFreshCareerAddresses.loaded,&loaded,1);
        }
        if(prepared){gSession=proposed;next=gSession.Snapshot(now);gCheckpointSender.Reset();gCheckpointReceiver.Reset();}
    }
    if(!selected) {
        LogMessage("[CAREER_CHOICE] Accepted New held: a fresh paired selection ticket is unavailable.\r\n");return false;
    }
    if(!prepared){
        const bool restored=NativeDeserializeProfile(before.data());
        const bool flags=SafeWriteBytes(gFreshCareerAddresses.loaded,&loaded,1)&&SafeWriteBytes(gFreshCareerAddresses.started,&started,1);
        if(!restored||!flags)gRestoreSpPending=true;
        LogMessage(restored&&flags?"[CAREER_CHOICE] Accepted New held: profile-only reset failed; prior RAM restored, no new ticket published.\r\n":
            "[CAREER_CHOICE] Accepted New held: profile rollback uncertain; singleplayer restore required, no new ticket published.\r\n");return false;
    }
    gAcceptedNewCareerSelectionTicket=next.ticket;gLocalSessionStarted=false;
    gCareerLaunchPending=false;gNativeCareerReleasedTicket=0;gReceivedLaunchKind.store(2);
    CoopPersonalSave::ResetContext();gNativeSaveCompleted.store(false);
    gNewCareerIntentTicket=0;gNewChoice={};
    LogMessage(loaded?"[SAVE_IDENTITY] Accepted New retained the native selected-profile flag; starter progress remains independently reset.\r\n":
        "[SAVE_IDENTITY] Accepted New had no native selected profile; genuine Create Profile remains available.\r\n");
    LogMessage("[CAREER_CHOICE] Accepted New Career reset the prior loaded campaign in isolated RAM; typed native profile name retained.\r\n");
    return true;
}

// Reuse the hidden native movie-caption group; no external overlay/FNG edits.
using FindElementFn = void*(__cdecl*)(const char*, uint32_t);
using LabelFn = int(__cdecl*)(const char*, uint32_t, const char*, ...);
using AnimateFn = void(__cdecl*)(void*, const char*, bool);
using ShowFn = void(__cdecl*)(void*);
using StopMovieFn = void(__cdecl*)();
static FindElementFn gFindMovieElement = reinterpret_cast<FindElementFn>(0x005379C0);
static LabelFn gSetMovieLabel = reinterpret_cast<LabelFn>(0x00537B80);
static AnimateFn gAnimateMovieGroup = reinterpret_cast<AnimateFn>(0x0051CF70);
static ShowFn gShowMovieGroup = reinterpret_cast<ShowFn>(0x0050CA50);
static StopMovieFn gStopExpiredMovie = reinterpret_cast<StopMovieFn>(0x00511BC0);
struct MovieGlobals {
    uintptr_t flow = 0x838568, complete = 0x8383A8, active = 0x8383AC, player = 0x8383A4;
};
static MovieGlobals gMovieGlobals;
static uintptr_t gPromptOwner = 0;
static uint32_t gPromptTicket = 0, gPromptVotes = ~uint32_t(0);
static bool gEnterHeld = false;
struct IntroPresentation {
    bool armed = false;
    DWORD started = 0, lastLog = 0, lastCleanup = 0;
    unsigned samples = 0;
};
static IntroPresentation gIntroPresentation;

static void FormatSkipPrompt(const coop::SessionSnapshot& state, char (&text)[96]) {
    const unsigned votes = unsigned(state.localSkipVote) + unsigned(state.peerSkipVote);
    std::snprintf(text, sizeof(text), "ENTER: SKIP (%u/2)%s", votes,
        state.skipDecided ? " - SKIPPING" : state.localSkipVote ? " - WAITING" : "");
}

static bool UpdateMovieSkipPrompt(void* object, const coop::SessionSnapshot& state) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(object);
    uintptr_t vtable = 0, package = 0;
    char name[32]{};
    if (!ReadGameMemory(address, &vtable, sizeof(vtable)) || vtable != 0x0079CB44 ||
        !ReadGameMemory(address + 4, &package, sizeof(package)) ||
        !ReadGameMemory(package, name, sizeof(name) - 1) ||
        std::strcmp(name, "IG_PlayMovie.fng") != 0) return false;
    const uint32_t votes = unsigned(state.localSkipVote) | (unsigned(state.peerSkipVote) << 1) |
        (unsigned(state.skipDecided) << 2);
    if (gPromptOwner == address && gPromptTicket == state.ticket && gPromptVotes == votes) return true;
    void* label = gFindMovieElement(name, 0x599B8442);
    void* group = gFindMovieElement(name, 0xD3F7189E);
    uint32_t labelType = 0, groupType = 0;
    if (!ReadGameMemory(reinterpret_cast<uintptr_t>(label) + 0x18, &labelType, 4) || labelType != 2 ||
        !ReadGameMemory(reinterpret_cast<uintptr_t>(group) + 0x18, &groupType, 4) || groupType != 5) return false;
    if (gPromptOwner != address || gPromptTicket != state.ticket) {
        gAnimateMovieGroup(group, "SHOW", false);
        gShowMovieGroup(group);
        LogMessage("[INTRO_VOTE] Native caption ready: ENTER to vote to skip (2/2 required).\r\n");
    }
    char text[96]{};
    FormatSkipPrompt(state, text);
    gSetMovieLabel(name, 0x599B8442, "%s", text);
    gPromptOwner = address; gPromptTicket = state.ticket; gPromptVotes = votes;
    return true;
}

// Native 55B119 can continue the career even when 511BC0 refused to stop a
// decoder with a pending frame (+34). Retry only an expired intro's idle global
// player on the game thread. Never clear renderer/player globals by hand.
static bool TryRetireExpiredIntroMovie(DWORD flow) {
    if (flow != 6 || !IsCoopGameplayActive() || gCareerLaunchKind != 2 || !gNativeCareerReleasedTicket)
        return false;
    uint32_t movieFlow = 0, complete = 0, active = 0;
    uintptr_t movie = 0;
    uint8_t busy = 1;
    if (!ReadGameMemory(gMovieGlobals.flow, &movieFlow, 4) || movieFlow != 6 ||
        !ReadGameMemory(gMovieGlobals.complete, &complete, 4) || complete != 1 ||
        !ReadGameMemory(gMovieGlobals.active, &active, 4) || !active ||
        !ReadGameMemory(gMovieGlobals.player, &movie, 4) || !IsValidUserPtr(movie) ||
        !ReadGameMemory(movie + 0x34, &busy, 1) || busy) return false;
    gStopExpiredMovie();
    ReadGameMemory(gMovieGlobals.active, &active, 4);
    LogMessage(active ? "[INTRO_MOVIE] Native cleanup returned; movie layer still active.\r\n" :
        "[INTRO_MOVIE] Expired video layer released by native cleanup.\r\n");
    return !active;
}

struct Detour {
    uintptr_t address;
    const BYTE* expected;
    size_t length;
    void* replacement;
    BYTE* trampoline = nullptr;
    bool installed = false;
    bool inlineReplacement = false;
};

static void WriteRelativeJump(BYTE* location, const void* destination) {
    location[0] = 0xE9;
    const intptr_t distance = reinterpret_cast<intptr_t>(destination) -
        (reinterpret_cast<intptr_t>(location) + 5);
    const int32_t relative = static_cast<int32_t>(distance);
    std::memcpy(location + 1, &relative, sizeof(relative));
}

static bool MatchesNativeBytes(const Detour& detour) {
    BYTE current[16]{};
    return detour.length <= sizeof(current) &&
        ReadGameMemory(detour.address, current, detour.length) &&
        std::memcmp(current, detour.expected, detour.length) == 0;
}

static void PublishOriginal(const Detour& detour, bool useTrampoline) {
    const uintptr_t original = useTrampoline
        ? reinterpret_cast<uintptr_t>(detour.trampoline) : detour.address;
    if (detour.address == 0x527D30) {
        CoopPersonalSave::buildOffers=reinterpret_cast<CoopPersonalSave::OffersBuildFn>(original);
    } else if (detour.address == 0x4AF090) {
        gNewCareerButton=reinterpret_cast<NewCareerButtonFn>(original);
    } else if(detour.address==0x4AF0C0) {
        gResumeCareerButton=reinterpret_cast<NewCareerButtonFn>(original);
    } else if(detour.address==0x4AF0E0) {
        gLoadCareerButton=reinterpret_cast<NewCareerButtonFn>(original);
    } else if (detour.address == 0x4FD570) {
        gCareerDialog=reinterpret_cast<CareerDialogFn>(original);
    } else if (detour.address == kFreeRoamEntry) {
        gFreeRoam = reinterpret_cast<FreeRoamFn>(original);
    } else if (detour.address == kResumeEntry) {
        gResume = reinterpret_cast<ResumeFn>(original);
    } else if (detour.address == kCareerIntroEntry) {
        gCareerIntro = reinterpret_cast<CareerIntroFn>(original);
    } else if (detour.address == kMovieHandlerEntry) {
        gMovieHandler = reinterpret_cast<MovieHandlerFn>(original);
    }
}

static bool InstallDetour(Detour& detour) {
    if(!detour.inlineReplacement) {
    detour.trampoline = reinterpret_cast<BYTE*>(VirtualAlloc(nullptr, 32,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!detour.trampoline) return false;

    // All displaced blocks contain no relative branch or call.
    std::memcpy(detour.trampoline, detour.expected, detour.length);
    WriteRelativeJump(detour.trampoline + detour.length,
        reinterpret_cast<const void*>(detour.address + detour.length));
    FlushInstructionCache(GetCurrentProcess(), detour.trampoline, detour.length + 5);
    }

    DWORD oldProtection = 0;
    BYTE* target = reinterpret_cast<BYTE*>(detour.address);
    if (!VirtualProtect(target, detour.length, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        if(detour.trampoline)VirtualFree(detour.trampoline, 0, MEM_RELEASE);
        detour.trampoline = nullptr;
        return false;
    }
    // The adapter must already have a callable original when its entry changes.
    PublishOriginal(detour, !detour.inlineReplacement);
    WriteRelativeJump(target, detour.replacement);
    if (detour.length > 5) std::memset(target + 5, 0x90, detour.length - 5);
    FlushInstructionCache(GetCurrentProcess(), target, detour.length);
    DWORD ignored = 0;
    VirtualProtect(target, detour.length, oldProtection, &ignored);
    detour.installed = true;
    return true;
}

static void RollBackDetour(Detour& detour) {
    if (detour.installed) {
        DWORD oldProtection = 0;
        BYTE* target = reinterpret_cast<BYTE*>(detour.address);
        if (!VirtualProtect(target, detour.length, PAGE_EXECUTE_READWRITE, &oldProtection)) {
            // Keep the executable trampoline alive if restoration is unavailable.
            LogMessage("[CAREER_NATIVE] Could not restore native career hook bytes.\r\n");
            return;
        }
        std::memcpy(target, detour.expected, detour.length);
        FlushInstructionCache(GetCurrentProcess(), target, detour.length);
        DWORD ignored = 0;
        VirtualProtect(target, detour.length, oldProtection, &ignored);
        detour.installed = false;
    }
    PublishOriginal(detour, false);
    if (detour.trampoline) {
        VirtualFree(detour.trampoline, 0, MEM_RELEASE);
        detour.trampoline = nullptr;
    }
}

static void __cdecl HookFreeRoam() {
    if (!gReplayingCareer && gDeferCareer(1, nullptr)) return;
    gFreeRoam();
}

static void __declspec(noinline) __cdecl HookResume(uint32_t playerKey) {
    if(!ApplyNewCareerChoice(reinterpret_cast<uintptr_t>(_ReturnAddress())))return;
    const auto state = CoopSnapshot();
    uint32_t flow = 0;
    if (gReplayingCareer || !IsCoopGameplayActive() || !gProfileContext.IsCoopActive() ||
        !ReadGameMemory(gResumeGlobals.flow, &flow, 4) || flow != 3) {
        gResume(playerKey);
        return;
    }
    // Initial Client selection is owned by the Host checkpoint, not its keys.
    if (gCareerLaunchPending) return;
    if (state.role != coop::Role::Host || !state.joined || !state.fresh) {
        gDeferCareer(1, nullptr);
        return;
    }
    if (!gCompletedEvent(reinterpret_cast<void*>(gResumeGlobals.manager), 0xDD60E403)) {
        gResume(playerKey); // The native intro/free-roam paths already have barriers.
        return;
    }
    const int playerIndex = gPlayerIndex(playerKey);
    const uint32_t mode = 1;
    const uint8_t outsideCrib = 0, savedSpawn = 1;
    if (playerIndex < 0 || playerIndex > 3 ||
        !SafeWriteBytes(gResumeGlobals.playerIndex, &playerIndex, 4) ||
        !SafeWriteBytes(gResumeGlobals.mode, &mode, 4) ||
        !SafeWriteBytes(gResumeGlobals.crib, &outsideCrib, 1) ||
        !SafeWriteBytes(gResumeGlobals.savedSpawn, &savedSpawn, 1)) {
        LogMessage("[CAREER_NATIVE] Garage resume held: native player/context is invalid.\r\n");
        return;
    }
    // Match UI_CareerCrib's Return to Explore (4AF589..4AF61D), without
    // opening a Host-only garage. Do NOT select/reopen completed intro events.
    LogMessage("[CAREER_NATIVE] Initial garage Resume routed through the shared free-roam barrier.\r\n");
    HookFreeRoam();
}

static void __cdecl HookCareerIntro(const char* source) {
    if (!gReplayingCareer && gDeferCareer(2, source)) return;
    if (CoopMovieCaptureStart(6,source,"scene01",0)) return;
    gCareerIntro(source);
}

static bool CanSkipCurrentMovie(void* object) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(object);
    if (!IsValidUserPtr(address)) return false;
    float elapsed = 0.0f;
    uint8_t finished = 0;
    // These fields are read from this callback's live native object, never a
    // remembered object from a previous screen. Native input requires > 0.5s.
    return ReadGameMemory(address + 0x4C, &elapsed, sizeof(elapsed)) &&
        ReadGameMemory(address + 0x50, &finished, sizeof(finished)) &&
        std::isfinite(elapsed) && elapsed > 0.5f && finished == 0;
}

static void __fastcall HookMovieHandler(void* object, void*, uint32_t message,
    uintptr_t argument2, uintptr_t argument3, uintptr_t argument4) {
    if (CoopMovieHandleMessage(object,message,argument2,argument3,argument4)) return;
    // SP messages, including both skip inputs and natural completion, retain
    // the original thiscall ABI and all four native stack arguments.
    uint32_t movieFlow=0;
    ReadGameMemory(gMovieGlobals.flow,&movieFlow,sizeof(movieFlow));
    if (!IsCoopGameplayActive() || movieFlow!=6) {
        gMovieHandler(object, message, argument2, argument3, argument4);
        return;
    }

    if (message == kSkipInputA || message == kSkipInputB) {
        if (CanSkipCurrentMovie(object)) RequestCoopSkipVote();
        return;
    }

    const bool canSkip = CanSkipCurrentMovie(object);
    if (message == kFrameUpdate && canSkip) {
        // Read only the focused window's ENTER edge. Native skip events below
        // are also accepted and SessionCore makes repeated votes idempotent.
        const bool enter = IsGameWindowFocused() && (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        if (enter && !gEnterHeld) RequestCoopSkipVote();
        gEnterHeld = enter;
        UpdateMovieSkipPrompt(object, CoopSnapshot());
    } else if (message == kFrameUpdate) {
        gEnterHeld = false;
        gPromptOwner = 0; // Next playlist item may reuse the same object address.
    }
    if (message == kFrameUpdate && IsCoopSkipApproved() && canSkip) {
        // Replay the native skip action on the game thread. It stops the movie,
        // advances the playlist and marks completion; the normal frame update
        // below performs the native screen cleanup and career continuation.
        gMovieHandler(object, kSkipInputB, 0, 0, 0);
    }
    gMovieHandler(object, message, argument2, argument3, argument4);
}

static constexpr BYTE kFreeRoamBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
static constexpr BYTE kResumeBytes[] = {0x55, 0x8B, 0xEC, 0x68, 0x03, 0xE4, 0x60, 0xDD};
static constexpr BYTE kCareerIntroBytes[] = {
    0xC7, 0x05, 0x68, 0x85, 0x83, 0x00, 0x06, 0x00, 0x00, 0x00
};
static constexpr BYTE kMovieHandlerBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08};
static constexpr BYTE kCareerDialogBytes[] = {0x8B,0x44,0x24,0x10,0x8B,0x54,0x24,0x08};
static constexpr BYTE kCareerButtonBytes[]={0x81,0x7C,0x24,0x08,0x10,0x72,0x40,0x0C};
static constexpr BYTE kNewRowBytes[]={0x6A,0x48,0xE8,0xD7,0x63,0x0C,0x00};

static Detour gDetours[] = {
    {kFreeRoamEntry, kFreeRoamBytes, sizeof(kFreeRoamBytes), reinterpret_cast<void*>(&HookFreeRoam)},
    {kResumeEntry, kResumeBytes, sizeof(kResumeBytes), reinterpret_cast<void*>(&HookResume)},
    {kCareerIntroEntry, kCareerIntroBytes, sizeof(kCareerIntroBytes), reinterpret_cast<void*>(&HookCareerIntro)},
    {kMovieHandlerEntry, kMovieHandlerBytes, sizeof(kMovieHandlerBytes), reinterpret_cast<void*>(&HookMovieHandler)},
    {0x4FD570,kCareerDialogBytes,sizeof(kCareerDialogBytes),reinterpret_cast<void*>(&HookCareerDialog)},
    {0x4AF090,kCareerButtonBytes,sizeof(kCareerButtonBytes),reinterpret_cast<void*>(&HookNewCareerButton)},
    {0x4AF0C0,kCareerButtonBytes,sizeof(kCareerButtonBytes),reinterpret_cast<void*>(&HookResumeCareerButton)},
    {0x4AF0E0,kCareerButtonBytes,sizeof(kCareerButtonBytes),reinterpret_cast<void*>(&HookLoadCareerButton)},
    {0x4AF242,kNewRowBytes,sizeof(kNewRowBytes),reinterpret_cast<void*>(&HookNewCareerRow),nullptr,false,true}
};

} // namespace CoopCareerNative

void TickCoopCareerPresentation(DWORD flow) {
    auto& presentation = CoopCareerNative::gIntroPresentation;
    if (!presentation.armed || flow != 6 || !IsCoopGameplayActive() ||
        gCareerLaunchKind != 2 || !gNativeCareerReleasedTicket) return;
    const DWORD now = GetTickCount();
    if (!presentation.started) presentation.started = now;
    // Limit diagnostics/cleanup to the initial 30 seconds, not ordinary movies
    // or a later race. All object pointers here are freshly read each time.
    if (now - presentation.started > 30000) { presentation.armed = false; return; }
    if (!presentation.lastCleanup || now - presentation.lastCleanup >= 250) {
        presentation.lastCleanup = now;
        CoopCareerNative::TryRetireExpiredIntroMovie(flow);
    }
    if (presentation.samples >= 8 || (presentation.samples && now - presentation.lastLog < 1000)) return;
    presentation.lastLog = now; ++presentation.samples;
    uint32_t movieFlow = 0, complete = 0, active = 0, mode = ~uint32_t(0);
    uintptr_t movie = 0, director = 0, camera = 0, view = 0;
    uint32_t viewFlags[3]{};
    uint8_t busy = 0, intro = 0;
    ReadGameMemory(CoopCareerNative::gMovieGlobals.flow, &movieFlow, 4);
    ReadGameMemory(CoopCareerNative::gMovieGlobals.complete, &complete, 4);
    ReadGameMemory(CoopCareerNative::gMovieGlobals.active, &active, 4);
    ReadGameMemory(CoopCareerNative::gMovieGlobals.player, &movie, 4);
    if (IsValidUserPtr(movie)) ReadGameMemory(movie + 0x34, &busy, 1);
    ReadGameMemory(0x85EF6C, &intro, 1); ReadGameMemory(0x8900AC, &director, 4);
    if (IsValidUserPtr(director)) ReadGameMemory(director + 0x238, &camera, 4);
    if (IsValidUserPtr(camera)) {
        ReadGameMemory(camera + 0xC, &mode, 4); ReadGameMemory(camera + 0x1C, &view, 4);
    }
    if (IsValidUserPtr(view)) {
        ReadGameMemory(view + 0x270, &viewFlags[0], 4);
        ReadGameMemory(view + 0x27C, &viewFlags[1], 4);
        ReadGameMemory(view + 0x280, &viewFlags[2], 4);
    }
    char text[320]{};
    std::snprintf(text, sizeof(text), "[CAREER_VIEW] movieFlow=%u complete=%u active=%u busy=%u movie=%08X intro=%u camera=%08X mode=%u view=%08X flags=%08X/%08X/%08X shields=%u\r\n",
        movieFlow, complete, active, busy, unsigned(movie), intro, unsigned(camera), mode,
        unsigned(view), viewFlags[0], viewFlags[1], viewFlags[2], CoopCameraShieldCount());
    LogMessage(text);
}

bool InstallNativeCoopCareerHooks() {
    using namespace CoopCareerNative;
    if (gHooksInstalled) return true;
    for (const Detour& detour : gDetours) {
        if (!MatchesNativeBytes(detour)) {
            LogMessage("[CAREER_NATIVE] Native career/movie bytes differ; hooks were not installed.\r\n");
            return false;
        }
    }

    for (Detour& detour : gDetours) {
        if (!InstallDetour(detour)) {
            for (Detour& rollback : gDetours) RollBackDetour(rollback);
            LogMessage("[CAREER_NATIVE] Native career/movie hook installation failed.\r\n");
            return false;
        }
    }
    gHooksInstalled = true;
    LogMessage("[CAREER_NATIVE] Native career launch barrier and joint movie skip hooks installed.\r\n");
    return true;
}

bool IsNativeCoopCareerSourceReady(const char* source) {
    if (!source || !source[0]) return false;
    uintptr_t uiData = 0;
    if (!ReadGameMemory(0x008384C4, &uiData, sizeof(uiData)) || !IsValidUserPtr(uiData)) return false;
    // UIData::FindPackage traverses the native queue without modifying it.
    // ShowFNG(5377A0) requires this node and dereferences it without a null check.
    using FindPackageFn = void*(__thiscall*)(void*, const char*);
    return reinterpret_cast<FindPackageFn>(0x0051CCD0)(
        reinterpret_cast<void*>(uiData), source) != nullptr;
}

static bool PrepareReceivedCoopCareerManagers() {
    using namespace CoopCareerNative;
    uintptr_t event = 0;
    uint32_t hash = 0;
    if (!ReadGameMemory(gCareerManagerGlobals.career + 0x8738, &event, 4)) return false;
    if (!event) return true;
    if (!IsValidUserPtr(event) || !ReadGameMemory(event + 8, &hash, 4) || !hash) return false;
    // Rehydrate the same managers as SetActiveEvent(5410E0), but not its
    // completion-record write. CareerRead already restored the active pointer.
    gPrepareCareerEvent(reinterpret_cast<void*>(gCareerManagerGlobals.career + 0xD24),
        reinterpret_cast<void*>(event));
    gPrepareRaceEvent(reinterpret_cast<void*>(gCareerManagerGlobals.race),
        reinterpret_cast<void*>(event));
    return true;
}

bool ReplayNativeCoopCareer(uint32_t kind, const char* source) {
    using namespace CoopCareerNative;
    if (kind == 2 && !IsNativeCoopCareerSourceReady(source)) {
        LogMessage("[CAREER_NATIVE] Intro held: source UI package is not ready.\r\n");
        return false;
    }
    const auto session=CoopSnapshot();char trace[272];std::snprintf(trace,sizeof(trace),
        "[CAREER_ENTRY] session=%llu ticket=%u role=%u kind=%u source='%s' personalStarter=%u; native request only, loading is not yet complete.\r\n",
        static_cast<unsigned long long>(session.sessionId),session.ticket,unsigned(session.role),kind,source?source:"",unsigned(CoopPersonalSave::personalStarter));LogMessage(trace);
    struct ReplayScope {
        bool previous;
        ReplayScope() : previous(gReplayingCareer) { gReplayingCareer = true; }
        ~ReplayScope() { gReplayingCareer = previous; }
    } replayScope;
    if (kind == 1) {
        gIntroPresentation = {};
        if (CoopSnapshot().role == coop::Role::Client) {
            // A first-time participant keeps the original personal DDAY A/B
            // path and borrowed preset, not the Host's already-purchased car.
            if(!gCompletedEvent(reinterpret_cast<void*>(gResumeGlobals.manager),0xDD60E403)) {
                uint8_t started=0;ReadGameMemory(gFreshCareerAddresses.started,&started,1);
                if(!started) {gIntroPresentation.armed=true;gCareerLaunchKind=2;}
                gResume(1);
                LogMessage("[CAREER_NATIVE] Player 2 personal onboarding resumed; common stages wait for its own starter.\r\n");
                return true;
            }
            if (!PrepareReceivedCoopCareerManagers()) {
                LogMessage("[CAREER_NATIVE] Received resume held: active event is invalid.\r\n");
                return false;
            }
        }
        gFreeRoam();
    } else if (kind == 2 && source && source[0]) {
        // source must identify an existing native UI package. For clients,
        // initialize UI_StartCareer.fng before replaying its intro transition.
        gIntroPresentation = {}; gIntroPresentation.armed = true;
        gPromptOwner = 0; gEnterHeld = false;
        HookCareerIntro(source);
    } else {
        LogMessage("[CAREER_NATIVE] Invalid deferred native career action.\r\n");
        return false;
    }
    return true;
}
