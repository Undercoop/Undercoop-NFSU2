// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#include "../src/CoopAnimationModule.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace mod=coop_animation_module;
static unsigned checks=0;
static void Check(bool okay,const char* name){++checks;if(!okay){std::printf("FAIL: %s (%lu)\n",name,GetLastError());std::exit(1);}}
#define CHECK(x) Check(bool(x),#x)
static mod::Snapshot Good(){
    mod::Snapshot s;s.base=0x10000000;s.headersReadable=s.targetsReadable=s.diskVerified=true;
    s.dos.e_magic=IMAGE_DOS_SIGNATURE;s.dos.e_lfanew=0xF8;
    s.nt.Signature=IMAGE_NT_SIGNATURE;s.nt.FileHeader.Machine=IMAGE_FILE_MACHINE_I386;
    s.nt.FileHeader.NumberOfSections=5;s.nt.FileHeader.TimeDateStamp=mod::kTimestamp;
    s.nt.FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER32);s.nt.FileHeader.Characteristics=0x2102;
    s.nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR32_MAGIC;s.nt.OptionalHeader.SizeOfImage=mod::kImageSize;s.nt.OptionalHeader.SizeOfHeaders=0x400;
    s.query=mod::kQuery;s.inlined=mod::kInline;s.resume=mod::kResume;s.exit=mod::kExit;
    for(auto& r:s.regions){r.owner=r.allocation=s.base;r.start=s.base+0x3000;r.bytes=0x1000;
        r.state=MEM_COMMIT;r.type=MEM_IMAGE;r.protect=PAGE_EXECUTE_READ;r.sectionFlags=IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ;}
    return s;
}
template<class F>static void Reject(F change){auto s=Good();change(s);CHECK(!mod::ValidateSnapshot(s));}
static void Facts(){
    CHECK(mod::ValidateSnapshot(Good()));
    Reject([](auto& s){s.base=0;});Reject([](auto& s){s.base=UINTPTR_MAX-0x1000;});
    Reject([](auto& s){s.diskVerified=false;});Reject([](auto& s){s.headersReadable=false;});Reject([](auto& s){s.targetsReadable=false;});
    Reject([](auto& s){s.dos.e_magic=0;});Reject([](auto& s){s.dos.e_lfanew=0x300;});
    Reject([](auto& s){s.nt.Signature=0;});Reject([](auto& s){s.nt.FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;});
    Reject([](auto& s){s.nt.FileHeader.NumberOfSections=6;});Reject([](auto& s){++s.nt.FileHeader.TimeDateStamp;});
    Reject([](auto& s){s.nt.FileHeader.SizeOfOptionalHeader=0;});Reject([](auto& s){s.nt.FileHeader.Characteristics=0;});
    Reject([](auto& s){s.nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;});
    Reject([](auto& s){++s.nt.OptionalHeader.SizeOfImage;});Reject([](auto& s){s.nt.OptionalHeader.SizeOfHeaders=0x600;});
    for(size_t i=0;i<mod::kQuery.size();++i)Reject([&](auto& s){s.query[i]^=1;});
    for(size_t i=0;i<mod::kInline.size();++i)Reject([&](auto& s){s.inlined[i]^=1;});
    for(size_t i=0;i<mod::kResume.size();++i)Reject([&](auto& s){s.resume[i]^=1;});
    for(size_t i=0;i<mod::kExit.size();++i)Reject([&](auto& s){s.exit[i]^=1;});
    for(size_t i=0;i<mod::kRvas.size();++i){
        Reject([&](auto& s){s.regions[i].owner+=0x1000;});Reject([&](auto& s){s.regions[i].allocation+=0x1000;});
        Reject([&](auto& s){s.regions[i].type=MEM_PRIVATE;});Reject([&](auto& s){s.regions[i].state=MEM_RESERVE;});
        Reject([&](auto& s){s.regions[i].protect=PAGE_READWRITE;});Reject([&](auto& s){s.regions[i].protect|=PAGE_GUARD;});
        Reject([&](auto& s){s.regions[i].protect=PAGE_NOACCESS;});
        Reject([&](auto& s){s.regions[i].sectionFlags=IMAGE_SCN_MEM_READ;});
        Reject([&](auto& s){s.regions[i].sectionFlags=IMAGE_SCN_MEM_EXECUTE;});
        Reject([&](auto& s){s.regions[i].bytes=1;});
        Reject([&](auto& s){s.regions[i].start=s.base+mod::kRvas[i]+1;});
        Reject([&](auto& s){s.regions[i].start=s.base+mod::kRvas[i];s.regions[i].bytes=mod::TargetBytes(i)-1;});
        auto s=Good();s.regions[i].start=s.base+mod::kRvas[i];s.regions[i].bytes=mod::TargetBytes(i);CHECK(mod::ValidateSnapshot(s));
        s.regions[i].protect=PAGE_EXECUTE_WRITECOPY;CHECK(mod::ValidateSnapshot(s));
    }
}
static void FileChecks(const wchar_t* artifact){
    std::unique_ptr<mod::File> pin;
    CHECK(!mod::OpenVerifiedFile(L"C:\\missing\\AnimationController.asi",pin));CHECK(!pin);
    CHECK(!mod::OpenVerifiedFile(L"relative\\AnimationController.asi",pin));
    if(!artifact)return;
    CHECK(mod::OpenVerifiedFile(artifact,pin));CHECK(pin&&pin->value!=INVALID_HANDLE_VALUE);
    auto writer=CreateFileW(artifact,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    CHECK(writer==INVALID_HANDLE_VALUE);if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
    CHECK(GetLastError()==ERROR_SHARING_VIOLATION);
    pin.reset();
    wchar_t temp[MAX_PATH]{},root[MAX_PATH]{};CHECK(GetTempPathW(MAX_PATH,temp)>0);
    std::swprintf(root,MAX_PATH,L"%sUndercoopAnimationIdentity-%lu-%llu",temp,GetCurrentProcessId(),GetTickCount64());
    CHECK(CreateDirectoryW(root,nullptr)!=FALSE);
    const std::wstring copy=std::wstring(root)+L"\\AnimationController.asi";
    CHECK(CopyFileW(artifact,copy.c_str(),TRUE)!=FALSE);CHECK(mod::OpenVerifiedFile(copy,pin));
    pin.reset();
    mod::File file(CreateFileW(copy.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr));
    CHECK(file.value!=INVALID_HANDLE_VALUE);DWORD n=0;const unsigned char changed=0;LARGE_INTEGER at{};at.QuadPart=0;
    CHECK(SetFilePointerEx(file.value,at,nullptr,FILE_BEGIN)!=FALSE);CHECK(WriteFile(file.value,&changed,1,&n,nullptr)&&n==1);
    CloseHandle(file.value);file.value=INVALID_HANDLE_VALUE;
    CHECK(!mod::OpenVerifiedFile(copy,pin));CHECK(!pin);
    CHECK(DeleteFileW(copy.c_str())!=FALSE);
    // Only this explicitly created, empty TEMP child is removed; never recurse.
    wchar_t resolved[MAX_PATH]{};CHECK(GetFullPathNameW(root,MAX_PATH,resolved,nullptr)>0);
    CHECK(std::wstring(resolved).compare(0,std::wcslen(temp),temp)==0);CHECK(RemoveDirectoryW(resolved)!=FALSE);
}
int wmain(int argc,wchar_t** argv){
    Facts();FileChecks(argc>1?argv[1]:nullptr);
    mod::Proof proof;CHECK(!mod::Verify(nullptr,proof));CHECK(!proof.module&&!proof.base&&!proof.file);
    CHECK(!mod::Verify(GetModuleHandleW(nullptr),proof));
    CHECK(!mod::Verify(reinterpret_cast<HMODULE>(uintptr_t(0x10000)),proof));
    std::printf("Animation module tests: %u checks, 0 failures; no plugin loaded or executed.\n",checks);return 0;
}
