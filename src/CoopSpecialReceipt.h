// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopProfileContext.h"
#include <array>
#include <cstring>
#include <string>
#include <vector>

// Local-only, filesystem/engine/network-independent own reward transaction.
// A Sealed transaction is NOT proof of an on-disk native commit. The adapter
// must fence native autosaves/imports, persist/flush this record, atomically
// install the exact post-image and verify it before acknowledging LocalSaved.
// Host must durably flush Authorized BEFORE publishing shared Commit. Merely
// recovering Prepared is not evidence that any coordinator decided Commit.
// OwnDone must be durable BEFORE publishing Done, and Paid must be durable
// BEFORE publishing LocalSaved or releasing the native autosave/import fence.
// Exchanging an old-session decision after authenticated reconnect is an
// adapter responsibility; this pure module does not implement that exchange.
// No raw part, car, UI, mission or catalog pointers belong in Scope.
namespace coop_special_receipt {
constexpr uint32_t kMagic=0x5253324E; // N2SR
constexpr uint16_t kVersion=4; // Earlier experimental formats were not installed.
constexpr size_t kHeaderSize=168,kMaximumBytes=128*1024;
constexpr size_t kPersonalNameOffset=0x24+0xD201,kNameBytes=32;
enum class Phase:uint8_t {Empty=0,Prepared=1,Authorized=2,Sealed=3,Paid=4,CancelPending=5,Cancelled=6};
enum class Recovery {Invalid,Conflict,NeedDecision,NeedOwnReplay,InstallSealed,AlreadySaved,InstallCancellation,AlreadyCancelled};
struct Scope {
    std::string campaign,personal;
    uint32_t event=0,catalog=0;
    uint8_t stage=0,kind=0,localPlayer=0; // Original role 1/2, not the reconnect role.
    uint64_t session=0;
    uint32_t ticket=0,operation=0,carId=0,choiceId=0,choiceSlot=0,hostNonce=0,clientNonce=0;
};
struct AbortAuthorization {uint64_t session=0;uint32_t ticket=0,hostNonce=0,clientNonce=0;};
// Construct only after the current authenticated coordinator decision exchange
// validates both nonce echoes and the immutable old operation. This is a local
// capability object, never a peer packet to deserialize directly into a call.
struct AuthenticatedAbortDecision {Scope old;AbortAuthorization current;};
inline bool Valid(const AbortAuthorization& a){return a.session&&a.ticket&&a.hostNonce&&a.clientNonce;}
inline bool Empty(const AbortAuthorization& a){return !a.session&&!a.ticket&&!a.hostNonce&&!a.clientNonce;}
inline bool Same(const AbortAuthorization& a,const AbortAuthorization& b){return a.session==b.session&&a.ticket==b.ticket&&a.hostNonce==b.hostNonce&&a.clientNonce==b.clientNonce;}
struct Transaction {
    Phase phase=Phase::Empty;
    bool ownDone=false;
    Scope scope;
    AbortAuthorization abort;
    std::vector<uint8_t> before,after;
};
inline bool Same(const Scope& a,const Scope& b){
    return a.campaign==b.campaign&&a.personal==b.personal&&a.event==b.event&&a.catalog==b.catalog&&
        a.stage==b.stage&&a.kind==b.kind&&a.session==b.session&&a.ticket==b.ticket&&
        a.operation==b.operation&&a.carId==b.carId&&a.choiceId==b.choiceId&&a.choiceSlot==b.choiceSlot&&
        a.localPlayer==b.localPlayer&&a.hostNonce==b.hostNonce&&a.clientNonce==b.clientNonce;
}
inline bool SafeName(const std::string& name){
    if(name.empty()||name.size()>31)return false;
    std::wstring wide;wide.reserve(name.size());
    for(uint8_t c:name)wide.push_back(wchar_t(c));
    return coop_save::IsSafeProfileName(wide);
}
inline bool Valid(const Scope& s){
    // Stable car/choice IDs are resolved against the local native catalogs by
    // the adapter. Their numeric value must never be dereferenced by this code.
    return SafeName(s.campaign)&&SafeName(s.personal)&&s.event&&s.catalog&&s.stage<8&&
        s.kind>=1&&s.kind<=5&&(s.localPlayer==1||s.localPlayer==2)&&s.session&&s.ticket&&s.operation&&s.carId&&s.hostNonce&&s.clientNonce;
}
inline bool ReadName(const uint8_t* text,std::string& name){
    size_t count=0;while(count<kNameBytes&&text[count])++count;
    if(!count||count==kNameBytes)return false;
    // Native files may retain old bytes after the terminating NUL. These are
    // personal data and must survive byte-for-byte, not be rewritten here.
    name.assign(reinterpret_cast<const char*>(text),count);
    return SafeName(name);
}
inline bool Native(const std::vector<uint8_t>& bytes,const std::string& personal){
    if(!SafeName(personal)||coop_save::ValidateNativeProfileFile(bytes.data(),bytes.size())!=coop_save::SaveValidation::Valid)return false;
    std::string embedded;
    return ReadName(bytes.data()+kPersonalNameOffset,embedded)&&embedded==personal;
}
inline bool ChoiceKnown(const Scope& scope){return scope.choiceId!=0||scope.choiceSlot!=0;}
inline bool NeedsChoice(const Scope& scope){return scope.kind==2||scope.kind==3;}
inline bool IsCancellation(Phase p){return p==Phase::CancelPending||p==Phase::Cancelled;}
inline bool Terminal(Phase p){return p==Phase::Paid||p==Phase::Cancelled;}
inline bool Valid(const Transaction& tx){
    if(!Valid(tx.scope)||!Native(tx.before,tx.scope.personal))return false;
    if(tx.ownDone&&NeedsChoice(tx.scope)&&!ChoiceKnown(tx.scope))return false;
    if(IsCancellation(tx.phase))return Native(tx.after,tx.scope.personal)&&
        (tx.ownDone?Valid(tx.abort):Empty(tx.abort));
    if(!Empty(tx.abort))return false;
    if(tx.phase==Phase::Prepared)return tx.after.empty();
    if(!tx.ownDone)return false;
    if(tx.phase==Phase::Authorized)return tx.after.empty();
    return (tx.phase==Phase::Sealed||tx.phase==Phase::Paid)&&Native(tx.after,tx.scope.personal)&&tx.after!=tx.before;
}
enum class CoordinatorDecision {Unknown,Commit,Abort};
inline CoordinatorDecision OriginalCoordinatorDecision(const Transaction& tx){
    // A Client's own Paid/Cancelled record can never stand in for evidence
    // about what the ORIGINAL coordinator durably decided.
    if(!Valid(tx)||tx.scope.localPlayer!=1)return CoordinatorDecision::Unknown;
    if(tx.phase==Phase::Authorized||tx.phase==Phase::Sealed||tx.phase==Phase::Paid)return CoordinatorDecision::Commit;
    return IsCancellation(tx.phase)?CoordinatorDecision::Abort:CoordinatorDecision::Unknown;
}
inline bool Empty(const Transaction& tx){
    const Scope zero;
    return tx.phase==Phase::Empty&&!tx.ownDone&&Same(tx.scope,zero)&&Empty(tx.abort)&&tx.before.empty()&&tx.after.empty();
}
inline bool Prepare(const Scope& scope,const std::vector<uint8_t>& before,Transaction& out){
    if(!Valid(scope)||!Native(before,scope.personal))return false;
    if(!Empty(out))return Valid(out)&&Same(out.scope,scope)&&out.before==before;
    Transaction next;next.phase=Phase::Prepared;next.scope=scope;next.before=before;out=std::move(next);return true;
}
inline bool WithChoice(Transaction& tx,const Scope& expected,uint32_t choiceId,uint32_t choiceSlot){
    if(!Valid(tx)||!Valid(expected)||!Same(tx.scope,expected)||tx.phase!=Phase::Prepared||tx.ownDone||
        (!choiceId&&!choiceSlot))return false;
    if(ChoiceKnown(tx.scope))return tx.scope.choiceId==choiceId&&tx.scope.choiceSlot==choiceSlot;
    // Zero/zero is unknown, never a real bound choice. Native zero-based
    // selections must use a stable nonzero identifier (e.g. index + 1).
    Transaction next=tx;next.scope.choiceId=choiceId;next.scope.choiceSlot=choiceSlot;tx=std::move(next);return true;
}
inline bool MarkOwnDone(Transaction& tx,const Scope& expected){
    if(!Valid(tx)||!Valid(expected)||!Same(tx.scope,expected)||tx.phase!=Phase::Prepared||
        (NeedsChoice(tx.scope)&&!ChoiceKnown(tx.scope)))return false;
    tx.ownDone=true;return true;
}
inline bool Authorize(Transaction& tx,const Scope& expected,const std::vector<uint8_t>& currentBaseline){
    if(!Valid(tx)||!Valid(expected)||!Same(tx.scope,expected)||!tx.ownDone||currentBaseline!=tx.before||
        (NeedsChoice(tx.scope)&&!ChoiceKnown(tx.scope)))return false;
    if(tx.phase==Phase::Authorized||tx.phase==Phase::Sealed||tx.phase==Phase::Paid)return true;
    if(tx.phase!=Phase::Prepared)return false;
    Transaction next=tx;next.phase=Phase::Authorized;tx=std::move(next);return true;
}
inline bool Seal(Transaction& tx,const Scope& expected,const std::vector<uint8_t>& currentBaseline,
    const std::vector<uint8_t>& after){
    if(!Valid(tx)||!Valid(expected)||!Same(tx.scope,expected)||currentBaseline!=tx.before||
        !Native(after,expected.personal)||after==tx.before)return false;
    if(tx.phase==Phase::Sealed||tx.phase==Phase::Paid)return tx.after==after;
    if(tx.phase!=Phase::Authorized)return false;
    Transaction next=tx;next.after=after;next.phase=Phase::Sealed;tx=std::move(next);return true;
}
inline bool MarkPaid(Transaction& tx,const Scope& expected,const std::vector<uint8_t>& currentNative){
    if(!Valid(tx)||!Valid(expected)||!Same(tx.scope,expected)||!Native(currentNative,expected.personal))return false;
    // A durable Paid receipt survives legitimate later personal autosaves.
    // Its original images remain immutable; never install them again here.
    if(tx.phase==Phase::Paid)return true;
    if(tx.phase!=Phase::Sealed||currentNative!=tx.after)return false;
    tx.phase=Phase::Paid;return true;
}
inline bool CancelPending(Transaction& tx,const Scope& expected,const std::vector<uint8_t>& currentBaseline,
    const std::vector<uint8_t>& after,bool nativeCleanupComplete,const AuthenticatedAbortDecision* decision=nullptr){
    if(!nativeCleanupComplete||!Valid(tx)||!Valid(expected)||!Same(tx.scope,expected)||
        tx.phase!=Phase::Prepared||currentBaseline!=tx.before||!Native(after,expected.personal))return false;
    if(tx.ownDone&&(!decision||!Same(decision->old,expected)||!Valid(decision->current)))return false;
    Transaction next=tx;next.phase=Phase::CancelPending;next.after=after;
    next.abort=tx.ownDone?decision->current:AbortAuthorization{};tx=std::move(next);return true;
}
inline bool MarkCancelled(Transaction& tx,const Scope& expected,const std::vector<uint8_t>& currentNative){
    if(!Valid(tx)||!Valid(expected)||!Same(tx.scope,expected)||!Native(currentNative,expected.personal))return false;
    if(tx.phase==Phase::Cancelled)return true;
    if(tx.phase!=Phase::CancelPending||currentNative!=tx.after)return false;
    tx.phase=Phase::Cancelled;return true;
}
inline Recovery Recover(const Transaction& tx,const Scope& expected,const std::vector<uint8_t>& current){
    if(!Valid(tx)||!Valid(expected)||
        coop_save::ValidateNativeProfileFile(current.data(),current.size())!=coop_save::SaveValidation::Valid)return Recovery::Invalid;
    std::string personal;if(!ReadName(current.data()+kPersonalNameOffset,personal))return Recovery::Invalid;
    if(personal!=expected.personal||!Same(tx.scope,expected))return Recovery::Conflict;
    if(tx.phase==Phase::Paid)return Recovery::AlreadySaved;
    if(tx.phase==Phase::Cancelled)return Recovery::AlreadyCancelled;
    if(tx.phase==Phase::CancelPending){
        if(current==tx.after)return Recovery::AlreadyCancelled;
        return current==tx.before?Recovery::InstallCancellation:Recovery::Conflict;
    }
    if(current==tx.before){
        if(tx.phase==Phase::Prepared)return Recovery::NeedDecision;
        return tx.phase==Phase::Authorized?Recovery::NeedOwnReplay:Recovery::InstallSealed;
    }
    if(tx.phase==Phase::Sealed&&current==tx.after)return Recovery::AlreadySaved;
    return Recovery::Conflict;
}
inline void Put16(uint8_t* p,uint16_t value){p[0]=uint8_t(value);p[1]=uint8_t(value>>8);}
inline uint16_t Get16(const uint8_t* p){return uint16_t(p[0])|uint16_t(uint16_t(p[1])<<8);}
inline void Put64(uint8_t* p,uint64_t value){coop_save::WriteLe32(p,uint32_t(value));coop_save::WriteLe32(p+4,uint32_t(value>>32));}
inline uint64_t Get64(const uint8_t* p){return uint64_t(coop_save::ReadLe32(p))|(uint64_t(coop_save::ReadLe32(p+4))<<32);}
inline uint32_t Digest(const std::vector<uint8_t>& bytes){return coop_save::EaCrc32(bytes.data(),bytes.size());}
inline bool Encode(const Transaction& tx,std::vector<uint8_t>& output){
    if(!Valid(tx))return false;
    const size_t total=kHeaderSize+tx.before.size()+tx.after.size();if(total>kMaximumBytes)return false;
    std::vector<uint8_t> bytes(total,0);auto* p=bytes.data();const auto& s=tx.scope;
    coop_save::WriteLe32(p,kMagic);Put16(p+4,kVersion);p[6]=uint8_t(tx.phase);p[7]=tx.ownDone?1:0;
    coop_save::WriteLe32(p+8,uint32_t(total));
    coop_save::WriteLe32(p+16,uint32_t(tx.before.size()));coop_save::WriteLe32(p+20,uint32_t(tx.after.size()));
    coop_save::WriteLe32(p+24,Digest(tx.before));coop_save::WriteLe32(p+28,tx.after.empty()?0:Digest(tx.after));
    std::memcpy(p+32,s.campaign.data(),s.campaign.size());std::memcpy(p+64,s.personal.data(),s.personal.size());
    coop_save::WriteLe32(p+96,s.event);coop_save::WriteLe32(p+100,s.catalog);p[104]=s.stage;p[105]=s.kind;
    Put64(p+108,s.session);coop_save::WriteLe32(p+116,s.ticket);coop_save::WriteLe32(p+120,s.operation);
    coop_save::WriteLe32(p+124,s.carId);coop_save::WriteLe32(p+128,s.choiceId);coop_save::WriteLe32(p+132,s.choiceSlot);
    p[106]=s.localPlayer;coop_save::WriteLe32(p+136,s.hostNonce);coop_save::WriteLe32(p+140,s.clientNonce);
    Put64(p+144,tx.abort.session);coop_save::WriteLe32(p+152,tx.abort.ticket);
    coop_save::WriteLe32(p+156,tx.abort.hostNonce);coop_save::WriteLe32(p+160,tx.abort.clientNonce);
    std::copy(tx.before.begin(),tx.before.end(),bytes.begin()+kHeaderSize);
    std::copy(tx.after.begin(),tx.after.end(),bytes.begin()+kHeaderSize+tx.before.size());
    coop_save::WriteLe32(p+12,Digest(bytes));output=std::move(bytes);return true;
}
inline bool WireName(const uint8_t* text,std::string& name){
    if(!ReadName(text,name))return false;
    // Canonical transaction metadata, unlike the immutable native images,
    // cannot hide additional values after a NUL terminator.
    for(size_t i=name.size();i<kNameBytes;++i)if(text[i])return false;
    return true;
}
inline bool Decode(const uint8_t* data,size_t size,Transaction& out){
    if(!data||size<kHeaderSize||size>kMaximumBytes||coop_save::ReadLe32(data)!=kMagic||Get16(data+4)!=kVersion||
        data[7]>1||data[107]||coop_save::ReadLe32(data+164)||coop_save::ReadLe32(data+8)!=size)return false;
    const auto phase=Phase(data[6]);
    const uint32_t beforeSize=coop_save::ReadLe32(data+16),afterSize=coop_save::ReadLe32(data+20);
    if((phase!=Phase::Prepared&&phase!=Phase::Authorized&&phase!=Phase::Sealed&&phase!=Phase::Paid&&!IsCancellation(phase))||beforeSize!=coop_save::kNativeProfileFileSize||
        afterSize!=((phase==Phase::Sealed||phase==Phase::Paid||IsCancellation(phase))?coop_save::kNativeProfileFileSize:0)||
        size!=kHeaderSize+size_t(beforeSize)+size_t(afterSize))return false;
    std::vector<uint8_t> checked(data,data+size);const uint32_t crc=coop_save::ReadLe32(data+12);
    coop_save::WriteLe32(checked.data()+12,0);if(Digest(checked)!=crc)return false;
    Transaction next;next.phase=phase;next.ownDone=data[7]!=0;auto& s=next.scope;
    if(!WireName(data+32,s.campaign)||!WireName(data+64,s.personal))return false;
    s.event=coop_save::ReadLe32(data+96);s.catalog=coop_save::ReadLe32(data+100);s.stage=data[104];s.kind=data[105];
    s.session=Get64(data+108);s.ticket=coop_save::ReadLe32(data+116);s.operation=coop_save::ReadLe32(data+120);
    s.carId=coop_save::ReadLe32(data+124);s.choiceId=coop_save::ReadLe32(data+128);s.choiceSlot=coop_save::ReadLe32(data+132);
    s.localPlayer=data[106];s.hostNonce=coop_save::ReadLe32(data+136);s.clientNonce=coop_save::ReadLe32(data+140);
    next.abort.session=Get64(data+144);next.abort.ticket=coop_save::ReadLe32(data+152);
    next.abort.hostNonce=coop_save::ReadLe32(data+156);next.abort.clientNonce=coop_save::ReadLe32(data+160);
    if(!Valid(s))return false;
    next.before.assign(data+kHeaderSize,data+kHeaderSize+beforeSize);
    if(afterSize)next.after.assign(data+kHeaderSize+beforeSize,data+size);
    if(Digest(next.before)!=coop_save::ReadLe32(data+24)||
        (next.after.empty()?0:Digest(next.after))!=coop_save::ReadLe32(data+28)||!Valid(next))return false;
    // Decode must not overwrite a loaded different/modified transaction. A
    // caller intentionally replacing context must explicitly clear its slot.
    if(!Empty(out)){
        std::vector<uint8_t> existing;
        if(!Encode(out,existing)||existing.size()!=size||!std::equal(existing.begin(),existing.end(),data))return false;
        return true;
    }
    out=std::move(next);return true;
}
inline bool Decode(const std::vector<uint8_t>& data,Transaction& out){return Decode(data.data(),data.size(),out);}
static_assert(kHeaderSize+2*coop_save::kNativeProfileFileSize<kMaximumBytes,"Full before/after transaction must stay bounded");
} // namespace coop_special_receipt
