// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

// Compile every manager native-load call against a trap. These tests only
// inspect bytes and prepare requests; no synthetic or official DLL can execute.
static size_t nativeLoads=0;
static HMODULE WINAPI NeverLoadLibraryExW(LPCWSTR,HANDLE,DWORD){++nativeLoads;return nullptr;}
static HMODULE WINAPI NeverLoadLibraryExA(LPCSTR,HANDLE,DWORD){++nativeLoads;return nullptr;}
static HMODULE WINAPI NeverLoadLibraryW(LPCWSTR){++nativeLoads;return nullptr;}
static HMODULE WINAPI NeverLoadLibraryA(LPCSTR){++nativeLoads;return nullptr;}
#define LoadLibraryExW NeverLoadLibraryExW
#define LoadLibraryExA NeverLoadLibraryExA
#define LoadLibraryW NeverLoadLibraryW
#define LoadLibraryA NeverLoadLibraryA
static bool failFolderPublication=false;
static BOOL WINAPI TestMoveFileExW(LPCWSTR source,LPCWSTR target,DWORD flags){
    if(failFolderPublication&&std::wcsstr(source,L"install-staging\\folder-")){
        failFolderPublication=false;SetLastError(ERROR_ACCESS_DENIED);return FALSE;
    }
    return MoveFileExW(source,target,flags);
}
#define MoveFileExW TestMoveFileExW
#include "../src/CoopModManagerWin.h"
#undef MoveFileExW
#undef LoadLibraryExW
#undef LoadLibraryExA
#undef LoadLibraryW
#undef LoadLibraryA

namespace m=CoopModManager;
static size_t checks=0,failures=0;
static void Check(bool okay,const char* expression,int line){
    ++checks;if(!okay){++failures;std::printf("FAIL %d: %s (winerr %lu)\n",line,expression,GetLastError());}
}
#define CHECK(...) Check(bool((__VA_ARGS__)),#__VA_ARGS__,__LINE__)
static void Need(bool okay,const char* why){if(!okay){std::printf("Fixture error: %s (%lu)\n",why,GetLastError());std::exit(2);}}
static void Reset(){m::state=m::State{};coop_mod_scan::effectivePath=nullptr;coop_mod_scan::managedChange=nullptr;}

