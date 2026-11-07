// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Included inside the game's adapter namespace. No native engine work on the UDP thread.
#include "CoopCareerProgress.h"
#include <bcrypt.h>
#pragma comment(lib,"bcrypt.lib")
static SRWLOCK gSessionLock = SRWLOCK_INIT;
static coop::SessionCore gSession;
static coop_save::ProfileContext gProfileContext;
static coop_checkpoint::Sender gCheckpointSender;
static coop_checkpoint::Receiver gCheckpointReceiver;
static uint32_t gCheckpointSerial = 0;
static bool gCareerLaunchPending = false;
static uint32_t gNativeCareerReleasedTicket = 0;
static uint32_t gAcceptedNewCareerSelectionTicket = 0;
static uint32_t gCareerLaunchKind = 1;
static std::string gCareerLaunchSource;
static std::atomic<uint32_t> gReceivedLaunchKind{1};
static uintptr_t gCoopNativeProfileName=0x83A9E0; // Private fixtures replace this read-only cell only.
static bool gRestoreSpPending = false;
static bool gCoopProfileSelectionReady = false;
static bool gOpenLobbyPending = false;
static bool gLocalSessionStarted = false;
static bool gLocalResumePending = false;
static DWORD gProfileSelectionRetryAt = 0;
static bool gProfileSelectionReadHeld = false;
static bool gCloseLobbyPending = false;
static std::atomic<bool> gCoopCareerHooksReady{false};
static BYTE gOriginalRenderBranch=0;
static bool gRenderBranchCaptured=false;
static coop_save::NativeProfileSnapshot gPreLobbySingleplayer;
static std::string gCurrentNativeScreen;
static const char* gOriginalSaveRootName = nullptr;
static std::string gNativeSaveRootName;
static std::wstring gOriginalSaveRoot;
static std::wstring gCoopBaseRoot;
static std::string gRestoredHostProfile;
static bool gAutoResumeBackup=false;
static std::atomic<uint32_t> gRejectedRestoreTicket{0};
static uint64_t gPendingRestoreClearSession=0;
static uint32_t gPendingRestoreClearTicket=0;
static std::string gPendingRestoreClearProfile;
static DWORD gLastCheckpointPoll = 0;
static std::atomic<bool> gNativeSaveCompleted{false};
static SRWLOCK gSaveHandleLock = SRWLOCK_INIT;
struct CoopWriteHandle { HANDLE handle = INVALID_HANDLE_VALUE; std::wstring path; };
static std::array<CoopWriteHandle, 16> gCoopWriteHandles;
using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
using MoveFileAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR);
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static CloseHandleFn gNativeCloseHandle = nullptr;
static MoveFileAFn gNativeMoveFile = nullptr;
static CreateFileWFn gNativeCreateFileW = nullptr;
static bool CoopSpecialNativeSaveIatReady(const uintptr_t* privateSlots=nullptr);
static bool ReacquireCoopSaveCreateImports(const uintptr_t* privateSlots=nullptr);
static bool CoopSpecialPrepareMenuPreview(const char*,const char*,std::vector<uint8_t>&);
static thread_local coop_backup::Action gCoopBackupAction=coop_backup::Action::CareerSave;
static std::mutex gCoopBackupIdentityMutex;
static std::wstring gCoopBackupCampaign,gCoopBackupPersonal;
struct CoopBackupActionScope {
    coop_backup::Action previous;
    explicit CoopBackupActionScope(coop_backup::Action action):previous(gCoopBackupAction){gCoopBackupAction=action;}
    ~CoopBackupActionScope(){gCoopBackupAction=previous;}
};
static void CaptureCompletedCoopSave(const std::wstring& path) noexcept;
bool IsNativeCoopCareerSourceReady(const char*);
// Game-thread cache fencing is idempotent for an unchanged session/ticket;
// valid selection bytes received early for that generation remain usable.
static void ResetCoopPeerSelectionGeneration();

