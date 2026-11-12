// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Owned pre-game launcher controls. A name is display text, never a disk path.
struct ModpackPanel{
    std::vector<ModpackInfo> packs;std::vector<coop_packages::Manifest> choices;std::vector<std::string> members;
    std::wstring name,editing,message;size_t first=0;bool naming=false,suppress=false;
    HWND edit=nullptr;HFONT font=nullptr;HANDLE privateFont=nullptr;HBRUSH brush=nullptr;
    ~ModpackPanel(){if(edit&&IsWindow(edit))DestroyWindow(edit);if(font)DeleteObject(font);if(privateFont)RemoveFontMemResourceEx(privateFont);if(brush)DeleteObject(brush);}
};
struct ModpackLayout{
    UiLayout base;explicit ModpackLayout(float w,float h):base(w,h){}
    UiRect Create()const{return {base.width-308,119,144,32};}
    UiRect Library()const{return {base.width-174,119,144,32};}
    UiRect Import()const{return {base.width-332,119,146,32};}
    UiRect Card(size_t i)const{return {base.content,base.body+float(i)*90,base.ContentWidth(),78};}
    UiRect Activate(size_t i)const{const auto r=Card(i);return {r.x+r.w-194,r.y+26,104,30};}
    UiRect Edit(size_t i)const{const auto r=Card(i);return {r.x+r.w-78,r.y+26,62,30};}
    size_t Rows()const{return size_t((std::max)(1,int((base.footer-base.body-38)/90)));}
    UiRect Member(size_t i)const{return {base.content,base.body+float(i)*54,base.ContentWidth(),46};}
    size_t Members()const{return size_t((std::max)(1,int((base.footer-base.body-54)/54)));}
    UiRect Save()const{return {base.content+154,base.footer-44,162,32};}
    UiRect Cancel()const{return {base.content,base.footer-44,140,32};}
    UiRect NameCard()const{return {base.content,base.body,base.ContentWidth(),232};}
    UiRect NameInput()const{return {base.content+22,base.body+88,base.ContentWidth()-44,38};}
    UiRect NameOkay()const{return {base.content+180,base.body+164,180,38};}
    UiRect NameCancel()const{return {base.content+22,base.body+164,144,38};}
};
constexpr UINT kModpackNameConfirm=WM_APP+281,kModpackNameCancel=WM_APP+282;
inline LRESULT CALLBACK ModpackNameProc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    const auto previous=reinterpret_cast<WNDPROC>(GetPropW(window,L"Undercoop.Modpack.Edit.Previous"));
    if(message==WM_KEYDOWN&&(wp==VK_RETURN||wp==VK_ESCAPE)){SendMessageW(GetParent(window),wp==VK_RETURN?kModpackNameConfirm:kModpackNameCancel,0,0);return 0;}
    if(message==WM_CHAR&&(wp==VK_RETURN||wp==VK_ESCAPE))return 0;return previous?CallWindowProcW(previous,window,message,wp,lp):DefWindowProcW(window,message,wp,lp);
}
template<class V> inline void SyncModpackEditor(V& view){
    auto& p=view.modpacks;if(!view.window||!IsWindow(view.window))return;
    if(view.tab!=6||!p.naming){if(p.edit)ShowWindow(p.edit,SW_HIDE);return;}
    if(!p.edit){
        p.edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,0,0,10,10,view.window,reinterpret_cast<HMENU>(9121),view.module,nullptr);if(!p.edit){p.message=L"Cannot create the modpack name editor.";return;}
        const auto previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(p.edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&ModpackNameProc)));SetPropW(p.edit,L"Undercoop.Modpack.Edit.Previous",reinterpret_cast<HANDLE>(previous));SendMessageW(p.edit,EM_SETLIMITTEXT,64,0);
        if(view.module){const auto resource=FindResourceW(view.module,MAKEINTRESOURCEW(311),MAKEINTRESOURCEW(10));const auto data=resource?LoadResource(view.module,resource):nullptr;DWORD count=0;if(data)p.privateFont=AddFontMemResourceEx(LockResource(data),SizeofResource(view.module,resource),nullptr,&count);}
        const UINT dpi=UINT(coop_overlay::WindowDpi(view.window));p.font=CreateFontW(-MulDiv(14,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,p.privateFont?L"Barlow":L"Segoe UI");SendMessageW(p.edit,WM_SETFONT,reinterpret_cast<WPARAM>(p.font),FALSE);p.brush=CreateSolidBrush(RGB(30,34,40));
    }
    const float dpi=coop_overlay::WindowDpi(view.window);RECT client{};GetClientRect(view.window,&client);const auto input=ModpackLayout(client.right*96/dpi,client.bottom*96/dpi).NameInput();
    SetWindowPos(p.edit,nullptr,int((input.x+9)*dpi/96),int((input.y+7)*dpi/96),int((input.w-18)*dpi/96),int(25*dpi/96),SWP_NOZORDER|SWP_NOACTIVATE);
    wchar_t text[65]{};GetWindowTextW(p.edit,text,65);p.suppress=true;if(p.name!=text)SetWindowTextW(p.edit,p.name.c_str());p.suppress=false;ShowWindow(p.edit,SW_SHOW);
}
template<class V> inline void RefreshModpacks(V& view){view.modpacks.first=0;if(!ListModpacks(view.modpacks.packs))view.modpacks.message=state.error;}
template<class V> inline void BeginModpackName(V& view){auto& p=view.modpacks;p.naming=true;p.editing.clear();p.name.clear();p.message.clear();view.tab=6;RefreshModpacks(view);SyncModpackEditor(view);if(p.edit)SetFocus(p.edit);}
template<class V> inline bool BeginModpackEdit(V& view,size_t index){
    auto& p=view.modpacks;if(index>=p.packs.size())return false;ModpackLoaded existing,library;if(!ReadModpack(p.packs[index].id,existing)||!ModpackReadCatalog(state.root+L"\\mods",library,true)){p.message=state.error;return false;}
    p.editing=existing.info.id;p.name=existing.info.name;p.choices=existing.catalog;p.members.clear();for(const auto& item:existing.catalog)p.members.push_back(item.id);
    for(const auto& item:library.catalog)if(std::none_of(p.choices.begin(),p.choices.end(),[&](const auto& old){return old.id==item.id;}))p.choices.push_back(item);
    std::sort(p.choices.begin(),p.choices.end(),[](const auto& a,const auto& b){return a.name<b.name;});p.first=0;p.message=L"Keep a package checked to include it. Existing versions and settings are preserved.";p.naming=false;return true;
}
template<class V> inline UiHit ModpackHitTest(const V& view,float x,float y,float width,float height){
    const auto& p=view.modpacks;ModpackLayout l(width,height);
    if(p.naming){if(l.NameOkay().Contains(x,y))return {UiAction::PackConfirm};if(l.NameCancel().Contains(x,y))return {UiAction::PackCancel};return {};}
    if(!p.editing.empty()){if(l.Save().Contains(x,y))return {UiAction::PackSave};if(l.Cancel().Contains(x,y))return {UiAction::PackCancel};
        for(size_t i=0;i<l.Members()&&p.first+i<p.choices.size();++i)if(l.Member(i).Contains(x,y))return {UiAction::PackToggle,p.first+i};return {};}
    if(l.Library().Contains(x,y))return {UiAction::PackLibrary};
    if(l.Import().Contains(x,y))return {UiAction::PackImport};
    for(size_t i=0;i<l.Rows()&&p.first+i<p.packs.size();++i){if(l.Activate(i).Contains(x,y))return {UiAction::PackActivate,p.first+i};if(l.Edit(i).Contains(x,y))return {UiAction::PackEdit,p.first+i};}return {};
}
template<class V> inline bool DispatchModpack(V& view,UiHit hit){
    auto& p=view.modpacks;
    if(hit.action==UiAction::PackCreate){BeginModpackName(view);return true;}
    if(hit.action==UiAction::PackImport){PickImportModpack(view);return true;}
    if(hit.action==UiAction::PackCancel){p.naming=false;p.editing.clear();p.message=L"Changes cancelled. Your current setup was not changed.";SyncModpackEditor(view);return true;}
    if(hit.action==UiAction::PackConfirm){std::wstring id;if(CreateModpack(p.name,id)){p.naming=false;p.message=L"Modpack created. Activate it to play with its private versions and settings.";RefreshModpacks(view);}else p.message=state.error;SyncModpackEditor(view);return true;}
    if(hit.action==UiAction::PackEdit){BeginModpackEdit(view,hit.index);return true;}
    if(hit.action==UiAction::PackToggle&&hit.index<p.choices.size()){const auto id=p.choices[hit.index].id;const auto found=std::find(p.members.begin(),p.members.end(),id);if(found==p.members.end())p.members.push_back(id);else p.members.erase(found);return true;}
    if(hit.action==UiAction::PackSave){if(EditModpack(p.editing,p.members)){p.editing.clear();p.message=L"Modpack updated. Other instances and the mod library are unchanged.";RefreshModpacks(view);view.ini.selected=SIZE_MAX;view.ini.opened={};view.ini.files.clear();}else p.message=state.error;return true;}
    if(hit.action==UiAction::PackActivate&&hit.index<p.packs.size()){if(ActivateModpack(p.packs[hit.index].id)){p.message=L"Active: "+state.instanceName+L". Only this instance's selected mods will load.";view.ini.selected=SIZE_MAX;view.ini.opened={};view.ini.files.clear();}else p.message=state.error;return true;}
    if(hit.action==UiAction::PackLibrary){if(UseModLibrary()){p.message=L"Custom setup active. Your saved modpacks are unchanged.";view.ini.selected=SIZE_MAX;view.ini.opened={};view.ini.files.clear();}else p.message=state.error;return true;}return false;
}
template<class V> inline void PaintModpacks(V& view,float width,float height){
    auto& p=view.modpacks;auto& r=view.renderer;ModpackLayout l(width,height);const float cw=l.base.ContentWidth();
    auto text=[&](const std::wstring& value,float x,float y,float w,float h,float size=13,unsigned color=0xA6AFBD,UiFace face=UiFace::Regular,bool wrap=false){r.Text(value.c_str(),x,y,w,h,size,color,face,wrap);};
    auto button=[&](const wchar_t* label,const UiRect& box,bool prominent=false){r.Box(box.x,box.y,box.w,box.h,prominent?0xDEE4EE:0x303744,6);r.Text(label,box.x,box.y,box.w,box.h,12,prominent?0x171A20:0xD4DFEF,UiFace::Bold,false,true);};
    if(p.naming){const auto box=l.NameCard();r.Box(box.x,box.y,box.w,box.h,0x191B1F,8,0x2C3037);text(L"Name your modpack",box.x+22,box.y+18,cw-44,28,20,0xE8EDF5,UiFace::Bold);
        text(L"Snapshot all current mods, including disabled packages and private settings.",box.x+22,box.y+53,cw-44,26);const auto input=l.NameInput();r.Box(input.x,input.y,input.w,input.h,0x1E2228,5,0x586171);if(!view.window)text(p.name.empty()?L"Enter a name...":p.name,input.x+9,input.y+7,input.w-18,25,14,0xDDE4F1);
        button(L"Cancel",l.NameCancel());button(L"Create modpack",l.NameOkay(),true);return;}
    if(!p.editing.empty()){
        text(L"EDIT: "+p.name,l.base.content,156,cw,27,13,0xD0D9E7,UiFace::Bold);const size_t max=p.choices.size()>l.Members()?p.choices.size()-l.Members():0;p.first=(std::min)(p.first,max);
        for(size_t i=0;i<l.Members()&&p.first+i<p.choices.size();++i){const auto& item=p.choices[p.first+i];const auto box=l.Member(i);const bool included=std::find(p.members.begin(),p.members.end(),item.id)!=p.members.end();
            r.Box(box.x,box.y,box.w,box.h,0x191B1F,7,0x2C3037);r.Box(box.x+14,box.y+13,20,20,included?0xDEE4EE:0x303744,4);if(included){r.Line(box.x+18,box.y+23,box.x+23,box.y+27,0x171A20,1.8f);r.Line(box.x+23,box.y+27,box.x+31,box.y+18,0x171A20,1.8f);}
            text(Wide(item.name),box.x+48,box.y+7,box.w-175,25,14,0xE5EAF3,UiFace::Bold);text(Wide(item.version),box.x+box.w-117,box.y+10,100,25,12);}
        button(L"Cancel",l.Cancel());button(L"Save modpack",l.Save(),true);return;
    }
    button(L"Use custom setup",l.Library());button(L"Import modpack",l.Import());const size_t max=p.packs.size()>l.Rows()?p.packs.size()-l.Rows():0;p.first=(std::min)(p.first,max);
    if(p.packs.empty()){r.Box(l.base.content,l.base.body,cw,138,0x191B1F,8,0x2C3037);text(L"No modpacks yet",l.base.content+22,l.base.body+20,cw-44,30,21,0xE8EDF5,UiFace::Bold);text(L"Import a supported modpack ZIP, or create a snapshot from My mods. Imported packs remain inactive until you choose Activate.",l.base.content+22,l.base.body+65,cw-44,50,14,0xA6AFBD,UiFace::Regular,true);}
    for(size_t i=0;i<l.Rows()&&p.first+i<p.packs.size();++i){const auto& pack=p.packs[p.first+i];const auto box=l.Card(i);const bool active=pack.id==state.instanceId;r.Box(box.x,box.y,box.w,box.h,active?0x232B37:0x191B1F,8,active?0x5A6A83:0x2C3037);
        text(pack.name,box.x+18,box.y+10,box.w-225,26,17,0xE8EDF5,UiFace::Bold);text(std::to_wstring(pack.packages)+L" packages / "+std::to_wstring(pack.enabled)+L" enabled"+(active?L" / ACTIVE":L""),box.x+18,box.y+43,box.w-225,23,12);button(active?L"Active":L"Activate",l.Activate(i),!active);button(L"Edit",l.Edit(i));}
    text(state.instanceId.empty()?L"Current: Custom setup":L"Current: "+state.instanceName,l.base.content,l.base.footer-28,cw,23,12,0x9EADBF);
}
