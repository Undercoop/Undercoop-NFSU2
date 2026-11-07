// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopCareerHistory.h"

#if !defined(_WIN32)
#error CoopCareerHistoryWin requires Windows.
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <utility>

namespace coop_career_history {
enum class Result { Ok,Missing,Invalid,Stale,IoError };
struct Guard {
    bool probed=false,exists=false;
    std::wstring root;
    BY_HANDLE_FILE_INFORMATION identity{};
    std::vector<std::uint8_t> bytes;
};
namespace win_detail {
inline constexpr wchar_t kLeaf[]=L".undercoop-careers.dat";
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;explicit Handle(HANDLE h):value(h){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:value(other.value){other.value=INVALID_HANDLE_VALUE;}
    Handle& operator=(Handle&& other) noexcept {
        if(this!=&other){Close();value=other.value;other.value=INVALID_HANDLE_VALUE;}return *this;
    }
    ~Handle(){Close();}
    void Close(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
    explicit operator bool() const{return value!=INVALID_HANDLE_VALUE;}
};
inline bool EqualPath(const std::wstring& a,const std::wstring& b) {
    return CompareStringOrdinal(a.data(),int(a.size()),b.data(),int(b.size()),TRUE)==CSTR_EQUAL;
}
inline std::wstring Plain(std::wstring path) {
    std::replace(path.begin(),path.end(),L'/',L'\\');
    if(path.rfind(L"\\\\?\\UNC\\",0)==0)return L"\\\\"+path.substr(8);
    if(path.rfind(L"\\\\?\\",0)==0)return path.substr(4);
    return path;
}
inline std::wstring Extended(const std::wstring& path) {
    return path.rfind(L"\\\\",0)==0?L"\\\\?\\UNC\\"+path.substr(2):L"\\\\?\\"+path;
}
inline bool SafeComponent(const std::wstring& component) {
    if(component.empty()||component==L"."||component==L".."||component.back()==L'.'||component.back()==L' ')return false;
    for(wchar_t c:component)if(c<32||c==L':'||c==L'"'||c==L'<'||c==L'>'||c==L'|'||c==L'*'||c==L'?')return false;
    std::wstring stem=component.substr(0,component.find(L'.'));
    for(auto& c:stem)if(c>=L'a'&&c<=L'z')c=wchar_t(c-L'a'+L'A');
    if(stem==L"CON"||stem==L"PRN"||stem==L"AUX"||stem==L"NUL")return false;
    return !(stem.size()==4&&stem[3]>=L'1'&&stem[3]<=L'9'&&(stem.substr(0,3)==L"COM"||stem.substr(0,3)==L"LPT"));
}
inline bool CanonicalRoot(const std::wstring& input,std::wstring& root) {
    root.clear();auto path=Plain(input);
    if(path.empty()||path.size()>30000||path.find(L'\0')!=std::wstring::npos)return false;
    while(path.size()>3&&path.back()==L'\\')path.pop_back();
    const bool drive=path.size()>3&&((path[0]>=L'A'&&path[0]<=L'Z')||(path[0]>=L'a'&&path[0]<=L'z'))&&path[1]==L':'&&path[2]==L'\\';
    const bool unc=path.size()>4&&path.rfind(L"\\\\",0)==0&&path[2]!=L'.'&&path[2]!=L'?';
    if(!drive&&!unc)return false;
    for(std::size_t start=drive?3:2;start<path.size();) {
        const auto end=path.find(L'\\',start);
        if(!SafeComponent(path.substr(start,end==std::wstring::npos?end:end-start)))return false;
        if(end==std::wstring::npos)break;start=end+1;
    }
    wchar_t full[32768]{};const DWORD n=GetFullPathNameW(path.c_str(),DWORD(std::size(full)),full,nullptr);
    if(!n||n>=std::size(full)||!EqualPath(path,Plain(full)))return false;
    root=Plain(full);return true;
}
inline bool PhysicalHandle(HANDLE file,const std::wstring& expected,bool directory,BY_HANDLE_FILE_INFORMATION* returned=nullptr) {
    BY_HANDLE_FILE_INFORMATION info{};wchar_t finalPath[32768]{};
    const DWORD n=GetFinalPathNameByHandleW(file,finalPath,DWORD(std::size(finalPath)),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!n||n>=std::size(finalPath)||!EqualPath(Plain(finalPath),expected)||
       !GetFileInformationByHandle(file,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||
       bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory||(!directory&&info.nNumberOfLinks!=1))return false;
    if(returned)*returned=info;return true;
}
inline bool PinRoot(const std::wstring& root,std::vector<Handle>& pins) {
    pins.clear();std::size_t end=3;
    if(root.rfind(L"\\\\",0)==0) {
        const auto server=root.find(L'\\',2);if(server==std::wstring::npos)return false;
        const auto share=root.find(L'\\',server+1);end=share==std::wstring::npos?root.size():share;
    }
    for(;;) {
        const auto prefix=root.substr(0,end);
        Handle dir(CreateFileW(Extended(prefix).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
            OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!dir||!PhysicalHandle(dir.value,prefix,true))return false;
        pins.push_back(std::move(dir));if(end==root.size())return true;
        const auto next=root.find(L'\\',end==3?3:end+1);end=next==std::wstring::npos?root.size():next;
    }
}
inline bool SameIdentity(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow&&
        a.nFileSizeHigh==b.nFileSizeHigh&&a.nFileSizeLow==b.nFileSizeLow&&
        a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime&&a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime;
}
inline Result Probe(const std::wstring& root,History& history,Guard& guard,Handle* held=nullptr) {
    const auto path=root+L"\\"+kLeaf;
    const auto attributes=GetFileAttributesW(Extended(path).c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return Result::Invalid;
    Handle file(CreateFileW(Extended(path).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,
        OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file) {
        const auto error=GetLastError();
        if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)return Result::IoError;
        guard={};guard.probed=true;guard.root=root;history=History{};return Result::Missing;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if(!PhysicalHandle(file.value,path,false,&info)||info.nFileSizeHigh||info.nFileSizeLow<kHeaderBytes||info.nFileSizeLow>kMaxFileBytes)return Result::Invalid;
    std::vector<std::uint8_t> bytes(info.nFileSizeLow);DWORD got=0;
    if(!ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&got,nullptr)||got!=bytes.size())return Result::IoError;
    History staged;if(!Decode(bytes.data(),bytes.size(),staged))return Result::Invalid;
    Guard next;next.probed=next.exists=true;next.root=root;next.identity=info;next.bytes=std::move(bytes);
    guard=std::move(next);history=std::move(staged);if(held)*held=std::move(file);return Result::Ok;
}
struct Temporary {
    Handle file;bool published=false;
    ~Temporary(){if(file&&!published){FILE_DISPOSITION_INFO remove{};remove.DeleteFile=TRUE;SetFileInformationByHandle(file.value,FileDispositionInfo,&remove,sizeof(remove));}}
};
inline bool Publish(HANDLE temporary,const std::wstring& destination,bool replace) {
    const auto name=Extended(destination);
    std::vector<std::uint8_t> storage(sizeof(FILE_RENAME_INFO)+name.size()*sizeof(wchar_t),0);
    auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists=replace?TRUE:FALSE;rename->FileNameLength=DWORD(name.size()*sizeof(wchar_t));
    std::memcpy(rename->FileName,name.data(),rename->FileNameLength);
    return SetFileInformationByHandle(temporary,FileRenameInfo,rename,DWORD(storage.size()))!=FALSE;
}
} // namespace win_detail

inline Result Load(const std::wstring& root,History& history,Guard& guard) {
    guard={};std::wstring normalized;
    if(!win_detail::CanonicalRoot(root,normalized))return Result::Invalid;
    std::vector<win_detail::Handle> pins;
    if(!win_detail::PinRoot(normalized,pins))return Result::Invalid;
    return win_detail::Probe(normalized,history,guard);
}
inline Result Load(const std::wstring& root,History& history) {Guard ignored;return Load(root,history,ignored);}

inline Result Save(const std::wstring& root,const History& history,Guard& guard) {
    using namespace win_detail;
    std::wstring normalized;std::vector<std::uint8_t> bytes;
    if(!CanonicalRoot(root,normalized)||!guard.probed||!EqualPath(guard.root,normalized)||!Encode(history,bytes))return Result::Invalid;
    std::vector<Handle> pins;if(!PinRoot(normalized,pins))return Result::Invalid;
    // Cooperating game instances serialize metadata publication. The lock is a
    // newly-created, verified local file and disappears on handle close/crash.
    const auto lockPath=normalized+L"\\.undercoop-careers.lock";
    Handle lock(CreateFileW(Extended(lockPath).c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!lock||!PhysicalHandle(lock.value,lockPath,false))return Result::IoError;
    History current;Guard observed;Handle old;
    const auto status=Probe(normalized,current,observed,&old);
    if(status!=Result::Ok&&status!=Result::Missing)return status;
    if(guard.exists!=observed.exists||(guard.exists&&
       (!SameIdentity(guard.identity,observed.identity)||guard.bytes!=observed.bytes)))return Result::Stale;
    if(observed.exists&&observed.bytes==bytes){guard=std::move(observed);return Result::Ok;}
    static LONG serial=0;
    const auto temporaryPath=normalized+L"\\.undercoop-careers-"+std::to_wstring(GetCurrentProcessId())+L"-"+
        std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(InterlockedIncrement(&serial))+L".tmp";
    Temporary temporary;
    temporary.file=Handle(CreateFileW(Extended(temporaryPath).c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,
        FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!temporary.file||!PhysicalHandle(temporary.file.value,temporaryPath,false))return Result::IoError;
    DWORD wrote=0;
    if(!WriteFile(temporary.file.value,bytes.data(),DWORD(bytes.size()),&wrote,nullptr)||wrote!=bytes.size()||
       !FlushFileBuffers(temporary.file.value))return Result::IoError;
    LARGE_INTEGER zero{};DWORD got=0;std::vector<std::uint8_t> checked(bytes.size());
    if(!SetFilePointerEx(temporary.file.value,zero,nullptr,FILE_BEGIN)||
       !ReadFile(temporary.file.value,checked.data(),DWORD(checked.size()),&got,nullptr)||got!=checked.size()||checked!=bytes)return Result::IoError;
    // Re-probe the exact path while the old file remains locked against writes.
    History checkHistory;Guard checkGuard;
    const auto recheck=Probe(normalized,checkHistory,checkGuard);
    if(recheck!=status||checkGuard.exists!=observed.exists||(observed.exists&&
       (!SameIdentity(checkGuard.identity,observed.identity)||checkGuard.bytes!=observed.bytes)))return Result::Stale;
    const auto destination=normalized+L"\\"+kLeaf;
    // Classic Windows rename cannot replace an open target on every supported
    // filesystem. The cooperative publication lock and pinned ancestors stay
    // held while releasing the read lease and atomically publishing the rename.
    old.Close();
    if(!Publish(temporary.file.value,destination,observed.exists))return Result::IoError;
    temporary.published=true;old.Close();temporary.file.Close();
    History verified;Guard saved;
    if(Probe(normalized,verified,saved)!=Result::Ok||saved.bytes!=bytes)return Result::IoError;
    guard=std::move(saved);return Result::Ok;
}
// No implicit blind overwrite. Existing records require a Guard from Load.
inline Result Save(const std::wstring& root,const History& history) {
    History existing;Guard guard;const auto status=Load(root,existing,guard);
    if(status!=Result::Missing)return status==Result::Ok?Result::Stale:status;
    return Save(root,history,guard);
}
} // namespace coop_career_history