static std::wstring WideText(const char* text) {
    if (!text) return {};
    int count = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (count <= 0 || count > 32768) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, &result[0], count);
    result.resize(count - 1); return result;
}
static void SetCoopBackupIdentity(const std::string& campaign,const std::string& personal) noexcept {
    try {
        auto wideCampaign=WideText(campaign.c_str()),widePersonal=WideText(personal.c_str());
        std::lock_guard<std::mutex> lock(gCoopBackupIdentityMutex);
        gCoopBackupCampaign.swap(wideCampaign);gCoopBackupPersonal.swap(widePersonal);
    } catch(...) {}
}
static std::string NarrowText(const std::wstring& text) {
    BOOL substituted = FALSE;
    int count = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, &substituted);
    if (count <= 0 || count > MAX_PATH || substituted) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, &result[0], count, nullptr, &substituted);
    if (substituted) return {};
    result.resize(count - 1); return result;
}
static coop::SessionSnapshot CoopSnapshot() {
    AcquireSRWLockShared(&gSessionLock);
    auto state = gSession.Snapshot(GetTickCount());
    ReleaseSRWLockShared(&gSessionLock); return state;
}
static coop::SessionSnapshot CoopUiSnapshot() {
    auto state=CoopSnapshot();
    state.startAllowed=state.joined&&state.fresh&&state.localInMainMenu&&
        !state.resumeRequestPending&&(state.phase==coop::Phase::Lobby||state.phase==coop::Phase::Playing)&&
        (state.role==coop::Role::Host||(state.role==coop::Role::Client&&state.careerLaunched));
    if(!gCoopCareerHooksReady || !CoopModsReady(state)) state.startAllowed=false;
    return state;
}
static void SetCoopRenderBranch(bool active) {
    if(!gRenderBranchCaptured) { gOriginalRenderBranch=*reinterpret_cast<BYTE*>(0x60661B); gRenderBranchCaptured=true; }
    BYTE value=active?0xEB:gOriginalRenderBranch;
    DWORD protect=0;
    if(VirtualProtect(reinterpret_cast<void*>(0x60661B),1,PAGE_EXECUTE_READWRITE,&protect)) {
        *reinterpret_cast<BYTE*>(0x60661B)=value;
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(0x60661B),1);
        DWORD unused=0; VirtualProtect(reinterpret_cast<void*>(0x60661B),1,protect,&unused);
    }
}
static sockaddr_in SessionAddress(coop::Endpoint endpoint) {
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = endpoint.address; address.sin_port = htons(endpoint.port); return address;
}
static std::atomic<uint64_t> gCoopTxBytes{0},gCoopTxPackets{0},gCoopTxErrors{0};
static std::atomic<uint32_t> gCoopTxLastError{0};
static int SendMeasuredCoopDatagram(const void* data,size_t bytes,coop::Endpoint endpoint) {
    if (!endpoint.Valid() || gGameSocket == INVALID_SOCKET) return SOCKET_ERROR;
    const auto policy=coop_network::Classify(data,bytes,kPacketMagic,sizeof(PositionPacket));
    if(!policy.valid){WSASetLastError(WSAEMSGSIZE);return SOCKET_ERROR;}
    if(!policy.discovery){
        // Acceptance is local queueing only. Remote readiness and each game
        // transaction still require their own actual receive/ACK/commit path.
        if(gCoopTransport.Queue(data,bytes,endpoint,policy.delivery,policy.stream,GetTickCount()))return static_cast<int>(bytes);
        WSASetLastError(WSAEWOULDBLOCK);return SOCKET_ERROR;
    }
    const auto address = SessionAddress(endpoint);
    const int sent=sendto(gGameSocket, static_cast<const char*>(data), static_cast<int>(bytes), 0,
        reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if(sent==static_cast<int>(bytes)) {
        gCoopTxBytes.fetch_add(bytes,std::memory_order_relaxed);
        gCoopTxPackets.fetch_add(1,std::memory_order_relaxed);
    } else {
        gCoopTxErrors.fetch_add(1,std::memory_order_relaxed);
        gCoopTxLastError.store(sent==SOCKET_ERROR?WSAGetLastError():WSAEMSGSIZE,std::memory_order_relaxed);
    }
    return sent;
}
static void SendSessionBytes(const void* data, size_t bytes, coop::Endpoint endpoint) {
    SendMeasuredCoopDatagram(data,bytes,endpoint);
}
static bool NativeSerializeProfileImpl(uint8_t* output) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*,void*,uint32_t)>(0x0052A280)(
            reinterpret_cast<void*>(0x0083A9E0), output, static_cast<uint32_t>(coop_save::kNativeProfilePayloadSize));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool NativeDeserializeProfileImpl(const uint8_t* input) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*,const void*,uint32_t,int)>(0x005344D0)(
            reinterpret_cast<void*>(0x0083A9E0), input, static_cast<uint32_t>(coop_save::kNativeProfilePayloadSize), 0);
        return true;
    } __except([](EXCEPTION_POINTERS* fault)->LONG {
        static DWORD reported=0;const DWORD now=GetTickCount();
        if(!reported||now-reported>=3000){reported=now;char line[192]{};
            std::snprintf(line,sizeof(line),"[SAVE_SELECT] Native profile reader exception code=%08lX address=%08X; no career replay authorized.\r\n",
                fault&&fault->ExceptionRecord?fault->ExceptionRecord->ExceptionCode:0,
                fault&&fault->ExceptionRecord?unsigned(uintptr_t(fault->ExceptionRecord->ExceptionAddress)):0);LogMessage(line);}
        return EXCEPTION_EXECUTE_HANDLER;
    }(GetExceptionInformation())) { return false; }
}
// Private fixtures replace only these adapters; production retains the exact
// stock serializers. No full profile reader is ever used by live progress sync.
static bool (*gSerializeNativeProfile)(uint8_t*)=NativeSerializeProfileImpl;
static bool (*gDeserializeNativeProfile)(const uint8_t*)=NativeDeserializeProfileImpl;
static bool NativeSerializeProfile(uint8_t* output) {return output && gSerializeNativeProfile(output);}
static bool NativeDeserializeProfile(const uint8_t* input) {return input && gDeserializeNativeProfile(input);}
struct ProfileMetadata {
    uint32_t gameMode = 0, playerIndex = 0, selectedCar = 0, careerCar = 0;
    uint8_t loaded = 0, started = 0, autosave[2]{}, crib = 0;
};
// The vanilla New Career confirmation calls Career::Reset(1). Clearing only
// loaded/started bypasses that confirmation and leaves the SP garage/event in
// RAM. Use the narrow career reset, not UserProfile/manager initialization,
// which also resets other profiles and global options.
struct FreshCareerNativeAddresses {
    uintptr_t entry = 0x532830, manager = 0x85AD40;
    uintptr_t flow = kGameFlowState, loaded = 0x83AA00, started = 0x86347C;
};
static FreshCareerNativeAddresses gFreshCareerAddresses;
using ResetCareerFn = void(__thiscall*)(void*, int);
static ResetCareerFn gResetNativeCareer = reinterpret_cast<ResetCareerFn>(0x532830);
static bool PrepareFreshCoopCareer() {
    if (!gProfileContext.IsCoopActive()) return false;
    uint32_t flow = 0;
    const BYTE expected[12]{0x53,0x33,0xDB,0x56,0x8B,0xF1,0x57,0x88,0x9E,0x38,0x71,0x00};
    BYTE actual[12]{};
    if (!ReadGameMemory(gFreshCareerAddresses.flow, &flow, 4) || flow != 3 ||
        !ReadGameMemory(gFreshCareerAddresses.entry, actual, sizeof(actual)) ||
        std::memcmp(actual, expected, sizeof(actual))) return false;
    __try {
        gResetNativeCareer(reinterpret_cast<void*>(gFreshCareerAddresses.manager), 1);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    const uint8_t clear = 0;
    return SafeWriteBytes(gFreshCareerAddresses.loaded, &clear, 1) &&
        SafeWriteBytes(gFreshCareerAddresses.started, &clear, 1);
}
static coop_save::NativeProfileAdapter ProfileAdapter() {
    coop_save::NativeProfileAdapter adapter;
    adapter.captureSingleplayer = [](coop_save::NativeProfileSnapshot& snapshot) {
        if(!gPreLobbySingleplayer.payload.empty()) { snapshot=gPreLobbySingleplayer; return true; }
        snapshot.payload.resize(coop_save::kNativeProfilePayloadSize);
        if (!NativeSerializeProfile(snapshot.payload.data())) return false;
        ProfileMetadata meta{};
        if (!ReadGameMemory(0x850078,&meta.gameMode,4) || !ReadGameMemory(0x85AD28,&meta.playerIndex,4) ||
            !ReadGameMemory(0x863480,&meta.selectedCar,4) || !ReadGameMemory(0x83AA00,&meta.loaded,1) ||
            !ReadGameMemory(0x86347C,&meta.started,1) || !ReadGameMemory(0x844AD8,&meta.autosave,2) ||
            !ReadGameMemory(0x836324,&meta.crib,1) || !ReadGameMemory(0x83A9D8,&meta.careerCar,4)) return false;
        snapshot.metadata.resize(sizeof(meta)); std::memcpy(snapshot.metadata.data(),&meta,sizeof(meta)); return true;
    };
    adapter.setSaveRoot = [](const std::wstring& path) {
        std::string ansi = NarrowText(path); if (ansi.empty()) return false;
        uintptr_t manager = 0, wrapper = 0, device = 0;
        if (!ReadGameMemory(0x836498,&manager,4) || !IsValidUserPtr(manager) ||
            !ReadGameMemory(manager+0x5C,&wrapper,4) || !IsValidUserPtr(wrapper) ||
            !ReadGameMemory(wrapper,&device,4) || !IsValidUserPtr(device)) return false;
        if (!SafeWriteBytes(device+0x174,ansi.c_str(),ansi.size()+1)) return false;
        char rootLine[1000]{};uintptr_t enumSlot=0;
        ReadGameMemory(0x783128,&enumSlot,sizeof(enumSlot));
        std::snprintf(rootLine,sizeof(rootLine),"[SAVE_ROOT] Native device=%08X root='%s' SP=%u FindFirst=%08X owned=%08X original=%08X.\r\n",
            unsigned(device),ansi.c_str(),coop_save::EqualPathText(path,gOriginalSaveRoot)?1u:0u,
            unsigned(enumSlot),unsigned(reinterpret_cast<uintptr_t>(HookedFindFirstFileA)),
            unsigned(reinterpret_cast<uintptr_t>(g_pfnOriginalFindFirstFileA)));LogMessage(rootLine);
        if (coop_save::EqualPathText(path,gOriginalSaveRoot)) {
            return SafeWriteBytes(0x7F66A0,&gOriginalSaveRootName,sizeof(gOriginalSaveRootName));
        }
        std::wstring relative = path.substr(path.find_last_of(L'\\')+1);
        gNativeSaveRootName = std::string(coop_build::kCoopSaveFolder)+"\\"+NarrowText(relative);
        const char* pointer = gNativeSaveRootName.c_str();
        return SafeWriteBytes(0x7F66A0,&pointer,sizeof(pointer));
    };
    adapter.restoreSingleplayer = [](const coop_save::NativeProfileSnapshot& snapshot) {
        if (snapshot.payload.size()!=coop_save::kNativeProfilePayloadSize || snapshot.metadata.size()!=sizeof(ProfileMetadata)) return false;
        if (!NativeDeserializeProfile(snapshot.payload.data())) return false;
        ProfileMetadata meta{}; std::memcpy(&meta,snapshot.metadata.data(),sizeof(meta));
        return SafeWriteBytes(0x850078,&meta.gameMode,4) && SafeWriteBytes(0x85AD28,&meta.playerIndex,4) &&
            SafeWriteBytes(0x863480,&meta.selectedCar,4) && SafeWriteBytes(0x83AA00,&meta.loaded,1) &&
            SafeWriteBytes(0x86347C,&meta.started,1) && SafeWriteBytes(0x844AD8,&meta.autosave,2) &&
            SafeWriteBytes(0x836324,&meta.crib,1) && SafeWriteBytes(0x83A9D8,&meta.careerCar,4);
    }; return adapter;
}
static bool ConfigureCoopProfileContext() {
    // Ultimate ASI Loader restores its temporary CreateFileA/W imports AFTER
    // loading plugins. Our worker may have installed before that unwind. Own
    // them again only if they are exactly the canonical Windows functions;
    // never recapture arbitrary hooks (or our own hook) as call-through ports.
    // Root discovery also runs before initial IAT installation. No native
    // CO-OP routing can be enabled there; full admission still requires all7.
    if((g_pfnOriginalCreateFileA||gNativeCreateFileW)&&!ReacquireCoopSaveCreateImports())return false;
    if (!gOriginalSaveRoot.empty()) {
        if(CoopSpecialNativeSaveIatReady()&&!gSpecialSaveFence.ObserveRoot(gCoopBaseRoot))return false;
        return true;
    }
    wchar_t local[MAX_PATH]{};
    if (SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,0,local)!=S_OK) return false;
#ifdef UNDERCOOP_MOD_MANAGER
    if(!CoopModManager::state.ready||CoopModManager::state.spFolder.empty())return false;
    gOriginalSaveRoot = std::wstring(local)+L"\\"+CoopModManager::state.spFolder;
#else
    gOriginalSaveRoot = std::wstring(local)+L"\\NFS Underground 2";
#endif
    gCoopBaseRoot = std::wstring(local)+L"\\"+coop_build::kCoopSaveFolderWide;
    char rootLine[1300]{};
    std::snprintf(rootLine,sizeof(rootLine),"[SAVE_ROOT] Bind SP='%s' COOP='%s' configured='%s'.\r\n",
        NarrowText(gOriginalSaveRoot).c_str(),NarrowText(gCoopBaseRoot).c_str(),gConfiguredCareerProfile);LogMessage(rootLine);
    if (!ReadGameMemory(0x7F66A0,&gOriginalSaveRootName,sizeof(gOriginalSaveRootName))) return false;
    return gProfileContext.Configure(gOriginalSaveRoot,gCoopBaseRoot)&&
        (!CoopSpecialNativeSaveIatReady()||gSpecialSaveFence.ObserveRoot(gCoopBaseRoot));
}
bool RouteContextSavePath(const char* input,char* output,size_t capacity) {
    if (!input || !output || !capacity) return false;
    auto routed = gProfileContext.RoutePath(WideText(input),true);
    if (routed.decision==coop_save::PathDecision::Unchanged) return false;
    output[0]='\0';
    if (routed.decision==coop_save::PathDecision::Reject) { SetLastError(ERROR_ACCESS_DENIED); return true; }
    auto ansi = NarrowText(routed.path);
    if (ansi.empty() || ansi.size()>=capacity) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return true; }
    std::memcpy(output,ansi.c_str(),ansi.size()+1); return true;
}
void TraceCoopProfileEnumeration(const char* requested,const char* routed,HANDLE found,const WIN32_FIND_DATAA* first,DWORD error) {
    if(!gProfileContext.IsCoopActive())return;
    // The game can poll filesystem state repeatedly. Diagnostics are bounded
    // and never change native results, paths, metadata or LastError.
    static DWORD windowStart=0;static unsigned count=0;
    const DWORD now=GetTickCount();if(now-windowStart>=10000){windowStart=now;count=0;}
    if(count++>=32)return;
    char line[1600]{};
    std::snprintf(line,sizeof(line),"[SAVE_ENUM] requested='%.400s' routed='%.400s' success=%u error=%lu first='%.260s' active='%s'.\r\n",
        requested?requested:"(null)",routed?routed:"(null)",found!=INVALID_HANDLE_VALUE?1u:0u,
        found!=INVALID_HANDLE_VALUE?0ul:error,found!=INVALID_HANDLE_VALUE&&first?first->cFileName:"",
        NarrowText(gProfileContext.ActiveRoot()).c_str());LogMessage(line);
}
static bool SafeCoopDirectory(const std::wstring& path) {
    std::wstring canonical;
    if (!coop_save::CanonicalWindowsPath(path,canonical) || !coop_save::PathIsWithin(canonical,gCoopBaseRoot)) return false;
    // Reject junctions/symlinks in every existing component of the write path.
    for (size_t end=3; end<=canonical.size(); ++end) {
        if (end<canonical.size() && canonical[end]!=L'\\') continue;
        auto component=canonical.substr(0,end);
        DWORD attributes=GetFileAttributesW(component.c_str());
        if (attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    if (GetFileAttributesW(gCoopBaseRoot.c_str())==INVALID_FILE_ATTRIBUTES &&
        !CreateDirectoryW(gCoopBaseRoot.c_str(),nullptr)) return false;
    std::wstring relative=canonical.substr(gCoopBaseRoot.size());
    std::wstring part=gCoopBaseRoot;
    size_t cursor=relative.empty()?0:1;
    while (cursor<relative.size()) {
        size_t slash=relative.find(L'\\',cursor);
        part+=L"\\"+relative.substr(cursor,slash==std::wstring::npos?slash:slash-cursor);
        DWORD attributes=GetFileAttributesW(part.c_str());
        if (attributes==INVALID_FILE_ATTRIBUTES) { if(!CreateDirectoryW(part.c_str(),nullptr)) return false; }
        else if (!(attributes&FILE_ATTRIBUTE_DIRECTORY) || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (slash==std::wstring::npos) break; cursor=slash+1;
    }
    return true;
}
static bool ReadCoopFile(const std::wstring& path,std::vector<uint8_t>& file) {
    file.clear(); HANDLE handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE) return false;
    DWORD length=GetFileSize(handle,nullptr),read=0;
    DWORD failure=length==INVALID_FILE_SIZE?GetLastError():ERROR_INVALID_DATA;
    bool okay=length==coop_save::kNativeProfileFileSize;
    if(okay) { file.resize(length); okay=ReadFile(handle,file.data(),length,&read,nullptr)!=FALSE && read==length;
        if(!okay)failure=GetLastError()?GetLastError():ERROR_READ_FAULT; }
    CloseHandle(handle);
    if(!okay){SetLastError(failure);return false;}
    if(coop_save::ValidateNativeProfileFile(file.data(),file.size())!=coop_save::SaveValidation::Valid){SetLastError(ERROR_INVALID_DATA);return false;}
    return true;
}
void TrackCoopWriteHandle(HANDLE handle,const char* path,DWORD access) {
    if(handle==INVALID_HANDLE_VALUE || !(access&(GENERIC_WRITE|FILE_WRITE_DATA|FILE_APPEND_DATA)) || !gProfileContext.IsCoopActive()) return;
    std::wstring canonical;
    if(!coop_save::CanonicalWindowsPath(WideText(path),canonical) || !coop_save::PathIsWithin(canonical,gProfileContext.ActiveRoot())) return;
    AcquireSRWLockExclusive(&gSaveHandleLock);
    for(auto& item:gCoopWriteHandles) {
        if(item.handle==INVALID_HANDLE_VALUE) { item.handle=handle; item.path=std::move(canonical); break; }
    }
    ReleaseSRWLockExclusive(&gSaveHandleLock);
}
bool RouteClientCoopWrite(const char* path,char* output,size_t capacity,DWORD access) {
    // Root routing already isolates Client from Host and SP. Its native save
    // is now the authoritative PERSONAL file, not disposable staging beside a
    // Host mirror. Preserve the real engine temp/rename/save-completion flow.
    return false;
}
static BOOL WINAPI CoopCloseHandle(HANDLE handle) {
    BOOL okay=FALSE;DWORD error=0;std::wstring path;
    {
        auto specialIo=gSpecialSaveFence.Lock();
        okay=gNativeCloseHandle(handle);error=GetLastError();
        specialIo.Closed(uintptr_t(handle),okay!=FALSE);
        if(okay) {
            AcquireSRWLockExclusive(&gSaveHandleLock);
            for(auto& item:gCoopWriteHandles) {
                if(item.handle==handle) { path=std::move(item.path); item.handle=INVALID_HANDLE_VALUE; break; }
            }
            ReleaseSRWLockExclusive(&gSaveHandleLock);
        }
    }
    if(okay && !path.empty()) {
        CaptureCompletedCoopSave(path);
        gNativeSaveCompleted.store(true);
    }
    SetLastError(error);
    return okay;
}
static HANDLE WINAPI CoopCreateFileW(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE templateHandle) {
    auto routed=gProfileContext.RoutePath(path?path:L"",false);
    if(routed.decision==coop_save::PathDecision::Reject) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    const wchar_t* actual=routed.decision==coop_save::PathDecision::Redirect?routed.path.c_str():path;
    auto specialIo=gSpecialSaveFence.Lock();const std::wstring specialPath=actual?actual:L"";
    if(!specialIo.MayOpen(specialPath,access,creation)) {SetLastError(ERROR_BUSY);return INVALID_HANDLE_VALUE;}
    HANDLE result=gNativeCreateFileW(actual,access,share,security,creation,flags,templateHandle);
    if(!specialIo.Opened(uintptr_t(result),specialPath,access,creation)) {
        CloseHandle(result);SetLastError(ERROR_TOO_MANY_OPEN_FILES);return INVALID_HANDLE_VALUE;
    }
    auto ansi=actual?NarrowText(actual):std::string{}; TrackCoopWriteHandle(result,ansi.c_str(),access); return result;
}
static BOOL WINAPI CoopMoveFileA(LPCSTR from,LPCSTR to) {
    char redirectedFrom[MAX_PATH]{},redirectedTo[MAX_PATH]{};
    const char* source=RouteContextSavePath(from,redirectedFrom,sizeof(redirectedFrom))?redirectedFrom:from;
    const char* target=RouteContextSavePath(to,redirectedTo,sizeof(redirectedTo))?redirectedTo:to;
    BOOL okay=FALSE;DWORD error=0;std::wstring path;
    {
        auto specialIo=gSpecialSaveFence.Lock();
        if(!specialIo.MayMove(WideText(source),WideText(target))) {SetLastError(ERROR_BUSY);return FALSE;}
        okay=gNativeMoveFile(source,target);error=GetLastError();
        if(okay && target && gProfileContext.IsCoopActive()) {
            std::wstring canonical;
            if(coop_save::CanonicalWindowsPath(WideText(target),canonical) && coop_save::PathIsWithin(canonical,gProfileContext.ActiveRoot())) {
                path=std::move(canonical);
            }
        }
    }
    if(!path.empty()) {
        CaptureCompletedCoopSave(path);
        gNativeSaveCompleted.store(true);
    }
    SetLastError(error);
    return okay;
}
bool InstallCoopSaveCompletionHooks() {
    struct Entry { uintptr_t address; void* hook; void** original; };
    Entry entries[]={{0x783144,reinterpret_cast<void*>(&CoopCloseHandle),reinterpret_cast<void**>(&gNativeCloseHandle)},
        {0x7831D4,reinterpret_cast<void*>(&CoopCreateFileW),reinterpret_cast<void**>(&gNativeCreateFileW)},
        {0x783230,reinterpret_cast<void*>(&CoopMoveFileA),reinterpret_cast<void**>(&gNativeMoveFile)}};
    for(auto& item:entries) {
        DWORD protect=0; auto pointer=reinterpret_cast<void**>(item.address);
        if(!VirtualProtect(pointer,sizeof(void*),PAGE_READWRITE,&protect)) return false;
        *item.original=*pointer; *pointer=item.hook;
        DWORD unused=0; VirtualProtect(pointer,sizeof(void*),protect,&unused);
    }
    return true;
}
static BOOL(WINAPI* gCoopIatProtect)(LPVOID,SIZE_T,DWORD,PDWORD)=VirtualProtect;
static void* CoopIatCompareExchange(void* volatile* slot,void* desired,void* expected){
    return InterlockedCompareExchangePointer(slot,desired,expected);
}
static void* (*gCoopIatCompareExchange)(void* volatile*,void*,void*)=CoopIatCompareExchange;
static bool ReacquireCoopSaveCreateImports(const uintptr_t* privateSlots){
    const DWORD previousError=GetLastError();
    const auto kernel=GetModuleHandleW(L"kernel32.dll");if(!kernel)return false;
    struct Entry{uintptr_t slot;const char* name;void* hook;void* original;void* canonical;bool repair=false;};
    Entry entries[]{
        {privateSlots?privateSlots[0]:0x7831D0,"CreateFileA",reinterpret_cast<void*>(HookedCreateFileA),
            reinterpret_cast<void*>(g_pfnOriginalCreateFileA),reinterpret_cast<void*>(GetProcAddress(kernel,"CreateFileA"))},
        {privateSlots?privateSlots[1]:0x7831D4,"CreateFileW",reinterpret_cast<void*>(CoopCreateFileW),
            reinterpret_cast<void*>(gNativeCreateFileW),reinterpret_cast<void*>(GetProcAddress(kernel,"CreateFileW"))}};
    // Validate BOTH before changing either. A third-party owner, incomplete
    // installation, invalid call-through or guarded page is not ours to fix.
    for(auto& e:entries){
        MEMORY_BASIC_INFORMATION info{};void* current=nullptr;
        if(!e.original||e.original==e.hook||!e.canonical||e.canonical==e.hook||e.slot%sizeof(void*)||
            !VirtualQuery(reinterpret_cast<void*>(e.slot),&info,sizeof(info))||info.State!=MEM_COMMIT||
            (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
            !(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))||
            e.slot+sizeof(void*)>uintptr_t(info.BaseAddress)+info.RegionSize||
            !ReadGameMemory(e.slot,&current,sizeof(current)))return false;
        if(current==e.hook)continue;
        if(current!=e.canonical)return false;
        e.repair=true;
    }
    for(const auto& e:entries)if(e.repair){
        auto* slot=reinterpret_cast<void* volatile*>(e.slot);DWORD protection=0;
        if(!gCoopIatProtect(reinterpret_cast<void*>(e.slot),sizeof(void*),PAGE_READWRITE,&protection))return false;
        void* observed=gCoopIatCompareExchange(slot,e.hook,e.canonical);DWORD ignored=0;
        const bool restored=gCoopIatProtect(reinterpret_cast<void*>(e.slot),sizeof(void*),protection,&ignored)!=FALSE;
        if(!restored||(observed!=e.canonical&&observed!=e.hook))return false;
        if(observed==e.canonical){char line[208]{};
            std::snprintf(line,sizeof(line),"[SAVE_IAT] Reacquired loader-restored canonical %s at %08X; original call-through retained, no foreign hook replaced.\r\n",e.name,unsigned(e.slot));
            LogMessage(line);}
    }
    SetLastError(previousError);return true;
}
static bool CoopSpecialNativeSaveIatReady(const uintptr_t* privateSlots){
    // Legacy startup can leave a partially installed IAT on protection failure.
    // A logged "installed" boolean is insufficient for receipt safety: inspect
    // EVERY actual entry before observing/advertising the complete barrier.
    struct Required{uintptr_t slot;void* hook;void* original;};
    const Required required[]{
        {0x7831D0,reinterpret_cast<void*>(HookedCreateFileA),reinterpret_cast<void*>(g_pfnOriginalCreateFileA)},
        {0x7831D4,reinterpret_cast<void*>(CoopCreateFileW),reinterpret_cast<void*>(gNativeCreateFileW)},
        {0x783144,reinterpret_cast<void*>(CoopCloseHandle),reinterpret_cast<void*>(gNativeCloseHandle)},
        {0x783230,reinterpret_cast<void*>(CoopMoveFileA),reinterpret_cast<void*>(gNativeMoveFile)},
        {0x78322C,reinterpret_cast<void*>(HookedDeleteFileA),reinterpret_cast<void*>(g_pfnOriginalDeleteFileA)},
        {0x78312C,reinterpret_cast<void*>(HookedCreateDirectoryA),reinterpret_cast<void*>(g_pfnOriginalCreateDirectoryA)},
        {0x7832C4,reinterpret_cast<void*>(HookedRemoveDirectoryA),reinterpret_cast<void*>(g_pfnOriginalRemoveDirectoryA)}};
    for(size_t i=0;i<sizeof(required)/sizeof(*required);++i){const auto& item=required[i];void* current=nullptr;
        const auto slot=privateSlots?privateSlots[i]:item.slot;MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(slot),&info,sizeof(info))||info.State!=MEM_COMMIT||
            (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
            !(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))||
            slot+sizeof(current)>uintptr_t(info.BaseAddress)+info.RegionSize||!item.original||item.original==item.hook||
            !ReadGameMemory(slot,&current,sizeof(current))||current!=item.hook)return false;
    }
    return true;
}
static bool CommitCoopBytes(const char* name,const std::vector<uint8_t>& file,const wchar_t* suffix) {
    auto wideName=WideText(name);
    if(!suffix || !coop_save::IsSafeProfileName(wideName) || file.empty() || file.size()>coop_checkpoint::kMaxFile) return false;
    auto directory=gProfileContext.ActiveRoot()+L"\\"+wideName;
    if(!gProfileContext.IsCoopActive() || !SafeCoopDirectory(directory)) return false;
    auto target=directory+L"\\"+wideName+suffix, temporary=target+L".coop-transfer.tmp", previous=target+L".previous";
    for(const auto* path:{&target,&temporary,&previous}) {
        const DWORD attributes=GetFileAttributesW(path->c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))) return false;
    }
    HANDLE handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE) return false;
    DWORD written=0; bool okay=WriteFile(handle,file.data(),static_cast<DWORD>(file.size()),&written,nullptr)!=FALSE && written==file.size();
    okay=okay && FlushFileBuffers(handle)!=FALSE; bool closed=CloseHandle(handle)!=FALSE;
    if(!okay || !closed) { DeleteFileW(temporary.c_str()); return false; }
    // Keep the previous valid checkpoint recoverable until the new one is in place.
    if(GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES &&
        !CopyFileW(target.c_str(),previous.c_str(),FALSE)) {DeleteFileW(temporary.c_str());return false;}
    if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return false;
    if(!*suffix)CaptureCompletedCoopSave(target);
    return true;
}
static bool CommitCoopFile(const char* name,const std::vector<uint8_t>& file) {
    return coop_save::ValidateNativeProfileFile(file.data(),file.size())==coop_save::SaveValidation::Valid &&
        CommitCoopBytes(name,file,L"");
}
#include "CoopProfileSelect.inl"
static void ReportCoopProfileSelectionFailure(const char* point,CoopProfileSelect::Result result){
    const auto& detail=CoopProfileSelect::diagnostic;
    const auto& primary=CoopProfileSelect::primaryDiagnostic;
    char line[2200]{};
    std::snprintf(line,sizeof(line),"[SAVE_SELECT_DETAIL] point=%s result=%u stage=%s winerr=%lu profile='%s' domain=%s context=%u base='%s' active='%s' path='%s' primaryRoot='%s' primaryStage=%s primaryError=%lu primaryPath='%s' legacyAttempt=%u; no profile writes or fallback authorized.\r\n",
        point,unsigned(result),detail.stage,detail.error,gConfiguredCareerProfile,coop_build::kSaveDomain,unsigned(gProfileContext.CurrentDomain()),
        NarrowText(gCoopBaseRoot).c_str(),NarrowText(gProfileContext.ActiveRoot()).c_str(),NarrowText(detail.path).c_str(),
        NarrowText(CoopProfileSelect::primaryRoot).c_str(),primary.stage,primary.error,NarrowText(primary.path).c_str(),CoopProfileSelect::usedFallback?1:0);
    LogMessage(line);
}
static bool LatestCoopFile(const std::wstring& root,std::vector<uint8_t>& file,std::string& name) {
    WIN32_FIND_DATAW item{}; HANDLE scan=FindFirstFileW((root+L"\\*").c_str(),&item);
    if(scan==INVALID_HANDLE_VALUE) {
        char line[700]{};std::snprintf(line,sizeof(line),"[SAVE_SCAN] Cannot enumerate root='%s' error=%lu.\r\n",NarrowText(root).c_str(),GetLastError());LogMessage(line);return false;
    }
    FILETIME latest{}; bool found=false;
    do {
        std::wstring profile=item.cFileName;
        if(!(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) || !coop_save::IsSafeProfileName(profile)) continue;
        std::wstring path=root+L"\\"+profile+L"\\"+profile;
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes)) {
            char line[700]{};std::snprintf(line,sizeof(line),"[SAVE_SCAN] Candidate metadata unavailable path='%s' error=%lu.\r\n",NarrowText(path).c_str(),GetLastError());LogMessage(line);continue;
        }
        if((attributes.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
            (found && CompareFileTime(&attributes.ftLastWriteTime,&latest)<=0)) continue;
        std::vector<uint8_t> candidate;
        if(CoopProfileSelect::Read(root,NarrowText(profile),candidate)==CoopProfileSelect::Result::Loaded) {
            file=std::move(candidate); name=NarrowText(profile); latest=attributes.ftLastWriteTime; found=!name.empty();
        }
    } while(FindNextFileW(scan,&item));
    FindClose(scan); return found;
}
#include "CoopPersonalSave.inl"
static void CaptureCompletedCoopSave(const std::wstring& path) noexcept {
    if(!gCoopAutoBackup || !gProfileContext.IsCoopActive())return;
    try {
        const auto root=gProfileContext.ActiveRoot();
        const auto host=gProfileContext.CoopRoot(coop_save::Role::Host);
        const auto client=gProfileContext.CoopRoot(coop_save::Role::Client);
        const bool isHost=coop_save::EqualPathText(root,host);
        if((!isHost && !coop_save::EqualPathText(root,client)) || !coop_save::PathIsWithin(path,root))return;
        const auto separator=path.find_last_of(L'\\');
        if(separator==std::wstring::npos)return;
        const auto profile=path.substr(separator+1);
        // Ignore stock temp files, unrelated native data and all sidecar writes.
        if(!coop_save::IsSafeProfileName(profile) || !coop_save::EqualPathText(path,root+L"\\"+profile+L"\\"+profile))return;
        const auto session=CoopSnapshot();
        auto campaign=profile;
        if(!isHost) {
            std::lock_guard<std::mutex> lock(gCoopBackupIdentityMutex);
            if(coop_save::EqualPathText(profile,gCoopBackupPersonal) && coop_save::IsSafeProfileName(gCoopBackupCampaign))
                campaign=gCoopBackupCampaign;
            else if(session.campaignName[0] &&
                coop_save::EqualPathText(profile,WideText(CoopPersonalSave::ClientName(session.campaignName).c_str())))
                campaign=WideText(session.campaignName);
        }
        coop_backup::Snapshot snapshot;
        snapshot.record.campaign=campaign;snapshot.record.profile=profile;snapshot.record.utc=coop_backup::Now();
        snapshot.record.role=isHost?coop_backup::Role::Host:coop_backup::Role::Client;
        snapshot.record.action=gCoopBackupAction;
        if(session.joined && session.sessionId && session.ticket && coop_save::EqualPathText(campaign,WideText(session.campaignName))) {
            snapshot.record.session=session.sessionId;snapshot.record.ticket=session.ticket;
        }
        if(!coop_backup::Capture(root,snapshot) || !coop_backup::Write(gCoopBaseRoot,snapshot)) {
            LogMessage("[CAREER_BACKUP] Save succeeded; a complete automatic backup could not be published.\r\n");
        }
    } catch(...) {
        LogMessage("[CAREER_BACKUP] Save succeeded; automatic backup failed.\r\n");
    }
}
#include "CoopLobbyReentry.inl"
static bool EnterCoopProfileSelection(const coop::SessionSnapshot& state,bool incomingClient=false) {
    if(!CoopModsReady(state))return false;
    if((!gLocalSessionStarted && (!state.joined || !state.fresh)) || !ConfigureCoopProfileContext()) return false;
    // Native screen retries must not reset a profile already being created.
    if(gRestoreSpPending) return false;
    if(gProfileContext.IsCoopActive() && gCoopProfileSelectionReady) return true;
    if(gProfileSelectionReadHeld && GetTickCount()-gProfileSelectionRetryAt<1000)return false;
    gCoopProfileSelectionReady=false;
    gCoopResumePreviewAvailable=false;
    CoopPersonalSave::ResetContext();
    const auto role=state.role==coop::Role::Host?coop_save::Role::Host:coop_save::Role::Client;
    const bool enteringRoot=!gProfileContext.IsCoopActive();
    if(!gProfileContext.BeginCoopProfileSelection(role,true,ProfileAdapter())) return false;
    if(!SafeCoopDirectory(gProfileContext.ActiveRoot())) { gRestoreSpPending=true; return false; }
    const auto backupRole=state.role==coop::Role::Host?coop_backup::Role::Host:coop_backup::Role::Client;
    if(enteringRoot && !coop_backup::RecoverRestore(gCoopBaseRoot,backupRole)) {
        CoopAnnouncements::Post("The pending backup restore could not be recovered. Close the other game using this career and retry.");
        gRestoreSpPending=true;
        return false;
    }
#ifdef UNDERCOOP_MOD_MANAGER
    if(backupRole==coop_backup::Role::Host && !CoopModManager::selectedCareerBackupPath.empty()) {
        if(coop_save::EqualPathText(CoopModManager::selectedCareerBackupRoot,gCoopBaseRoot)) {
            coop_backup::Snapshot snapshot;
            if(!coop_backup::Read(CoopModManager::selectedCareerBackupPath,snapshot) ||
                snapshot.record.role!=backupRole || !coop_backup::Restore(gCoopBaseRoot,snapshot,coop_backup::Point(snapshot.record))) {
                CoopAnnouncements::Post("The selected backup could not be restored. Choose another backup in the launcher.");
                CoopModManager::selectedCareerBackupPath.clear();
                return false;
            }
            gRestoredHostProfile=NarrowText(snapshot.record.profile);
            gAutoResumeBackup=true;
            LogMessage("[COOP_BACKUP] Host backup restored. Previous profile retained beside the backup archive.\r\n");
        }
        CoopModManager::selectedCareerBackupPath.clear();
    }
#endif
    InterlockedExchange(&gCoopGameplayActive,1);
    gCoopSessionMode.store(state.role==coop::Role::Host?COOP_SESSION_HOST:COOP_SESSION_CLIENT);
    SetCoopRenderBranch(true);
    uint32_t mode=1; SafeWriteBytes(0x850078,&mode,4);
    uint8_t online=0; SafeWriteBytes(0x89E7D8,&online,1);
    if(state.role==coop::Role::Host || (state.careerLaunched&&!incomingClient)) {
        std::vector<uint8_t> file; std::string name;
        bool found=false,missingPreferred=false,invalidPreferred=false;
        // Only Host chooses a campaign. Client subsequently resolves its own
        // personal save using that authenticated campaign, not a local override.
        const auto preferred=state.role==coop::Role::Host&&!gRestoredHostProfile.empty() ?
            gRestoredHostProfile : PreferredCoopPersonal(state);
        bool explicitChoice=!preferred.empty();
        if(explicitChoice){
            const auto selected=CoopProfileSelect::Select(preferred.c_str(),file,name);
            if(selected==CoopProfileSelect::Result::Missing) {
                ReportCoopProfileSelectionFailure("isolated-menu-missing",selected);
                missingPreferred=true;explicitChoice=false;gProfileSelectionReadHeld=false;
                CoopAnnouncements::Post("The last career is missing. Choose Load Career or create a new Host career. Existing saves were not changed.");
            } else if(selected!=CoopProfileSelect::Result::Loaded){
                static DWORD lastNotice=0;const DWORD now=GetTickCount();
                if(!gProfileSelectionReadHeld||now-lastNotice>=3000){lastNotice=now;
                    ReportCoopProfileSelectionFailure("isolated-menu-read",selected);}
                if(CoopProfileSelect::RetryableMenuRead()) {
                    CoopAnnouncements::Post("Selected CO-OP save is busy. Waiting for its current write to finish; no saves were changed.");
                    // Retain one SP snapshot and isolated root during a real
                    // transient sharing/lock failure, without reset churn.
                    gProfileSelectionReadHeld=true;gProfileSelectionRetryAt=now;return false;
                }
                // An unavailable last-used profile must not prevent the user
                // reaching Load/New. Never deserialize it, silently select a
                // different career, import another directory or repair bytes.
                invalidPreferred=missingPreferred=true;explicitChoice=false;
                file.clear();name.clear();gProfileSelectionReadHeld=false;
                CoopAnnouncements::Post("The last CO-OP save is unavailable. Choose Load Career or create a new career. Existing saves were not changed.");
            } else {
                gProfileSelectionReadHeld=false;
            }
        }else if(CareerHistory(state.role).readable)found=LatestCoopFile(gProfileContext.ActiveRoot(),file,name);
        else missingPreferred=true;
        found=explicitChoice||found;
        // Preserve an existing Gemini COOP profile, but never clone a SP template.
        if(!found&&!missingPreferred && LatestCoopFile(gCoopBaseRoot,file,name)) found=CommitCoopFile(name.c_str(),file);
        if(found) {
            std::string selectedCampaign=name;
            if(state.role==coop::Role::Client&&!ResolveClientCampaign(name,selectedCampaign,state)) {
                CoopAnnouncements::Post("This personal save needs its Host campaign selected first. No save was changed.");return false;
            }
            std::vector<uint8_t> selectedPayload(file.begin()+0x24,file.end());
            // Host's selected OWN receipt/lease must precede the first native
            // menu/profile load, not merely the later outgoing shared merge.
            // Client normally resolves its own campaign in Initial(), before
            // its native reader. The local menu fallback never clones Host.
            if(!CoopSpecialPrepareMenuPreview(selectedCampaign.c_str(),name.c_str(),selectedPayload)) {
                // No native reader/reset has run. Keep this isolated context
                // and its captured SP snapshot while the durable admission
                // retries; restoring/re-entering every frame churns the same
                // managers and obscures the original barrier failure.
                static DWORD lastBarrierNotice=0;
                if(!lastBarrierNotice||GetTickCount()-lastBarrierNotice>=3000){lastBarrierNotice=GetTickCount();
                    LogMessage("[SAVE_SELECT] Host own receipt admission held BEFORE native profile reader; isolated menu context retained for retry.\r\n");}
                return false;
            }
            if(!NativeDeserializeProfile(selectedPayload.data())) {
                LogMessage("[SAVE_SELECT] Native selected-profile reader raised an exception AFTER receipt admission; SP restore queued.\r\n");
                gRestoreSpPending=true;return false;
            }
            CoopPersonalSave::BindNames(selectedCampaign,name);
            gCoopResumePreviewAvailable=true;
            char line[192]{};std::snprintf(line,sizeof(line),"[SAVE_SELECT] role=%s profile=%s source=%s native CRC verified; personal garage/cash preserved.\r\n",
                state.role==coop::Role::Host?"Host":"Client",name.c_str(),explicitChoice?"last-loaded/bootstrap":"latest-file");LogMessage(line);
            LogMessage("[SAVE] Existing isolated COOP profile loaded for native Resume/Load menu.\r\n");
        } else {
            if(!PrepareFreshCoopCareer()) {
                gRestoreSpPending=true;
                LogMessage("[SAVE] Fresh COOP initialization failed; career menu blocked and SP restore queued.\r\n");
                return false;
            }
            LogMessage(invalidPreferred?
                "[SAVE_SELECT] Invalid last-used save retained on disk; Load/New chooser opened with Resume disabled. Only isolated RAM prepared; SP snapshot retained.\r\n":
                "[SAVE] No COOP save: native career/events/garage reset in isolated RAM; SP snapshot retained.\r\n");
        }
    } else if(!PrepareFreshCoopCareer()) {
        gRestoreSpPending=true;
        LogMessage("[SAVE] Client blank COOP context failed; SP restore queued.\r\n");
        return false;
    } else {
        LogMessage("[SAVE] Client isolated initial context ready; own campaign save will be selected after the shared manifest.\r\n");
    }
    gCoopProfileSelectionReady=true;
    return true;
}
using NativeCoopScreenPushFn=void(__cdecl*)(const char*,const char*,int);
static NativeCoopScreenPushFn gPushNativeCoopScreen=reinterpret_cast<NativeCoopScreenPushFn>(0x537980);
static bool (*gNativeCoopScreenReady)(const char*)=IsNativeCoopCareerSourceReady;
// Game-thread only. Native screen requests borrow names across the FEng
// queue; pointers must survive this call, later UI ticks and session resets.
// Set nodes remain stable as other screen names are added. Never clear them
// while the native frontend may still hold a queued request.
static std::set<std::string> gNativeCoopScreenNames;
static bool PushNativeCoopScreen(const char* target) {
    if(!target||!target[0])return false;
    // PushScreen calls ShowFNG on its source and stock code dereferences the
    // returned queue node without a null check (5377C1). A tracked name alone
    // does not prove the package still exists after shop/menu teardown.
    const std::string source=gCurrentNativeScreen.empty()?"UI_Main.fng":gCurrentNativeScreen;
    const std::string destination=target;
    if(!gNativeCoopScreenReady(source.c_str())){
        static DWORD last=0;const DWORD now=GetTickCount();
        if(!last||coop_time::Elapsed(now,last)>=2000){last=now;char line[240]{};
            std::snprintf(line,sizeof(line),"[MENU_TRANSITION] Deferred source='%s' target='%s': source package no longer exists; native PushScreen not called.\r\n",source.c_str(),destination.c_str());LogMessage(line);}
        return false;
    }
    const auto session=CoopSnapshot();char line[256]{};
    std::snprintf(line,sizeof(line),"[MENU_TRANSITION] Push source='%s' target='%s' role=%u ticket=%u phase=%u.\r\n",source.c_str(),destination.c_str(),unsigned(session.role),session.ticket,unsigned(session.phase));LogMessage(line);
    const char* stableSource=gNativeCoopScreenNames.insert(source).first->c_str();
    const char* stableDestination=gNativeCoopScreenNames.insert(destination).first->c_str();
    gPushNativeCoopScreen(stableSource,stableDestination,0);return true;
}
void BrowseCoopServers() {
    if(!gProfileContext.IsCoopActive() && gPreLobbySingleplayer.payload.empty()) {
        if(ConfigureCoopProfileContext()) ProfileAdapter().captureSingleplayer(gPreLobbySingleplayer);
    }
    gCoopUiActive=true;
    AcquireSRWLockExclusive(&gSessionLock);
    if(gSession.Snapshot(GetTickCount()).role==coop::Role::Idle) {
        char name[32]{}; ReadGameMemory(0x83A9E0,name,31);
        if(!coop_checkpoint::SafeName(name)) std::strcpy(name,"Player");
        gSession.BeginBrowse(name,GetTickCount());
    }
    ReleaseSRWLockExclusive(&gSessionLock);
    const auto state=CoopSnapshot();
    gCoopSessionMode.store(state.role==coop::Role::Host?COOP_SESSION_HOST:state.role==coop::Role::Client?COOP_SESSION_CLIENT:COOP_SESSION_LOBBY);
    if(state.role==coop::Role::Host || state.role==coop::Role::Client) gOpenLobbyPending=true;
}
static bool BeginNativeCoopJoin() {
    const auto state=CoopSnapshot();
    if(state.role!=coop::Role::Browser || !state.haveDiscoveredHost) return false;
    char name[32]{}; ReadGameMemory(0x83A9E0,name,31);
    if(!coop_checkpoint::SafeName(name)) std::strcpy(name,"Player 2");
    // A process-local counter restarts at 1 after a Client crash/relaunch.
    // The same Host/IP/port must not mistake that new process for the previous
    // membership or accept a delayed old Leave/Heartbeat on its behalf.
    uint32_t membership=0;
    for(unsigned attempt=0;attempt<4&&!membership;++attempt)
        if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&membership),sizeof(membership),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)break;
    if(!membership){
        LogMessage("[SESSION] Join held: Windows could not create a fresh membership identity.\r\n");
        CoopAnnouncements::Post("Could not prepare a new connection identity. Please retry joining.");return false;
    }
    AcquireSRWLockExclusive(&gSessionLock);
    bool okay=gSession.Join(state.discoveredHost,state.discoveredSessionId,name,GetTickCount(),membership);
    ReleaseSRWLockExclusive(&gSessionLock);
    if(okay) { gIsSecondInstance=true; gCoopSessionMode.store(COOP_SESSION_CLIENT); }
    return okay;
}
static bool HostNativeCoopSession(const char* name) {
    if(!gCoopUiActive || !coop_checkpoint::SafeName(name)) return false;
    LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
    uint64_t id=static_cast<uint64_t>(counter.QuadPart)^(static_cast<uint64_t>(GetCurrentProcessId())<<32);
    if(!id) id=1;
    AcquireSRWLockExclusive(&gSessionLock);
    bool okay=gSession.Host(name,id,GetTickCount());
    ReleaseSRWLockExclusive(&gSessionLock);
    if(okay) { gIsSecondInstance=false; gCoopSessionMode.store(COOP_SESSION_HOST); gOpenLobbyPending=true; }
    return okay;
}
void StartCoopCareerFromLobby() {
    if(!gCoopCareerHooksReady) { LogMessage("[LOBBY] Start disabled: native career hook validation failed.\r\n"); return; }
    // Some ASIs install their detours after our startup verification. Recheck
    // immediately before creating a ticket, not only at the periodic UI tick.
    RefreshCoopVehicleAssetAdaptersReady();
    const auto state=CoopSnapshot();
    if(!CoopModsReady(state)) {CoopAnnouncements::Post(CoopModStatus());return;}
    if(!state.joined||!state.fresh||!state.localInMainMenu||state.resumeRequestPending||
        (state.phase!=coop::Phase::Lobby&&state.phase!=coop::Phase::Playing)||(state.role!=coop::Role::Host&&
        (state.role!=coop::Role::Client||!state.careerLaunched)))return;
    // Opening a menu is local UI, NOT a paired career selection/load ticket.
    // In particular the first Client remains in the lobby until Host loads.
    if(!gLocalResumePending){++gCoopCareerMenuGeneration;if(!gCoopCareerMenuGeneration)++gCoopCareerMenuGeneration;}
    gLocalResumePending=true;
    LogMessage("[LOBBY] Local career menu requested; peer screen and load ticket unchanged.\r\n");
}
void CloseNativeCoopSession() {
    AcquireSRWLockExclusive(&gSessionLock); gSession.Leave(GetTickCount()); ReleaseSRWLockExclusive(&gSessionLock);
    gCloseLobbyPending=true;
}
void RequestCoopSkipVote() {
    const auto state=CoopSnapshot();
    AcquireSRWLockExclusive(&gSessionLock);
    const bool accepted=gSession.VoteSkip(state.ticket,GetTickCount());
    ReleaseSRWLockExclusive(&gSessionLock);
    if(accepted) LogMessage("[INTRO_VOTE] Local skip vote sent; both players must vote.\r\n");
}
bool IsCoopSkipApproved() { return CoopSnapshot().skipDecided; }
static bool ContinuesReleasedInitialCareer(const coop::SessionSnapshot& state,uint32_t kind){
    return kind==1&&state.joined&&state.fresh&&state.careerLaunched&&state.ticket&&
        state.phase==coop::Phase::InitialLoading&&state.initialBarrierActive&&
        !state.initialBarrierComplete&&!state.localInMainMenu&&!gCareerLaunchPending&&
        gNativeCareerReleasedTicket==state.ticket;
}
bool DeferNativeCoopCareerLaunch(uint32_t kind,const char* source) {
    if(!IsCoopGameplayActive() || !gProfileContext.IsCoopActive()) return false;
    auto state=CoopSnapshot();
    if(ContinuesReleasedInitialCareer(state,kind)){
        // The stock intro continuation54AF97 selects this player's DDAY
        // event and then calls5404A0 asynchronously, after Replay's stack
        // scope has ended. Continue the already released load on BOTH roles.
        // A second CareerSelect here retires the Client's live intro/source
        // and leaves the Host waiting for a different ticket at Stage3.
        LogMessage("[CAREER_RETURN] Opening scene continued the released initial career; existing ticket/barrier retained.\r\n");
        return false;
    }
    if(kind==1&&state.careerLaunched&&state.phase==coop::Phase::Playing&&
        state.initialBarrierComplete&&!state.localInMainMenu){
        // 5404A0 is also the stock Return to Explore entry for every shop,
        // garage and ordinary in-career continuation. That local exit is not
        // a Resume/New/Load request and must not replace the paired ticket.
        LogMessage("[CAREER_RETURN] Existing career returning to Free Roam locally; shared session/ticket retained.\r\n");
        return false;
    }
    RefreshCoopVehicleAssetAdaptersReady();
    if(!CoopModsReady(state)) {CoopAnnouncements::Post(CoopModStatus());return true;}
    if(gCareerLaunchPending) return true;
    if(!state.joined||!state.fresh)return true;
    char name[32]{};ReadGameMemory(gCoopNativeProfileName,name,31);
    if(state.role==coop::Role::Client) {
        std::string campaign;
        if(kind!=1||!state.careerLaunched||!ResolveClientCampaign(name,campaign,state))return true;
        AcquireSRWLockExclusive(&gSessionLock);
        const bool requested=gSession.RequestResume(campaign.c_str(),GetTickCount());
        ReleaseSRWLockExclusive(&gSessionLock);
        if(requested)CoopAnnouncements::Post("Resume requested. Waiting for the Host to prepare the shared career.");
        return true; // Never launch a local Client world before Host approval.
    }
    if(state.role!=coop::Role::Host)return true;
    std::vector<uint8_t> payload(coop_save::kNativeProfilePayloadSize),file;
    if(coop_checkpoint::SafeName(name)) {
        if(kind==2) CoopPersonalSave::ResetContext();
        CoopPersonalSave::BindNames(name,name);
        if(kind==1) CoopPersonalSave::LoadCommonCache();
    }
    if(!coop_checkpoint::SafeName(name) || !NativeSerializeProfile(payload.data()) ||
        !CoopSpecialPrepareInitial(name,name,kind,true,payload,nullptr)||!CoopPersonalSave::BuildSharedBlob(payload,file)) {
        LogMessage("[SAVE] Career launch held: profile snapshot failed.\r\n"); return true;
    }
    coop_backup::RestorePoint restorePoint;
    const bool restoring=kind==1&&coop_backup::PendingRestorePoint(gCoopBaseRoot,coop_backup::Role::Host,WideText(name),restorePoint);
    if(restoring) {
        const auto* header=reinterpret_cast<const uint8_t*>(&restorePoint);
        file.insert(file.begin(),header,header+sizeof(restorePoint));
    }
    AcquireSRWLockExclusive(&gSessionLock);
    const bool alreadyReserved=state.ticket&&state.ticket==gAcceptedNewCareerSelectionTicket;
    const uint8_t retain=!restoring&&kind==1&&state.careerLaunched&&!state.peerInMainMenu&&
        CoopPersonalSave::SameName(state.campaignName,name)?2:0;
    if(!alreadyReserved&&!gSession.RequestCareer(GetTickCount(),true,name,retain)){
        ReleaseSRWLockExclusive(&gSessionLock);return true;
    }
    state=gSession.Snapshot(GetTickCount());gCheckpointSender.Reset();gCheckpointReceiver.Reset();
    gPreparedHostCareerTicket=state.ticket;
    gCareerLaunchPending=true; gCareerLaunchKind=kind; gCareerLaunchSource=source?source:gCurrentNativeScreen;
    if(++gCheckpointSerial==0)++gCheckpointSerial;
    const bool queued=gCheckpointSender.Start(state.sessionId,gCheckpointSerial,state.ticket,name,file,GetTickCount(),
        restoring?coop_checkpoint::kRestoreCareer:kind);
    if(!queued)gCareerLaunchPending=false;
    ReleaseSRWLockExclusive(&gSessionLock);
    if(!queued){LogMessage("[SAVE] Career manifest reservation failed; native launch held for retry.\r\n");return true;}
    if(restoring) {
        gPendingRestoreClearSession=state.sessionId;
        gPendingRestoreClearTicket=state.ticket;
        gPendingRestoreClearProfile=name;
    }
    LogMessage("[SAVE] Progress-only career manifest queued; personal garage/cash/options are not transmitted.\r\n"); return true;
}
bool InitialCoopLoadingBarrier() {
    if(!IsCoopGameplayActive()) return false;
    AcquireSRWLockExclusive(&gSessionLock);
    auto state=gSession.Snapshot(GetTickCount());
    if(state.initialBarrierActive && state.joined && state.fresh) gSession.MarkInitialLoaded(state.ticket,GetTickCount());
    state=gSession.Snapshot(GetTickCount());
    ReleaseSRWLockExclusive(&gSessionLock);
    return state.initialBarrierActive && !state.initialBarrierComplete && state.joined && state.fresh;
}
void OnCoopNativeScreen(const char* name) {
    if(!name) return;
    gCurrentNativeScreen=name;
    gNativeCoopLobby=nullptr;
    if(std::strcmp(name,"UI_Main.fng")==0) {
        const auto state=CoopSnapshot();
        gAutoResumeBackup=false;
        gRestoredHostProfile.clear();
        const bool coordinated=gCoopReturningForTicket&&gCoopReturningForTicket==state.ticket;
        gCoopReturningForTicket=0;
        gCoopUiActive=false; gRestoreSpPending=gProfileContext.IsCoopActive();
        gLocalSessionStarted=false;gLocalResumePending=false;gNativeCareerReleasedTicket=0;
        gCoopProfileSelectionReady=false;gProfileSelectionReadHeld=false;
        if(!coordinated){gCareerLaunchPending=false;gPreparedHostCareerTicket=0;gAcceptedNewCareerSelectionTicket=0;}
        InterlockedExchange(&gCoopGameplayActive,0);
        SetCoopRenderBranch(false);
        gCoopSessionMode.store(COOP_SESSION_INACTIVE);
        AcquireSRWLockExclusive(&gSessionLock);
        if(!coordinated){gCoopMenuAnnouncementPending=!gSession.AcrossMainMenu(GetTickCount());gCheckpointSender.Reset();gCheckpointReceiver.Reset();}
        ReleaseSRWLockExclusive(&gSessionLock);
        gCoopRaceState=COOP_RACE_IDLE; gCar1LoadedForRace=false;
    }
}
bool IsJoinedCoopSender(const sockaddr_in& sender) {
    auto state=CoopSnapshot();
    return state.joined && state.fresh && state.peer.address==sender.sin_addr.s_addr && state.peer.port==ntohs(sender.sin_port);
}
static void NoteAcceptedCoopMotion(const coop::SessionSnapshot& accepted) {
    AcquireSRWLockExclusive(&gSessionLock);
    const auto now = GetTickCount();
    const auto live = gSession.Snapshot(now);
    if (live.membershipNonce == accepted.membershipNonce && live.ticket == accepted.ticket)
        gSession.ObserveGameplay(accepted.sessionId, accepted.peer, now);
    ReleaseSRWLockExclusive(&gSessionLock);
}
void TickCoopSessionNetwork(DWORD now) {
    std::array<coop::Outgoing,32> output{}; size_t count=0;
    coop::SessionMessage beacon{}; coop_checkpoint::Packet chunk{}; bool sendBeacon=false,sendChunk=false;
    coop::Endpoint peer{};
    const auto admission=CoopSnapshot();
    const bool admitted=admission.initialBarrierComplete||CoopModsReady(admission);
    AcquireSRWLockExclusive(&gSessionLock);
    // Serialize the timestamp with game-thread Join/Ready/transfer creation.
    // A pre-lock network sample must not expire freshly published peer state.
    now=GetTickCount();
    gSession.Tick(now);
    while(count<output.size() && gSession.PopOutgoing(output[count])) ++count;
    static DWORD lastBeacon=0;
    if(now-lastBeacon>=500) { sendBeacon=gSession.BuildBeacon(beacon,now); lastBeacon=now; }
    auto state=gSession.Snapshot(now); peer=state.peer;
    if(state.joined && state.fresh && admitted && state.sessionId==admission.sessionId && state.peer==admission.peer)
        sendChunk=gCheckpointSender.Tick(now,chunk);
    ReleaseSRWLockExclusive(&gSessionLock);
    for(size_t i=0;i<count;++i) SendSessionBytes(&output[i].message,sizeof(coop::SessionMessage),output[i].endpoint);
    if(sendChunk) SendSessionBytes(&chunk,sizeof(chunk),peer);
    if(sendBeacon) {
        SendSessionBytes(&beacon,sizeof(beacon),{inet_addr(gPeerAddress),gTargetPort});
        SendSessionBytes(&beacon,sizeof(beacon),{inet_addr("127.0.0.1"),gTargetPort});
        SendSessionBytes(&beacon,sizeof(beacon),{INADDR_BROADCAST,gTargetPort});
    }
}
bool ReceiveCoopSessionPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now) {
    uint32_t magic=0; if(bytes<4) return false; std::memcpy(&magic,data,4);
    if(magic==coop::kSessionMagic) {
        // A partially checked peer cannot select/read/write a career context.
        // Reliable control messages retry after the mod handshake completes.
        if(bytes==sizeof(coop::SessionMessage)) {
            coop::SessionMessage header{};std::memcpy(&header,data,sizeof(header));
            if(header.version!=coop::kSessionVersion) {
                static DWORD lastVersionNotice=0;
                if(now-lastVersionNotice>=3000){lastVersionNotice=now;CoopAnnouncements::Post("Different Undercoop network version. Install " UNDERCOOP_BUILD_VERSION " on both computers.");}
                return true;
            }
            if((header.type==coop::MessageType::CareerSelect||header.type==coop::MessageType::CareerReady||
                header.type==coop::MessageType::CareerStart)&&!CoopModsReady(CoopSnapshot()))return true;
        }
        AcquireSRWLockExclusive(&gSessionLock);
        const auto before=gSession.Snapshot(now);
        gSession.Receive(data,static_cast<size_t>(bytes),{sender.sin_addr.s_addr,ntohs(sender.sin_port)},now);
        const auto after=gSession.Snapshot(now);
        // Only a validated, newly accepted Host selection changes Client's
        // nonzero ticket. Retire abandoned old transfers before this lock is
        // released and any packet for the new ticket can be accepted. Doing
        // this later in the UI action could erase an already received first
        // block; doing nothing leaves a partial NAME receiver rejecting ADANA.
        if(after.role==coop::Role::Client && after.joined && after.ticket && after.ticket!=before.ticket) {
            gCheckpointSender.Reset();gCheckpointReceiver.Reset();
        }
        ReleaseSRWLockExclusive(&gSessionLock);
        if((!before.joined&&after.joined)||(before.joined&&!after.joined))ResetCoopModPeer();
        return true;
    }
    if(magic!=coop_checkpoint::kMagic) return false;
    const auto checkpointAdmission=CoopSnapshot();
    if(bytes!=sizeof(coop_checkpoint::Packet) || !IsJoinedCoopSender(sender) ||
        (!checkpointAdmission.initialBarrierComplete&&!CoopModsReady(checkpointAdmission))) return true;
    coop_checkpoint::Packet packet{},ack{}; std::memcpy(&packet,data,sizeof(packet));
    bool sendAck=false;
    AcquireSRWLockExclusive(&gSessionLock);
    auto state=gSession.Snapshot(now);
    if(packet.session==state.sessionId && packet.ticket==state.ticket) {
        if(packet.type==coop_checkpoint::Type::Ack) {
            if(state.role==coop::Role::Host && gCheckpointSender.RejectRestore(packet))gRejectedRestoreTicket.store(state.ticket);
            else gCheckpointSender.Ack(packet,now);
        }
        else if(state.role==coop::Role::Client || (state.role==coop::Role::Host && state.initialBarrierComplete && packet.launchKind==3))
            sendAck=gCheckpointReceiver.Receive(packet,ack);
    }
    ReleaseSRWLockExclusive(&gSessionLock);
    if(sendAck) SendSessionBytes(&ack,sizeof(ack),{sender.sin_addr.s_addr,ntohs(sender.sin_port)});
    return true;
}

