// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#pragma comment(lib,"bcrypt.lib")

// This verifier never loads or calls the third-party plugin. The constants
// describe the released 1.2.1 artifact; they are not an arbitrary mod allowlist.
namespace coop_animation_module {
constexpr DWORD kTimestamp=0x5F6521BA,kImageSize=0x42000,kFileSize=252416;
constexpr std::array<uint32_t,4> kRvas{{0x36A0,0x370C,0x3738,0x37F0}};
constexpr std::array<uint8_t,5> kQuery{{0xA1,0x9C,0x7C,0x82,0x00}};
constexpr std::array<uint8_t,8> kInline{{0xA1,0x9C,0x7C,0x82,0x00,0x83,0xC0,0x08}};
// The scalar operand following kResume is ASLR-relocated. Only its fixed
// instruction prefix is compared; ownership and disk identity are checked too.
constexpr std::array<uint8_t,3> kResume{{0xF3,0x0F,0x10}};
constexpr std::array<uint8_t,5> kExit{{0x5E,0x8B,0xE5,0x5D,0xC3}};
constexpr size_t TargetBytes(size_t i){return i==0?kQuery.size():i==1?kInline.size():i==2?kResume.size():kExit.size();}
constexpr std::array<uint8_t,32> kHash{{
    0x2C,0xCB,0xCA,0xA1,0xCE,0xD0,0x37,0xC2,0xF0,0xCE,0x27,0x89,0xB2,0xE8,0x8B,0x3B,
    0xB7,0x58,0xD9,0xB2,0x96,0x32,0xE6,0x1E,0x36,0xE5,0x70,0x96,0x08,0x0A,0x56,0x50}};
struct File {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit File(HANDLE handle=INVALID_HANDLE_VALUE):value(handle){}
    ~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    File(const File&)=delete;File& operator=(const File&)=delete;
};
struct Proof { HMODULE module=nullptr;uintptr_t base=0;std::unique_ptr<File> file; };
struct Region {
    uintptr_t owner=0,allocation=0,start=0;
    size_t bytes=0;
    DWORD state=0,type=0,protect=0,sectionFlags=0;
};
struct Snapshot {
    uintptr_t base=0;
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS32 nt{};
    std::array<Region,4> regions{};
    std::array<uint8_t,5> query{};std::array<uint8_t,8> inlined{};
    std::array<uint8_t,3> resume{};std::array<uint8_t,5> exit{};
    bool diskVerified=false,headersReadable=false,targetsReadable=false;
};
inline bool ValidateSnapshot(const Snapshot& s){
    if(s.base<0x10000||s.base>(std::numeric_limits<uintptr_t>::max)()-kImageSize||
        !s.headersReadable||!s.targetsReadable||!s.diskVerified||
        s.dos.e_magic!=IMAGE_DOS_SIGNATURE||s.dos.e_lfanew<sizeof(IMAGE_DOS_HEADER)||s.dos.e_lfanew>0x200||
        s.nt.Signature!=IMAGE_NT_SIGNATURE||s.nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||
        s.nt.FileHeader.NumberOfSections!=5||s.nt.FileHeader.TimeDateStamp!=kTimestamp||
        s.nt.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER32)||s.nt.FileHeader.Characteristics!=0x2102||
        s.nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||
        s.nt.OptionalHeader.SizeOfImage!=kImageSize||s.nt.OptionalHeader.SizeOfHeaders!=0x400||
        s.query!=kQuery||s.inlined!=kInline||s.resume!=kResume||s.exit!=kExit)return false;
    for(size_t i=0;i<s.regions.size();++i){
        const auto& r=s.regions[i];const uintptr_t target=s.base+kRvas[i];const size_t length=TargetBytes(i);
        const DWORD protection=r.protect&0xFF;
        if(r.owner!=s.base||r.allocation!=s.base||r.state!=MEM_COMMIT||r.type!=MEM_IMAGE||
            (r.protect&(PAGE_GUARD|PAGE_NOACCESS))||
            (protection!=PAGE_EXECUTE_READ&&protection!=PAGE_EXECUTE_READWRITE&&protection!=PAGE_EXECUTE_WRITECOPY)||
            !(r.sectionFlags&IMAGE_SCN_MEM_EXECUTE)||!(r.sectionFlags&IMAGE_SCN_MEM_READ)||
            target<r.start||r.bytes<length||target-r.start>r.bytes-length)return false;
    }
    return true;
}
namespace detail {
struct LastErrorScope{DWORD value=GetLastError();~LastErrorScope(){SetLastError(value);}};
inline bool Read(uintptr_t at,void* out,size_t bytes){
    if(!out||!bytes||bytes>4096||at<0x10000||at>(std::numeric_limits<uintptr_t>::max)()-bytes)return false;
    SIZE_T copied=0;return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(at),out,bytes,&copied)&&copied==bytes;
}
inline std::wstring Normal(std::wstring path){if(path.compare(0,4,L"\\\\?\\")==0)path.erase(0,4);return path;}
inline bool Equal(const std::wstring& a,const std::wstring& b){
    return a.size()<=32767&&b.size()<=32767&&CompareStringOrdinal(a.data(),int(a.size()),b.data(),int(b.size()),TRUE)==CSTR_EQUAL;
}
inline bool RegularPath(const std::wstring& path){
    if(path.size()<4||path.size()>=MAX_PATH||path[1]!=L':'||path[2]!=L'\\')return false;
    for(size_t end=3;end<=path.size();++end){
        if(end<path.size()&&path[end]!=L'\\')continue;
        const DWORD attrs=GetFileAttributesW(path.substr(0,end).c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES||(attrs&FILE_ATTRIBUTE_REPARSE_POINT)||
            (end<path.size()&&!(attrs&FILE_ATTRIBUTE_DIRECTORY))||
            (end==path.size()&&(attrs&FILE_ATTRIBUTE_DIRECTORY)))return false;
    }
    const size_t slash=path.find_last_of(L'\\');
    return Equal(path.substr(slash+1),L"AnimationController.asi");
}
inline bool Hash(HANDLE file){
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    bool good=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if(good)good=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    std::array<uint8_t,32768> bytes{};uint64_t total=0;
    while(good){DWORD read=0;if(!ReadFile(file,bytes.data(),DWORD(bytes.size()),&read,nullptr)){good=false;break;}
        if(!read)break;total+=read;if(total>kFileSize){good=false;break;}
        good=BCryptHashData(hash,bytes.data(),read,0)>=0;
    }
    std::array<uint8_t,32> digest{};
    good=good&&total==kFileSize&&BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0)>=0&&digest==kHash;
    if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);return good;
}
inline bool SameFile(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b){
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow&&
        a.nFileSizeHigh==b.nFileSizeHigh&&a.nFileSizeLow==b.nFileSizeLow&&
        a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime&&a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime;
}
} // namespace detail
// Also usable for offline artifact verification; it does not map or execute it.
inline bool OpenVerifiedFile(const std::wstring& path,std::unique_ptr<File>& output){
    detail::LastErrorScope saved;output.reset();
    try{
        if(!detail::RegularPath(path))return false;
        auto file=std::make_unique<File>(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
        BY_HANDLE_FILE_INFORMATION before{},after{};std::array<wchar_t,MAX_PATH+4> final{};
        if(file->value==INVALID_HANDLE_VALUE||GetFileType(file->value)!=FILE_TYPE_DISK||
            !GetFileInformationByHandle(file->value,&before)||
            (before.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||before.nFileSizeHigh||before.nFileSizeLow!=kFileSize)return false;
        const DWORD length=GetFinalPathNameByHandleW(file->value,final.data(),DWORD(final.size()),FILE_NAME_NORMALIZED);
        if(!length||length>=final.size()||!detail::Equal(detail::Normal(std::wstring(final.data(),length)),detail::Normal(path))||
            !detail::Hash(file->value)||!GetFileInformationByHandle(file->value,&after)||!detail::SameFile(before,after))return false;
        output=std::move(file);return true;
    }catch(...){return false;}
}
inline bool Verify(HMODULE module,Proof& output){
    detail::LastErrorScope saved;output=Proof{};
    if(!module)return false;
    try{
        Snapshot s;s.base=reinterpret_cast<uintptr_t>(module);
        if(s.base<0x10000||s.base>(std::numeric_limits<uintptr_t>::max)()-kImageSize||
            !detail::Read(s.base,&s.dos,sizeof(s.dos))||s.dos.e_magic!=IMAGE_DOS_SIGNATURE||
            s.dos.e_lfanew<sizeof(IMAGE_DOS_HEADER)||s.dos.e_lfanew>0x200||
            !detail::Read(s.base+uintptr_t(s.dos.e_lfanew),&s.nt,sizeof(s.nt)))return false;
        const size_t offset=size_t(s.dos.e_lfanew)+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+sizeof(IMAGE_OPTIONAL_HEADER32);
        std::array<IMAGE_SECTION_HEADER,5> sections{};
        if(offset>0x400||sizeof(sections)>0x400-offset||!detail::Read(s.base+offset,sections.data(),sizeof(sections)))return false;
        for(size_t i=0;i<s.regions.size();++i){
            auto& r=s.regions[i];const uintptr_t at=s.base+kRvas[i];const size_t bytes=TargetBytes(i);
            HMODULE owner=nullptr;MEMORY_BASIC_INFORMATION memory{};
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(at),&owner)||owner!=module||!VirtualQuery(reinterpret_cast<void*>(at),&memory,sizeof(memory)))return false;
            r.owner=reinterpret_cast<uintptr_t>(owner);r.allocation=reinterpret_cast<uintptr_t>(memory.AllocationBase);
            r.start=reinterpret_cast<uintptr_t>(memory.BaseAddress);r.bytes=memory.RegionSize;
            r.state=memory.State;r.type=memory.Type;r.protect=memory.Protect;
            unsigned matches=0;
            for(const auto& section:sections){
                const auto start=section.VirtualAddress,length=section.Misc.VirtualSize;
                if(start>kImageSize||length>kImageSize-start)return false;
                if(kRvas[i]>=start&&length>=bytes&&kRvas[i]-start<=length-bytes){++matches;r.sectionFlags=section.Characteristics;}
            }
            if(matches!=1)return false;
        }
        s.headersReadable=true;
        s.targetsReadable=detail::Read(s.base+kRvas[0],s.query.data(),s.query.size())&&
            detail::Read(s.base+kRvas[1],s.inlined.data(),s.inlined.size())&&
            detail::Read(s.base+kRvas[2],s.resume.data(),s.resume.size())&&
            detail::Read(s.base+kRvas[3],s.exit.data(),s.exit.size());
        std::array<wchar_t,MAX_PATH> path{};const DWORD length=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
        Proof proof;if(!length||length>=path.size()||!OpenVerifiedFile(std::wstring(path.data(),length),proof.file))return false;
        s.diskVerified=true;if(!ValidateSnapshot(s))return false;
        proof.module=module;proof.base=s.base;output=std::move(proof);return true;
    }catch(...){return false;}
}
} // namespace coop_animation_module
