// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <windows.h>
#include <algorithm>
#include <cwchar>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>
#include "CoopCareerProgress.h"
#include "CoopSpecialReceipt.h"

namespace coop_backup {
enum class Role:uint8_t {Host=1,Client=2};
enum class Action:uint8_t {CareerSave=1,RaceResult,SharedProgress,CareerStart,SpecialEvent,BeforeRestore};
struct Record {
    std::wstring path,campaign,profile;
    uint64_t utc=0,session=0;
    uint32_t ticket=0;
    Role role=Role::Host;
    Action action=Action::CareerSave;
};
struct Entry {std::wstring suffix;std::vector<uint8_t> bytes;};
struct Snapshot {Record record;std::vector<Entry> files;};

inline uint64_t Now() {
    FILETIME time{};GetSystemTimeAsFileTime(&time);
    return uint64_t(time.dwLowDateTime)|(uint64_t(time.dwHighDateTime)<<32);
}
inline std::wstring Directory(const std::wstring& base,Role role) {
    std::wstring root;
    if(!coop_save::CanonicalWindowsPath(base,root) || (role!=Role::Host && role!=Role::Client))return {};
    return root+(role==Role::Host?L"\\Backups\\Host":L"\\Backups\\Client");
}
namespace detail {
constexpr size_t kMaximumBytes=64u*1024u*1024u,kMaximumEntries=512,kSuffixLength=72;
constexpr wchar_t kExtension[]=L".ucbackup",kArchivePrefix[]=L".coop-special-receipt.";
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle):value(handle){}
    ~Handle(){if(*this)CloseHandle(value);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    explicit operator bool()const{return value!=INVALID_HANDLE_VALUE;}
};
inline bool ExactHandle(HANDLE handle,const std::wstring& path,bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};wchar_t finalName[MAX_PATH+4]{};
    const DWORD count=GetFinalPathNameByHandleW(handle,finalName,DWORD(std::size(finalName)),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    std::wstring finalPath;
    return GetFileInformationByHandle(handle,&info) && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) &&
        !!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory && count && count<std::size(finalName) &&
        coop_save::CanonicalWindowsPath(finalName,finalPath) && coop_save::EqualPathText(path,finalPath);
}
inline HANDLE Open(const std::wstring& path,bool directory) {
    if(path.empty() || path.size()>=MAX_PATH)return INVALID_HANDLE_VALUE;
    const HANDLE handle=CreateFileW(path.c_str(),directory?FILE_LIST_DIRECTORY:GENERIC_READ,
        FILE_SHARE_READ|(directory?FILE_SHARE_WRITE:0),nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr);
    if(handle==INVALID_HANDLE_VALUE || ExactHandle(handle,path,directory))return handle;
    CloseHandle(handle);return INVALID_HANDLE_VALUE;
}
inline bool ExactRead(HANDLE file,void* output,DWORD size) {
    DWORD read=0;return ReadFile(file,output,size,&read,nullptr) && read==size;
}
inline bool ReadBytes(const std::wstring& path,size_t maximum,std::vector<uint8_t>& bytes) {
    const Handle file(Open(path,false));LARGE_INTEGER size{};
    if(!file || !GetFileSizeEx(file.value,&size) || size.QuadPart<=0 || uint64_t(size.QuadPart)>maximum)return false;
    bytes.resize(size_t(size.QuadPart));return ExactRead(file.value,bytes.data(),DWORD(bytes.size()));
}
inline bool EnsureDirectory(const std::wstring& path) {
    return CreateDirectoryW(path.c_str(),nullptr) || GetLastError()==ERROR_ALREADY_EXISTS;
}
inline bool Tail(const std::wstring& name,const wchar_t* suffix) {
    const size_t size=std::wcslen(suffix);
    return name.size()>=size && coop_save::EqualPathText(name.substr(name.size()-size),suffix);
}
inline std::wstring ArchiveSuffix(const coop_special_receipt::Scope& scope) {
    wchar_t suffix[kSuffixLength]{};
    std::swprintf(suffix,std::size(suffix),L".coop-special-receipt.%016llX-%08X-%08X.archive",
        static_cast<unsigned long long>(scope.session),scope.ticket,scope.operation);
    return suffix;
}
inline bool IsSpecialArchive(const std::wstring& suffix) {
    constexpr size_t prefix=std::size(kArchivePrefix)-1;
    return suffix.size()==prefix+16+1+8+1+8+8 &&
        coop_save::EqualPathText(suffix.substr(0,prefix),kArchivePrefix) && Tail(suffix,L".archive");
}
inline std::wstring Wide(const std::string& name) {
    if(name.empty() || name.size()>31)return {};
    wchar_t wide[32]{};const int size=MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,name.data(),int(name.size()),wide,32);
    return size>0?std::wstring(wide,size):std::wstring{};
}
inline bool NativeName(const Entry& entry,const std::wstring& profile) {
    if(coop_save::ValidateNativeProfileFile(entry.bytes.data(),entry.bytes.size())!=coop_save::SaveValidation::Valid)return false;
    const auto* start=reinterpret_cast<const char*>(entry.bytes.data()+coop_special_receipt::kPersonalNameOffset);
    const auto* end=std::find(start,start+coop_special_receipt::kNameBytes,'\0');
    return end!=start+coop_special_receipt::kNameBytes && coop_save::EqualPathText(Wide(std::string(start,end)),profile);
}
inline bool RecordValid(const Record& record) {
    return record.utc && coop_save::IsSafeProfileName(record.profile) && coop_save::IsSafeProfileName(record.campaign) &&
        (record.role==Role::Host || record.role==Role::Client) &&
        record.action>=Action::CareerSave && record.action<=Action::BeforeRestore;
}
inline bool Validate(const Snapshot& snapshot) {
    const auto& record=snapshot.record;
    const auto& files=snapshot.files;
    if(!RecordValid(record) || files.empty() || files.size()>kMaximumEntries ||
        !files[0].suffix.empty() || !NativeName(files[0],record.profile))return false;
    size_t total=0;std::vector<std::wstring> suffixes;
    for(size_t index=0;index<files.size();++index) {
        const auto& file=files[index];
        if(file.bytes.empty() || file.bytes.size()>kMaximumBytes-total)return false;
        total+=file.bytes.size();
        if(!index)continue;
        for(const auto& known:suffixes)if(coop_save::EqualPathText(known,file.suffix))return false;
        suffixes.push_back(file.suffix);
        if(coop_save::EqualPathText(file.suffix,L".coop-progress")) {
            coop_career::Progress progress;
            if(!coop_career::Decode(file.bytes,progress))return false;
            continue;
        }
        coop_special_receipt::Transaction tx;
        if(!coop_special_receipt::Decode(file.bytes,tx) ||
            !coop_save::EqualPathText(Wide(tx.scope.personal),record.profile) ||
            !coop_save::EqualPathText(Wide(tx.scope.campaign),record.campaign))return false;
        if(coop_save::EqualPathText(file.suffix,L".coop-special-receipt")) {
            const auto recovered=coop_special_receipt::Recover(tx,tx.scope,files[0].bytes);
            if(recovered==coop_special_receipt::Recovery::Invalid || recovered==coop_special_receipt::Recovery::Conflict)return false;
        } else if(!coop_special_receipt::Terminal(tx.phase) || !coop_save::EqualPathText(file.suffix,ArchiveSuffix(tx.scope)))return false;
    }
    return true;
}
#pragma pack(push,1)
struct Header {
    uint32_t magic=0x4B424355u,version=1;
    uint32_t size=0,count=0;
    uint64_t utc=0,session=0;
    uint32_t ticket=0;
    Role role=Role::Host;
    Action action=Action::CareerSave;
    uint16_t reserved=0;
    wchar_t campaign[32]{},profile[32]{};
    uint32_t dataChecksum=0,headerChecksum=0;
};
struct EntryHeader {wchar_t suffix[kSuffixLength]{};uint32_t size=0;};
#pragma pack(pop)
template<size_t N> inline bool Text(const wchar_t (&source)[N],std::wstring& out) {
    const auto* end=std::find(source,source+N,L'\0');
    if(end==source+N || std::any_of(end,source+N,[](wchar_t c){return c!=0;}))return false;
    out.assign(source,end);return true;
}
inline uint32_t HeaderChecksum(const Header& header) {
    return coop_save::EaCrc32(reinterpret_cast<const uint8_t*>(&header),offsetof(Header,headerChecksum));
}
inline bool ReadHeader(HANDLE file,Header& header,Record& record) {
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file,&size) || size.QuadPart<=sizeof(Header) || uint64_t(size.QuadPart)>kMaximumBytes ||
        !ExactRead(file,&header,sizeof(header)) || header.magic!=Header{}.magic || header.version!=1 || header.reserved ||
        header.size!=size.QuadPart || !header.count || header.count>kMaximumEntries || header.headerChecksum!=HeaderChecksum(header) ||
        !Text(header.profile,record.profile) || !Text(header.campaign,record.campaign))return false;
    record.utc=header.utc;record.session=header.session;record.ticket=header.ticket;record.role=header.role;record.action=header.action;
    return RecordValid(record);
}
inline void Append(std::vector<uint8_t>& out,const void* data,size_t size) {
    const auto* bytes=static_cast<const uint8_t*>(data);out.insert(out.end(),bytes,bytes+size);
}
} // namespace detail

