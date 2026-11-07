// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopSpecialRecovery.h"
#include "CoopSpecialReceipt.h"

// Local-only translation between validated CURRENT-session evidence and an
// immutable OLD personal receipt. No disk writes, payout, native replay or
// imported peer images occur here. The game-thread adapter must persist the
// resulting authorization before telling the native lifecycle to continue.
namespace coop_special_recovery_receipt {
inline uint32_t Campaign(const std::string& name){
    uint32_t h=2166136261u;for(uint8_t c:name){if(c>='a'&&c<='z')c-=32;h=(h^c)*16777619u;}return h?h:1;
}
inline coop_special_recovery::Context Context(const coop_special_receipt::Scope& s){return {Campaign(s.campaign),s.catalog};}
inline coop_special_recovery::Key Key(const coop_special_receipt::Scope& s){
    return {s.session,s.ticket,s.operation,s.hostNonce,s.clientNonce,s.event,s.stage,s.kind,0};
}
inline bool SharedIdentity(const coop_special_receipt::Scope& s,const coop_special_recovery::Request& request){
    return coop_special_receipt::Valid(s)&&coop_special_recovery::Valid(request)&&coop_special_recovery::Same(Context(s),request.context)&&
        coop_special_recovery::Same(Key(s),request.old);
}
inline bool CoordinatorProof(const coop_special_receipt::Transaction& original,const coop_special_recovery::Request& request,
    const std::string& ownPersonal,const std::string& campaign,coop_special_recovery::CoordinatorProof& output){
    if(original.scope.personal!=ownPersonal||original.scope.campaign!=campaign||!SharedIdentity(original.scope,request))return false;
    const auto decision=coop_special_receipt::OriginalCoordinatorDecision(original);
    if(decision==coop_special_receipt::CoordinatorDecision::Unknown)return false;
    // Deliberately NOT comparing request.beforeDigest with Host's own image.
    // Cars, money and native baseline CRCs are personal and need not match.
    output={request.context,request.old,decision==coop_special_receipt::CoordinatorDecision::Commit?
        coop_special_recovery::Decision::Commit:coop_special_recovery::Decision::Abort,1};return true;
}
inline bool MatchesPending(const coop_special_receipt::Transaction& own,const coop_special_recovery::Evidence& evidence){
    const auto& r=evidence.request;
    return bool(evidence)&&coop_special_receipt::Valid(own)&&own.phase==coop_special_receipt::Phase::Prepared&&
        own.ownDone&&own.scope.localPlayer==2&&SharedIdentity(own.scope,r)&&r.session&&r.ticket&&r.hostNonce&&r.clientNonce&&
        r.id&&r.serial&&r.beforeDigest==coop_special_receipt::Digest(own.before)&&
        (evidence.decision==coop_special_recovery::Decision::Commit||evidence.decision==coop_special_recovery::Decision::Abort);
}
inline bool AbortDecision(const coop_special_receipt::Transaction& own,const coop_special_recovery::Evidence& evidence,
    coop_special_receipt::AuthenticatedAbortDecision& output){
    if(!MatchesPending(own,evidence)||evidence.decision!=coop_special_recovery::Decision::Abort)return false;
    const auto& r=evidence.request;output={own.scope,{r.session,r.ticket,r.hostNonce,r.clientNonce}};return true;
}
} // namespace coop_special_recovery_receipt
