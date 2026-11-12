// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Pre-game instances are complete private snapshots. No stock asset, career,
// native loader or installer is touched by these operations.
struct ModpackInfo{std::wstring id,name,revision;size_t packages=0,enabled=0;};
struct ModpackLoaded{ModpackInfo info;std::wstring root;std::vector<coop_packages::Manifest> catalog;std::vector<std::string> selected;std::map<std::string,coop_import::Receipt> imported;};
inline std::wstring ModpackHome(){return state.root+L"\\mods\\_undercoop\\modpacks";}
inline bool ModpackToken(const std::wstring& value){
    if(value.empty()||value.size()>64)return false;
    for(const auto c:value)if(!(c>=L'a'&&c<=L'z'||c>=L'0'&&c<=L'9'||c==L'-'))return false;return true;
}
inline std::wstring ModpackTokenNew(const wchar_t* prefix){
    static LONG serial=0;return std::wstring(prefix)+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(InterlockedIncrement(&serial));
}
inline bool ModpackName(const std::wstring& input,std::wstring& output){
    output=input;const auto begin=output.find_first_not_of(L" \t"),end=output.find_last_not_of(L" \t");
    if(begin==std::wstring::npos)return false;output=output.substr(begin,end-begin+1);
    const auto bytes=coop_mod_scan::Utf8(output);return output.size()<=64&&bytes.size()<=256&&coop_packages::detail::Utf8(bytes,true);
}
inline bool ModpackDirectory(const std::wstring& path){
    if(path.size()>=MAX_PATH)return Fail(L"This instance path is too long. Move the test game to a shorter folder.");
    if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES&&!CreateDirectoryW(path.c_str(),nullptr))return Fail(L"Cannot create the instance folder.");
    return RealPath(path,true)||Fail(L"The instance folder is linked or unreadable.");
}
inline bool ModpackWorkspace(){
    if(state.ready||state.fatal)return Fail(L"Modpacks can only be changed before the game starts.");
    auto mods=Pin(state.root+L"\\mods",true),owner=Pin(state.root+L"\\mods\\_undercoop",true);
    if(!mods||!owner)return Fail(L"The private mods workspace is unavailable.");
    return ModpackDirectory(ModpackHome());
}
inline bool ModpackWriteNew(const std::wstring& path,const std::string& text){
    if(path.size()>=MAX_PATH||text.size()>256*1024)return false;auto parent=Pin(path.substr(0,path.find_last_of(L'\\')),true);if(!parent)return false;
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));DWORD wrote=0;
    return file.value!=INVALID_HANDLE_VALUE&&(text.empty()||WriteFile(file.value,text.data(),DWORD(text.size()),&wrote,nullptr)&&wrote==text.size())&&FlushFileBuffers(file.value);
}
inline bool ModpackAtomicText(const std::wstring& path,const std::string& text){
    const auto parent=path.substr(0,path.find_last_of(L'\\'));auto folder=Pin(parent,true);if(!folder)return false;
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES&&!RealPath(path))return false;
    const auto pending=parent+L"\\"+ModpackTokenNew(L"pending-")+L".txt";
    if(!ModpackWriteNew(pending,text))return false;
    if(MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;
    if(RealPath(pending))DeleteFileW(pending.c_str());return false;
}
inline std::string ModpackSelectionText(const std::vector<std::string>& selected){std::string text;for(const auto& id:selected)text+=id+"\n";return text;}
inline bool ModpackSelection(const std::string& text,std::vector<std::string>& selected,bool legacyCustom=false){
    selected.clear();if(text.size()>16384)return false;std::set<std::string> unique;size_t at=0;
    while(at<text.size()){
        const auto end=text.find('\n',at);
        auto id=text.substr(at,end==std::string::npos?end:end-at);
        if(legacyCustom&&!id.empty()&&id.back()=='\r')id.pop_back();
        if(!legacyCustom||!id.empty()){
            if(!coop_packages::detail::Id(id,{})||!unique.insert(id).second||unique.size()>256)return false;
            selected.push_back(std::move(id));
        }
        if(end==std::string::npos)break;
        at=end+1;
    }
    return true;
}
inline bool ModpackReadCatalog(const std::wstring& root,ModpackLoaded& loaded,bool library=false){
    loaded.catalog.clear();loaded.imported.clear();auto folder=Pin(root,true);if(!folder)return Fail(L"This modpack's package folder is linked or missing.");
    WIN32_FIND_DATAW row{};HANDLE find=FindFirstFileW((root+L"\\*").c_str(),&row);if(find==INVALID_HANDLE_VALUE)return Fail(L"Cannot list this modpack's packages.");bool okay=true;
    do{const std::wstring name=row.cFileName;if(name==L"."||name==L".."||library&&name==L"_undercoop")continue;
        if(library&&!(row.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(row.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))continue;
        if(!(row.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(row.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){okay=false;break;}
        coop_packages::Manifest p;coop_packages::Error why;std::string text;
        const auto manifest=root+L"\\"+name+L"\\undercoop.mod.ini";
        if(library&&GetFileAttributesW(manifest.c_str())==INVALID_FILE_ATTRIBUTES){
            const auto missing=GetLastError();if(missing==ERROR_FILE_NOT_FOUND||missing==ERROR_PATH_NOT_FOUND)continue;
        }
        if(!ReadText(manifest,text)||!coop_packages::ParseManifest(text,p,why)||Wide(p.id)!=name||loaded.catalog.size()>=256){okay=false;break;}
        const auto receiptPath=root+L"\\"+name+L"\\undercoop.native.txt";const DWORD attrs=GetFileAttributesW(receiptPath.c_str());
        if(attrs!=INVALID_FILE_ATTRIBUTES){coop_import::Receipt receipt;if(!ReadText(receiptPath,text,1024*1024)||!coop_import::Decode(text,receipt)||receipt.id!=p.id||receipt.version!=p.version||p.kind!=coop_packages::Kind::Dependency){okay=false;break;}loaded.imported[p.id]=std::move(receipt);}
        else if(GetLastError()!=ERROR_FILE_NOT_FOUND){okay=false;break;}
        loaded.catalog.push_back(std::move(p));
    }while(FindNextFileW(find,&row));okay=okay&&GetLastError()==ERROR_NO_MORE_FILES;FindClose(find);
    if(!okay)return Fail(L"This modpack contains an invalid, linked or changed package.");
    std::sort(loaded.catalog.begin(),loaded.catalog.end(),[](const auto& a,const auto& b){return a.id<b.id;});return true;
}
inline bool ReadModpack(const std::wstring& id,ModpackLoaded& loaded){
    loaded={};if(!ModpackToken(id))return Fail(L"Invalid modpack identity.");const auto directory=ModpackHome()+L"\\"+id;std::string current,name,selected;
    if(!ReadText(directory+L"\\current.txt",current,128))return Fail(L"Cannot read this modpack's active revision.");
    const auto revision=Wide(current);if(!ModpackToken(revision))return Fail(L"Invalid modpack revision.");const auto root=directory+L"\\"+revision;
    if(!ReadText(root+L"\\name.txt",name,256)||!ModpackName(Wide(name),loaded.info.name)||!ReadText(root+L"\\selection.txt",selected,16384)||!ModpackSelection(selected,loaded.selected))return Fail(L"The modpack name or selection is invalid.");
    loaded.info.id=id;loaded.info.revision=revision;loaded.root=root+L"\\mods";if(!ModpackReadCatalog(loaded.root,loaded))return false;
    for(const auto& selectedId:loaded.selected)if(std::none_of(loaded.catalog.begin(),loaded.catalog.end(),[&](const auto& p){return p.id==selectedId;}))return Fail(L"This modpack selects a package it does not contain.");
    loaded.info.packages=loaded.catalog.size();loaded.info.enabled=loaded.selected.size();return true;
}
inline bool ListModpacks(std::vector<ModpackInfo>& packs){
    packs.clear();if(!ModpackWorkspace())return false;auto home=Pin(ModpackHome(),true);if(!home)return false;
    WIN32_FIND_DATAW row{};HANDLE find=FindFirstFileW((ModpackHome()+L"\\*").c_str(),&row);if(find==INVALID_HANDLE_VALUE)return false;bool okay=true;const auto previous=state.error;
    do{const std::wstring id=row.cFileName;if(id==L"."||id==L".."||id.rfind(L"pending-",0)==0)continue;
        if(!(row.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(row.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!ModpackToken(id)){okay=false;break;}
        ModpackLoaded loaded;if(!ReadModpack(id,loaded)){okay=false;break;}packs.push_back(std::move(loaded.info));if(packs.size()>128){okay=false;break;}
    }while(FindNextFileW(find,&row));okay=okay&&GetLastError()==ERROR_NO_MORE_FILES;FindClose(find);
    if(!okay){packs.clear();if(state.error.empty())return Fail(L"Cannot list modpacks safely.");return false;}
    state.error=previous;std::sort(packs.begin(),packs.end(),[](const auto& a,const auto& b){return a.name<b.name;});return true;
}
inline void ApplyModpack(ModpackLoaded&& loaded){
    state.packageRoot=std::move(loaded.root);state.instanceId=std::move(loaded.info.id);state.instanceName=std::move(loaded.info.name);
    state.catalog=std::move(loaded.catalog);state.selected=std::move(loaded.selected);state.imported=std::move(loaded.imported);state.nativeTrusted=false;state.error.clear();
}
inline bool LoadActiveModpack(){
    const auto path=state.root+L"\\mods\\_undercoop\\state\\active-modpack.txt";if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)return GetLastError()==ERROR_FILE_NOT_FOUND;
    std::string id;if(!ReadText(path,id,128))return Fail(L"The active modpack marker is unreadable or linked.");if(id.empty())return true;
    ModpackLoaded loaded;if(!ReadModpack(Wide(id),loaded))return false;ApplyModpack(std::move(loaded));return true;
}
inline bool ReadCustomModSetup(ModpackLoaded& library,bool validatePlan=false){
    if(!ModpackReadCatalog(state.root+L"\\mods",library,true))return false;
    library.selected.clear();
    std::string selection;
    const auto path=state.root+L"\\mods\\_undercoop\\state\\selection.txt";
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES){
        if(!ReadText(path,selection,16384)||!ModpackSelection(selection,library.selected,true))return Fail(L"Cannot read the custom setup's selection.");
    }else{
        const auto missing=GetLastError();
        if(missing!=ERROR_FILE_NOT_FOUND&&missing!=ERROR_PATH_NOT_FOUND)return Fail(L"Cannot inspect the custom selection.");
        for(const auto& p:library.catalog){
            if(!library.imported.count(p.id)&&(p.kind==coop_packages::Kind::Assets||p.kind==coop_packages::Kind::Data||StandaloneNative(p.id)))
                library.selected.push_back(p.id);
        }
    }
    if(validatePlan){coop_packages::Plan plan;coop_packages::Error error;if(!coop_packages::BuildPlan(library.catalog,library.selected,plan,error))return Fail(L"The custom library also needs attention: "+Wide(error.detail));}return true;
}
inline bool ModpackRecoveryAvailable(){
    if(state.ready||state.fatal||!state.instanceId.empty())return false;const auto previous=state.error;std::string id;
    if(!ReadText(state.root+L"\\mods\\_undercoop\\state\\active-modpack.txt",id,128)||id.empty())return false;
    ModpackLoaded instance,library;const bool available=!ReadModpack(Wide(id),instance)&&ReadCustomModSetup(library,true);state.error=previous;return available;
}
inline std::vector<std::string> MissingModpackRequirements(){
    std::vector<std::string> missing;if(state.instanceId.empty())return missing;std::vector<std::string> pending=state.selected;std::set<std::string> visited,unique;
    for(size_t i=0;i<pending.size()&&pending.size()<=512;++i){const auto id=pending[i];if(!visited.insert(id).second)continue;const auto p=std::find_if(state.catalog.begin(),state.catalog.end(),[&](const auto& item){return item.id==id;});if(p==state.catalog.end())continue;
        for(const auto& required:p->requires){const auto found=std::find_if(state.catalog.begin(),state.catalog.end(),[&](const auto& item){return item.id==required.id&&item.version==required.version;});if(found!=state.catalog.end())pending.push_back(found->id);else if(unique.insert(required.id).second)missing.push_back(required.id+" "+required.version);}}
    return missing;
}
inline bool SaveActiveModpackSelection(bool validatePlan=true);
inline bool PersistCurrentModSelection(){
    if(!state.instanceId.empty())return SaveActiveModpackSelection();
    return ModpackAtomicText(state.root+L"\\mods\\_undercoop\\state\\selection.txt",ModpackSelectionText(state.selected));
}
inline bool ActivateModpack(const std::wstring& id){
    if(state.ready||state.fatal)return Fail(L"Restart the game before switching modpacks.");if(state.instanceId==id)return true;ModpackLoaded loaded;if(!ReadModpack(id,loaded))return false;
    if(!PersistCurrentModSelection())return Fail(L"Cannot preserve the current selection. Your active setup was not changed.");
    if(!ModpackAtomicText(state.root+L"\\mods\\_undercoop\\state\\active-modpack.txt",coop_mod_scan::Utf8(id)))return Fail(L"Cannot save the active modpack. Your previous setup was not changed.");
    ApplyModpack(std::move(loaded));return true;
}
inline bool UseModLibrary(){
    if(state.ready||state.fatal)return Fail(L"Restart before switching modpacks.");
    if(state.instanceId.empty()){
        // Discovery can finish the custom library before a deleted/corrupt
        // active instance fails to load. Explicitly choosing Custom must clear
        // that stale pointer, not re-save or reset the library selection.
        const auto marker=state.root+L"\\mods\\_undercoop\\state\\active-modpack.txt";const DWORD attrs=GetFileAttributesW(marker.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES)return GetLastError()==ERROR_FILE_NOT_FOUND||Fail(L"Cannot inspect the active modpack choice.");
        std::string previous;if(!ReadText(marker,previous,128))return Fail(L"The active modpack marker is linked or unreadable. It was not overwritten.");
        if(previous.empty()){state.error.clear();return true;}ModpackLoaded library;if(!ReadCustomModSetup(library,true))return false;
        if(!ModpackAtomicText(marker,""))return Fail(L"Cannot clear the unavailable modpack choice. No selection was changed.");
        library.root.clear();ApplyModpack(std::move(library));return true;
    }
    ModpackLoaded library;
    if(!ReadCustomModSetup(library))return false;
    // Preserve the named selection even when a dependency needs attention.
    // Play still requires a valid build plan. Switching setups does not.
    if(!SaveActiveModpackSelection(false))return Fail(L"Cannot preserve the modpack's selection. It is still active.");
    if(!ModpackAtomicText(state.root+L"\\mods\\_undercoop\\state\\active-modpack.txt",""))return Fail(L"Cannot save the custom setup choice.");
    library.root.clear();ApplyModpack(std::move(library));return true;
}
inline bool SaveActiveModpackSelection(bool validatePlan){
    if(state.instanceId.empty())return false;
    const auto selection=ModpackSelectionText(state.selected);
    if(validatePlan){
        coop_packages::Plan plan;coop_packages::Error why;
        if(!coop_packages::BuildPlan(state.catalog,state.selected,plan,why))return false;
    }else{
        std::vector<std::string> selected;
        if(!ModpackSelection(selection,selected))return false;
        for(const auto& id:selected){
            if(std::none_of(state.catalog.begin(),state.catalog.end(),[&](const auto& p){return p.id==id;}))return false;
        }
    }
    const auto directory=state.packageRoot.substr(0,state.packageRoot.find_last_of(L'\\'));
    return ModpackAtomicText(directory+L"\\selection.txt",selection);
}
struct ModpackCopyBudget{uint64_t bytes=0;size_t files=0;const std::atomic<bool>* cancel=nullptr;};
inline bool CopyModpackTree(const std::wstring& source,const std::wstring& target,ModpackCopyBudget& budget,unsigned depth=0){
    if(depth>12||budget.cancel&&budget.cancel->load())return Fail(L"Modpack creation cancelled safely.");
    auto from=Pin(source,true),to=Pin(target,true);if(!from||!to)return Fail(L"An instance source or target folder is linked or unavailable.");
    WIN32_FIND_DATAW row{};HANDLE find=FindFirstFileW((source+L"\\*").c_str(),&row);if(find==INVALID_HANDLE_VALUE)return false;bool okay=true;
    do{const std::wstring name=row.cFileName;if(name==L"."||name==L"..")continue;
        if(budget.cancel&&budget.cancel->load()){okay=false;break;}if(row.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){okay=false;break;}
        const auto src=source+L"\\"+name,dst=target+L"\\"+name;if(dst.size()>=MAX_PATH){okay=false;break;}
        if(row.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){if(!CreateDirectoryW(dst.c_str(),nullptr)||!CopyModpackTree(src,dst,budget,depth+1)){okay=false;break;}}
        else{if(++budget.files>32768){okay=false;break;}auto input=Pin(src);if(!input){okay=false;break;}std::string sourceHash;uint64_t bytes=0;
            if(!Digest(input->value,sourceHash,bytes)||bytes>256ull*1024*1024||budget.bytes+bytes>2ull*1024*1024*1024){okay=false;break;}budget.bytes+=bytes;
            Handle output(CreateFileW(dst.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));if(output.value==INVALID_HANDLE_VALUE){okay=false;break;}
            std::array<char,65536> block{};for(;;){DWORD got=0,written=0;if(budget.cancel&&budget.cancel->load()||!ReadFile(input->value,block.data(),DWORD(block.size()),&got,nullptr)){okay=false;break;}if(!got)break;
                if(!WriteFile(output.value,block.data(),got,&written,nullptr)||written!=got){okay=false;break;}}
            okay=okay&&FlushFileBuffers(output.value);CloseHandle(output.value);output.value=INVALID_HANDLE_VALUE;if(!okay)break;
            auto verified=Pin(dst);std::string hash;uint64_t length=0;if(!verified||!Digest(verified->value,hash,length)||hash!=sourceHash||length!=bytes){okay=false;break;}
        }
    }while(FindNextFileW(find,&row));okay=okay&&GetLastError()==ERROR_NO_MORE_FILES;FindClose(find);
    return okay||Fail(budget.cancel&&budget.cancel->load()?L"Modpack creation cancelled safely.":L"Cannot snapshot the packages safely (linked, busy, excessive or long paths). The existing setup was not changed.");
}
inline void ModpackRemovePending(const std::wstring& path,const std::wstring& boundary){
    if(!Below(path,boundary)||!RealPath(path,true))return;WIN32_FIND_DATAW row{};HANDLE find=FindFirstFileW((path+L"\\*").c_str(),&row);
    if(find!=INVALID_HANDLE_VALUE){do{const std::wstring name=row.cFileName;if(name==L"."||name==L"..")continue;const auto child=path+L"\\"+name;
        if(row.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)continue;if(row.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)ModpackRemovePending(child,boundary);else if(RealPath(child))DeleteFileW(child.c_str());
    }while(FindNextFileW(find,&row));FindClose(find);}RemoveDirectoryW(path.c_str());
}
inline bool WriteModpackRevision(const std::wstring& name,const std::wstring& target,const std::vector<coop_packages::Manifest>& packages,
    const std::vector<std::string>& selected,const std::map<std::string,std::wstring>& sources,const std::atomic<bool>* cancel=nullptr){
    if(!CreateDirectoryW(target.c_str(),nullptr))return Fail(L"Cannot stage this modpack revision.");
    const auto packageRoot=target+L"\\mods";if(!CreateDirectoryW(packageRoot.c_str(),nullptr))return false;ModpackCopyBudget budget;budget.cancel=cancel;
    for(const auto& p:packages){const auto found=sources.find(p.id);if(found==sources.end()||!coop_packages::detail::Id(p.id,{}))return false;
        const auto to=packageRoot+L"\\"+Wide(p.id);if(!CreateDirectoryW(to.c_str(),nullptr)||!CopyModpackTree(found->second,to,budget))return false;
        coop_packages::Manifest copied;coop_packages::Error why;std::string text;
        if(!ReadText(to+L"\\undercoop.mod.ini",text)||!coop_packages::ParseManifest(text,copied,why)||copied.id!=p.id||copied.name!=p.name||copied.version!=p.version||copied.kind!=p.kind||copied.files.size()!=p.files.size()||copied.requires.size()!=p.requires.size())return Fail(L"A package manifest changed while creating the modpack. Refresh My mods and retry.");
        for(size_t i=0;i<p.files.size();++i)if(copied.files[i].destination!=p.files[i].destination||copied.files[i].source!=p.files[i].source)return Fail(L"A package's resources changed while creating the modpack.");
        for(size_t i=0;i<p.requires.size();++i)if(copied.requires[i].id!=p.requires[i].id||copied.requires[i].version!=p.requires[i].version)return Fail(L"A package's requirements changed while creating the modpack.");}
    if(cancel&&cancel->load())return Fail(L"Modpack creation cancelled safely.");
    return ModpackWriteNew(target+L"\\name.txt",coop_mod_scan::Utf8(name))&&ModpackWriteNew(target+L"\\selection.txt",ModpackSelectionText(selected));
}
inline bool CreateModpack(const std::wstring& rawName,std::wstring& created,const std::atomic<bool>* cancel=nullptr){
    created.clear();std::wstring name;if(!ModpackName(rawName,name))return Fail(L"Enter a name containing 1 to 64 printable characters.");if(!ModpackWorkspace())return false;
    std::vector<ModpackInfo> previous;if(!ListModpacks(previous))return false;if(previous.size()>=128)return Fail(L"The limit is 128 modpacks.");
    for(const auto& p:previous)if(Equal(name,p.name))return Fail(L"A modpack with that name already exists.");
    auto home=Pin(ModpackHome(),true);if(!home)return false;const auto id=ModpackTokenNew(L"pack-"),revision=ModpackTokenNew(L"v-");
    const auto pending=ModpackHome()+L"\\"+ModpackTokenNew(L"pending-"),target=ModpackHome()+L"\\"+id;if(!CreateDirectoryW(pending.c_str(),nullptr))return false;
    bool okay=false;std::map<std::string,std::wstring> sources;for(const auto& p:state.catalog)sources[p.id]=PackageDirectory(p.id);
    if(WriteModpackRevision(name,pending+L"\\"+revision,state.catalog,state.selected,sources,cancel)&&ModpackWriteNew(pending+L"\\current.txt",coop_mod_scan::Utf8(revision))&&
        !(cancel&&cancel->load())&&SnapshotModpackCareers(id,pending,cancel)&&!(cancel&&cancel->load()))
        okay=MoveFileExW(pending.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!okay){ModpackRemovePending(pending,ModpackHome());if(state.error.empty())Fail(L"Cannot publish this modpack. Your current setup was not changed.");return false;}
    created=id;state.error.clear();return true;
}
inline bool EditModpack(const std::wstring& id,const std::vector<std::string>& members,const std::atomic<bool>* cancel=nullptr){
    if(state.ready||state.fatal)return Fail(L"Restart before editing modpacks.");ModpackLoaded current,library;if(!ReadModpack(id,current)||!ModpackReadCatalog(state.root+L"\\mods",library,true))return false;
    if(members.size()>256)return Fail(L"Too many packages in this modpack.");std::set<std::string> unique;std::vector<coop_packages::Manifest> packages;std::vector<std::string> selected;std::map<std::string,std::wstring> sources;
    for(const auto& member:members){if(!unique.insert(member).second)return Fail(L"Duplicate package in the modpack edit.");
        auto found=std::find_if(current.catalog.begin(),current.catalog.end(),[&](const auto& p){return p.id==member;});
        if(found!=current.catalog.end()){packages.push_back(*found);sources[member]=current.root+L"\\"+Wide(member);if(std::find(current.selected.begin(),current.selected.end(),member)!=current.selected.end())selected.push_back(member);}
        else{found=std::find_if(library.catalog.begin(),library.catalog.end(),[&](const auto& p){return p.id==member;});if(found==library.catalog.end())return Fail(L"A package selected for this modpack is no longer installed.");
            packages.push_back(*found);sources[member]=state.root+L"\\mods\\"+Wide(member);if(found->kind==coop_packages::Kind::Assets||found->kind==coop_packages::Kind::Data||StandaloneNative(member)||library.imported.count(member))selected.push_back(member);}
    }
    const auto directory=ModpackHome()+L"\\"+id,revision=ModpackTokenNew(L"v-"),target=directory+L"\\"+revision;auto pin=Pin(directory,true);if(!pin)return false;
    if(!WriteModpackRevision(current.info.name,target,packages,selected,sources,cancel)||cancel&&cancel->load()){
        ModpackRemovePending(target,directory);if(state.error.empty())Fail(L"The modpack edit could not be staged. The old revision is unchanged.");return false;}
    ModpackLoaded refreshed;refreshed.info={id,current.info.name,revision,packages.size(),selected.size()};refreshed.root=target+L"\\mods";refreshed.selected=selected;
    if(!ModpackReadCatalog(refreshed.root,refreshed)){ModpackRemovePending(target,directory);return false;}
    if(!ModpackAtomicText(directory+L"\\current.txt",coop_mod_scan::Utf8(revision))){ModpackRemovePending(target,directory);return Fail(L"Cannot publish the edited modpack. The old revision is unchanged.");}
    if(state.instanceId==id)ApplyModpack(std::move(refreshed));
    state.error.clear();return true;
}
