// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Owned launch-window import jobs. Workers never touch a game, load native code,
// edit UI state or write outside their mods workspace.
enum class ImportPhase{Idle,Inspecting,Review,Installing,Complete,Failed};
enum class ImportWork{Inspect,PackInspect,Install,Repair,Runtime,PlayRuntime};
inline bool ImportInspectWork(ImportWork work){return work==ImportWork::Inspect||work==ImportWork::PackInspect;}
inline bool ConfirmRequirements(HWND window,const std::wstring& message){return window&&MessageBoxW(window,message.c_str(),L"Undercoop - required components",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES;}
struct ImportPanel{
    ImportPhase phase=ImportPhase::Idle;ImportWork work=ImportWork::Inspect;
    bool allowDownloads=true,codeAccepted=false,closeAfter=false;
    std::shared_ptr<ImportPlan> plan,result;std::vector<std::string> selectedBefore;
    std::thread worker;std::mutex mutex;std::atomic<bool> cancel{false},done{false};bool okay=false;
    std::wstring message,createdPackId;
    bool (*confirmRequirements)(HWND,const std::wstring&)=&ConfirmRequirements;
    FetchDependency fetchRuntime=&DownloadHTTPS;
    ~ImportPanel(){cancel.store(true);if(worker.joinable())worker.join();}
    ImportPanel()=default;ImportPanel(const ImportPanel&)=delete;ImportPanel& operator=(const ImportPanel&)=delete;
};
struct ImportLayout{
    UiLayout base;explicit ImportLayout(float w,float h):base(w,h){}
    UiRect Pick()const{return {base.width-154,119,124,32};}
    UiRect Folder()const{return {base.width-290,119,124,32};}
    UiRect Downloads()const{return {base.content+20,base.body+150,base.ContentWidth()-40,37};}
    UiRect Consent()const{return {base.content+20,base.body+190,base.ContentWidth()-40,50};}
    UiRect Install()const{return {base.content+20,base.footer-69,176,40};}
    UiRect Cancel()const{return {base.content+208,base.footer-69,115,40};}
    UiRect Details()const{return {base.width-214,base.footer-69,184,40};}
};
constexpr UINT kImportTimer=7213;
constexpr UINT kImportComplete=WM_APP+243;
// The picker is a convenience, not a trust boundary: archive contents determine
// the format. Executable installers are deliberately not advertised here.
constexpr wchar_t kModArchiveFilter[]=L"Mod archives\0*.zip;*.rar;*.7z;*.7zip;*.tar;*.tar.gz;*.tgz;*.tar.bz2;*.tbz2;*.tar.xz;*.txz;*.gz;*.bz2;*.xz;*.cab;*.arj;*.lzh;*.lha;*.cpio;*.iso;*.wim;*.xar;*.z;*.zst\0All files (*.*)\0*.*\0\0";
constexpr wchar_t kModArchivePickerTitle[]=L"Undercoop - choose a mod archive";
constexpr wchar_t kChooseArchiveLabel[]=L"Choose archive";
constexpr wchar_t kModpackArchiveFilter[]=L"Modpack ZIP archives\0*.zip\0\0";
inline bool UiConfirmImported(HWND window,const std::wstring& text){
    return MessageBoxW(window,text.c_str(),L"Undercoop - approve imported native code",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES;
}
inline std::wstring ImportedApprovalText(){
    coop_packages::Plan plan;coop_packages::Error error;if(!coop_packages::BuildPlan(state.catalog,state.selected,plan,error))return {};
    std::wstring text;for(const auto& id:plan.Packages()){const auto it=state.imported.find(id);if(it==state.imported.end())continue;
        if(text.empty())text=L"These imported plugins have NOT been reviewed for CO-OP compatibility. They run with the game's permissions, can write outside mods, and may crash or conflict with save/physics hooks.\n\n";
        text+=Wide(id)+L"\n";for(const auto& file:it->second.files)if(file.module)text+=L"  "+Wide(file.source)+L"  ["+Wide(file.hash.substr(0,12))+L"]\n";
    }if(!text.empty())text+=L"\nOnly proceed with plugins you trust. Approve this selected set for this launch?";return text;
}
inline std::wstring ImportStatus(ImportPanel& panel){std::lock_guard<std::mutex> lock(panel.mutex);return panel.message;}
inline void SetImportStatus(ImportPanel& panel,const std::wstring& message){std::lock_guard<std::mutex> lock(panel.mutex);panel.message=message;}
template<class V> inline bool BeginImport(V& view,ImportWork work,const std::wstring& source={},const std::string& binaryLauncher={}){
    auto& panel=view.imports;if(view.busy||view.accepted||panel.worker.joinable()||state.ready||state.fatal)return false;
    if(work==ImportWork::Install&&panel.plan&&!panel.plan->binaryChoices.empty()){
        if(!view.window)return false;HMENU menu=CreatePopupMenu();if(!menu)return false;
        for(size_t i=0;i<panel.plan->binaryChoices.size();++i)AppendMenuW(menu,MF_STRING,UINT_PTR(i+1),Wide(panel.plan->binaryChoices[i]).c_str());
        POINT point{};GetCursorPos(&point);const UINT chosen=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_LEFTALIGN,point.x,point.y,0,view.window,nullptr);DestroyMenu(menu);
        if(!chosen||chosen>panel.plan->binaryChoices.size())return false;
        const auto path=panel.plan->archive;const auto entry=panel.plan->binaryChoices[chosen-1];return BeginImport(view,ImportWork::Inspect,path,entry);
    }
    if(work==ImportWork::Repair&&!state.instanceId.empty()){
        const auto missing=MissingModpackRequirements();if(!missing.empty()){std::wstring text=L"This modpack is missing: ";for(size_t i=0;i<missing.size();++i){if(i)text+=L", ";text+=Wide(missing[i]);}
            text+=L". Open Modpacks > Edit to add these components from your library. If not installed yet, choose Use custom setup, get requirements there, then edit this modpack. Other instances are never changed automatically.";
            SetImportStatus(panel,text);view.tab=5;return false;}
    }
    if(work==ImportWork::Install&&(!panel.plan||panel.phase!=ImportPhase::Review||panel.plan->code&&!panel.codeAccepted))return false;
    if(work==ImportWork::PlayRuntime||work==ImportWork::Install&&panel.plan->runtimeMissing){
        const bool launch=work==ImportWork::PlayRuntime;
        const std::wstring message=L"The selected mods need Microsoft .NET 8 x86 for Binary data conversion.\n\nDownload 29.2 MiB from Microsoft's official server and install a verified private copy under mods? No administrator rights, system installation or registry changes are needed.\n\n"+std::wstring(launch?L"After installation, Play will continue automatically with the same selected mods.":L"The mod will be inspected again before you approve its installation.");
        if(!panel.confirmRequirements||!panel.confirmRequirements(view.window,message)){
            if(launch)state.error=L"Play cancelled. Microsoft .NET 8 x86 was not downloaded or installed.";
            return false;
        }
        if(!launch)work=ImportWork::Runtime;
    }else if(work==ImportWork::Install&&panel.allowDownloads){
        std::wstring missing;for(const auto& need:panel.plan->package.requires){bool installed=false;for(const auto& p:state.catalog)installed=installed||(p.id==need.id&&p.version==need.version);if(!installed)missing+=Wide(need.id)+L" "+Wide(need.version)+L"\n";}
        if(!missing.empty()&&(!panel.confirmRequirements||!panel.confirmRequirements(view.window,L"This mod needs the following components:\n\n"+missing+L"\nDownload verified copies from their approved official sources and install them inside mods?")))return false;
    }else if(work==ImportWork::Repair){
        if(!panel.confirmRequirements||!panel.confirmRequirements(view.window,L"Check the selected mods and download any missing supported requirements from verified official sources? Existing packages are not replaced."))return false;
    }
    const auto root=state.root;const auto catalog=state.catalog;const auto selected=state.selected;const auto plan=panel.plan;const bool downloads=panel.allowDownloads;const HWND owner=view.window;const auto runtimeFetch=panel.fetchRuntime;
    const DWORD sourceAttributes=work==ImportWork::Inspect?GetFileAttributesW(source.c_str()):INVALID_FILE_ATTRIBUTES;
    const bool folderSource=sourceAttributes!=INVALID_FILE_ATTRIBUTES&&(sourceAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    panel.selectedBefore=state.selected;panel.work=work;panel.phase=ImportInspectWork(work)?ImportPhase::Inspecting:ImportPhase::Installing;
    if(work==ImportWork::Repair||work==ImportWork::PlayRuntime)panel.plan.reset();
    panel.cancel=false;panel.done=false;panel.okay=false;panel.result.reset();panel.codeAccepted=false;panel.createdPackId.clear();
    SetImportStatus(panel,ImportInspectWork(work)?(folderSource?L"Inspecting manual folder contents...":work==ImportWork::PackInspect?L"Inspecting modpack archive...":L"Inspecting archive contents..."):L"Checking requirements and staging files...");
    view.tab=5;view.busy=true;SyncIniEditor(view);
    try{panel.worker=std::thread([&panel,root,catalog,selected,plan,source,binaryLauncher,work,downloads,owner,runtimeFetch,folderSource]{
        std::wstring error,createdPackId;bool okay=false;std::shared_ptr<ImportPlan> result;
        try{
            if(ImportInspectWork(work)){
                result=std::make_shared<ImportPlan>();
                if(folderSource)okay=ImportInspectFolder(source,*result,error,&panel.cancel,root);
                else{
                    auto pack=std::make_shared<PackArchivePlan>();
                    const auto probe=InspectModpackArchive(source,*pack,error,&panel.cancel);
                    if(probe==PackProbe::Ready){
                        result->pack=std::move(pack);result->archive=source;result->hash=result->pack->hash;
                        result->bytes=result->pack->bytes;result->package.name=coop_mod_scan::Utf8(result->pack->name);
                        result->code=!result->pack->nativeModules.empty();result->notes=result->pack->notes;okay=true;
                    }else if(probe==PackProbe::NotPack){
                        if(work==ImportWork::PackInspect)error=L"This ZIP is not a supported complete modpack. Use Add mods for individual mod archives.";
                        else okay=ImportInspect(source,*result,error,&panel.cancel,root,binaryLauncher);
                    }
                }
            }
            else if(work==ImportWork::Runtime){result=std::make_shared<ImportPlan>();okay=InstallBinaryRuntime(root,error,&panel.cancel,[&](const std::wstring& message){SetImportStatus(panel,message);})&&ImportInspect(plan->archive,*result,error,&panel.cancel,root);}
            else if(work==ImportWork::PlayRuntime)okay=runtimeFetch&&InstallBinaryRuntime(root,error,&panel.cancel,[&](const std::wstring& message){SetImportStatus(panel,message);},runtimeFetch);
            else if(work==ImportWork::Repair)okay=RepairRequirements(root,catalog,selected,error,&panel.cancel,[&](const std::wstring& message){SetImportStatus(panel,message);});
            else if(plan&&plan->pack){
                okay=CommitModpackArchive(root,*plan->pack,createdPackId,error,&panel.cancel,
                    [&](const std::wstring& message){SetImportStatus(panel,message);});
            }else{
                const auto target=root+L"\\mods\\"+Wide(plan->package.id);
                if(plan->folder){
                    if(ImportFolderTargetAvailable(root,*plan,error))okay=ImportRequirements(root,*plan,catalog,downloads,error,&panel.cancel,[&](const std::wstring& message){SetImportStatus(panel,message);})&&ImportCommitFolder(root,*plan,error,&panel.cancel);
                }else if(GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES)error=L"This package ID is already installed. Use Components / Get requirements to repair missing dependencies. Existing packages are never overwritten.";
                else okay=ImportRequirements(root,*plan,catalog,downloads,error,&panel.cancel,[&](const std::wstring& message){SetImportStatus(panel,message);})&&ImportCommit(root,*plan,error,&panel.cancel);
            }
        }catch(const std::exception&){error=L"Import failed within its resource limits. No existing package was replaced.";okay=false;}
        catch(...){error=L"Import could not complete. Existing packages and careers were not overwritten.";okay=false;}
        {std::lock_guard<std::mutex> lock(panel.mutex);panel.okay=okay;panel.result=std::move(result);panel.createdPackId=std::move(createdPackId);
            panel.message=okay?(work==ImportWork::PlayRuntime?L"Microsoft .NET 8 x86 installed and verified. Continuing Play...":ImportInspectWork(work)||work==ImportWork::Runtime?L"Review the package, then choose Install. It is not enabled automatically.":work==ImportWork::Repair?L"Required component packages checked. Launch validates their files; the selected mods are unchanged.":plan&&plan->pack?L"Imported into Modpacks; not activated. Your current setup and careers are unchanged. Install the same archive on your teammate's computer.":plan&&plan->folder?L"Folder registered and disabled. Enable it in My mods or add it with Modpacks > Edit. Your teammate needs the same folder contents.":L"Installed and disabled. Enable it in My mods when you are ready; install the same archive on your teammate's computer."):error;}
        if(okay&&panel.result&&!panel.result->binaryChoices.empty())SetImportStatus(panel,L"Choose an installer to inspect its changes. Nothing has been installed.");
        panel.done.store(true);
        if(owner)PostMessageW(owner,kImportComplete,0,0);
    });}catch(...){view.busy=false;panel.phase=ImportPhase::Failed;SetImportStatus(panel,L"Cannot start the import worker.");return false;}
    if(view.window)SetTimer(view.window,kImportTimer,100,nullptr);return true;
}
template<class V> inline bool PollImport(V& view){
    auto& panel=view.imports;if(!panel.worker.joinable()||!panel.done.load())return false;
    panel.worker.join();if(view.window)KillTimer(view.window,kImportTimer);
    bool okay=false;{std::lock_guard<std::mutex> lock(panel.mutex);okay=panel.okay;if(ImportInspectWork(panel.work)||panel.work==ImportWork::Runtime)panel.plan=okay?std::move(panel.result):nullptr;}
    const bool packCommit=panel.work==ImportWork::Install&&panel.plan&&panel.plan->pack;
    if(packCommit){
        // A whole-pack import publishes a new inactive instance. Discovery and
        // SaveSelection here would mutate the unrelated currently active setup.
        if(okay){
            view.modpacks.first=0;view.modpacks.message=ImportStatus(panel);
            if(!ListModpacks(view.modpacks.packs))view.modpacks.message+=L" The modpack list could not refresh: "+state.error;
            view.tab=6;
        }
    }else if(!ImportInspectWork(panel.work)&&panel.work!=ImportWork::Runtime&&panel.work!=ImportWork::PlayRuntime){const auto root=state.root;const auto selection=panel.selectedBefore;
        if(!Discover(root)){okay=false;SetImportStatus(panel,L"Installation finished but the package list could not reload: "+state.error);}
        state.selected=selection;state.nativeTrusted=false;
        if(!SaveSelection()){okay=false;SetImportStatus(panel,L"Package processing finished, but the disabled selection could not be saved. Review My mods before launch; existing packages were not overwritten.");}
        if(okay&&panel.work==ImportWork::Install&&!state.instanceId.empty())SetImportStatus(panel,L"Installed and disabled in your custom mod library. Open Modpacks > Edit to add it to this instance; other modpacks were not changed.");
    }
    panel.phase=okay?(ImportInspectWork(panel.work)||panel.work==ImportWork::Runtime?ImportPhase::Review:ImportPhase::Complete):ImportPhase::Failed;view.busy=false;
    if(panel.closeAfter&&view.window)PostMessageW(view.window,WM_CLOSE,0,0);return true;
}
template<class V> inline bool PickImport(V& view){
    wchar_t path[32768]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=view.window;
    dialog.lpstrFilter=kModArchiveFilter;dialog.lpstrFile=path;dialog.nMaxFile=DWORD(std::size(path));
    dialog.lpstrTitle=kModArchivePickerTitle;dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT;
    if(!GetOpenFileNameW(&dialog))return false;return BeginImport(view,ImportWork::Inspect,path);
}
template<class V> inline bool PickImportModpack(V& view){
    wchar_t path[32768]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=view.window;
    dialog.lpstrFilter=kModpackArchiveFilter;dialog.lpstrFile=path;dialog.nMaxFile=DWORD(std::size(path));
    dialog.lpstrTitle=L"Undercoop - import a modpack ZIP";dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT;
    if(!GetOpenFileNameW(&dialog))return false;return BeginImport(view,ImportWork::PackInspect,path);
}
inline std::wstring PackImportDetailsText(const PackArchivePlan& pack){
    std::wstring text=pack.name+L"\n\n"+std::to_wstring(pack.files)+L" files / "+std::to_wstring((pack.bytes+1024*1024-1)/(1024*1024))+L" MiB\n";
    text+=L"Archive SHA-256: "+Wide(pack.hash)+L"\n";
    if(!pack.nativeModules.empty()){text+=L"\nNative plugins included:\n";for(const auto& module:pack.nativeModules)text+=L"- "+module+L"\n";}
    if(!pack.notes.empty()){text+=L"\nImport notes:\n";for(const auto& note:pack.notes)text+=L"- "+note+L"\n";}
    text+=L"\nImport creates an inactive modpack. Your current selection and existing careers are not replaced. Both players must import the same archive.";
    return text;
}
template<class V> inline bool ShowImportPackDetails(V& view){
    const auto& plan=view.imports.plan;if(!plan||!plan->pack||!view.window)return false;
    const auto text=PackImportDetailsText(*plan->pack);
    MessageBoxW(view.window,text.c_str(),L"Undercoop - modpack import details",MB_OK|MB_ICONINFORMATION);return true;
}

inline bool ChooseManualFolder(HWND window,std::wstring& path){
    path.clear();if(!window)return false;
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const bool uninitialize=SUCCEEDED(initialized);
    if(FAILED(initialized)&&initialized!=RPC_E_CHANGED_MODE)return false;
    IFileOpenDialog* dialog=nullptr;
    bool okay=false;
    if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))){
        DWORD options=0;
        if(SUCCEEDED(dialog->GetOptions(&options))&&SUCCEEDED(dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_DONTADDTORECENT))){
            dialog->SetTitle(L"Undercoop - choose a game-ready mod folder");
            dialog->SetOkButtonLabel(L"Review folder");
            if(SUCCEEDED(dialog->Show(window))){
                IShellItem* item=nullptr;
                if(SUCCEEDED(dialog->GetResult(&item))){
                    PWSTR name=nullptr;
                    if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&name))){path=name;CoTaskMemFree(name);okay=true;}
                    item->Release();
                }
            }
        }
        dialog->Release();
    }
    if(uninitialize)CoUninitialize();return okay;
}
template<class V> inline bool PickImportFolder(V& view){
    if(!view.window)return false;
    // Directories placed below mods are pending rather than an invalid package.
    // The same review/registration pipeline handles them and external folders.
    if(!state.pendingFolders.empty()){
        HMENU menu=CreatePopupMenu();if(!menu)return false;
        AppendMenuW(menu,MF_STRING,1,L"Browse for a folder...");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        for(size_t i=0;i<state.pendingFolders.size();++i){
            std::wstring label=L"Register: ";
            for(wchar_t c:state.pendingFolders[i]){label+=c;if(c==L'&')label+=L'&';}
            AppendMenuW(menu,MF_STRING,UINT_PTR(i+2),label.c_str());
        }
        POINT point{};GetCursorPos(&point);
        const UINT selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_LEFTALIGN,point.x,point.y,0,view.window,nullptr);DestroyMenu(menu);
        if(!selected)return false;
        if(selected>=2&&selected-2<state.pendingFolders.size())return BeginImport(view,ImportWork::Inspect,state.root+L"\\mods\\"+state.pendingFolders[selected-2]);
        if(selected!=1)return false;
    }
    std::wstring folder;if(!ChooseManualFolder(view.window,folder))return false;
    return BeginImport(view,ImportWork::Inspect,folder);
}
template<class V> inline UiHit ImportHitTest(const V& view,float x,float y,float width,float height){
    const ImportLayout l(width,height);if(l.Cancel().Contains(x,y)&&view.busy)return {UiAction::ImportCancel};if(view.busy)return {};
    if(l.Pick().Contains(x,y))return {UiAction::ImportPick};
    if(l.Folder().Contains(x,y))return {UiAction::ImportFolder};
    if(view.imports.plan&&view.imports.plan->pack&&l.Details().Contains(x,y))return {UiAction::ImportPackDetails};
    if(view.imports.phase!=ImportPhase::Review&&l.Install().Contains(x,y))return {UiAction::ImportRepair};
    if(view.imports.phase==ImportPhase::Review&&view.imports.plan){
        if(!view.imports.plan->pack&&l.Downloads().Contains(x,y))return {UiAction::ImportDownloads};
        if(l.Consent().Contains(x,y)&&view.imports.plan->code)return {UiAction::ImportConsent};
        if(l.Install().Contains(x,y)&&(!view.imports.plan->code||view.imports.codeAccepted))return {UiAction::ImportInstall};
    }return {};
}
template<class V> inline void PaintImport(V& view,float width,float height){
    auto& panel=view.imports;auto& r=view.renderer;const ImportLayout l(width,height);const float x=l.base.content,cw=l.base.ContentWidth(),y=l.base.body;
    auto text=[&](const std::wstring& value,float px,float py,float w,float h,float size=13,unsigned color=0xB9C4D5,UiFace face=UiFace::Regular,bool wrap=false){r.Text(value.c_str(),px,py,w,h,size,color,face,wrap);};
    auto button=[&](UiRect box,const wchar_t* label,bool enabled){r.Box(box.x,box.y,box.w,box.h,enabled?0xDBE3EF:0x262D37,6);r.Text(label,box.x,box.y,box.w,box.h,12,enabled?0x1B2431:0x7C899C,UiFace::Bold,false,true);};
    button(l.Folder(),L"Choose folder",!view.busy);button(l.Pick(),kChooseArchiveLabel,!view.busy);r.Box(x,y,cw,l.base.footer-y-88,0x191E25,8,0x303845);
    if(panel.phase==ImportPhase::Review&&panel.plan&&!panel.plan->binaryChoices.empty()){
        text(L"CHOOSE AN INSTALLER",x+20,y+17,cw-40,30,20,0xE6EBF3,UiFace::Bold);
        text(L"This archive contains multiple compatible Binary installation entries. Select the one you want; nothing has been installed.",x+20,y+65,cw-40,75,14,0xA6B3C6,UiFace::Regular,true);
        text(std::to_wstring(panel.plan->binaryChoices.size())+L" entries available",x+20,y+150,cw-40,30);
        button(l.Install(),L"Choose installer",true);return;
    }
    if(panel.work==ImportWork::PlayRuntime){
        text(L"REQUIRED COMPONENT",x+20,y+17,cw-40,30,20,0xE6EBF3,UiFace::Bold);
        text(L"Microsoft .NET 8 x86",x+20,y+65,cw-40,35,16,0xDCE4F0,UiFace::Regular,true);
        text(L"Downloading and verifying a private copy inside mods. Play continues automatically when it is ready. Your selected mods and careers stay unchanged.",x+20,y+115,cw-40,90,14,0xA6B3C6,UiFace::Regular,true);
    }else if(!panel.plan||panel.phase==ImportPhase::Inspecting){
        text(view.busy?(panel.work==ImportWork::Repair?L"Checking requirements...":panel.work==ImportWork::PackInspect?L"Inspecting modpack...":L"Inspecting package..."):L"ADD A MOD",x+20,y+17,cw-40,30,20,0xE6EBF3,UiFace::Bold);
        text(L"Choose an archive or a game-ready folder, or drop either here. Supported layouts include CARS/GLOBAL resources and scripts-style ASI plugins. Folder metadata is created for you.",x+20,y+65,cw-40,70,14,0xA6B3C6,UiFace::Regular,true);
        text(L"Supported Binary data installers are converted inside mods; installer programs are not executed. Missing supported components require your approval before downloading.",x+20,y+152,cw-40,72,13,0x929FB2,UiFace::Regular,true);
        const float pendingRoom=l.base.footer-88-y-239-12;
        if(!state.pendingFolders.empty()&&pendingRoom>28)text(std::to_wstring(state.pendingFolders.size())+L" unregistered folder(s) found in mods. Choose folder to review and register one.",x+20,y+239,cw-40,pendingRoom,13,0xC7D2E0,UiFace::Regular,true);
    }else if(panel.plan->pack){
        const auto& pack=*panel.plan->pack;
        text(pack.name,x+20,y+15,cw-40,31,21,0xEDF1F8,UiFace::Bold);
        text(L"Complete modpack  /  "+std::to_wstring(pack.files)+L" files  /  "+std::to_wstring((pack.bytes+1024*1024-1)/(1024*1024))+L" MiB",x+20,y+53,cw-40,24);
        std::wstring modules=L"Included native plugins: ";
        if(pack.nativeModules.empty())modules+=L"none";
        for(size_t i=0;i<pack.nativeModules.size();++i){if(i)modules+=L", ";modules+=pack.nativeModules[i];}
        text(modules,x+20,y+83,cw-40,76,12,0xB4C4D9,UiFace::Regular,true);
        text(L"Imported into Modpacks, inactive. Your current setup is preserved.",x+20,y+163,cw-40,25,12,0xAABBD3);
        if(panel.plan->code){const auto box=l.Consent();r.Box(box.x,box.y+8,18,18,panel.codeAccepted?0xDCE4F0:0x29313D,4,0x697A93);
            if(panel.codeAccepted){r.Line(box.x+4,box.y+17,box.x+8,box.y+21,0x243044,1.7f);r.Line(box.x+8,box.y+21,box.x+14,box.y+13,0x243044,1.7f);}
            text(L"I approve importing these native plugins. They are not sandboxed. Importing does not activate the modpack.",box.x+29,box.y+6,box.w-29,box.h,12,0xB4C0D2,UiFace::Regular,true);}
        const float noteY=y+(panel.plan->code?250:198),room=l.base.footer-94-noteY;
        if(room>25)text(pack.notes.empty()?L"Both players need the same archive. See Package details before importing.":pack.notes.front()+L" See Package details for all notes.",x+20,noteY,cw-40,room,12,0x8E9FB7,UiFace::Regular,true);
        if(!view.busy)button(l.Details(),L"Package details",true);
    }else{
        const auto& plan=*panel.plan;
        text(Wide(plan.package.name),x+20,y+15,cw-40,31,21,0xEDF1F8,UiFace::Bold);
        text(std::wstring(plan.folder?L"Manual folder":plan.binary?L"Binary data installer":plan.code?L"Native plugin":L"Resource overlay")+L"  /  "+std::to_wstring(plan.items.size())+L" files  /  "+std::to_wstring((plan.bytes+1023)/1024)+L" KiB",x+20,y+53,cw-40,24);
        text(L"ID: "+Wide(plan.package.id)+L"   Hash: "+Wide(plan.hash.substr(0,16))+L"...",x+20,y+83,cw-40,23,11,0x8F9EB2);
        std::wstring needs=plan.package.requires.empty()?L"No additional mod requirements declared.":L"Requirements: ";
        for(const auto& need:plan.package.requires){const auto found=std::find_if(state.catalog.begin(),state.catalog.end(),[&](const auto& p){return p.id==need.id&&p.version==need.version;});
            needs+=Wide(need.id)+L" ("+(found!=state.catalog.end()?L"installed":DownloadRecipe(need)?L"will download":L"no recipe")+L")  ";}
        if(plan.runtimeMissing)needs=L"Required: Microsoft .NET 8 x86. Download and private installation need your approval.";
        text(needs,x+20,y+111,cw-40,34,12,0xB4C4D9,UiFace::Regular,true);
        auto check=[&](UiRect box,bool checked,const wchar_t* label){r.Box(box.x,box.y+8,18,18,checked?0xDCE4F0:0x29313D,4,0x697A93);if(checked){r.Line(box.x+4,box.y+17,box.x+8,box.y+21,0x243044,1.7f);r.Line(box.x+8,box.y+21,box.x+14,box.y+13,0x243044,1.7f);}text(label,box.x+29,box.y+6,box.w-29,box.h,12,0xB4C0D2,UiFace::Regular,true);};
        check(l.Downloads(),panel.allowDownloads,L"Automatically download missing supported requirements (verified official sources).");
        if(plan.code)check(l.Consent(),panel.codeAccepted,L"I understand this native mod is not sandboxed or proven CO-OP-compatible. Installing leaves it disabled.");
        const float noteY=y+(plan.code?250:198),room=l.base.footer-94-noteY;
        if(room>25){std::wstring notes;
            if(plan.folder){
                notes=plan.adoptFolder?L"Registering this mods folder keeps its original contents in mods/_undercoop/manual-backups.\n":L"The source folder stays unchanged. The launcher creates the managed package inside mods.\n";
                const size_t limit=room>140?4:room>100?2:0;
                for(size_t i=0;i<plan.items.size()&&i<limit;++i)notes+=L"Game path: "+Wide(plan.items[i].destination)+L"\n";
                if(limit&&plan.items.size()>limit)notes+=L"... and "+std::to_wstring(plan.items.size()-limit)+L" more files.\n";
                notes+=L"Enable/disable in My mods; add/remove with Modpacks > Edit.";
            }else{for(const auto& note:plan.notes)if(note.rfind(L"Default choice:",0)==0)notes+=note+L"\n";notes+=L"Original game files and existing careers are not replaced. Both players need the same effective setup.";}
            text(notes,x+20,noteY,cw-40,room,12,0x8E9FB7,UiFace::Regular,true);}
    }
    if(view.busy)button(l.Cancel(),panel.cancel.load()?L"Cancelling...":L"Cancel",!panel.cancel.load());
    else if(panel.phase==ImportPhase::Review&&panel.plan)button(l.Install(),panel.plan->pack?L"Import modpack":panel.plan->runtimeMissing?L"Get required runtime":panel.plan->folder?L"Register folder":L"Install package",!panel.plan->code||panel.codeAccepted);
    else button(l.Install(),L"Check requirements",true);
}
