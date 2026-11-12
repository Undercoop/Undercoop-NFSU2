// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Included inside CoopModManager. Only the owned pre-game window is changed.
// Layout and interaction share one DIP coordinate system, independent of DPI.
using UiFace=coop_manager_graphics::Face;
struct UiRect{
    float x=0,y=0,w=0,h=0;
    bool Contains(float px,float py)const{return px>=x&&py>=y&&px<x+w&&py<y+h;}
};
enum class UiAction{None,Tab,Package,Approve,Play,Manage,Minimize,Maximize,Close,IniMod,IniFile,IniRow,IniSave,IniDiscard,IniReload,IniAll,IniGroup,ImportPick,ImportFolder,ImportInstall,ImportCancel,ImportDownloads,ImportConsent,ImportRepair,ImportPackDetails,PackImport,PackCreate,PackConfirm,PackCancel,PackActivate,PackEdit,PackToggle,PackSave,PackLibrary,UndercoopCategory,UndercoopRole,UndercoopToggle,UndercoopShow,UndercoopOpacity,UndercoopSave,UndercoopDiscard,UndercoopReload,
    NoticeToggle,NoticeClose,NoticeAcknowledge,NoticeBody,DeveloperSignIn,DeveloperSubmit,DeveloperSignOut,DeveloperBack,
    BackupAuto,BackupSelect,BackupPage,CareerImportPick,CareerImportSelect,CareerImportPage};
