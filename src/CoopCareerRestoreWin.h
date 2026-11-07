// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopCareerBackupWin.h"

namespace coop_backup {
#pragma pack(push,1)
struct RestorePoint {
    uint32_t magic=0x31524255; // UBR1
    uint64_t utc=0,session=0;
    uint32_t ticket=0;
    Action action=Action::CareerSave;
};
#pragma pack(pop)
inline bool ValidPoint(const RestorePoint& point) {
    return point.magic==RestorePoint{}.magic && point.utc && point.session && point.ticket &&
        point.action>=Action::CareerSave && point.action<=Action::BeforeRestore;
}
inline bool SamePoint(const RestorePoint& a,const RestorePoint& b) {
    return ValidPoint(a) && ValidPoint(b) && a.utc==b.utc && a.session==b.session && a.ticket==b.ticket && a.action==b.action;
}
inline RestorePoint Point(const Record& record) {
    return {RestorePoint{}.magic,record.utc,record.session,record.ticket,record.action};
}

namespace restore_detail {
inline constexpr wchar_t kMarker[]=L".undercoop-restore-point";
inline std::wstring RoleRoot(const std::wstring& base,Role role) {
    return base+(role==Role::Host?L"\\Host":L"\\Client");
}
inline int DirectoryState(const std::wstring& path) {
    const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES) {
        const DWORD error=GetLastError();
        return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND?0:-1;
    }
    const detail::Handle dir(detail::Open(path,true));
    return dir?1:-1;
}
inline bool WriteNew(const std::wstring& path,const void* data,size_t size) {
    if(path.size()>=MAX_PATH || !size || size>detail::kMaximumBytes)return false;
    const detail::Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    DWORD written=0;
    return file && detail::ExactHandle(file.value,path,false) &&
        WriteFile(file.value,data,DWORD(size),&written,nullptr) && written==size && FlushFileBuffers(file.value);
}
inline std::wstring Unique(const std::wstring& directory,const wchar_t* prefix) {
    static LONG serial=0;
    return directory+L"\\"+prefix+std::to_wstring(Now())+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+
        std::to_wstring(InterlockedIncrement(&serial));
}
inline bool ReadTarget(const std::wstring& pending,std::wstring& profile) {
    std::vector<uint8_t> bytes;
    if(!detail::ReadBytes(pending+L"\\target",31*sizeof(wchar_t),bytes) || bytes.size()%sizeof(wchar_t))return false;
    profile.resize(bytes.size()/sizeof(wchar_t));
    std::memcpy(profile.data(),bytes.data(),bytes.size());
    return coop_save::IsSafeProfileName(profile) && profile.find(L'\0')==std::wstring::npos;
}
inline bool ReadPointFile(const std::wstring& path,RestorePoint& point) {
    std::vector<uint8_t> bytes;
    if(!detail::ReadBytes(path,sizeof(point),bytes) || bytes.size()!=sizeof(point))return false;
    std::memcpy(&point,bytes.data(),sizeof(point));
    return ValidPoint(point);
}
inline bool MatchesPending(const std::wstring& pending,const std::wstring& profile,const RestorePoint& point) {
    std::wstring name;RestorePoint selected;
    return ReadTarget(pending,name) && coop_save::EqualPathText(name,profile) &&
        ReadPointFile(pending+L"\\point",selected) && SamePoint(selected,point);
}
inline bool ArchivePending(const std::wstring& archive,const std::wstring& pending) {
    const auto retained=Unique(archive,L"restore-retained-");
    return MoveFileExW(pending.c_str(),retained.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
}
inline bool RecoverLocked(const std::wstring& archive,const std::wstring& roleRoot) {
    const auto pending=archive+L"\\restore-pending";
    const int state=DirectoryState(pending);
    if(state<=0)return state==0;
    std::wstring name;RestorePoint point;
    if(!ReadTarget(pending,name) || !ReadPointFile(pending+L"\\point",point))return false;
    const auto target=roleRoot+L"\\"+name;
    const auto prior=pending+L"\\previous";
    const int priorState=DirectoryState(prior);int targetState=DirectoryState(target);
    if(priorState<0 || targetState<0)return false;
    RestorePoint live;
    if(targetState==1 && ReadPointFile(target+L"\\"+kMarker,live) && SamePoint(point,live)) {
        if(!MoveFileExW(target.c_str(),(pending+L"\\cancelled").c_str(),MOVEFILE_WRITE_THROUGH))return false;
        targetState=0;
    }
    if(priorState==1 && targetState==0 && !MoveFileExW(prior.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))return false;
    return ArchivePending(archive,pending);
}
inline HANDLE Lock(const std::wstring& archive) {
    const auto path=archive+L"\\restore.lock";
    const HANDLE file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE || detail::ExactHandle(file,path,false))return file;
    CloseHandle(file);
    return INVALID_HANDLE_VALUE;
}
} // namespace restore_detail

