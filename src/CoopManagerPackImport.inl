// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.

// A file-backed archive becomes an inactive, ordinary named modpack. Only an
// owned staging tree is written; no installer, native module or game is run.
enum class PackProbe { NotPack, Ready, Failed };
struct PackArchiveItem { size_t index=0;std::string owner,destination,source;uint64_t bytes=0;bool module=false,config=false; };
struct PackArchivePlan {
    std::wstring archive,name;std::string hash;uint64_t bytes=0;size_t files=0;
    std::vector<std::wstring> notes,nativeModules;
    std::vector<PackArchiveItem> items;std::vector<coop_packages::Manifest> packages;
};
inline bool PackArchiveHash(HANDLE file,std::string& hash,const std::atomic<bool>* cancel){
    hash.clear();LARGE_INTEGER zero{};if(!SetFilePointerEx(file,zero,nullptr,FILE_BEGIN))return false;
    coop_mod_scan::Sha256 sha;BCRYPT_HASH_HANDLE running=nullptr;if(!sha.Begin(running))return false;
    std::array<uint8_t,65536> block{};uint64_t total=0;bool good=true;
    while(good){DWORD n=0;if(cancel&&cancel->load()||!ReadFile(file,block.data(),DWORD(block.size()),&n,nullptr)){good=false;break;}
        if(!n)break;total+=n;if(total>coop_zip_file::kMaxArchive||!coop_mod_scan::Sha256::Add(running,block.data(),n))good=false;}
    coop_mods::Hash result{};if(good){good=coop_mod_scan::Sha256::End(running,result);running=nullptr;}
    if(running)BCryptDestroyHash(running);if(good)hash=coop_mod_scan::Hex(result);return good;
}
inline std::string PackNativeOwner(const std::string& path){
    if(path.rfind("scripts/",0)!=0)return {};const auto p=path.substr(8);
    if(p=="animationcontroller.asi"||p=="animationcontroller.ini")return "animation-controller";
    if(p=="nfsu2carsoundtuner.asi"||p=="nfsu2carsoundtunersettings.ini"||p.rfind("carsounddata/",0)==0)return "nfsu2-car-sound-tuner";
    if(p=="nfsu2extraoptions.asi"||p=="nfsu2extraoptionssettings.ini")return "nfsu2-extra-options";
    if(p=="nfsu2hdreflections.asi"||p=="nfsu2hdreflections.ini")return "nfsu2-hd-reflections";
    if(p=="nfsu2unlimiter.asi"||p=="nfsu2unlimitersettings.ini"||p.rfind("unlimiterdata/",0)==0)return "nfsu2-unlimiter";
    if(p=="nfsu2_stancemod.asi"||p=="nfsu2_stancemod.ini")return "nfsu2-stance-mod";
    if(p=="nfsunderground2.widescreenfix.asi"||p=="nfsunderground2.widescreenfix.ini"||p=="nfsunderground2.widescreenfix.dat")return "nfsu2-widescreen-fix";
    if(p=="reshade.asi"||p=="reshade.ini"||p.rfind("shaders/",0)==0||p.rfind("textures/",0)==0||p.find("-reshade-")!=std::string::npos)return "reshade";
    if(p=="speedreflect.asi")return "speedreflect";
    if(p=="texwizard.asi"||p=="texwizard.json")return "texwizard";
    return {};
}
inline bool PackExtraOptions(std::string& data,std::wstring& error){
    coop_ini::Document document;coop_ini::Error why;
    if(!coop_ini::Parse(data,document,why)){error=L"The bundled Extra Options INI cannot be parsed safely.";return false;}
    std::vector<coop_ini::Edit> edits;
    for(const auto& row:document.Rows()){
        const auto section=coop_mods::Lower(row.section),key=coop_mods::Lower(row.key);
        if((section=="hotkeys"&&(key=="unlockallthings"||key=="autodrive"))||
            (section=="gameplay"&&key=="unlockallthings"))edits.push_back({row.id,"0"});
    }
    if(edits.size()!=3){error=L"The bundled Extra Options control profile differs from the supported 1.0.3 layout.";return false;}
    std::string fixed;if(!coop_ini::Render(document,data,edits,fixed,why)){error=L"Cannot prepare the CO-OP-safe Extra Options controls.";return false;}
    data=std::move(fixed);return true;
}
inline PackProbe InspectModpackArchive(const std::wstring& path,PackArchivePlan& plan,std::wstring& error,const std::atomic<bool>* cancel=nullptr){
    plan={};error.clear();const auto dot=path.find_last_of(L'.');if(dot==std::wstring::npos||!Equal(path.substr(dot),L".zip"))return PackProbe::NotPack;
    auto file=Pin(path);if(!file){error=L"Cannot pin the selected modpack archive.";return PackProbe::Failed;}
    // Ordinary ZIPs may require the existing ZIP64/7-Zip engine. A probe must
    // not take that fallback away before a supported whole-pack is identified.
    coop_zip_file::Archive archive;std::string why;if(!archive.Open(file->value,why,cancel)){error.clear();return PackProbe::NotPack;}
    const std::string prefix="underground2.net_1.0.3/1 - mod files/";
    bool known=false;for(const auto& entry:archive.Entries())if(coop_mods::Lower(entry.path).rfind(prefix,0)==0){known=true;break;}
    if(!known)return PackProbe::NotPack;
    plan.archive=path;plan.name=L"underground2.net 1.0.3";
    if(!PackArchiveHash(file->value,plan.hash,cancel)){error=L"Cannot fingerprint the complete modpack archive.";return PackProbe::Failed;}
    std::map<std::string,coop_packages::Manifest> packages;std::set<std::string> modules,destinations;
    for(const auto& entry:archive.Entries()){
        if(cancel&&cancel->load()){error=L"Modpack inspection cancelled.";return PackProbe::Failed;}
        if(entry.directory)continue;const auto lower=coop_mods::Lower(entry.path);
        if(lower.rfind(prefix,0)!=0)continue;const auto destination=lower.substr(prefix.size()),ext=coop_packages::detail::Extension(destination);
        if(destination=="dinput8.dll"||destination.rfind("savegames/",0)==0||ext==".bacc"||ext==".log"||ext==".url")continue;
        std::string canonical;if(!coop_packages::CanonicalPath(destination,canonical)||canonical!=destination||
            coop_packages::detail::ReservedDestination(destination)||!destinations.insert(destination).second){error=L"Unsafe or conflicting modpack destination: "+Wide(destination);return PackProbe::Failed;}
        const bool module=ext==".asi";auto owner=PackNativeOwner(destination);
        if(module&&(owner.empty()||!modules.insert(owner).second)){error=L"Unrecognized or repeated native plugin in this modpack.";return PackProbe::Failed;}
        if(!module&&!coop_packages::detail::Asset(destination)){error=L"Unsupported modpack resource: "+Wide(destination);return PackProbe::Failed;}
        const auto slash=destination.find('/');if(slash==std::string::npos){error=L"Unexpected root-level runtime file: "+Wide(destination);return PackProbe::Failed;}
        const auto first=destination.substr(0,slash);
        if(first!="cars"&&first!="global"&&first!="frontend"&&first!="languages"&&first!="scripts"&&first!="sound"&&first!="tracks"&&first!="texturepacks"){
            error=L"Unrecognized runtime tree: "+Wide(first);return PackProbe::Failed;}
        if(owner.empty()){if(first=="scripts"){error=L"Unassigned script sidecar in this modpack: "+Wide(destination);return PackProbe::Failed;}owner="ug2net-103-"+first;}
        auto& package=packages[owner];if(package.id.empty()){
            package.id=owner;package.name=first=="scripts"?owner:"underground2.net - "+first;package.version="1.0.3";
            package.kind=first=="scripts"?coop_packages::Kind::Dependency:coop_packages::Kind::Assets;
            if(owner=="nfsu2-unlimiter")package.version="3.1.1.1337";
            if(const auto* knownNative=StandaloneNative(owner))package.version=knownNative->version;
        }
        PackArchiveItem item{entry.index,owner,destination,"files/"+destination,entry.bytes,module,ext==".ini"};
        if(package.kind==coop_packages::Kind::Assets)package.files.push_back({destination,item.source});
        if(module){std::string bytes;std::vector<std::string> dependencies;
            if(!archive.Extract(entry.index,bytes,why)||!coop_import::PE(bytes,dependencies)){error=L"Invalid x86 native plugin: "+Wide(destination);return PackProbe::Failed;}
            plan.nativeModules.push_back(Wide(destination));
        }
        if(destination=="scripts/nfsu2extraoptionssettings.ini"){
            std::string bytes;if(!archive.Extract(entry.index,bytes,why)||!PackExtraOptions(bytes,error))return PackProbe::Failed;
        }
        plan.bytes+=entry.bytes;plan.items.push_back(std::move(item));
    }
    if(modules.size()!=10||!destinations.count("texturepacks/underground2.net/textures.bin")||!destinations.count("global/globalb.lzc")){
        error=L"The archive is missing required underground2.net 1.0.3 components.";return PackProbe::Failed;}
    for(auto& package:packages){
        if(package.first=="ug2net-103-cars"||package.first=="ug2net-103-global"||package.first=="nfsu2-stance-mod")
            package.second.requires.push_back({"nfsu2-unlimiter","3.1.1.1337"});
        if(package.first=="texwizard")package.second.requires.push_back({"ug2net-103-texturepacks","1.0.3"});
        if(package.first=="nfsu2-car-sound-tuner")package.second.requires.push_back({"ug2net-103-sound","1.0.3"});
        coop_packages::Error check;if(!coop_packages::ValidateManifest(package.second,check)){
            error=L"The modpack package layout is invalid: "+Wide(check.detail);return PackProbe::Failed;}
        plan.packages.push_back(std::move(package.second));}
    plan.files=plan.items.size();
    plan.notes={L"Imports the main 1.0.3 configuration into a new inactive Modpacks instance.",
        L"Keeps all ten native plugins with their module-relative sidecars. Native-code approval is required before Play.",
        L"F5 Unlock All and F6 AutoDrive are disabled in the imported Extra Options INI; Gameplay Unlock All is off for the shared career.",
        L"The existing ASI loader, original game assets, current mod selection and saves are not replaced.",
        L"Optional variants, bundled save settings, backup files and logs are excluded. Current careers are copied into the new instance.",
        L"Your teammate must import the same archive and activate the same setup. Final in-game testing is required."};
    return PackProbe::Ready;
}
inline bool PackWriteFile(const std::wstring& root,const std::string& relative,const std::string& bytes,std::wstring& error){
    std::string canonical;if(!coop_packages::CanonicalPath(relative,canonical)||canonical!=relative){error=L"Unsafe modpack staging resource.";return false;}
    const auto path=root+L"\\"+Disk(Wide(relative));if(path.size()>=MAX_PATH){error=L"The imported path is too long; move the game to a shorter folder.";return false;}
    size_t at=root.size()+1;for(;;){const auto slash=path.find(L'\\',at);if(slash==std::wstring::npos)break;
        const auto directory=path.substr(0,slash);if(!ModpackDirectory(directory)){error=state.error;return false;}at=slash+1;}
    auto parent=Pin(path.substr(0,path.find_last_of(L'\\')),true);if(!parent){error=L"The modpack staging parent changed.";return false;}
    Handle out(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(out.value==INVALID_HANDLE_VALUE){error=L"Cannot create a new modpack resource.";return false;}
    size_t written=0;while(written<bytes.size()){const auto count=DWORD((std::min)(size_t(65536),bytes.size()-written));DWORD n=0;
        if(!WriteFile(out.value,bytes.data()+written,count,&n,nullptr)||n!=count){error=L"Cannot finish the new modpack resource.";return false;}written+=n;}
    if(!FlushFileBuffers(out.value)){error=L"Cannot flush the new modpack resource.";return false;}return true;
}
inline bool CommitModpackArchive(const std::wstring& root,const PackArchivePlan& reviewed,std::wstring& created,std::wstring& error,
    const std::atomic<bool>* cancel=nullptr,const std::function<void(const std::wstring&)>& progress={},
    bool (*snapshot)(const std::wstring&,const std::wstring&,const std::atomic<bool>*)=&SnapshotModpackCareers){
    created.clear();error.clear();if(state.ready||state.fatal||!Equal(root,state.root)||!coop_import::Hash(reviewed.hash)||reviewed.items.empty()||!ModpackWorkspace()){
        error=L"Modpack import is only available in the pre-game workspace.";return false;}
    PackArchivePlan verified;if(InspectModpackArchive(reviewed.archive,verified,error,cancel)!=PackProbe::Ready||verified.hash!=reviewed.hash){
        if(error.empty())error=L"The archive changed after review; choose it again.";return false;}
    const auto& plan=verified;
    std::vector<ModpackInfo> existing;if(!ListModpacks(existing)||existing.size()>=128){error=L"Cannot add another modpack to this workspace.";return false;}
    auto input=Pin(plan.archive);std::string hash;if(!input||!PackArchiveHash(input->value,hash,cancel)||hash!=plan.hash){error=L"The archive changed after review; choose it again.";return false;}
    coop_zip_file::Archive archive;std::string why;if(!archive.Open(input->value,why,cancel)){error=Wide(why);return false;}
    const auto id=L"ug2net-103-"+Wide(plan.hash.substr(0,8)),revision=std::wstring(L"v1");
    const auto target=ModpackHome()+L"\\"+id,pending=ModpackHome()+L"\\"+ModpackTokenNew(L"pending-");
    if(GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES){error=L"This archive is already in Modpacks. Activate or edit the existing instance.";return false;}
    auto mods=Pin(root+L"\\mods",true),owner=Pin(root+L"\\mods\\_undercoop",true),home=Pin(ModpackHome(),true);
    if(!mods||!owner||!home||!CreateDirectoryW(pending.c_str(),nullptr)){error=L"Cannot create modpack staging.";return false;}
    struct Cleanup {std::wstring path,boundary;bool keep=false;~Cleanup(){if(!keep)ModpackRemovePending(path,boundary);}} cleanup{pending,ModpackHome()};
    const auto versionRoot=pending+L"\\"+revision,packagesRoot=versionRoot+L"\\mods";
    if(!ModpackDirectory(versionRoot)||!ModpackDirectory(packagesRoot)){error=state.error;return false;}
    std::map<std::string,coop_import::Receipt> receipts;
    for(const auto& package:plan.packages){if(!ModpackDirectory(packagesRoot+L"\\"+Wide(package.id))){error=state.error;return false;}
        if(package.kind==coop_packages::Kind::Dependency)receipts[package.id]={package.id,package.version,plan.hash,{}};}
    size_t number=0;
    for(const auto& item:plan.items){
        if(cancel&&cancel->load()){error=L"Modpack import cancelled; no instance was published.";return false;}
        if(progress&&(number%25==0))progress(L"Importing underground2.net: "+std::to_wstring(number)+L" / "+std::to_wstring(plan.files)+L" files...");++number;
        std::string bytes;if(!archive.Extract(item.index,bytes,why)){error=Wide(why);return false;}
        if(item.destination=="scripts/nfsu2extraoptionssettings.ini"&&!PackExtraOptions(bytes,error))return false;
        const auto digest=ImportHash(bytes);if(digest.empty()){error=L"Cannot hash an imported resource.";return false;}
        if(!PackWriteFile(packagesRoot+L"\\"+Wide(item.owner),item.source,bytes,error))return false;
        if(const auto receipt=receipts.find(item.owner);receipt!=receipts.end())receipt->second.files.push_back({item.source,item.destination,digest,uint64_t(bytes.size()),item.module,item.config});
    }
    std::vector<std::string> selected;
    for(const auto& package:plan.packages){
        if(!PackWriteFile(packagesRoot+L"\\"+Wide(package.id),"undercoop.mod.ini",ImportManifest(package),error))return false;
        if(const auto receipt=receipts.find(package.id);receipt!=receipts.end()){
            if(!coop_import::Valid(receipt->second)){error=L"The imported native component receipt is invalid.";return false;}
            if(!PackWriteFile(packagesRoot+L"\\"+Wide(package.id),"undercoop.native.txt",coop_import::Encode(receipt->second),error))return false;
        }selected.push_back(package.id);
    }
    // Author credits are retained as metadata, never executable shortcuts.
    const std::string distribution="underground2.net_1.0.3/";
    for(const auto& entry:archive.Entries()){
        if(entry.directory)continue;const auto lower=coop_mods::Lower(entry.path);
        if(lower!=distribution+"readme.txt"&&lower.rfind(distribution+"credits/",0)!=0)continue;
        if(entry.bytes>1024*1024){error=L"Unexpected oversized credit document.";return false;}
        std::string text;if(!archive.Extract(entry.index,text,why)){error=Wide(why);return false;}
        auto relative="credits/"+lower.substr(distribution.size());
        if(coop_packages::detail::Extension(relative)==".url")relative+=".txt";
        if(!PackWriteFile(versionRoot,relative,text,error))return false;
    }
    if(!PackWriteFile(versionRoot,"name.txt",coop_mod_scan::Utf8(plan.name),error)||
        !PackWriteFile(versionRoot,"selection.txt",ModpackSelectionText(selected),error)||
        !PackWriteFile(pending,"current.txt",coop_mod_scan::Utf8(revision),error))return false;
    if(cancel&&cancel->load()){error=L"Modpack import cancelled before publication.";return false;}
    if(!snapshot||!snapshot(id,pending,cancel)){error=state.error.empty()?L"Cannot preserve the current career snapshot.":state.error;return false;}
    if(cancel&&cancel->load()||!MoveFileExW(pending.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)){
        error=L"Cannot publish the imported modpack; the active setup is unchanged.";return false;}
    cleanup.keep=true;created=id;return true;
}
