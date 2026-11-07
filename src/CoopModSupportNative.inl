// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Included after CoopNativeSession.inl. No native game calls on the scanner or
// network thread. No asset downloading, installation or save mutation here.
namespace CoopModSupport {
static SRWLOCK lock=SRWLOCK_INIT;
static coop_mod_scan::Inventory inventory;
static coop_mods::Exchange exchange;
static coop::Endpoint boundPeer{};static uint64_t boundSession=0;static uint32_t boundMembership=0;
static std::atomic<bool> scanning{false},scanDone{false},invalidated{false},catalogReady{false};
static std::atomic<bool> vehicleAssetsReady{false};
static bool (*vehicleAssetsProbe)()=ProbeCoopVehicleAssetAdaptersReady;
static bool RefreshVehicleAssets() {
    const bool ready=vehicleAssetsProbe();vehicleAssetsReady.store(ready);return ready;
}
static std::atomic<unsigned> status{0};
static std::atomic<bool> peerFailed{false};
static bool started=false,reported=false;
static std::wstring root;
static std::string settings;
static const char* Status() {
    if(status.load()==2){const auto state=CoopSnapshot();if(state.joined&&!CoopConnectedTransportReady(state))
        return gCoopTransportOpened.load()?"WAITING FOR GAMEPLAY CONNECTION - CHECK NETWORK PORTS":"GAMEPLAY NETWORK PORT UNAVAILABLE - CHECK CONSOLE (Z)";}
    if(invalidated.load())return "MOD FILES CHANGED - RESTART BOTH GAMES";
    if(scanDone.load()&&status.load()!=4&&!gCoopCareerHooksReady.load())return "NATIVE COOP HOOKS NOT READY - CHECK CONSOLE (Z)";
    if(scanDone.load()&&status.load()!=4&&!vehicleAssetsReady.load())return "TRAFFIC/AI MOD CONFLICT - CHECK CONSOLE (Z)";
    if(scanDone.load()&&status.load()!=4&&!catalogReady.load())return "CAR CATALOG/LOADER NOT READY - CHECK CONSOLE (Z)";
    switch(status.load()) {
    case 0:return "CHECKING INSTALLED FILES...";
    case 1:return "WAITING FOR TEAMMATE'S MOD CHECK...";
    case 2:return "MOD PACK MATCHED";
    case 3:return "MOD PACK MISMATCH - CHECK CONSOLE (Z)";
    case 4:return "MOD CHECK FAILED - CHECK CONSOLE (Z)";
    case 5:return "CAR LOADER/CATALOG UNSUPPORTED - CHECK CONSOLE (Z)";
    case 7:return "TEAMMATE MOD CHECK FAILED - CHECK CONSOLE (Z)";
    }return "CHECKING INSTALLED FILES...";
}
static void ReportFile(const std::string& text) {
    if(root.empty())return;const auto path=root+L"\\scripts\\NFSU2Coop-Mods.txt";
    const DWORD attrs=GetFileAttributesW(path.c_str());
    if(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;DWORD written=0;
    WriteFile(file,text.data(),DWORD(text.size()),&written,nullptr);CloseHandle(file);
}
static bool SameNormalizedIni(const std::string& path) {
    if(path!="scripts/nfsu2extraoptionssettings.ini"&&path!="scripts/nfsunderground2.widescreenfix.ini")return false;
    coop_mods::Entry expected;bool found=false;AcquireSRWLockShared(&lock);
    for(const auto& item:inventory.entries)if(item.path==path){expected=item;found=true;break;}
    ReleaseSRWLockShared(&lock);if(!found)return false;
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.data(),int(path.size()),nullptr,0);
    if(count<=0)return false;std::wstring relative(size_t(count),L'\0');
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.data(),int(path.size()),relative.data(),count))return false;
    for(auto& c:relative)if(c==L'/')c=L'\\';
    coop_mod_scan::Sha256 sha;coop_mods::Entry actual;uint64_t read=0;std::string error;
    return coop_mod_scan::HashFile({root+L"\\"+relative,path},sha,actual,read,error)&&actual.size==expected.size&&actual.hash==expected.hash;
}
static bool RelevantChanges(const uint8_t* bytes,DWORD length) {
    std::vector<coop_mods::Entry> baseline;AcquireSRWLockShared(&lock);baseline=inventory.entries;ReleaseSRWLockShared(&lock);
    coop_mod_scan::Change reason;return coop_mod_scan::Changed(root,bytes,length,baseline,reason);
}
static void LogModChange(const coop_mod_scan::Change& change) {
    const std::string line="[MODS] Compatibility revoked: path='"+change.path+"' action="+std::to_string(change.action)+
        " reason='"+change.reason+"' error="+std::to_string(change.error)+"; restart required before a new career load.\r\n";
    LogMessage(line.c_str());
}
static DWORD WINAPI ScanWorker(void*) {
    SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_BEGIN);
    HANDLE directory=CreateFileW(root.c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,nullptr);
    HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);OVERLAPPED pending{};pending.hEvent=event;
    std::array<uint8_t,64*1024> notifications{};
    const DWORD filters=FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE;
    bool watching=directory!=INVALID_HANDLE_VALUE&&event&&ReadDirectoryChangesW(directory,notifications.data(),DWORD(notifications.size()),TRUE,
        filters,nullptr,&pending,nullptr);
    coop_mod_scan::Inventory scanned;
    if(watching)scanned=coop_mod_scan::Scan(root,settings);
    else scanned.error="runtime file change monitoring could not be started";
    const auto baseline=scanned.entries;
    // Monitoring is armed BEFORE hashing. A replaced/new/deleted asset cannot
    // be approved merely because its old handle's timestamp stayed unchanged.
    auto inspectBatch=[&]() {
        DWORD got=0;coop_mod_scan::Change change;
        if(!GetOverlappedResult(directory,&pending,&got,FALSE)||got>notifications.size()) {
            change.reason="notification read failed";change.error=GetLastError();invalidated.store(true);LogModChange(change);return false;
        }
        const std::vector<uint8_t> batch(notifications.begin(),notifications.begin()+got);
        // Keep collecting events while hashes are verified in the background.
        ResetEvent(event);pending={};pending.hEvent=event;
        watching=ReadDirectoryChangesW(directory,notifications.data(),DWORD(notifications.size()),TRUE,filters,nullptr,&pending,nullptr)!=FALSE;
        if(!watching){invalidated.store(true);change.reason="watcher rearm failed";change.error=GetLastError();LogModChange(change);return false;}
        if(coop_mod_scan::Changed(root,batch.data(),DWORD(batch.size()),baseline,change)) {
            invalidated.store(true);LogModChange(change);return false;
        }return true;
    };
    while(watching&&!invalidated.load()) {
        const DWORD wait=WaitForSingleObject(event,0);
        if(wait==WAIT_TIMEOUT)break;
        if(wait!=WAIT_OBJECT_0) {
            coop_mod_scan::Change change;change.reason="initial notification wait failed";change.error=GetLastError();
            invalidated.store(true);LogModChange(change);break;
        }
        if(!inspectBatch())break;
    }
    std::string report="Undercoop " UNDERCOOP_BUILD_VERSION " - local installed-file inventory\r\nNo files were received or installed. No saves are included.\r\n";
    if(scanned.Ready()) {
        char line[192]{};std::snprintf(line,sizeof(line),"[MODS] Scanned %u runtime files, %llu bytes in %lu ms; SHA-256=%s.\r\n",
            unsigned(scanned.entries.size()),static_cast<unsigned long long>(scanned.bytes),scanned.milliseconds,coop_mod_scan::Hex(scanned.hash).c_str());LogMessage(line);
        for(const auto& item:scanned.entries)report+=coop_mod_scan::Hex(item.hash)+"  "+std::to_string(item.size)+"  "+item.path+"\r\n";
    }else {report+="ERROR: "+scanned.error+"\r\n";LogMessage(("[MODS] Inventory failed: "+scanned.error+". Career start remains blocked.\r\n").c_str());}
    ReportFile(report);
    AcquireSRWLockExclusive(&lock);inventory=std::move(scanned);status.store(inventory.Ready()?1:4);scanDone.store(true);ReleaseSRWLockExclusive(&lock);
    scanning.store(false);
    while(watching&&!invalidated.load()) {
        const DWORD wait=WaitForSingleObject(event,500);
        if(wait==WAIT_TIMEOUT)continue;
        coop_mod_scan::Change change;
        if(wait!=WAIT_OBJECT_0) {
            change.reason="notification wait failed";change.error=GetLastError();invalidated.store(true);LogModChange(change);break;
        }
        if(!inspectBatch())break;
    }
    if(directory!=INVALID_HANDLE_VALUE){CancelIoEx(directory,nullptr);CloseHandle(directory);}
    if(event)CloseHandle(event);return 0;
}
static void Start() {
    if(started)return;started=true;
    wchar_t executable[32768]{};const auto length=GetModuleFileNameW(nullptr,executable,DWORD(std::size(executable)));
    if(!length||length>=std::size(executable)){status.store(4);return;}
    root.assign(executable,length);const auto slash=root.find_last_of(L"\\/");
    if(slash==std::wstring::npos){status.store(4);return;}root.resize(slash);
    const auto directory=NarrowText(root);
    if(directory.empty()||!CoopCarCatalogNative::InitializeFromGameDirectory(directory.c_str())) {
        status.store(5);LogMessage((std::string("[MODS] Installed car catalog rejected: ")+CoopCarCatalogNative::Failure()+".\r\n").c_str());return;
    }
    if(CoopCarCatalogNative::Count()>coop_car_catalog::kStockCount&&!InstallCoopStockCatalogReaderHook()){
        status.store(5);LogMessage("[MODS] Addon stock-cache import observer conflicts with the installed game/mod code. Career start remains blocked; no hook was replaced.\r\n");return;
    }
    settings="Undercoop=" UNDERCOOP_BUILD_VERSION "\nSaveDomain="+std::string(coop_build::kSaveDomain)+"\nRaceAI="+std::to_string(gCoopAIEnabled?1:0)+"\nWorldFleet="+
        std::to_string(gCoopFleetEnabled?1:0)+"\nCarCount="+std::to_string(CoopCarCatalogNative::Count())+
        "\nCarMapping="+std::to_string(CoopCarCatalogNative::Footprint())+"\n";
#ifdef UNDERCOOP_MOD_MANAGER
    settings+="ManagedPack="+CoopModManager::state.pack+"\n";
#endif
    scanning.store(true);HANDLE worker=CreateThread(nullptr,0,ScanWorker,nullptr,0,nullptr);
    if(worker)CloseHandle(worker);else{scanning.store(false);status.store(4);}
}
static void ResetPeer() {
    AcquireSRWLockExclusive(&lock);exchange={};boundSession=0;boundMembership=0;boundPeer={};reported=false;peerFailed.store(false);
    if(status.load()==2||status.load()==3)status.store(1);ReleaseSRWLockExclusive(&lock);
}
static bool Ready(const coop::SessionSnapshot& state) {
    if(!state.joined||!state.fresh||invalidated.load()||!catalogReady.load()||!vehicleAssetsReady.load()||peerFailed.load()||!gCoopCareerHooksReady.load())return false;
    AcquireSRWLockShared(&lock);
    const bool ready=state.sessionId==boundSession&&state.peer==boundPeer&&state.membershipNonce==boundMembership&&exchange.Ready()&&inventory.Ready();
    ReleaseSRWLockShared(&lock);return ready;
}
static void TickNetwork(DWORD now) {
    const auto state=CoopSnapshot();
    if(!state.joined||!state.fresh){ResetPeer();return;}
    const unsigned ownStatus=status.load();
    if(invalidated.load()||ownStatus==4||ownStatus==5||(scanDone.load()&&!gCoopCareerHooksReady.load())) {
        static DWORD lastFailure=0;if(now-lastFailure>=500) {
            lastFailure=now;coop_mods::Packet failure;failure.session=state.sessionId;
            failure.token=GetCurrentProcessId()?GetCurrentProcessId():1;failure.type=coop_mods::Type::Failure;
            failure.runtimeReady=0;failure.index=invalidated.load()?4:ownStatus==5?2:ownStatus==4?1:3;
            SendSessionBytes(&failure,sizeof(failure),state.peer);
        }return;
    }
    if(!scanDone.load())return;
    std::array<coop_mods::Packet,4> output{};size_t count=0;std::vector<uint8_t> remote;coop_mods::Hash expected{};
    std::vector<coop_mods::Entry> local;
    AcquireSRWLockExclusive(&lock);
    if(inventory.Ready()) {
        if(boundSession!=state.sessionId||boundPeer!=state.peer||boundMembership!=state.membershipNonce||!exchange.Bound()) {
            static uint32_t serial=0;LARGE_INTEGER clock{};QueryPerformanceCounter(&clock);
            uint32_t token=uint32_t(clock.QuadPart)^GetCurrentProcessId()^(++serial);if(!token)token=++serial;
            exchange.Bind(state.sessionId,token,inventory.blob,inventory.hash);boundSession=state.sessionId;boundPeer=state.peer;
            boundMembership=state.membershipNonce;reported=false;peerFailed.store(false);
        }
        exchange.SetRuntimeReady(catalogReady.load()&&vehicleAssetsReady.load()&&gCoopCareerHooksReady.load());
        count=exchange.Tick(now,output);
        status.store(peerFailed.load()?7:exchange.Ready()?2:exchange.Mismatch()?3:1);
        if(!reported&&exchange.PopRemote(remote,expected)){reported=true;local=inventory.entries;}
    }
    ReleaseSRWLockExclusive(&lock);
    for(size_t i=0;i<count;++i)SendSessionBytes(&output[i],sizeof(output[i]),state.peer);
    if(remote.empty())return;
    coop_mod_scan::Sha256 sha;coop_mods::Hash actual{};std::vector<coop_mods::Entry> peer;
    if(!sha.Bytes(remote.data(),remote.size(),actual)||actual!=expected||!coop_mods::Decode(remote,peer)) {
        LogMessage("[MODS] Peer inventory hash/structure rejected. No file paths were opened and no career start authorized.\r\n");
        CoopAnnouncements::Post("Peer mod list could not be verified. Reconnect both players.");return;
    }
    const auto differences=coop_mods::Compare(local,peer);
    std::string report="Undercoop " UNDERCOOP_BUILD_VERSION " - mod pack mismatch\r\nCompared with connected teammate. Nothing was installed, removed or received except file metadata.\r\n";
    unsigned shown=0;
    for(const auto& difference:differences) {
        const char* kind=difference.kind==coop_mods::Difference::MissingHere?"MISSING HERE":
            difference.kind==coop_mods::Difference::ExtraHere?"ONLY HERE":"DIFFERENT CONTENT/SETTINGS";
        const std::string line=std::string(kind)+": "+difference.path;
        report+=line+"\r\n";
        if(shown<64)LogMessage(("[MODS] "+line+"\r\n").c_str());
        if(shown++<6)CoopAnnouncements::Post(line.c_str());
    }
    report+="\r\nMatch the actual installed game/mod files and gameplay settings, then restart both games.\r\n"
        "IPs, ports, names, saves and known display/multi-instance options are personal.\r\n"
        "Matching files does not certify arbitrary executable mods as compatible.\r\n";
    ReportFile(report);
    LogMessage(("[MODS] "+std::to_string(differences.size())+" differences; full report scripts/NFSU2Coop-Mods.txt. Career start blocked.\r\n").c_str());
    CoopAnnouncements::Post("Career start blocked. Full differences: scripts/NFSU2Coop-Mods.txt");
}
static bool Receive(const char* data,int bytes,const sockaddr_in& sender,DWORD now) {
    uint32_t magic=0;if(bytes<4)return false;std::memcpy(&magic,data,4);if(magic!=coop_mods::kMagic)return false;
    if(bytes!=sizeof(coop_mods::Packet)||!IsJoinedCoopSender(sender)||invalidated.load())return true;
    const auto state=CoopSnapshot();coop_mods::Packet packet{},ack{};std::memcpy(&packet,data,sizeof(packet));
    if(!coop_mods::Valid(packet)||packet.session!=state.sessionId)return true;
    if(packet.type==coop_mods::Type::Failure) {
        if(!peerFailed.exchange(true))CoopAnnouncements::Post("Your teammate's mod check failed. They must check their console/report and restart.");
        return true;
    }
    bool reply=false;AcquireSRWLockExclusive(&lock);
    if(state.sessionId==boundSession&&state.peer==boundPeer&&state.membershipNonce==boundMembership) {
        reply=exchange.Receive(packet,now,ack);
        if(packet.type==coop_mods::Type::Hello&&packet.runtimeReady)peerFailed.store(false);
    }
    ReleaseSRWLockExclusive(&lock);if(reply)SendSessionBytes(&ack,sizeof(ack),state.peer);return true;
}
static void TickUi(DWORD flow) {
    // Stock/runtime tables are established by the native frontend loader. This
    // is the only thread allowed to publish native-catalog eligibility.
    static DWORD lastCatalog=0;const DWORD now=GetTickCount();
    if((flow==3||flow==5||flow==6)&&now-lastCatalog>=500) {
        lastCatalog=now;RefreshVehicleAssets();
        if(flow!=5)catalogReady.store(CoopCarCatalogNative::PrepareRuntimeCatalog());
    }
    TickCoopStockCatalog(flow);TraceCoopAddonMenus(flow);
    const auto state=CoopSnapshot();if(!state.joined||!state.fresh)return;
    static unsigned previous=UINT32_MAX;
    const unsigned current=invalidated.load()?6:scanDone.load()&&!gCoopCareerHooksReady.load()?8:
        scanDone.load()&&!vehicleAssetsReady.load()?9:scanDone.load()&&!catalogReady.load()?5:
        status.load()==2&&!CoopConnectedTransportReady(state)?10:status.load();
    if(previous==current)return;previous=current;
    if(current==2)CoopAnnouncements::Post("Mod pack matched. Both players can start the career.");
    else if(current==3)CoopAnnouncements::Post("Mod pack mismatch. Comparing missing/different files...");
    else if(current==4)CoopAnnouncements::Post("Mod scan failed. Check scripts/NFSU2Coop-Mods.txt. Career start is blocked.");
    else if(current==10)CoopAnnouncements::Post("Waiting for the gameplay connection. Both computers must allow their configured UDP port and that port + 1000.");
    else if(current==5) {
        const auto message=std::string("Car catalog/loader rejected: ")+CoopCarCatalogNative::Failure();
        LogMessage(("[MODS] "+message+"\r\n").c_str());CoopAnnouncements::Post(message.c_str());
    }else if(current==6)CoopAnnouncements::Post("Installed files changed. Restart both games before starting another career.");
    else if(current==7)CoopAnnouncements::Post("Your teammate's mod check failed. Career start is blocked until they fix it.");
    else if(current==8)CoopAnnouncements::Post("Native CO-OP hooks could not be verified. Check the startup log for a conflicting mod/game version.");
    else if(current==9) {
        LogMessage("[MODS] Traffic/AI native adapter rejected a changed game function. Career start blocked; no traffic/AI settings or saves changed. Check FLEET_ASSETS diagnostics.\r\n");
        CoopAnnouncements::Post("Traffic/AI mod conflict. Career start blocked. Check FLEET_ASSETS in the log; restart after fixing the conflicting mod.");
    }
    else CoopAnnouncements::Post("Checking installed mods. Career start unlocks when both packs match.");
}
}
bool CoopModsReady(const coop::SessionSnapshot& state){return CoopConnectedTransportReady(state)&&CoopModSupport::Ready(state);}
bool RefreshCoopVehicleAssetAdaptersReady(){return CoopModSupport::RefreshVehicleAssets();}
const char* CoopModStatus(){return CoopModSupport::Status();}
void ResetCoopModPeer(){CoopModSupport::ResetPeer();}
void TickCoopModNetwork(DWORD now){CoopModSupport::TickNetwork(now);}
bool ReceiveCoopModPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now){return CoopModSupport::Receive(data,bytes,sender,now);}
void TickCoopModUi(DWORD flow){CoopModSupport::TickUi(flow);}
void StartCoopModScan(){CoopModSupport::Start();}
