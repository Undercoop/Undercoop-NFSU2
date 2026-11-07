// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Launcher-only import. The native profile file is the only imported artifact.
struct CareerImportSource {std::wstring path,profile;bool stock=false;};

inline bool ReadCareerImport(const std::wstring& path,std::string& bytes,std::string& name){
    constexpr size_t identity=0x24+coop_career::kPersonalNameOffset;
    static_assert(identity+32<=coop_save::kNativeProfileFileSize);
    if(!ReadText(path,bytes,coop_save::kNativeProfileFileSize) ||
       coop_save::ValidateNativeProfileFile(reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size())!=coop_save::SaveValidation::Valid)return false;
    const auto* begin=bytes.data()+identity;
    const auto* end=std::find(begin,begin+32,'\0');
    if(end==begin+32)return false;
    name.assign(begin,end);
    return coop_career_history::SafeName(name);
}

inline bool PreviewCareerImport(const std::wstring& path,std::wstring& profile,std::wstring& message){
    profile.clear();message.clear();std::string bytes,name;
    if(!ReadCareerImport(path,bytes,name)){
        message=L"Choose a valid NFSU2 PC profile file with a supported name. Extract downloaded saves first.";
        return false;
    }
    profile=Wide(name);
    return true;
}

inline std::vector<CareerImportSource> ListCareerImportSources(){
    std::vector<CareerImportSource> sources;std::wstring base;
    if(!careerBase(base))return sources;
    const auto active=CareerBackupRoot();
    const auto stock=base+L"\\NFS Underground 2";
    for(const auto& root:{active.empty()?std::wstring{}:active+L" SP",stock}){
        auto directory=Pin(root,true);if(!directory)continue;
        WIN32_FIND_DATAW item{};const auto scan=FindFirstFileW((root+L"\\*").c_str(),&item);
        if(scan==INVALID_HANDLE_VALUE)continue;
        size_t inspected=0;
        do{
            if(!(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))continue;
            const std::wstring leaf=item.cFileName;
            if(!coop_save::IsSafeProfileName(leaf))continue;
            const auto path=root+L"\\"+leaf+L"\\"+leaf;
            std::string bytes,name;
            if(ReadCareerImport(path,bytes,name) && Equal(Wide(name),leaf))sources.push_back({path,Wide(name),Equal(root,stock)});
        }while(++inspected<256 && FindNextFileW(scan,&item));
        FindClose(scan);
    }
    std::stable_sort(sources.begin(),sources.end(),[](const auto& a,const auto& b){
        return CompareStringOrdinal(a.profile.c_str(),-1,b.profile.c_str(),-1,TRUE)==CSTR_LESS_THAN;
    });
    return sources;
}

inline std::string CareerImportClientName(const std::string& name){
    // Match CoopPersonalSave::ClientName, including its 31-byte long-name alias.
    if(name.size()<=28)return name+"_P2";
    uint32_t hash=2166136261u;
    for(char ch:name){hash^=static_cast<unsigned char>(coop_career_history::Fold(ch));hash*=16777619u;}
    char suffix[16]{};std::snprintf(suffix,sizeof(suffix),"_%08X_P2",hash);
    return name.substr(0,19)+suffix;
}

