// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopSpecialReceipt.h"
#include <atomic>
#include <functional>
#include <cstdio>
#include <cwchar>

// Game-thread local durable adapter; only its atomic gates may be read from
// native IO hooks. No engine calls, session traffic, or automatic reward replay.
// Do not wire native grants until every native save path obeys MayNativeSave.
namespace CoopSpecialNative { namespace ReceiptStore {
using Bytes=std::vector<uint8_t>;
using Scope=coop_special_receipt::Scope;
using Transaction=coop_special_receipt::Transaction;
using Phase=coop_special_receipt::Phase;
using Recovery=coop_special_receipt::Recovery;
enum class FileKind {Native,Current,Archive};
struct FileId {FileKind kind=FileKind::Current;uint64_t session=0;uint32_t ticket=0,operation=0;};
inline FileId NativeId(){return {FileKind::Native,0,0,0};}
inline FileId CurrentId(){return {FileKind::Current,0,0,0};}
inline FileId ArchiveId(const Scope& s){return {FileKind::Archive,s.session,s.ticket,s.operation};}
inline bool ValidId(const FileId& id){
    if(id.kind==FileKind::Archive)return id.session&&id.ticket&&id.operation;
    return (id.kind==FileKind::Native||id.kind==FileKind::Current)&&!id.session&&!id.ticket&&!id.operation;
}
inline std::wstring Suffix(const FileId& id){
    if(!ValidId(id))return {};
    if(id.kind==FileKind::Native)return L"";
    if(id.kind==FileKind::Current)return L".coop-special-receipt";
    wchar_t text[100]{};
    std::swprintf(text,sizeof(text)/sizeof(*text),L".coop-special-receipt.%016llX-%08X-%08X.archive",
        static_cast<unsigned long long>(id.session),id.ticket,id.operation);
    return text;
}
struct Ports {
    std::function<int(const std::string&,const FileId&)> probe; // 0 missing, 1 regular file, -1 inaccessible/reparse/error.
    std::function<bool(const std::string&,const FileId&,Bytes&)> read;
    std::function<bool(const std::string&,const FileId&,const Bytes&)> write; // Atomic+flushed own file commit.
};
// Path preflight never creates a directory. The context-selected active root,
// not a peer-supplied role or path, is the only possible storage domain.
static int ProductionPath(const std::string& name,const FileId& id,std::wstring& path){
    path.clear();if(!ValidId(id)||!coop_special_receipt::SafeName(name)||!gProfileContext.IsCoopActive())return -1;
    const auto root=gProfileContext.ActiveRoot();
    if(!coop_save::EqualPathText(root,gProfileContext.CoopRoot(coop_save::Role::Host))&&
        !coop_save::EqualPathText(root,gProfileContext.CoopRoot(coop_save::Role::Client)))return -1;
    std::wstring canonical,wide=WideText(name.c_str());
    if(wide.empty()||!coop_save::IsSafeProfileName(wide)||
        !coop_save::CanonicalWindowsPath(root+L"\\"+wide+L"\\"+wide+Suffix(id),canonical)||
        !coop_save::PathIsWithin(canonical,root)||canonical.size()>=MAX_PATH)return -1;
    path=canonical;
    // Missing ancestors mean missing data, never a reason to create/reset a
    // profile. An existing junction/symlink anywhere in the path is an error.
    for(size_t i=3;i<=path.size();++i){
        if(i<path.size()&&path[i]!=L'\\')continue;
        const auto part=path.substr(0,i);const DWORD attrs=GetFileAttributesW(part.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES){const auto e=GetLastError();return e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND?0:-1;}
        if((attrs&FILE_ATTRIBUTE_REPARSE_POINT)||(i<path.size()&&!(attrs&FILE_ATTRIBUTE_DIRECTORY))||
            (i==path.size()&&(attrs&FILE_ATTRIBUTE_DIRECTORY)))return -1;
    }
    return 1;
}
static int ProductionProbe(const std::string& name,const FileId& id){std::wstring path;return ProductionPath(name,id,path);}
static bool ProductionRead(const std::string& name,const FileId& id,Bytes& output){
    std::wstring path;if(ProductionPath(name,id,path)!=1)return false;
    HANDLE handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER length{};wchar_t finalName[MAX_PATH]{};
    const DWORD count=GetFinalPathNameByHandleW(handle,finalName,MAX_PATH,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    std::wstring finalPath;bool okay=GetFileInformationByHandle(handle,&info)!=FALSE&&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&
        GetFileSizeEx(handle,&length)!=FALSE&&length.QuadPart>0&&length.QuadPart<=coop_special_receipt::kMaximumBytes&&
        count&&count<MAX_PATH&&coop_save::CanonicalWindowsPath(finalName,finalPath)&&coop_save::EqualPathText(path,finalPath);
    if(okay&&id.kind==FileKind::Native)okay=length.QuadPart==coop_save::kNativeProfileFileSize;
    Bytes bytes;if(okay){bytes.resize(static_cast<size_t>(length.QuadPart));DWORD got=0;
        okay=ReadFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr)!=FALSE&&got==bytes.size();}
    const bool closed=CloseHandle(handle)!=FALSE;if(!okay||!closed)return false;
    output=std::move(bytes);return true;
}
static bool ProductionWrite(const std::string& name,const FileId& id,const Bytes& bytes){
    std::wstring path;const int found=ProductionPath(name,id,path);
    if(found<0||bytes.empty()||bytes.size()>coop_special_receipt::kMaximumBytes)return false;
    if(id.kind==FileKind::Native)return coop_special_receipt::Native(bytes,name)&&CommitCoopFile(name.c_str(),bytes);
    Transaction tx;if(!coop_special_receipt::Decode(bytes,tx)||tx.scope.personal!=name)return false;
    if(id.kind==FileKind::Archive&&(!coop_special_receipt::Terminal(tx.phase)||tx.scope.session!=id.session||
        tx.scope.ticket!=id.ticket||tx.scope.operation!=id.operation))return false;
    if(id.kind==FileKind::Archive&&found==1){Bytes existing;return ProductionRead(name,id,existing)&&existing==bytes;}
    const auto suffix=Suffix(id);return CommitCoopBytes(name.c_str(),bytes,suffix.c_str());
}
inline Ports ProductionPorts(){return {ProductionProbe,ProductionRead,ProductionWrite};}
enum class LoadState {Unbound,None,Pending,Paid,Cancelled,Faulted};
class Store {
public:
    Transaction tx;
    explicit Store(Ports ports=ProductionPorts()):io(std::move(ports)){}
    bool Bind(const std::string& personal,const std::string& campaign){return Load(personal,campaign);}
    bool Load(const std::string& personal,const std::string& campaign){
        gate.store(false,std::memory_order_release);
        if(fault&&!reloading)return false; // Only explicit Reload may resolve ambiguous IO.
        if(!coop_special_receipt::SafeName(personal)||!coop_special_receipt::SafeName(campaign)||!io.probe||!io.read||!io.write)return Fail();
        if(!coop_special_receipt::Empty(tx)&&!coop_special_receipt::Terminal(tx.phase)&&
            (tx.scope.personal!=personal||tx.scope.campaign!=campaign))return Fail();
        profile=personal;team=campaign;bound=true;loaded=false;fault=false;
        Bytes bytes,native;const int found=Fetch(CurrentId(),bytes);
        if(found<0)return Fail();
        const int haveNative=Fetch(NativeId(),native);if(haveNative<0)return Fail();
        if(haveNative&&!coop_special_receipt::Native(native,profile))return Fail();
        Transaction next;
        if(found){
            if(!haveNative||!coop_special_receipt::Decode(bytes,next)||next.scope.personal!=profile||next.scope.campaign!=team)return Fail();
            const auto result=coop_special_receipt::Recover(next,next.scope,native);
            if(result==Recovery::Invalid||result==Recovery::Conflict)return Fail();
            if(!coop_special_receipt::Empty(tx)){
                auto a=tx.scope,b=next.scope;a.choiceId=a.choiceSlot=b.choiceId=b.choiceSlot=0;
                if(!coop_special_receipt::Same(a,b)){
                    if(!coop_special_receipt::Terminal(tx.phase))return Fail();
                    if(tx.scope.personal==profile&&tx.scope.campaign==team){
                        Bytes archived,expected;
                        if(Fetch(ArchiveId(tx.scope),archived)!=1||!coop_special_receipt::Encode(tx,expected)||archived!=expected)return Fail();
                    }
                }else if(tx.before!=next.before||!MayAdvance(tx.phase,next.phase)||
                    (tx.ownDone&&!next.ownDone)||
                    (coop_special_receipt::ChoiceKnown(tx.scope)&&
                        (tx.scope.choiceId!=next.scope.choiceId||tx.scope.choiceSlot!=next.scope.choiceSlot))||
                    (!tx.after.empty()&&tx.after!=next.after)||
                    (!coop_special_receipt::Empty(tx.abort)&&!coop_special_receipt::Same(tx.abort,next.abort)))return Fail();
            }
            recovery=result;
        }else{
            // A disappeared outstanding receipt must not be treated as a new
            // empty profile in this same process. A fresh career may have no
            // native file yet, but cannot Prepare until its first save exists.
            if(!coop_special_receipt::Empty(tx)&&tx.scope.personal==profile&&tx.scope.campaign==team)return Fail();
            recovery=Recovery::Invalid;
        }
        tx=std::move(next);loaded=true;fault=false;PublishGate();return true;
    }
    bool Reload(){if(profile.empty()||team.empty())return false;reloading=true;
        const bool okay=Load(profile,team);reloading=false;return okay;}
    void Unbind(){gate.store(false,std::memory_order_release);bound=false;loaded=false;}
    bool Ready()const{return bound&&loaded&&!fault;}
    bool MayImport()const{return gate.load(std::memory_order_acquire);}
    bool MayNativeSave()const{return gate.load(std::memory_order_acquire);}
    LoadState Status()const{
        if(fault)return LoadState::Faulted;if(!bound||!loaded)return LoadState::Unbound;
        return coop_special_receipt::Empty(tx)?LoadState::None:tx.phase==Phase::Paid?LoadState::Paid:
            tx.phase==Phase::Cancelled?LoadState::Cancelled:LoadState::Pending;
    }
    Recovery RecoveryState()const{return Ready()?recovery:Recovery::Invalid;}
    // Called ONLY by the game-thread Prepare driver after its exact native
    // save barrier has been proved held. Preserve genuine unsaved own cash,
    // car and customization before any native mission starts. A crash between
    // baseline and Prepared is safe: no Armed/Begin has been acknowledged yet.
    bool PrepareLive(const Scope& scope,const Bytes& ownBefore){
        if(!Ready()||!Matches(scope)||!coop_special_receipt::Native(ownBefore,profile))return false;
        if(!coop_special_receipt::Empty(tx)&&!coop_special_receipt::Terminal(tx.phase)){
            return coop_special_receipt::Same(tx.scope,scope)&&tx.phase==Phase::Prepared&&tx.before==ownBefore&&
                PrepareReceipt(scope,ownBefore);
        }
        gate.store(false,std::memory_order_release);
        if(!CurrentMatchesMemory())return false;
        if(!coop_special_receipt::Empty(tx)&&!ArchiveTerminal())return false;
        Bytes native;if(!ReadNative(native))return Fail();
        if(native!=ownBefore){
            if(!io.write(profile,NativeId(),ownBefore)||!ReadNative(native)||native!=ownBefore)return Fail();
        }
        return PrepareReceipt(scope,ownBefore);
    }
    bool PrepareReceipt(const Scope& scope,const Bytes& before){
        if(!Ready()||!Matches(scope))return false;
        gate.store(false,std::memory_order_release);Bytes native;
        if(!ReadNative(native)||native!=before)return Fail();
        if(!CurrentMatchesMemory())return false;
        if(!coop_special_receipt::Empty(tx)){
            if(coop_special_receipt::Same(tx.scope,scope)){
                const bool same=coop_special_receipt::Prepare(scope,before,tx);if(same)PublishGate();return same;
            }
            if(!coop_special_receipt::Terminal(tx.phase))return false;
            if(!ArchiveTerminal())return false;
        }
        Transaction next;if(!coop_special_receipt::Prepare(scope,before,next))return false;
        return Persist(next);
    }
    bool BindChoice(const Scope& expected,uint32_t choiceId,uint32_t choiceSlot){
        if(!Ready()||!Matches(expected))return false;Transaction next=tx;
        return coop_special_receipt::WithChoice(next,expected,choiceId,choiceSlot)&&Persist(next);
    }
    bool MarkDone(const Scope& expected){
        if(!Ready()||!Matches(expected))return false;Transaction next=tx;
        return coop_special_receipt::MarkOwnDone(next,expected)&&Persist(next);
    }
    bool Authorize(const Scope& expected){
        if(!Ready()||!Matches(expected)||!coop_special_receipt::Same(tx.scope,expected))return false;
        gate.store(false,std::memory_order_release);Bytes native;
        if(!ReadNative(native))return Fail();Transaction next=tx;
        const auto result=coop_special_receipt::Recover(tx,expected,native);
        if(result==Recovery::Invalid||result==Recovery::Conflict)return Fail();
        // A durable Sealed/Paid decision can be acknowledged again without
        // pretending its current personal file must still equal pre-reward
        // baseline. This never downgrades phase or permits another grant.
        const auto& baseline=tx.phase==Phase::Prepared?native:tx.before;
        return coop_special_receipt::Authorize(next,expected,baseline)&&Persist(next);
    }
    bool SealAndInstall(const Bytes& after){
        if(!Ready()||coop_special_receipt::Empty(tx))return false;
        if(tx.phase==Phase::Paid||tx.phase==Phase::Sealed)return RecoverSealed(); // Ignore new external image; use durable stored one.
        if(tx.phase!=Phase::Authorized)return false;
        Bytes native;if(!ReadNative(native))return Fail();Transaction next=tx;
        if(!coop_special_receipt::Seal(next,tx.scope,native,after))return false;
        if(!Persist(next))return false;
        return RecoverSealed();
    }
    bool RecoverSealed(){
        if(!Ready()||coop_special_receipt::Empty(tx))return false;
        if(tx.phase!=Phase::Sealed&&tx.phase!=Phase::Paid)return false;
        gate.store(false,std::memory_order_release);Bytes native;
        if(!CurrentMatchesMemory())return false;
        if(!ReadNative(native))return Fail();
        recovery=coop_special_receipt::Recover(tx,tx.scope,native);
        if(tx.phase==Phase::Paid){if(recovery!=Recovery::AlreadySaved)return Fail();PublishGate();return true;}
        if(recovery==Recovery::InstallSealed){
            if(!io.write(profile,NativeId(),tx.after))return Fail();
            if(!ReadNative(native)||native!=tx.after)return Fail();
        }else if(recovery!=Recovery::AlreadySaved)return Fail();
        Transaction next=tx;if(!coop_special_receipt::MarkPaid(next,tx.scope,native))return Fail();
        return Persist(next); // Paid is flushed+verified before true/ACK/unfence.
    }
    bool CancelAndInstall(const Scope& expected,const Bytes& after,bool nativeCleanupComplete,
        const coop_special_receipt::AuthenticatedAbortDecision* decision=nullptr){
        if(!Ready()||!Matches(expected)||!coop_special_receipt::Same(tx.scope,expected))return false;
        if(coop_special_receipt::IsCancellation(tx.phase))return RecoverCancellation(); // Immutable stored abort image only.
        if(tx.phase!=Phase::Prepared)return false;
        gate.store(false,std::memory_order_release);Bytes native;
        if(!ReadNative(native))return Fail();Transaction next=tx;
        if(!coop_special_receipt::CancelPending(next,expected,native,after,nativeCleanupComplete,decision))return false;
        if(!Persist(next))return false;
        return RecoverCancellation();
    }
    bool RecoverCancellation(){
        if(!Ready()||!coop_special_receipt::IsCancellation(tx.phase))return false;
        gate.store(false,std::memory_order_release);Bytes native;
        if(!CurrentMatchesMemory())return false;
        if(!ReadNative(native))return Fail();
        recovery=coop_special_receipt::Recover(tx,tx.scope,native);
        if(tx.phase==Phase::Cancelled){if(recovery!=Recovery::AlreadyCancelled)return Fail();PublishGate();return true;}
        if(recovery==Recovery::InstallCancellation){
            if(!io.write(profile,NativeId(),tx.after))return Fail();
            if(!ReadNative(native)||native!=tx.after)return Fail();
        }else if(recovery!=Recovery::AlreadyCancelled)return Fail();
        Transaction next=tx;if(!coop_special_receipt::MarkCancelled(next,tx.scope,native))return Fail();
        return Persist(next); // Durable Cancelled precedes imports/native autosaves.
    }
    bool LookupArchive(const Scope& exact,Transaction& output){
        if(!Ready()||!Matches(exact))return false;Bytes bytes;const int found=Fetch(ArchiveId(exact),bytes);
        if(found<0)return Fail();if(!found)return false;
        Transaction archived;
        if(!coop_special_receipt::Decode(bytes,archived)||!coop_special_receipt::Terminal(archived.phase)||
            !coop_special_receipt::Same(archived.scope,exact))return Fail();
        // Do not overwrite a caller's different outstanding operation either.
        if(!coop_special_receipt::Empty(output)){
            Bytes current;if(!coop_special_receipt::Encode(output,current)||current!=bytes)return false;return true;
        }
        output=std::move(archived);return true;
    }
private:
    Ports io;std::string profile,team;bool bound=false,loaded=false,fault=false,reloading=false;
    std::atomic<bool> gate{false};Recovery recovery=Recovery::Invalid;
    bool Fail(){gate.store(false,std::memory_order_release);fault=true;return false;}
    void PublishGate(){gate.store(Ready()&&(coop_special_receipt::Empty(tx)||coop_special_receipt::Terminal(tx.phase)),std::memory_order_release);}
    static bool MayAdvance(Phase from,Phase to){
        if(from==Phase::Prepared)return to!=Phase::Empty;
        if(from==Phase::Authorized)return to==Phase::Authorized||to==Phase::Sealed||to==Phase::Paid;
        if(from==Phase::Sealed)return to==Phase::Sealed||to==Phase::Paid;
        if(from==Phase::CancelPending)return to==Phase::CancelPending||to==Phase::Cancelled;
        return from==to;
    }
    bool Matches(const Scope& s)const{return coop_special_receipt::Valid(s)&&s.personal==profile&&s.campaign==team;}
    int Fetch(const FileId& id,Bytes& bytes){
        if(!ValidId(id))return -1;const int result=io.probe(profile,id);
        if(result==0)return 0;if(result!=1)return -1;
        Bytes next;if(!io.read(profile,id,next)||next.empty()||next.size()>coop_special_receipt::kMaximumBytes)return -1;
        bytes=std::move(next);return 1;
    }
    bool ReadNative(Bytes& bytes){return Fetch(NativeId(),bytes)==1&&coop_special_receipt::Native(bytes,profile);}
    bool CurrentMatchesMemory(){
        Bytes actual;const int found=Fetch(CurrentId(),actual);
        if(found<0)return Fail();
        if(coop_special_receipt::Empty(tx))return found==0?true:Fail();
        Bytes expected;
        return found==1&&coop_special_receipt::Encode(tx,expected)&&actual==expected?true:Fail();
    }
    bool Persist(const Transaction& next){
        gate.store(false,std::memory_order_release);Bytes bytes;
        if(!Matches(next.scope)||!coop_special_receipt::Encode(next,bytes))return false;
        if(!CurrentMatchesMemory())return false;
        if(!io.write(profile,CurrentId(),bytes))return Fail();
        Bytes actual;if(Fetch(CurrentId(),actual)!=1||actual!=bytes)return Fail();
        tx=next;loaded=bound=true;fault=false;
        recovery=tx.phase==Phase::Paid?Recovery::AlreadySaved:tx.phase==Phase::Cancelled?Recovery::AlreadyCancelled:
            tx.phase==Phase::CancelPending?Recovery::InstallCancellation:tx.phase==Phase::Sealed?Recovery::InstallSealed:
            tx.phase==Phase::Authorized?Recovery::NeedOwnReplay:Recovery::NeedDecision;
        PublishGate();return true;
    }
    bool ArchiveTerminal(){
        if(!coop_special_receipt::Terminal(tx.phase)||!coop_special_receipt::Valid(tx)||!CurrentMatchesMemory())return false;
        Bytes expected;if(!coop_special_receipt::Encode(tx,expected))return false;
        const auto id=ArchiveId(tx.scope);Bytes actual;const int found=Fetch(id,actual);
        if(found<0)return Fail();if(found)return actual==expected?true:Fail();
        if(!io.write(profile,id,expected))return Fail();
        return Fetch(id,actual)==1&&actual==expected?true:Fail();
    }
};
// Runtime integration is deliberately separate. Unbind closes gates but never
// erases pending state; reload its own role-root file before a new handshake.
// Prepared/Authorized need genuine native/old-coordinator recovery, not a fake
// success here. Archive lookups return exact local Paid/Cancelled decisions;
// only OriginalCoordinatorDecision with localPlayer 1 can prove Host authority. No
// arbitrary suffix or full native image is accepted from session packets.
static Store store;
static Transaction& tx=store.tx;
inline bool Bind(const std::string& personal,const std::string& campaign){return store.Bind(personal,campaign);}
inline bool Load(const std::string& personal,const std::string& campaign){return store.Load(personal,campaign);}
inline bool Reload(){return store.Reload();}
inline void Unbind(){store.Unbind();}
inline bool Ready(){return store.Ready();}
inline LoadState Status(){return store.Status();}
inline Recovery RecoveryState(){return store.RecoveryState();}
inline bool MayImport(){return store.MayImport();}
inline bool MayNativeSave(){return store.MayNativeSave();}
inline bool PrepareReceipt(const Scope& scope,const Bytes& before){return store.PrepareReceipt(scope,before);}
inline bool PrepareLive(const Scope& scope,const Bytes& ownBefore){return store.PrepareLive(scope,ownBefore);}
inline bool BindChoice(const Scope& scope,uint32_t id,uint32_t slot){return store.BindChoice(scope,id,slot);}
inline bool MarkDone(const Scope& scope){return store.MarkDone(scope);}
inline bool Authorize(const Scope& scope){return store.Authorize(scope);}
inline bool SealAndInstall(const Bytes& after){return store.SealAndInstall(after);}
inline bool RecoverSealed(){return store.RecoverSealed();}
inline bool CancelAndInstall(const Scope& scope,const Bytes& after,bool nativeCleanupComplete,
    const coop_special_receipt::AuthenticatedAbortDecision* decision=nullptr){return store.CancelAndInstall(scope,after,nativeCleanupComplete,decision);}
inline bool RecoverCancellation(){return store.RecoverCancellation();}
inline bool LookupArchive(const Scope& scope,Transaction& output){return store.LookupArchive(scope,output);}
}} // namespace CoopSpecialNative::ReceiptStore
