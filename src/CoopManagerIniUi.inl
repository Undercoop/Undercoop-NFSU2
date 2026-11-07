// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Included after UiLayout; templates keep the editor local to its owned window.
struct IniPanel{
    std::vector<IniFile> files;IniOpen opened;std::string ownerFilter;
    std::set<std::string> expandedOwners;
    size_t selected=SIZE_MAX,row=SIZE_MAX,fileFirst=0,rowFirst=0;
    std::wstring value,message;bool dirty=false,suppress=false;
    HWND edit=nullptr;HFONT font=nullptr;HANDLE privateFont=nullptr;HBRUSH brush=nullptr;UINT fontDpi=0;
    IniPanel()=default;IniPanel(const IniPanel&)=delete;IniPanel& operator=(const IniPanel&)=delete;
    ~IniPanel(){if(edit&&IsWindow(edit))DestroyWindow(edit);if(font)DeleteObject(font);if(brush)DeleteObject(brush);if(privateFont)RemoveFontMemResourceEx(privateFont);}
};
struct IniLayout{
    UiLayout base;float left=218,right,rw,detail;explicit IniLayout(float w,float h):base(w,h),right(base.content+234),rw(w-right-30),detail(base.footer-174){}
    size_t Files()const{return size_t((std::max)(1,int((base.footer-base.body-18)/57)));}
    size_t Rows()const{return size_t((std::max)(1,int((detail-base.body-43)/44)));}
    UiRect File(size_t i)const{return {base.content,base.body+float(i)*57,left,51};}
    UiRect Row(size_t i)const{return {right,base.body+39+float(i)*44,rw,40};}
    UiRect Input()const{return {right+12,detail+31,rw-121,32};}
    UiRect Save()const{return {right+rw-99,detail+31,87,32};}
    UiRect Discard()const{return {right+rw-99,detail+69,87,28};}
    UiRect Reload()const{return {base.width-132,119,102,32};}
    UiRect All()const{return {base.width-244,119,102,32};}
};
inline std::vector<size_t> IniVisible(const IniPanel& panel){
    std::vector<size_t> out;for(size_t i=0;i<panel.files.size();++i)if(panel.ownerFilter.empty()||panel.files[i].owner==panel.ownerFilter)out.push_back(i);return out;
}
struct IniTreeEntry{size_t index=SIZE_MAX,count=0;bool group=false,expanded=false;};
inline std::vector<IniTreeEntry> IniTree(const IniPanel& panel){
    std::vector<IniTreeEntry> out;std::vector<std::string> order;std::map<std::string,std::vector<size_t>> groups;
    // Files may be sorted editable-first. Group by identity rather than adjacency
    // so one mod never receives duplicate headings for its read-only INIs.
    for(const auto index:IniVisible(panel)){
        auto& files=groups[panel.files[index].owner];if(files.empty())order.push_back(panel.files[index].owner);files.push_back(index);
    }
    for(const auto& owner:order){const auto& files=groups[owner];const bool expanded=panel.expandedOwners.count(owner)!=0;
        out.push_back({files.front(),files.size(),true,expanded});
        if(expanded)for(const auto index:files)out.push_back({index,0,false,false});
    }
    return out;
}
inline const coop_ini::Row* IniRow(const IniPanel& panel){return panel.row<panel.opened.document.Rows().size()?&panel.opened.document.Rows()[panel.row]:nullptr;}
inline bool IniEditable(const IniPanel& panel){const auto* row=IniRow(panel);return row&&row->editable&&panel.opened.file.readOnly.empty()&&panel.opened.error.empty();}
constexpr UINT kIniSaveMessage=WM_APP+241,kIniDiscardMessage=WM_APP+242;
inline LRESULT CALLBACK IniEditProc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    const auto previous=reinterpret_cast<WNDPROC>(GetPropW(window,L"Undercoop.IniEdit.Previous"));
    if(message==WM_KEYDOWN){
        if(wp==VK_RETURN||(wp=='S'&&(GetKeyState(VK_CONTROL)&0x8000))){SendMessageW(GetParent(window),kIniSaveMessage,0,0);return 0;}
        if(wp==VK_ESCAPE){SendMessageW(GetParent(window),kIniDiscardMessage,0,0);return 0;}
        if(wp==VK_TAB){SetFocus(GetParent(window));return 0;}
    }
    if(message==WM_CHAR&&(wp==VK_RETURN||wp==VK_ESCAPE||wp==VK_TAB||wp==19))return 0;
    return previous?CallWindowProcW(previous,window,message,wp,lp):DefWindowProcW(window,message,wp,lp);
}
template<class V> inline void SyncIniEditor(V& view){
    auto& panel=view.ini;if(!view.window||!IsWindow(view.window))return;
    const bool visible=view.tab==4&&IniRow(panel);
    if(!visible){if(panel.edit)ShowWindow(panel.edit,SW_HIDE);return;}
    if(!panel.edit){
        panel.edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,0,0,10,10,view.window,reinterpret_cast<HMENU>(9107),view.module,nullptr);
        if(!panel.edit){panel.message=L"Cannot create the value editor.";return;}
        const auto previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(panel.edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&IniEditProc)));
        SetPropW(panel.edit,L"Undercoop.IniEdit.Previous",reinterpret_cast<HANDLE>(previous));
        SendMessageW(panel.edit,EM_SETLIMITTEXT,8192,0);panel.brush=CreateSolidBrush(RGB(30,34,40));
    }
    const UINT dpi=UINT(coop_overlay::WindowDpi(view.window));
    if(!panel.font||dpi!=panel.fontDpi){
        if(!panel.privateFont&&view.module){const auto resource=FindResourceW(view.module,MAKEINTRESOURCEW(311),MAKEINTRESOURCEW(10));
            const auto data=resource?LoadResource(view.module,resource):nullptr;DWORD count=0;
            if(data)panel.privateFont=AddFontMemResourceEx(LockResource(data),SizeofResource(view.module,resource),nullptr,&count);}
        const auto font=CreateFontW(-MulDiv(14,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,panel.privateFont?L"Barlow":L"Segoe UI");
        SendMessageW(panel.edit,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);if(panel.font)DeleteObject(panel.font);panel.font=font;panel.fontDpi=dpi;
    }
    RECT rect{};GetClientRect(view.window,&rect);const IniLayout layout(rect.right*96.0f/dpi,rect.bottom*96.0f/dpi);const auto input=layout.Input();
    SetWindowPos(panel.edit,nullptr,MulDiv(int(input.x+7),dpi,96),MulDiv(int(input.y+6),dpi,96),MulDiv(int(input.w-14),dpi,96),MulDiv(22,dpi,96),SWP_NOZORDER|SWP_NOACTIVATE);
    panel.suppress=true;const int length=GetWindowTextLengthW(panel.edit);std::wstring actual(size_t(length)+1,0);GetWindowTextW(panel.edit,actual.data(),length+1);actual.resize(size_t(length));
    if(actual!=panel.value)SetWindowTextW(panel.edit,panel.value.c_str());
    SendMessageW(panel.edit,EM_SETREADONLY,!IniEditable(panel),0);panel.suppress=false;ShowWindow(panel.edit,SW_SHOW);
}
template<class V> inline void SelectIniRow(V& view,size_t row){
    auto& panel=view.ini;panel.row=row;panel.dirty=false;const auto* value=IniRow(panel);
    panel.value=value?IniWide(value->value,panel.opened.codepage):L"";SyncIniEditor(view);
}
template<class V> inline void SelectIniFile(V& view,size_t index){
    auto& panel=view.ini;panel.selected=index;panel.rowFirst=0;panel.message.clear();
    if(index<panel.files.size()){panel.expandedOwners.insert(panel.files[index].owner);OpenIni(panel.files[index],panel.opened);SelectIniRow(view,panel.opened.document.Rows().empty()?SIZE_MAX:0);}
    else{panel.opened=IniOpen{};SelectIniRow(view,SIZE_MAX);}
}
template<class V> inline void RefreshIni(V& view){
    auto& panel=view.ini;const auto previous=panel.selected<panel.files.size()?panel.files[panel.selected].path:L"";
    std::wstring error;if(!DiscoverIni(panel.files,error)){panel.message=error;panel.files.clear();}
    panel.fileFirst=0;const auto visible=IniVisible(panel);size_t selected=visible.empty()?SIZE_MAX:visible.front();
    for(const auto index:visible)if(Equal(panel.files[index].path,previous))selected=index;
    SelectIniFile(view,selected);if(!error.empty())panel.message=error;
}
template<class V> inline bool IniDraftGuard(V& view){
    if(!view.ini.dirty)return false;view.ini.message=L"Save or discard the edited value before changing files or launching.";return true;
}
template<class V> inline void ToggleIniGroup(V& view,size_t index){
    auto& panel=view.ini;if(index>=panel.files.size()||IniDraftGuard(view))return;
    const auto& owner=panel.files[index].owner;if(!panel.ownerFilter.empty()&&panel.ownerFilter!=owner)return;
    if(!panel.expandedOwners.erase(owner))panel.expandedOwners.insert(owner);
    const auto tree=IniTree(panel);if(panel.fileFirst>=tree.size())panel.fileFirst=tree.empty()?0:tree.size()-1;
}
template<class V> inline UiHit IniHitTest(const V& view,float x,float y,float width,float height){
    const IniLayout l(width,height);if(l.Reload().Contains(x,y))return {UiAction::IniReload};
    if(l.All().Contains(x,y))return {UiAction::IniAll};
    const auto tree=IniTree(view.ini);
    for(size_t i=0;i<l.Files()&&view.ini.fileFirst+i<tree.size();++i)if(l.File(i).Contains(x,y)){
        const auto& entry=tree[view.ini.fileFirst+i];return {entry.group?UiAction::IniGroup:UiAction::IniFile,entry.index};}
    for(size_t i=0;i<l.Rows()&&view.ini.rowFirst+i<view.ini.opened.document.Rows().size();++i)if(l.Row(i).Contains(x,y))return {UiAction::IniRow,view.ini.rowFirst+i};
    if(l.Save().Contains(x,y)&&IniEditable(view.ini))return {UiAction::IniSave};
    if(l.Discard().Contains(x,y)&&view.ini.dirty)return {UiAction::IniDiscard};return {};
}
template<class V> inline void PaintIni(V& view,float width,float height){
    auto& panel=view.ini;auto& r=view.renderer;const IniLayout l(width,height);
    auto text=[&](const wchar_t* value,float x,float y,float w,float h,float size=13,unsigned color=0xC6CDD8,UiFace face=UiFace::Regular,bool wrap=false){r.Text(value,x,y,w,h,size,color,face,wrap);};
    auto button=[&](UiRect box,const wchar_t* label,bool active){r.Box(box.x,box.y,box.w,box.h,active?0xDDE3EB:0x282E37,5);r.Text(label,box.x,box.y,box.w,box.h,12,active?0x19202A:0xC3CCD9,UiFace::Bold,false,true);};
    button(l.All(),L"All mods",false);button(l.Reload(),L"Reload files",false);
    const auto tree=IniTree(panel);const size_t maxFile=tree.size()>l.Files()?tree.size()-l.Files():0;panel.fileFirst=(std::min)(panel.fileFirst,maxFile);
    for(size_t i=0;i<l.Files()&&panel.fileFirst+i<tree.size();++i){const auto& entry=tree[panel.fileFirst+i];const auto& file=panel.files[entry.index];const auto box=l.File(i);
        if(entry.group){const bool active=panel.selected<panel.files.size()&&panel.files[panel.selected].owner==file.owner;
            r.Box(box.x,box.y,box.w,box.h,active?0x272E38:0x1B1E24,6,active?0x515E71:0x2A3039);
            const float x=box.x+12,y=box.y+19;
            if(entry.expanded){r.Line(x,y,x+5,y+5,0xB0BECE,1.4f);r.Line(x+5,y+5,x+10,y,0xB0BECE,1.4f);}
            else{r.Line(x+2,y-2,x+7,y+3,0xB0BECE,1.4f);r.Line(x+7,y+3,x+2,y+8,0xB0BECE,1.4f);}
            text(file.mod.c_str(),box.x+32,box.y+7,box.w-42,22,13,0xE2E7EE,UiFace::Bold);
            const auto count=std::to_wstring(entry.count)+(entry.count==1?L" settings file":L" settings files");
            text(count.c_str(),box.x+32,box.y+30,box.w-42,18,11,0x919DAE);
        }else{const bool selected=entry.index==panel.selected;const float indent=16;
            r.Box(box.x+indent,box.y,box.w-indent,box.h,selected?0x2C333E:0x171B21,5,selected?0x747F92:0x252B34);
            const auto split=file.relative.find_last_of(L"\\/");const auto name=split==std::wstring::npos?file.relative:file.relative.substr(split+1);
            text(name.c_str(),box.x+indent+11,box.y+7,box.w-indent-21,22,12,selected?0xE2E7EE:0xBBC6D5,selected?UiFace::Bold:UiFace::Regular);
            const auto detail=split==std::wstring::npos?(file.readOnly.empty()?L"Editable options":L"Read-only"):file.relative.substr(0,split);
            text(detail.c_str(),box.x+indent+11,box.y+30,box.w-indent-21,18,10,0x8491A3);
        }
    }
    if(tree.empty()){text(L"No INI option files found for this mod.",l.base.content,l.base.body+16,l.left,110,14,0x929EAF,UiFace::Regular,true);return;}
    text(panel.opened.file.relative.c_str(),l.right,l.base.body,l.rw,28,15,0xE1E6EF,UiFace::Bold);
    if(!panel.opened.error.empty()){text(panel.opened.error.c_str(),l.right,l.base.body+48,l.rw,160,14,0xD6BCC0,UiFace::Regular,true);return;}
    const auto& rows=panel.opened.document.Rows();const size_t maxRow=rows.size()>l.Rows()?rows.size()-l.Rows():0;panel.rowFirst=(std::min)(panel.rowFirst,maxRow);
    for(size_t i=0;i<l.Rows()&&panel.rowFirst+i<rows.size();++i){const size_t index=panel.rowFirst+i;const auto& row=rows[index];const auto box=l.Row(i);
        r.Box(box.x,box.y,box.w,box.h,index==panel.row?0x2A313B:0x1B1F25,5);
        text(IniWide(row.key,panel.opened.codepage).c_str(),box.x+10,box.y+3,box.w-120,22,13,0xDDE4EF,UiFace::Bold);
        text((L"["+IniWide(row.section,panel.opened.codepage)+L"]").c_str(),box.x+10,box.y+23,box.w-120,16,10,0x8D9AAE);
        text(IniWide(row.value,panel.opened.codepage).c_str(),box.x+box.w-105,box.y+10,95,24,13,0xB7C4D8);
    }
    const auto* row=IniRow(panel);if(!row){text(L"No editable values in this file.",l.right,l.detail,l.rw,70,14,0x9DA9BC,UiFace::Regular,true);return;}
    r.Box(l.right,l.detail,l.rw,157,0x191E25,7,0x303946);
    text((IniWide(row->section,panel.opened.codepage)+L" / "+IniWide(row->key,panel.opened.codepage)).c_str(),l.right+12,l.detail+6,l.rw-24,22,12,0xA6B4C7,UiFace::Bold);
    const auto input=l.Input();r.Box(input.x,input.y,input.w,input.h,0x1E2228,4,IniEditable(panel)?0x707F95:0x414A57);
    if(!panel.edit||!view.window)text(panel.value.c_str(),input.x+7,input.y+6,input.w-14,23,14);
    button(l.Save(),IniEditable(panel)?L"Save":L"Read-only",IniEditable(panel));if(panel.dirty)button(l.Discard(),L"Discard",false);
    std::wstring help=panel.opened.file.readOnly;
    if(help.empty()&&!row->editable)help=IniWide(row->readOnlyReason,panel.opened.codepage);
    if(help.empty())help=IniWide(row->inlineComment.empty()?row->leadingComments:row->inlineComment,panel.opened.codepage);
    if(help.empty())help=L"Original comments and formatting are preserved. Shared gameplay options must match your teammate.";
    text(help.c_str(),l.right+12,l.detail+72,l.rw-124,76,11,0x93A2B8,UiFace::Regular,true);
}
