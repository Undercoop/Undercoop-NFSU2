// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#define main CoopExistingFolderTests_NotInvoked
#include "CoopManagerFolderImportTests.cpp"
#undef main

static HANDLE OpenWriter(const std::wstring& path) {
    const auto writer=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Need(writer!=INVALID_HANDLE_VALUE,"open exclusive writer on fixture file");
    return writer;
}

static void ManagedReShadeWrite() {
    Fixture fixture;
    const auto root=fixture.Use(L"game");
    const std::wstring relative=L"mods\\reshade\\files\\scripts\\reshade.ini";
    const auto ini=fixture.File(L"game\\"+relative,"[GENERAL]\r\nPresetPath=.\\Bayview.ini\r\n");
    m::ConfigWatch watch;
    watch.physical=ini;
    Need(coop_mod_scan::RelativeName(relative,watch.relative),"canonical watch path");
    coop_mod_scan::Sha256 sha;
    uint64_t bytes=0;
    std::string error;
    Need(coop_mod_scan::HashFile({ini,"scripts/reshade.ini",true},sha,watch.expected,bytes,error),"capture fixture baseline");
    m::state.watches.push_back(watch);
    coop_mod_scan::managedChange=&m::ManagedConfigChanged;

    const auto bufferSize=offsetof(FILE_NOTIFY_INFORMATION,FileName)+relative.size()*sizeof(wchar_t);
    std::vector<uint8_t> notification(bufferSize);
    auto* event=reinterpret_cast<FILE_NOTIFY_INFORMATION*>(notification.data());
    event->Action=FILE_ACTION_MODIFIED;
    event->FileNameLength=DWORD(relative.size()*sizeof(wchar_t));
    std::memcpy(event->FileName,relative.data(),event->FileNameLength);
    auto checkNotification=[&](coop_mod_scan::Change& change) {
        return coop_mod_scan::Changed(root,notification.data(),DWORD(notification.size()),{},change);
    };

    coop_mod_scan::Change change;
    auto writer=OpenWriter(ini);
    std::thread releaseUnchanged([writer]{Sleep(70);CloseHandle(writer);});
    CHECK(!checkNotification(change));
    releaseUnchanged.join();
    CHECK(change.reason.empty());

    writer=OpenWriter(ini);
    std::thread releaseChanged([writer]{
        Sleep(70);
        constexpr char edited[]="[GENERAL]\r\nPresetPath=.\\Olympic.ini\r\n";
        DWORD written=0;
        Need(WriteFile(writer,edited,sizeof(edited)-1,&written,nullptr)&&written==sizeof(edited)-1&&SetEndOfFile(writer),"rewrite fixture INI");
        CloseHandle(writer);
    });
    CHECK(checkNotification(change));
    releaseChanged.join();
    CHECK(change.reason=="managed component shared settings changed"&&change.error==ERROR_SUCCESS);

    writer=OpenWriter(ini);
    CHECK(checkNotification(change));
    CHECK(change.error==ERROR_SHARING_VIOLATION);
    CloseHandle(writer);
    CHECK(DeleteFileW(ini.c_str())!=FALSE);
    CHECK(checkNotification(change));
    CHECK(change.error==ERROR_FILE_NOT_FOUND);
    coop_mod_scan::managedChange=nullptr;
    CHECK(nativeLoads==0);
}

static void InitialScanWaitsForWriter() {
    Fixture fixture;
    const auto root=fixture.Use(L"scan");
    fixture.File(L"scan\\SPEED2.EXE","fixture executable");
    const auto ini=fixture.File(L"scan\\scripts\\reshade.ini","[GENERAL]\nPresetPath=Bayview.ini\n");
    const auto expected=coop_mod_scan::Scan(root,"same options");
    Need(expected.Ready(),"scan clean fixture");
    const auto writer=OpenWriter(ini);
    std::thread release([writer]{Sleep(70);CloseHandle(writer);});
    const auto retried=coop_mod_scan::Scan(root,"same options");
    release.join();
    CHECK(retried.Ready()&&retried.hash==expected.hash);
}

int main() {
    ManagedReShadeWrite();
    InitialScanWaitsForWriter();
    std::printf("Mod INI watch: %u checks, %u failures, %zu native loads.\n",checks,failures,nativeLoads);
    return failures?1:0;
}
