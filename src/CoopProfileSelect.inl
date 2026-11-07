// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Included after the strict native file reader and atomic role-local writer.
// Selection is main-thread only. No imported profile is deserialized here.
namespace CoopProfileSelect {
enum class Result {Missing,Loaded,Invalid};
// Game-thread diagnostics: distinguish a path/Windows rejection from damaged
// native data without changing which profiles are allowed or importing one.
struct Diagnostic {
    const char* stage="none";DWORD error=ERROR_SUCCESS;
    std::wstring path;
};
static Diagnostic diagnostic;
static Diagnostic primaryDiagnostic;
static std::wstring primaryRoot;
static bool usedFallback=false;
static Result Reject(Result result,const char* stage,DWORD error=ERROR_SUCCESS){
    diagnostic.stage=stage;diagnostic.error=error;return result;
}
static bool RetryableMenuRead() {
    // A busy native writer can finish on a subsequent frame. Damaged data,
    // mismatched folder/internal names, unsafe paths or missing permissions
    // cannot be repaired by repeatedly swallowing Start the Career forever.
    const DWORD error=diagnostic.error;
    return error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION ||
        error==ERROR_BUSY || error==ERROR_NOT_READY || error==ERROR_IO_PENDING ||
        error==ERROR_RETRY || error==ERROR_USER_MAPPED_FILE;
}
static bool SafeName(const std::string& name){
    return name.size()<32&&coop_checkpoint::SafeName(name.c_str())&&
        coop_save::IsSafeProfileName(WideText(name.c_str()));
}
// Inspect the actual opened filesystem object. This preserves reparse checks
// for EVERY ancestor, independently of a legacy/shimmed path-attribute query.
// Access=0 is metadata-only; payload reads retain their strict sharing/CRC
// checks below. OPEN_REPARSE_POINT inspects a link instead of following it.
static bool Attributes(const std::wstring& path,DWORD& attributes,DWORD& error){
    const HANDLE handle=CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE){error=GetLastError();return false;}
    BY_HANDLE_FILE_INFORMATION info{};
    const bool disk=GetFileType(handle)==FILE_TYPE_DISK;
    const bool okay=disk&&GetFileInformationByHandle(handle,&info)!=FALSE;
    error=okay?ERROR_SUCCESS:disk?GetLastError():ERROR_NOT_SUPPORTED;
    CloseHandle(handle);
    if(!okay)return false;
    attributes=info.dwFileAttributes;return true;
}
static Result Read(const std::wstring& root,const std::string& name,std::vector<uint8_t>& file){
    file.clear();
    diagnostic={};
    if(!SafeName(name))return Reject(Result::Invalid,"profile-name",ERROR_INVALID_NAME);
    std::wstring canonical;
    const auto path=root+L"\\"+WideText(name.c_str())+L"\\"+WideText(name.c_str());
    diagnostic.path=path;
    if(!coop_save::CanonicalWindowsPath(path,canonical)||!coop_save::PathIsWithin(canonical,gCoopBaseRoot))
        return Reject(Result::Invalid,"root-boundary",ERROR_ACCESS_DENIED);
    diagnostic.path=canonical;
    // A user-supplied profile must not traverse a junction, even for reads.
    // Check every existing component, not just the final file attribute.
    for(size_t end=3;end<=canonical.size();++end){
        if(end<canonical.size()&&canonical[end]!=L'\\')continue;
        const auto component=canonical.substr(0,end);DWORD attrs=0,error=ERROR_SUCCESS;
        if(!Attributes(component,attrs,error)){
            diagnostic.path=component;
            return Reject(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?Result::Missing:Result::Invalid,"metadata-handle",error);
        }
        if(attrs&FILE_ATTRIBUTE_REPARSE_POINT){diagnostic.path=component;return Reject(Result::Invalid,"reparse-point",ERROR_ACCESS_DENIED);}
        if(end==canonical.size()&&(attrs&FILE_ATTRIBUTE_DIRECTORY))return Reject(Result::Invalid,"file-is-directory",ERROR_DIRECTORY);
    }
    SetLastError(ERROR_SUCCESS);
    if(!ReadCoopFile(canonical,file)){
        const DWORD error=GetLastError();file.clear();
        return Reject(Result::Invalid,"native-file",error);
    }
    const char* internal=reinterpret_cast<const char*>(file.data()+0x24+coop_career::kPersonalNameOffset);
    if(!std::memchr(internal,0,32)||!SafeName(internal)||
        !coop_save::EqualPathText(WideText(internal),WideText(name.c_str()))){file.clear();return Reject(Result::Invalid,"internal-name",ERROR_INVALID_DATA);}
    diagnostic.stage="loaded";
    return Result::Loaded;
}
// Read-only admission BEFORE broadcasting a career-selection ticket. Native
// root switching/imports/deserialization still happen only in the owned menu
// transaction. A rejected Host must not send the Client into a different UI.
static Result ProbeHost(const char* configured,std::vector<uint8_t>& file){
    file.clear();diagnostic={};primaryDiagnostic={};primaryRoot.clear();usedFallback=false;
    if(!configured||!*configured)return Result::Missing;
    const std::string selected(configured);
    primaryRoot=gProfileContext.CoopRoot(coop_save::Role::Host);
    auto result=Read(primaryRoot,selected,file);primaryDiagnostic=diagnostic;
    if(result==Result::Missing){usedFallback=true;result=Read(gCoopBaseRoot,selected,file);}
    if(result!=Result::Loaded)file.clear();
    return result;
}
static Result Select(const char* configured,std::vector<uint8_t>& file,std::string& name){
    file.clear();name.clear();diagnostic={};primaryDiagnostic={};primaryRoot.clear();usedFallback=false;
    if(!configured||!*configured)return Result::Missing; // caller retains latest policy
    const std::string selected(configured);
    if(!SafeName(selected))return Reject(Result::Invalid,"profile-name",ERROR_INVALID_NAME);
    primaryRoot=gProfileContext.ActiveRoot();
    auto result=Read(primaryRoot,selected,file);primaryDiagnostic=diagnostic;
    if(result==Result::Missing){
        usedFallback=true;
        result=Read(gCoopBaseRoot,selected,file);
        // Legacy root import is a COPY. Existing role files are never replaced
        // by a different directory's profile, even if they are corrupt.
        if(result==Result::Loaded&&!CommitCoopFile(selected.c_str(),file))result=Reject(Result::Invalid,"legacy-copy",GetLastError());
    }
    if(result==Result::Loaded)name=selected;
    else file.clear();
    return result;
}
} // namespace CoopProfileSelect
