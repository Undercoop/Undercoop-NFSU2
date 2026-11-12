// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopModPackages.h"
#include "CoopModScanWin.h"
#include "CoopOverlayText.h"
#include "CoopManagerGraphics.h"
#include "CoopIniDocument.h"
#include "CoopBuildVersion.h"
#include "CoopManagerImportTypes.h"
#include "CoopZipImport.h"
#include "CoopZipFileWin.h"
#include "CoopArchiveImport.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <winhttp.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wintrust.h>
#include <softpub.h>
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"wintrust.lib")
#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"comdlg32.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"uuid.lib")
#include <dwmapi.h>
#pragma comment(lib,"dwmapi.lib")
#include "CoopProfileContext.h"
#include <shlobj.h>
#include <map>
#include <set>
#include <memory>

// Experimental ASI-hosted manager. Never loaded by ordinary 6.12.08 builds.
// Packages are declarative data. Known local native adapters are hash-pinned;
// no installer, shell command, downloaded executable or arbitrary DLL is run.
#include "CoopCareerBackupWin.h"
#include "CoopCareerHistory.h"
#include "CoopCareerProgress.h"

namespace CoopModManager {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v=INVALID_HANDLE_VALUE):value(v){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
struct Mapping {std::wstring original,physical;std::string relative,hash;uint64_t bytes=0;bool inventory=true;};
struct ConfigWatch{std::wstring physical;std::string relative;coop_mods::Entry expected;};
struct State {
    std::wstring root,error,packageRoot,instanceId,instanceName;std::vector<coop_packages::Manifest> catalog;
    std::vector<std::string> selected;std::vector<Mapping> mapping;
    std::map<std::string,coop_import::Receipt> imported;
    std::vector<std::unique_ptr<Handle>> held;
    coop_packages::Plan plan;std::string pack;std::wstring saveFolder,spFolder;std::vector<ConfigWatch> watches;
    bool ready=false,nativeTrusted=false,fatal=false;HMODULE native=nullptr;std::vector<HMODULE> nativeModules;
    std::vector<std::wstring> inventoryNativePaths;
    // Unregistered user folders are visible to the importer, never part of an
    // effective setup until their contents have been reviewed and registered.
    std::vector<std::wstring> pendingFolders;
};
inline State state;
inline std::wstring Wide(const std::string& value){
    if(value.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
    if(n<=0)return {};std::wstring out(size_t(n),0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),out.data(),n);return out;
}
inline std::wstring Disk(std::wstring value){for(auto& c:value)if(c==L'/')c=L'\\';return value;}
inline bool Equal(const std::wstring& a,const std::wstring& b){return CompareStringOrdinal(a.c_str(),int(a.size()),b.c_str(),int(b.size()),TRUE)==CSTR_EQUAL;}
inline bool Below(const std::wstring& child,const std::wstring& root){return child.size()>root.size()&&child[root.size()]==L'\\'&&Equal(child.substr(0,root.size()),root);}
inline bool Fail(const std::wstring& error){state.error=error;return false;}
inline bool RealPath(const std::wstring& path,bool directory=false){
    if(path.size()<3||path.size()>=MAX_PATH||path[1]!=L':'||path[2]!=L'\\')return false;
    for(size_t start=3;start<path.size();){const size_t end=path.find(L'\\',start);const auto part=path.substr(start,end==std::wstring::npos?end:end-start);
        if(part.empty()||part==L"."||part==L".."||part.back()==L'.'||part.back()==L' ')return false;
        for(wchar_t c:part)if(c<32||c==127||std::wcschr(L"/:*?\"<>|",c))return false;
        auto stem=part.substr(0,part.find(L'.'));for(auto& c:stem)if(c>=L'A'&&c<=L'Z')c=wchar_t(c+32);
        if(stem==L"con"||stem==L"prn"||stem==L"aux"||stem==L"nul"||
            (stem.size()==4&&(stem.substr(0,3)==L"com"||stem.substr(0,3)==L"lpt")&&stem[3]>=L'1'&&stem[3]<=L'9'))return false;
        if(end==std::wstring::npos)break;start=end+1;if(start==path.size())return false;
    }
    for(size_t n=3;n<=path.size();++n){
        if(n<path.size()&&path[n]!=L'\\')continue;
        const auto attrs=GetFileAttributesW(path.substr(0,n).c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES||(attrs&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        if((n<path.size()||directory)&&!(attrs&FILE_ATTRIBUTE_DIRECTORY))return false;
        if(n==path.size()&&!directory&&(attrs&FILE_ATTRIBUTE_DIRECTORY))return false;
    }return true;
}
inline std::unique_ptr<Handle> Pin(const std::wstring& path,bool directory=false){
    if(!RealPath(path,directory))return {};
    auto out=std::make_unique<Handle>(CreateFileW(path.c_str(),directory?FILE_LIST_DIRECTORY:GENERIC_READ,
        directory?FILE_SHARE_READ|FILE_SHARE_WRITE:FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:FILE_FLAG_SEQUENTIAL_SCAN),nullptr));
    if(out->value==INVALID_HANDLE_VALUE)return {};
    BY_HANDLE_FILE_INFORMATION info{};wchar_t actual[32768]{};
    const DWORD n=GetFinalPathNameByHandleW(out->value,actual,DWORD(std::size(actual)),FILE_NAME_NORMALIZED);
    if(!n||n>=std::size(actual)||!Equal(actual,L"\\\\?\\"+path)||!GetFileInformationByHandle(out->value,&info)||
        (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||GetFileType(out->value)!=FILE_TYPE_DISK)return {};
    return out;
}
inline bool ReadText(const std::wstring& path,std::string& output,size_t budget=256*1024){
    output.clear();
    auto file=Pin(path);if(!file)return false;LARGE_INTEGER size{};
    if(!GetFileSizeEx(file->value,&size)||size.QuadPart<0||uint64_t(size.QuadPart)>budget)return false;
    output.assign(size_t(size.QuadPart),0);DWORD read=0;
    const bool okay=output.empty()||ReadFile(file->value,output.data(),DWORD(output.size()),&read,nullptr)&&read==output.size();
    if(!okay)output.clear();return okay;
}
inline bool Digest(HANDLE file,std::string& hex,uint64_t& size){
    hex.clear();size=0;LARGE_INTEGER beginning{};if(!SetFilePointerEx(file,beginning,nullptr,FILE_BEGIN))return false;
    coop_mod_scan::Sha256 sha;BCRYPT_HASH_HANDLE hash=nullptr;if(!sha.Begin(hash))return false;
    std::array<uint8_t,65536> bytes{};bool okay=true;size=0;
    while(okay){DWORD got=0;if(!ReadFile(file,bytes.data(),DWORD(bytes.size()),&got,nullptr)){okay=false;break;}
        if(!got)break;size+=got;if(size>256ull*1024*1024||!coop_mod_scan::Sha256::Add(hash,bytes.data(),got))okay=false;}
    coop_mods::Hash result{};if(okay){okay=coop_mod_scan::Sha256::End(hash,result);hash=nullptr;}
    if(hash)BCryptDestroyHash(hash);LARGE_INTEGER zero{};okay=SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)&&okay;
    if(okay)hex=coop_mod_scan::Hex(result);else size=0;return okay;
}
inline std::wstring Resolve(const std::wstring& path){
    for(const auto& m:state.mapping)if(Equal(m.original,path))return m.physical;return path;
}
inline bool ResolveAnsi(char* path,size_t capacity){
    const int n=MultiByteToWideChar(CP_ACP,0,path,-1,nullptr,0);if(n<=0)return false;
    std::wstring wide(size_t(n),0);MultiByteToWideChar(CP_ACP,0,path,-1,wide.data(),n);wide.resize(size_t(n)-1);
    const auto resolved=Resolve(wide);BOOL changed=FALSE;
    const int bytes=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,resolved.c_str(),-1,nullptr,0,nullptr,&changed);
    if(bytes<=0||size_t(bytes)>capacity||changed)return false;
    return WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,resolved.c_str(),-1,path,int(capacity),nullptr,&changed)&&!changed;
}
inline std::wstring PackageDirectory(const std::string& id){return (state.packageRoot.empty()?state.root+L"\\mods":state.packageRoot)+L"\\"+Wide(id);}
#include "CoopManagerImportedNative.inl"
#include "CoopManagerNative.inl"
#include "CoopManagerImportedPrepare.inl"
inline bool SnapshotModpackCareers(const std::wstring&,const std::wstring&,const std::atomic<bool>*);
#include "CoopManagerModpacks.inl"
#include "CoopManagerBootstrap.inl"
inline bool Discover(const std::wstring& root){
    state.root=root;state.catalog.clear();state.selected.clear();state.imported.clear();state.pendingFolders.clear();state.error.clear();state.packageRoot.clear();state.instanceId.clear();state.instanceName.clear();
    if(!BootstrapInstallation(root))return false;
    WIN32_FIND_DATAW row{};HANDLE find=FindFirstFileW((root+L"\\mods\\*").c_str(),&row);
    if(find==INVALID_HANDLE_VALUE)return Fail(L"Cannot inspect the mods folder.");
    std::wstring error;
    do{
        const std::wstring name=row.cFileName;if(name==L"."||name==L".."||name==L"_undercoop")continue;
        if(row.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){error=L"Linked mod packages are not supported.";break;}
        if(!(row.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))continue;
        const auto manifestPath=root+L"\\mods\\"+name+L"\\undercoop.mod.ini";
        const DWORD manifestAttrs=GetFileAttributesW(manifestPath.c_str());
        if(manifestAttrs==INVALID_FILE_ATTRIBUTES){
            const DWORD missing=GetLastError();
            if(missing==ERROR_FILE_NOT_FOUND||missing==ERROR_PATH_NOT_FOUND){
                state.pendingFolders.push_back(name);
                if(state.pendingFolders.size()>256){error=L"Too many unregistered mod folders.";break;}
                continue;
            }
        }
        std::string text;coop_packages::Manifest item;coop_packages::Error why;
        if(!ReadText(manifestPath,text)||!coop_packages::ParseManifest(text,item,why)){
            error=L"Package needs a supported import: "+name+L". "+Wide(why.detail);break;
        }
        if(Wide(item.id)!=name){error=L"Package ID must match its lowercase folder name: "+name;break;}
        if(!ReadImportedNative(item)){error=state.error;break;}
        if(Selectable(item)&&!ImportedNativeKnown(item))state.selected.push_back(item.id);
        state.catalog.push_back(std::move(item));
        if(state.catalog.size()>256){error=L"Too many mod packages.";break;}
    }while(FindNextFileW(find,&row));
    if(error.empty()&&GetLastError()!=ERROR_NO_MORE_FILES)error=L"Package enumeration failed.";
    FindClose(find);if(!error.empty())return Fail(error);
    std::sort(state.catalog.begin(),state.catalog.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    std::sort(state.pendingFolders.begin(),state.pendingFolders.end());
    std::string chosen;
    const auto selectionPath=root+L"\\mods\\_undercoop\\state\\selection.txt";
    const DWORD selectionAttrs=GetFileAttributesW(selectionPath.c_str());
    const DWORD selectionError=selectionAttrs==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
    if(selectionAttrs!=INVALID_FILE_ATTRIBUTES&&!ReadText(selectionPath,chosen,16384))return Fail(L"The saved mod selection is invalid, linked or unreadable.");
    if(selectionAttrs==INVALID_FILE_ATTRIBUTES&&selectionError!=ERROR_FILE_NOT_FOUND&&selectionError!=ERROR_PATH_NOT_FOUND)return Fail(L"The saved selection cannot be inspected.");
    if(selectionAttrs!=INVALID_FILE_ATTRIBUTES){
        std::vector<std::string> selection;size_t start=0;
        while(start<chosen.size()){
            const auto end=chosen.find('\n',start);auto id=chosen.substr(start,end==std::string::npos?end:end-start);
            if(!id.empty()&&id.back()=='\r')id.pop_back();
            if(!id.empty())selection.push_back(id);if(end==std::string::npos)break;start=end+1;
        }state.selected=std::move(selection);
    }return LoadActiveModpack();
}
inline bool MakePlan(){
    coop_packages::Error error;state.error.clear();
    if(!coop_packages::BuildPlan(state.catalog,state.selected,state.plan,error))return Fail(Wide(error.detail));
    for(const auto& id:state.plan.Packages())for(const auto& p:state.catalog)if(p.id==id&&p.kind==coop_packages::Kind::Dependency&&
        !ApprovedNative(p))return Fail(L"No approved native adapter for "+Wide(p.id));
    for(const auto& id:state.plan.Packages())for(const auto& p:state.catalog)if(p.id==id&&p.kind==coop_packages::Kind::Data&&
        !RealPath(PackageDirectory(id)+L"\\undercoop.binary.txt"))return Fail(L"A data installer is missing its verified source recipe: "+Wide(p.id));
    return true;
}
inline bool SaveSelection(){
    if(!state.instanceId.empty())return SaveActiveModpackSelection();
    coop_packages::Plan checked;coop_packages::Error error;
    if(!coop_packages::BuildPlan(state.catalog,state.selected,checked,error))return false;
    const auto dir=state.root+L"\\mods\\_undercoop\\state";
    auto mods=Pin(state.root+L"\\mods",true),owner=Pin(state.root+L"\\mods\\_undercoop",true),directory=Pin(dir,true);
    if(!mods||!owner||!directory)return false;
    std::string text;for(const auto& id:state.selected)text+=id+"\n";
    const auto staging=dir+L"\\selection."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L".pending",target=dir+L"\\selection.txt";
    if(GetFileAttributesW(staging.c_str())!=INVALID_FILE_ATTRIBUTES)return false;
    if(GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES&&!RealPath(target))return false;
    Handle out(CreateFileW(staging.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(out.value==INVALID_HANDLE_VALUE)return false;DWORD n=0;
    bool okay=WriteFile(out.value,text.data(),DWORD(text.size()),&n,nullptr)&&n==text.size()&&FlushFileBuffers(out.value);
    CloseHandle(out.value);out.value=INVALID_HANDLE_VALUE;
    return okay&&MoveFileExW(staging.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
using AddPath=bool(WINAPI*)(const wchar_t*,const wchar_t*,int);
using RemovePath=void(WINAPI*)(const wchar_t*);
#include "CoopManagerLoader.inl"
#include "CoopManagerSaveSeed.inl"
#include "CoopManagerCareerDomain.inl"
struct PreparedActivation{
    std::vector<Mapping> mappings;std::vector<std::unique_ptr<Handle>> pins;
    std::vector<NativeRequest> natives;std::vector<ConfigWatch> watches;std::string pack;std::wstring saveFolder,legacySaveFolder;bool compatibleSeed=false;
};
inline bool PrepareBinaryData(std::vector<Mapping>&,std::vector<std::unique_ptr<Handle>>&,std::string&);
inline bool PrepareUnlimiterProfiles(PreparedActivation&,std::string&);
inline bool PrepareActivation(PreparedActivation& staged){
    if(state.ready||state.fatal)return false;if(!MakePlan()||!SelectedImportedLibrariesSafe())return false;
    staged=PreparedActivation{};auto& mappings=staged.mappings;auto& pins=staged.pins;
    std::string receipt="Undercoop manager protocol=2\n";
    auto rootPin=Pin(state.root+L"\\mods",true);if(!rootPin)return Fail(L"Cannot lock the mods folder.");pins.push_back(std::move(rootPin));
    for(const auto& id:state.plan.Packages()){
        const auto directory=PackageDirectory(id);auto pin=Pin(directory,true);
        if(!pin)return Fail(L"Cannot lock package "+Wide(id));pins.push_back(std::move(pin));
        for(const auto& p:state.catalog)if(p.id==id)receipt+=p.id+"="+p.version+"\n";
        if(id=="nfsu2-unlimiter"&&!state.imported.count(id)){
            if(GetModuleHandleW(L"NFSU2Unlimiter.asi"))return Fail(L"Unlimiter was loaded before the manager. Remove duplicate autoloading in this test copy.");
            const auto nativePath=directory+L"\\NFSU2Unlimiter.asi";auto native=Pin(nativePath);
            std::string hash;uint64_t size=0;
            if(!native||!Digest(native->value,hash,size)||hash!="ddbaadeb70e517a493835b3aa60b0d46e8dece30fe6beda6efa4c7fef553c11b")
                return Fail(L"Unlimiter does not match the approved 3.1.1.1337 component.");
            receipt+="Native="+hash+"\n";pins.push_back(std::move(native));
            const std::pair<const wchar_t*,const char*> approved[]{
                {L"NFSU2UnlimiterSettings.ini","8c78235dd806c56b742a4215e449d681272246ea0fab42b7735b6bd35c69f104"},
                {L"UnlimiterData\\_FNGFixes.ini","0dbbad04c29e5f66361bc6d7755d4d9bdb53be2902caca86b6f23db2cd761f0b"},
                {L"UnlimiterData\\_General.ini","eadba5c686a7f5696821c4de71930c8681fadc9a98074bb24032d95465d789b2"},
                {L"UnlimiterData\\_RimBrands.ini","6dee22b3b88249dfa035decf1ac6e5025e2bd9290cfaf4aa9b7a54ec8e868be5"},
                {L"UnlimiterData\\BMWM3GTR.ini","49cd517e33753537419f7f1c4fc325525b03ff61dd76a0b4a250dea6357b52ee"}
            };
            auto configDir=Pin(directory+L"\\UnlimiterData",true);if(!configDir)return Fail(L"Unlimiter configuration directory is invalid.");pins.push_back(std::move(configDir));
            for(const auto& config:approved){
                auto handle=Pin(directory+L"\\"+config.first);std::string content;uint64_t bytes=0;
                if(!handle||!Digest(handle->value,content,bytes)||content!=config.second)return Fail(L"Unlimiter configuration differs from the approved preview adapter.");
                receipt+=coop_mod_scan::Utf8(config.first)+"="+content+"\n";pins.push_back(std::move(handle));}
            // No unapproved DLL sidecars can participate in Windows dependency
            // search next to the explicitly loaded native component.
            for(const auto& relative:{std::wstring(),std::wstring(L"UnlimiterData\\")}){
                WIN32_FIND_DATAW info{};HANDLE scan=FindFirstFileW((directory+L"\\"+relative+L"*").c_str(),&info);
                if(scan==INVALID_HANDLE_VALUE)return Fail(L"Cannot inspect the native dependency payload.");bool valid=true;
                do{const std::wstring name=info.cFileName;if(name==L"."||name==L"..")continue;
                    if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){valid=false;break;}
                    const auto path=relative+name;bool known=relative.empty()&&(Equal(name,L"undercoop.mod.ini")||Equal(name,L"NFSU2Unlimiter.asi")||Equal(name,L"UnlimiterData"));
                    for(const auto& config:approved)known=known||Equal(path,config.first);
                    if(!known){valid=false;break;}
                }while(FindNextFileW(scan,&info));
                valid=valid&&GetLastError()==ERROR_NO_MORE_FILES;FindClose(scan);if(!valid)return Fail(L"Unexpected files beside the approved native component.");
            }
            staged.natives.push_back({nativePath,state.root});
        }else if(StandaloneNative(id)&&!state.imported.count(id)){
            const auto found=std::find_if(state.catalog.begin(),state.catalog.end(),[&](const auto& p){return p.id==id;});
            if(found==state.catalog.end()||!PrepareStandalone(*found,pins,staged.natives,staged.watches,receipt))return false;
        }else if(state.imported.count(id)){
            const auto found=std::find_if(state.catalog.begin(),state.catalog.end(),[&](const auto& p){return p.id==id;});
            if(found==state.catalog.end()||!PrepareImported(*found,pins,staged.natives,staged.watches,mappings,receipt))return false;
        }
    }
    for(const auto& file:state.plan.Index()){
        Mapping item;item.relative=file.destination;item.original=state.root+L"\\"+Disk(Wide(file.destination));
        item.physical=PackageDirectory(file.owner)+L"\\"+Disk(Wide(file.source));
        if(!Below(item.physical,state.root+L"\\mods"))return Fail(L"Package source escaped mods.");
        auto pin=Pin(item.physical);if(!pin||!Digest(pin->value,item.hash,item.bytes))return Fail(L"Cannot verify "+Wide(file.destination));
        receipt+=file.destination+"="+item.hash+"\n";pins.push_back(std::move(pin));mappings.push_back(std::move(item));
    }
    if(!PrepareBinaryData(mappings,pins,receipt))return false;
    if(!PrepareUnlimiterProfiles(staged,receipt))return false;
    std::set<std::wstring> nativeNames;for(const auto& n:staged.natives){auto name=n.path.substr(n.path.find_last_of(L'\\')+1);for(auto& c:name)if(c>=L'A'&&c<=L'Z')c=wchar_t(c+32);if(!nativeNames.insert(name).second)return Fail(L"The same native plugin is selected twice: "+name);}
    std::set<std::string> destinations;for(const auto& m:mappings)if(!destinations.insert(m.relative).second)return Fail(L"Imported resources conflict: "+Wide(m.relative));
    for(const auto& d:destinations)for(size_t slash=d.find('/');slash!=std::string::npos;slash=d.find('/',slash+1))if(destinations.count(d.substr(0,slash)))return Fail(L"Imported file/folder resources conflict.");
    coop_mod_scan::Sha256 sha;coop_mods::Hash hash{};
    if(!sha.Bytes(receipt.data(),receipt.size(),hash))return Fail(L"Cannot fingerprint this mod pack.");
    staged.pack=coop_mod_scan::Hex(hash);
    staged.legacySaveFolder=L"NFSU2 Undercoop Mods "+Wide(staged.pack.substr(0,24));
    if(!ResolveCareerDomain(staged.legacySaveFolder,staged.saveFolder))return false;
    for(const auto& m:mappings)if(m.relative=="global/globalb.lzc"&&m.hash=="d54e3225b7377d0a27576f2b7286c42d2e68cbc200d110cd1f0ef2bf05aabdaf")staged.compatibleSeed=true;
    return true;
}
inline bool ManagedInventoryFiles(const std::wstring& root,std::vector<coop_mod_scan::File>& files,std::string& error){
    if(!state.ready||!Equal(root,state.root))return true;
    auto add=[&](const std::wstring& physical,const std::string& canonical,bool overlay=false,bool inventory=true){
        if(coop_mods::IncludeFile(canonical))files.push_back({physical,canonical,true,overlay,inventory});
    };
    for(const auto& mapping:state.mapping)add(mapping.physical,mapping.relative,mapping.relative.rfind("scripts/",0)!=0,mapping.inventory);
    for(const auto& id:state.plan.Packages()){
        const auto directory=PackageDirectory(id);
        if(const auto* known=state.imported.count(id)?nullptr:StandaloneNative(id)){
            add(directory+L"\\"+known->file,"scripts/"+coop_mods::Lower(coop_mod_scan::Utf8(known->file)));
            if(id!="camera-toolkit")add(directory+L"\\"+known->config,"scripts/"+coop_mods::Lower(coop_mod_scan::Utf8(known->config)));
            if(known->data)add(directory+L"\\"+known->data,"scripts/"+coop_mods::Lower(coop_mod_scan::Utf8(known->data)));
        }else if(id=="nfsu2-unlimiter"){
            auto base=directory;
            for(const auto& path:state.inventoryNativePaths){const auto slash=path.find_last_of(L'\\');
                if(Equal(path.substr(slash+1),L"NFSU2Unlimiter.asi")){base=path.substr(0,slash);break;}}
            add(base+L"\\NFSU2Unlimiter.asi","scripts/nfsu2unlimiter.asi");
            add(base+L"\\NFSU2UnlimiterSettings.ini","scripts/nfsu2unlimitersettings.ini");
            WIN32_FIND_DATAW row{};HANDLE find=FindFirstFileW((base+L"\\UnlimiterData\\*").c_str(),&row);
            if(find==INVALID_HANDLE_VALUE){error="cannot enumerate selected Unlimiter profiles";return false;}
            bool okay=true;do{const std::wstring name=row.cFileName;if(name==L"."||name==L"..")continue;
                if(row.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)){okay=false;break;}
                add(base+L"\\UnlimiterData\\"+name,"scripts/unlimiterdata/"+coop_mods::Lower(coop_mod_scan::Utf8(name)));
            }while(FindNextFileW(find,&row));okay=okay&&GetLastError()==ERROR_NO_MORE_FILES;FindClose(find);
            if(!okay){error="selected Unlimiter profile directory changed";return false;}
        }else if(const auto imported=state.imported.find(id);imported!=state.imported.end()){
            for(const auto& file:imported->second.files)add(directory+L"\\"+Disk(Wide(file.source)),file.destination,file.destination.rfind("scripts/",0)!=0);
        }
    }return true;
}
inline bool Activate(){
    if(state.ready||state.fatal)return false;
    LoaderBinding loader;if(!BindActiveLoader(loader))return false;
    const auto add=loader.add;const auto remove=loader.remove;
    PreparedActivation staged;if(!PrepareActivation(staged))return false;
    staged.pins.push_back(std::move(loader.file));
    if(!staged.natives.empty()&&!state.nativeTrusted)return Fail(L"Open Components and approve the verified native components first.");
    if(!CommitCareerDomain(staged.legacySaveFolder,staged.saveFolder))return false;
    // Named setups already represent the user's explicit career snapshot,
    // including an intentionally empty career. Do not repopulate that snapshot
    // from the optional historical developer preview import.
    if(state.instanceId.empty()&&staged.compatibleSeed&&!ImportSeed(staged.saveFolder))return false;
    if(!SaveSelection())return Fail(L"Cannot save the selection safely under mods. Check write permissions.");
    size_t published=0;for(const auto& m:staged.mappings){if(!add(m.original.c_str(),m.physical.c_str(),100))break;++published;}
    if(published!=staged.mappings.size()){
        while(published)remove(staged.mappings[--published].original.c_str());return Fail(L"The file overlay could not be published.");
    }
    state.mapping=std::move(staged.mappings);state.held=std::move(staged.pins);state.watches=std::move(staged.watches);state.pack=staged.pack;
    state.saveFolder=staged.saveFolder;state.spFolder=state.saveFolder+L" SP";
    for(const auto& request:staged.natives){
        // Load once only after data mappings exist. Dependency is outside the
        // loader's autoscanned scripts tree; no command scripts are executed.
        HMODULE module=nullptr;try{module=LoadNative(request);}catch(...){module=nullptr;}
        if(!module){for(const auto& m:state.mapping)remove(m.original.c_str());state.mapping.clear();state.fatal=true;
            // Already-loaded modules and their file pins stay alive until the
            // failed process exits; native initialization cannot be undone.
            return Fail(L"A selected native plugin could not initialize: "+request.path.substr(request.path.find_last_of(L'\\')+1)+L". Restart required.");}
        state.nativeModules.push_back(module);if(!state.native)state.native=module;
        state.inventoryNativePaths.push_back(request.path);
    }
    coop_mod_scan::Change settingsChange;
    if(ManagedConfigChanged(state.root,"mods",settingsChange)){state.fatal=true;return Fail(L"A native component changed shared settings during initialization. Restart required.");}
    coop_mod_scan::effectivePath=&Resolve;coop_mod_scan::managedChange=&ManagedConfigChanged;
    state.ready=true;coop_mod_scan::additionalFiles=&ManagedInventoryFiles;return true;
}

#include "CoopManagerIni.inl"
#include "CoopManagerUndercoopSettings.inl"
#include "CoopManagerImport.inl"
#include "CoopManagerDownloads.inl"
#include "CoopManagerLiveNotice.inl"
#include "CoopManagerDeveloperSession.inl"
#include "CoopManagerCareerBackups.inl"
#include "CoopManagerCareerImport.inl"
#include "CoopModManagerUi.inl"
}