// All writes, deletions and publication targets belong to this fresh TEMP
// child. The two optional catalog ZIPs and user ZIP are read only inputs.
class Fixture{
    std::wstring parent_;
    void Clean(const std::wstring& path){
        Need(m::Equal(path,root)||m::Below(path,root),"cleanup containment");
        const DWORD attrs=GetFileAttributesW(path.c_str());if(attrs==INVALID_FILE_ATTRIBUTES)return;
        if(!(attrs&FILE_ATTRIBUTE_DIRECTORY)){Need(DeleteFileW(path.c_str()),"delete owned file");return;}
        if(!(attrs&FILE_ATTRIBUTE_REPARSE_POINT)){
            WIN32_FIND_DATAW row{};HANDLE find=FindFirstFileW((path+L"\\*").c_str(),&row);
            if(find!=INVALID_HANDLE_VALUE){do{if(std::wcscmp(row.cFileName,L".")&&std::wcscmp(row.cFileName,L".."))Clean(path+L"\\"+row.cFileName);}while(FindNextFileW(find,&row));FindClose(find);}
        }Need(RemoveDirectoryW(path.c_str()),"delete owned directory");
    }
public:
    std::wstring root;
    Fixture(){
        wchar_t temp[MAX_PATH]{};const DWORD n=GetTempPathW(MAX_PATH,temp);Need(n&&n<MAX_PATH,"TEMP path");parent_=temp;
        while(!parent_.empty()&&parent_.back()==L'\\')parent_.pop_back();Need(m::RealPath(parent_,true),"plain TEMP parent");
        for(unsigned i=0;i<100;++i){root=parent_+L"\\UndercoopFolderTests-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(i);
            Need(root.size()<150&&m::Below(root,parent_),"unique private TEMP child");if(CreateDirectoryW(root.c_str(),nullptr))return;
            Need(GetLastError()==ERROR_ALREADY_EXISTS,"create private fixture");}Need(false,"fixture uniqueness");
    }
    ~Fixture(){Reset();Need(m::Below(root,parent_)&&m::RealPath(root,true),"final cleanup boundary");Clean(root);}
    std::wstring Path(const std::wstring& relative)const{
        const auto path=root+L"\\"+relative;wchar_t full[MAX_PATH]{};const DWORD n=GetFullPathNameW(path.c_str(),MAX_PATH,full,nullptr);
        Need(n&&n<MAX_PATH&&m::Equal(path,full)&&m::Below(full,root),"fixture path containment");return path;
    }
    std::wstring Dir(const std::wstring& relative){const auto path=Path(relative);
        for(size_t i=root.size()+1;i<=path.size();++i)if(i==path.size()||path[i]==L'\\'){
            const auto part=path.substr(0,i);if(!CreateDirectoryW(part.c_str(),nullptr))Need(GetLastError()==ERROR_ALREADY_EXISTS&&m::RealPath(part,true),"create plain fixture directory");}
        return path;
    }
    std::wstring File(const std::wstring& relative,const std::string& bytes){const auto path=Path(relative);const auto slash=relative.find_last_of(L'\\');if(slash!=std::wstring::npos)Dir(relative.substr(0,slash));
        if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)Need(m::RealPath(path),"replace own plain file");
        m::Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));Need(file.value!=INVALID_HANDLE_VALUE,"create owned fixture file");
        DWORD written=0;Need(WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size(),"write owned fixture bytes");return path;
    }
    std::wstring Use(const std::wstring& name){Reset();Dir(name+L"\\mods\\_undercoop\\state");m::state.root=Path(name);return m::state.root;}
};
static std::string Read(const std::wstring& path){std::string bytes;Need(m::ReadText(path,bytes,128*1024*1024),"read test bytes");return bytes;}
static bool Exists(const std::wstring& path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;}
static void W16(std::string& b,size_t at,uint16_t n){Need(at+2<=b.size(),"W16 range");b[at]=char(n&255);b[at+1]=char(n>>8);}
static void W32(std::string& b,size_t at,uint32_t n){W16(b,at,uint16_t(n&65535));W16(b,at+2,uint16_t(n>>16));}
static void A16(std::string& b,uint16_t n){b+=char(n&255);b+=char(n>>8);}
static void A32(std::string& b,uint32_t n){A16(b,uint16_t(n&65535));A16(b,uint16_t(n>>16));}
static uint32_t Crc(const std::string& bytes){uint32_t c=0xffffffffu;for(unsigned char b:bytes){c^=b;for(unsigned i=0;i<8;++i)c=(c>>1)^((c&1)?0xedb88320u:0);}return c^0xffffffffu;}
struct Member{std::string path,bytes;};
static std::string Zip(const std::vector<Member>& members){std::string bytes;std::vector<size_t> offsets;
    for(const auto& f:members){offsets.push_back(bytes.size());A32(bytes,0x04034b50);A16(bytes,20);A16(bytes,0);A16(bytes,0);A16(bytes,0);A16(bytes,0);
        A32(bytes,Crc(f.bytes));A32(bytes,uint32_t(f.bytes.size()));A32(bytes,uint32_t(f.bytes.size()));A16(bytes,uint16_t(f.path.size()));A16(bytes,0);bytes+=f.path;bytes+=f.bytes;}
    const size_t cd=bytes.size();for(size_t i=0;i<members.size();++i){const auto& f=members[i];A32(bytes,0x02014b50);A16(bytes,20);A16(bytes,20);A16(bytes,0);A16(bytes,0);A16(bytes,0);A16(bytes,0);
        A32(bytes,Crc(f.bytes));A32(bytes,uint32_t(f.bytes.size()));A32(bytes,uint32_t(f.bytes.size()));A16(bytes,uint16_t(f.path.size()));A16(bytes,0);A16(bytes,0);A16(bytes,0);A16(bytes,0);A32(bytes,0x20);A32(bytes,uint32_t(offsets[i]));bytes+=f.path;}
    const size_t end=bytes.size();A32(bytes,0x06054b50);A16(bytes,0);A16(bytes,0);A16(bytes,uint16_t(members.size()));A16(bytes,uint16_t(members.size()));A32(bytes,uint32_t(end-cd));A32(bytes,uint32_t(cd));A16(bytes,0);return bytes;
}
static std::string FakePE(const std::string& dependency=""){
    std::string b(0x600,0);b.replace(0,2,"MZ");W32(b,60,0x80);b.replace(0x80,4,std::string("PE\0\0",4));W16(b,0x84,0x14c);W16(b,0x86,1);W16(b,0x94,224);W16(b,0x96,0x2102);
    W16(b,0x98,0x10b);W32(b,0x98+92,16);const size_t section=0x98+224;b.replace(section,5,".data");W32(b,section+8,0x400);W32(b,section+12,0x1000);W32(b,section+16,0x400);W32(b,section+20,0x200);
    if(!dependency.empty()){W32(b,0x98+104,0x1000);W32(b,0x98+108,40);W32(b,0x200+12,0x1080);b.replace(0x280,dependency.size(),dependency);}return b;
}

