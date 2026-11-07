// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Launcher-only configuration ownership and staged, backed-up writes. No AppData,
// game save, archive execution, or native loading participates in this editor.
struct IniFile{std::string owner;std::wstring mod,relative,path,readOnly;};
struct IniOpen{IniFile file;coop_ini::Document document;UINT codepage=CP_ACP;std::wstring error;};
inline std::wstring IniWide(const std::string& bytes,UINT codepage){
    if(bytes.empty())return {};const DWORD flags=codepage==CP_UTF8?MB_ERR_INVALID_CHARS:0;
    const int n=MultiByteToWideChar(codepage,flags,bytes.data(),int(bytes.size()),nullptr,0);if(n<=0)return L"[unsupported text]";
    std::wstring out(size_t(n),0);MultiByteToWideChar(codepage,flags,bytes.data(),int(bytes.size()),out.data(),n);return out;
}
inline bool IniBytes(const std::wstring& text,UINT codepage,std::string& bytes){
    bytes.clear();if(text.empty())return true;BOOL replaced=FALSE;
    const DWORD flags=codepage==CP_UTF8?WC_ERR_INVALID_CHARS:WC_NO_BEST_FIT_CHARS;
    const int n=WideCharToMultiByte(codepage,flags,text.data(),int(text.size()),nullptr,0,nullptr,codepage==CP_UTF8?nullptr:&replaced);
    if(n<=0||replaced)return false;bytes.assign(size_t(n),0);
    return WideCharToMultiByte(codepage,flags,text.data(),int(text.size()),bytes.data(),n,nullptr,codepage==CP_UTF8?nullptr:&replaced)>0&&!replaced;
}
inline std::wstring IniPolicy(const coop_packages::Manifest& package,const std::wstring& relative){
    if(package.id=="nfsu2-unlimiter")return L"Read-only: this component's verified adapter currently pins its technical configuration.";
    if(const auto* native=StandaloneNative(package.id))return Equal(relative,native->config)?L"":L"Read-only: this file is not an editable option file of this component.";
    std::string normalized;if(!coop_mod_scan::RelativeName(relative,normalized))return L"Read-only: unsupported file path.";
    if(const auto imported=state.imported.find(package.id);imported!=state.imported.end()){
        for(const auto& file:imported->second.files)if(file.source==normalized&&file.config)return {};
        return L"Read-only: not an option file in this native import's receipt.";
    }
    for(const auto& file:package.files)if(file.source==normalized){
        if(file.destination.rfind("scripts/unlimiterdata/",0)==0)return L"Read-only compatibility mirror. The runtime configuration belongs to NFSU2 Unlimiter.";
        return {};
    }
    return L"Read-only: not declared as a runtime resource by this package.";
}
inline bool GatherIni(const coop_packages::Manifest& package,const std::wstring& root,const std::wstring& relative,
    std::vector<IniFile>& out,std::wstring& error,unsigned depth=0){
    if(depth>8){error=L"INI folder nesting limit exceeded.";return false;}
    const auto directory=root+(relative.empty()?L"":L"\\"+relative);auto pin=Pin(directory,true);
    if(!pin){error=L"An INI folder is linked or unreadable.";return false;}
    WIN32_FIND_DATAW item{};HANDLE find=FindFirstFileW((directory+L"\\*").c_str(),&item);
    if(find==INVALID_HANDLE_VALUE){error=L"Cannot enumerate mod settings.";return false;}bool okay=true;size_t entries=0;
    do{const std::wstring name=item.cFileName;if(name==L"."||name==L"..")continue;
        if(++entries>4096||(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){error=L"Linked or excessive settings files are not supported.";okay=false;break;}
        const auto child=relative.empty()?name:relative+L"\\"+name;
        if(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){
            if(Equal(name,L"Tracks")||Equal(name,L"Saves")||Equal(name,L"cache"))continue;
            if(!GatherIni(package,root,child,out,error,depth+1)){okay=false;break;}
        }else{
            if(Equal(name,L"undercoop.mod.ini")||Equal(name,L"imgui.ini")||name.size()<4||!Equal(name.substr(name.size()-4),L".ini"))continue;
            if(out.size()>=512){error=L"Too many INI files.";okay=false;break;}
            out.push_back({package.id,Wide(package.name),child,root+L"\\"+child,IniPolicy(package,child)});
        }
    }while(FindNextFileW(find,&item));
    if(okay&&GetLastError()!=ERROR_NO_MORE_FILES){error=L"INI enumeration failed.";okay=false;}FindClose(find);return okay;
}
inline bool DiscoverIni(std::vector<IniFile>& out,std::wstring& error){
    out.clear();error.clear();for(const auto& package:state.catalog){
        if(!GatherIni(package,PackageDirectory(package.id),L"",out,error))return false;
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){
        if(a.readOnly.empty()!=b.readOnly.empty())return a.readOnly.empty();return a.owner==b.owner?a.relative<b.relative:a.owner<b.owner;});return true;
}
inline bool IniBound(const IniFile& file){
    const auto package=std::find_if(state.catalog.begin(),state.catalog.end(),[&](const auto& p){return p.id==file.owner;});
    if(package==state.catalog.end())return false;
    const auto root=PackageDirectory(file.owner),expected=root+L"\\"+file.relative;
    return Equal(expected,file.path)&&RealPath(file.path)&&Below(file.path,root);
}
inline coop_ini::Limits IniLimits(){coop_ini::Limits limits;limits.documentBytes=256*1024;return limits;}
inline bool OpenIni(const IniFile& file,IniOpen& opened){
    opened=IniOpen{};opened.file=file;std::string bytes;
    if(!IniBound(file)){opened.error=L"This INI no longer belongs to the displayed mod.";return false;}
    for(const auto& p:state.catalog)if(p.id==file.owner)opened.file.readOnly=IniPolicy(p,file.relative);
    if(!ReadText(file.path,bytes,256*1024)){opened.error=L"This INI is too large, linked, busy or unreadable.";return false;}
    coop_ini::Error why;if(!coop_ini::Parse(bytes,opened.document,why,IniLimits())){
        opened.error=L"Read-only: "+Wide(why.detail);return false;}
    opened.codepage=opened.document.EncodingKind()==coop_ini::Encoding::Utf8Bom?CP_UTF8:CP_ACP;return true;
}
inline bool IniOwned(const IniFile& file){
    if(state.ready||state.fatal)return false;
    const auto package=std::find_if(state.catalog.begin(),state.catalog.end(),[&](const auto& p){return p.id==file.owner;});
    if(package==state.catalog.end()||!IniPolicy(*package,file.relative).empty())return false;
    return IniBound(file);
}
inline std::unique_ptr<Handle> IniHandle(const std::wstring& path,DWORD access,DWORD creation){
    if(creation==OPEN_EXISTING&&!RealPath(path))return {};
    auto handle=std::make_unique<Handle>(CreateFileW(path.c_str(),access,FILE_SHARE_READ,nullptr,creation,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(handle->value==INVALID_HANDLE_VALUE)return {};wchar_t actual[32768]{};BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFinalPathNameByHandleW(handle->value,actual,DWORD(std::size(actual)),FILE_NAME_NORMALIZED)||!Equal(actual,L"\\\\?\\"+path)||
       !GetFileInformationByHandle(handle->value,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return {};
    return handle;
}
inline bool IniRead(HANDLE file,std::string& out){
    LARGE_INTEGER length{},zero{};DWORD got=0;
    if(!GetFileSizeEx(file,&length)||length.QuadPart<0||length.QuadPart>256*1024||!SetFilePointerEx(file,zero,nullptr,FILE_BEGIN))return false;
    out.assign(size_t(length.QuadPart),0);return out.empty()||ReadFile(file,out.data(),DWORD(out.size()),&got,nullptr)&&got==out.size();
}
inline bool IniRename(HANDLE file,const std::wstring& name){
    std::vector<uint8_t> storage(sizeof(FILE_RENAME_INFO)+name.size()*sizeof(wchar_t));auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists=FALSE;rename->RootDirectory=nullptr;rename->FileNameLength=DWORD(name.size()*sizeof(wchar_t));std::memcpy(rename->FileName,name.data(),rename->FileNameLength);
    return SetFileInformationByHandle(file,FileRenameInfo,rename,DWORD(storage.size()))!=FALSE;
}
inline bool SaveIni(IniOpen& opened,size_t rowId,const std::wstring& value,std::wstring& message){
    message.clear();if(!IniOwned(opened.file)){message=L"This file is read-only or no longer belongs to its mod.";return false;}
    std::string replacement;if(!IniBytes(value,opened.codepage,replacement)){message=L"This value cannot be represented in the INI's original encoding.";return false;}
    const auto parent=opened.file.path.substr(0,opened.file.path.find_last_of(L'\\'));
    auto mods=Pin(state.root+L"\\mods",true),package=Pin(PackageDirectory(opened.file.owner),true),folder=Pin(parent,true);
    if(!mods||!package||!folder){message=L"The owning settings directory changed.";return false;}
    auto original=IniHandle(opened.file.path,GENERIC_READ|DELETE,OPEN_EXISTING);std::string current,output;
    if(!original||!IniRead(original->value,current)){message=L"The INI is busy. Close the game or other editor and retry.";return false;}
    coop_ini::Error why;if(!coop_ini::Render(opened.document,current,{{rowId,replacement}},output,why,IniLimits())){message=Wide(why.detail);return false;}
    if(output==current){message=L"No changes to save.";return true;}
    const auto work=state.root+L"\\mods\\_undercoop\\state";auto workPin=Pin(work,true);
    if(!workPin){message=L"The private settings workspace is unavailable.";return false;}
    for(const auto* child:{L"ini-backups",L"ini-staging"}){
        const auto path=work+L"\\"+child;
        if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES&&!CreateDirectoryW(path.c_str(),nullptr)){message=L"Cannot create the settings backup folder.";return false;}
    }
    auto backups=Pin(work+L"\\ini-backups",true),staging=Pin(work+L"\\ini-staging",true);
    if(!backups||!staging){message=L"Settings backup folders are linked or unreadable.";return false;}
    static LONG serial=0;const auto token=std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(InterlockedIncrement(&serial));
    const auto staged=work+L"\\ini-staging\\"+token+L".pending",backup=work+L"\\ini-backups\\"+token+L".ini";
    if(staged.size()>=MAX_PATH||backup.size()>=MAX_PATH){message=L"The settings backup path is too long.";return false;}
    auto next=IniHandle(staged,GENERIC_READ|GENERIC_WRITE|DELETE,CREATE_NEW);DWORD written=0;
    if(!next||!WriteFile(next->value,output.data(),DWORD(output.size()),&written,nullptr)||written!=output.size()||!FlushFileBuffers(next->value)){
        message=L"Cannot stage settings. The original INI was not changed.";return false;}
    std::string verified;if(!IniRead(next->value,verified)||verified!=output){message=L"Settings verification failed; the original INI was not changed.";return false;}
    // The old file is locked against other writers/deletes until it is safely
    // backed up. Publish without replacement, so a concurrently created file
    // is never overwritten. Failure restores the original name when possible.
    if(!IniRename(original->value,backup)){message=L"Cannot back up the INI. The original was not changed.";return false;}
    if(!IniRename(next->value,opened.file.path)){
        const bool restored=IniRename(original->value,opened.file.path);
        message=restored?L"Save failed; the original INI was restored.":L"Save failed. Your original INI is safe at: "+backup;return false;
    }
    original.reset();next.reset();
    // Backup metadata is separate; the actual backup remains byte-for-byte.
    const auto record=work+L"\\ini-backups\\"+token+L".txt";
    auto metadata=IniHandle(record,GENERIC_WRITE,CREATE_NEW);
    if(metadata){const auto text=opened.file.owner+"/"+coop_mod_scan::Utf8(opened.file.relative)+"\r\n";DWORD ignored=0;WriteFile(metadata->value,text.data(),DWORD(text.size()),&ignored,nullptr);}
    IniOpen fresh;if(!OpenIni(opened.file,fresh)){message=L"Saved with a backup, but reload failed. Reopen settings.";return false;}
    opened=std::move(fresh);message=L"Saved. A copy of the previous INI is kept in mods/_undercoop/state/ini-backups.";return true;
}
