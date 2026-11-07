// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

inline std::wstring selectedCareerBackupPath;
inline std::wstring selectedCareerBackupRoot;

inline std::wstring CareerBackupRoot() {
    std::wstring folder, base;
    bool bound=false;
    if(!ReadCareerBinding(state.instanceId,folder,bound) ||
        (!bound && (!state.instanceId.empty() || !PreviousCareerDomain(folder))) || !careerBase(base))return {};
    return base+L"\\"+folder;
}

inline std::vector<coop_backup::Record> ListCareerBackups() {
    const auto root=CareerBackupRoot();
    if(root.empty())return {};
    return coop_backup::List(root,coop_backup::Role::Host);
}

inline bool SelectCareerBackup(const coop_backup::Record& record,std::wstring& message) {
    const auto root=CareerBackupRoot();
    const auto directory=coop_backup::Directory(root,coop_backup::Role::Host);
    const auto slash=record.path.find_last_of(L'\\');
    coop_backup::Snapshot snapshot;
    if(root.empty() || slash==std::wstring::npos || !Equal(record.path.substr(0,slash),directory) ||
        !coop_backup::Read(record.path,snapshot) || snapshot.record.role!=coop_backup::Role::Host ||
        snapshot.record.utc!=record.utc || !Equal(snapshot.record.profile,record.profile)) {
        message=L"This backup is unavailable or could not be verified.";
        return false;
    }
    if(!snapshot.record.session || !snapshot.record.ticket) {
        message=L"This backup was saved outside a paired career ticket.";
        return false;
    }
    selectedCareerBackupPath=record.path;
    selectedCareerBackupRoot=root;
    message=L"Backup selected. Host a CO-OP lobby and choose Start Career.";
    return true;
}
