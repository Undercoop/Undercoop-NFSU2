// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <map>
#include "CoopModInventory.h"
#include "CoopPhysicalSaveReadWin.h"
#pragma comment(lib,"bcrypt.lib")
namespace coop_mod_scan {
// Set once by the experimental manager before its observer starts. Ordinary
// builds retain physical paths and their exact original containment checks.
inline std::wstring (*effectivePath)(const std::wstring&)=nullptr;
struct Sha256 {
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    Sha256(){BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0);}
    ~Sha256(){if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    Sha256(const Sha256&)=delete;Sha256& operator=(const Sha256&)=delete;
    bool Begin(BCRYPT_HASH_HANDLE& hash)const {
        return algorithm&&BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    }
    static bool Add(BCRYPT_HASH_HANDLE hash,const uint8_t* data,size_t size) {
        return size<=UINT32_MAX&&BCryptHashData(hash,const_cast<PUCHAR>(data),ULONG(size),0)>=0;
    }
    static bool End(BCRYPT_HASH_HANDLE hash,coop_mods::Hash& output) {
        const bool okay=BCryptFinishHash(hash,output.data(),ULONG(output.size()),0)>=0;
        BCryptDestroyHash(hash);return okay;
    }
    bool Bytes(const void* bytes,size_t size,coop_mods::Hash& output)const {
        BCRYPT_HASH_HANDLE hash=nullptr;if(!Begin(hash))return false;
        if(!Add(hash,static_cast<const uint8_t*>(bytes),size)){BCryptDestroyHash(hash);return false;}
        return End(hash,output);
    }
};
inline std::string Utf8(const std::wstring& text) {
    if(text.empty())return {};
    const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    if(count<=0)return {};std::string out(size_t(count),'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),out.data(),count,nullptr,nullptr))return {};
    return out;
}
inline bool RelativeName(std::wstring relative,std::string& output) {
    for(auto& c:relative)if(c==L'\\')c=L'/';
    const int count=LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,relative.data(),int(relative.size()),nullptr,0,nullptr,nullptr,0);
    if(count<=0)return false;std::wstring lower(size_t(count),L'\0');
    if(!LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,relative.data(),int(relative.size()),lower.data(),count,nullptr,nullptr,0))return false;
    output=Utf8(lower);return coop_mods::SafePath(output);
}
struct File {
    std::wstring absolute;std::string relative;
    bool direct=false; // Already a verified physical package path, not a game-relative lookup.
    bool overlay=false; // Selected resource overlays intentionally replace stock resources.
    bool inventory=true; // A loader alias of an already counted effective file.
};
inline bool (*additionalFiles)(const std::wstring&,std::vector<File>&,std::string&)=nullptr;
struct Inventory {
    std::vector<coop_mods::Entry> entries;std::vector<uint8_t> blob;coop_mods::Hash hash{};
    std::string error;uint64_t bytes=0;DWORD milliseconds=0;
    bool Ready()const{return error.empty()&&!blob.empty();}
};
inline bool Gather(const std::wstring& root,const std::wstring& relative,std::vector<File>& files,std::string& error,unsigned depth=0) {
    if(depth>20){error="directory nesting limit exceeded";return false;}
    const auto directory=root+(relative.empty()?L"":L"\\"+relative);
    WIN32_FIND_DATAW item{};HANDLE find=FindFirstFileW((directory+L"\\*").c_str(),&item);
    if(find==INVALID_HANDLE_VALUE){error="cannot enumerate "+Utf8(relative);return false;}
    bool okay=true;
    do {
        if(!std::wcscmp(item.cFileName,L".")||!std::wcscmp(item.cFileName,L".."))continue;
        const auto child=relative.empty()?std::wstring(item.cFileName):relative+L"\\"+item.cFileName;
        std::string name;
        if(!RelativeName(child,name)){error="unsupported relative path";okay=false;break;}
        const auto slash=name.find('/');const auto first=name.substr(0,slash);
        if(coop_mods::IsPrivateDirectory(first))continue;
        const bool isDirectory=(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        if(isDirectory&&coop_mods::IsPrivateDirectory(coop_mods::Lower(Utf8(item.cFileName))))continue;
        if(!isDirectory&&!coop_mods::IncludeFile(name))continue;
        if(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){error="linked files/folders are not allowed: "+name;okay=false;break;}
        if(isDirectory) {
            if(!Gather(root,child,files,error,depth+1)){okay=false;break;}
        }else {
            files.push_back({root+L"\\"+child,name});
            if(files.size()>coop_mods::kMaxEntries-1){error="too many runtime files";okay=false;break;}
        }
    }while(FindNextFileW(find,&item));
    if(okay&&GetLastError()!=ERROR_NO_MORE_FILES){error="directory enumeration failed";okay=false;}
    FindClose(find);return okay;
}
inline bool HashFile(const File& file,const Sha256& sha,coop_mods::Entry& entry,uint64_t& bytes,std::string& error,DWORD* openError=nullptr) {
    const auto physical=!file.direct&&effectivePath?effectivePath(file.absolute):file.absolute;
    if(openError)*openError=ERROR_SUCCESS;
    // A loader can redirect Win32 calls even for a "direct" inventory entry.
    // Read physical package/loose duplicates through the existing read-only
    // native file adapter so virtual listings cannot impersonate loose files.
    DWORD nativeError=ERROR_SUCCESS;
    const HANDLE handle=file.direct?coop_physical_save::OpenRead(physical,nativeError):
        CreateFileW(physical.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle==INVALID_HANDLE_VALUE){const DWORD code=file.direct?nativeError:GetLastError();
        if(openError)*openError=code;
        error="cannot read "+file.relative+" (Windows error "+std::to_string(code)+")";
        SetLastError(code);return false;}
    BY_HANDLE_FILE_INFORMATION before{},after{};BCRYPT_HASH_HANDLE hash=nullptr;
    bool okay=GetFileInformationByHandle(handle,&before)&&!(before.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&sha.Begin(hash);
    std::array<wchar_t,32768> resolved{};
    const DWORD finalLength=GetFinalPathNameByHandleW(handle,resolved.data(),DWORD(resolved.size()),FILE_NAME_NORMALIZED);
    const auto expected=L"\\\\?\\"+physical;
    okay=okay&&finalLength&&finalLength<resolved.size()&&
        CompareStringOrdinal(resolved.data(),int(finalLength),expected.data(),int(expected.size()),TRUE)==CSTR_EQUAL;
    const bool ini=file.relative=="scripts/nfsu2extraoptionssettings.ini"||file.relative=="scripts/nfsunderground2.widescreenfix.ini";
    std::string text;std::array<uint8_t,64*1024> buffer{};uint64_t readTotal=0;
    while(okay) {
        DWORD got=0;if(!ReadFile(handle,buffer.data(),DWORD(buffer.size()),&got,nullptr)){okay=false;break;}
        if(!got)break;readTotal+=got;
        if(ini&&readTotal<=1024*1024)text.append(reinterpret_cast<const char*>(buffer.data()),got);
        else if(ini){okay=false;break;}
        if(!ini&&!Sha256::Add(hash,buffer.data(),got)){okay=false;break;}
    }
    const uint64_t length=(uint64_t(before.nFileSizeHigh)<<32)|before.nFileSizeLow;
    okay=okay&&readTotal==length&&GetFileInformationByHandle(handle,&after)&&
        before.nFileSizeHigh==after.nFileSizeHigh&&before.nFileSizeLow==after.nFileSizeLow&&
        before.ftLastWriteTime.dwLowDateTime==after.ftLastWriteTime.dwLowDateTime&&
        before.ftLastWriteTime.dwHighDateTime==after.ftLastWriteTime.dwHighDateTime;
    CloseHandle(handle);entry.path=file.relative;entry.size=length;
    if(okay&&ini) {
        std::string normalized;
        if(coop_mods::CanonicalIni(file.relative,text,normalized)){text=std::move(normalized);entry.size=text.size();}
        okay=Sha256::Add(hash,reinterpret_cast<const uint8_t*>(text.data()),text.size());
    }
    if(okay){okay=Sha256::End(hash,entry.hash);hash=nullptr;}
    if(hash)BCryptDestroyHash(hash);
    if(!okay){error="file changed or could not be hashed: "+file.relative;return false;}
    bytes+=length;return true;
}
inline bool LooseFileAgrees(File loose,File selected,const Sha256& sha,std::string& error){
    if(selected.overlay||loose.absolute==selected.absolute)return true;
    loose.direct=true;selected.direct=true;coop_mods::Entry a,b;uint64_t bytes=0;DWORD openError=ERROR_SUCCESS;
    if(!HashFile(loose,sha,a,bytes,error,&openError)){
        // A virtual directory listing can name an overlay with no real loose
        // counterpart. Access, link and read errors are never treated absent.
        if(openError!=ERROR_FILE_NOT_FOUND&&openError!=ERROR_PATH_NOT_FOUND)return false;
        error.clear();return true;
    }
    if(!HashFile(selected,sha,b,bytes,error))return false;
    if(a.hash!=b.hash||a.size!=b.size){error="loose runtime file conflicts with selected package: "+selected.relative;return false;}
    return true;
}
inline bool SelectRuntimeFile(const std::wstring& root,File& file,const Sha256& sha,bool& selected,std::string& error){
    selected=false;if(!additionalFiles)return true;
    std::vector<File> extra;if(!additionalFiles(root,extra,error))return false;
    if(extra.size()>coop_mods::kMaxEntries){error="too many managed runtime files";return false;}
    for(auto& candidate:extra)if(candidate.relative==file.relative){
        candidate.direct=true;if(!LooseFileAgrees(file,candidate,sha,error))return false;
        file=std::move(candidate);selected=true;
    }return true;
}
inline bool GatherRuntime(const std::wstring& root,const std::wstring& relative,std::vector<File>& files,std::string& error){
    if(!additionalFiles)return Gather(root,relative,files,error);
    std::vector<File> extra,physical;
    if(!additionalFiles(root,extra,error))return false;
    if(extra.size()>coop_mods::kMaxEntries){error="too many managed runtime files";return false;}
    const auto directory=root+(relative.empty()?L"":L"\\"+relative);
    const auto attrs=GetFileAttributesW(directory.c_str());const auto code=GetLastError();
    if(attrs!=INVALID_FILE_ATTRIBUTES){if(!Gather(root,relative,physical,error))return false;}
    else if(relative.empty()||(code!=ERROR_FILE_NOT_FOUND&&code!=ERROR_PATH_NOT_FOUND)){
        error="cannot enumerate runtime directory";return false;
    }
    std::string subtree;if(!relative.empty()&&!RelativeName(relative,subtree)){error="invalid runtime subtree";return false;}
    if(!subtree.empty())subtree+='/';
    std::map<std::string,File> merged;for(auto& f:physical)merged.emplace(f.relative,std::move(f));
    Sha256 sha;
    for(auto& f:extra){
        if(!coop_mods::SafePath(f.relative)){error="unsafe managed inventory destination";return false;}
        if(!coop_mods::IncludeFile(f.relative)||!subtree.empty()&&f.relative.rfind(subtree,0)!=0)continue;
        if(!f.inventory){merged.erase(f.relative);continue;}
        f.direct=true;const auto old=merged.find(f.relative);
        // A real loose native/config duplicate must agree even when the
        // loader normally redirects its Win32 read to the selected package.
        if(old!=merged.end()&&!LooseFileAgrees(old->second,f,sha,error))return false;
        merged[f.relative]=std::move(f);
    }
    if(merged.size()>coop_mods::kMaxEntries-1){error="too many effective runtime files";return false;}
    for(auto& f:merged)files.push_back(std::move(f.second));return true;
}
inline Inventory Scan(const std::wstring& root,const std::string& runtimeSettings) {
    Inventory result;const DWORD start=GetTickCount();std::vector<File> files;
    const auto attributes=GetFileAttributesW(root.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
        result.error="game directory is missing or linked";return result;
    }
    Sha256 sha;if(!sha.algorithm){result.error="Windows SHA-256 unavailable";return result;}
    if(!GatherRuntime(root,L"",files,result.error))return result;
    for(const auto& file:files) {
        coop_mods::Entry entry;if(!HashFile(file,sha,entry,result.bytes,result.error))return result;
        result.entries.push_back(std::move(entry));
    }
    // Known CO-OP simulation options, never IPs, ports, names or save paths.
    coop_mods::Entry options;options.path="~undercoop/runtime-settings";options.size=runtimeSettings.size();
    if(!sha.Bytes(runtimeSettings.data(),runtimeSettings.size(),options.hash)){result.error="settings hash failed";return result;}
    result.entries.push_back(options);
    std::sort(result.entries.begin(),result.entries.end(),[](const auto& a,const auto& b){return a.path<b.path;});
    const auto exe=std::find_if(result.entries.begin(),result.entries.end(),[](const auto& e){return e.path=="speed2.exe";});
    if(exe==result.entries.end()){result.error="SPEED2.EXE is missing";return result;}
    if(!coop_mods::Encode(result.entries,result.blob)||!sha.Bytes(result.blob.data(),result.blob.size(),result.hash))result.error="inventory limit/encoding/hash failure";
    result.milliseconds=GetTickCount()-start;return result;
}
// A Windows notification is only a hint to compare content, not proof of a
// changed mod. Directory timestamps and identical writes are normal at startup.
struct Change {
    std::string path,reason;DWORD action=0,error=0;
};
// Optional managed-package settings guard. Ordinary installations retain the
// existing physical scanner and notifications unchanged.
inline bool (*managedChange)(const std::wstring&,const std::string&,Change&)=nullptr;
inline bool PrivatePath(const std::string& path) {
    for(size_t start=0;start<path.size();) {
        const auto end=path.find('/',start);
        if(coop_mods::IsPrivateDirectory(path.substr(start,end==std::string::npos?end:end-start)))return true;
        if(end==std::string::npos)break;start=end+1;
    }return false;
}
inline bool WideRelative(const std::string& path,std::wstring& output) {
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.data(),int(path.size()),nullptr,0);
    if(count<=0)return false;output.assign(size_t(count),L'\0');
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.data(),int(path.size()),output.data(),count))return false;
    for(auto& c:output)if(c==L'/')c=L'\\';return true;
}
inline auto FindEntry(const std::vector<coop_mods::Entry>& entries,const std::string& path) {
    return std::lower_bound(entries.begin(),entries.end(),path,[](const auto& entry,const auto& name){return entry.path<name;});
}
inline bool SameFile(const File& file,const std::vector<coop_mods::Entry>& baseline,const Sha256& sha,Change& change) {
    const auto expected=FindEntry(baseline,file.relative);
    if(expected==baseline.end()||expected->path!=file.relative){change.path=file.relative;change.reason="runtime file added";return false;}
    coop_mods::Entry current;uint64_t bytes=0;std::string error;
    if(!HashFile(file,sha,current,bytes,error)){change.path=file.relative;change.reason=error;change.error=GetLastError();return false;}
    if(current.size!=expected->size||current.hash!=expected->hash){change.path=file.relative;change.reason="runtime file content/settings changed";return false;}
    return true;
}
inline bool SamePath(const std::wstring& root,const std::string& path,const std::vector<coop_mods::Entry>& baseline,
    const Sha256& sha,Change& change,bool& directory) {
    directory=false;if(PrivatePath(path))return true;
    std::wstring relative;if(!WideRelative(path,relative)){change.reason="invalid UTF-8 path";return false;}
    const auto absolute=root+L"\\"+relative;
    const auto exact=FindEntry(baseline,path);const bool tracked=exact!=baseline.end()&&exact->path==path;
    const auto prefix=path+"/";const auto descendant=FindEntry(baseline,prefix);
    const bool descendants=descendant!=baseline.end()&&descendant->path.compare(0,prefix.size(),prefix)==0;
    const DWORD attrs=GetFileAttributesW(absolute.c_str()),error=GetLastError();
    if(attrs==INVALID_FILE_ATTRIBUTES) {
        if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND) {
            if(!tracked&&!descendants&&!coop_mods::IncludeFile(path))return true;
            change.reason="runtime path cannot be inspected";change.error=error;return false;
        }
        if(tracked&&additionalFiles){
            File file{absolute,path};bool selected=false;std::string errorText;
            if(!SelectRuntimeFile(root,file,sha,selected,errorText)){change.reason=errorText;change.error=GetLastError();return false;}
            if(selected)return SameFile(file,baseline,sha,change);
        }
        if(tracked||descendants){change.reason=descendants?"runtime directory removed/renamed":"runtime file removed/renamed";return false;}
        return true; // untracked empty directory or already-removed temporary output
    }
    directory=(attrs&FILE_ATTRIBUTE_DIRECTORY)!=0;
    // Classify directories BEFORE extension exclusions. A populated directory
    // named foo.log is not a log file and may carry real addon assets.
    if(directory) {
        if(attrs&FILE_ATTRIBUTE_REPARSE_POINT){change.reason="runtime directory became linked";return false;}
        if(tracked){change.reason="runtime file replaced with a directory";return false;}
        std::vector<File> files;std::string errorText;
        if(!GatherRuntime(root,relative,files,errorText)){change.reason=errorText;change.error=GetLastError();return false;}
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.relative<b.relative;});
        auto expected=descendant;size_t index=0;
        while(expected!=baseline.end()&&expected->path.compare(0,prefix.size(),prefix)==0) {
            if(index>=files.size()||files[index].relative!=expected->path) {
                change.reason="runtime directory file set changed";return false;
            }
            ++index;++expected;
        }
        if(index!=files.size()){change.reason="runtime directory gained files";return false;}
        for(const auto& file:files)if(!SameFile(file,baseline,sha,change))return false;
        return true; // includes newly-created EMPTY CameraToolKit/Tracks
    }
    if(descendants){change.reason="runtime directory replaced with a file";return false;}
    if(!tracked&&!coop_mods::IncludeFile(path))return true;
    if(attrs&FILE_ATTRIBUTE_REPARSE_POINT){change.reason="runtime file became linked";return false;}
    File file{absolute,path};bool selected=false;std::string errorText;
    if(!SelectRuntimeFile(root,file,sha,selected,errorText)){change.reason=errorText;change.error=GetLastError();return false;}
    return SameFile(file,baseline,sha,change);
}
inline bool Changed(const std::wstring& root,const uint8_t* bytes,DWORD length,
    const std::vector<coop_mods::Entry>& baseline,Change& change) {
    change={};
    if(!bytes||!length){change.reason="notification overflow/empty result";return true;}
    std::vector<std::pair<std::string,DWORD>> paths;
    for(size_t at=0;at<length;) {
        if(length-at<12){change.reason="truncated notification";return true;}
        const auto* item=reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(bytes+at);
        if(item->Action<FILE_ACTION_ADDED||item->Action>FILE_ACTION_RENAMED_NEW_NAME||
            (item->FileNameLength&1)||!item->FileNameLength||item->FileNameLength>length-at-12) {
            change.reason="invalid notification action/name length";return true;
        }
        std::string path;
        if(!RelativeName(std::wstring(item->FileName,item->FileNameLength/2),path)){change.reason="unsafe notification path";return true;}
        if(managedChange&&managedChange(root,path,change)){change.action=item->Action;return true;}
        if(!PrivatePath(path))paths.push_back({std::move(path),item->Action});
        if(!item->NextEntryOffset)break;
        if((item->NextEntryOffset&3)||item->NextEntryOffset<12+item->FileNameLength||item->NextEntryOffset>length-at) {
            change.reason="invalid notification chain";return true;
        }
        at+=item->NextEntryOffset;
    }
    std::sort(paths.begin(),paths.end());Sha256 sha;
    if(!sha.algorithm){change.reason="Windows SHA-256 unavailable";return true;}
    std::vector<std::string> directories;std::string previous;
    for(const auto& item:paths) {
        if(item.first==previous)continue;previous=item.first;
        bool covered=false;for(const auto& prefix:directories)if(item.first.compare(0,prefix.size(),prefix)==0){covered=true;break;}
        if(covered)continue;
        change.path=item.first;change.action=item.second;bool directory=false;
        if(!SamePath(root,item.first,baseline,sha,change,directory))return true;
        if(directory)directories.push_back(item.first+"/");
    }
    change={};return false;
}
inline std::string Hex(const coop_mods::Hash& hash) {
    static const char digits[]="0123456789abcdef";std::string out;out.reserve(64);
    for(auto c:hash){out.push_back(digits[c>>4]);out.push_back(digits[c&15]);}return out;
}
}