// Forward native integrations are defined immediately after this include.
void UpdateNativeCoopLobby(void*,const coop::SessionSnapshot&);
void TickNativeCoopLobby(const coop::SessionSnapshot&,DWORD);
bool ReplayNativeCoopCareer(uint32_t,const char*);
bool IsNativeCoopCareerSourceReady(const char*);
bool PrepareCoopRetainedWorldReentry();
static bool RetainedCoopWorldReady(){return !gCoopFleetEnabled||PrepareCoopRetainedWorldReentry();}
static bool PrepareCoopCareerSelectionAction(const coop::Action& action,const coop::SessionSnapshot& state) {
    const bool acceptedNew=state.role==coop::Role::Host && action.ticket==gAcceptedNewCareerSelectionTicket;
    gLocalSessionStarted=state.localRetainsNative;gLocalResumePending=false;
    if(!acceptedNew) {gAcceptedNewCareerSelectionTicket=0;if(action.ticket!=gPreparedHostCareerTicket)gCareerLaunchPending=false;}
    gNativeCareerReleasedTicket=0;
    return acceptedNew;
}
static bool PrepareRequestedHostCareer(DWORD flow) {
    auto state=CoopSnapshot();
    if(state.role!=coop::Role::Host||!state.joined||!state.fresh||!CoopModsReady(state)||gCoopRequestedHostCampaign.empty())return false;
    if(gCareerLaunchPending&&gPreparedHostCareerTicket==state.ticket){gCoopRequestedHostCampaign.clear();return true;}
    const auto campaign=gCoopRequestedHostCampaign;
    if(CanRetainCoopNative(campaign.c_str(),flow)) {
        std::vector<uint8_t> payload(coop_save::kNativeProfilePayloadSize),blob;
        if(!NativeSerializeProfile(payload.data())||!CoopPersonalSave::BuildSharedBlob(payload,blob))return false;
        AcquireSRWLockExclusive(&gSessionLock);
        if(!gSession.RequestCareer(GetTickCount(),true,campaign.c_str(),1)){ReleaseSRWLockExclusive(&gSessionLock);return false;}
        state=gSession.Snapshot(GetTickCount());gCheckpointSender.Reset();gCheckpointReceiver.Reset();
        gPreparedHostCareerTicket=state.ticket;gCareerLaunchPending=true;gCareerLaunchKind=1;gCareerLaunchSource.clear();
        if(++gCheckpointSerial==0)++gCheckpointSerial;
        const bool sent=gCheckpointSender.Start(state.sessionId,gCheckpointSerial,state.ticket,campaign.c_str(),blob,GetTickCount(),1);
        ReleaseSRWLockExclusive(&gSessionLock);
        if(!sent){gCareerLaunchPending=false;LogMessage("[SESSION_REENTRY] Manifest reservation failed; native world preserved.\r\n");return false;}
        gCoopRequestedHostCampaign.clear();
        LogMessage("[SESSION_REENTRY] Fresh rejoin ticket published; Host keeps its existing native world/profile.\r\n");return true;
    }
    std::vector<uint8_t> file;
    const auto found=CoopProfileSelect::ProbeHost(campaign.c_str(),file);
    if(found!=CoopProfileSelect::Result::Loaded||!CoopSpecialMayImport()) {
        AcquireSRWLockExclusive(&gSessionLock);gSession.RejectResume(GetTickCount(),found==CoopProfileSelect::Result::Loaded?2:1);ReleaseSRWLockExclusive(&gSessionLock);
        gCoopRequestedHostCampaign.clear();CoopAnnouncements::Post("Career change is unavailable: the Host save is missing, busy, or a special event is pending. No saves were replaced.");return true;
    }
    if(flow==6) {
        if(gCoopReturningForTicket==UINT32_MAX)return false;
        if(ReturnCoopNativeToMenu(0)){gCoopReturningForTicket=UINT32_MAX;return false;}
        AcquireSRWLockExclusive(&gSessionLock);gSession.RejectResume(GetTickCount(),2);ReleaseSRWLockExclusive(&gSessionLock);
        gCoopRequestedHostCampaign.clear();return false;
    }
    if(flow!=3||gRestoreSpPending)return false;
    if(gCoopExpectedMenuProfile!=campaign){gCoopProfileSelectionReady=false;gCoopExpectedMenuProfile=campaign;}
    if(!EnterCoopProfileSelection(state))return false;
    if(!PushNativeCoopScreen("UI_StartCareer.fng"))return false;
    gCoopUiActive=false;
    DeferNativeCoopCareerLaunch(1,"UI_StartCareer.fng");
    if(gCareerLaunchPending){gCoopRequestedHostCampaign.clear();gCoopExpectedMenuProfile.clear();return true;}
    return false;
}
void TickCoopSessionUi(DWORD flow) {
    TickCoopModUi(flow);
    if(flow==3 && gCloseLobbyPending) {
        // No native LAN backend was opened for this UDP session. Destroying
        // that backend here can retire the source queue before PushScreen.
        if(PushNativeCoopScreen("UI_Main.fng")){
            gCloseLobbyPending=false;gCoopUiActive=false;gCoopSessionMode.store(COOP_SESSION_INACTIVE);
        }
    }
    if(flow==3 && gRestoreSpPending) {
        gRestoreSpPending=false;
        gCoopProfileSelectionReady=false;
        gProfileSelectionReadHeld=false;
        uint32_t frontendCars=1; SafeWriteBytes(0x89E814,&frontendCars,4);
        if(!gProfileContext.RestoreSingleplayer(ProfileAdapter())) {
            LogMessage("[SAVE] SP restore failed; profile I/O remains fail-closed.\r\n");
        } else { gPreLobbySingleplayer={}; LogMessage("[SAVE] Original singleplayer profile/root restored; session remains open.\r\n"); }
    }
    if(flow==3 && !gCoopUiActive && !gProfileContext.IsCoopActive() && !gPreLobbySingleplayer.payload.empty()) {
        if(ProfileAdapter().restoreSingleplayer(gPreLobbySingleplayer)) gPreLobbySingleplayer={};
    }
    if(gCoopMenuAnnouncementPending) {
        AcquireSRWLockExclusive(&gSessionLock);const auto s=gSession.Snapshot(GetTickCount());
        if(!s.joined||gSession.AcrossMainMenu(GetTickCount()))gCoopMenuAnnouncementPending=false;
        ReleaseSRWLockExclusive(&gSessionLock);
    }
    if(flow==3 && gLocalResumePending && CoopModsReady(CoopSnapshot())&&!gRestoreSpPending) {
        if(EnterCoopProfileSelection(CoopSnapshot())&&PushNativeCoopScreen("UI_StartCareer.fng")) { gLocalResumePending=false;gCoopUiActive=false; }
    }
    if(flow==3 && gAutoResumeBackup && gCoopResumePreviewAvailable &&
        IsNativeCoopCareerSourceReady("UI_StartCareer.fng") &&
        CoopPersonalSave::SameName(gRestoredHostProfile,CoopPersonalSave::personal)) {
        DeferNativeCoopCareerLaunch(1,"UI_StartCareer.fng");
        if(gCareerLaunchPending)gAutoResumeBackup=false;
    }
    if(const auto rejected=gRejectedRestoreTicket.exchange(0);rejected && rejected==gPreparedHostCareerTicket) {
        const auto profile=WideText(gPendingRestoreClearProfile.c_str());
        coop_backup::RestorePoint point;
        const bool reverted=coop_backup::PendingRestorePoint(gCoopBaseRoot,coop_backup::Role::Host,profile,point) &&
            coop_backup::RevertRestore(gCoopBaseRoot,coop_backup::Role::Host,profile,point);
        gCareerLaunchPending=false;
        gPreparedHostCareerTicket=0;
        gRestoredHostProfile.clear();
        gPendingRestoreClearSession=0;gPendingRestoreClearTicket=0;gPendingRestoreClearProfile.clear();
        gCoopProfileSelectionReady=false;gCoopResumePreviewAvailable=false;gRestoreSpPending=true;
        PushNativeCoopScreen("UI_Main.fng");
        CoopAnnouncements::Post(reverted?"Player 2 could not restore the selected backup. Your previous career was restored.":
            "Player 2 could not restore the selected backup. Reopen the career menu to recover the previous profile.");
    }
    if(gCoopAutoRejoinPending&&flow==6&&CoopModsReady(CoopSnapshot())) {
        auto s=CoopSnapshot();
        if(s.role==coop::Role::Host&&CanRetainCoopNative(CoopPersonalSave::campaign.c_str(),flow)) {
            AcquireSRWLockExclusive(&gSessionLock);const bool adopted=gSession.AdoptPlayingCareer(CoopPersonalSave::campaign.c_str(),GetTickCount());ReleaseSRWLockExclusive(&gSessionLock);
            if(adopted){gCoopRequestedHostCampaign=CoopPersonalSave::campaign;gCoopAutoRejoinPending=false;}
        }
    }
    if(!gCoopRequestedHostCampaign.empty())PrepareRequestedHostCareer(flow);
    {
        const auto retained=CoopSnapshot();
        if(flow==6&&retained.localRetainsNative&&!retained.initialBarrierComplete&&CoopModsReady(retained)&&RetainedCoopWorldReady()&&
            retained.phase==coop::Phase::InitialLoading&&gNativeCareerReleasedTicket==retained.ticket) {
            // Retained worlds have no Stage3 callback to retry a full reliable
            // queue. Keep their ready announcement retryable on later ticks.
            AcquireSRWLockExclusive(&gSessionLock);gSession.BeginInitialLoad(retained.ticket,GetTickCount());
            gSession.MarkInitialLoaded(retained.ticket,GetTickCount());ReleaseSRWLockExclusive(&gSessionLock);
        }
    }
    static coop::Action deferredAction{};
    static bool haveDeferredAction=false;
    static uint64_t deferredSession=0;
    coop::Action action{};
    while(true) {
        bool have=haveDeferredAction;
        if(have) { action=deferredAction; haveDeferredAction=false;if(CoopSnapshot().sessionId!=deferredSession)continue; }
        else { AcquireSRWLockExclusive(&gSessionLock); have=gSession.PopAction(action); ReleaseSRWLockExclusive(&gSessionLock); }
        if(!have) break;
        auto state=CoopSnapshot();
        if(action.ticket&&action.ticket!=state.ticket)continue;
        if((action.type==coop::ActionType::CareerSelect || action.type==coop::ActionType::StartCareer) &&
            (!state.joined || !state.fresh || action.ticket!=state.ticket)) continue;
        // A deferred native action may outlive a cancelled selection even when
        // its ticket remains the same. Never reopen that abandoned menu/load.
        if(action.type==coop::ActionType::CareerSelect&&state.phase!=coop::Phase::CareerSelection&&state.phase!=coop::Phase::ProfileSync)continue;
        if(action.type==coop::ActionType::StartCareer&&(state.phase!=coop::Phase::InitialLoading||gNativeCareerReleasedTicket==action.ticket))continue;
        if(action.type==coop::ActionType::CareerSelect&&state.localRetainsNative&&
            !CanRetainCoopNative(state.campaignName,flow)) {
            AcquireSRWLockExclusive(&gSessionLock);gSession.DeclineNativeRetention(state.ticket);ReleaseSRWLockExclusive(&gSessionLock);
            state=CoopSnapshot();
        }
        if((action.type==coop::ActionType::CareerSelect || action.type==coop::ActionType::StartCareer) &&
            ((!state.localRetainsNative&&flow!=3) || (state.localRetainsNative&&flow!=6) || !CoopModsReady(state))) {
            if(action.type==coop::ActionType::CareerSelect&&!state.localRetainsNative&&flow==6&&gCoopReturningForTicket!=state.ticket)
                ReturnCoopNativeToMenu(state.ticket);
            deferredAction=action;deferredSession=state.sessionId; haveDeferredAction=true; break;
        }
        if(action.type==coop::ActionType::PeerJoined) {
            ResetCoopPeerSelectionGeneration();
            LogMessage("[SESSION] Explicit Join/JoinAck confirmed; two lobby members.\r\n");
            if(gCoopUiActive) gOpenLobbyPending=true;
            if(state.role==coop::Role::Host&&flow==6)gCoopAutoRejoinPending=true;
        } else if(action.type==coop::ActionType::ResumeRequested) {
            if(state.role==coop::Role::Host&&action.ticket==state.ticket&&state.resumeRequestPending)
                gCoopRequestedHostCampaign=state.requestedCampaignName;
        } else if(action.type==coop::ActionType::ResumeRejected) {
            CoopAnnouncements::Post("The Host could not open that career. Choose an available save or retry after the current event.");
            LogMessage("[SESSION_REENTRY] Resume rejected for this ticket; menu input remains available.\r\n");
        } else if(action.type==coop::ActionType::PeerMainMenu) {
            LogMessage("[SESSION_REENTRY] Teammate returned to the frontend; membership kept, old readiness retired.\r\n");
        } else if(action.type==coop::ActionType::CareerSelect) {
            ResetCoopPeerSelectionGeneration();
            const bool acceptedNew=PrepareCoopCareerSelectionAction(action,state);
            const bool preparedHost=state.role==coop::Role::Host&&action.ticket==gPreparedHostCareerTicket;
            const bool profileReady=(state.localRetainsNative||acceptedNew||preparedHost)?
                (gProfileContext.IsCoopActive()&&!gRestoreSpPending):EnterCoopProfileSelection(state,state.role==coop::Role::Client);
            if(profileReady) {
                if(state.localRetainsNative){
                    LogMessage("[SESSION_REENTRY] Existing native career retained; waiting only for the returning player's profile.\r\n");continue;
                }
                if(acceptedNew)gCoopProfileSelectionReady=true;
                // UDP COOP never created a native LAN backend. Its teardown
                // deletes UI managers and can undo the newly selected profile.
                // The normal screen transition owns lobby destruction instead.
                uint32_t mode=1; SafeWriteBytes(0x850078,&mode,4);
                // Accepted New's native dialog already owns this package and
                // has queued its fresh manifest. Reopening it here can reload
                // the previously resumed NAME and cancel the pending launch.
                if(!acceptedNew&&!preparedHost&&!PushNativeCoopScreen("UI_StartCareer.fng")){
                    deferredAction=action;deferredSession=state.sessionId;haveDeferredAction=true;break;
                }
                gCoopUiActive=false;
                if(acceptedNew)gAcceptedNewCareerSelectionTicket=0;
                LogMessage("[LOBBY] Native career choices requested; UDP session preserved.\r\n");
            } else {
                deferredAction=action;deferredSession=state.sessionId; haveDeferredAction=true;
                static DWORD lastFailure=0;
                if(GetTickCount()-lastFailure>=3000) { LogMessage("[SAVE] Career selection held safely; native save context not ready.\r\n"); lastFailure=GetTickCount(); }
                break;
            }
        } else if(action.type==coop::ActionType::StartCareer) {
            ResetCoopPeerSelectionGeneration();
            uint32_t kind=state.role==coop::Role::Host?gCareerLaunchKind:gReceivedLaunchKind.load();
            if(state.localRetainsNative&&!RetainedCoopWorldReady()) {
                deferredAction=action;deferredSession=state.sessionId;haveDeferredAction=true;break;
            }
            if(!state.localRetainsNative&&(kind==2 || (state.role==coop::Role::Client && CoopPersonalSave::personalStarter)) &&
                !IsNativeCoopCareerSourceReady("UI_StartCareer.fng")) {
                static DWORD lastSourceWait=0;const DWORD now=GetTickCount();
                if(!lastSourceWait||coop_time::Elapsed(now,lastSourceWait)>=5000){
                    lastSourceWait=now;char line[272];std::snprintf(line,sizeof(line),
                        "[CAREER_NATIVE_WAIT] session=%llu ticket=%u role=%u kind=%u flow=%u source='UI_StartCareer.fng' ready=0 current='%s'; native entry remains deferred.\r\n",
                        static_cast<unsigned long long>(state.sessionId),state.ticket,unsigned(state.role),kind,flow,gCurrentNativeScreen.c_str());LogMessage(line);
                }
                deferredAction=action;deferredSession=state.sessionId; haveDeferredAction=true; break;
            }
            gCareerLaunchKind=kind; // Keep the client's native story/camera context too.
            if(state.localRetainsNative) {
                gNativeCareerReleasedTicket=action.ticket;
                AcquireSRWLockExclusive(&gSessionLock);gSession.BeginInitialLoad(action.ticket,GetTickCount());ReleaseSRWLockExclusive(&gSessionLock);
                AcquireSRWLockExclusive(&gSessionLock);gSession.MarkInitialLoaded(action.ticket,GetTickCount());ReleaseSRWLockExclusive(&gSessionLock);
                gCareerLaunchPending=false;
                LogMessage("[SESSION_REENTRY] Retained native world marked ready for the fresh ticket; no replay or deserialization.\r\n");continue;
            }
            // A Host resuming its existing career has no introductory movie.
            // Its approval lets a first-time Client skip its own personal
            // movie if it chooses; never force that Client to watch or skip.
            if(state.role==coop::Role::Host && kind==1) {
                AcquireSRWLockExclusive(&gSessionLock);gSession.VoteSkip(action.ticket,GetTickCount());ReleaseSRWLockExclusive(&gSessionLock);
            }
            if(!ReplayNativeCoopCareer(kind,kind==2?"UI_StartCareer.fng":gCareerLaunchSource.c_str())) {
                deferredAction=action;deferredSession=state.sessionId;haveDeferredAction=true;break;
            }
            gNativeCareerReleasedTicket=action.ticket;
            AcquireSRWLockExclusive(&gSessionLock);gSession.BeginInitialLoad(action.ticket,GetTickCount());ReleaseSRWLockExclusive(&gSessionLock);
            gCareerLaunchPending=false;
            gPreparedHostCareerTicket=0;
            LogMessage("[SESSION] Both profiles ready; native entry requested once for this ticket; movie/load completion pending.\r\n");
        } else if(action.type==coop::ActionType::PeerLeft || action.type==coop::ActionType::Closed) {
            ResetCoopPeerSelectionGeneration();
            AcquireSRWLockExclusive(&gSessionLock); gCheckpointSender.Reset(); gCheckpointReceiver.Reset(); ReleaseSRWLockExclusive(&gSessionLock);
            gCareerLaunchPending=false;
            gPreparedHostCareerTicket=0;gCoopRequestedHostCampaign.clear();gCoopExpectedMenuProfile.clear();gLocalResumePending=false;
            // Keep the pending native stage callback: its initial barrier now releases without a peer.
            gCoopAutoRejoinPending=false;
            if(action.type==coop::ActionType::Closed)gLocalSessionStarted=false;
            LogMessage("[SESSION] Peer left/timed out; initial waiting cancelled.\r\n");
        } else if(action.type==coop::ActionType::InitialLoadReleased) {
            if(state.initialBarrierComplete)gLocalSessionStarted=true;
        }
    }
    auto state=CoopSnapshot();
    if(state.joined && state.peer.Valid()) gTargetEndpoint=SessionAddress(state.peer); // only main thread commits gameplay target
    if(gCoopUiActive && gOpenLobbyPending && flow==3) {
        if(gCurrentNativeScreen=="PC_OL_Lobby.fng"||PushNativeCoopScreen("PC_OL_Lobby.fng"))gOpenLobbyPending=false;
    }
    if(gCoopUiActive && flow==3) TickNativeCoopLobby(CoopUiSnapshot(),GetTickCount());
    coop_checkpoint::Completed complete;
    // Native race guards snapshot the session too. Never run them while
    // owning the non-recursive SRW lock; only receiver access belongs inside.
    const bool canTake=state.initialBarrierComplete||state.localRetainsNative?CoopSharedCareerCanApply(flow):(flow==3&&CoopModsReady(state)&&gProfileContext.IsCoopActive()&&gCoopProfileSelectionReady);
    AcquireSRWLockExclusive(&gSessionLock);
    bool haveFile=canTake && gCheckpointReceiver.Take(complete);
    ReleaseSRWLockExclusive(&gSessionLock);
    if(haveFile) {
        const auto applyState=CoopSnapshot();
        bool okay=(applyState.initialBarrierComplete||CoopModsReady(applyState))&&
            applyState.sessionId==state.sessionId&&applyState.peer==state.peer&&
            state.joined && state.fresh && complete.session==state.sessionId &&
            complete.ticket==state.ticket && CoopPersonalSave::Receive(complete,flow,state);
        const bool restoring=complete.launchKind==coop_checkpoint::kRestoreCareer;
        if(okay && (complete.launchKind<=2 || restoring))
            gReceivedLaunchKind.store(complete.launchKind==2?2:1);
        if(okay && restoring) {
            gPendingRestoreClearSession=state.sessionId;
            gPendingRestoreClearTicket=state.ticket;
            gPendingRestoreClearProfile=CoopPersonalSave::personal;
        }
        coop_checkpoint::Packet ack{};
        AcquireSRWLockExclusive(&gSessionLock);
        bool send=!okay&&restoring?gCheckpointReceiver.RejectRestore(ack):gCheckpointReceiver.Commit(okay,ack);
        if(okay && !state.initialBarrierComplete) gSession.ReadyForCareer(state.ticket,GetTickCount());
        ReleaseSRWLockExclusive(&gSessionLock);
        if(send) SendSessionBytes(&ack,sizeof(ack),state.peer);
        LogMessage(okay?"[SAVE] Shared career facts committed; personal car, garage, cash and choices retained.\r\n":
            restoring?"[COOP_BACKUP] Restore manifest rejected; native career launch held.\r\n":"[SAVE] Progress manifest rejected; personal saves untouched.\r\n");
    }
    if(state.role==coop::Role::Host && state.joined && (state.initialBarrierComplete||CoopModsReady(state))) {
        AcquireSRWLockExclusive(&gSessionLock);
        if(gCareerLaunchPending && gCheckpointSender.Done()) gSession.ReadyForCareer(state.ticket,GetTickCount());
        ReleaseSRWLockExclusive(&gSessionLock);
    }
    CoopPersonalSave::Tick(flow,state);
    RememberLoadedCoopCareer(flow,CoopSnapshot());
    if(flow==6 && state.initialBarrierComplete && gLocalSessionStarted &&
        gProfileContext.IsCoopActive() && CoopPersonalSave::Name(CoopPersonalSave::personal)) {
        if(state.role==coop::Role::Host)gRestoredHostProfile.clear();
        if(gPendingRestoreClearSession==state.sessionId && gPendingRestoreClearTicket==state.ticket &&
            CoopPersonalSave::SameName(CoopPersonalSave::personal,gPendingRestoreClearProfile)) {
            static DWORD lastCompletionAttempt=0;
            const DWORD now=GetTickCount();
            if(!lastCompletionAttempt || now-lastCompletionAttempt>=500) {
                lastCompletionAttempt=now;
                const auto role=state.role==coop::Role::Host?coop_backup::Role::Host:coop_backup::Role::Client;
                if(coop_backup::CompleteRestore(gCoopBaseRoot,role,WideText(CoopPersonalSave::personal.c_str()))) {
                    gPendingRestoreClearSession=0;gPendingRestoreClearTicket=0;gPendingRestoreClearProfile.clear();
                    LogMessage("[COOP_BACKUP] Restored career active; previous personal profile retained.\r\n");
                }
            }
        }
    }
    static DWORD lastWaitLog=0;
    const auto waiting=CoopSnapshot();
    if(waiting.joined&&!waiting.initialBarrierComplete&&waiting.ticket&&waiting.phase!=coop::Phase::Lobby&&
        GetTickCount()-lastWaitLog>=3000) {
        lastWaitLog=GetTickCount();char line[400]{};
        std::snprintf(line,sizeof(line),"[SESSION_REENTRY_WAIT] session=%llu membership=%u ticket=%u campaign=%s role=%u phase=%u flow=%lu menu=%u/%u retain=%u ready=%u/%u native=%u/%u pending=%u released=%u.\r\n",
            static_cast<unsigned long long>(waiting.sessionId),waiting.membershipNonce,waiting.ticket,waiting.campaignName,unsigned(waiting.role),unsigned(waiting.phase),flow,
            unsigned(waiting.localInMainMenu),unsigned(waiting.peerInMainMenu),unsigned(waiting.retainPlayingMask),unsigned(waiting.localCareerReady),unsigned(waiting.peerCareerReady),
            unsigned(waiting.localInitialReady),unsigned(waiting.peerInitialReady),unsigned(gCareerLaunchPending),gNativeCareerReleasedTicket);LogMessage(line);
    }
}
