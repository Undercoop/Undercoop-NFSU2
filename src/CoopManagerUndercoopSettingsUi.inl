// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Dedicated controls for Undercoop, separate from third-party mod INI files.
using UndercoopField=undercoop_settings::Field;
constexpr int kBackupSection=4;
constexpr int kSaveImportSection=5;
constexpr int kUndercoopCategories[]={0,1,2,kBackupSection,kSaveImportSection};
struct UndercoopPanel {
    undercoop_settings::Draft draft;
    int section=0;bool opened=false,suppress=false,backupLaunchRequested=false;
    std::wstring message;
    std::vector<coop_backup::Record> backups;
    size_t backupPage=0;
    std::vector<CareerImportSource> careerImports;
    size_t careerImportPage=0;
    std::array<HWND,4> edits{};HFONT font=nullptr;HBRUSH brush=nullptr;
    HANDLE privateFont=nullptr;UINT fontDpi=0;
    UndercoopPanel()=default;UndercoopPanel(const UndercoopPanel&)=delete;UndercoopPanel& operator=(const UndercoopPanel&)=delete;
    ~UndercoopPanel(){for(auto edit:edits)if(edit&&IsWindow(edit))DestroyWindow(edit);if(font)DeleteObject(font);if(brush)DeleteObject(brush);if(privateFont)RemoveFontMemResourceEx(privateFont);}
};
struct UndercoopLayout {
    UiLayout base;UiRect card;float row;
    explicit UndercoopLayout(float w,float h):base(w,h),card{base.content,248,base.ContentWidth(),(std::min)(286.0f,base.footer-314)},row(card.h/3){}
    UiRect Category(size_t i)const{
        const float step=(std::min)(126.0f,(DeveloperSignIn().x-base.content-12)/std::size(kUndercoopCategories));
        return {base.content+float(i)*step,198,step-8,34};
    }
    UiRect DeveloperSignIn() const { return {base.width - 194, 198, 164, 34}; }
    UiRect Role(size_t i)const{return {card.x+card.w-320+float(i)*100,card.y+(row-34)/2,100,34};}
    UiRect Input(size_t i)const{
        if(i==0)return {card.x+card.w-320,card.y+row+(row-36)/2,300,36};
        if(i==1||i==2)return {card.x+card.w-320+float(i-1)*158,card.y+row*2+(row-22)/2,142,34};
        return {card.x+card.w-320,card.y+card.h*0.75f+(card.h*0.25f-34)/2,300,34};
    }
    UiRect Toggle(size_t i)const{return {card.x+card.w-66,card.y+(float(i)+0.5f)*(card.h/4)-12,44,24};}
    UiRect ShowMode(size_t i)const{return {card.x+card.w-320+float(i)*100,card.y+20,100,34};}
    UiRect Opacity()const{return {card.x+card.w-306,card.y+90,266,32};}
    UiRect Save()const{return {base.width-182,base.footer-46,152,34};}
    UiRect Discard()const{return {base.width-292,base.footer-46,98,34};}
    UiRect Reload()const{return {base.width-132,119,102,32};}
    int OpacityAt(float x)const{const auto b=Opacity();const float f=(std::max)(0.0f,(std::min)(1.0f,(x-b.x)/b.w));return 10+5*int(f*18+0.5f);}
};
struct CareerImportLayout {
    UiLayout base;
    explicit CareerImportLayout(const UiLayout& layout):base(layout){}
    UiRect Options()const{return {base.content,248,base.ContentWidth(),89};}
    UiRect Choose()const{const auto box=Options();return {box.x+box.w-151,box.y+25,133,36};}
    UiRect Row(size_t i)const{return {base.content,377+51*float(i),base.ContentWidth(),44};}
    size_t Rows()const{return size_t((std::max)(1,int((base.footer-55-370)/51)));}
    UiRect PageButton(bool next)const{return {base.content+(next?164.0f:0.0f),base.footer-46,82,34};}
};
inline size_t CareerImportPages(const UndercoopPanel& panel,size_t rows){
    return panel.careerImports.empty()?1:1+(panel.careerImports.size()-1)/rows;
}
inline size_t VisibleCareerImportPage(const UndercoopPanel& panel,size_t rows){
    return (std::min)(panel.careerImportPage,CareerImportPages(panel,rows)-1);
}
inline void StepCareerImportPage(UndercoopPanel& panel,size_t rows,bool next){
    const auto page=VisibleCareerImportPage(panel,rows);
    panel.careerImportPage=next?(std::min)(page+1,CareerImportPages(panel,rows)-1):page-(page>0);
}
template<class V> inline void RefreshCareerImportPanel(V& view){
    auto& panel=view.undercoop;
    panel.careerImports=view.listCareerImports?view.listCareerImports():std::vector<CareerImportSource>{};
    panel.careerImportPage=0;
}
inline bool ConfirmCareerImport(HWND owner,const std::wstring& profile){
    const auto warning=L"Import "+profile+L" into the active CO-OP setup for both Host and Player 2?\n\n"
        L"Your teammate must import the same original save file into their matching setup before you play. "
        L"Both of you should use this campaign name.";
    return MessageBoxW(owner,warning.c_str(),L"Undercoop - import career",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES;
}
template<class V> inline void ConfirmAndImportCareer(V& view,const std::wstring& path){
    auto& panel=view.undercoop;
    if(!view.previewCareerImport||!view.importCareer||!view.confirmCareerImport)return;
    std::wstring profile,message;
    if(!view.previewCareerImport(path,profile,message)){
        panel.message=message.empty()?L"This career file could not be verified.":std::move(message);return;
    }
    if(!view.confirmCareerImport(view.window,profile))return;
    const bool imported=view.importCareer(path,profile,message);
    panel.message=message.empty()?(imported?L"Career imported for CO-OP.":L"This career could not be imported."):std::move(message);
}
template<class V> inline void ChooseCareerImportFile(V& view){
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=view.window;
    dialog.lpstrFilter=L"NFSU2 profile files\0*.*\0";dialog.lpstrFile=path;dialog.nMaxFile=DWORD(std::size(path));
    dialog.lpstrTitle=L"Select an NFS Underground 2 career file";
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT;
    if(GetOpenFileNameW(&dialog))ConfirmAndImportCareer(view,path);
}
inline const wchar_t* UndercoopFooter(const UndercoopPanel& panel){
    if(!panel.message.empty())return panel.message.c_str();
    if(panel.section==kBackupSection)return L"Choose a backup, then launch and select Resume Career in the CO-OP lobby.";
    if(panel.section==kSaveImportSection)return L"Ask your teammate to import the same original save into their matching setup.";
    return L"Undercoop settings belong to this copy.";
}
struct BackupLayout {
    UiLayout base;
    explicit BackupLayout(const UiLayout& layout):base(layout){}
    UiRect Options()const{return {base.content,248,base.ContentWidth(),64};}
    UiRect Toggle()const{const auto box=Options();return {box.x+box.w-66,box.y+20,44,24};}
    UiRect Row(size_t i)const{return {base.content,350+54*float(i),base.ContentWidth(),48};}
    size_t Rows()const{return size_t((std::max)(1,int((base.footer-60-350)/54)));}
    UiRect PageButton(bool next)const{return {base.content+(next?164.0f:0.0f),base.footer-46,82,34};}
};
inline size_t BackupPages(const UndercoopPanel& panel,size_t rows){
    return panel.backups.empty()?1:1+(panel.backups.size()-1)/rows;
}
inline size_t VisibleBackupPage(const UndercoopPanel& panel,size_t rows){
    return (std::min)(panel.backupPage,BackupPages(panel,rows)-1);
}
inline void StepBackupPage(UndercoopPanel& panel,size_t rows,bool next){
    const auto page=VisibleBackupPage(panel,rows);
    panel.backupPage=next?(std::min)(page+1,BackupPages(panel,rows)-1):page-(page>0);
}
template<class V> inline void RefreshBackupPanel(V& view){
    auto& panel=view.undercoop;
    panel.backups=view.listBackups?view.listBackups():std::vector<coop_backup::Record>{};
    std::stable_sort(panel.backups.begin(),panel.backups.end(),[](const auto& a,const auto& b){return a.utc>b.utc;});
    panel.backupPage=0;
}
inline const wchar_t* BackupActionLabel(coop_backup::Action action){
    switch(action){
    case coop_backup::Action::RaceResult:return L"Race result saved";
    case coop_backup::Action::SharedProgress:return L"Shared progress saved";
    case coop_backup::Action::CareerStart:return L"Career started";
    case coop_backup::Action::SpecialEvent:return L"Special event saved";
    case coop_backup::Action::BeforeRestore:return L"Before backup restore";
    default:return L"Career saved";
    }
}
inline std::wstring BackupTimeLabel(uint64_t utc){
    const FILETIME stored{DWORD(utc),DWORD(utc>>32)};
    SYSTEMTIME time{},local{};
    if(!FileTimeToSystemTime(&stored,&time)||!SystemTimeToTzSpecificLocalTime(nullptr,&time,&local))return L"Time unavailable";
    wchar_t label[32]{};
    swprintf_s(label,L"%02u.%02u.%04u  %02u:%02u:%02u",unsigned(local.wDay),unsigned(local.wMonth),unsigned(local.wYear),
        unsigned(local.wHour),unsigned(local.wMinute),unsigned(local.wSecond));
    return label;
}
constexpr UINT kUndercoopSaveMessage=WM_APP+251,kUndercoopDiscardMessage=WM_APP+252;
inline LRESULT CALLBACK UndercoopEditProc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    const auto previous=reinterpret_cast<WNDPROC>(GetPropW(window,L"Undercoop.Settings.Previous"));
    if(message==WM_KEYDOWN){
        if(wp==VK_RETURN||(wp=='S'&&(GetKeyState(VK_CONTROL)&0x8000))){SendMessageW(GetParent(window),kUndercoopSaveMessage,0,0);return 0;}
        if(wp==VK_ESCAPE){SendMessageW(GetParent(window),kUndercoopDiscardMessage,0,0);return 0;}
        if(wp==VK_TAB){const auto next=GetNextDlgTabItem(GetParent(window),window,(GetKeyState(VK_SHIFT)&0x8000)!=0);if(next)SetFocus(next);return 0;}
    }
    if(message==WM_CHAR&&(wp==VK_RETURN||wp==VK_ESCAPE||wp==VK_TAB||wp==19))return 0;
    return previous?CallWindowProcW(previous,window,message,wp,lp):DefWindowProcW(window,message,wp,lp);
}
inline UndercoopField UndercoopInputField(size_t i){const UndercoopField fields[]={UndercoopField::PeerAddress,UndercoopField::Port,UndercoopField::PeerPort,UndercoopField::Profile};return fields[(std::min)(i,size_t(3))];}
inline int UndercoopNumber(const UndercoopPanel& panel,UndercoopField field,int fallback=0){
    const auto& text=undercoop_settings::Get(panel.draft,field);if(text.empty())return fallback;
    wchar_t* end=nullptr;const long value=std::wcstol(text.c_str(),&end,10);return end&&!*end&&value>=0&&value<=65535?int(value):fallback;
}
template<class V> inline void SyncUndercoopEditors(V& view){
    auto& p=view.undercoop;if(!view.window||!IsWindow(view.window))return;
    const bool active=view.tab==7&&p.draft.loaded;
    if(!active){for(auto edit:p.edits)if(edit)ShowWindow(edit,SW_HIDE);return;}
    const UINT dpi=UINT(coop_overlay::WindowDpi(view.window));
    if(!p.font||p.fontDpi!=dpi){
        if(!p.privateFont&&view.module){const auto resource=FindResourceW(view.module,MAKEINTRESOURCEW(311),MAKEINTRESOURCEW(10));const auto data=resource?LoadResource(view.module,resource):nullptr;DWORD count=0;
            if(data)p.privateFont=AddFontMemResourceEx(LockResource(data),SizeofResource(view.module,resource),nullptr,&count);}
        HFONT next=CreateFontW(-MulDiv(14,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,p.privateFont?L"Barlow":L"Segoe UI");
        if(next){for(auto edit:p.edits)if(edit)SendMessageW(edit,WM_SETFONT,reinterpret_cast<WPARAM>(next),FALSE);if(p.font)DeleteObject(p.font);p.font=next;p.fontDpi=dpi;}
    }
    if(!p.brush)p.brush=CreateSolidBrush(RGB(30,34,40));
    RECT client{};GetClientRect(view.window,&client);const UndercoopLayout l(client.right*96.0f/dpi,client.bottom*96.0f/dpi);
    for(size_t i=0;i<p.edits.size();++i){
        const bool visible=i<3?p.section==0:p.section==1;
        if(!visible){if(p.edits[i])ShowWindow(p.edits[i],SW_HIDE);continue;}
        if(!p.edits[i]){
            const DWORD style=WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL|((i==1||i==2)?ES_NUMBER:0);
            const wchar_t* names[]={L"Teammate IPv4 address",L"Local connection port",L"Teammate connection port",L"First career profile"};
            p.edits[i]=CreateWindowExW(0,L"EDIT",names[i],style,0,0,10,10,view.window,reinterpret_cast<HMENU>(UINT_PTR(9210+i)),view.module,nullptr);
            if(!p.edits[i]){p.message=L"Cannot create a settings control.";continue;}
            SetPropW(p.edits[i],L"Undercoop.Settings.Previous",reinterpret_cast<HANDLE>(SetWindowLongPtrW(p.edits[i],GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&UndercoopEditProc))));
            SendMessageW(p.edits[i],EM_SETLIMITTEXT,i==0?15:i==3?31:5,0);SendMessageW(p.edits[i],WM_SETFONT,reinterpret_cast<WPARAM>(p.font),FALSE);
        }
        const auto box=l.Input(i);SetWindowPos(p.edits[i],nullptr,MulDiv(int(box.x+10),dpi,96),MulDiv(int(box.y+8),dpi,96),MulDiv(int(box.w-20),dpi,96),MulDiv(22,dpi,96),SWP_NOZORDER|SWP_NOACTIVATE);
        const auto& text=undercoop_settings::Get(p.draft,UndercoopInputField(i));const int length=GetWindowTextLengthW(p.edits[i]);std::wstring actual(size_t(length)+1,0);GetWindowTextW(p.edits[i],actual.data(),length+1);actual.resize(size_t(length));
        p.suppress=true;if(text!=actual)SetWindowTextW(p.edits[i],text.c_str());p.suppress=false;EnableWindow(p.edits[i],!view.busy);ShowWindow(p.edits[i],SW_SHOW);
    }
}
template<class V> inline void OpenUndercoopSettings(V& view,bool reload=false){
    auto& p=view.undercoop;if(!p.opened||reload){p.opened=true;p.message.clear();if(!undercoop_settings::Load(p.draft))p.message=p.draft.error;}
    if(p.section==kBackupSection)RefreshBackupPanel(view);
    if(p.section==kSaveImportSection)RefreshCareerImportPanel(view);
    SyncUndercoopEditors(view);
}
template<class V> inline bool UndercoopDraftGuard(V& view){
    if(!undercoop_settings::Dirty(view.undercoop.draft))return false;
    view.tab=7;view.undercoop.message=L"Save or discard your Undercoop changes before leaving this page or launching.";SyncUndercoopEditors(view);return true;
}
template<class V> inline bool DispatchUndercoop(V& view,UiHit hit){
    auto& p=view.undercoop;
    if(hit.action==UiAction::UndercoopSave){if(p.draft.loaded)undercoop_settings::Save(p.draft,p.message);SyncUndercoopEditors(view);return true;}
    if(hit.action==UiAction::UndercoopDiscard){undercoop_settings::Discard(p.draft);p.message=L"Changes discarded.";SyncUndercoopEditors(view);return true;}
    if(hit.action==UiAction::UndercoopReload){if(!UndercoopDraftGuard(view))OpenUndercoopSettings(view,true);return true;}
    if(hit.action==UiAction::UndercoopCategory&&hit.index<std::size(kUndercoopCategories)){
        if(kUndercoopCategories[hit.index]==kSaveImportSection&&UndercoopDraftGuard(view))return true;
        p.section=kUndercoopCategories[hit.index];
        if(p.section==kBackupSection){p.message.clear();RefreshBackupPanel(view);}
        if(p.section==kSaveImportSection){p.message.clear();RefreshCareerImportPanel(view);}
        SyncUndercoopEditors(view);return true;
    }
    if(view.tab==7&&p.section==kSaveImportSection){
        if(hit.action==UiAction::CareerImportPage&&hit.index<p.careerImports.size()){p.careerImportPage=hit.index;return true;}
        if(hit.action==UiAction::CareerImportPick||hit.action==UiAction::CareerImportSelect){
            if(UndercoopDraftGuard(view))return true;
            if(hit.action==UiAction::CareerImportPick)ChooseCareerImportFile(view);
            else if(hit.index<p.careerImports.size())ConfirmAndImportCareer(view,p.careerImports[hit.index].path);
            return true;
        }
    }
    if(view.tab==7&&p.section==kBackupSection){
        if(hit.action==UiAction::BackupPage&&hit.index<p.backups.size()){p.backupPage=hit.index;return true;}
        if(hit.action==UiAction::BackupSelect&&hit.index<p.backups.size()&&view.restoreBackup){
            if(UndercoopDraftGuard(view))return true;
            p.message.clear();
            const bool selected=view.restoreBackup(p.backups[hit.index],p.message);
            p.backupLaunchRequested=selected;
            if(p.message.empty())p.message=selected?L"Backup selected. Host a CO-OP lobby and choose Start Career.":L"This backup could not be selected.";
            return true;
        }
        if(hit.action==UiAction::BackupAuto&&p.draft.loaded){
            const auto field=UndercoopField::AutoBackup;
            undercoop_settings::Set(p.draft,field,UndercoopNumber(p,field,1)?L"0":L"1");p.message.clear();return true;
        }
    }
    if(!p.draft.loaded)return false;
    if(hit.action==UiAction::UndercoopRole&&hit.index<3)undercoop_settings::Set(p.draft,UndercoopField::Instance,std::to_wstring(hit.index));
    else if(hit.action==UiAction::UndercoopToggle&&hit.index<3){const UndercoopField fields[]={UndercoopField::EnableSync,UndercoopField::RaceAI,UndercoopField::WorldFleet};const auto f=fields[hit.index];undercoop_settings::Set(p.draft,f,UndercoopNumber(p,f)?L"0":L"1");}
    else if(hit.action==UiAction::UndercoopShow&&hit.index<3)undercoop_settings::Set(p.draft,UndercoopField::AutoShow,std::to_wstring(hit.index));
    else if(hit.action==UiAction::UndercoopOpacity&&hit.index>=10&&hit.index<=100)undercoop_settings::Set(p.draft,UndercoopField::BackgroundOpacity,std::to_wstring(hit.index));
    else return false;
    p.message.clear();return true;
}
template<class V> inline UiHit UndercoopHitTest(const V& view,float x,float y,float width,float height){
    const UndercoopLayout l(width,height);
    if (l.DeveloperSignIn().Contains(x,y)) return {UiAction::DeveloperSignIn};
    for(size_t i=0;i<std::size(kUndercoopCategories);++i)if(l.Category(i).Contains(x,y))return {UiAction::UndercoopCategory,i};
    if (DeveloperPageOpen(view)) return DeveloperHitTest(view, x, y, l.base);
    if(l.Reload().Contains(x,y))return {UiAction::UndercoopReload};
    const auto& p=view.undercoop;
    if(undercoop_settings::Dirty(p.draft)){
        if(l.Save().Contains(x,y))return {UiAction::UndercoopSave};if(l.Discard().Contains(x,y))return {UiAction::UndercoopDiscard};
    }
    if(p.section==kSaveImportSection){
        const CareerImportLayout imports(l.base);const auto rows=imports.Rows(),page=VisibleCareerImportPage(p,rows);
        if(imports.Choose().Contains(x,y))return {UiAction::CareerImportPick};
        if(page&&imports.PageButton(false).Contains(x,y))return {UiAction::CareerImportPage,page-1};
        if(page+1<CareerImportPages(p,rows)&&imports.PageButton(true).Contains(x,y))return {UiAction::CareerImportPage,page+1};
        for(size_t i=0;i<rows&&page*rows+i<p.careerImports.size();++i)
            if(imports.Row(i).Contains(x,y))return {UiAction::CareerImportSelect,page*rows+i};
        return {};
    }
    if(p.section==kBackupSection){
        const BackupLayout backup(l.base);const auto rows=backup.Rows(),page=VisibleBackupPage(p,rows),pages=BackupPages(p,rows);
        if(p.draft.loaded&&backup.Toggle().Contains(x,y))return {UiAction::BackupAuto};
        if(page&&backup.PageButton(false).Contains(x,y))return {UiAction::BackupPage,page-1};
        if(page+1<pages&&backup.PageButton(true).Contains(x,y))return {UiAction::BackupPage,page+1};
        if(view.restoreBackup)for(size_t i=0;i<rows&&page*rows+i<p.backups.size();++i)
            if(backup.Row(i).Contains(x,y))return {UiAction::BackupSelect,page*rows+i};
        return {};
    }
    if(!p.draft.loaded)return {};
    if(p.section==0)for(size_t i=0;i<3;++i)if(l.Role(i).Contains(x,y))return {UiAction::UndercoopRole,i};
    if(p.section==1)for(size_t i=0;i<3;++i)if(l.Toggle(i).Contains(x,y))return {UiAction::UndercoopToggle,i};
    if(p.section==2){for(size_t i=0;i<3;++i)if(l.ShowMode(i).Contains(x,y))return {UiAction::UndercoopShow,i};if(l.Opacity().Contains(x,y))return {UiAction::UndercoopOpacity,size_t(l.OpacityAt(x))};}
    return {};
}
inline unsigned UndercoopBlend(unsigned a,unsigned b,int percent){unsigned c=0;for(unsigned shift:{0u,8u,16u})c|=unsigned((int((a>>shift)&255)*(100-percent)+int((b>>shift)&255)*percent)/100)<<shift;return c;}
template<class V,class Button> inline void PaintBackupPanel(V& view,const BackupLayout& layout,Button&& button){
    auto& panel=view.undercoop;
    auto& r=view.renderer;
    const auto options=layout.Options();
    const auto toggle=layout.Toggle();
    const bool enabled=UndercoopNumber(panel,UndercoopField::AutoBackup,1)!=0;
    r.Box(options.x,options.y,options.w,options.h,0x191C21,9,0x2C323B);
    r.Text(L"Auto Backup",options.x+18,options.y+9,options.w-130,21,14,0xE5E9F0,UiFace::Bold);
    r.Text(panel.draft.loaded?L"Create a restore point every time this CO-OP career saves.":L"Reload settings to change automatic backups.",
        options.x+18,options.y+34,options.w-130,20,12,0x929DAE);
    r.Box(toggle.x,toggle.y,toggle.w,toggle.h,enabled?0xE1E5EB:0x3B4350,12);
    r.Circle(toggle.x+(enabled?32:12),toggle.y+12,8,enabled?0x262C34:0xA5AEBC);
    r.Text(enabled?L"On":L"Off",toggle.x-38,toggle.y+3,32,20,12,0xA9B4C4);
    r.Text(L"Host backup history",options.x,321,options.w,22,15,0xE5E9F0,UiFace::Bold);

    if(panel.backups.empty()){
        const auto row=layout.Row(0);
        r.Box(row.x,row.y,row.w,72,0x191C21,7,0x2C323B);
        r.Text(L"No backups yet",row.x+16,row.y+9,row.w-32,22,14,0xE5E9F0,UiFace::Bold);
        r.Text(L"Restore points appear here after hosting and saving a CO-OP career.",
            row.x+16,row.y+34,row.w-32,25,12,0x929DAE);
        return;
    }
    const auto rows=layout.Rows(),page=VisibleBackupPage(panel,rows),pages=BackupPages(panel,rows);
    for(size_t i=0;i<rows&&page*rows+i<panel.backups.size();++i){
        const auto index=page*rows+i;
        const auto& entry=panel.backups[index];
        const auto row=layout.Row(i);
        const bool hovered=view.hover==UiHit{UiAction::BackupSelect,index};
        r.Box(row.x,row.y,row.w,row.h,hovered?0x262F3B:0x191C21,7,hovered?0x596D84:0x303842);
        const auto campaign=entry.campaign+(entry.role==coop_backup::Role::Host?L"  ·  Host":L"  ·  Player 2");
        r.Text(BackupActionLabel(entry.action),row.x+15,row.y+4,row.w-300,22,14,0xE5E9F0,UiFace::Bold);
        r.Text(campaign.c_str(),row.x+15,row.y+26,row.w-300,18,12,0x9BA7B8);
        const auto time=BackupTimeLabel(entry.utc);
        r.Text(time.c_str(),row.x+row.w-276,row.y+13,151,22,12,0xB9C4D4);
        if(view.restoreBackup){
            r.Box(row.x+row.w-110,row.y+9,96,30,hovered?0xE1E5EB:0x293340,5);
            r.Text(L"Play backup",row.x+row.w-110,row.y+9,96,30,12,hovered?0x1D222A:0xC6D1DF,UiFace::Bold,false,true);
        }
    }
    if(pages>1){
        const auto previous=layout.PageButton(false);
        button(previous,L"Previous",false,page>0);
        button(layout.PageButton(true),L"Next",false,page+1<pages);
        const auto counter=std::to_wstring(page+1)+L" / "+std::to_wstring(pages);
        r.Text(counter.c_str(),previous.x+previous.w,previous.y,82,34,12,0x9BA7B8,UiFace::Regular,false,true);
    }
}
template<class V,class Button> inline void PaintCareerImportPanel(V& view,const CareerImportLayout& layout,Button&& button){
    auto& panel=view.undercoop;auto& r=view.renderer;
    const auto options=layout.Options();
    r.Box(options.x,options.y,options.w,options.h,0x191C21,9,0x2C323B);
    r.Text(L"Import a career into CO-OP",options.x+18,options.y+10,options.w-185,24,15,0xE5E9F0,UiFace::Bold);
    r.Text(L"Creates a career for Host and Player 2 in this setup.",options.x+18,options.y+36,options.w-185,20,12,0xADB7C6);
    const auto setup=state.instanceName.empty()?std::wstring(L"Default setup"):state.instanceName;
    r.Text(setup.c_str(),options.x+18,options.y+59,options.w-185,22,12,0x929DAE);
    button(layout.Choose(),L"Choose file",true);
    r.Text(L"Detected single-player saves",options.x,347,options.w,24,14,0xE5E9F0,UiFace::Bold);
    if(panel.careerImports.empty()){
        const auto row=layout.Row(0);
        r.Box(row.x,row.y,row.w,62,0x191C21,7,0x2C323B);
        r.Text(L"No single-player saves found",row.x+16,row.y+7,row.w-32,22,14,0xE5E9F0,UiFace::Bold);
        r.Text(L"Choose a career file from your computer to import it.",row.x+16,row.y+31,row.w-32,21,12,0x929DAE);
        return;
    }
    const auto rows=layout.Rows(),page=VisibleCareerImportPage(panel,rows),pages=CareerImportPages(panel,rows);
    for(size_t i=0;i<rows&&page*rows+i<panel.careerImports.size();++i){
        const auto index=page*rows+i;const auto& source=panel.careerImports[index];const auto row=layout.Row(i);
        const bool hovered=view.hover==UiHit{UiAction::CareerImportSelect,index};
        r.Box(row.x,row.y,row.w,row.h,hovered?0x262F3B:0x191C21,7,hovered?0x596D84:0x303842);
        r.Text(source.profile.c_str(),row.x+15,row.y+3,row.w-145,21,14,0xE5E9F0,UiFace::Bold);
        r.Text(source.stock?L"Original game":L"This setup",row.x+15,row.y+24,row.w-145,17,11,0x9BA7B8);
        r.Box(row.x+row.w-112,row.y+7,96,30,hovered?0xE1E5EB:0x293340,5);
        r.Text(L"Import",row.x+row.w-112,row.y+7,96,30,12,hovered?0x1D222A:0xC6D1DF,UiFace::Bold,false,true);
    }
    if(pages>1){
        const auto previous=layout.PageButton(false);
        button(previous,L"Previous",false,page>0);
        button(layout.PageButton(true),L"Next",false,page+1<pages);
        const auto counter=std::to_wstring(page+1)+L" / "+std::to_wstring(pages);
        r.Text(counter.c_str(),previous.x+previous.w,previous.y,82,34,12,0x9BA7B8,UiFace::Regular,false,true);
    }
}
template<class V> inline void PaintUndercoop(V& view,float width,float height){
    auto& p=view.undercoop;auto& r=view.renderer;const UndercoopLayout l(width,height);const auto card=l.card;const float labelWidth=card.w-370;
    auto text=[&](const wchar_t* value,float x,float y,float w,float h,float size=13,unsigned color=0xA0A8B5,UiFace face=UiFace::Regular,bool wrap=false){r.Text(value,x,y,w,h,size,color,face,wrap);};
    auto button=[&](UiRect box,const wchar_t* value,bool selected,bool enabled=true){const auto target=UndercoopHitTest(view,box.x+box.w/2,box.y+box.h/2,width,height);const bool hover=target.action!=UiAction::None&&target==view.hover;
        r.Box(box.x,box.y,box.w,box.h,selected?0xE1E5EB:hover&&enabled?0x363C46:0x242930,5);r.Text(value,box.x,box.y,box.w,box.h,12,selected?0x1D222A:enabled?0xBFC7D3:0x606873,UiFace::Bold,false,true);};
    auto actions=[&]{
        const bool dirty=undercoop_settings::Dirty(p.draft);
        if(p.section!=kBackupSection){
            r.Circle(card.x+5,l.base.footer-29,3,dirty?0xDFCC9E:0x84958D);
            text(dirty?L"Unsaved changes":L"Changes apply on launch",card.x+17,l.base.footer-40,card.w-330,24,12,dirty?0xDFCC9E:0x929EAD);
        }
        button(l.Discard(),L"Discard",false,dirty);button(l.Save(),L"Save changes",dirty,dirty);
    };
    const wchar_t* categories[]={L"Connection",L"Gameplay",L"Console",L"Backups",L"Import saves"};
    static_assert(std::size(categories)==std::size(kUndercoopCategories));
    for(size_t i=0;i<std::size(categories);++i)button(l.Category(i),categories[i],p.section==kUndercoopCategories[i]);
    const auto developer = l.DeveloperSignIn();
    const bool developerHover = view.hover.action == UiAction::DeveloperSignIn;
    const bool developerOpen = DeveloperPageOpen(view);
    const bool developerSignedIn = view.developer.SignedIn();
    r.Box(developer.x, developer.y, developer.w, developer.h,
        developerOpen ? 0xDCE5F0 : developerHover ? 0x293746 : 0x1B242E, 5, 0x3B4C5E);
    r.Text(developerSignedIn ? L"Developer Session" : L"Developer Sign-in",
        developer.x + 10, developer.y, developer.w - 26, developer.h,
        12, developerOpen ? 0x1D252F : 0xC5D8EB, UiFace::Bold, false, true);
    r.Circle(developer.x + developer.w - 15, developer.y + 17, 3,
        developerSignedIn ? 0x5FAD8B : developerOpen ? 0x586F85 : 0x798DA3);
    if (developerOpen) {
        PaintDeveloper(view, l.base);
        return;
    }
    if(p.section==kBackupSection){
        button(l.Reload(),L"Refresh",false);
        PaintBackupPanel(view,BackupLayout(l.base),button);
        if(p.draft.loaded)actions();
        return;
    }
    if(p.section==kSaveImportSection){
        button(l.Reload(),L"Refresh",false);
        PaintCareerImportPanel(view,CareerImportLayout(l.base),button);
        if(undercoop_settings::Dirty(p.draft))actions();
        return;
    }
    button(l.Reload(),L"Reload",false);r.Box(card.x,card.y,card.w,card.h,0x191C21,9,0x2C323B);
    if(!p.draft.loaded){text(L"Settings could not be opened",card.x+22,card.y+22,card.w-44,30,18,0xE5E9F0,UiFace::Bold);text(p.draft.error.c_str(),card.x+22,card.y+66,card.w-44,100,13,0xC9AEB0,UiFace::Regular,true);return;}
    auto label=[&](float y,const wchar_t* title,const wchar_t* description){text(title,card.x+20,y,labelWidth,20,14,0xE5E9F0,UiFace::Bold);text(description,card.x+20,y+22,labelWidth,p.section==1?26.0f:32.0f,11,0x929DAE,UiFace::Regular,true);};
    auto input=[&](size_t i){const auto b=l.Input(i);r.Box(b.x,b.y,b.w,b.h,0x1E2228,6,0x414A58);if(!p.edits[i]||!view.window)text(undercoop_settings::Get(p.draft,UndercoopInputField(i)).c_str(),b.x+10,b.y+8,b.w-20,24,14,0xDCE3ED);};
    if(p.section==0){
        label(card.y+(l.row-50)/2,L"Player slot",L"Automatic, Player 1 or Player 2 for this copy.");
        const wchar_t* roles[]={L"Automatic",L"Player 1",L"Player 2"};for(size_t i=0;i<3;++i)button(l.Role(i),roles[i],UndercoopNumber(p,UndercoopField::Instance)==int(i));
        r.Line(card.x+20,card.y+l.row,card.x+card.w-20,card.y+l.row,0x292F38);
        label(card.y+l.row+(l.row-50)/2,L"Teammate address",L"Your teammate's Radmin or LAN IPv4 address.");input(0);
        r.Line(card.x+20,card.y+2*l.row,card.x+card.w-20,card.y+2*l.row,0x292F38);
        label(card.y+2*l.row+(l.row-50)/2,L"Connection ports",L"Use opposite ports for two local game copies.");
        for(size_t i=1;i<=2;++i){const auto b=l.Input(i);text(i==1?L"THIS COPY":L"TEAMMATE",b.x,b.y-20,b.w,18,10,0x8C98AB,UiFace::Bold);input(i);}
    }else if(p.section==1){
        const wchar_t* titles[]={L"Teammate vehicle",L"Race opponents",L"Shared world traffic"};
        const wchar_t* descriptions[]={L"Show and synchronize your teammate's car.",L"Include AI opponents in supported races.",L"Traffic and ambient racers. Requires race opponents."};
        const UndercoopField fields[]={UndercoopField::EnableSync,UndercoopField::RaceAI,UndercoopField::WorldFleet};
        for(size_t i=0;i<3;++i){const float y=card.y+i*card.h/4;label(y+7,titles[i],descriptions[i]);const auto b=l.Toggle(i);const bool on=UndercoopNumber(p,fields[i])!=0;
            r.Box(b.x,b.y,b.w,b.h,on?0xE1E5EB:0x3B4350,12);r.Circle(b.x+(on?32:12),b.y+12,8,on?0x262C34:0xA5AEBC);text(on?L"On":L"Off",b.x-38,b.y+3,30,20,12,0xA9B4C4);
            r.Line(card.x+20,y+card.h/4,card.x+card.w-20,y+card.h/4,0x292F38);}
        label(card.y+card.h*0.75f+7,L"First career profile",L"Host's first Resume only. The last loaded career takes priority.");input(3);
    }else{
        label(card.y+14,L"Show notifications",L"Z always opens the console and chat.");const wchar_t* modes[]={L"Manual",L"Invitations",L"All notices"};for(size_t i=0;i<3;++i)button(l.ShowMode(i),modes[i],UndercoopNumber(p,UndercoopField::AutoShow,1)==int(i));
        r.Line(card.x+20,card.y+74,card.x+card.w-20,card.y+74,0x292F38);
        const int opacity=(std::max)(10,(std::min)(100,UndercoopNumber(p,UndercoopField::BackgroundOpacity,70)));label(card.y+86,L"Background opacity",L"Change the panel; keep the text readable.");const auto track=l.Opacity();const float knob=track.x+track.w*float(opacity-10)/90;
        r.Box(track.x,track.y+14,track.w,4,0x414957,2);r.Box(track.x,track.y+14,knob-track.x,4,0xC7D0DF,2);r.Circle(knob,track.y+16,7,0xE8ECF3);
        text((std::to_wstring(opacity)+L"%").c_str(),track.x+track.w-48,track.y-17,60,22,12,0xD6DEEB,UiFace::Bold);
        const UiRect preview{card.x+20,card.y+153,card.w-40,card.h-168};r.Box(preview.x,preview.y,preview.w,preview.h,UndercoopBlend(0x505966,0x080A0E,opacity),7,0x3B4554);
        text(L"NOTICE",preview.x+12,preview.y+8,72,18,10,0xB7C2D3,UiFace::Bold);text(L"Your teammate is ready.",preview.x+86,preview.y+6,preview.w-100,22,12,0xEFF2F7);
        text(L"TEAMMATE",preview.x+12,preview.y+29,72,18,10,0xD6DEE9,UiFace::Bold);text(L"Let's race.",preview.x+86,preview.y+27,preview.w-100,22,12,0xD1D8E3);
    }
    if(l.base.footer-(card.y+card.h)>94){text(p.section==0?L"Use 127.0.0.1 only when both game copies run on this computer.":p.section==1?L"Match race opponents and shared traffic with your teammate before joining.":L"Invitations include race, sponsor and special-event proposals.",card.x+2,card.y+card.h+16,card.w-4,40,12,0x8895A9,UiFace::Regular,true);}
    actions();
}
