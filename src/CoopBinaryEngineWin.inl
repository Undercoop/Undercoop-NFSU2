// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Pre-launch typed Binary adapter. A separate bounded managed data worker never
// executes archive installers, shells, README URLs or native payloads.
inline bool BinaryResource(int id,std::string& bytes);
inline std::wstring BinaryHome(const std::wstring& root){
    // Engine updates are separate immutable builds. Reusing a fixed directory
    // would mistake an older legitimate engine for tampering after an update.
    static const std::string identity=[](){std::string all,bytes;for(int id=322;id<=328;++id){if(!BinaryResource(id,bytes))return std::string();all+=std::to_string(id)+"="+ImportHash(bytes)+"\n";}return ImportHash(all);}();
    return identity.empty()?std::wstring():root+L"\\mods\\_undercoop\\binary-engine\\engine-"+Wide(identity.substr(0,24));
}
inline bool BinaryResource(int id,std::string& bytes){HMODULE owner=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&BinaryResource),&owner))return false;
    auto resource=FindResourceW(owner,MAKEINTRESOURCEW(id),MAKEINTRESOURCEW(10));auto loaded=resource?LoadResource(owner,resource):nullptr;DWORD n=resource?SizeofResource(owner,resource):0;
    const auto* memory=loaded?static_cast<const char*>(LockResource(loaded)):nullptr;if(!memory||!n||n>8*1024*1024)return false;bytes.assign(memory,n);return true;
}
inline bool BinaryEngineFiles(const std::wstring& root,std::vector<std::unique_ptr<Handle>>& pins,std::wstring& error){
    const auto owner=root+L"\\mods\\_undercoop",home=BinaryHome(root);if(home.empty()){error=L"Bundled Binary support is incomplete. Reinstall Undercoop.";return false;}
    const auto relative=home.substr(owner.size()+1);if(!ImportDirectory(owner,relative,error))return false;
    for(const auto& path:{root+L"\\mods",owner,owner+L"\\binary-engine",BinaryHome(root)}){auto p=Pin(path,true);if(!p){error=L"The Binary engine folder changed or is linked.";return false;}pins.push_back(std::move(p));}
    for(const auto& file:std::initializer_list<std::pair<int,const char*>>{{322,"undercoop.binary.dll"},{323,"nikki.dll"},{324,"coreextensions.dll"},{325,"undercoop.binary.runtimeconfig.json"},{326,"nikki-license.txt"},{327,"coreextensions-license.txt"},{328,"undercoop-source.txt"}}){
        std::string bytes;if(!BinaryResource(file.first,bytes)){error=L"Bundled Binary support is incomplete. Reinstall Undercoop.";return false;}const auto path=BinaryHome(root)+L"\\"+Wide(file.second);
        if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES&&!ImportWriteNew(home,file.second,bytes,error))return false;
        auto p=Pin(path);std::string hash;uint64_t size=0;if(!p||!Digest(p->value,hash,size)||hash!=ImportHash(bytes)||size!=bytes.size()){error=L"A Binary engine file changed. It was not executed or overwritten.";return false;}pins.push_back(std::move(p));
    }return true;
}
inline std::wstring BinaryFindSystemRuntime(){
    // Use the architecture-specific registered install, never PATH or a DLL
    // supplied by an imported mod. The helper additionally runs with startup
    // hooks/profiling/additional dependency environment variables removed.
    wchar_t path[MAX_PATH]{};DWORD bytes=sizeof(path);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\dotnet\\Setup\\InstalledVersions\\x86",L"InstallLocation",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,path,&bytes)!=ERROR_SUCCESS){
        DWORD n=GetEnvironmentVariableW(L"ProgramFiles(x86)",path,MAX_PATH);if(!n||n>=MAX_PATH)return {};const auto fallback=std::wstring(path)+L"\\dotnet";if(fallback.size()>=MAX_PATH)return {};std::copy(fallback.begin(),fallback.end(),path);path[fallback.size()]=0;}
    std::wstring base=path;while(!base.empty()&&(base.back()==L'\\'||base.back()==L'/'))base.pop_back();const auto exe=base+L"\\dotnet.exe";
    if(!RealPath(exe))return {};WIN32_FIND_DATAW row{};HANDLE found=FindFirstFileW((base+L"\\shared\\Microsoft.NETCore.App\\8.0.*").c_str(),&row);if(found==INVALID_HANDLE_VALUE)return {};bool okay=false;
    do{if((row.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(row.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){const auto runtime=base+L"\\shared\\Microsoft.NETCore.App\\"+row.cFileName;okay=okay||RealPath(runtime+L"\\coreclr.dll")&&RealPath(runtime+L"\\System.Private.CoreLib.dll");}}while(FindNextFileW(found,&row));FindClose(found);return okay?exe:L"";
}
inline std::wstring(*binarySystemRuntime)()=&BinaryFindSystemRuntime;
inline std::wstring BinaryRuntime(const std::wstring& root){const auto portable=root+L"\\mods\\_undercoop\\runtimes\\dotnet-8.0.32-x86\\dotnet.exe";return RealPath(portable)?portable:binarySystemRuntime?binarySystemRuntime():L"";}
inline bool BinaryJob(const std::wstring& root,std::wstring& job,std::wstring& error){
    const auto owner=root+L"\\mods\\_undercoop";static LONG sequence=0;
    const auto relative=L"binary-jobs\\job-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(InterlockedIncrement(&sequence));
    if(!ImportDirectory(owner,relative,error))return false;job=owner+L"\\"+relative;
    for(const auto* folder:{L"input",L"base",L"output",L"scratch"})if(!CreateDirectoryW((job+L"\\"+folder).c_str(),nullptr)){error=L"Cannot prepare a new Binary data job.";return false;}return true;
}
inline bool BinaryBase(const std::wstring& root,const std::wstring& job,const std::vector<Mapping>& effective,std::wstring& error){
    std::vector<std::string> names{"global/globalb.lzc","frontend/frontb.lzc","global/carheadersmemoryfile.bin","global/frontendmemoryfile.bin","global/ingamememoryfile.bin","global/permanentmemoryfile.bin","global/globalmemoryfile.bin"};
    for(const auto* lang:{"english","spanish","chinese","danish","dutch","french","german","italian","swedish","korean","japanese","polish","russian"})names.push_back("languages/"+std::string(lang)+".bin");
    for(const auto& name:names){auto source=root+L"\\"+Disk(Wide(name));for(const auto& m:effective)if(m.relative==name)source=m.physical;
        if(GetFileAttributesW(source.c_str())==INVALID_FILE_ATTRIBUTES)continue;std::string bytes;if(!ReadText(source,bytes,64*1024*1024)||!ImportWriteNew(job,"base/"+name,bytes,error)){if(error.empty())error=L"Cannot snapshot the effective Binary database.";return false;}}
    return true;
}
inline bool BinaryCurrentBase(const std::wstring& root,std::vector<Mapping>& result,std::wstring& error){
    if(!Equal(root,state.root)){error=L"The active mod workspace changed.";return false;}
    coop_packages::Plan planned;coop_packages::Error why;if(!coop_packages::BuildPlan(state.catalog,state.selected,planned,why)){error=Wide(why.detail);return false;}
    for(const auto& file:planned.Index()){Mapping m;m.relative=file.destination;m.physical=PackageDirectory(file.owner)+L"\\"+Disk(Wide(file.source));result.push_back(std::move(m));}return true;
}
inline std::vector<std::string> BinaryFields(const std::string&);
inline bool BinaryPinRuntime(const std::wstring& root,const std::wstring& exe,std::vector<std::unique_ptr<Handle>>& pins,std::wstring& error){
    auto executable=Pin(exe);if(!executable){error=L"The selected x86 runtime is linked or unavailable.";return false;}
    const auto portable=root+L"\\mods\\_undercoop\\runtimes\\dotnet-8.0.32-x86";
    if(Equal(exe,portable+L"\\dotnet.exe")){
        std::string manifest;if(!BinaryResource(329,manifest)){error=L"The private runtime verification manifest is unavailable.";return false;}
        std::istringstream rows(manifest);std::string line;std::getline(rows,line);size_t count=0;std::set<std::wstring> parents;
        while(std::getline(rows,line)){auto a=BinaryFields(line);std::string path;uint64_t expected=0;if(a.size()!=3||!coop_packages::CanonicalPath(a[0],path)||path!=a[0]||!coop_import::Hash(a[1])||!coop_import::Number(a[2],expected)||++count>512){error=L"Invalid private runtime verification manifest.";return false;}
            const auto disk=portable+L"\\"+Disk(Wide(path));for(size_t at=portable.size();at<disk.size();++at)if(disk[at]==L'\\')parents.insert(disk.substr(0,at));
            auto p=Pin(disk);std::string hash;uint64_t n=0;if(!p||!Digest(p->value,hash,n)||hash!=a[1]||n!=expected){error=L"The private .NET runtime changed or is incomplete. It was not executed.";return false;}pins.push_back(std::move(p));
        }
        for(const auto& parent:parents){auto p=Pin(parent,true);if(!p){error=L"The private runtime directory changed.";return false;}pins.push_back(std::move(p));}
    }else{
        WINTRUST_FILE_INFO file{};file.cbStruct=sizeof(file);file.pcwszFilePath=exe.c_str();WINTRUST_DATA data{};data.cbStruct=sizeof(data);data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_NONE;data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
        GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;LONG trust=WinVerifyTrust(nullptr,&policy,&data);data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&policy,&data);
        std::string bytes;if(trust!=ERROR_SUCCESS||!ReadText(exe,bytes,4*1024*1024)||bytes.size()<64||bytes.compare(0,2,"MZ")||coop_import::U32(bytes,60)>bytes.size()-24||coop_import::U16(bytes,coop_import::U32(bytes,60)+4)!=0x14c){error=L"The registered .NET executable is not a signed x86 runtime. It was not executed.";return false;}
    }
    pins.push_back(std::move(executable));return true;
}
inline bool BinaryRun(const std::wstring& root,const std::wstring& job,const wchar_t* mode,std::wstring& error,const std::atomic<bool>* cancel=nullptr){
    std::vector<std::unique_ptr<Handle>> pins;if(!BinaryEngineFiles(root,pins,error))return false;const auto runtime=BinaryRuntime(root);auto directory=Pin(job,true);if(runtime.empty()||!directory){error=L"Binary installation requires .NET 8 x86. Choose Get required runtime to install its private copy.";return false;}if(!BinaryPinRuntime(root,runtime,pins,error))return false;
    // Exclude process-start hooks, profilers, extra deps and command search
    // overrides. No untrusted script path can become an application argument.
    std::map<std::wstring,std::wstring> env;LPWCH existing=GetEnvironmentStringsW();if(!existing){error=L"Cannot prepare the Binary worker environment.";return false;}
    for(const wchar_t* p=existing;*p;p+=std::wcslen(p)+1){std::wstring entry=p;auto at=entry.find(L'=');if(at==std::wstring::npos||at==0)continue;auto key=entry.substr(0,at),upper=key;for(auto& c:upper)c=wchar_t(std::towupper(c));
        if(upper.rfind(L"DOTNET_",0)==0||upper.rfind(L"COMPLUS_",0)==0||upper.rfind(L"CORECLR_",0)==0||upper.rfind(L"COR_",0)==0||upper==L"PATH"||upper==L"TEMP"||upper==L"TMP")continue;env[key]=entry.substr(at+1);}FreeEnvironmentStringsW(existing);
    env[L"DOTNET_MULTILEVEL_LOOKUP"]=L"0";env[L"DOTNET_EnableDiagnostics"]=L"0";env[L"DOTNET_ROLL_FORWARD"]=L"LatestPatch";env[L"DOTNET_GCHeapHardLimit"]=L"20000000";env[L"TEMP"]=job+L"\\scratch";env[L"TMP"]=job+L"\\scratch";
    wchar_t system[MAX_PATH]{};if(!GetSystemDirectoryW(system,MAX_PATH)){error=L"Cannot locate Windows dependencies.";return false;}env[L"PATH"]=system;
    std::wstring block;for(const auto& item:env){block+=item.first+L"="+item.second;block.push_back(0);}block.push_back(0);
    std::wstring command=L"\""+runtime+L"\" exec --runtimeconfig \""+BinaryHome(root)+L"\\undercoop.binary.runtimeconfig.json\" \""+BinaryHome(root)+L"\\undercoop.binary.dll\" "+mode+L" \""+job+L"\"";
    Handle processJob(CreateJobObjectW(nullptr,nullptr));JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_ACTIVE_PROCESS;limits.ProcessMemoryLimit=768ull*1024*1024;limits.BasicLimitInformation.ActiveProcessLimit=1;
    if(processJob.value==INVALID_HANDLE_VALUE||!processJob.value||!SetInformationJobObject(processJob.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){error=L"Cannot bound the Binary data worker.";return false;}
    STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION process{};
    if(!CreateProcessW(runtime.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT,block.data(),job.c_str(),&start,&process)){error=L"Could not start the Binary data worker.";return false;}
    Handle thread(process.hThread),running(process.hProcess);if(!AssignProcessToJobObject(processJob.value,process.hProcess)){TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,5000);error=L"Could not isolate the Binary data worker limits.";return false;}ResumeThread(process.hThread);
    const auto began=GetTickCount64();while(WaitForSingleObject(process.hProcess,50)==WAIT_TIMEOUT){if((cancel&&cancel->load())||GetTickCount64()-began>90000){TerminateJobObject(processJob.value,1);WaitForSingleObject(process.hProcess,5000);error=L"Binary preparation cancelled or timed out. No game assets or careers were replaced.";return false;}}
    DWORD code=1;if(!GetExitCodeProcess(process.hProcess,&code)||code){std::string reason;if(ReadText(job+L"\\error.txt",reason,16384))error=Wide(reason);else error=L"Binary preparation failed within its limits. No package was activated.";return false;}return true;
}
inline std::vector<std::string> BinaryFields(const std::string& s){std::vector<std::string> out;size_t at=0;for(;;){auto end=s.find('|',at);out.push_back(s.substr(at,end==std::string::npos?end:end-at));if(end==std::string::npos)return out;at=end+1;}}
inline bool InspectBinaryArchive(coop_archive::Archive& zip,const std::wstring& root,ImportPlan& plan,std::wstring& error,const std::atomic<bool>* cancel,const std::string& selectedLauncher){
    plan.binary=true;if(root.empty()){error=L"Binary installers need the launcher's private mods workspace.";return false;}
    if(BinaryRuntime(root).empty()){plan.runtimeMissing=true;plan.package.name="Binary installation support";plan.package.id="binary-runtime-required";plan.package.version="1";plan.notes.push_back(L"This archive requires .NET 8 x86 for its data conversion. Download and private installation require your approval.");return true;}
    std::wstring job;if(!BinaryJob(root,job,error))return false;
    for(const auto& entry:zip.Entries()){if(cancel&&cancel->load()){error=L"Binary inspection cancelled.";return false;}if(entry.directory)continue;auto name=coop_mods::Lower(entry.path);std::string bytes,why;if(!zip.Extract(entry.index,bytes,why)||!ImportWriteNew(job,"input/"+name,bytes,error)){if(error.empty())error=Wide(why);return false;}}
    if(!selectedLauncher.empty()){std::string canonical;if(!coop_packages::CanonicalPath(selectedLauncher,canonical)||canonical!=selectedLauncher||coop_packages::detail::Extension(canonical)!=".end"||
        !ImportWriteNew(job,"selected-launcher.txt",selectedLauncher,error))return false;}
    std::vector<Mapping> base;if(!BinaryCurrentBase(root,base,error)||!BinaryBase(root,job,base,error))return false;
    if(!BinaryRun(root,job,L"inspect",error,cancel)){
        std::string choices;if(!selectedLauncher.empty()||!ReadText(job+L"\\launchers.txt",choices,65536)||choices.rfind("Undercoop Binary launcher choices 1\n",0)!=0)return false;
        std::istringstream rows(choices);std::string row;std::getline(rows,row);std::set<std::string> unique;
        while(std::getline(rows,row)){auto fields=BinaryFields(row);std::string canonical;
            if(fields.size()!=2||fields[0]!="launcher"||!coop_packages::CanonicalPath(fields[1],canonical)||canonical!=fields[1]||
                coop_packages::detail::Extension(canonical)!=".end"||!unique.insert(canonical).second||unique.size()>16){error=L"Invalid Binary installation choices.";return false;}
            bool exists=false;for(const auto& entry:zip.Entries())exists=exists||(!entry.directory&&coop_mods::Lower(entry.path)==canonical);
            if(!exists){error=L"Binary installation choice is missing from the archive.";return false;}plan.binaryChoices.push_back(canonical);
        }
        if(plan.binaryChoices.size()<2)return false;error.clear();plan.package.name="Choose a Binary installer";return true;
    }
    std::string text;if(!ReadText(job+L"\\plan.txt",text,256*1024)||text.rfind("Undercoop Binary data plan 1\n",0)!=0){error=L"Invalid Binary data plan.";return false;}
    auto filename=coop_archive::Stem(coop_mod_scan::Utf8(plan.archive.substr(plan.archive.find_last_of(L"\\/")+1)));std::string id;
    for(char c:coop_mods::Lower(filename))id+=((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_')?c:'-';while(!id.empty()&&(id.front()=='-'||id.front()=='_'))id.erase(id.begin());if(id.empty())id="binary-addon";if(id.size()>48)id.resize(48);
    plan.package.id=id;plan.package.name=filename;plan.package.version="binary-"+plan.hash.substr(0,12);plan.package.kind=coop_packages::Kind::Assets;
    std::map<std::string,const coop_zip::Entry*> files;for(const auto& e:zip.Entries())if(!e.directory)files[coop_mods::Lower(e.path)]=&e;
    std::string descriptor="Undercoop Binary package 1\narchive|"+plan.hash+"\n";std::set<std::string> raw,dest;
    std::istringstream in(text);std::string line;std::getline(in,line);
    auto item=[&](const std::string& path,const std::string& source,const std::string& destination)->bool{
        const auto f=files.find(path);if(f==files.end()){error=L"Binary plan refers to a missing archive file.";return false;}std::string content,why;if(!zip.Extract(f->second->index,content,why)){error=Wide(why);return false;}
        plan.items.push_back({f->second->index,source,destination,ImportHash(content),content.size()});plan.bytes+=content.size();return true;};
    while(std::getline(in,line)){auto a=BinaryFields(line);if(a[0]=="launcher"&&a.size()==2)descriptor+="launcher|"+a[1]+"\n";
        else if(a[0]=="copy"&&a.size()==3){std::string d;if(!coop_packages::CanonicalPath(a[2],d)||!coop_packages::detail::Asset(d)||!dest.insert(d).second){error=L"Unsafe or conflicting Binary runtime output.";return false;}const auto source="files/"+d;if(!item(a[1],source,d))return false;plan.package.files.push_back({d,source});}
        else if(a[0]=="raw"&&a.size()==2){std::string path;if(!coop_packages::CanonicalPath(a[1],path)||!raw.insert(path).second||!item(path,"binary/"+path,"")){error=L"Invalid Binary source path.";return false;}const auto& f=plan.items.back();descriptor+="raw|"+path+"|"+f.hash+"|"+std::to_string(f.bytes)+"\n";}
        else if(a[0]=="require"&&a.size()==3)plan.package.requires.push_back({a[1],a[2]});else if(a[0]=="note"&&a.size()==2)plan.notes.push_back(Wide(a[1]));else if(a[0]=="name"){}else{error=L"Unknown Binary plan record.";return false;}}
    // Hybrid archives can include manual runtime resources beside Binary data
    // edits. Preserve the whole mapped tree; never silently omit the car/rims.
    for(const auto& entry:zip.Entries())if(!entry.directory){const auto sourcePath=coop_mods::Lower(entry.path);if(raw.count(sourcePath))continue;
        std::string destination;if(!ImportRuntimeDestination(sourcePath,root,destination,error)){if(!error.empty())return false;continue;}
        if(dest.count(destination)){
            const auto existing=std::find_if(plan.items.begin(),plan.items.end(),[&](const auto& value){return value.destination==destination;});
            std::string content,why;if(existing==plan.items.end()||!zip.Extract(entry.index,content,why)||existing->hash!=ImportHash(content)||existing->bytes!=content.size()){
                error=L"The archive contains conflicting resources for one game destination: "+Wide(destination);return false;}
            continue;
        }
        dest.insert(destination);const auto source="files/"+destination;
        if(!item(sourcePath,source,destination))return false;plan.package.files.push_back({destination,source});
    }
    if(plan.package.files.empty())plan.package.kind=coop_packages::Kind::Data;
    coop_packages::Error why;if(raw.empty()||!coop_packages::ValidateManifest(plan.package,why)){error=L"Invalid Binary asset plan: "+Wide(why.detail);return false;}
    ImportItem receipt;receipt.index=SIZE_MAX;receipt.source="undercoop.binary.txt";receipt.generated=descriptor;receipt.hash=ImportHash(descriptor);receipt.bytes=descriptor.size();plan.bytes+=receipt.bytes;plan.items.push_back(std::move(receipt));
    plan.notes.push_back(L"Binary data is composed before launch inside mods. The active combination has its own career save area.");plan.manifestText=ImportManifest(plan.package);return true;
}
inline bool PrepareBinaryData(std::vector<Mapping>& mappings,std::vector<std::unique_ptr<Handle>>& pins,std::string& receipt){
    std::vector<std::string> addons;for(const auto& id:state.plan.Packages())if(GetFileAttributesW((PackageDirectory(id)+L"\\undercoop.binary.txt").c_str())!=INVALID_FILE_ATTRIBUTES)addons.push_back(id);std::sort(addons.begin(),addons.end());
    for(const auto& id:addons){std::wstring job,error;if(!BinaryJob(state.root,job,error)||!BinaryBase(state.root,job,mappings,error))return Fail(error);const auto folder=PackageDirectory(id);auto descriptorPin=Pin(folder+L"\\undercoop.binary.txt");std::string text;
        if(!descriptorPin||!ReadText(folder+L"\\undercoop.binary.txt",text,256*1024)||text.rfind("Undercoop Binary package 1\n",0)!=0)return Fail(L"Invalid Binary package descriptor.");receipt+="BinarySource="+id+"="+ImportHash(text)+"\n";pins.push_back(std::move(descriptorPin));
        std::istringstream in(text);std::string line,selectedLauncher;std::getline(in,line);size_t count=0;
        while(std::getline(in,line)){auto a=BinaryFields(line);if(a.size()==4&&a[0]=="raw"){std::string path;if(!coop_packages::CanonicalPath(a[1],path)||path!=a[1]||!coop_import::Hash(a[2])||++count>4096)return Fail(L"Invalid Binary source receipt.");auto source=Pin(folder+L"\\binary\\"+Disk(Wide(path)));std::string hash,bytes;uint64_t n=0,expected=0;
                if(!source||!Digest(source->value,hash,n)||!coop_import::Number(a[3],expected)||hash!=a[2]||n!=expected||!ReadText(folder+L"\\binary\\"+Disk(Wide(path)),bytes,64*1024*1024)||!ImportWriteNew(job,"input/"+path,bytes,error))return Fail(error.empty()?L"Binary source changed after installation.":error);pins.push_back(std::move(source));}
            else if(a.size()==2&&a[0]=="launcher"){std::string canonical;if(!selectedLauncher.empty()||!coop_packages::CanonicalPath(a[1],canonical)||canonical!=a[1]||coop_packages::detail::Extension(canonical)!=".end")return Fail(L"Invalid pinned Binary installer.");selectedLauncher=canonical;}
            else if(!(a.size()==2&&a[0]=="archive"))return Fail(L"Unknown Binary source receipt field.");}
        if(selectedLauncher.empty()||!ImportWriteNew(job,"selected-launcher.txt",selectedLauncher,error))return Fail(L"Cannot pin the selected Binary installer.");
        if(!count||!BinaryRun(state.root,job,L"build",error))return Fail(error);if(!ReadText(job+L"\\plan.txt",text,256*1024))return Fail(L"Cannot verify prepared Binary data.");
        std::istringstream outputs(text);while(std::getline(outputs,line)){auto a=BinaryFields(line);if(a[0]!="output")continue;if(a.size()!=4)return Fail(L"Invalid prepared Binary output.");std::string path;if(!coop_packages::CanonicalPath(a[1],path)||!coop_packages::detail::Asset(path)||!(path.rfind("global/",0)==0||path.rfind("frontend/",0)==0||path.rfind("languages/",0)==0))return Fail(L"Unsafe generated Binary destination.");
            Mapping m;m.relative=path;m.original=state.root+L"\\"+Disk(Wide(path));m.physical=job+L"\\output\\"+Disk(Wide(path));auto p=Pin(m.physical);uint64_t expected=0;
            if(!p||!Digest(p->value,m.hash,m.bytes)||m.hash!=a[2]||!coop_import::Number(a[3],expected)||m.bytes!=expected)return Fail(L"Generated Binary data changed before activation.");receipt+="BinaryData="+path+"="+m.hash+"\n";pins.push_back(std::move(p));auto old=std::find_if(mappings.begin(),mappings.end(),[&](const auto& v){return v.relative==path;});if(old==mappings.end())mappings.push_back(std::move(m));else *old=std::move(m);
        }
    }return true;
}
inline bool PrepareUnlimiterProfiles(PreparedActivation& staged,std::string& receipt){
    // Unlimiter reads model INIs beside its own module using Windows profile
    // APIs. Those reads need real private files, not just a game-path overlay.
    // Clone only the verified dependency and selected profiles into a generated
    // mods folder. Never add files to the installed dependency or stock scripts.
    auto native=std::find_if(staged.natives.begin(),staged.natives.end(),[](const auto& n){const auto slash=n.path.find_last_of(L'\\');return Equal(n.path.substr(slash+1),L"NFSU2Unlimiter.asi");});if(native==staged.natives.end())return true;
    const auto source=PackageDirectory("nfsu2-unlimiter");std::map<std::string,std::string> files;
    const auto imported=state.imported.find("nfsu2-unlimiter");
    if(imported!=state.imported.end()){
        // A reviewed whole-pack already owns a complete, module-relative
        // Unlimiter profile tree. Retain its technical and stock-car profiles.
        for(const auto& f:imported->second.files){
            if(f.destination!="scripts/nfsu2unlimiter.asi"&&f.destination!="scripts/nfsu2unlimitersettings.ini"&&f.destination.rfind("scripts/unlimiterdata/",0)!=0)continue;
            const auto name=f.destination.substr(8);std::string bytes;
            if(!ReadText(source+L"\\"+Disk(Wide(f.source)),bytes,4*1024*1024))return Fail(L"Cannot read the imported Unlimiter profile base.");files.emplace(name,std::move(bytes));
        }
        for(const auto* required:{"nfsu2unlimiter.asi","nfsu2unlimitersettings.ini","unlimiterdata/_fngfixes.ini","unlimiterdata/_general.ini","unlimiterdata/_rimbrands.ini"})
            if(!files.count(required))return Fail(L"The imported Unlimiter profile tree is incomplete.");
    }else for(const auto* name:{"nfsu2unlimiter.asi","nfsu2unlimitersettings.ini","unlimiterdata/_fngfixes.ini","unlimiterdata/_general.ini","unlimiterdata/_rimbrands.ini","unlimiterdata/bmwm3gtr.ini"}){std::string bytes;if(!ReadText(source+L"\\"+Disk(Wide(name)),bytes,4*1024*1024))return Fail(L"Cannot read the verified Unlimiter profile base.");files.emplace(name,std::move(bytes));}
    bool changed=false;for(const auto& mapping:staged.mappings){const std::string prefix="scripts/unlimiterdata/";if(mapping.relative.rfind(prefix,0)!=0)continue;const auto name=mapping.relative.substr(prefix.size());if(name.find('/')!=std::string::npos||coop_packages::detail::Extension(name)!=".ini")return Fail(L"A mod tried to replace Unlimiter's protected technical profiles.");
        std::string bytes;if(!ReadText(mapping.physical,bytes,256*1024))return Fail(L"Cannot read a selected addon profile.");const auto key="unlimiterdata/"+name;auto old=files.find(key);if(old==files.end()||old->second!=bytes){if(name.front()=='_')return Fail(L"A mod tried to replace Unlimiter's protected technical profiles.");files[key]=std::move(bytes);changed=true;}}
    if(!changed)return true;std::string identity="Undercoop Unlimiter profile set 1\n";for(const auto& file:files)identity+=file.first+"="+ImportHash(file.second)+"\n";
    const auto hash=ImportHash(identity),relative="native-builds/unlimiter-"+hash.substr(0,24);const auto owner=state.root+L"\\mods\\_undercoop",folder=owner+L"\\"+Disk(Wide(relative));std::wstring error;
    if(!ImportDirectory(owner,Disk(Wide(relative)),error))return Fail(error);for(const auto& directory:{owner,owner+L"\\native-builds",folder}){auto p=Pin(directory,true);if(!p)return Fail(L"The generated native profile directory changed.");staged.pins.push_back(std::move(p));}
    for(const auto& file:files){const auto physical=folder+L"\\"+Disk(Wide(file.first));if(GetFileAttributesW(physical.c_str())==INVALID_FILE_ATTRIBUTES&&!ImportWriteNew(folder,file.first,file.second,error))return Fail(error);auto p=Pin(physical);std::string actual;uint64_t n=0;
        if(!p||!Digest(p->value,actual,n)||actual!=ImportHash(file.second)||n!=file.second.size())return Fail(L"A generated native profile changed. It was not loaded or overwritten.");staged.pins.push_back(std::move(p));}
    auto profiles=Pin(folder+L"\\unlimiterdata",true);if(!profiles)return Fail(L"Cannot lock generated addon profiles.");staged.pins.push_back(std::move(profiles));
    // Refuse unknown code sidecars in a previously prepared build directory.
    WIN32_FIND_DATAW row{};HANDLE scan=FindFirstFileW((folder+L"\\*").c_str(),&row);if(scan==INVALID_HANDLE_VALUE)return false;bool okay=true;
    do{std::wstring name=row.cFileName;if(name==L"."||name==L"..")continue;if(row.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){okay=false;break;}if(row.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){if(!Equal(name,L"unlimiterdata")){okay=false;break;}}else if(!files.count(coop_mods::Lower(coop_mod_scan::Utf8(name)))){okay=false;break;}}while(FindNextFileW(scan,&row));okay=okay&&GetLastError()==ERROR_NO_MORE_FILES;FindClose(scan);
    if(okay){scan=FindFirstFileW((folder+L"\\unlimiterdata\\*").c_str(),&row);if(scan==INVALID_HANDLE_VALUE)okay=false;else{do{const std::wstring name=row.cFileName;if(name==L"."||name==L"..")continue;if((row.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!files.count("unlimiterdata/"+coop_mods::Lower(coop_mod_scan::Utf8(name)))){okay=false;break;}}while(FindNextFileW(scan,&row));okay=okay&&GetLastError()==ERROR_NO_MORE_FILES;FindClose(scan);}}
    if(!okay)return Fail(L"Unexpected files beside the generated native component.");native->path=folder+L"\\nfsu2unlimiter.asi";receipt+="UnlimiterProfiles="+hash+"\n";return true;
}