inline bool RecoverRestore(const std::wstring& base,Role role) {
    using namespace restore_detail;
    const auto archive=Directory(base,role);
    if(archive.empty())return false;
    const int pending=DirectoryState(archive+L"\\restore-pending");
    if(pending<=0)return pending==0;
    const detail::Handle dir(detail::Open(archive,true));
    if(!dir)return false;
    const detail::Handle lock(Lock(archive));
    return lock && RecoverLocked(archive,RoleRoot(base,role));
}

inline bool ReadRestorePoint(const std::wstring& roleRoot,const std::wstring& profile,RestorePoint& point) {
    return coop_save::IsSafeProfileName(profile) &&
        restore_detail::ReadPointFile(roleRoot+L"\\"+profile+L"\\"+restore_detail::kMarker,point);
}

inline bool CompleteRestore(const std::wstring& base,Role role,const std::wstring& profile) {
    using namespace restore_detail;
    const auto archive=Directory(base,role),root=RoleRoot(base,role),pending=archive+L"\\restore-pending";
    RestorePoint point;
    const detail::Handle dir(detail::Open(archive,true));
    if(!dir || !ReadRestorePoint(root,profile,point))return false;
    const detail::Handle lock(Lock(archive));
    if(!lock || !MatchesPending(pending,profile,point) || !ArchivePending(archive,pending))return false;
    // A locked marker may remain after publication, but the journal is the
    // authoritative proof that a restore is still pending.
    DeleteFileW((root+L"\\"+profile+L"\\"+kMarker).c_str());
    return true;
}

inline bool PendingRestorePoint(const std::wstring& base,Role role,const std::wstring& profile,RestorePoint& point) {
    using namespace restore_detail;
    const auto archive=Directory(base,role);
    return !archive.empty() && ReadRestorePoint(RoleRoot(base,role),profile,point) &&
        MatchesPending(archive+L"\\restore-pending",profile,point);
}

inline bool RevertRestore(const std::wstring& base,Role role,const std::wstring& profile,const RestorePoint& point) {
    using namespace restore_detail;
    const auto archive=Directory(base,role);
    const detail::Handle dir(detail::Open(archive,true));
    if(!dir)return false;
    const detail::Handle lock(Lock(archive));
    return lock && MatchesPending(archive+L"\\restore-pending",profile,point) && RecoverLocked(archive,RoleRoot(base,role));
}

inline bool Restore(const std::wstring& base,const Snapshot& snapshot,const RestorePoint& point) {
    using namespace restore_detail;
    if(!ValidPoint(point) || !detail::Validate(snapshot))return false;
    const auto& record=snapshot.record;
    const auto archive=Directory(base,record.role);
    const auto roleRoot=RoleRoot(base,record.role);
    const detail::Handle archivePin(detail::Open(archive,true)), rolePin(detail::Open(roleRoot,true));
    if(!archivePin || !rolePin)return false;
    const detail::Handle lock(Lock(archive));
    if(!lock)return false;
    RestorePoint existing;
    if(PendingRestorePoint(base,record.role,record.profile,existing) && SamePoint(existing,point))return true;
    if(!RecoverLocked(archive,roleRoot))return false;
    const auto stage=Unique(archive,L"restore-stage-");
    const auto incoming=stage+L"\\incoming";
    const auto pending=archive+L"\\restore-pending";
    const auto target=roleRoot+L"\\"+record.profile;
    if(!CreateDirectoryW(stage.c_str(),nullptr) || !CreateDirectoryW(incoming.c_str(),nullptr) ||
        DirectoryState(incoming)!=1 ||
        !WriteNew(stage+L"\\target",record.profile.data(),record.profile.size()*sizeof(wchar_t)) ||
        !WriteNew(stage+L"\\point",&point,sizeof(point)))return false;
    for(const auto& file:snapshot.files) {
        if(!WriteNew(incoming+L"\\"+record.profile+file.suffix,file.bytes.data(),file.bytes.size()))return false;
    }
    if(!WriteNew(incoming+L"\\"+kMarker,&point,sizeof(point)) ||
        !MoveFileExW(stage.c_str(),pending.c_str(),MOVEFILE_WRITE_THROUGH))return false;
    const int targetState=DirectoryState(target);
    if(targetState<0 || (targetState==1 &&
        !MoveFileExW(target.c_str(),(pending+L"\\previous").c_str(),MOVEFILE_WRITE_THROUGH))) {
        RecoverLocked(archive,roleRoot);
        return false;
    }
    if(!MoveFileExW((pending+L"\\incoming").c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)) {
        RecoverLocked(archive,roleRoot);
        return false;
    }
    return true;
}
} // namespace coop_backup
