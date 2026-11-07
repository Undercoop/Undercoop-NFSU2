// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Include at translation-unit scope, never inside the game adapter namespace.
// Only validates an already-read entry snapshot and reads the existing module.
// Does not call its code, load/unload it, patch it, or bypass its detour.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winver.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"version.lib")

namespace coop_unlimiter_ride {
constexpr uintptr_t kEntry=0x61BCD0;
constexpr uint32_t kTargetRva=0x4530,kTimestamp=0x649EAD1C,kImageSize=0x4A000,kFileSize=281088;
constexpr DWORD kVersionMS=0x00030001,kVersionLS=0x00010539; // 3.1.1.1337
constexpr std::array<uint8_t,29> kPrefix{{
    0x55,0x8B,0xEC,0x81,0xEC,0xE0,0,0,0,0x53,0x56,0x8B,0xF1,0x57,
    0x68,0xA8,0,0,0,0x6A,0x01,0x8B,0x3E,0x8D,0x86,0x38,0x08,0,0}};
constexpr std::array<uint8_t,32> kSha256{{
    0xDD,0xBA,0xAD,0xEB,0x70,0xE5,0x17,0xA4,0x93,0x83,0x5B,0x3A,0xA6,0x0B,0x0D,0x46,
    0xE8,0xDE,0xCE,0x30,0xFE,0x6B,0xED,0xA6,0xEF,0xA4,0xC7,0xFE,0xF5,0x53,0xC1,0x1B}};

struct Snapshot {
    uintptr_t address=0,moduleBase=0,owner=0,allocationBase=0,regionBase=0;
    size_t entryLength=0,regionSize=0;
    std::array<uint8_t,6> entry{};
    bool entryReadable=false,headersReadable=false,bodyReadable=false;
    bool diskVerified=false,pathVerified=false,versionVerified=false;
    DWORD state=0,protect=0,type=0,sectionFlags=0;
    WORD dosMagic=0,machine=0,optionalMagic=0,optionalBytes=0,sections=0,characteristics=0;
    DWORD ntSignature=0,timestamp=0,imageSize=0,headerSize=0;
    DWORD fileVersionMS=0,fileVersionLS=0,productVersionMS=0,productVersionLS=0;
    std::array<uint8_t,32> sha256{};
    std::array<uint8_t,29> body{};
};

inline bool Target(uintptr_t address,const std::array<uint8_t,6>& bytes,uintptr_t& target){
    if(address!=kEntry||bytes[0]!=0xE9||bytes[5]!=0)return false;
    int32_t relative=0;std::memcpy(&relative,bytes.data()+1,4);
    const int64_t result=int64_t(address)+5+relative;
    if(result<0x10000||uint64_t(result)>(std::numeric_limits<uintptr_t>::max)())return false;
    target=uintptr_t(result);return true;
}
// Pure validation: its facts are populated from bounded OS reads below. Useful
// for deterministic negative tests without mapping or calling game addresses.
inline bool ValidateSnapshot(const Snapshot& s){
    uintptr_t target=0;
    if(!s.entryReadable||s.entryLength<6||!Target(s.address,s.entry,target)||s.moduleBase<0x10000||
        s.moduleBase>(std::numeric_limits<uintptr_t>::max)()-kImageSize||target!=s.moduleBase+kTargetRva||
        s.owner!=s.moduleBase||s.allocationBase!=s.moduleBase)return false;
    const DWORD executable=PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY;
    if(s.state!=MEM_COMMIT||s.type!=MEM_IMAGE||(s.protect&(PAGE_GUARD|PAGE_NOACCESS))||
        !(s.protect&executable)||target<s.regionBase||s.regionSize<kPrefix.size()||
        target-s.regionBase>s.regionSize-kPrefix.size()||
        !(s.sectionFlags&IMAGE_SCN_MEM_EXECUTE)||!(s.sectionFlags&IMAGE_SCN_MEM_READ))return false;
    if(!s.headersReadable||s.dosMagic!=IMAGE_DOS_SIGNATURE||s.ntSignature!=IMAGE_NT_SIGNATURE||
        s.machine!=IMAGE_FILE_MACHINE_I386||s.optionalMagic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||
        s.optionalBytes!=sizeof(IMAGE_OPTIONAL_HEADER32)||s.sections!=5||s.characteristics!=0x2102||
        s.timestamp!=kTimestamp||s.imageSize!=kImageSize||s.headerSize!=0x400)return false;
    return s.bodyReadable&&s.body==kPrefix&&s.pathVerified&&s.diskVerified&&s.sha256==kSha256&&
        s.versionVerified&&s.fileVersionMS==kVersionMS&&s.fileVersionLS==kVersionLS&&
        s.productVersionMS==kVersionMS&&s.productVersionLS==kVersionLS;
}

namespace detail {
struct ErrorScope {DWORD value=GetLastError();~ErrorScope(){SetLastError(value);}};
struct File {HANDLE value=INVALID_HANDLE_VALUE;~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
inline bool Read(uintptr_t address,void* output,size_t bytes){
    if(!output||!bytes||bytes>4096||address<0x10000||
        address>(std::numeric_limits<uintptr_t>::max)()-bytes)return false;
    SIZE_T copied=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),output,bytes,&copied)!=FALSE&&copied==bytes;
}
inline bool SameFile(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b){
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow&&
        a.nFileSizeHigh==b.nFileSizeHigh&&a.nFileSizeLow==b.nFileSizeLow&&
        a.ftCreationTime.dwHighDateTime==b.ftCreationTime.dwHighDateTime&&a.ftCreationTime.dwLowDateTime==b.ftCreationTime.dwLowDateTime&&
        a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime&&a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime;
}
inline std::wstring NormalPath(std::wstring path){if(path.compare(0,4,L"\\\\?\\")==0)path.erase(0,4);return path;}
inline bool SamePath(const std::wstring& a,const std::wstring& b){
    return a.size()<=32767&&b.size()<=32767&&CompareStringOrdinal(a.data(),int(a.size()),b.data(),int(b.size()),TRUE)==CSTR_EQUAL;
}
struct Proof {
    bool valid=false;uintptr_t module=0;std::wstring path;BY_HANDLE_FILE_INFORMATION file{};
    DWORD fileMS=0,fileLS=0,productMS=0,productLS=0;
};
struct Cache {SRWLOCK lock=SRWLOCK_INIT;Proof proof;size_t hashes=0;};
inline Cache& DiskCache(){static Cache cache;return cache;}
struct CacheScope {Cache& value;explicit CacheScope(Cache& c):value(c){AcquireSRWLockExclusive(&value.lock);}~CacheScope(){ReleaseSRWLockExclusive(&value.lock);}};
inline bool Hash(HANDLE file,std::array<uint8_t,32>& output){
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    bool okay=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if(okay)okay=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    std::array<uint8_t,32768> block{};uint64_t total=0;
    while(okay){DWORD got=0;if(!ReadFile(file,block.data(),DWORD(block.size()),&got,nullptr)){okay=false;break;}
        if(!got)break;total+=got;if(total>kFileSize){okay=false;break;}
        okay=BCryptHashData(hash,block.data(),got,0)>=0;
    }
    okay=okay&&total==kFileSize&&BCryptFinishHash(hash,output.data(),ULONG(output.size()),0)>=0;
    if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);return okay;
}
inline bool Version(const std::wstring& path,Proof& proof){
    DWORD unused=0;const DWORD bytes=GetFileVersionInfoSizeW(path.c_str(),&unused);
    if(!bytes||bytes>1024*1024)return false;
    std::vector<uint8_t> data(bytes);VS_FIXEDFILEINFO* value=nullptr;UINT length=0;
    if(!GetFileVersionInfoW(path.c_str(),0,bytes,data.data())||
        !VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&value),&length)||
        !value||length<sizeof(*value)||value->dwSignature!=0xFEEF04BD)return false;
    proof.fileMS=value->dwFileVersionMS;proof.fileLS=value->dwFileVersionLS;
    proof.productMS=value->dwProductVersionMS;proof.productLS=value->dwProductVersionLS;
    return proof.fileMS==kVersionMS&&proof.fileLS==kVersionLS&&proof.productMS==kVersionMS&&proof.productLS==kVersionLS;
}
inline bool Disk(uintptr_t module,const std::wstring& modulePath,Snapshot& snapshot){
    File file;file.value=CreateFileW(modulePath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    BY_HANDLE_FILE_INFORMATION before{},after{};
    if(file.value==INVALID_HANDLE_VALUE||GetFileType(file.value)!=FILE_TYPE_DISK||!GetFileInformationByHandle(file.value,&before)||
        (before.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||before.nFileSizeHigh||before.nFileSizeLow!=kFileSize)return false;
    std::array<wchar_t,MAX_PATH+4> final{};
    const DWORD count=GetFinalPathNameByHandleW(file.value,final.data(),DWORD(final.size()),FILE_NAME_NORMALIZED);
    if(!count||count>=final.size()||!SamePath(NormalPath(std::wstring(final.data(),count)),NormalPath(modulePath)))return false;
    snapshot.pathVerified=true;
    Cache& cache=DiskCache();CacheScope locked(cache);
    if(!cache.proof.valid||cache.proof.module!=module||!SamePath(cache.proof.path,modulePath)||!SameFile(cache.proof.file,before)){
        cache.proof={};Proof next;next.module=module;next.path=modulePath;std::array<uint8_t,32> digest{};++cache.hashes;
        if(!Hash(file.value,digest)||digest!=kSha256||!Version(modulePath,next)||
            !GetFileInformationByHandle(file.value,&after)||!SameFile(before,after))return false;
        next.file=after;next.valid=true;cache.proof=std::move(next);
    }
    const auto& proof=cache.proof;snapshot.sha256=kSha256;snapshot.diskVerified=true;snapshot.versionVerified=true;
    snapshot.fileVersionMS=proof.fileMS;snapshot.fileVersionLS=proof.fileLS;
    snapshot.productVersionMS=proof.productMS;snapshot.productVersionLS=proof.productLS;return true;
}
inline bool Headers(uintptr_t module,Snapshot& snapshot){
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS32 nt{};
    if(!Read(module,&dos,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<sizeof(dos)||dos.e_lfanew>0x1000||
        module>(std::numeric_limits<uintptr_t>::max)()-uintptr_t(dos.e_lfanew)-sizeof(nt)||
        !Read(module+uintptr_t(dos.e_lfanew),&nt,sizeof(nt)))return false;
    snapshot.dosMagic=dos.e_magic;snapshot.ntSignature=nt.Signature;snapshot.machine=nt.FileHeader.Machine;
    snapshot.sections=nt.FileHeader.NumberOfSections;snapshot.optionalBytes=nt.FileHeader.SizeOfOptionalHeader;
    snapshot.characteristics=nt.FileHeader.Characteristics;snapshot.optionalMagic=nt.OptionalHeader.Magic;
    snapshot.timestamp=nt.FileHeader.TimeDateStamp;snapshot.imageSize=nt.OptionalHeader.SizeOfImage;snapshot.headerSize=nt.OptionalHeader.SizeOfHeaders;
    if(nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.NumberOfSections!=5||nt.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER32)||
        nt.OptionalHeader.SizeOfHeaders!=0x400||nt.OptionalHeader.SizeOfImage!=kImageSize)return false;
    const size_t sectionOffset=size_t(dos.e_lfanew)+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+sizeof(IMAGE_OPTIONAL_HEADER32);
    if(sectionOffset>0x400||5*sizeof(IMAGE_SECTION_HEADER)>0x400-sectionOffset)return false;
    std::array<IMAGE_SECTION_HEADER,5> sections{};
    if(!Read(module+sectionOffset,sections.data(),sizeof(sections)))return false;
    unsigned matches=0;
    for(const auto& section:sections){
        const uint32_t start=section.VirtualAddress,bytes=section.Misc.VirtualSize;
        if(start>kImageSize||bytes>kImageSize-start)return false;
        if(kTargetRva>=start&&bytes>=kPrefix.size()&&kTargetRva-start<=bytes-kPrefix.size()){
            ++matches;snapshot.sectionFlags=section.Characteristics;
        }
    }
    snapshot.headersReadable=matches==1;return snapshot.headersReadable;
}
} // namespace detail

// actualBytes must be the caller's fresh read of address (the existing native
// signature probe). This function safely copies only six bytes from that
// snapshot, then independently validates the current loaded target and disk.
inline bool Approved(uintptr_t address,const uint8_t* actualBytes,size_t length){
    detail::ErrorScope saved;
    try{
        Snapshot s;s.address=address;s.entryLength=length;
        if(length<6||!detail::Read(reinterpret_cast<uintptr_t>(actualBytes),s.entry.data(),s.entry.size()))return false;
        s.entryReadable=true;uintptr_t target=0;if(!Target(address,s.entry,target))return false;
        HMODULE owner=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(target),&owner))return false;
        s.moduleBase=s.owner=reinterpret_cast<uintptr_t>(owner);
        if(s.moduleBase>(std::numeric_limits<uintptr_t>::max)()-kImageSize||target!=s.moduleBase+kTargetRva)return false;
        MEMORY_BASIC_INFORMATION region{};if(!VirtualQuery(reinterpret_cast<const void*>(target),&region,sizeof(region)))return false;
        s.allocationBase=reinterpret_cast<uintptr_t>(region.AllocationBase);s.regionBase=reinterpret_cast<uintptr_t>(region.BaseAddress);
        s.regionSize=region.RegionSize;s.state=region.State;s.type=region.Type;s.protect=region.Protect;
        if(!detail::Headers(s.moduleBase,s))return false;
        s.bodyReadable=detail::Read(target,s.body.data(),s.body.size());
        if(!s.bodyReadable||s.body!=kPrefix)return false;
        std::array<wchar_t,MAX_PATH> path{};const DWORD count=GetModuleFileNameW(owner,path.data(),DWORD(path.size()));
        if(!count||count>=path.size()||!detail::Disk(s.moduleBase,std::wstring(path.data(),count),s))return false;
        return ValidateSnapshot(s);
    }catch(...){return false;}
}
} // namespace coop_unlimiter_ride