struct UiHit{
    UiAction action=UiAction::None;size_t index=0;
    bool operator==(const UiHit& other)const{return action==other.action&&index==other.index;}
    bool operator!=(const UiHit& other)const{return !(*this==other);}
};
struct UiLayout{
    float width,height,content=242,body=198,footer;
    explicit UiLayout(float w,float h):width(w),height(h),footer(h-84){}
    float ContentWidth()const{return width-content-30;}
    UiRect Navigation(size_t i)const{return {16,108+float(i)*46,180,42};}
    UiRect Package(size_t i)const{return {content,body+float(i)*82,ContentWidth(),70};}
    UiRect PackageSettings(size_t i)const{const auto row=Package(i);return {row.x+row.w-186,row.y+22,76,28};}
    UiRect RequiredBadge(size_t i)const{const auto row=Package(i);return {row.x+row.w-100,row.y+22,80,25};}
    size_t Rows()const{return size_t((std::max)(1,int((footer-body-18)/82)));}
    UiRect Approve()const{return {content+22,body+155,246,40};}
    UiRect Play()const{return {width-212,height-63,182,44};}
    UiRect Manage()const{return {width-154,119,124,32};}
    UiRect Minimize()const{return {width-144,0,48,46};}
    UiRect Maximize()const{return {width-96,0,48,46};}
    UiRect Close()const{return {width-48,0,48,46};}
};
struct UiSummary{bool valid=false,native=false;size_t assets=0,dependencies=0;std::wstring name,error;std::vector<std::string> required;std::vector<std::wstring> components;};
inline UiSummary DescribeSetup(){
    UiSummary out;coop_packages::Plan plan;coop_packages::Error error;
    out.valid=coop_packages::BuildPlan(state.catalog,state.selected,plan,error);
    if(!out.valid){out.error=Wide(error.detail);return out;}
    for(const auto& id:plan.Packages())for(const auto& p:state.catalog)if(id==p.id){
        if(p.kind==coop_packages::Kind::Assets||p.kind==coop_packages::Kind::Data){++out.assets;if(out.name.empty())out.name=Wide(p.name);}
        else{++out.dependencies;out.native=true;out.required.push_back(p.id);out.components.push_back(Wide(p.name));
            if(!ApprovedNative(p)){out.valid=false;out.error=L"This component needs a supported adapter: "+Wide(p.id);}}
    }
    if(out.name.empty())out.name=L"Original car lineup";else if(out.assets>1)out.name+=L" + "+std::to_wstring(out.assets-1)+L" more";
    return out;
}
#include "CoopManagerIniUi.inl"
#include "CoopManagerDeveloperUi.inl"
#include "CoopManagerUndercoopSettingsUi.inl"
#include "CoopManagerImportUi.inl"
#include "CoopManagerModpacksUi.inl"
#include "CoopManagerLiveNoticeUi.inl"
struct View{
    HMODULE module=nullptr;HWND window=nullptr;coop_manager_graphics::Renderer renderer;
    int tab=0;size_t first=0;bool accepted=false,busy=false,tracking=false;
    UiHit pressed{},hover{};bool (*activate)()=&Activate;
    IniPanel ini;
    UndercoopPanel undercoop;
    std::vector<coop_backup::Record> (*listBackups)()=nullptr;
    bool (*restoreBackup)(const coop_backup::Record&,std::wstring&)=nullptr;
    std::vector<CareerImportSource> (*listCareerImports)()=&ListCareerImportSources;
    bool (*previewCareerImport)(const std::wstring&,std::wstring&,std::wstring&)=&PreviewCareerImport;
    bool (*importCareer)(const std::wstring&,const std::wstring&,std::wstring&)=&ImportCareerProfile;
    bool (*confirmCareerImport)(HWND,const std::wstring&)=&ConfirmCareerImport;
    ImportPanel imports;bool (*confirmImported)(HWND,const std::wstring&)=&UiConfirmImported;
    ModpackPanel modpacks;
    live_notice::Panel notice;
    DeveloperPanel developerPanel;
    coop_developer::Session developer;
};
inline void SyncLauncherEditors(View& view) {
    SyncDeveloperEditors(view);
    if (view.notice.open) {
        if (view.ini.edit) ShowWindow(view.ini.edit, SW_HIDE);
        if (view.modpacks.edit) ShowWindow(view.modpacks.edit, SW_HIDE);
        for (auto editor : view.undercoop.edits)
            if (editor) ShowWindow(editor, SW_HIDE);
        if (view.window) SetFocus(view.window);
        return;
    }
    SyncIniEditor(view);
    SyncModpackEditor(view);
    SyncUndercoopEditors(view);
}
inline UiHit HitTest(const View& view,float x,float y,float width,float height){
    UiLayout l(width,height);
    if(l.Close().Contains(x,y))return {UiAction::Close};
    if(l.Minimize().Contains(x,y))return {UiAction::Minimize};
    if(l.Maximize().Contains(x,y))return {UiAction::Maximize};
    const auto noticeHit = LiveNoticeHit(view.notice, l, x, y);
    if (noticeHit.action != UiAction::None) return noticeHit;
    if(view.tab==6&&view.modpacks.naming)return ModpackHitTest(view,x,y,width,height);
    if(l.Play().Contains(x,y))return {UiAction::Play};
    for(size_t i=0;i<8;++i)if(l.Navigation(i).Contains(x,y))return {UiAction::Tab,i};
    if(view.tab==7)return UndercoopHitTest(view,x,y,width,height);
    if(view.tab==0&&l.Manage().Contains(x,y))return {UiAction::Manage};
    if(view.tab==1&&l.Manage().Contains(x,y))return {UiAction::ImportPick};
    if(view.tab==1&&ModpackLayout(width,height).Create().Contains(x,y))return {UiAction::PackCreate};
    if(view.tab==2&&l.Manage().Contains(x,y))return {UiAction::ImportRepair};
    if(view.tab==5)return ImportHitTest(view,x,y,width,height);
    if(view.tab==6)return ModpackHitTest(view,x,y,width,height);
    if(view.tab==1)for(size_t i=0;i<l.Rows();++i){const size_t row=view.first+i;
        if(row<state.catalog.size()&&l.PackageSettings(i).Contains(x,y))return {UiAction::IniMod,row};
        if(row<state.catalog.size()&&l.Package(i).Contains(x,y)&&Selectable(state.catalog[row]))return {UiAction::Package,row};}
    if(view.tab==4)return IniHitTest(view,x,y,width,height);
    if(view.tab==2&&DescribeSetup().native&&l.Approve().Contains(x,y))return {UiAction::Approve};
    return {};
}
inline int ChromeHitTest(const View& view,float x,float y,float width,float height,bool maximized=false){
    if(x<0||y<0||x>=width||y>=height)return HTNOWHERE;
    if(!maximized){const bool left=x<6,right=x>=width-6,top=y<6,bottom=y>=height-6;
        if(top&&left)return HTTOPLEFT;if(top&&right)return HTTOPRIGHT;if(bottom&&left)return HTBOTTOMLEFT;if(bottom&&right)return HTBOTTOMRIGHT;
        if(left)return HTLEFT;if(right)return HTRIGHT;if(top)return HTTOP;if(bottom)return HTBOTTOM;}
    if(y<80&&HitTest(view,x,y,width,height).action==UiAction::None)return HTCAPTION;
    return HTCLIENT;
}
enum class UiEffect{None,Redraw,Minimize,Maximize,Close};
inline bool SelectedBinaryRuntimeMissing(){
    if(!BinaryRuntime(state.root).empty())return false;
    coop_packages::Plan plan;coop_packages::Error error;
    if(!coop_packages::BuildPlan(state.catalog,state.selected,plan,error))return false;
    for(const auto& id:plan.Packages())if(RealPath(PackageDirectory(id)+L"\\undercoop.binary.txt"))return true;
    return false;
}
inline UiEffect DispatchUi(View& view,UiHit hit){
    if (DispatchLiveNotice(view.notice, hit)) return UiEffect::Redraw;
    if(view.busy&&view.imports.worker.joinable()&&(hit.action==UiAction::ImportCancel||hit.action==UiAction::Close)){
        view.imports.cancel=true;if(hit.action==UiAction::Close)view.imports.closeAfter=true;SetImportStatus(view.imports,L"Cancelling safely. Completed requirements may remain available; existing packages are not overwritten.");return UiEffect::Redraw;}
    if(view.busy||view.accepted)return UiEffect::None;
    if(DispatchDeveloper(view,hit))return UiEffect::Redraw;
    if(DispatchUndercoop(view,hit)) {
        if(view.undercoop.backupLaunchRequested) {
            view.undercoop.backupLaunchRequested=false;
            return DispatchUi(view,{UiAction::Play});
        }
        return UiEffect::Redraw;
    }
    if(hit.action!=UiAction::None&&hit.action!=UiAction::Minimize&&hit.action!=UiAction::Maximize&&
       !(hit.action==UiAction::Tab&&hit.index==7)&&UndercoopDraftGuard(view))return UiEffect::Redraw;
    if(hit.action==UiAction::IniDiscard){SelectIniRow(view,view.ini.row);view.ini.message=L"Draft discarded.";return UiEffect::Redraw;}
    if(hit.action==UiAction::IniSave){const auto* row=IniRow(view.ini);if(!row||!IniEditable(view.ini))return UiEffect::None;
        std::wstring message;if(SaveIni(view.ini.opened,row->id,view.ini.value,message)){view.ini.dirty=false;SelectIniRow(view,view.ini.row);}view.ini.message=message;return UiEffect::Redraw;}
    if(hit.action!=UiAction::None&&hit.action!=UiAction::Minimize&&hit.action!=UiAction::Maximize&&IniDraftGuard(view))return UiEffect::Redraw;
    if(DispatchModpack(view,hit))return UiEffect::Redraw;
    if(hit.action==UiAction::ImportPick){view.tab=5;SyncIniEditor(view);PickImport(view);return UiEffect::Redraw;}
    if(hit.action==UiAction::ImportFolder){view.tab=5;SyncIniEditor(view);PickImportFolder(view);return UiEffect::Redraw;}
    if(hit.action==UiAction::ImportPackDetails){ShowImportPackDetails(view);return UiEffect::Redraw;}
    if(hit.action==UiAction::ImportInstall){BeginImport(view,ImportWork::Install);return UiEffect::Redraw;}
    if(hit.action==UiAction::ImportRepair){BeginImport(view,ImportWork::Repair);return UiEffect::Redraw;}
    if(hit.action==UiAction::ImportDownloads){view.imports.allowDownloads=!view.imports.allowDownloads;return UiEffect::Redraw;}
    if(hit.action==UiAction::ImportConsent){view.imports.codeAccepted=!view.imports.codeAccepted;return UiEffect::Redraw;}
    if(hit.action==UiAction::IniReload){RefreshIni(view);return UiEffect::Redraw;}
    if(hit.action==UiAction::IniAll){view.ini.ownerFilter.clear();RefreshIni(view);return UiEffect::Redraw;}
    if(hit.action==UiAction::IniFile){SelectIniFile(view,hit.index);return UiEffect::Redraw;}
    if(hit.action==UiAction::IniRow){SelectIniRow(view,hit.index);return UiEffect::Redraw;}
    if(hit.action==UiAction::IniGroup){ToggleIniGroup(view,hit.index);return UiEffect::Redraw;}
    if(hit.action==UiAction::IniMod&&hit.index<state.catalog.size()){view.tab=4;view.ini.ownerFilter=state.catalog[hit.index].id;RefreshIni(view);return UiEffect::Redraw;}
    if(hit.action==UiAction::Close)return UiEffect::Close;
    if(hit.action==UiAction::Minimize)return UiEffect::Minimize;
    if(hit.action==UiAction::Maximize)return UiEffect::Maximize;
    if(hit.action==UiAction::Tab&&hit.index<8){view.tab=int(hit.index);view.first=0;state.error.clear();if(view.tab==4)RefreshIni(view);if(view.tab==6)RefreshModpacks(view);if(view.tab==7)OpenUndercoopSettings(view);SyncIniEditor(view);SyncModpackEditor(view);SyncUndercoopEditors(view);return UiEffect::Redraw;}
    if(hit.action==UiAction::Manage){view.tab=1;view.first=0;state.error.clear();return UiEffect::Redraw;}
    if(hit.action==UiAction::Package&&view.tab==1&&hit.index<state.catalog.size()){
        const auto& package=state.catalog[hit.index];if(!Selectable(package))return UiEffect::None;
        auto found=std::find(state.selected.begin(),state.selected.end(),package.id);
        if(found==state.selected.end())state.selected.push_back(package.id);else state.selected.erase(found);
        state.nativeTrusted=false;
        state.error.clear();return UiEffect::Redraw;
    }
    if(hit.action==UiAction::Approve&&view.tab==2&&DescribeSetup().native){
        const auto warning=ImportedApprovalText();if(!state.nativeTrusted&&!warning.empty()&&(!view.confirmImported||!view.confirmImported(view.window,warning)))return UiEffect::None;
        state.nativeTrusted=!state.nativeTrusted;state.error.clear();return UiEffect::Redraw;}
    if(hit.action==UiAction::Play){
        if(state.fatal)return UiEffect::Close;
        live_notice::Tick(view.notice, GetTickCount64());
        const bool developerApproved = view.developer.TakePlayApproval();
        if (!view.notice.CanPlay() && !developerApproved) {
            if (view.notice.checked && view.developer.SignedIn()) {
                view.notice.open = false;
                view.developer.VerifyPlay();
                return UiEffect::Redraw;
            }
            view.notice.open = true;
            view.notice.seen = true;
            return UiEffect::Redraw;
        }
        const auto setup=DescribeSetup();if(!setup.valid){state.error=setup.error;view.tab=3;return UiEffect::Redraw;}
        if(setup.native&&!state.nativeTrusted){view.tab=2;state.error.clear();return UiEffect::Redraw;}
        if(SelectedBinaryRuntimeMissing()){
            state.error.clear();BeginImport(view,ImportWork::PlayRuntime);return UiEffect::Redraw;
        }
        view.busy=true;struct Reset{bool& value;~Reset(){value=false;}} reset{view.busy};
        if(view.activate&&view.activate()){view.accepted=true;return UiEffect::Close;}
        return state.fatal?UiEffect::Close:UiEffect::Redraw;
    }return UiEffect::None;
}
inline UiEffect CompleteImportUi(View& view){
    if(!PollImport(view))return UiEffect::None;
    const auto& panel=view.imports;
    if(panel.work==ImportWork::PlayRuntime){
        if(panel.okay&&!panel.cancel.load()&&!panel.closeAfter)return DispatchUi(view,{UiAction::Play});
        state.error=ImportStatus(view.imports);
    }
    return UiEffect::Redraw;
}
inline void ScrollUi(View& view,int direction,float width,float height){
    if(view.busy)return;
    if(view.tab==7 && view.undercoop.section==kBackupSection){
        if(direction&&!view.notice.open)StepBackupPage(view.undercoop,BackupLayout(UiLayout(width,height)).Rows(),direction<0);
        return;
    }
    if(view.tab==7 && view.undercoop.section==kSaveImportSection){
        if(direction&&!view.notice.open)StepCareerImportPage(view.undercoop,CareerImportLayout(UiLayout(width,height)).Rows(),direction<0);
        return;
    }
    if(view.tab==6){auto& p=view.modpacks;if(p.naming)return;const ModpackLayout l(width,height);const auto count=p.editing.empty()?p.packs.size():p.choices.size(),rows=p.editing.empty()?l.Rows():l.Members();const size_t maximum=count>rows?count-rows:0;p.first=(std::min)(p.first,maximum);if(direction<0&&p.first<maximum)++p.first;else if(direction>0&&p.first)--p.first;return;}if(view.tab!=1)return;const size_t rows=UiLayout(width,height).Rows();
    const size_t maximum=state.catalog.size()>rows?state.catalog.size()-rows:0;
    view.first=(std::min)(view.first,maximum);
    if(direction<0&&view.first<maximum)++view.first;else if(direction>0&&view.first)--view.first;
}
inline void DrawIcon(coop_manager_graphics::Renderer& r,int icon,float x,float y,unsigned color){
    if(icon==0){r.Line(x,y+7,x+8,y,color,1.5f);r.Line(x+8,y,x+16,y+7,color,1.5f);r.Line(x+3,y+6,x+3,y+16,color,1.5f);r.Line(x+13,y+6,x+13,y+16,color,1.5f);r.Line(x+3,y+16,x+13,y+16,color,1.5f);}
    if(icon==1){for(int i=0;i<4;++i)r.Box(x+float(i%2)*9,y+float(i/2)*9,6,6,color,1);}
    if(icon==2){r.Circle(x+4,y+5,3,color,false,1.5f);r.Circle(x+12,y+13,3,color,false,1.5f);r.Line(x+6,y+7,x+10,y+11,color,1.5f);}
    if(icon==3){r.Line(x+8,y,x,y+4,color,1.5f);r.Line(x+8,y,x+16,y+4,color,1.5f);r.Line(x,y+4,x+2,y+12,color,1.5f);r.Line(x+16,y+4,x+14,y+12,color,1.5f);r.Line(x+2,y+12,x+8,y+17,color,1.5f);r.Line(x+14,y+12,x+8,y+17,color,1.5f);}
    if(icon==4){r.Line(x,y+3,x+16,y+3,color);r.Line(x,y+8,x+16,y+8,color);r.Line(x,y+13,x+16,y+13,color);r.Circle(x+5,y+3,2,color);r.Circle(x+12,y+8,2,color);r.Circle(x+6,y+13,2,color);}
    if(icon==5){r.Line(x+8,y,x+8,y+12,color,1.5f);r.Line(x+3,y+7,x+8,y+12,color,1.5f);r.Line(x+13,y+7,x+8,y+12,color,1.5f);r.Line(x,y+16,x+16,y+16,color,1.5f);}
    if(icon==6){r.Box(x+1,y+2,11,11,0x141619,2,color);r.Line(x+5,y+16,x+16,y+16,color,1.5f);r.Line(x+16,y+6,x+16,y+16,color,1.5f);}
    if(icon==7){r.Circle(x+8,y+8,6,color,false,1.5f);r.Circle(x+8,y+8,2,color,false,1.5f);for(int n=0;n<4;++n){const float xx=n==0?x+8:n==1?x+8:n==2?x-1:x+14,yy=n==0?y-1:n==1?y+14:y+8;r.Line(xx,yy,n<2?xx:xx+3,n<2?yy+3:yy,color,1.5f);}}
}
inline void Paint(View& view,HDC dc,int pixelsWide,int pixelsHigh,float dpi=96){
    auto& r=view.renderer;if(!r.Begin(dc,pixelsWide,pixelsHigh,dpi))return;
    const float width=pixelsWide*96/dpi,height=pixelsHigh*96/dpi;const UiLayout l(width,height);const float cw=l.ContentWidth();
    const auto setup=DescribeSetup();const bool needsApproval=setup.native&&!state.nativeTrusted;
    auto label=[&](const wchar_t* text,float x,float y,float w,float h,float size=14,unsigned color=0xD6D8DD,UiFace face=UiFace::Regular,bool wrap=false){r.Text(text,x,y,w,h,size,color,face,wrap);};
    auto card=[&](float x,float y,float w,float h){r.Box(x,y,w,h,0x191B1E,9,0x2C2F34);};
    auto badge=[&](const wchar_t* text,float x,float y,float w){r.Box(x,y,w,25,0x292C31,12);r.Text(text,x,y,w,25,11,0xC7CBD3,UiFace::Bold,false,true);};
    r.Box(0,0,width,80,0x000000);r.Logo(26,24,310);r.Line(0,79,width,79,0x292B2F);
    label(L"MOD MANAGER",width-350,29,172,24,11,0x8A909B,UiFace::Bold);
    const UiRect controls[]={l.Minimize(),l.Maximize(),l.Close()};const UiAction controlActions[]={UiAction::Minimize,UiAction::Maximize,UiAction::Close};
    for(size_t i=0;i<3;++i){const auto box=controls[i];if(view.hover.action==controlActions[i])r.Box(box.x,box.y,box.w,box.h,i==2?0xA83C43:0x25282C);
        const float x=box.x+19,y=box.y+18;if(i==0)r.Line(x,y+6,x+10,y+6,0xD8DBE0,1.2f);
        if(i==1)r.Box(x,y,10,10,0x000000,0,0xD8DBE0);
        if(i==2){r.Line(x,y,x+10,y+10,0xD8DBE0,1.2f);r.Line(x+10,y,x,y+10,0xD8DBE0,1.2f);}}
    r.Box(0,80,212,height-80,0x141619);r.Line(211,80,211,height,0x292C31);
    const wchar_t* nav[]={L"Overview",L"My mods",L"Components",L"Compatibility",L"Mod settings",L"Add mods",L"Modpacks",L"Undercoop Settings"};
    for(size_t i=0;i<8;++i){const auto box=l.Navigation(i);const bool active=int(i)==view.tab,hover=view.hover==UiHit{UiAction::Tab,i};
        if(active||hover)r.Box(box.x,box.y,box.w,box.h,active?0x292D33:0x1D2025,7);
        if(active)r.Box(box.x,box.y+13,3,18,0xE2E5EB,1);DrawIcon(r,int(i),31,box.y+13,active?0xE9EBEF:0x858C98);
        label(nav[i],59,box.y+11,134,24,i==7?12.5f:14,active?0xF2F3F5:0x9AA1AC,active?UiFace::Bold:UiFace::Regular);
    }
    if(height>=680){label(L"WORKSPACE",26,height-146,160,20,10,0x686F7B,UiFace::Bold);r.Circle(30,height-112,3,0xAAB1BC);label(L"Local packages",42,height-123,145,23,12,0xA2A9B4);}
    label((Wide(UNDERCOOP_MANAGER_VERSION)+L" / Preview").c_str(),26,height-55,170,22,11,0x737B88);
    const wchar_t* titles[]={L"READY TO DRIVE",L"YOUR MODS",L"REQUIRED COMPONENTS",L"SETUP CHECK",L"MOD SETTINGS",L"ADD MODS",L"MODPACKS",L"UNDERCOOP SETTINGS"};
    const wchar_t* subtitles[]={L"Choose your setup. Keep your original game intact.",L"Enable the packages you want to use in your next session.",
        L"Know what your mods need before the game starts.",L"A clear view of what is ready and what still needs attention.",L"Your careers stay with this setup when options change. Keep both players' shared settings matching.",L"Review an archive or folder, then install inside mods only.",L"New modpacks copy your current careers. Switch your whole setup before launching.",L"Your connection, shared world and console. Set up this game copy before playing."};
    label(titles[view.tab],l.content,110,view.tab==7?cw-112:cw,37,view.tab==7?22.0f:25.0f,0xF0F2F6,UiFace::Heading);
    label(subtitles[view.tab],l.content,158,cw,25,13,0x9199A6);
    if(view.tab==0){
        const auto manage=l.Manage();r.Box(manage.x,manage.y,manage.w,manage.h,view.hover.action==UiAction::Manage?0x353A42:0x25292F,6);
        r.Text(L"Manage mods",manage.x,manage.y,manage.w,manage.h,12,0xD8DDE5,UiFace::Bold,false,true);
        card(l.content,l.body,cw,154);label((state.instanceId.empty()?L"CURRENT SETUP / CUSTOM":L"MODPACK / "+state.instanceName).c_str(),l.content+22,l.body+17,cw-44,22,10,0x8F97A4,UiFace::Bold);
        label(setup.name.c_str(),l.content+22,l.body+48,cw-160,41,26,0xF1F3F7,UiFace::Bold);
        std::wstring detail=std::to_wstring(setup.assets)+L" mod package(s)  /  "+std::to_wstring(setup.dependencies)+L" required component(s)";
        label(detail.c_str(),l.content+22,l.body+103,cw-44,25,13,0xA6ADBA);
        badge(!setup.valid?L"Check setup":needsApproval?L"Needs approval":L"Local setup",width-168,l.body+23,114);
        const float half=(cw-16)/2,y=l.body+172;card(l.content,y,half,118);card(l.content+half+16,y,half,118);
        DrawIcon(r,3,l.content+20,y+19,0x9AA4B3);label(L"Original game",l.content+46,y+16,half-62,25,15,0xE5E8ED,UiFace::Bold);
        label(L"Mod files stay in mods. Original assets are not replaced.",l.content+20,y+56,half-40,47,13,0x979FAB,UiFace::Regular,true);
        DrawIcon(r,1,l.content+half+36,y+19,0x9AA4B3);label(L"Separate careers",l.content+half+62,y+16,half-62,25,15,0xE5E8ED,UiFace::Bold);
        label(L"Each setup uses its own save area. Your other careers stay separate.",l.content+half+36,y+56,half-40,47,13,0x979FAB,UiFace::Regular,true);
        if(l.footer-(l.body+308)>=52)label(L"Add mods accepts archives and game-ready folders, and downloads known missing requirements. Folder metadata is created for you. Unknown installers are never executed.",l.content,l.body+308,cw,42,12,0x78818F,UiFace::Regular,true);
    }else if(view.tab==1){
        const auto create=ModpackLayout(width,height).Create();r.Box(create.x,create.y,create.w,create.h,0x303744,6);r.Text(L"Create modpack",create.x,create.y,create.w,create.h,12,0xD4DFEF,UiFace::Bold,false,true);
        const auto import=l.Manage();r.Box(import.x,import.y,import.w,import.h,0x303744,6);r.Text(L"Import archive",import.x,import.y,import.w,import.h,12,0xD4DFEF,UiFace::Bold,false,true);
        const size_t maxFirst=state.catalog.size()>l.Rows()?state.catalog.size()-l.Rows():0;view.first=(std::min)(view.first,maxFirst);
        if(state.catalog.empty()){card(l.content,l.body,cw,122);label(L"No local packages found",l.content+22,l.body+22,cw-44,30,19,0xE7EAF0,UiFace::Bold);label(L"Add a supported package inside your mods folder.",l.content+22,l.body+68,cw-44,30,13,0x959DAB);}
        for(size_t i=0;i<l.Rows()&&view.first+i<state.catalog.size();++i){const size_t index=view.first+i;const auto& p=state.catalog[index];const auto box=l.Package(i);
            const bool dependency=p.kind==coop_packages::Kind::Dependency;
            const bool enabled=dependency?std::find(setup.required.begin(),setup.required.end(),p.id)!=setup.required.end():std::find(state.selected.begin(),state.selected.end(),p.id)!=state.selected.end();
            r.Box(box.x,box.y,box.w,box.h,view.hover==UiHit{UiAction::Package,index}?0x22262D:0x191B1F,8,0x2C3037);
            r.Box(box.x+16,box.y+17,34,34,0x282D35,7);DrawIcon(r,dependency?2:1,box.x+25,box.y+25,0xAAB2C0);
            label(Wide(p.name).c_str(),box.x+64,box.y+12,box.w-264,25,16,0xE8EBF0,UiFace::Bold);
            label((Wide(p.version)+L"  /  "+(ImportedNativeKnown(p)?L"Imported native plugin":dependency?enabled?L"Required component":L"Available component":L"Mod package")).c_str(),box.x+64,box.y+40,box.w-264,21,12,0x858F9F);
            const auto settings=l.PackageSettings(i);r.Box(settings.x,settings.y,settings.w,settings.h,0x303744,5);r.Text(L"Settings",settings.x,settings.y,settings.w,settings.h,11,0xCAD4E3,UiFace::Bold,false,true);
            if(dependency&&!Selectable(p)){const auto required=l.RequiredBadge(i);badge(enabled?L"Required":L"Available",required.x,required.y,required.w);}
            else{r.Box(box.x+box.w-69,box.y+24,44,23,enabled?0xDEE3EB:0x3B424D,12);r.Circle(box.x+box.w-(enabled?37:57),box.y+35.5f,8,enabled?0x252A32:0x8B94A4);}
        }
        label((state.instanceId.empty()?L"Custom setup. Changes take effect when you launch.":L"Active modpack: "+state.instanceName+L". Private settings and versions.").c_str(),l.content,l.footer-27,cw,22,12,0x778292);
    }else if(view.tab==2){
        const auto repair=l.Manage();r.Box(repair.x,repair.y,repair.w,repair.h,0x303744,6);r.Text(L"Get requirements",repair.x,repair.y,repair.w,repair.h,11,0xD4DFEF,UiFace::Bold,false,true);
        card(l.content,l.body,cw,222);DrawIcon(r,2,l.content+22,l.body+23,0xB0B8C5);
        label(!setup.valid?L"Requirements need attention":setup.native?L"Selected game components":L"No extra components needed",l.content+52,l.body+17,cw-210,31,20,0xE8ECF2,UiFace::Bold);
        badge(!setup.valid?L"Needs attention":!setup.native?L"Not required":state.nativeTrusted?L"Approved":L"Approval needed",width-178,l.body+20,126);
        if(setup.native){
            for(size_t i=0;i<setup.components.size()&&i<4;++i)label(setup.components[i].c_str(),l.content+22,l.body+59+float(i)*21,cw-44,23,13,0xB0B8C5);
            const auto box=l.Approve();r.Box(box.x,box.y,box.w,box.h,state.nativeTrusted?0x2B313A:0xDDE2EB,6);
            r.Text(state.nativeTrusted?L"Approved - click to revoke":L"Approve selected components",box.x,box.y,box.w,box.h,13,state.nativeTrusted?0xCBD2DF:0x171A20,UiFace::Bold,false,true);
        }else label(!setup.valid?setup.error.c_str():L"Your selected setup does not require a managed native component.",l.content+22,l.body+83,cw-44,55,14,0xB0B8C5,UiFace::Regular,true);
        label(L"Built-in adapters are hash-pinned. Imported native plugins require separate explicit approval and are NOT proven CO-OP-compatible. Installers are never executed.",l.content,l.body+246,cw,55,13,0x828D9F,UiFace::Regular,true);
    }else if(view.tab==3){
        card(l.content,l.body,cw,133);DrawIcon(r,3,l.content+22,l.body+24,0xB8C0CE);
        label(setup.valid?L"Package plan is ready":L"Your setup needs attention",l.content+52,l.body+19,cw-76,31,21,0xE9EDF4,UiFace::Bold);
        label(setup.valid?L"Package ownership and declared dependencies resolve. File hashes are checked when you launch.":setup.error.c_str(),l.content+22,l.body+72,cw-44,48,14,0x9EA8B8,UiFace::Regular,true);
        const wchar_t* lines[]={L"Original files remain separate",L"Components are verified before loading",L"The CO-OP lobby compares both players' active setups"};
        for(int i=0;i<3;++i){const float y=l.body+159+i*44;r.Circle(l.content+7,y+9,3,0x9AA5B5);label(lines[i],l.content+24,y,cw-24,26,14,0xABB5C5);}
    }else if(view.tab==4)PaintIni(view,width,height);else if(view.tab==6)PaintModpacks(view,width,height);else if(view.tab==7)PaintUndercoop(view,width,height);else PaintImport(view,width,height);
    r.Box(212,l.footer,width-212,84,0x15171B);r.Line(212,l.footer,width,l.footer,0x2B3037);
    const wchar_t* statusFooter = NoticeFooter(view.notice);
    if (!view.notice.CanPlay() && !view.developer.Message().empty())
        statusFooter = view.developer.Message().c_str();
    const float footerWidth = cw - (view.notice.Visible() ? 270 : 216);
    const float footerHeight = statusFooter ? 40.0f : 60.0f;
    if(view.tab==7)label(UndercoopFooter(view.undercoop),l.content,l.footer+14,footerWidth,footerHeight,12,0xCBD5E4,UiFace::Regular,true);
    else if(view.tab==5&&!ImportStatus(view.imports).empty())label(ImportStatus(view.imports).c_str(),l.content,l.footer+12,footerWidth,footerHeight,12,0xCBD5E4,UiFace::Regular,true);
    else if(view.tab==4&&!view.ini.message.empty())label(view.ini.message.c_str(),l.content,l.footer+16,footerWidth,footerHeight,12,0xCBD5E4,UiFace::Regular,true);
    else if(!state.error.empty())label(state.error.c_str(),l.content,l.footer+16,footerWidth,footerHeight,13,0xE0C5C6,UiFace::Regular,true);
    else if(view.tab==6&&!view.modpacks.message.empty())label(view.modpacks.message.c_str(),l.content,l.footer+12,footerWidth,footerHeight,12,0xCBD5E4,UiFace::Regular,true);
    else{label(view.busy?L"Preparing your setup...":!setup.valid?L"Your setup needs attention":needsApproval?L"Components need your approval":L"Your setup will be checked before launch",l.content,l.footer+18,footerWidth,25,13,0xCFD6E2,UiFace::Bold);
        if (!statusFooter) label(L"Local packages / verified requirements",l.content,l.footer+44,footerWidth,23,11,0x737F91);}
    if (statusFooter) label(statusFooter,l.content,l.footer+56,footerWidth,24,11,NoticeColor(view.notice));
    const auto play = l.Play();
    const bool developerAvailable = view.notice.checked && view.developer.SignedIn();
    const bool allowed = view.notice.CanPlay() || developerAvailable;
    const unsigned ink = allowed ? 0x171A20 : 0xA8B3C1;
    const unsigned fill = !allowed ? 0x343D49 : view.busy ? 0x9AA3B0 :
        view.hover.action == UiAction::Play ? 0xFFFFFF : 0xE2E7EF;
    const wchar_t* playText = view.busy || (view.notice.enabled && !view.notice.checked) ||
        (developerAvailable && view.developer.Busy() && !view.notice.CanPlay()) ? L"WAIT" :
        !allowed && !view.notice.status.playEnabled ? L"LOCKED" : L"PLAY";
    r.Box(play.x, play.y, play.w, play.h, fill, 7);
    r.Text(playText, play.x+14, play.y, play.w-40, play.h, 14, ink, UiFace::Heading, false, true);
    r.Line(play.x+147,play.y+17,play.x+153,play.y+22,ink,1.6f);r.Line(play.x+153,play.y+22,play.x+147,play.y+27,ink,1.6f);
    PaintLiveNotice(r, view.notice, l, view.hover);
    r.End();
}
inline void ApplyUiEffect(View& view,UiEffect effect){
    if(effect==UiEffect::Close){DestroyWindow(view.window);return;}
    if(effect==UiEffect::Minimize)ShowWindow(view.window,SW_MINIMIZE);
    else if(effect==UiEffect::Maximize)ShowWindow(view.window,IsZoomed(view.window)?SW_RESTORE:SW_MAXIMIZE);
    else if(effect==UiEffect::Redraw){SyncLauncherEditors(view);InvalidateRect(view.window,nullptr,FALSE);}
}
inline LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    auto* view=reinterpret_cast<View*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){view=static_cast<View*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);view->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(view));}
    if(!view)return DefWindowProcW(window,message,wp,lp);
    if (message == WM_DESTROY) ClearDeveloperPassword(view->developerPanel.password);
    if (message == WM_TIMER && wp == live_notice::kTimer) {
        const auto now = GetTickCount64();
        const bool open = view->notice.open;
        const bool statusChanged = live_notice::Tick(view->notice, now);
        const bool developerChanged = view->developer.Tick();
        if (view->developer.HasPlayApproval()) {
            const auto effect = DispatchUi(*view, {UiAction::Play});
            view->developer.TakePlayApproval();
            ApplyUiEffect(*view, effect);
        } else if (statusChanged || developerChanged) {
            if (open != view->notice.open || developerChanged) SyncLauncherEditors(*view);
            InvalidateRect(window, nullptr, FALSE);
        } else if (view->notice.Pulsing(now)) InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if(message==WM_TIMER&&wp==kImportTimer){ApplyUiEffect(*view,CompleteImportUi(*view));if(IsWindow(window))InvalidateRect(window,nullptr,FALSE);return 0;}
    if(message==kImportComplete){ApplyUiEffect(*view,CompleteImportUi(*view));if(IsWindow(window))InvalidateRect(window,nullptr,FALSE);return 0;}
    if(message==WM_DROPFILES){const auto drop=reinterpret_cast<HDROP>(wp);wchar_t path[MAX_PATH]{};
        const bool one=DragQueryFileW(drop,0xffffffff,nullptr,0)==1&&DragQueryFileW(drop,0,nullptr,0)<MAX_PATH&&DragQueryFileW(drop,0,path,MAX_PATH)>0;DragFinish(drop);
        if(!view->notice.open&&!view->busy&&one&&!IniDraftGuard(*view)&&!UndercoopDraftGuard(*view)){BeginImport(*view,view->tab==6?ImportWork::PackInspect:ImportWork::Inspect,path);SyncLauncherEditors(*view);InvalidateRect(window,nullptr,FALSE);}return 0;}
    if (message == kDeveloperSubmitMessage || message == kDeveloperBackMessage) {
        ApplyUiEffect(*view, DispatchUi(*view, {message == kDeveloperSubmitMessage ?
            UiAction::DeveloperSubmit : UiAction::DeveloperBack}));
        return 0;
    }
    if ((message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC) && lp &&
        (reinterpret_cast<HWND>(lp) == view->developerPanel.email ||
         reinterpret_cast<HWND>(lp) == view->developerPanel.password) && view->developerPanel.brush) {
        const auto dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(220, 227, 237));
        SetBkColor(dc, RGB(30, 34, 40));
        return reinterpret_cast<LRESULT>(view->developerPanel.brush);
    }
    if(message==kUndercoopSaveMessage){ApplyUiEffect(*view,DispatchUi(*view,{UiAction::UndercoopSave}));return 0;}
    if(message==kUndercoopDiscardMessage){ApplyUiEffect(*view,DispatchUi(*view,{UiAction::UndercoopDiscard}));return 0;}
    for(size_t i=0;i<view->undercoop.edits.size();++i)if(view->undercoop.edits[i]&&reinterpret_cast<HWND>(lp)==view->undercoop.edits[i]){
        auto& p=view->undercoop;if(message==WM_COMMAND&&HIWORD(wp)==EN_CHANGE&&!p.suppress){const int length=GetWindowTextLengthW(p.edits[i]);std::wstring text(size_t(length)+1,0);GetWindowTextW(p.edits[i],text.data(),length+1);text.resize(size_t(length));undercoop_settings::Set(p.draft,UndercoopInputField(i),text);p.message.clear();InvalidateRect(window,nullptr,FALSE);return 0;}
        if(message==WM_CTLCOLOREDIT||message==WM_CTLCOLORSTATIC){SetTextColor(reinterpret_cast<HDC>(wp),RGB(220,227,237));SetBkColor(reinterpret_cast<HDC>(wp),RGB(30,34,40));return reinterpret_cast<LRESULT>(p.brush);}
    }
    if(message==kIniSaveMessage){ApplyUiEffect(*view,DispatchUi(*view,{UiAction::IniSave}));return 0;}
    if(message==kIniDiscardMessage){ApplyUiEffect(*view,DispatchUi(*view,{UiAction::IniDiscard}));return 0;}
    if(message==kModpackNameConfirm){ApplyUiEffect(*view,DispatchUi(*view,{UiAction::PackConfirm}));return 0;}
    if(message==kModpackNameCancel){ApplyUiEffect(*view,DispatchUi(*view,{UiAction::PackCancel}));return 0;}
    if(message==WM_COMMAND&&reinterpret_cast<HWND>(lp)==view->modpacks.edit&&HIWORD(wp)==EN_CHANGE&&!view->modpacks.suppress){wchar_t text[65]{};GetWindowTextW(view->modpacks.edit,text,65);view->modpacks.name=text;return 0;}
    if((message==WM_CTLCOLOREDIT||message==WM_CTLCOLORSTATIC)&&reinterpret_cast<HWND>(lp)==view->modpacks.edit){SetTextColor(reinterpret_cast<HDC>(wp),RGB(219,228,242));SetBkColor(reinterpret_cast<HDC>(wp),RGB(30,34,40));return reinterpret_cast<LRESULT>(view->modpacks.brush);}
    if(message==WM_COMMAND&&reinterpret_cast<HWND>(lp)==view->ini.edit&&HIWORD(wp)==EN_CHANGE&&!view->ini.suppress){
        const int length=GetWindowTextLengthW(view->ini.edit);view->ini.value.assign(size_t(length)+1,0);GetWindowTextW(view->ini.edit,view->ini.value.data(),length+1);view->ini.value.resize(size_t(length));
        const auto* row=IniRow(view->ini);view->ini.dirty=row&&view->ini.value!=IniWide(row->value,view->ini.opened.codepage);InvalidateRect(window,nullptr,FALSE);return 0;}
    if((message==WM_CTLCOLOREDIT||message==WM_CTLCOLORSTATIC)&&reinterpret_cast<HWND>(lp)==view->ini.edit){SetTextColor(reinterpret_cast<HDC>(wp),RGB(219,228,242));SetBkColor(reinterpret_cast<HDC>(wp),RGB(30,34,40));return reinterpret_cast<LRESULT>(view->ini.brush);}
    auto dimensions=[&](){RECT c{};GetClientRect(window,&c);const float scale=96/coop_overlay::WindowDpi(window);return UiLayout(c.right*scale,c.bottom*scale);};
    auto point=[&](){const float scale=96/coop_overlay::WindowDpi(window);return D2D1::Point2F(short(LOWORD(lp))*scale,short(HIWORD(lp))*scale);};
    if(message==WM_NCCALCSIZE)return 0;
    if(message==WM_NCACTIVATE)return TRUE;
    if(message==WM_NCHITTEST){POINT p{short(LOWORD(lp)),short(HIWORD(lp))};ScreenToClient(window,&p);const float s=96/coop_overlay::WindowDpi(window);const auto l=dimensions();return ChromeHitTest(*view,p.x*s,p.y*s,l.width,l.height,IsZoomed(window)!=FALSE);}
    if(message==WM_GETMINMAXINFO){auto* info=reinterpret_cast<MINMAXINFO*>(lp);const float s=coop_overlay::WindowDpi(window)/96;
        info->ptMinTrackSize={LONG(920*s),LONG(620*s)};MONITORINFO monitor{sizeof(monitor)};
        if(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)){info->ptMaxPosition={monitor.rcWork.left-monitor.rcMonitor.left,monitor.rcWork.top-monitor.rcMonitor.top};
            info->ptMaxSize={monitor.rcWork.right-monitor.rcWork.left,monitor.rcWork.bottom-monitor.rcWork.top};}return 0;}
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT){PAINTSTRUCT paint{};const HDC dc=BeginPaint(window,&paint);RECT rect{};GetClientRect(window,&rect);
        if(rect.right>0&&rect.bottom>0){HDC memory=CreateCompatibleDC(dc);HBITMAP bitmap=memory?CreateCompatibleBitmap(dc,rect.right,rect.bottom):nullptr;
            if(memory&&bitmap){auto old=SelectObject(memory,bitmap);Paint(*view,memory,rect.right,rect.bottom,coop_overlay::WindowDpi(window));BitBlt(dc,0,0,rect.right,rect.bottom,memory,0,0,SRCCOPY);SelectObject(memory,old);}
            if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);}EndPaint(window,&paint);return 0;}
    if(message==WM_SIZE){SyncLauncherEditors(*view);InvalidateRect(window,nullptr,FALSE);return 0;}
    if(message==WM_DPICHANGED){auto* r=reinterpret_cast<RECT*>(lp);SetWindowPos(window,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    if(message==WM_MOUSEMOVE){const auto p=point();const auto l=dimensions();const auto hit=HitTest(*view,p.x,p.y,l.width,l.height);
        if(view->tab==7&&view->pressed.action==UiAction::UndercoopOpacity){DispatchUndercoop(*view,{UiAction::UndercoopOpacity,size_t(UndercoopLayout(l.width,l.height).OpacityAt(p.x))});InvalidateRect(window,nullptr,FALSE);}
        if(hit!=view->hover){view->hover=hit;InvalidateRect(window,nullptr,FALSE);}if(!view->tracking){TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,window,0};TrackMouseEvent(&tracking);view->tracking=true;}return 0;}
    if(message==WM_MOUSELEAVE){view->tracking=false;view->hover={};InvalidateRect(window,nullptr,FALSE);return 0;}
    if(message==WM_SETCURSOR&&LOWORD(lp)==HTCLIENT){SetCursor(LoadCursorW(nullptr,MAKEINTRESOURCEW(view->busy?32514:view->hover.action==UiAction::None?32512:32649)));return TRUE;}
    if(message==WM_LBUTTONDOWN){if(view->busy&&!view->imports.worker.joinable())return 0;const auto p=point();const auto l=dimensions();view->pressed=HitTest(*view,p.x,p.y,l.width,l.height);
        if(view->pressed.action==UiAction::UndercoopOpacity){DispatchUi(*view,view->pressed);InvalidateRect(window,nullptr,FALSE);}
        if(view->pressed.action!=UiAction::None)SetCapture(window);return 0;}
    if(message==WM_LBUTTONUP){const auto pressed=view->pressed;view->pressed={};if(GetCapture()==window)ReleaseCapture();const auto p=point();const auto l=dimensions();
        const auto hit=HitTest(*view,p.x,p.y,l.width,l.height);if(hit.action!=UiAction::None&&hit==pressed){if(hit.action==UiAction::Play)SetCursor(LoadCursorW(nullptr,MAKEINTRESOURCEW(32514)));
            ApplyUiEffect(*view,DispatchUi(*view,hit));if(IsWindow(window))SetCursor(LoadCursorW(nullptr,MAKEINTRESOURCEW(32512)));}return 0;}
    if(message==WM_CAPTURECHANGED||message==WM_CANCELMODE){view->pressed={};if(message==WM_CANCELMODE&&GetCapture()==window)ReleaseCapture();return 0;}
    if(message==WM_MOUSEWHEEL){const auto l=dimensions();const int delta=short(HIWORD(wp));
        if (view->notice.open) {
            view->notice.scroll = (std::max)(0.0f, (std::min)(view->notice.maxScroll,
                view->notice.scroll - 54.0f * delta / WHEEL_DELTA));
        } else if(view->tab==4){POINT p{short(LOWORD(lp)),short(HIWORD(lp))};ScreenToClient(window,&p);const IniLayout ini(l.width,l.height);const bool files=p.x*96/coop_overlay::WindowDpi(window)<ini.right;
            auto& first=files?view->ini.fileFirst:view->ini.rowFirst;const size_t count=files?IniTree(view->ini).size():view->ini.opened.document.Rows().size(),rows=files?ini.Files():ini.Rows();
            const size_t maximum=count>rows?count-rows:0;first=(std::min)(first,maximum);if(delta<0&&first<maximum)++first;else if(delta>0&&first)--first;
        }else ScrollUi(*view,delta,l.width,l.height);InvalidateRect(window,nullptr,FALSE);return 0;}
    if(message==WM_CLOSE){ApplyUiEffect(*view,DispatchUi(*view,{UiAction::Close}));return 0;}
    if (message == WM_KEYDOWN && wp == VK_F1 && view->notice.Visible()) {
        ApplyUiEffect(*view, DispatchUi(*view, {UiAction::NoticeToggle}));
        return 0;
    }
    if (message == WM_KEYDOWN && view->notice.open) {
        if (wp == VK_ESCAPE || wp == VK_RETURN) {
            ApplyUiEffect(*view, DispatchUi(*view, {wp == VK_ESCAPE || !view->notice.NeedsAcknowledgment() ?
                UiAction::NoticeClose : UiAction::NoticeAcknowledge, view->notice.generation}));
        } else if (wp == VK_HOME || wp == VK_END || wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT) {
            const float step = wp == VK_PRIOR || wp == VK_NEXT ? 130.0f : 24.0f;
            const float offset = (wp == VK_UP || wp == VK_PRIOR) ? -step : step;
            view->notice.scroll = wp == VK_HOME ? 0.0f : wp == VK_END ? view->notice.maxScroll :
                (std::max)(0.0f, (std::min)(view->notice.maxScroll, view->notice.scroll + offset));
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    if (message == WM_KEYDOWN && wp == VK_ESCAPE && DeveloperPageOpen(*view)) {
        ApplyUiEffect(*view, DispatchUi(*view, {UiAction::DeveloperBack}));
        return 0;
    }
    if(message==WM_KEYDOWN&&wp=='S'&&(GetKeyState(VK_CONTROL)&0x8000)&&view->tab==7&&!DeveloperPageOpen(*view)){ApplyUiEffect(*view,DispatchUi(*view,{UiAction::UndercoopSave}));return 0;}
    if(message==WM_KEYDOWN&&wp==VK_TAB&&view->tab==7){const auto next=GetNextDlgTabItem(window,nullptr,(GetKeyState(VK_SHIFT)&0x8000)!=0);if(next)SetFocus(next);return 0;}
    if(message==WM_KEYDOWN&&wp==VK_ESCAPE){ApplyUiEffect(*view,DispatchUi(*view,{view->modpacks.naming||!view->modpacks.editing.empty()?UiAction::PackCancel:view->ini.dirty?UiAction::IniDiscard:undercoop_settings::Dirty(view->undercoop.draft)?UiAction::UndercoopDiscard:view->busy&&view->imports.worker.joinable()?UiAction::ImportCancel:UiAction::Close}));return 0;}
    return DefWindowProcW(window,message,wp,lp);
}
inline bool Show(HMODULE module,const std::wstring& root){
    if(!Discover(root)){
        if(!ModpackRecoveryAvailable()||MessageBoxW(nullptr,L"The selected modpack was deleted or cannot be read. Your custom mod library and its saved selection are valid.\n\nUse the custom setup instead? This only clears the unavailable modpack choice. No modpack, mod setting or career is deleted or reset.",L"Undercoop - unavailable modpack",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES||!UseModLibrary())return false;
    }
    coop_manager_graphics::ComScope apartment;if(!apartment.Ready())return Fail(L"The launcher renderer could not initialize.");
    View view;view.module=module;view.listBackups=&ListCareerBackups;view.restoreBackup=&SelectCareerBackup;
    if(!view.renderer.Initialize(module))return Fail(L"The embedded launcher fonts or logo could not initialize.");
    coop_overlay::DpiScope dpi;WNDCLASSW wc{};wc.hInstance=module;wc.lpfnWndProc=WindowProc;wc.lpszClassName=L"Undercoop.ModManager.Modern";
    wc.style=CS_DBLCLKS;wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
    if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return Fail(L"Cannot register the manager window.");
    const auto dpiApi=reinterpret_cast<UINT(WINAPI*)()>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForSystem"));const UINT initialDpi=dpiApi?dpiApi():96;
    POINT position{};GetCursorPos(&position);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(position,MONITOR_DEFAULTTOPRIMARY),&monitor);
    const int width=(std::min)(MulDiv(1040,initialDpi,96),int(monitor.rcWork.right-monitor.rcWork.left-24));
    const int height=(std::min)(MulDiv(720,initialDpi,96),int(monitor.rcWork.bottom-monitor.rcWork.top-24));
    const DWORD style=WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX;
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"Undercoop - Mod Manager",style,monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2,
        monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2,width,height,nullptr,nullptr,module,&view);
    if(!window)return Fail(L"Cannot open the manager window.");
    // Creation on another monitor does not guarantee WM_DPICHANGED. Correct
    // the initial size while still hidden, using this window's actual DPI.
    const UINT windowDpi=UINT(coop_overlay::WindowDpi(window));
    if(windowDpi!=initialDpi){const int adjustedWidth=(std::min)(MulDiv(1040,windowDpi,96),int(monitor.rcWork.right-monitor.rcWork.left-24));
        const int adjustedHeight=(std::min)(MulDiv(720,windowDpi,96),int(monitor.rcWork.bottom-monitor.rcWork.top-24));
        SetWindowPos(window,nullptr,monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-adjustedWidth)/2,
            monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-adjustedHeight)/2,adjustedWidth,adjustedHeight,SWP_NOZORDER|SWP_NOACTIVATE);}
    const BOOL dark=TRUE;DwmSetWindowAttribute(window,20,&dark,sizeof(dark));const DWORD rounded=2;DwmSetWindowAttribute(window,33,&rounded,sizeof(rounded));
    view.notice.enabled = true;
    view.notice.started = GetTickCount64();
    if (SetTimer(window, live_notice::kTimer, 120, nullptr))
        live_notice::Tick(view.notice, view.notice.started);
    else view.notice.checked = true;
    DragAcceptFiles(window,TRUE);ShowWindow(window,SW_SHOW);UpdateWindow(window);MSG message{};
    while(IsWindow(window)){const int got=GetMessageW(&message,nullptr,0,0);if(got<=0)break;TranslateMessage(&message);DispatchMessageW(&message);}
    view.renderer.Shutdown();return view.accepted;
}