inline bool Read(const std::wstring& path,Snapshot& snapshot) {
    using namespace detail;
    if(!Tail(path,kExtension))return false;
    const Handle file(Open(path,false));Header header{};Snapshot candidate;candidate.record.path=path;
    if(!file || !ReadHeader(file.value,header,candidate.record))return false;
    std::vector<uint8_t> content(header.size-sizeof(header));
    if(!ExactRead(file.value,content.data(),DWORD(content.size())) ||
        coop_save::EaCrc32(content.data(),content.size())!=header.dataChecksum)return false;
    size_t offset=0;
    for(uint32_t index=0;index<header.count;++index) {
        EntryHeader item{};Entry entry;
        if(content.size()-offset<sizeof(item))return false;
        std::memcpy(&item,content.data()+offset,sizeof(item));offset+=sizeof(item);
        if(!item.size || item.size>content.size()-offset || !Text(item.suffix,entry.suffix))return false;
        entry.bytes.assign(content.data()+offset,content.data()+offset+item.size);offset+=item.size;
        candidate.files.push_back(std::move(entry));
    }
    if(offset!=content.size() || !Validate(candidate))return false;
    snapshot=std::move(candidate);return true;
}

inline std::vector<Record> List(const std::wstring& base,Role role) {
    using namespace detail;
    std::vector<Record> results;const auto dir=Directory(base,role);
    const Handle root(Open(dir,true));if(!root)return results;
    WIN32_FIND_DATAW item{};const HANDLE scan=FindFirstFileW((dir+L"\\*.ucbackup").c_str(),&item);
    if(scan==INVALID_HANDLE_VALUE)return results;
    do {
        if(item.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))continue;
        Record record;record.path=dir+L"\\"+item.cFileName;
        if(!Tail(record.path,kExtension))continue;
        const Handle file(Open(record.path,false));Header header{};
        if(file && ReadHeader(file.value,header,record) && record.role==role)results.push_back(std::move(record));
    }while(FindNextFileW(scan,&item));
    FindClose(scan);
    std::sort(results.begin(),results.end(),[](const Record& a,const Record& b){return a.utc!=b.utc?a.utc>b.utc:a.path>b.path;});
    return results;
}