struct CareerImportTarget {
    std::wstring role,profile,directory,stage;
    std::string bytes;
    std::unique_ptr<Handle> parent;
    bool exists=false;
    std::unique_ptr<Handle> stageOwner;
    BY_HANDLE_FILE_INFORMATION stageIdentity{};
    bool LockStage(){
        stageOwner=Pin(stage,true);
        return stageOwner && GetFileInformationByHandle(stageOwner->value,&stageIdentity);
    }
    std::unique_ptr<Handle> OwnedStage(){
        stageOwner.reset();
        auto file=std::make_unique<Handle>(CreateFileW(stage.c_str(),FILE_LIST_DIRECTORY|DELETE,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION current{};wchar_t actual[MAX_PATH+4]{};
        if(file->value==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file->value,&current) ||
           !(current.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (current.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
           current.dwVolumeSerialNumber!=stageIdentity.dwVolumeSerialNumber ||
           current.nFileIndexHigh!=stageIdentity.nFileIndexHigh || current.nFileIndexLow!=stageIdentity.nFileIndexLow ||
           !GetFinalPathNameByHandleW(file->value,actual,DWORD(std::size(actual)),FILE_NAME_NORMALIZED) ||
           !Equal(actual,L"\\\\?\\"+stage))return {};
        return file;
    }
    bool Publish(){
        auto file=OwnedStage();if(!file)return false;
        std::vector<uint8_t> storage(sizeof(FILE_RENAME_INFO)+directory.size()*sizeof(wchar_t));
        auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());rename->ReplaceIfExists=FALSE;
        rename->RootDirectory=nullptr;rename->FileNameLength=DWORD(directory.size()*sizeof(wchar_t));
        std::memcpy(rename->FileName,directory.data(),rename->FileNameLength);
        if(!SetFileInformationByHandle(file->value,FileRenameInfo,rename,DWORD(storage.size())))return false;
        stage.clear();return true;
    }
    ~CareerImportTarget(){
        if(stage.empty())return;
        auto directoryOwner=OwnedStage();if(!directoryOwner)return;
        const auto file=stage+L"\\"+profile;
        if(RealPath(file))DeleteFileW(file.c_str());
        FILE_DISPOSITION_INFO remove{TRUE};
        SetFileInformationByHandle(directoryOwner->value,FileDispositionInfo,&remove,sizeof(remove));
    }
};

inline bool ImportCareerProfile(const std::wstring& path,const std::wstring& expectedProfile,std::wstring& message){
    message.clear();
    if(state.ready || state.fatal){message=L"Import saves before starting the game.";return false;}
    std::string host,name;
    if(!ReadCareerImport(path,host,name) || Wide(name)!=expectedProfile){
        message=L"The selected profile changed or could not be verified. Choose the save again.";return false;
    }
    const auto clientName=CareerImportClientName(name);
    if(!coop_career_history::SafeName(clientName)){
        message=L"This profile name cannot be used for the Player 2 copy.";return false;
    }
    std::vector<uint8_t> payload(host.begin()+0x24,host.end()),client;
    auto identity=payload.begin()+coop_career::kPersonalNameOffset;
    std::fill_n(identity,32,uint8_t(0));std::copy(clientName.begin(),clientName.end(),identity);
    if(!coop_save::EncodeNativeProfileFile(payload,client)){
        message=L"The Player 2 profile could not be prepared.";return false;
    }
    std::wstring base,folder;
    if(!careerBase(base)){message=L"The local save folder is unavailable.";return false;}
    if(!CurrentCareerDomain(folder)){message=state.error;state.error.clear();return false;}
    const auto root=base+L"\\"+folder;
    auto basePin=Pin(base,true);
    if(!basePin || !ModpackDirectory(root)){
        message=L"The active setup's CO-OP save folder is unavailable.";return false;
    }
    auto rootPin=Pin(root,true);
    if(!rootPin){message=L"The CO-OP save folder could not be locked.";return false;}
    CareerImportTarget targets[]{
        {L"Host",expectedProfile,{}, {},std::move(host)},
        {L"Client",Wide(clientName),{}, {},std::string(client.begin(),client.end())}
    };
    for(auto& target:targets){
        const auto role=root+L"\\"+target.role;
        if(!ModpackDirectory(role) || !(target.parent=Pin(role,true))){
            message=L"A CO-OP player save folder is unavailable.";return false;
        }
        target.directory=role+L"\\"+target.profile;
        const auto attrs=GetFileAttributesW(target.directory.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES){
            if(GetLastError()!=ERROR_FILE_NOT_FOUND && GetLastError()!=ERROR_PATH_NOT_FOUND){
                message=L"Could not check the destination profile.";return false;
            }
            continue;
        }
        std::string current;
        if(!ReadText(target.directory+L"\\"+target.profile,current,coop_save::kNativeProfileFileSize) || current!=target.bytes){
            message=L"A different CO-OP profile named "+expectedProfile+L" already exists for "+
                (target.role==L"Host"?L"Host":L"Player 2")+L". The existing save was kept.";return false;
        }
        target.exists=true;
    }
    for(auto& target:targets){
        if(target.exists)continue;
        const auto role=target.directory.substr(0,target.directory.find_last_of(L'\\'));
        const auto stage=role+L"\\"+ModpackTokenNew(L"pending-import-");
        if(stage.size()+1+target.profile.size()>=MAX_PATH || !CreateDirectoryW(stage.c_str(),nullptr)){
            message=L"Could not stage the imported save.";return false;
        }
        target.stage=stage;
        if(!target.LockStage() || !ModpackWriteNew(stage+L"\\"+target.profile,target.bytes)){
            message=L"Could not write the staged save.";return false;
        }
    }
    for(auto& target:targets){
        if(target.exists)continue;
        // Publish the entire profile as a new directory. Concurrent imports
        // cannot replace one another or merge old CO-OP sidecars into this save.
        if(!target.Publish()){
            message=L"Import could not finish. Existing profiles were kept. You can retry the same import.";
            return false;
        }
    }
    message=L"Imported "+expectedProfile+L" for both roles. The Host can now choose it in CO-OP Load Career.";
    return true;
}