static std::wstring careerTestBase;
static bool TestCareerBase(std::wstring& out){out=careerTestBase;return m::RealPath(out,true);}
static void Package(Fixture& f,const std::wstring& tag,const std::string& id){
    const auto base=tag+L"\\mods\\"+m::Wide(id);
    f.File(base+L"\\undercoop.mod.ini","[Package]\nId="+id+"\nName="+id+"\nVersion=1\nKind=assets\n[Files]\ncars/"+id+"/geometry.bin=geometry.bin\n");
    f.File(base+L"\\geometry.bin","existing-managed-resource");
}
static void Sentinels(Fixture& f,const std::wstring& tag){
    f.File(tag+L"\\CARS\\STOCK\\GEOMETRY.BIN","stock-original");
    f.File(tag+L"\\scripts\\NFSU2Coop.ini","[COOP]\nProfile=NAME\n");
    f.File(tag+L"\\saves\\NAME\\NAME","personal-career");
}
static void Preserved(const std::wstring& root){
    CHECK(Read(root+L"\\CARS\\STOCK\\GEOMETRY.BIN")=="stock-original");
    CHECK(Read(root+L"\\scripts\\NFSU2Coop.ini")=="[COOP]\nProfile=NAME\n");
    CHECK(Read(root+L"\\saves\\NAME\\NAME")=="personal-career");
}
static bool Finish(m::View& view){
    const auto began=GetTickCount64();
    while(!view.imports.done.load()&&GetTickCount64()-began<10000)Sleep(1);
    CHECK(view.imports.done.load());
    if(!view.imports.done.load()){view.imports.cancel=true;if(view.imports.worker.joinable())view.imports.worker.join();return false;}
    CHECK(m::PollImport(view));CHECK(!view.busy&&!view.imports.worker.joinable());CHECK(nativeLoads==0);return true;
}
static bool Review(m::View& view,const std::wstring& source){
    CHECK(m::BeginImport(view,m::ImportWork::Inspect,source));
    CHECK(view.busy&&view.tab==5);if(!Finish(view))return false;
    CHECK(view.imports.phase==m::ImportPhase::Review&&view.imports.plan&&view.imports.plan->folder);
    if(!view.imports.plan)std::wprintf(L"Review failed: %ls\n",m::ImportStatus(view.imports).c_str());
    return bool(view.imports.plan);
}
static void ExternalAndModpacks(Fixture& f){
    const auto root=f.Use(L"a");Sentinels(f,L"a");Package(f,L"a","retained");
    CHECK(m::Discover(root));m::state.selected={"retained"};CHECK(m::SaveSelection());
    const auto source=f.Dir(L"incoming\\Manual car");
    f.File(L"incoming\\Manual car\\CARS\\MANUAL\\GEOMETRY.BIN","manual-geometry");
    f.File(L"incoming\\Manual car\\CARS\\MANUAL\\TEXTURES.BIN","manual-textures");
    f.File(L"incoming\\Manual car\\readme.txt","source documentation");
    m::View view;
    if(!Review(view,source))return;
    CHECK(!view.imports.plan->code&&!view.imports.plan->adoptFolder&&view.imports.plan->items.size()==2);
    CHECK(view.imports.plan->package.id=="manual-car");
    const m::ImportLayout layout(920,620);
    CHECK(!layout.Folder().Contains(layout.Pick().x+1,layout.Pick().y+1));
    CHECK(m::ImportHitTest(view,layout.Folder().x+1,layout.Folder().y+1,920,620).action==m::UiAction::ImportFolder);
    CHECK(m::BeginImport(view,m::ImportWork::Install));if(!Finish(view))return;
    CHECK(view.imports.phase==m::ImportPhase::Complete);
    CHECK(m::state.selected==std::vector<std::string>{"retained"});
    CHECK(Read(root+L"\\mods\\_undercoop\\state\\selection.txt")=="retained\n");
    CHECK(Read(root+L"\\mods\\manual-car\\files\\cars\\manual\\geometry.bin")=="manual-geometry");
    CHECK(Read(source+L"\\CARS\\MANUAL\\TEXTURES.BIN")=="manual-textures");
    CHECK(!Exists(source+L"\\undercoop.mod.ini"));
    CHECK(m::ImportStatus(view.imports).find(L"Folder registered")!=std::wstring::npos);
    const auto found=std::find_if(m::state.catalog.begin(),m::state.catalog.end(),[](const auto& p){return p.id=="manual-car";});
    CHECK(found!=m::state.catalog.end());
    if(found==m::state.catalog.end())return;
    const size_t index=static_cast<size_t>(found-m::state.catalog.begin());
    view.tab=1;CHECK(m::DispatchUi(view,{m::UiAction::Package,index})==m::UiEffect::Redraw);
    CHECK(std::find(m::state.selected.begin(),m::state.selected.end(),"manual-car")!=m::state.selected.end());
    CHECK(m::DispatchUi(view,{m::UiAction::Package,index})==m::UiEffect::Redraw);
    CHECK(m::state.selected==std::vector<std::string>{"retained"});
    CHECK(m::DispatchUi(view,{m::UiAction::Package,index})==m::UiEffect::Redraw);
    careerTestBase=f.Dir(L"careers");m::careerBase=&TestCareerBase;
    f.File(L"a\\mods\\pending\\CARS\\PENDING\\GEOMETRY.BIN","unregistered");
    m::ModpackLoaded library;CHECK(m::ModpackReadCatalog(root+L"\\mods",library,true));
    CHECK(library.catalog.size()==2);
    std::wstring pack;CHECK(m::CreateModpack(L"Manual setup",pack));
    if(pack.empty()){std::wprintf(L"Pack failed: %ls\n",m::state.error.c_str());return;}
    CHECK(m::ActivateModpack(pack));CHECK(m::state.instanceId==pack);
    CHECK(m::Below(m::PackageDirectory("manual-car"),m::ModpackHome()));
    CHECK(Read(m::PackageDirectory("manual-car")+L"\\files\\cars\\manual\\geometry.bin")=="manual-geometry");
    const auto activeBefore=m::state.packageRoot;const auto enabledBefore=m::state.selected;
    const auto newSource=f.Dir(L"later-folder");f.File(L"later-folder\\CARS\\LATER\\GEOMETRY.BIN","later-library-only");
    if(Review(view,newSource)){
        CHECK(m::BeginImport(view,m::ImportWork::Install));CHECK(Finish(view));
        CHECK(view.imports.phase==m::ImportPhase::Complete&&m::state.instanceId==pack);
        CHECK(m::state.packageRoot==activeBefore&&m::state.selected==enabledBefore&&m::state.catalog.size()==2);
        CHECK(Exists(root+L"\\mods\\later-folder\\undercoop.mod.ini"));
        CHECK(m::ImportStatus(view.imports).find(L"Modpacks > Edit")!=std::wstring::npos);
    }
    CHECK(m::EditModpack(pack,{"retained"}));CHECK(m::state.catalog.size()==1);
    CHECK(m::EditModpack(pack,{"retained","manual-car"}));CHECK(m::state.catalog.size()==2);
    CHECK(m::UseModLibrary());CHECK(m::state.instanceId.empty());
    CHECK(m::Discover(root));CHECK(m::state.pendingFolders==std::vector<std::wstring>{L"pending"});
    CHECK(m::state.catalog.size()==3&&m::state.selected.size()==2);Preserved(root);CHECK(nativeLoads==0);
}
static void Adoption(Fixture& f){
    const auto root=f.Use(L"b");Sentinels(f,L"b");Package(f,L"b","retained");
    const auto source=f.Dir(L"b\\mods\\Manual rim");
    f.File(L"b\\mods\\Manual rim\\GLOBAL\\RIMS.BIN","manual-rims");
    f.File(L"b\\mods\\Manual rim\\README.txt","original-folder-documentation");
    CHECK(m::Discover(root));CHECK(m::state.catalog.size()==1&&m::state.pendingFolders.size()==1);
    m::state.selected={"retained"};CHECK(m::SaveSelection());
    m::View view;if(!Review(view,source))return;
    CHECK(view.imports.plan->adoptFolder&&view.imports.plan->package.id=="manual-rim");
    CHECK(m::BeginImport(view,m::ImportWork::Install));if(!Finish(view))return;
    CHECK(view.imports.phase==m::ImportPhase::Complete);CHECK(!Exists(source));
    CHECK(Read(root+L"\\mods\\manual-rim\\files\\global\\rims.bin")=="manual-rims");
    WIN32_FIND_DATAW row{};const auto home=root+L"\\mods\\_undercoop\\manual-backups";
    HANDLE find=FindFirstFileW((home+L"\\*").c_str(),&row);bool original=false;
    if(find!=INVALID_HANDLE_VALUE){do{if(row.cFileName[0]==L'.')continue;
        const auto path=home+L"\\"+row.cFileName;
        if(Exists(path+L"\\README.txt")){CHECK(Read(path+L"\\README.txt")=="original-folder-documentation");CHECK(Read(path+L"\\GLOBAL\\RIMS.BIN")=="manual-rims");original=true;}
    }while(FindNextFileW(find,&row));FindClose(find);}
    CHECK(original&&m::state.pendingFolders.empty());CHECK(m::state.selected==std::vector<std::string>{"retained"});
    Preserved(root);
}
static void AdoptionRollback(Fixture& f){
    const auto root=f.Use(L"e");Sentinels(f,L"e");
    const auto source=f.Dir(L"e\\mods\\rollback");f.File(L"e\\mods\\rollback\\GLOBAL\\RIMS.BIN","original-rollback-content");
    m::ImportPlan plan;std::wstring error;
    CHECK(m::ImportInspectFolder(source,plan,error,nullptr,root));CHECK(plan.adoptFolder);
    failFolderPublication=true;CHECK(!m::ImportCommitFolder(root,plan,error));CHECK(!failFolderPublication);
    CHECK(Exists(source));CHECK(!Exists(source+L"\\undercoop.mod.ini"));
    CHECK(Read(source+L"\\GLOBAL\\RIMS.BIN")=="original-rollback-content");
    CHECK(m::Discover(root)&&m::state.catalog.empty()&&m::state.pendingFolders==std::vector<std::wstring>{L"rollback"});
    CHECK(m::ImportCommitFolder(root,plan,error));CHECK(Read(root+L"\\mods\\rollback\\files\\global\\rims.bin")=="original-rollback-content");
    Preserved(root);
}
static bool Junction(const std::wstring& link,const std::wstring& target){
    struct MountPoint {DWORD tag;WORD bytes,reserved;WORD subOffset,subBytes,printOffset,printBytes;wchar_t paths[MAX_PATH*2+8];} data{};
    const auto sub=L"\\??\\"+target;const size_t characters=sub.size()+target.size()+2;
    if(characters>std::size(data.paths))return false;
    data.tag=IO_REPARSE_TAG_MOUNT_POINT;data.subBytes=WORD(sub.size()*2);data.printOffset=WORD(data.subBytes+2);data.printBytes=WORD(target.size()*2);
    data.bytes=WORD(8+characters*2);std::memcpy(data.paths,sub.c_str(),(sub.size()+1)*2);std::memcpy(reinterpret_cast<BYTE*>(data.paths)+data.printOffset,target.c_str(),(target.size()+1)*2);
    m::Handle directory(CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    DWORD done=0;return directory.value!=INVALID_HANDLE_VALUE&&DeviceIoControl(directory.value,FSCTL_SET_REPARSE_POINT,&data,DWORD(8+data.bytes),nullptr,0,&done,nullptr);
}
static void ReparseRejected(Fixture& f){
    const auto root=f.Use(L"f");Sentinels(f,L"f");const auto source=f.Dir(L"linked-input");
    f.File(L"linked-input\\CARS\\TEST\\GEOMETRY.BIN","reviewed-link-source");
    const auto destination=f.Dir(L"other-tree");f.File(L"other-tree\\TEST\\GEOMETRY.BIN","outside-link-content");
    m::ImportPlan plan;std::wstring error;CHECK(m::ImportInspectFolder(source,plan,error,nullptr,root));
    CHECK(MoveFileExW((source+L"\\CARS").c_str(),(source+L"\\CARS-before").c_str(),MOVEFILE_WRITE_THROUGH));
    const auto link=f.Dir(L"linked-input\\CARS");CHECK(Junction(link,destination));
    CHECK(!m::ImportCommitFolder(root,plan,error));CHECK(!Exists(root+L"\\mods\\linked-input"));
    CHECK(!m::ImportInspectFolder(source,plan,error,nullptr,root));
    CHECK(Read(destination+L"\\TEST\\GEOMETRY.BIN")=="outside-link-content");Preserved(root);
}
static void NativeFolder(Fixture& f){
    const auto root=f.Use(L"c");Sentinels(f,L"c");
    const auto source=f.Dir(L"native-input");
    f.File(L"native-input\\scripts\\ManualPlugin.asi",FakePE("USER32.dll"));
    f.File(L"native-input\\scripts\\ManualPlugin.ini","[Options]\nEnabled=1\n");
    m::View view;if(!Review(view,source))return;
    CHECK(view.imports.plan->code&&coop_import::Valid(view.imports.plan->native));
    CHECK(!m::BeginImport(view,m::ImportWork::Install));
    CHECK(m::DispatchUi(view,{m::UiAction::ImportConsent})==m::UiEffect::Redraw);
    CHECK(m::BeginImport(view,m::ImportWork::Install));if(!Finish(view))return;
    CHECK(view.imports.phase==m::ImportPhase::Complete);CHECK(m::state.selected.empty());
    CHECK(m::state.imported.count("manualplugin")==1);CHECK(!m::state.nativeTrusted);
    CHECK(m::Discover(root)&&m::state.selected.empty());
    std::vector<m::IniFile> settings;std::wstring why;
    CHECK(m::DiscoverIni(settings,why));CHECK(std::any_of(settings.begin(),settings.end(),[](const auto& file){return file.owner=="manualplugin";}));
    CHECK(Read(root+L"\\mods\\manualplugin\\files\\scripts\\manualplugin.asi")==FakePE("USER32.dll"));
    CHECK(Read(source+L"\\scripts\\ManualPlugin.ini")=="[Options]\nEnabled=1\n");
    CHECK(nativeLoads==0);Preserved(root);
}
static void ChangesAndUnsafe(Fixture& f){
    const auto root=f.Use(L"d");Sentinels(f,L"d");std::wstring error;m::ImportPlan plan;
    auto source=f.Dir(L"stale-input");f.File(L"stale-input\\CARS\\TEST\\GEOMETRY.BIN","reviewed");
    CHECK(m::ImportInspectFolder(source,plan,error,nullptr,root));
    f.File(L"stale-input\\CARS\\TEST\\GEOMETRY.BIN","changed");
    CHECK(!m::ImportCommitFolder(root,plan,error));CHECK(error.find(L"changed")!=std::wstring::npos);
    CHECK(!Exists(root+L"\\mods\\stale-input"));
    CHECK(m::ImportInspectFolder(source,plan,error,nullptr,root));
    f.File(L"stale-input\\CARS\\TEST\\TEXTURES.BIN","new-file");
    CHECK(!m::ImportCommitFolder(root,plan,error));CHECK(!Exists(root+L"\\mods\\stale-input"));
    std::atomic<bool> cancel{true};CHECK(!m::ImportInspectFolder(source,plan,error,&cancel,root));
    cancel=false;CHECK(m::ImportInspectFolder(source,plan,error,&cancel,root));cancel=true;
    CHECK(!m::ImportCommitFolder(root,plan,error,&cancel));CHECK(!Exists(root+L"\\mods\\stale-input"));
    source=f.Dir(L"installer-input");f.File(L"installer-input\\CARS\\TEST\\GEOMETRY.BIN","data");
    for(const auto* name:{L"Install.end",L"Install.nfsms",L"Install.exe",L"Install.u2cfg"}){
        const auto path=f.File(std::wstring(L"installer-input\\")+name,"never executed");
        CHECK(!m::ImportInspectFolder(source,plan,error,nullptr,root));CHECK(DeleteFileW(path.c_str()));
    }
    source=f.Dir(L"mixed-input");f.File(L"mixed-input\\VersionA\\CARS\\A\\GEOMETRY.BIN","a");f.File(L"mixed-input\\VersionB\\CARS\\B\\GEOMETRY.BIN","b");
    CHECK(!m::ImportInspectFolder(source,plan,error,nullptr,root));CHECK(error.find(L"separate")!=std::wstring::npos);
    source=f.Dir(L"bad-native");f.File(L"bad-native\\scripts\\Plugin.asi",FakePE("WrongPlace.dll"));f.File(L"bad-native\\scripts\\nested\\WrongPlace.dll",FakePE());
    CHECK(!m::ImportInspectFolder(source,plan,error,nullptr,root));CHECK(error.find(L"beside")!=std::wstring::npos);
    CHECK(!m::ImportInspectFolder(root+L"\\mods\\_undercoop",plan,error,nullptr,root));
    Preserved(root);CHECK(nativeLoads==0);
}
int main(){
    Fixture fixture;ExternalAndModpacks(fixture);Adoption(fixture);AdoptionRollback(fixture);ReparseRejected(fixture);NativeFolder(fixture);ChangesAndUnsafe(fixture);
    m::careerBase=&m::DefaultCareerBase;
    CHECK(nativeLoads==0);std::printf("CoopManagerFolderImport: %zu checks, %zu failures; private TEMP only; no game/native execution\n",checks,failures);
    return failures?1:0;
}
