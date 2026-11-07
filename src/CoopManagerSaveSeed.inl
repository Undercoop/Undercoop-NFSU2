// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Included in CoopModManager. Local preview seed only, never a downloaded mod.
// Import copies are staged per role into a NEW pack-bound domain. Existing role
// directories are never overwritten, merged, repaired or replaced.
struct SeedFile {std::string relative,bytes;};
inline bool ImportSeedAt(const std::wstring& destination){
    const auto base=state.root+L"\\mods\\_undercoop\\import";
    const auto list=base+L"\\files.txt";
    const DWORD attrs=GetFileAttributesW(list.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES&&(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND))return true;
    std::string index;if(!ReadText(list,index,16384))return Fail(L"The optional career import index is invalid.");
    const std::set<std::string> allow{
        "Host/Player/Player","Host/Player/Player.coop-progress","Host/Player/Player.coop-progress.previous","Host/Player/Player.previous",
        "Client/Player/Player","Client/Player_P2/Player","Client/Player_P2/Player_P2","Client/Player_P2/Player_P2.coop-progress",
        "Client/Player_P2/Player_P2.coop-progress.previous","Client/Player_P2/Player_P2.previous"
    };
    std::set<std::string> seen;std::vector<SeedFile> files;size_t start=0;
    while(start<index.size()){
        const auto end=index.find('\n',start);auto line=index.substr(start,end==std::string::npos?end:end-start);
        if(!line.empty()&&line.back()=='\r')line.pop_back();const auto equal=line.find('=');
        if(equal==std::string::npos)return Fail(L"Career import metadata is malformed.");
        const auto name=line.substr(0,equal),expected=line.substr(equal+1);
        if(!allow.count(name)||!seen.insert(name).second||expected.size()!=64)return Fail(L"Career import contains an unsupported path.");
        for(char c:expected)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return Fail(L"Career import hash is malformed.");
        auto file=Pin(base+L"\\"+Disk(Wide(name)));std::string actual,bytes;uint64_t length=0;
        if(!file||!Digest(file->value,actual,length)||actual!=expected||length>4*1024*1024)return Fail(L"Career import changed or is unreadable.");
        bytes.assign(size_t(length),0);DWORD read=0;
        if(length&&(!ReadFile(file->value,bytes.data(),DWORD(length),&read,nullptr)||read!=length))return Fail(L"Cannot read the optional career copy.");
        const auto leaf=name.substr(name.find_last_of('/')+1);
        if(leaf.find('.')==std::string::npos&&coop_save::ValidateNativeProfileFile(reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size())!=coop_save::SaveValidation::Valid)
            return Fail(L"The optional native career copy failed its checksum.");
        files.push_back({name,std::move(bytes)});if(end==std::string::npos)break;start=end+1;
    }
    if(!seen.count("Host/Player/Player")||!seen.count("Client/Player_P2/Player_P2"))return Fail(L"Both preview career roles are required.");
    const auto separator=destination.find_last_of(L'\\');
    if(separator==std::wstring::npos||separator<3)return Fail(L"Invalid private save parent.");
    auto parent=Pin(destination.substr(0,separator),true);if(!parent)return Fail(L"The private save parent is linked or unreadable.");
    if(GetFileAttributesW(destination.c_str())==INVALID_FILE_ATTRIBUTES&&!CreateDirectoryW(destination.c_str(),nullptr))return Fail(L"Cannot create the isolated career domain.");
    auto owner=Pin(destination,true);if(!owner)return Fail(L"The isolated save domain is linked or unreadable.");
    for(const std::string role:{"Host","Client"}){
        const auto target=destination+L"\\"+Wide(role);const DWORD current=GetFileAttributesW(target.c_str());
        if(current!=INVALID_FILE_ATTRIBUTES){if(!RealPath(target,true))return Fail(L"Existing private role directory is invalid.");continue;}
        if(GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND)return Fail(L"Cannot inspect the private role directory.");
        const auto staging=destination+L"\\"+Wide(role)+L".import."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
        if(!CreateDirectoryW(staging.c_str(),nullptr))return Fail(L"Cannot stage a separate career copy.");
        Handle stage(CreateFileW(staging.c_str(),FILE_LIST_DIRECTORY|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
            OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(stage.value==INVALID_HANDLE_VALUE||!RealPath(staging,true))return Fail(L"Cannot lock the private import staging folder.");
        for(const auto& file:files){
            if(file.relative.rfind(role+"/",0)!=0)continue;
            const auto relative=file.relative.substr(role.size()+1);const auto slash=relative.find('/');
            const auto directory=staging+L"\\"+Wide(relative.substr(0,slash));
            if(GetFileAttributesW(directory.c_str())==INVALID_FILE_ATTRIBUTES&&!CreateDirectoryW(directory.c_str(),nullptr))return Fail(L"Cannot stage the profile folder.");
            if(!RealPath(directory,true))return Fail(L"A staging profile folder changed.");
            auto pinned=Pin(directory,true);if(!pinned)return Fail(L"Cannot lock the staging profile folder.");
            Handle out(CreateFileW((staging+L"\\"+Disk(Wide(relative))).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            DWORD written=0;if(out.value==INVALID_HANDLE_VALUE||!WriteFile(out.value,file.bytes.data(),DWORD(file.bytes.size()),&written,nullptr)||written!=file.bytes.size()||!FlushFileBuffers(out.value))
                return Fail(L"Cannot write the separate preview profile.");
        }
        std::vector<uint8_t> storage(sizeof(FILE_RENAME_INFO)+target.size()*sizeof(wchar_t));
        auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());rename->ReplaceIfExists=FALSE;
        rename->RootDirectory=nullptr;rename->FileNameLength=DWORD(target.size()*sizeof(wchar_t));
        std::memcpy(rename->FileName,target.data(),rename->FileNameLength);
        if(!SetFileInformationByHandle(stage.value,FileRenameInfo,rename,DWORD(storage.size())))return Fail(L"Cannot publish the separate career copy; existing data retained.");
    }return true;
}
inline bool ImportSeed(const std::wstring& folder){
    if(folder.empty()||folder.size()>100||folder.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ")!=std::wstring::npos)return Fail(L"Invalid pack save folder.");
    wchar_t local[MAX_PATH]{};if(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,0,local)!=S_OK)return Fail(L"Cannot locate the private save directory.");
    return ImportSeedAt(std::wstring(local)+L"\\"+folder);
}
