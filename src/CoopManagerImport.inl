// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Archive import is pre-launch only. No archive installer/script is executed.
inline bool EnsureArchiveEngine(const std::wstring&,std::wstring&);
struct ImportItem{size_t index=0;std::string source,destination,hash;uint64_t bytes=0;bool module=false,config=false;std::string generated;};
struct ImportFolderFile{std::wstring relative;std::string path,hash;uint64_t bytes=0,fileId=0;uint32_t volume=0;};
struct PackArchivePlan;
struct ImportPlan{
    std::wstring archive;std::string hash,manifestText;coop_packages::Manifest package;coop_import::Receipt native;
    std::vector<ImportItem> items;std::vector<std::wstring> notes;std::vector<std::string> binaryChoices;uint64_t bytes=0;bool code=false,binary=false,runtimeMissing=false;
    bool folder=false,adoptFolder=false;
    std::vector<ImportFolderFile> folderFiles;
    std::shared_ptr<PackArchivePlan> pack;
};
inline bool InspectBinaryArchive(coop_archive::Archive&,const std::wstring&,ImportPlan&,std::wstring&,const std::atomic<bool>*,const std::string&);
inline std::string ImportHash(const std::string& bytes){coop_mod_scan::Sha256 sha;coop_mods::Hash h{};return sha.Bytes(bytes.data(),bytes.size(),h)?coop_mod_scan::Hex(h):"";}
inline std::wstring ImportUnsupportedInstaller(const std::string& path){
    const auto ext=coop_packages::detail::Extension(path);
    if(ext==".u2car"||ext==".u2cfg")return L"This archive needs the legacy NFS-CfgInstaller data adapter. Its vehicle configuration cannot be ignored; no incomplete car was installed: "+Wide(path);
    if(ext==".nfsms")return L"This archive contains a VltEd/database script, not a supported Underground2 Binary installer. No script was executed or silently skipped: "+Wide(path);
    if(ext==".tpf")return L"This archive needs a TexMod texture-package adapter. Textures were not silently omitted: "+Wide(path);
    if(ext==".exe"||ext==".msi"||ext==".bat"||ext==".cmd"||ext==".ps1"||ext==".vbs"||ext==".wsf"||ext==".reg")return L"This archive needs an installer adapter; bundled programs, system changes and shell scripts are not run automatically: "+Wide(path);
    if(ext==".end")return L"This Binary archive needs a supported Underground2 User-mode launcher: "+Wide(path);
    return {};
}
inline std::string ImportManifest(const coop_packages::Manifest& p){
    std::string text="[Package]\nId="+p.id+"\nName="+p.name+"\nVersion="+p.version+"\nKind="+(p.kind==coop_packages::Kind::Assets?"assets":p.kind==coop_packages::Kind::Data?"data":"dependency")+"\n";
    if(!p.files.empty()){text+="[Files]\n";for(const auto& f:p.files)text+=f.destination+"="+f.source+"\n";}
    if(!p.requires.empty()){text+="[Requires]\n";for(const auto& r:p.requires)text+=r.id+"="+r.version+"\n";}return text;
}
#include "CoopManagerPackImport.inl"
inline bool ImportReadArchive(const std::wstring& path,std::string& bytes,std::wstring& error){
    if(!ReadText(path,bytes,128*1024*1024)){error=L"The archive is linked, busy, unreadable or larger than 128 MiB.";return false;}return true;
}
// Resolve declared game-relative trees and abbreviated trees anchored by a
// real existing game subdirectory. No README commands or mod names are used.
inline bool ImportRuntimeDestination(const std::string& name,const std::wstring& root,std::string& destination,std::wstring& error){
    destination.clear();error.clear();std::set<std::string> candidates;
    const auto add=[&](const std::string& path){std::string canonical;
        if(coop_packages::CanonicalPath(path,canonical)&&coop_packages::detail::Asset(canonical)&&
           !coop_packages::detail::ReservedDestination(canonical))candidates.insert(canonical);};
    for(size_t at=0;at<name.size();){
        const auto tail=name.substr(at);const auto slash=tail.find('/');
        if(slash!=std::string::npos){const auto first=tail.substr(0,slash);
            for(const auto* category:{"cars","global","frontend","languages","tracks","movies","sound","scripts"}){
                if(first==category)add(tail);
            }
        }
        const auto next=name.find('/',at);if(next==std::string::npos)break;at=next+1;
    }
    // A declared game root takes precedence over inferred short names.
    if(candidates.empty()&&!root.empty())for(size_t at=0;at<name.size();){
        const auto tail=name.substr(at);const auto slash=tail.find('/');
        if(slash!=std::string::npos){const auto first=tail.substr(0,slash);
            for(const auto* category:{"cars","global","frontend","languages","tracks","movies","sound","scripts"}){
                const auto folder=root+L"\\"+Wide(category)+L"\\"+Wide(first);
                if(Pin(folder,true))add(std::string(category)+"/"+tail);
            }
        }
        const auto next=name.find('/',at);if(next==std::string::npos)break;at=next+1;
    }
    if(candidates.size()>1){error=L"The archive resource has multiple possible game destinations; an explicit folder mapping is required: "+Wide(name);return false;}
    if(candidates.empty())return false;destination=*candidates.begin();return true;
}
inline bool ImportInspect(const std::wstring& path,ImportPlan& plan,std::wstring& error,const std::atomic<bool>* cancel=nullptr,const std::wstring& gameRoot={},const std::string& binaryLauncher={}){
    plan={};error.clear();plan.archive=path;std::string bytes,why;
    if(!ImportReadArchive(path,bytes,error))return false;plan.hash=ImportHash(bytes);if(plan.hash.empty()){error=L"Cannot hash this archive.";return false;}
    if(coop_archive::RequiresEngine(bytes)&&!coop_archive::EngineReady()&&!EnsureArchiveEngine(gameRoot,error))return false;
    coop_archive::Archive zip;if(!zip.Open(std::move(bytes),why,coop_mod_scan::Utf8(path.substr(path.find_last_of(L"\\/")+1)),cancel)){error=Wide(why);return false;}
    const auto& entries=zip.Entries();std::map<std::string,size_t> files;std::vector<size_t> manifests,modules;std::string prefix;
    for(const auto& e:entries)if(!e.directory){const auto name=coop_mods::Lower(e.path);files[name]=e.index;
        if(name=="undercoop.mod.ini"||name.size()>18&&name.substr(name.size()-18)=="/undercoop.mod.ini")manifests.push_back(e.index);
        if(coop_packages::detail::Extension(name)==".asi")modules.push_back(e.index);
    }
    if(manifests.size()>1){error=L"This archive contains multiple packages. Import one package at a time.";return false;}
    bool prepared=!manifests.empty();
    if(!prepared){bool hasBinary=false;for(const auto& e:entries)if(!e.directory&&coop_packages::detail::Extension(coop_mods::Lower(e.path))==".end")hasBinary=true;
        if(hasBinary)return InspectBinaryArchive(zip,gameRoot,plan,error,cancel,binaryLauncher);}
    if(prepared){std::string manifest;if(!zip.Extract(manifests.front(),manifest,why)){error=Wide(why);return false;}
        coop_packages::Error reason;if(!coop_packages::ParseManifest(manifest,plan.package,reason)){error=L"Invalid package manifest: "+Wide(reason.detail);return false;}
        const auto name=coop_mods::Lower(entries[manifests.front()].path);const auto slash=name.find_last_of('/');prefix=slash==std::string::npos?"":name.substr(0,slash+1);
        if(plan.package.kind==coop_packages::Kind::Dependency){error=L"Prepared native dependencies must use a supported download recipe. Import the original scripts-style archive instead.";return false;}
        if(plan.package.kind==coop_packages::Kind::Data){error=L"Data installers must be inspected from their original archive so their operations and inputs can be verified.";return false;}
        if(!modules.empty()){error=L"An asset manifest cannot authorize bundled native code. Import a separate native package.";return false;}
    }
    plan.code=!modules.empty();
    if(!prepared){
        for(const auto& e:entries)if(!e.directory){
            const auto unsupported=ImportUnsupportedInstaller(coop_mods::Lower(e.path));if(!unsupported.empty()){error=unsupported;return false;}}
        plan.package.kind=plan.code?coop_packages::Kind::Dependency:coop_packages::Kind::Assets;
        if(plan.code){
            const auto module=coop_mods::Lower(entries[modules.front()].path);const auto slash=module.find_last_of('/');
            const auto scripts=module.rfind("scripts/");prefix=scripts==std::string::npos?(slash==std::string::npos?"":module.substr(0,slash+1)):module.substr(0,scripts);
            auto name=module.substr(slash==std::string::npos?0:slash+1);name.resize(name.size()-4);plan.package.id=name;
            plan.package.name=entries[modules.front()].path.substr(slash==std::string::npos?0:slash+1);plan.package.name.resize(plan.package.name.size()-4);
        }else{
            for(const auto& pair:files){const auto& name=pair.first;for(const auto* root:{"cars/","global/","frontend/","languages/","tracks/","movies/","sound/","scripts/"}){
                    const auto at=name.find(root);if(at!=std::string::npos&&(at==0||name[at-1]=='/')){prefix=name.substr(0,at);goto foundAssetRoot;}}
            }foundAssetRoot:;
            auto filename=coop_archive::Stem(coop_mod_scan::Utf8(path.substr(path.find_last_of(L"\\/")+1)));
            std::string id;for(char c:coop_mods::Lower(filename))id+=((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_')?c:'-';
            while(!id.empty()&&(id.front()=='-'||id.front()=='_'))id.erase(id.begin());if(id.size()>48)id.resize(48);if(id.empty())id="imported-mod";
            plan.package.id=id;plan.package.name=coop_packages::detail::Utf8(filename,true)?filename:id;
        }
        if(plan.package.id.size()>64)plan.package.id.resize(64);plan.package.version=(zip.Format()=="ZIP"?"zip-":"archive-")+plan.hash.substr(0,12);
        const auto req=files.find(prefix+"undercoop.requires.ini");if(req!=files.end()){
            std::string requirements;if(!zip.Extract(req->second,requirements,why)){error=Wide(why);return false;}
            coop_packages::Manifest declared;coop_packages::Error reason;
            if(!coop_packages::ParseManifest("[Package]\nId=import-requirements\nName=Requirements\nVersion=1\nKind=dependency\n"+requirements,declared,reason)){
                error=L"Invalid dependency metadata: "+Wide(reason.detail);return false;}plan.package.requires=std::move(declared.requires);
        }
    }
    std::vector<std::pair<size_t,coop_packages::File>> selected;
    if(prepared){for(const auto& f:plan.package.files){const auto it=files.find(prefix+f.source);if(it==files.end()){error=L"Declared package resource is missing: "+Wide(f.source);return false;}selected.push_back({it->second,f});}}
    else for(const auto& e:entries){
        if(e.directory)continue;const auto name=coop_mods::Lower(e.path);if(name.rfind(prefix,0)!=0)continue;auto destination=name.substr(prefix.size());
        if(plan.code&&prefix.find("scripts/")==std::string::npos){bool hasScripts=false;for(const auto i:modules)hasScripts=hasScripts||coop_mods::Lower(entries[i].path).rfind(prefix+"scripts/",0)==0;
            if(!hasScripts)destination="scripts/"+destination;}
        const auto first=destination.substr(0,destination.find('/')),ext=coop_packages::detail::Extension(destination);
        if(destination=="undercoop.requires.ini"||destination=="scripts/undercoop.requires.ini")continue;
        const bool runtimeFolder=first=="cars"||first=="global"||first=="frontend"||first=="languages"||first=="tracks"||first=="movies"||first=="sound"||first=="scripts";
        if(!runtimeFolder){const auto unsupported=ImportUnsupportedInstaller(name);if(!unsupported.empty()){error=unsupported;return false;}
            std::string resolved;if(!ImportRuntimeDestination(name,gameRoot,resolved,error)){if(!error.empty())return false;continue;}destination=std::move(resolved);}
        if(ext!=".asi"&&ext!=".dll"&&!coop_packages::detail::Asset(destination)){error=L"Unsupported runtime file: "+Wide(e.path);return false;}
        std::string normalized;if(!coop_packages::CanonicalPath(destination,normalized)||coop_packages::detail::ReservedDestination(normalized)){
            error=L"Unsupported or reserved runtime path: "+Wide(e.path);return false;}
        if((ext==".asi"||ext==".dll")&&first!="scripts"){error=L"Native code must be a scripts-style plugin, not a replacement game executable.";return false;}
        if(ext==".dll"&&!plan.code){error=L"A DLL-only package needs a supported plugin/loader recipe; it is not a data replacement.";return false;}
        selected.push_back({e.index,{normalized,"files/"+normalized}});
    }
    if(!prepared){std::set<size_t> chosen;for(const auto& f:selected)chosen.insert(f.first);
        for(const auto module:modules)if(!chosen.count(module)){error=L"This archive contains multiple native installation roots. Import separate packages instead of silently omitting plugins.";return false;}
        for(const auto& e:entries)if(!e.directory&&!chosen.count(e.index)){
            const auto pathName=coop_mods::Lower(e.path);bool runtime=false;
            for(const auto* root:{"cars/","global/","frontend/","languages/","tracks/","movies/","sound/","scripts/"}){
                const auto at=pathName.find(root);if(at!=std::string::npos&&(at==0||pathName[at-1]=='/'))runtime=true;}
            const auto ext=coop_packages::detail::Extension(pathName);
            if(runtime&&pathName.substr(pathName.find_last_of('/')+1)!="undercoop.requires.ini"&&(coop_packages::detail::Asset(pathName)||ext==".dll")){
                error=L"The archive has ambiguous runtime folders. No resources were silently dropped: "+Wide(e.path);return false;}
        }
    }
    std::set<std::string> bundled;for(const auto& f:selected)bundled.insert(f.second.destination);
    for(const auto& selectedFile:selected){if(cancel&&cancel->load()){error=L"Import cancelled.";return false;}
        const auto& e=entries[selectedFile.first];std::string content;if(!zip.Extract(e.index,content,why)){error=Wide(why);return false;}
        ImportItem item{e.index,selectedFile.second.source,selectedFile.second.destination,ImportHash(content),e.bytes};
        if(item.hash.empty()){error=L"Cannot fingerprint an archive resource.";return false;}const auto ext=coop_packages::detail::Extension(item.destination);
        item.module=ext==".asi";item.config=ext==".ini";
        if(ext==".asi"||ext==".dll"){
            std::vector<std::string> imports;if(!coop_import::PE(content,imports)){error=L"Native plugin is not a valid 32-bit DLL: "+Wide(e.path);return false;}
            const auto parent=item.destination.substr(0,item.destination.find_last_of('/')+1);
            for(const auto& dll:imports)if(!bundled.count(parent+dll)){
                bool elsewhere=false;for(const auto& pathName:bundled)elsewhere=elsewhere||pathName.substr(pathName.find_last_of('/')+1)==dll;
                if(elsewhere){error=L"Native dependency must be beside the plugin that imports it: "+Wide(dll);return false;}
                wchar_t system[MAX_PATH]{};if(!GetSystemDirectoryW(system,MAX_PATH)){error=L"Cannot inspect Windows dependencies.";return false;}
                const auto candidate=std::wstring(system)+L"\\"+Wide(dll);
                const bool apiSet=dll.rfind("api-ms-win-",0)==0||dll.rfind("ext-ms-win-",0)==0;
                const bool inGame=!gameRoot.empty()&&RealPath(gameRoot+L"\\"+Wide(dll));
                if(!apiSet&&!inGame&&!GetModuleHandleW(Wide(dll).c_str())&&GetFileAttributesW(candidate.c_str())==INVALID_FILE_ATTRIBUTES){
                    error=L"Missing Windows/native library: "+Wide(dll)+L". No trusted download recipe exists for this DLL; Windows installers are not run automatically.";return false;}
            }
        }
        const auto duplicate=std::find_if(plan.items.begin(),plan.items.end(),[&](const auto& f){return f.source==item.source;});
        if(duplicate!=plan.items.end()){if(duplicate->hash!=item.hash||duplicate->bytes!=item.bytes){error=L"Ambiguous shared package source.";return false;}continue;}
        plan.bytes+=item.bytes;plan.items.push_back(item);
        if(plan.code)plan.native.files.push_back({item.source,item.destination,item.hash,item.bytes,item.module,item.config});
        else if(!prepared)plan.package.files.push_back(selectedFile.second);
    }
    if(plan.items.empty()){error=L"No supported game resources were found. Use a prepared package or an archive with CARS/GLOBAL/scripts folders.";return false;}
    if(plan.code){plan.native.id=plan.package.id;plan.native.version=plan.package.version;plan.native.archive=plan.hash;
        if(!coop_import::Valid(plan.native)){error=L"This native layout cannot be installed safely.";return false;}
        plan.notes.push_back(L"Native code is not sandboxed and may conflict with CO-OP. Installation does not enable it; approval is required before launch.");
    }
    bool unlimiter=false;for(const auto& f:plan.items)unlimiter=unlimiter||f.destination.rfind("scripts/unlimiterdata/",0)==0;
    if(unlimiter){bool declared=false;for(const auto& r:plan.package.requires)declared=declared||r.id=="nfsu2-unlimiter";if(!declared)plan.package.requires.push_back({"nfsu2-unlimiter","3.1.1.1337"});}
    for(auto& r:plan.package.requires)if(r.id=="nfsu2-extra-options"&&r.version=="5.1.0.1340")r.version="local-2f59d8f8";
    coop_packages::Error reason;if(!coop_packages::ValidateManifest(plan.package,reason)){error=Wide(reason.detail);return false;}
    plan.manifestText=ImportManifest(plan.package);
    plan.notes.push_back(L"Only declared metadata and DLL imports can reveal requirements; arbitrary README instructions cannot be inferred reliably.");
    return true;
}
inline bool ImportDirectory(const std::wstring& base,const std::wstring& relative,std::wstring& error){
    if(!RealPath(base,true)){error=L"The import root is missing or linked.";return false;}
    for(size_t at=0;at<=relative.size();++at)if(at==relative.size()||relative[at]==L'\\'){
        const auto directory=base+L"\\"+relative.substr(0,at);if(!Below(directory,base)){error=L"Invalid import directory.";return false;}
        const auto parent=directory.substr(0,directory.find_last_of(L'\\'));auto parentPin=Pin(parent,true);
        if(!parentPin){error=L"The import parent changed.";return false;}
        if(GetFileAttributesW(directory.c_str())==INVALID_FILE_ATTRIBUTES&&!CreateDirectoryW(directory.c_str(),nullptr)){error=L"Cannot create the private import folder.";return false;}
        if(!RealPath(directory,true)){error=L"An import folder is linked or invalid.";return false;}
    }return true;
}
inline bool ImportWriteNew(const std::wstring& base,const std::string& relative,const std::string& bytes,std::wstring& error){
    std::string canonical;if(!coop_packages::CanonicalPath(relative,canonical)||canonical!=relative){error=L"Invalid staged file path.";return false;}
    const auto disk=Disk(Wide(relative));const auto slash=disk.find_last_of(L'\\');if(slash!=std::wstring::npos&&!ImportDirectory(base,disk.substr(0,slash),error))return false;
    const auto path=base+L"\\"+disk;auto parent=Pin(path.substr(0,path.find_last_of(L'\\')),true);if(!parent){error=L"A staging parent changed.";return false;}
    auto handle=IniHandle(path,GENERIC_READ|GENERIC_WRITE,CREATE_NEW);DWORD written=0;
    if(!handle||!WriteFile(handle->value,bytes.data(),DWORD(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(handle->value)){
        error=L"Cannot write a new staged resource. Existing files were not replaced.";return false;}
    std::string hash;uint64_t length=0;if(!Digest(handle->value,hash,length)||hash!=ImportHash(bytes)||length!=bytes.size()){error=L"Staged resource verification failed.";return false;}return true;
}
inline bool ImportCommit(const std::wstring& root,const ImportPlan& plan,std::wstring& error,const std::atomic<bool>* cancel=nullptr){
    if(plan.runtimeMissing){error=L"Install the required Binary runtime, then inspect this archive again.";return false;}
    if(!plan.binaryChoices.empty()){error=L"Choose and inspect a Binary installation entry before installing this package.";return false;}
    coop_packages::Error reason;
    if(!coop_packages::ValidateManifest(plan.package,reason)||plan.manifestText!=ImportManifest(plan.package)||!coop_import::Hash(plan.hash)||plan.items.empty()||plan.items.size()>4096||
        (plan.code&&(!coop_import::Valid(plan.native)||plan.native.id!=plan.package.id||plan.native.version!=plan.package.version))){error=L"The inspected installation plan is invalid.";return false;}
    const auto mods=root+L"\\mods",owner=mods+L"\\_undercoop";auto rootPin=Pin(mods,true),ownerPin=Pin(owner,true);
    if(!rootPin||!ownerPin){error=L"The private mod workspace is unavailable.";return false;}
    const auto target=mods+L"\\"+Wide(plan.package.id);if(GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES){error=L"A package with this ID already exists. It was not overwritten: "+Wide(plan.package.id);return false;}
    std::string bytes,why;if(!ImportReadArchive(plan.archive,bytes,error)||ImportHash(bytes)!=plan.hash){error=L"The archive changed after inspection. Select it again.";return false;}
    if(coop_archive::RequiresEngine(bytes)&&!coop_archive::EngineReady()&&!EnsureArchiveEngine(root,error))return false;
    coop_archive::Archive zip;if(!zip.Open(std::move(bytes),why,coop_mod_scan::Utf8(plan.archive.substr(plan.archive.find_last_of(L"\\/")+1)),cancel)){error=Wide(why);return false;}
    const auto relative=L"install-staging\\"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    if(!ImportDirectory(owner,relative,error))return false;const auto stage=owner+L"\\"+relative;
    for(const auto& item:plan.items){if(cancel&&cancel->load()){error=L"Import cancelled. Incomplete staging is retained privately; no package was activated.";return false;}
        std::string content;if(item.index==SIZE_MAX)content=item.generated;
        if((item.index!=SIZE_MAX&&!zip.Extract(item.index,content,why))||ImportHash(content)!=item.hash||content.size()!=item.bytes){error=L"Archive resource verification failed: "+Wide(item.source);return false;}
        if(!ImportWriteNew(stage,item.source,content,error))return false;
    }
    if(!ImportWriteNew(stage,"undercoop.mod.ini",plan.manifestText,error)||plan.code&&!ImportWriteNew(stage,"undercoop.native.txt",coop_import::Encode(plan.native),error))return false;
    if(cancel&&cancel->load()){error=L"Import cancelled before publication.";return false;}
    if(!RealPath(stage,true)||!Below(stage,owner)||!MoveFileExW(stage.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)){
        error=L"Could not publish the package. No existing package was replaced; staging is retained under mods/_undercoop.";return false;}
    return true;
}
#include "CoopManagerFolderImport.inl"
#include "CoopArchiveEngineWin.inl"
#include "CoopBinaryEngineWin.inl"