inline bool Write(const std::wstring& base,const Snapshot& snapshot,std::wstring* published=nullptr) {
    using namespace detail;
    if(!Validate(snapshot))return false;
    const auto& record=snapshot.record;
    const auto dir=Directory(base,record.role);
    if(dir.empty())return false;
    std::wstring root;coop_save::CanonicalWindowsPath(base,root);
    const auto top=root+L"\\Backups";
    const Handle pinnedBase(Open(root,true));
    if(!pinnedBase || !EnsureDirectory(top))return false;
    const Handle pinnedTop(Open(top,true));
    if(!pinnedTop || !EnsureDirectory(dir))return false;
    const Handle pinnedRole(Open(dir,true));if(!pinnedRole)return false;
    std::vector<uint8_t> content;
    for(const auto& entry:snapshot.files) {
        EntryHeader item{};
        if(entry.suffix.size()>=std::size(item.suffix) || content.size()+sizeof(Header)+sizeof(item)+entry.bytes.size()>kMaximumBytes)return false;
        std::copy(entry.suffix.begin(),entry.suffix.end(),item.suffix);item.size=uint32_t(entry.bytes.size());
        Append(content,&item,sizeof(item));Append(content,entry.bytes.data(),entry.bytes.size());
    }
    Header header{};
    header.size=uint32_t(sizeof(header)+content.size());header.count=uint32_t(snapshot.files.size());
    header.utc=record.utc;header.session=record.session;header.ticket=record.ticket;header.role=record.role;header.action=record.action;
    std::copy(record.campaign.begin(),record.campaign.end(),header.campaign);std::copy(record.profile.begin(),record.profile.end(),header.profile);
    header.dataChecksum=coop_save::EaCrc32(content.data(),content.size());header.headerChecksum=HeaderChecksum(header);
    static LONG serial=0;
    const auto target=dir+L"\\"+std::to_wstring(record.utc)+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+
        std::to_wstring(InterlockedIncrement(&serial))+kExtension;
    const auto pending=target+L".pending";if(pending.size()>=MAX_PATH)return false;
    const Handle file(CreateFileW(pending.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file)return false;
    const auto write=[&](const void* data,DWORD size){DWORD written=0;return WriteFile(file.value,data,size,&written,nullptr) && written==size;};
    const bool flushed=ExactHandle(file.value,pending,false) && write(&header,sizeof(header)) &&
        write(content.data(),DWORD(content.size())) && FlushFileBuffers(file.value);
    if(flushed) {
        std::vector<uint8_t> bytes(sizeof(FILE_RENAME_INFO)+target.size()*sizeof(wchar_t));
        auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(bytes.data());
        rename->FileNameLength=DWORD(target.size()*sizeof(wchar_t));
        std::memcpy(rename->FileName,target.data(),rename->FileNameLength);
        if(SetFileInformationByHandle(file.value,FileRenameInfo,rename,DWORD(bytes.size()))) {
            if(published)*published=target;
            return true;
        }
    }
    FILE_DISPOSITION_INFO cleanup{TRUE};
    SetFileInformationByHandle(file.value,FileDispositionInfo,&cleanup,sizeof(cleanup));
    return false;
}

inline bool Capture(const std::wstring& roleRoot,Snapshot& snapshot) {
    using namespace detail;
    const auto& profile=snapshot.record.profile;
    if(!RecordValid(snapshot.record))return false;
    const auto dir=roleRoot+L"\\"+profile;
    const Handle pinnedRoot(Open(roleRoot,true)),pinnedProfile(Open(dir,true));
    if(!pinnedRoot || !pinnedProfile)return false;
    std::vector<Entry> files(1);
    if(!ReadBytes(dir+L"\\"+profile,coop_save::kNativeProfileFileSize,files[0].bytes))return false;
    size_t total=files[0].bytes.size();
    const auto prefix=dir+L"\\"+profile;
    WIN32_FIND_DATAW item{};const HANDLE scan=FindFirstFileW((prefix+L".*").c_str(),&item);
    if(scan!=INVALID_HANDLE_VALUE) {
        bool okay=true;
        do {
            const std::wstring name=item.cFileName;
            if(name.size()<=profile.size() || !coop_save::EqualPathText(name.substr(0,profile.size()),profile))continue;
            Entry entry;entry.suffix=name.substr(profile.size());size_t maximum=0;
            if(coop_save::EqualPathText(entry.suffix,L".coop-progress"))maximum=coop_career::kMaximumExtendedWireSize;
            else if(coop_save::EqualPathText(entry.suffix,L".coop-special-receipt") || IsSpecialArchive(entry.suffix))
                maximum=coop_special_receipt::kMaximumBytes;
            else continue;
            if(files.size()>=kMaximumEntries || (item.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !ReadBytes(dir+L"\\"+name,maximum,entry.bytes) || entry.bytes.size()>kMaximumBytes-total){okay=false;break;}
            if(snapshot.record.action==Action::CareerSave && coop_save::EqualPathText(entry.suffix,L".coop-special-receipt")) {
                coop_special_receipt::Transaction tx;
                if(coop_special_receipt::Decode(entry.bytes,tx) &&
                    (tx.phase==coop_special_receipt::Phase::Sealed || tx.phase==coop_special_receipt::Phase::CancelPending) &&
                    tx.after==files[0].bytes)snapshot.record.action=Action::SpecialEvent;
            }
            total+=entry.bytes.size();files.push_back(std::move(entry));
        }while(FindNextFileW(scan,&item));
        const DWORD end=GetLastError();FindClose(scan);
        if(!okay || end!=ERROR_NO_MORE_FILES)return false;
    } else if(GetLastError()!=ERROR_FILE_NOT_FOUND)return false;
    snapshot.files=std::move(files);return true;
}

inline uint32_t SharedFingerprint(const Snapshot& snapshot) {
    for(const auto& entry:snapshot.files)if(coop_save::EqualPathText(entry.suffix,L".coop-progress"))
        return coop_career::Digest(entry.bytes);
    return 0;
}
} // namespace coop_backup
