// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopSpecial.h"

// Native work is requested on the game thread, never inside Receive(). Every
// completion carries an operation/incarnation token; a recycled handler or a
// reentrant native callback cannot acknowledge a later mission accidentally.
namespace coop_special {
enum class NativeAction:uint8_t {None,Prepare,Begin,Release,Commit,Abort};
enum class NativeResult:uint8_t {Wait,Applied,Refused};
struct NativeGuard {
    bool context=false,ownAtStart=false,peerAtStart=false,eligible=false,busy=false;
};
struct NativeRequest {
    NativeAction action=NativeAction::None;
    uint64_t session=0;uint32_t ticket=0,nonce=0,operation=0,request=0,serial=0;
    Spec spec{};
    explicit operator bool()const{return action!=NativeAction::None;}
};
class Lifecycle {
    uint64_t session_=0;uint32_t ticket_=0,nonce_=0,operation_=0,request_=0,serial_=0;
    bool prepared_=false,began_=false,released_=false,committed_=false,aborted_=false;
    NativeRequest pending_{};
    bool Current(const State& state)const{return session_==state.Session()&&ticket_==state.Ticket()&&
        nonce_==state.Nonce()&&operation_==state.Operation()&&request_==state.RequestId();}
    NativeRequest Issue(const State& state,NativeAction action){
        if(!++serial_)++serial_;
        pending_={action,state.Session(),state.Ticket(),state.Nonce(),state.Operation(),state.RequestId(),serial_,state.Current()};return pending_;
    }
public:
    void Reset(){*this=Lifecycle{};}
    bool ResumeAuthorized(const State& state){
        if(pending_||!state.OfflineRecovery()||!state.Active()||!(state.Phase()&Commit)||state.Terminal())return false;
        Reset();session_=state.Session();ticket_=state.Ticket();nonce_=state.Nonce();operation_=state.Operation();request_=state.RequestId();
        prepared_=began_=released_=true;return true;
    }
    bool Prepared()const{return prepared_;}bool Began()const{return began_;}
    bool Released()const{return released_;}bool Committed()const{return committed_;}
    bool Aborted()const{return aborted_;}bool InNativeCall()const{return bool(pending_);}
    bool NativeProofWindow(const State& state)const{
        // Frontend covers can synchronously complete inside their constructor,
        // before Begin returns. Accept only this exact in-flight Begin (whose
        // Prepare/geometry guards already passed), not an arbitrary native UI
        // callback during Prepare, Commit, Abort or a replacement operation.
        return Current(state)&&(began_||(pending_.action==NativeAction::Begin&&Same(pending_.spec,state.Current())));
    }
    NativeRequest Next(State& state,const NativeGuard& guard,uint32_t now){
        // A native callback can pump UI messages. Do not start work for a new
        // session reentrantly while the old native stack is still executing.
        if(pending_)return {};
        if(!Current(state)){
            Reset();session_=state.Session();ticket_=state.Ticket();nonce_=state.Nonce();
            operation_=state.Operation();request_=state.RequestId();
        }
        if(!state.Active())return {};
        if(!(state.Phase()&Go))state.Presence(guard.context&&guard.ownAtStart,now);
        if(!(state.Phase()&Commit)&&(!guard.context||guard.busy))state.Fail(4,now);
        // A fresh Go must not start a native mission using an older advertised
        // AtStart flag after either real vehicle has already left the polygon.
        if((state.Phase()&Go)&&!began_&&(!guard.ownAtStart||!guard.peerAtStart))state.Fail(4,now);
        if((state.Phase()&Go)&&!(state.Phase()&Commit)&&!state.Fresh(now))state.Fail(4,now);
        if(state.Terminal())return aborted_?NativeRequest{}:Issue(state,NativeAction::Abort);
        // Client cancellation is a request, not a unilateral commit/abort
        // decision. Host may already have durably committed both Done proofs.
        // Do not clear its owed intent or continue native steps while deciding.
        if(state.AwaitingFailureDecision())return {};
        // Commit is an immutable authorization. Once received, settling this
        // player's own durable receipt does not require a still-connected peer.
        // The adapter must verify its local owner and perform/retry no payout
        // twice; Wait means "retry this same receipt", not "grant it again".
        if(state.Phase()&Commit)return committed_?NativeRequest{}:Issue(state,NativeAction::Commit);
        if(!state.Fresh(now))return {};
        if((state.Phase()&Prepare)&&!prepared_){
            if(!guard.eligible){state.Fail(4,now);return Issue(state,NativeAction::Abort);}
            if(!guard.ownAtStart||!guard.peerAtStart)return {};
            return Issue(state,NativeAction::Prepare);
        }
        if((state.Phase()&Go)&&prepared_&&!began_){
            // For untimed covers, their admitted starting area IS their
            // destination. Set this before a native view constructor can emit
            // its genuine completion proof synchronously.
            if(!Timed(state.Current().kind))state.MarkArrived(now);
            return Issue(state,NativeAction::Begin);
        }
        if((state.Phase()&Release)&&began_&&!released_)return Issue(state,NativeAction::Release);
        return {};
    }
    bool Complete(State& state,const NativeRequest& request,NativeResult result,uint32_t now){
        if(!pending_||request.action!=pending_.action||request.serial!=pending_.serial||
            request.session!=pending_.session||request.ticket!=pending_.ticket||request.nonce!=pending_.nonce||
            request.operation!=pending_.operation||request.request!=pending_.request||!Same(request.spec,pending_.spec))return false;
        pending_={};
        if(!Current(state))return false;
        if(result==NativeResult::Wait)return true;
        if(result==NativeResult::Refused){
            // A committed transaction cannot be turned into failure/replayed
            // as a new reward. Leave it pending for explicit receipt recovery.
            if(request.action!=NativeAction::Commit)state.Fail(4,now);
            return true;
        }
        switch(request.action){
        case NativeAction::Prepare:prepared_=true;state.MarkArmed(now);break;
        case NativeAction::Begin:began_=true;break;
        case NativeAction::Release:released_=true;break;
        case NativeAction::Commit:committed_=true;state.MarkSaved(now);break;
        case NativeAction::Abort:aborted_=true;state.MarkSaved(now);break;
        default:return false;
        }
        return true;
    }
};
} // namespace coop_special
