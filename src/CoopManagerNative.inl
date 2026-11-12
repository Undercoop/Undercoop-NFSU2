// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Included in CoopModManager after the confined file/hash helpers. These are
// exact local components, not permission to run arbitrary packages/installers.
struct NativeRequest{std::wstring path,cwd;};
struct NativeAdapter{const char* id;const char* version;const wchar_t* file;const wchar_t* config;const wchar_t* data;const char* hash;};
inline const NativeAdapter* StandaloneNative(const std::string& id){
    static const NativeAdapter known[]{
        {"camera-toolkit","local-relocated-1",L"NFSU2CameraToolkit.asi",L"NFSU2CameraToolkit.ini",nullptr,
            "42a00fa912716a1f48b1a54a0fa4393fbee8d5caf4ea589fae026fc31e4f809a"},
        {"nfsu2-extra-options","local-2f59d8f8",L"NFSU2ExtraOptions.asi",L"NFSU2ExtraOptionsSettings.ini",nullptr,
            "2f59d8f82c69ab2b8102e4041dc04a8c7ea9440e597eb1222a41889cfbe547ae"},
        {"nfsu2-widescreen-fix","local-3f715968",L"NFSUnderground2.WidescreenFix.asi",L"NFSUnderground2.WidescreenFix.ini",L"NFSUnderground2.WidescreenFix.dat",
            "3f715968c08efcf2f9879fbae25d5e72388fd1518c3acba68e6ec74dcec8d94f"}
    };
    for(const auto& item:known)if(id==item.id)return &item;return nullptr;
}
inline bool ApprovedNative(const coop_packages::Manifest& p){
    if(p.kind!=coop_packages::Kind::Dependency)return false;
    if(p.id=="nfsu2-unlimiter")return p.version=="3.1.1.1337";
    const auto* adapter=StandaloneNative(p.id);return adapter&&p.version==adapter->version||ImportedNativeKnown(p);
}
inline bool Selectable(const coop_packages::Manifest& p){return p.kind==coop_packages::Kind::Assets||p.kind==coop_packages::Kind::Data||StandaloneNative(p.id)||ImportedNativeKnown(p);}
inline bool ManagedConfigChanged(const std::wstring& root,const std::string& path,coop_mod_scan::Change& change){
    if(!Equal(root,state.root))return false;
    for(const auto& watch:state.watches){
        if(path!=watch.relative&&watch.relative.compare(0,path.size()+1,path+"/")!=0)continue;
        coop_mod_scan::Sha256 sha;coop_mods::Entry current;uint64_t bytes=0;std::string error;DWORD readError=ERROR_SUCCESS;
        if(!sha.algorithm||!coop_mod_scan::HashFile({watch.physical,watch.expected.path,true},sha,current,bytes,error,&readError)||
           current.hash!=watch.expected.hash||current.size!=watch.expected.size){
            change.path=watch.relative;change.reason=error.empty()?"managed component shared settings changed":error;change.error=readError;return true;
        }
    }
    // Directory notifications reach this callback before mods-private output
    // filtering. Do not let a newly added native sidecar bypass the live pack
    // check. Ordinary log/damage output is ignored without rescanning the tree.
    for(const auto& id:state.plan.Packages()){const auto found=state.imported.find(id);if(found==state.imported.end())continue;
        auto prefix=coop_mods::Lower(coop_mod_scan::Utf8(PackageDirectory(id).substr(state.root.size()+1)));for(auto& c:prefix)if(c=='\\')c='/';
        if(path!=prefix&&path.rfind(prefix+"/",0)!=0)continue;
        const auto ext=coop_packages::detail::Extension(path);bool immutable=false;
        for(const auto& f:found->second.files)immutable=immutable||!f.config&&path==prefix+"/"+f.source;
        if(immutable||ext==".asi"||ext==".dll"||ext==".exe"||ext.empty()){
            bool okay=ImportedTreeSafe(PackageDirectory(id),L"",found->second);
            if(okay&&immutable){const auto relative=path.substr(prefix.size()+1);auto file=Pin(state.root+L"\\"+Disk(Wide(path)));std::string hash;uint64_t bytes=0;
                okay=file&&Digest(file->value,hash,bytes);for(const auto& f:found->second.files)if(f.source==relative)okay=okay&&hash==f.hash&&bytes==f.bytes;}
            if(!okay){change.path=path;change.reason="imported native payload or sidecar changed";return true;}
        }
    }return false;
}
inline bool PrepareStandalone(const coop_packages::Manifest& p,std::vector<std::unique_ptr<Handle>>& pins,
    std::vector<NativeRequest>& requests,std::vector<ConfigWatch>& watches,std::string& receipt){
    const auto* known=StandaloneNative(p.id);if(!known||!ApprovedNative(p))return Fail(L"This native component has no approved adapter.");
    const auto directory=PackageDirectory(p.id);
    if(GetModuleHandleW(known->file))return Fail(L"A duplicate native component was loaded before the manager: "+std::wstring(known->file));
    auto native=Pin(directory+L"\\"+known->file);std::string hash;uint64_t size=0;
    if(!native||!Digest(native->value,hash,size)||hash!=known->hash)return Fail(L"The component differs from its verified local version: "+Wide(p.id));
    receipt+="Native="+p.id+"="+hash+"\n";pins.push_back(std::move(native));
    // Simulation-relevant settings use the same normalization as the existing
    // lobby scanner. Different window sizes, input binds and camera UI do not
    // split careers or falsely reject a teammate. Mutable INIs are watched,
    // not pinned: the mods' own normal settings writes must remain possible.
    auto settings=Pin(directory+L"\\"+known->config);std::string raw;
    if(!settings||!ReadText(directory+L"\\"+known->config,raw,1024*1024))return Fail(L"Cannot read component settings: "+Wide(p.id));
    if(p.id!="camera-toolkit"){
        const auto virtualName="scripts/"+coop_mods::Lower(coop_mod_scan::Utf8(known->config));std::string normalized;
        if(!coop_mods::CanonicalIni(virtualName,raw,normalized))return Fail(L"Component settings are malformed: "+Wide(p.id));
        coop_mod_scan::Sha256 sha;coop_mods::Hash digest{};
        if(!sha.Bytes(normalized.data(),normalized.size(),digest))return Fail(L"Cannot fingerprint component settings.");
        receipt+="NativeSettings="+p.id+"="+coop_mod_scan::Hex(digest)+"\n";
        ConfigWatch watch;watch.physical=directory+L"\\"+known->config;
        watch.relative=coop_mods::Lower(coop_mod_scan::Utf8((directory+L"\\"+known->config).substr(state.root.size()+1)));for(auto& c:watch.relative)if(c=='\\')c='/';
        watch.expected={virtualName,uint64_t(normalized.size()),digest};watches.push_back(std::move(watch));
    } // Camera hotkeys/UI are personal and may be saved by the tool at runtime.
    if(known->data){auto data=Pin(directory+L"\\"+known->data);
        if(!data||!Digest(data->value,hash,size))return Fail(L"Cannot verify component data: "+Wide(p.id));
        receipt+="NativeData="+p.id+"="+hash+"\n";pins.push_back(std::move(data));}
    WIN32_FIND_DATAW row{};HANDLE find=FindFirstFileW((directory+L"\\*").c_str(),&row);
    if(find==INVALID_HANDLE_VALUE)return Fail(L"Cannot inspect native component files.");bool okay=true;size_t count=0;
    do{const std::wstring name=row.cFileName;if(name==L"."||name==L"..")continue;
        if(++count>16||(row.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){okay=false;break;}
        const bool folder=(row.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        const bool camera=p.id=="camera-toolkit";
        const bool allowed=folder?(camera&&Equal(name,L"Tracks")):
            (Equal(name,known->file)||Equal(name,known->config)||Equal(name,L"undercoop.mod.ini")||
            (known->data&&Equal(name,known->data))||(camera&&Equal(name,L"imgui.ini")));
        if(!allowed){okay=false;break;}
    }while(FindNextFileW(find,&row));
    okay=okay&&GetLastError()==ERROR_NO_MORE_FILES;FindClose(find);
    if(!okay)return Fail(L"Unexpected files beside a native component: "+Wide(p.id));
    // Camera's bounded path adapter uses explicit game-relative mods paths.
    // ExtraOptions still uses ../CARS, so keep its old scripts working folder.
    // Its existing relocated literals are mods/camera-toolkit/... . An
    // instance revision has its own mods tree, so use that revision as CWD.
    const auto cameraRoot=state.packageRoot.empty()?state.root:state.packageRoot.substr(0,state.packageRoot.find_last_of(L'\\'));
    requests.push_back({directory+L"\\"+known->file,p.id=="camera-toolkit"?cameraRoot:state.root+L"\\scripts"});return true;
}
struct NativeApi{
    decltype(&::LoadLibraryExW) load=&::LoadLibraryExW;
    decltype(&::GetProcAddress) procedure=&::GetProcAddress;
};
inline HMODULE LoadNative(const NativeRequest& request,const NativeApi& api={}){
    const DWORD needed=GetCurrentDirectoryW(0,nullptr);if(!needed||needed>32768)return nullptr;
    std::wstring previous(size_t(needed),0);if(!GetCurrentDirectoryW(needed,previous.data()))return nullptr;
    previous.resize(std::wcslen(previous.c_str()));
    struct Restore{const std::wstring& path;~Restore(){SetCurrentDirectoryW(path.c_str());}} restore{previous};
    if(!SetCurrentDirectoryW(request.cwd.c_str()))return nullptr;
    const HMODULE module=api.load(request.path.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);if(!module)return nullptr;
    if(!SetCurrentDirectoryW(request.cwd.c_str()))return nullptr;
    if(const auto initialize=reinterpret_cast<void(*)()>(api.procedure(module,"InitializeASI")))initialize();
    return module;
}
