// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// A manual source is a directory of game-ready files, not an installer. Read
// and hash it without executing anything; publish the same managed layout as
// archive imports so selection, native approval and modpacks stay unchanged.
namespace folder_import {
inline bool NormalizeRoot(std::wstring path,std::wstring& result,std::wstring& error){
    path=Disk(std::move(path));while(path.size()>3&&path.back()==L'\\')path.pop_back();
    wchar_t full[MAX_PATH]{};const DWORD size=GetFullPathNameW(path.c_str(),MAX_PATH,full,nullptr);
    if(!size||size>=MAX_PATH||!RealPath(full,true)){error=L"The selected folder is missing, linked or unsupported.";return false;}
    result=full;return true;
}
struct Scan {
    std::vector<ImportFolderFile> files;std::set<std::string> paths;
    std::vector<std::unique_ptr<Handle>> directories;
    const std::atomic<bool>* cancel=nullptr;uint64_t bytes=0;size_t entries=0;
    std::chrono::steady_clock::time_point began=std::chrono::steady_clock::now();
    bool Okay(std::wstring& error)const{
        if(cancel&&cancel->load()){error=L"Folder inspection cancelled.";return false;}
        if(std::chrono::steady_clock::now()-began>std::chrono::seconds(120)){error=L"Folder inspection exceeded its time budget.";return false;}
        return true;
    }
    bool Walk(const std::wstring& root,const std::wstring& relative,std::wstring& error,unsigned depth=0){
        if(depth>12||!Okay(error)){if(error.empty())error=L"The manual folder is nested too deeply.";return false;}
        const auto path=root+(relative.empty()?L"":L"\\"+relative);auto directory=Pin(path,true);
        if(!directory){error=L"A manual source folder changed, is linked or is unreadable.";return false;}
        directories.push_back(std::move(directory));
        WIN32_FIND_DATAW row{};
        HANDLE find=FindFirstFileW((path+L"\\*").c_str(),&row);
        if(find==INVALID_HANDLE_VALUE){error=L"Cannot enumerate the manual source folder.";return false;}
        bool okay=true;
        do{
            const std::wstring name=row.cFileName;if(name==L"."||name==L"..")continue;
            if(!Okay(error)){okay=false;break;}
            if(++entries>8192||(row.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){
                error=L"Linked files/folders and oversized source trees cannot be registered.";okay=false;break;}
            const auto child=relative.empty()?name:relative+L"\\"+name;
            std::string canonical,why;bool isDirectory=false;
            if(!coop_zip::detail::Path(coop_mod_scan::Utf8(child),canonical,isDirectory,why,coop_zip::Limits{})||
               !paths.insert(coop_mods::Lower(canonical)).second){error=L"Unsafe or ambiguous manual source path: "+child;okay=false;break;}
            if(row.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){
                if(!Walk(root,child,error,depth+1)){okay=false;break;}
                continue;
            }
            if(files.size()>=4096){error=L"A manual package is limited to 4096 source files.";okay=false;break;}
            auto file=Pin(root+L"\\"+child);LARGE_INTEGER length{};BY_HANDLE_FILE_INFORMATION identity{};
            if(!file||!GetFileSizeEx(file->value,&length)||length.QuadPart<0||uint64_t(length.QuadPart)>coop_zip::kMaxFile||
               !GetFileInformationByHandle(file->value,&identity)||GetFileType(file->value)!=FILE_TYPE_DISK){
                error=L"A manual source file is linked, busy, unreadable or larger than 128 MiB: "+child;okay=false;break;}
            if(uint64_t(length.QuadPart)>coop_zip::kMaxUnpacked-bytes){error=L"A manual package is limited to 512 MiB of source files.";okay=false;break;}
            ImportFolderFile item;item.relative=child;item.path=coop_mods::Lower(canonical);
            item.volume=identity.dwVolumeSerialNumber;item.fileId=(uint64_t(identity.nFileIndexHigh)<<32)|identity.nFileIndexLow;
            if(!Digest(file->value,item.hash,item.bytes)||item.bytes!=uint64_t(length.QuadPart)){
                error=L"Cannot fingerprint the manual source file: "+child;okay=false;break;}
            bytes+=item.bytes;files.push_back(std::move(item));
        }while(FindNextFileW(find,&row));
        if(okay&&GetLastError()!=ERROR_NO_MORE_FILES){error=L"Manual source enumeration failed.";okay=false;}
        FindClose(find);return okay;
    }
};
inline std::string Fingerprint(const std::vector<ImportFolderFile>& files){
    std::string text="Undercoop manual source1\n";
    for(const auto& file:files)text+=file.path+"|"+file.hash+"|"+std::to_string(file.bytes)+"\n";
    return ImportHash(text);
}
inline bool Snapshot(const std::wstring& root,std::vector<ImportFolderFile>& files,std::string& hash,
                     std::wstring& error,const std::atomic<bool>* cancel){
    Scan scan;scan.cancel=cancel;if(!scan.Walk(root,L"",error))return false;
    if(scan.files.empty()){error=L"The manual source folder contains no files.";return false;}
    std::sort(scan.files.begin(),scan.files.end(),[](const auto& a,const auto& b){return a.path<b.path;});
    files=std::move(scan.files);hash=Fingerprint(files);return !hash.empty();
}
inline bool Read(const std::wstring& root,const ImportFolderFile& expected,std::string& content,std::wstring& error){
    auto file=Pin(root+L"\\"+expected.relative);BY_HANDLE_FILE_INFORMATION identity{};LARGE_INTEGER length{};
    if(!file||!GetFileInformationByHandle(file->value,&identity)||!GetFileSizeEx(file->value,&length)||
       expected.volume!=identity.dwVolumeSerialNumber||expected.fileId!=((uint64_t(identity.nFileIndexHigh)<<32)|identity.nFileIndexLow)||
       length.QuadPart<0||uint64_t(length.QuadPart)!=expected.bytes||expected.bytes>coop_zip::kMaxFile){
        error=L"A manual source file changed after inspection: "+expected.relative;return false;}
    content.assign(size_t(expected.bytes),0);DWORD got=0;
    if((!content.empty()&&(!ReadFile(file->value,content.data(),DWORD(content.size()),&got,nullptr)||got!=content.size()))||
       ImportHash(content)!=expected.hash){error=L"A manual source file changed after inspection: "+expected.relative;return false;}
    return true;
}
inline bool NativeFile(const ImportItem& item,const std::string& content,const std::set<std::string>& bundled,
                       const std::wstring& gameRoot,std::wstring& error){
    const auto ext=coop_packages::detail::Extension(item.destination);
    if(ext!=".asi"&&ext!=".dll")return true;
    std::vector<std::string> imports;if(!coop_import::PE(content,imports)){error=L"Native plugin is not a valid 32-bit DLL: "+Wide(item.destination);return false;}
    const auto parent=item.destination.substr(0,item.destination.find_last_of('/')+1);
    for(const auto& dll:imports)if(!bundled.count(parent+dll)){
        for(const auto& path:bundled)if(path.substr(path.find_last_of('/')+1)==dll){error=L"Native dependencies must be beside their importing plugin: "+Wide(dll);return false;}
        wchar_t system[MAX_PATH]{};if(!GetSystemDirectoryW(system,MAX_PATH)){error=L"Cannot inspect Windows dependencies.";return false;}
        const bool api=dll.rfind("api-ms-win-",0)==0||dll.rfind("ext-ms-win-",0)==0;
        const bool inGame=!gameRoot.empty()&&RealPath(gameRoot+L"\\"+Wide(dll));
        if(!api&&!inGame&&!GetModuleHandleW(Wide(dll).c_str())&&GetFileAttributesW((std::wstring(system)+L"\\"+Wide(dll)).c_str())==INVALID_FILE_ATTRIBUTES){
            error=L"Missing Windows/native library: "+Wide(dll)+L". No installer was executed.";return false;}
    }
    return true;
}
inline bool RuntimeRoot(const std::string& name){
    for(const auto* root:{"cars","global","frontend","languages","tracks","movies","sound","scripts"})if(name==root)return true;
    return false;
}
// Resolve explicit roots for code as well as assets. The archive resolver only
// accepts data extensions, so passing an ASI through it would duplicate scripts.
inline bool DeclaredDestination(const std::string& name,std::string& destination,std::string& prefix,std::wstring& error){
    std::set<std::pair<std::string,std::string>> candidates;
    for(size_t at=0;at<name.size();){
        const auto slash=name.find('/',at);if(slash==std::string::npos)break;
        if(RuntimeRoot(name.substr(at,slash-at)))candidates.insert({name.substr(at),name.substr(0,at)});
        at=slash+1;
    }
    if(candidates.size()>1){error=L"A manual file has ambiguous game roots: "+Wide(name);return false;}
    if(candidates.empty())return false;
    destination=candidates.begin()->first;prefix=candidates.begin()->second;return true;
}
}

inline bool ImportInspectFolder(const std::wstring& input,ImportPlan& plan,std::wstring& error,
                               const std::atomic<bool>* cancel=nullptr,const std::wstring& gameRoot={}){
    plan={};error.clear();plan.folder=true;
    if(!folder_import::NormalizeRoot(input,plan.archive,error))return false;
    const auto system=gameRoot+L"\\mods\\_undercoop";
    if(!gameRoot.empty()&&(Equal(plan.archive,system)||Below(plan.archive,system))){error=L"Undercoop's internal workspace is not a manual mod source.";return false;}
    if(!folder_import::Snapshot(plan.archive,plan.folderFiles,plan.hash,error,cancel))return false;
    const auto basename=plan.archive.substr(plan.archive.find_last_of(L'\\')+1);
    std::vector<size_t> manifests,modules;std::map<std::string,size_t> names;
    for(size_t i=0;i<plan.folderFiles.size();++i){const auto& f=plan.folderFiles[i];names[f.path]=i;
        const auto leaf=f.path.substr(f.path.find_last_of('/')+1);
        if(leaf=="undercoop.mod.ini")manifests.push_back(i);
        if(coop_packages::detail::Extension(f.path)==".asi")modules.push_back(i);
        if(const auto unsupported=ImportUnsupportedInstaller(f.path);!unsupported.empty()){
            error=L"Manual folders must contain game-ready files. Installer inputs are not executed or silently skipped: "+Wide(f.path);return false;}
    }
    if(manifests.size()>1){error=L"This folder contains multiple packages. Register one package at a time.";return false;}
    const bool prepared=!manifests.empty();std::string prefix;
    if(prepared){
        std::string text;const auto& f=plan.folderFiles[manifests.front()];
        if(!folder_import::Read(plan.archive,f,text,error))return false;
        coop_packages::Error why;if(!coop_packages::ParseManifest(text,plan.package,why)){error=L"Invalid package manifest: "+Wide(why.detail);return false;}
        const auto slash=f.path.find_last_of('/');prefix=slash==std::string::npos?"":f.path.substr(0,slash+1);
        if(plan.package.kind!=coop_packages::Kind::Assets||!modules.empty()){
            error=L"Choose the original game-ready plugin folder, not a prepared native/data installation recipe.";return false;}
    }else{
        plan.code=!modules.empty();plan.package.kind=plan.code?coop_packages::Kind::Dependency:coop_packages::Kind::Assets;
        plan.package.name=coop_mod_scan::Utf8(basename);
        auto id=coop_mods::Lower(plan.package.name);
        if(plan.code){const auto& module=plan.folderFiles[modules.front()].path;const auto slash=module.find_last_of('/');
            id=module.substr(slash==std::string::npos?0:slash+1);id.resize(id.size()-4);
            const auto scripts=module.rfind("scripts/");prefix=scripts==std::string::npos?(slash==std::string::npos?"":module.substr(0,slash+1)):module.substr(0,scripts);
        }
        for(char& c:id)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'))c='-';
        while(!id.empty()&&(id.front()=='-'||id.front()=='_'))id.erase(id.begin());
        if(id.size()>48)id.resize(48);if(id.empty())id="manual-mod";
        if(id.size()>64)id.resize(64);plan.package.id=id;plan.package.version="folder-"+plan.hash.substr(0,12);
        const auto requirements=names.find(prefix+"undercoop.requires.ini");
        if(requirements!=names.end()){
            std::string text;if(!folder_import::Read(plan.archive,plan.folderFiles[requirements->second],text,error))return false;
            coop_packages::Manifest declaration;coop_packages::Error why;
            if(!coop_packages::ParseManifest("[Package]\nId=manual-requirements\nName=Requirements\nVersion=1\nKind=dependency\n"+text,declaration,why)){
                error=L"Invalid dependency metadata: "+Wide(why.detail);return false;}
            plan.package.requires=std::move(declaration.requires);
        }
    }
    std::vector<std::pair<size_t,coop_packages::File>> selected;std::string declaredPrefix;bool haveDeclaredPrefix=false;
    if(prepared){
        for(const auto& f:plan.package.files){const auto found=names.find(prefix+f.source);
            if(found==names.end()){error=L"A declared manual resource is missing: "+Wide(f.source);return false;}
            selected.push_back({found->second,f});}
    }else{
        const auto rootName=coop_mods::Lower(coop_mod_scan::Utf8(basename));
        for(size_t index=0;index<plan.folderFiles.size();++index){
            const auto& source=plan.folderFiles[index];const auto& name=source.path;
            const auto ext=coop_packages::detail::Extension(name);
            const auto leaf=name.substr(name.find_last_of('/')+1);
            if(leaf=="undercoop.requires.ini"||leaf=="undercoop.native.txt")continue;
            std::string destination,sourcePrefix;
            const auto declared=folder_import::RuntimeRoot(rootName)?rootName+"/"+name:name;
            const bool explicitRoot=folder_import::DeclaredDestination(declared,destination,sourcePrefix,error);
            if(!explicitRoot&&!error.empty())return false;
            if(explicitRoot){
                if(haveDeclaredPrefix&&sourcePrefix!=declaredPrefix){error=L"The folder contains separate game installation roots. Register them separately; no resources were omitted.";return false;}
                declaredPrefix=sourcePrefix;haveDeclaredPrefix=true;
            }
            bool runtime=explicitRoot;
            if(!runtime){runtime=ImportRuntimeDestination(declared,gameRoot,destination,error);if(!runtime&&!error.empty())return false;}
            if(!runtime&&plan.code&&name.rfind(prefix,0)==0){
                // Loose ASI sidecars belong beside that ASI; explicit game
                // trees take precedence, allowing native+resource packages.
                const auto relative=name.substr(prefix.size());
                if(relative.rfind("readme",0)==0||relative.rfind("license",0)==0||ext==".md"||ext==".pdf")continue;
                destination="scripts/"+relative;
            }
            if(destination.empty()){
                if(ext==".asi"||ext==".dll"){error=L"Native code has an ambiguous installation root: "+Wide(name);return false;}
                continue;
            }
            std::string canonical;
            if(!coop_packages::CanonicalPath(destination,canonical)||coop_packages::detail::ReservedDestination(canonical)||
               canonical=="scripts/nfsu2coop.asi"||canonical=="scripts/dinput8.dll"){
                error=L"Unsupported or reserved manual target: "+Wide(destination);return false;}
            const auto first=canonical.substr(0,canonical.find('/'));
            if((ext==".asi"||ext==".dll")&&first!="scripts"){error=L"Native code must use a scripts-style plugin layout.";return false;}
            if(ext==".dll"&&!plan.code){error=L"A DLL-only folder needs a supported plugin/loader recipe.";return false;}
            if(ext!=".asi"&&ext!=".dll"&&!coop_packages::detail::Asset(canonical)){
                error=L"Unsupported runtime file in manual folder: "+Wide(name);return false;}
            selected.push_back({index,{canonical,"files/"+canonical}});
        }
    }
    std::set<std::string> bundled,outputs;std::set<size_t> included;
    for(const auto& item:selected){if(!outputs.insert(item.second.destination).second){error=L"Multiple manual files map to the same game path.";return false;}bundled.insert(item.second.destination);included.insert(item.first);}
    for(const auto module:modules)if(!included.count(module)){error=L"A native plugin would be omitted. Register separate installation roots separately.";return false;}
    for(const auto& chosen:selected){
        if(cancel&&cancel->load()){error=L"Folder inspection cancelled.";return false;}
        const auto& source=plan.folderFiles[chosen.first];std::string content;
        if(!folder_import::Read(plan.archive,source,content,error))return false;
        ImportItem item{chosen.first,chosen.second.source,chosen.second.destination,source.hash,source.bytes};
        const auto ext=coop_packages::detail::Extension(item.destination);item.module=ext==".asi";item.config=ext==".ini";
        if(!folder_import::NativeFile(item,content,bundled,gameRoot,error))return false;
        if(std::find_if(plan.items.begin(),plan.items.end(),[&](const auto& f){return f.source==item.source;})!=plan.items.end())continue;
        plan.items.push_back(item);plan.bytes+=item.bytes;
        if(plan.code)plan.native.files.push_back({item.source,item.destination,item.hash,item.bytes,item.module,item.config});
        else if(!prepared)plan.package.files.push_back(chosen.second);
    }
    if(plan.items.empty()){error=L"No game-ready resources were found. Arrange files under CARS, GLOBAL, scripts or another supported game folder.";return false;}
    if(plan.code){plan.native.id=plan.package.id;plan.native.version=plan.package.version;plan.native.archive=plan.hash;
        if(!coop_import::Valid(plan.native)){error=L"This manual native layout cannot be registered safely.";return false;}}
    bool unlimiter=false;for(const auto& f:plan.items)unlimiter=unlimiter||f.destination.rfind("scripts/unlimiterdata/",0)==0;
    if(unlimiter){bool declared=false;for(const auto& r:plan.package.requires)declared=declared||r.id=="nfsu2-unlimiter";if(!declared)plan.package.requires.push_back({"nfsu2-unlimiter","3.1.1.1337"});}
    for(auto& r:plan.package.requires)if(r.id=="nfsu2-extra-options"&&r.version=="5.1.0.1340")r.version="local-2f59d8f8";
    coop_packages::Error why;if(!coop_packages::ValidateManifest(plan.package,why)){error=Wide(why.detail);return false;}
    plan.manifestText=ImportManifest(plan.package);
    const auto parent=plan.archive.substr(0,plan.archive.find_last_of(L'\\'));
    plan.adoptFolder=!prepared&&!gameRoot.empty()&&Equal(parent,gameRoot+L"\\mods");
    if(plan.adoptFolder)plan.notes.push_back(L"This unregistered mods folder will be organized automatically. Its original contents are retained under mods/_undercoop/manual-backups.");
    plan.notes.push_back(L"The launcher creates the package metadata. Installation stays disabled until you enable the mod; use the same prepared folder on your teammate's computer.");
    if(plan.code)plan.notes.push_back(L"Native code is not sandboxed or proven CO-OP-compatible. Separate approval is required before launch.");
    return true;
}

inline bool ImportFolderTargetAvailable(const std::wstring& root,const ImportPlan& plan,std::wstring& error){
    const auto target=root+L"\\mods\\"+Wide(plan.package.id);
    if(GetFileAttributesW(target.c_str())==INVALID_FILE_ATTRIBUTES){
        const auto reason=GetLastError();if(reason==ERROR_FILE_NOT_FOUND||reason==ERROR_PATH_NOT_FOUND)return true;
    }else if(plan.adoptFolder&&Equal(target,plan.archive)&&RealPath(target,true)){
        const auto manifest=target+L"\\undercoop.mod.ini";
        if(GetFileAttributesW(manifest.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND)return true;
    }
    error=L"A registered package or different folder already uses this ID. It was not overwritten: "+Wide(plan.package.id);return false;
}
inline bool ImportCommitFolder(const std::wstring& root,const ImportPlan& plan,std::wstring& error,const std::atomic<bool>* cancel=nullptr){
    error.clear();coop_packages::Error why;
    if(!plan.folder||plan.binary||plan.runtimeMissing||plan.folderFiles.empty()||plan.folderFiles.size()>4096||
       plan.items.empty()||plan.items.size()>4096||!coop_import::Hash(plan.hash)||
       !coop_packages::ValidateManifest(plan.package,why)||plan.manifestText!=ImportManifest(plan.package)||
       plan.code&&(!coop_import::Valid(plan.native)||plan.native.id!=plan.package.id||plan.native.version!=plan.package.version||plan.native.archive!=plan.hash)){
        error=L"The reviewed manual installation plan is invalid.";return false;}
    const auto mods=root+L"\\mods",owner=mods+L"\\_undercoop";auto modsPin=Pin(mods,true),ownerPin=Pin(owner,true);
    if(!modsPin||!ownerPin||!ImportFolderTargetAvailable(root,plan,error))return false;
    std::vector<ImportFolderFile> current;std::string digest;
    if(!folder_import::Snapshot(plan.archive,current,digest,error,cancel))return false;
    if(digest!=plan.hash||current.size()!=plan.folderFiles.size()){error=L"The manual source folder changed after inspection. Review it again.";return false;}
    for(size_t i=0;i<current.size();++i)if(current[i].path!=plan.folderFiles[i].path||current[i].volume!=plan.folderFiles[i].volume||current[i].fileId!=plan.folderFiles[i].fileId){
        error=L"A manual source file was replaced after inspection. Review the folder again.";return false;}
    auto sourcePin=Pin(plan.archive,true);if(!sourcePin){error=L"The manual source root changed.";return false;}
    const auto stamp=std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    const auto relative=L"install-staging\\folder-"+stamp;
    if(!ImportDirectory(owner,relative,error))return false;const auto stage=owner+L"\\"+relative;
    for(const auto& item:plan.items){
        if(cancel&&cancel->load()){error=L"Folder installation cancelled. Incomplete staging is retained privately; no package was activated.";return false;}
        if(item.index>=plan.folderFiles.size()){error=L"Invalid manual source entry.";return false;}
        std::string content;const auto& source=plan.folderFiles[item.index];
        if(!folder_import::Read(plan.archive,source,content,error)||item.hash!=source.hash||item.bytes!=source.bytes||
           !ImportWriteNew(stage,item.source,content,error))return false;
    }
    if(!ImportWriteNew(stage,"undercoop.mod.ini",plan.manifestText,error)||
       plan.code&&!ImportWriteNew(stage,"undercoop.native.txt",coop_import::Encode(plan.native),error))return false;
    // A directory can gain or lose files while its selected resources are
    // copied. Recheck the complete source, not only the files being installed.
    std::vector<ImportFolderFile> finalFiles;std::string finalDigest;
    if(!folder_import::Snapshot(plan.archive,finalFiles,finalDigest,error,cancel))return false;
    if(finalDigest!=plan.hash||finalFiles.size()!=plan.folderFiles.size()){
        error=L"The manual source folder changed during staging. Review it again; no package was published.";return false;}
    for(size_t i=0;i<finalFiles.size();++i)if(finalFiles[i].path!=plan.folderFiles[i].path||finalFiles[i].volume!=plan.folderFiles[i].volume||finalFiles[i].fileId!=plan.folderFiles[i].fileId){
        error=L"A manual source file was replaced during staging. Review it again.";return false;}
    if(cancel&&cancel->load()){error=L"Folder installation cancelled before publication.";return false;}
    if(!ImportFolderTargetAvailable(root,plan,error)||!RealPath(stage,true)||!Below(stage,owner))return false;
    std::wstring original;
    if(plan.adoptFolder){
        if(!Equal(plan.archive.substr(0,plan.archive.find_last_of(L'\\')),mods)||
           Equal(plan.archive,owner)||!Below(plan.archive,mods)){error=L"The manual folder is outside its reviewed location.";return false;}
        if(!ImportDirectory(owner,L"manual-backups",error))return false;
        original=owner+L"\\manual-backups\\"+stamp+L"-"+Wide(plan.package.id);
        sourcePin.reset();
        if(!RealPath(plan.archive,true)||GetFileAttributesW(original.c_str())!=INVALID_FILE_ATTRIBUTES||
           !MoveFileExW(plan.archive.c_str(),original.c_str(),MOVEFILE_WRITE_THROUGH)){
            error=L"Cannot retain the original manual folder. Nothing was replaced.";return false;}
    }
    const auto target=mods+L"\\"+Wide(plan.package.id);
    if(!MoveFileExW(stage.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)){
        if(!original.empty()&&!MoveFileExW(original.c_str(),plan.archive.c_str(),MOVEFILE_WRITE_THROUGH))
            error=L"Registration failed. The untouched original folder is retained at: "+original;
        else error=L"Could not publish the manual package. Original files were retained; private staging remains available.";
        return false;
    }
    return true;
}
