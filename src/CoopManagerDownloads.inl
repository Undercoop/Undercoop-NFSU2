// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

struct DependencyRecipe{const char* id;const char* version;const wchar_t* url;const char* hash;uint64_t bytes;};
inline const DependencyRecipe* DownloadRecipe(const coop_packages::Requirement& need){
    static const DependencyRecipe recipes[]{
        {"nfsu2-extra-options","local-2f59d8f8",L"https://github.com/ExOptsTeam/NFSU2ExOpts/releases/download/v5.1.0.1340/NFSU2.ExOpts.v5.1.0.1340.zip","88125ecec733d116b5657c53d499e0bb25d3f32bf7b274f0fbf66f8d7d3e37ab",1217170},
        {"nfsu2-unlimiter","3.1.1.1337",L"https://github.com/nlgxzef/NFSU2Unlimiter/releases/download/v3.1.1.1337/NFSU2.Unlimiter.v3.3.1.1337.zip","f0dd09098dcb0eaecfd61b15e9268445842692bcf6b4a1b5b6b50ee645ede12f",1315406}
    };for(const auto& recipe:recipes)if(need.id==recipe.id&&need.version==recipe.version)return &recipe;return nullptr;
}
struct WebHandle{HINTERNET value=nullptr;explicit WebHandle(HINTERNET h):value(h){}~WebHandle(){if(value)WinHttpCloseHandle(value);}WebHandle(const WebHandle&)=delete;};
inline bool DownloadUrlAllowed(const std::wstring& url,std::wstring& host,std::wstring& path){
    URL_COMPONENTS c{};c.dwStructSize=sizeof(c);c.dwHostNameLength=c.dwUrlPathLength=c.dwExtraInfoLength=c.dwUserNameLength=c.dwPasswordLength=DWORD(-1);
    if(url.size()>8192||url.find(L'#')!=std::wstring::npos||!WinHttpCrackUrl(url.c_str(),DWORD(url.size()),0,&c)||c.nScheme!=INTERNET_SCHEME_HTTPS||c.nPort!=443||c.dwUserNameLength||c.dwPasswordLength)return false;
    host.assign(c.lpszHostName,c.dwHostNameLength);path.assign(c.lpszUrlPath,c.dwUrlPathLength);if(c.dwExtraInfoLength)path.append(c.lpszExtraInfo,c.dwExtraInfoLength);
    return Equal(host,L"github.com")||Equal(host,L"release-assets.githubusercontent.com")||Equal(host,L"objects.githubusercontent.com")||Equal(host,L"builds.dotnet.microsoft.com");
}
using DownloadProgress=std::function<void(const std::wstring&)>;
using FetchDependency=bool(*)(const DependencyRecipe&,std::string&,std::wstring&,const std::atomic<bool>*,const DownloadProgress&);
inline bool DownloadHTTPS(const DependencyRecipe& recipe,std::string& output,std::wstring& error,const std::atomic<bool>* cancel,const DownloadProgress& progress){
    output.clear();WebHandle session(WinHttpOpen(L"Undercoop/6.12.13",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if(!session.value||!WinHttpSetTimeouts(session.value,5000,5000,5000,5000)){error=L"Cannot initialize the HTTPS downloader.";return false;}
    std::wstring url=recipe.url;const auto started=GetTickCount64();
    for(unsigned redirects=0;redirects<6;++redirects){
        if((cancel&&cancel->load())||GetTickCount64()-started>120000){error=L"Download cancelled or timed out.";return false;}
        std::wstring host,path;if(!DownloadUrlAllowed(url,host,path)){error=L"The download or redirect is not an approved HTTPS source.";return false;}
        WebHandle connection(WinHttpConnect(session.value,host.c_str(),443,0));WebHandle request(connection.value?WinHttpOpenRequest(connection.value,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr);
        DWORD disable=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_AUTHENTICATION|WINHTTP_DISABLE_COOKIES;
        if(!request.value||!WinHttpSetOption(request.value,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable))||
            !WinHttpSendRequest(request.value,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request.value,nullptr)){
            error=L"HTTPS download failed. Check the connection; no package was installed.";return false;}
        DWORD status=0,size=sizeof(status);if(!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)){error=L"Cannot read the HTTPS response status.";return false;}
        if(status==301||status==302||status==303||status==307||status==308){wchar_t location[8192]{};size=sizeof(location);
            if(!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_LOCATION,WINHTTP_HEADER_NAME_BY_INDEX,location,&size,WINHTTP_NO_HEADER_INDEX)){error=L"Invalid download redirect.";return false;}
            url=location;continue;}
        if(status!=200){error=L"The official source returned HTTP "+std::to_wstring(status)+L". No package was installed.";return false;}
        DWORD length=0;size=sizeof(length);if(WinHttpQueryHeaders(request.value,WINHTTP_QUERY_CONTENT_LENGTH|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&length,&size,WINHTTP_NO_HEADER_INDEX)&&length!=recipe.bytes){error=L"The official download size changed; a catalog update is required.";return false;}
        std::string bytes;bytes.reserve(size_t(recipe.bytes));std::array<char,65536> block{};
        for(;;){if((cancel&&cancel->load())||GetTickCount64()-started>120000){error=L"Download cancelled or timed out.";return false;}
            DWORD got=0;if(!WinHttpReadData(request.value,block.data(),DWORD(block.size()),&got)){error=L"The download was interrupted.";return false;}if(!got)break;
            if(got>recipe.bytes-bytes.size()){error=L"The download exceeded its expected size.";return false;}bytes.append(block.data(),got);
            if(progress)progress(L"Downloading "+Wide(recipe.id)+L"  "+std::to_wstring(bytes.size()*100/recipe.bytes)+L"%");
        }
        if(bytes.size()!=recipe.bytes||ImportHash(bytes)!=recipe.hash){error=L"The downloaded file failed SHA-256 verification. It was not installed.";return false;}
        output.swap(bytes);return true;
    }error=L"Too many download redirects.";return false;
}
inline bool InstallBinaryRuntime(const std::wstring& root,std::wstring& error,const std::atomic<bool>* cancel,const DownloadProgress& progress,FetchDependency fetch=&DownloadHTTPS){
    static const DependencyRecipe recipe{"Microsoft .NET 8 x86","8.0.32",L"https://builds.dotnet.microsoft.com/dotnet/Runtime/8.0.32/dotnet-runtime-8.0.32-win-x86.zip","1a486d68c6823a7c8ae2a811fcc5090cae3b57185e67050391378666b23f1d35",30578936};
    if(!BinaryRuntime(root).empty()){std::vector<std::unique_ptr<Handle>> pins;return BinaryPinRuntime(root,BinaryRuntime(root),pins,error);}
    const auto owner=root+L"\\mods\\_undercoop";if(!ImportDirectory(owner,L"downloads",error)||!ImportDirectory(owner,L"runtimes",error))return false;
    const auto cache=owner+L"\\downloads\\"+Wide(recipe.hash)+L".zip";std::string bytes;
    if(GetFileAttributesW(cache.c_str())!=INVALID_FILE_ATTRIBUTES){if(!ReadText(cache,bytes,64*1024*1024)||bytes.size()!=recipe.bytes||ImportHash(bytes)!=recipe.hash){error=L"The cached runtime failed verification. It was not used.";return false;}}
    else {if(!fetch||!fetch(recipe,bytes,error,cancel,progress)||bytes.size()!=recipe.bytes||ImportHash(bytes)!=recipe.hash){if(error.empty())error=L"The runtime download failed verification.";return false;}if(!ImportWriteNew(owner,"downloads/"+std::string(recipe.hash)+".zip",bytes,error))return false;}
    const auto target=owner+L"\\runtimes\\dotnet-8.0.32-x86";if(GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES){error=L"A private runtime folder already exists but is incomplete. It was not overwritten.";return false;}
    const auto relative=L"runtimes\\pending-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());if(!ImportDirectory(owner,relative,error))return false;const auto stage=owner+L"\\"+relative;
    coop_zip::Archive zip;std::string why;if(!zip.Open(std::move(bytes),why)){error=Wide(why);return false;}
    std::string expected;if(!BinaryResource(329,expected)){error=L"The private runtime manifest is missing.";return false;}std::map<std::string,std::pair<std::string,uint64_t>> wanted;
    std::istringstream in(expected);std::string line;std::getline(in,line);while(std::getline(in,line)){auto a=BinaryFields(line);uint64_t n=0;if(a.size()!=3||!coop_import::Number(a[2],n)||!wanted.emplace(a[0],std::make_pair(a[1],n)).second){error=L"Invalid runtime manifest.";return false;}}
    size_t count=0;for(const auto& e:zip.Entries()){if(e.directory)continue;if(cancel&&cancel->load()){error=L"Runtime installation cancelled before publication.";return false;}auto path=coop_mods::Lower(e.path);if(path=="shared/microsoft.netcore.app/8.0.32/.version")continue;
        auto check=wanted.find(path);std::string data;if(check==wanted.end()||!zip.Extract(e.index,data,why)||data.size()!=check->second.second||ImportHash(data)!=check->second.first||!ImportWriteNew(stage,path,data,error)){if(error.empty())error=L"Runtime content verification failed.";return false;}++count;}
    if(count!=wanted.size()||(cancel&&cancel->load())){error=L"The runtime is incomplete or installation was cancelled.";return false;}
    auto parent=Pin(owner+L"\\runtimes",true);if(!parent||!MoveFileExW(stage.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)){error=L"Cannot publish the verified private runtime. Existing data retained.";return false;}
    std::vector<std::unique_ptr<Handle>> pins;return BinaryPinRuntime(root,target+L"\\dotnet.exe",pins,error);
}
inline bool BuildDependencyPlan(const DependencyRecipe& recipe,const std::wstring& archive,const std::string& bytes,ImportPlan& plan,std::wstring& error){
    plan={};plan.archive=archive;plan.hash=recipe.hash;plan.package.id=recipe.id;plan.package.version=recipe.version;plan.package.kind=coop_packages::Kind::Dependency;
    plan.package.name=plan.package.id=="nfsu2-unlimiter"?"NFSU2 Unlimiter":"NFSU2 ExtraOptions";
    if(bytes.size()!=recipe.bytes||ImportHash(bytes)!=recipe.hash){error=L"Dependency archive verification failed.";return false;}
    coop_zip::Archive zip;std::string why;if(!zip.Open(bytes,why)){error=Wide(why);return false;}
    std::vector<std::string> wanted=plan.package.id=="nfsu2-unlimiter"?std::vector<std::string>{"nfsu2unlimiter.asi","nfsu2unlimitersettings.ini","unlimiterdata/_fngfixes.ini","unlimiterdata/_general.ini","unlimiterdata/_rimbrands.ini"}:
        std::vector<std::string>{"nfsu2extraoptions.asi","nfsu2extraoptionssettings.ini"};
    for(const auto& relative:wanted){bool found=false;for(const auto& e:zip.Entries())if(!e.directory){const auto path=coop_mods::Lower(e.path),needle="scripts/"+relative;
                if(path!=needle&&(path.size()<=needle.size()||path.substr(path.size()-needle.size()-1)!="/"+needle))continue;
                if(found){error=L"Ambiguous dependency archive.";return false;}std::string content;if(!zip.Extract(e.index,content,why)){error=Wide(why);return false;}
                const auto hash=ImportHash(content);if(relative==wanted.front()&&hash!=(plan.package.id=="nfsu2-unlimiter"?"ddbaadeb70e517a493835b3aa60b0d46e8dece30fe6beda6efa4c7fef553c11b":"2f59d8f82c69ab2b8102e4041dc04a8c7ea9440e597eb1222a41889cfbe547ae")){
                    error=L"Dependency binary differs from its tested adapter.";return false;}
                plan.items.push_back({e.index,relative,"scripts/"+relative,hash,e.bytes});plan.bytes+=e.bytes;found=true;
            }if(!found){error=L"A required dependency resource is missing: "+Wide(relative);return false;}
    }
    if(plan.package.id=="nfsu2-unlimiter"){
        // The existing tested 3.1.1 adapter includes this exact local frontend
        // profile. Keep the same complete configuration and receipt on both
        // fresh downloads and existing installations; do not mutate an older
        // package later when an addon is imported.
        ImportItem item;item.index=SIZE_MAX;item.source="unlimiterdata/bmwm3gtr.ini";item.destination="scripts/unlimiterdata/bmwm3gtr.ini";
        item.generated="// Isolated BMW test pack only. Do not unlock every car or change any save.\n[Main]\nInitiallyUnlocked = 1 // Register this addon in Unlimiter's frontend boot-unlocked lists.\n\n[CarRenderInfo]\nLinkLicensePlateToTrunk = 1\nShowTrunkUnderInFE = 1\nRemoveCentreBrakeWithCustomSpoiler = 0\nHasSunroof = 0\n";
        item.hash=ImportHash(item.generated);item.bytes=item.generated.size();
        if(item.hash!="49cd517e33753537419f7f1c4fc325525b03ff61dd76a0b4a250dea6357b52ee"){error=L"The built-in adapter configuration failed verification.";return false;}
        plan.bytes+=item.bytes;plan.items.push_back(std::move(item));
    }
    plan.manifestText=ImportManifest(plan.package);return true;
}
inline bool ImportRequirements(const std::wstring& root,const ImportPlan& plan,const std::vector<coop_packages::Manifest>& catalog,bool allowDownloads,std::wstring& error,
    const std::atomic<bool>* cancel,const DownloadProgress& progress,FetchDependency fetch=&DownloadHTTPS){
    std::vector<const DependencyRecipe*> missing;
    for(const auto& need:plan.package.requires){
        const auto found=std::find_if(catalog.begin(),catalog.end(),[&](const auto& p){return p.id==need.id;});
        if(found!=catalog.end()){if(found->version!=need.version){error=L"Installed requirement has another version: "+Wide(need.id)+L". Existing packages are never silently replaced.";return false;}continue;}
        const auto* recipe=DownloadRecipe(need);if(!recipe){error=L"No trusted download recipe exists for "+Wide(need.id)+L" "+Wide(need.version)+L". Nothing was installed.";return false;}
        if(!allowDownloads){error=L"Enable automatic requirement downloads or install "+Wide(need.id)+L" first.";return false;}missing.push_back(recipe);
    }
    for(const auto* recipe:missing){if(cancel&&cancel->load()){error=L"Import cancelled.";return false;}
        if(progress)progress(L"Checking required component: "+Wide(recipe->id));
        const auto owner=root+L"\\mods\\_undercoop";if(!ImportDirectory(owner,L"downloads",error))return false;
        const auto cache=owner+L"\\downloads\\"+Wide(recipe->hash)+L".zip";std::string bytes;
        if(GetFileAttributesW(cache.c_str())!=INVALID_FILE_ATTRIBUTES){if(!ReadText(cache,bytes,128*1024*1024)||bytes.size()!=recipe->bytes||ImportHash(bytes)!=recipe->hash){error=L"A cached requirement is corrupt. It was not used.";return false;}}
        else{if(!fetch||!fetch(*recipe,bytes,error,cancel,progress))return false;
            if(bytes.size()!=recipe->bytes||ImportHash(bytes)!=recipe->hash){error=L"Requirement download verification failed.";return false;}
            if(!ImportWriteNew(owner,"downloads/"+std::string(recipe->hash)+".zip",bytes,error))return false;}
        ImportPlan dependency;if(!BuildDependencyPlan(*recipe,cache,bytes,dependency,error))return false;
        if(!ImportCommit(root,dependency,error,cancel))return false;
    }return true;
}
inline bool RepairRequirements(const std::wstring& root,const std::vector<coop_packages::Manifest>& catalog,const std::vector<std::string>& selected,std::wstring& error,
    const std::atomic<bool>* cancel,const DownloadProgress& progress,FetchDependency fetch=&DownloadHTTPS){
    ImportPlan needs;std::vector<std::string> queue=selected;std::set<std::string> visited;std::map<std::string,std::string> versions;
    for(size_t at=0;at<queue.size();++at){if(queue.size()>512){error=L"Requirement graph limit exceeded.";return false;}const auto id=queue[at];if(!visited.insert(id).second)continue;
        const auto p=std::find_if(catalog.begin(),catalog.end(),[&](const auto& value){return value.id==id;});if(p==catalog.end()){error=L"A selected package itself is missing: "+Wide(id);return false;}
        for(const auto& r:p->requires){const auto old=versions.find(r.id);if(old!=versions.end()&&old->second!=r.version){error=L"Selected packages require conflicting versions of "+Wide(r.id);return false;}
            if(old==versions.end()){versions[r.id]=r.version;needs.package.requires.push_back(r);}
            const auto installed=std::find_if(catalog.begin(),catalog.end(),[&](const auto& value){return value.id==r.id;});if(installed!=catalog.end())queue.push_back(r.id);
        }
    }return ImportRequirements(root,needs,catalog,true,error,cancel,progress,fetch);
}
