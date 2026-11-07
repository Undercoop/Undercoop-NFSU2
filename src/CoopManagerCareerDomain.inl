// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Save identity belongs to a setup, never its mutable mod/settings digest.
// Fingerprints still protect multiplayer compatibility; they no longer select
// a different career directory whenever a mod or INI setting changes.
inline bool DefaultCareerBase(std::wstring& out){
    wchar_t local[MAX_PATH]{};if(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,0,local)!=S_OK)return false;
    out=local;return RealPath(out,true);
}
inline bool (*careerBase)(std::wstring&)=DefaultCareerBase;
inline bool CareerFolder(const std::wstring& folder){
    return folder.rfind(L"NFSU2 Undercoop ",0)==0&&folder.size()<=100&&
        folder.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ")==std::wstring::npos&&folder.back()!=L' ';
}
inline std::wstring CareerBindingPath(const std::wstring& id){
    return id.empty()?state.root+L"\\mods\\_undercoop\\state\\career-domain.txt":
        ModpackHome()+L"\\"+id+L"\\career-domain.txt";
}
inline bool ReadCareerBinding(const std::wstring& id,std::wstring& folder,bool& exists){
    folder.clear();exists=false;if(!id.empty()&&!ModpackToken(id))return Fail(L"Invalid career setup identity.");
    const auto path=CareerBindingPath(id);const DWORD attrs=GetFileAttributesW(path.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND||Fail(L"Cannot inspect this setup's saved career binding.");}
    exists=true;std::string text;
    if(!ReadText(path,text,256)||!CareerFolder(folder=Wide(text)))return Fail(L"This setup's career binding is invalid or linked. Existing careers were retained.");
    return true;
}
inline bool NewCareerBinding(const std::wstring& path,const std::wstring& folder){
    if(!CareerFolder(folder))return Fail(L"Invalid private career folder.");
    const auto parent=path.substr(0,path.find_last_of(L'\\'));auto owner=Pin(parent,true);if(!owner)return Fail(L"Cannot lock the career binding directory.");
    const auto pending=parent+L"\\"+ModpackTokenNew(L"pending-")+L".txt";
    if(!ModpackWriteNew(pending,coop_mod_scan::Utf8(folder)))return Fail(L"Cannot stage the career binding.");
    if(MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH))return true;
    const auto error=GetLastError();if(RealPath(pending))DeleteFileW(pending.c_str());
    std::string existing;if((error==ERROR_ALREADY_EXISTS||error==ERROR_FILE_EXISTS)&&ReadText(path,existing,256)&&Equal(Wide(existing),folder))return true;
    return Fail(L"The career binding could not be published. Existing bindings and careers were not overwritten.");
}
inline bool StableCareerFolder(const std::wstring& identity,const wchar_t* prefix,std::wstring& folder){
    const auto value=coop_mod_scan::Utf8(identity);coop_mod_scan::Sha256 sha;coop_mods::Hash hash{};
    if(!sha.Bytes(value.data(),value.size(),hash))return Fail(L"Cannot identify this career setup.");
    folder=std::wstring(prefix)+Wide(coop_mod_scan::Hex(hash).substr(0,24));return true;
}
inline bool ResolveCareerDomain(const std::wstring& legacy,std::wstring& folder){
    bool bound=false;if(!ReadCareerBinding(state.instanceId,folder,bound))return false;if(bound)return true;
    if(state.instanceId.empty()){if(!CareerFolder(legacy))return Fail(L"Invalid legacy career folder.");folder=legacy;return true;}
    return StableCareerFolder(state.instanceId,L"NFSU2 Undercoop Instance ",folder);
}
inline bool CloneCareerTree(const std::wstring& source,const std::wstring& destination,const std::atomic<bool>* cancel){
    const DWORD sourceAttrs=GetFileAttributesW(source.c_str());
    if(sourceAttrs==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND||Fail(L"Cannot inspect the current careers.");}
    auto from=Pin(source,true);if(!from)return Fail(L"The current career directory is linked or unreadable.");
    const DWORD existing=GetFileAttributesW(destination.c_str());
    if(existing!=INVALID_FILE_ATTRIBUTES)return RealPath(destination,true)||Fail(L"The existing career target is invalid; it was retained.");
    const auto missing=GetLastError();if(missing!=ERROR_FILE_NOT_FOUND&&missing!=ERROR_PATH_NOT_FOUND)return Fail(L"Cannot inspect the destination career directory.");
    const auto parent=destination.substr(0,destination.find_last_of(L'\\'));auto parentPin=Pin(parent,true);if(!parentPin)return Fail(L"The private career parent is linked or unreadable.");
    const auto staging=destination+L"."+ModpackTokenNew(L"pending-");
    if(!CreateDirectoryW(staging.c_str(),nullptr))return Fail(L"Cannot stage a private career snapshot.");
    auto staged=Pin(staging,true);BY_HANDLE_FILE_INFORMATION before{};
    if(!staged||!GetFileInformationByHandle(staged->value,&before))return Fail(L"Cannot lock the career staging directory.");
    ModpackCopyBudget budget;budget.cancel=cancel;
    if(!CopyModpackTree(source,staging,budget))return false;
    staged.reset();
    Handle stage(CreateFileW(staging.c_str(),FILE_LIST_DIRECTORY|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    BY_HANDLE_FILE_INFORMATION after{};
    if(stage.value==INVALID_HANDLE_VALUE||!RealPath(staging,true)||!GetFileInformationByHandle(stage.value,&after)||
        before.dwVolumeSerialNumber!=after.dwVolumeSerialNumber||before.nFileIndexHigh!=after.nFileIndexHigh||before.nFileIndexLow!=after.nFileIndexLow)
        return Fail(L"The career staging directory changed before publication.");
    std::vector<uint8_t> storage(sizeof(FILE_RENAME_INFO)+destination.size()*sizeof(wchar_t));
    auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());rename->ReplaceIfExists=FALSE;rename->RootDirectory=nullptr;
    rename->FileNameLength=DWORD(destination.size()*sizeof(wchar_t));std::memcpy(rename->FileName,destination.data(),rename->FileNameLength);
    if(!SetFileInformationByHandle(stage.value,FileRenameInfo,rename,DWORD(storage.size())))return Fail(L"Cannot publish the career snapshot; existing careers were retained.");
    return true;
}
inline bool CloneCareerPair(const std::wstring& source,const std::wstring& target,const std::atomic<bool>* cancel=nullptr){
    if(!CareerFolder(source)||!CareerFolder(target))return Fail(L"Invalid private career identity.");
    if(Equal(source,target))return true;std::wstring base;if(!careerBase(base))return Fail(L"Cannot locate the private career parent.");
    return CloneCareerTree(base+L"\\"+source,base+L"\\"+target,cancel)&&
        CloneCareerTree(base+L"\\"+source+L" SP",base+L"\\"+target+L" SP",cancel);
}
inline bool CommitCareerDomain(const std::wstring& legacy,const std::wstring& folder){
    std::wstring existing;bool bound=false;if(!ReadCareerBinding(state.instanceId,existing,bound))return false;
    if(bound)return Equal(existing,folder)||Fail(L"The career setup changed while preparing Play. Restart the launcher.");
    if(!state.instanceId.empty()&&!CloneCareerPair(legacy,folder))return false;
    return NewCareerBinding(CareerBindingPath(state.instanceId),folder);
}
inline bool PreviousCareerLog(const std::wstring& path,std::string& text){
    text.clear();if(!RealPath(path))return false;
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    BY_HANDLE_FILE_INFORMATION info{};wchar_t actual[MAX_PATH+4]{};
    if(file.value==INVALID_HANDLE_VALUE||GetFileType(file.value)!=FILE_TYPE_DISK||
        !GetFileInformationByHandle(file.value,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||
        !GetFinalPathNameByHandleW(file.value,actual,DWORD(std::size(actual)),FILE_NAME_NORMALIZED)||!Equal(actual,L"\\\\?\\"+path))return false;
    // SAVE_ROOT is written during startup. A long race log must not prevent
    // first-upgrade adoption, nor require loading its whole fleet trace.
    text.assign(65536,0);DWORD read=0;if(!ReadFile(file.value,text.data(),DWORD(text.size()),&read,nullptr))return false;
    text.resize(read);return true;
}
inline bool PreviousCareerDomain(std::wstring& folder){
    std::wstring base;if(!careerBase(base))return false;
    // First-upgrade fallback reads only this installation's own previous
    // explicit SAVE_ROOT record. Never choose another global save by date.
    FILETIME newest{};std::string best;
    for(const wchar_t* file:{L"NFSU2Coop.log",L"NFSU2Coop_2.log"}){
        const auto path=state.root+L"\\scripts\\"+file;WIN32_FILE_ATTRIBUTE_DATA info{};std::string text;
        if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info)&&PreviousCareerLog(path,text)&&
            (best.empty()||CompareFileTime(&info.ftLastWriteTime,&newest)>0)){best=std::move(text);newest=info.ftLastWriteTime;}
    }
    const std::string marker="[SAVE_ROOT] Bind SP='";const auto record=best.rfind(marker);if(record==std::string::npos)return false;
    const auto begin=best.find(" COOP='",record);if(begin==std::string::npos)return false;
    const auto end=best.find('\'',begin+7);if(end==std::string::npos)return false;
    const auto full=Wide(best.substr(begin+7,end-begin-7));
    if(!Below(full,base))return false;const auto name=full.substr(base.size()+1);if(!CareerFolder(name))return false;
    bool any=false;
    for(const auto& candidate:{full,full+L" SP"}){
        const auto attributes=GetFileAttributesW(candidate.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)return false;}
        else {if(!RealPath(candidate,true))return false;any=true;}
    }
    if(!any)return false;folder=name;return true;
}
inline bool CurrentCareerDomain(std::wstring& folder){
    bool bound=false;if(!ReadCareerBinding(state.instanceId,folder,bound))return false;if(bound)return true;
    if(PreviousCareerDomain(folder))return NewCareerBinding(CareerBindingPath(state.instanceId),folder);
    // A new setup with no prior game session has no career to migrate.
    if(!StableCareerFolder(state.instanceId.empty()?state.root:state.instanceId,
        state.instanceId.empty()?L"NFSU2 Undercoop Default ":L"NFSU2 Undercoop Instance ",folder))return false;
    return NewCareerBinding(CareerBindingPath(state.instanceId),folder);
}
inline bool SnapshotModpackCareers(const std::wstring& id,const std::wstring& pending,const std::atomic<bool>* cancel){
    std::wstring source,target;if(!CurrentCareerDomain(source)||!StableCareerFolder(id,L"NFSU2 Undercoop Instance ",target))return false;
    if(cancel&&cancel->load())return Fail(L"Modpack creation cancelled safely.");
    if(!CloneCareerPair(source,target,cancel))return false;
    return NewCareerBinding(pending+L"\\career-domain.txt",target);
}
