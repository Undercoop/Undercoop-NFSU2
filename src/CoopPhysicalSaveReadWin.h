// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopProfileContext.h"
#include <winternl.h>
#include <limits>

// Physical, read-only save admission. The game's VFS may detour Win32 path
// APIs globally; these calls intentionally inspect real filesystem objects.
// No syscall stubs, hook changes, write access, or missing-file creation.
namespace coop_physical_save {
using OpenFn=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,ULONG,ULONG);
using QueryFn=NTSTATUS(NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
using ErrorFn=ULONG(NTAPI*)(NTSTATUS);
struct Api {OpenFn open=nullptr;QueryFn query=nullptr;ErrorFn error=nullptr;};
struct Diagnostic {const char* stage="none";NTSTATUS status=0;DWORD error=ERROR_SUCCESS;std::wstring path;};
inline thread_local Diagnostic diagnostic;
inline const Api& Windows(){
    static const Api api=[] {Api p;const auto module=GetModuleHandleW(L"ntdll.dll");if(module){
        p.open=reinterpret_cast<OpenFn>(GetProcAddress(module,"NtOpenFile"));
        p.query=reinterpret_cast<QueryFn>(GetProcAddress(module,"NtQueryInformationFile"));
        p.error=reinterpret_cast<ErrorFn>(GetProcAddress(module,"RtlNtStatusToDosError"));}return p;}();return api;
}
#ifdef UNDERCOOP_PHYSICAL_SAVE_TEST
inline const Api* testApi=nullptr;
#endif
inline const Api& Native(){
#ifdef UNDERCOOP_PHYSICAL_SAVE_TEST
    if(testApi)return *testApi;
#endif
    return Windows();
}
inline DWORD WinError(NTSTATUS status){
    const auto convert=Native().error;const DWORD error=convert?convert(status):ERROR_PROC_NOT_FOUND;
    return error==ERROR_MR_MID_NOT_FOUND?ERROR_GEN_FAILURE:error;
}
inline bool Fail(const char* stage,DWORD error,const std::wstring& path,NTSTATUS status=0){
    diagnostic={stage,status,error,path};SetLastError(error);return false;
}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;explicit Handle(HANDLE file):value(file){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other)noexcept:value(other.value){other.value=INVALID_HANDLE_VALUE;}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
    HANDLE Release(){const auto result=value;value=INVALID_HANDLE_VALUE;return result;}
};
struct Basic {LARGE_INTEGER creation,access,write,change;ULONG attributes;};
struct Standard {LARGE_INTEGER allocation,end;ULONG links;BOOLEAN deleting,directory;};
struct Internal {LARGE_INTEGER index;};
static_assert(sizeof(Basic)==40&&sizeof(Standard)==24&&sizeof(Internal)==8,"Native file information ABI");
inline FILETIME Time(LARGE_INTEGER value){return {DWORD(value.LowPart),DWORD(value.HighPart)};}
inline bool Query(HANDLE file,const std::wstring& expected,BY_HANDLE_FILE_INFORMATION& info){
    const auto& api=Native();Basic basic{};Standard standard{};Internal internal{};IO_STATUS_BLOCK io{};
    const struct Item{void* data;ULONG bytes;FILE_INFORMATION_CLASS kind;} items[]{
        {&basic,sizeof(basic),static_cast<FILE_INFORMATION_CLASS>(4)},
        {&standard,sizeof(standard),static_cast<FILE_INFORMATION_CLASS>(5)},
        {&internal,sizeof(internal),static_cast<FILE_INFORMATION_CLASS>(6)}};
    for(const auto& item:items){const auto status=api.query(file,&io,item.data,item.bytes,item.kind);
        if(status<0)return Fail("nt-query",WinError(status),expected,status);
        if(io.Information<item.bytes)return Fail("nt-query-size",ERROR_INVALID_DATA,expected);}
    if(basic.attributes&FILE_ATTRIBUTE_REPARSE_POINT)return Fail("reparse-point",ERROR_CANT_ACCESS_FILE,expected);
    if(standard.end.QuadPart<0||standard.deleting||bool(standard.directory)!=bool(basic.attributes&FILE_ATTRIBUTE_DIRECTORY))
        return Fail("physical-file-kind",ERROR_INVALID_DATA,expected);
    BY_HANDLE_FILE_INFORMATION actual{};
    if(GetFileType(file)!=FILE_TYPE_DISK||!GetFileInformationByHandle(file,&actual))
        return Fail("physical-handle",GetLastError()?GetLastError():ERROR_NOT_SUPPORTED,expected);
    const uint64_t index=(uint64_t(actual.nFileIndexHigh)<<32)|actual.nFileIndexLow;
    if(index!=uint64_t(internal.index.QuadPart)||actual.nNumberOfLinks!=standard.links||
       (actual.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return Fail("physical-identity",ERROR_INVALID_DATA,expected);
    std::vector<wchar_t> path(32768);const DWORD length=GetFinalPathNameByHandleW(file,path.data(),DWORD(path.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    std::wstring canonical;
    if(!length||length>=path.size()||!coop_save::CanonicalWindowsPath(std::wstring(path.data(),length),canonical)||
       !coop_save::EqualPathText(canonical,expected))return Fail("physical-final-path",ERROR_ACCESS_DENIED,expected);
    actual.dwFileAttributes=basic.attributes;actual.ftCreationTime=Time(basic.creation);
    actual.ftLastAccessTime=Time(basic.access);actual.ftLastWriteTime=Time(basic.write);
    actual.nFileSizeHigh=DWORD(uint64_t(standard.end.QuadPart)>>32);actual.nFileSizeLow=DWORD(standard.end.LowPart);
    info=actual;return true;
}
inline bool Canonical(const std::wstring& input,std::wstring& canonical,std::vector<std::wstring>& parts){
    parts.clear();
    if(input.size()>32760||!coop_save::CanonicalWindowsPath(input,canonical)||
       !coop_save::EqualPathText(coop_save::NormalizedPathSpelling(input),canonical))
        return Fail("canonical-path",ERROR_INVALID_NAME,input);
    size_t end=3;
    if(canonical.rfind(L"\\\\",0)==0){const auto server=canonical.find(L'\\',2);
        if(server==std::wstring::npos)return Fail("unc-root",ERROR_INVALID_NAME,input);
        const auto share=canonical.find(L'\\',server+1);end=share==std::wstring::npos?canonical.size():share;}
    for(;;){parts.push_back(canonical.substr(0,end));if(end==canonical.size())break;
        const auto next=canonical.find(L'\\',end==3?3:end+1);end=next==std::wstring::npos?canonical.size():next;}
    return true;
}
inline HANDLE Open(const std::wstring& input,bool read,DWORD share,BY_HANDLE_FILE_INFORMATION& info,DWORD& error){
    diagnostic={};error=ERROR_SUCCESS;
    const auto& api=Native();if(!api.open||!api.query||!api.error){Fail("native-api",ERROR_PROC_NOT_FOUND,input);error=diagnostic.error;return INVALID_HANDLE_VALUE;}
    if(share&~DWORD(FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE)){Fail("share-mode",ERROR_INVALID_PARAMETER,input);error=diagnostic.error;return INVALID_HANDLE_VALUE;}
    std::wstring canonical;std::vector<std::wstring> parts;if(!Canonical(input,canonical,parts)){error=diagnostic.error;return INVALID_HANDLE_VALUE;}
    std::vector<Handle> held;held.reserve(parts.size());BY_HANDLE_FILE_INFORMATION actual{};
    for(size_t i=0;i<parts.size();++i){const bool last=i+1==parts.size();
        std::wstring name;
        if(!i)name=parts[i].rfind(L"\\\\",0)==0?L"\\??\\UNC\\"+parts[i].substr(2):L"\\??\\"+parts[i];
        else name=parts[i].substr(parts[i-1].size()+(parts[i-1].back()==L'\\'?0:1));
        if(name.empty()||name.size()>32766){Fail("native-name",ERROR_INVALID_NAME,parts[i]);error=diagnostic.error;return INVALID_HANDLE_VALUE;}
        UNICODE_STRING text{};text.Buffer=name.data();text.Length=USHORT(name.size()*sizeof(wchar_t));text.MaximumLength=text.Length;
        OBJECT_ATTRIBUTES attributes{};attributes.Length=sizeof(attributes);attributes.RootDirectory=i?held.back().value:nullptr;
        attributes.ObjectName=&text;attributes.Attributes=0x40; // OBJ_CASE_INSENSITIVE
        IO_STATUS_BLOCK io{};HANDLE raw=nullptr;
        const ACCESS_MASK access=last&&read?FILE_GENERIC_READ:FILE_READ_ATTRIBUTES|SYNCHRONIZE;
        const ULONG sharing=last&&read?share:FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE;
        // OPEN_REPARSE_POINT + OPEN_FOR_BACKUP_INTENT + SYNCHRONOUS_IO_NONALERT.
        const ULONG options=0x00200000u|0x00004000u|0x00000020u;
        const auto status=api.open(&raw,access,&attributes,&io,sharing,options);Handle file(raw);
        if(status<0||!raw||raw==INVALID_HANDLE_VALUE){Fail("nt-open",status<0?WinError(status):ERROR_INVALID_HANDLE,parts[i],status);error=diagnostic.error;return INVALID_HANDLE_VALUE;}
        if(!Query(raw,parts[i],actual)){error=diagnostic.error;return INVALID_HANDLE_VALUE;}
        if((!last&&!(actual.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))||(last&&read&&(actual.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))){
            Fail("path-object-kind",ERROR_DIRECTORY,parts[i]);error=diagnostic.error;return INVALID_HANDLE_VALUE;}
        held.push_back(std::move(file));
    }
    info=actual;diagnostic={"loaded",0,ERROR_SUCCESS,canonical};SetLastError(ERROR_SUCCESS);
    return held.back().Release();
}
inline bool Attributes(const std::wstring& path,DWORD& attributes,DWORD& error,BY_HANDLE_FILE_INFORMATION* metadata=nullptr){
    attributes=0;BY_HANDLE_FILE_INFORMATION info{};Handle file(Open(path,false,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,info,error));
    if(file.value==INVALID_HANDLE_VALUE)return false;attributes=info.dwFileAttributes;if(metadata)*metadata=info;return true;
}
inline HANDLE OpenRead(const std::wstring& path,DWORD& error,DWORD share=FILE_SHARE_READ){
    BY_HANDLE_FILE_INFORMATION info{};return Open(path,true,share,info,error);
}
} // namespace coop_physical_save
