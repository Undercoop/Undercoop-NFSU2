// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopTime.h"
#include <array>
#include <cstdint>
#include <cstring>

// Two-player mission agreement. No pointers, balances, chosen car parts or
// cross-machine wall-clock deadlines are accepted from the wire.
namespace coop_special {
constexpr uint32_t kMagic=0x4553324E,kRetryMs=200,kFreshMs=3000; // N2SE
enum class Kind:uint8_t {Show=1,Visual=2,Performance=3,Photo=4,Frontend=5};
struct Spec {
    uint32_t id=0,start=0,destination=0,durationMs=0;
    Kind kind=Kind::Show;uint8_t stage=0;uint16_t reserved=0;
};
struct Context{uint32_t campaign=0,catalog=0;uint8_t stage=0;};
inline bool Same(const Spec& a,const Spec& b){return a.id==b.id&&a.start==b.start&&a.destination==b.destination&&a.durationMs==b.durationMs&&a.kind==b.kind&&a.stage==b.stage&&a.reserved==b.reserved;}
inline bool Same(const Context& a,const Context& b){return a.campaign==b.campaign&&a.catalog==b.catalog&&a.stage==b.stage;}
inline bool Timed(Kind k){return k==Kind::Show||k==Kind::Visual||k==Kind::Performance;}
inline bool Valid(const Context& c){return c.campaign&&c.catalog&&c.stage<8;}
inline bool Valid(const Spec& s){return s.id&&s.start&&s.destination&&s.stage<8&&!s.reserved&&
    (Timed(s.kind)?s.durationMs>=1000&&s.durationMs<=3600000:(s.kind==Kind::Photo||s.kind==Kind::Frontend)&&!s.durationMs);}
enum Flag:uint16_t {Accepted=1,Armed=2,Arrived=4,Done=8,Saved=16,AtStart=32};
enum Authority:uint16_t {Prepare=1,Go=2,Release=4,Commit=8,Failed=16,Declined=32,Complete=64};
#pragma pack(push,1)
struct Packet{
    uint32_t magic=kMagic;uint16_t version=1,size=80;uint64_t session=0;
    uint32_t ticket=0,nonce=0,echo=0,sequence=0,campaign=0,catalog=0;
    uint32_t operation=0,request=0,remainingMs=0;
    Spec spec{};uint16_t flags=0,authority=0;uint8_t player=0,initiator=0,reason=0,stage=0;
};
#pragma pack(pop)
static_assert(sizeof(Packet)==80,"Special mission wire must stay bounded");
inline bool Valid(const Packet& p){
    if(p.magic!=kMagic||p.version!=1||p.size!=sizeof(Packet)||!p.session||!p.ticket||!p.nonce||!p.sequence||
        !p.campaign||!p.catalog||p.stage>=8||(p.player!=1&&p.player!=2)||(p.flags&~63u)||(p.authority&~127u)||p.reason>4)return false;
    if(!p.operation&&!p.request)return !p.flags&&!p.authority&&!p.initiator&&Same(p.spec,Spec{})&&!p.remainingMs&&!p.reason;
    if(!Valid(p.spec)||p.spec.stage!=p.stage||(p.initiator!=1&&p.initiator!=2)||p.remainingMs>p.spec.durationMs)return false;
    if(!p.operation&&(p.player!=2||p.initiator!=2||p.authority||(p.flags&~(Accepted|AtStart))))return false;
    if(p.player==2&&p.authority)return false;
    if((p.flags&Armed)&&!(p.flags&Accepted))return false;
    if((p.flags&Arrived)&&!(p.flags&Armed))return false;
    if((p.flags&Done)&&!(p.flags&Arrived))return false;
    if((p.flags&Saved)&&!(p.flags&Done)&&!p.reason&&!(p.authority&(Failed|Declined)))return false;
    if((p.authority&Go)&&!(p.authority&Prepare))return false;
    if((p.authority&Release)&&!(p.authority&Go))return false;
    if((p.authority&Commit)&&!(p.authority&Release))return false;
    if((p.authority&Complete)&&!(p.authority&(Commit|Failed|Declined)))return false;
    if((p.authority&Commit)&&(p.authority&(Failed|Declined)))return false;
    if((p.authority&(Failed|Declined))==(Failed|Declined))return false;
    return true;
}
class State{
    uint64_t session_=0;uint32_t ticket_=0,nonce_=0,peerNonce_=0,sequence_=0,peerSequence_=0,peerAt_=0,lastSent_=0;
    uint32_t operation_=0,request_=0,nextOperation_=0,nextRequest_=0,lastHostOperation_=0;
    uint32_t remaining_=0;uint16_t own_=0,peer_=0,authority_=0;uint8_t player_=0,initiator_=0,ownReason_=0,peerReason_=0;
    bool seen_=false,sent_=false,confirmed_=false,peerContextMatch_=false,offlineRecovery_=false;Context context_{};Spec spec_{};
    std::array<uint32_t,8> retired_{};unsigned retiredCount_=0;
    void Reset(){operation_=request_=remaining_=0;own_=peer_=authority_=0;initiator_=ownReason_=peerReason_=0;spec_={};}
    void Advance(uint32_t now){
        if(player_!=1||!operation_||(authority_&Commit)){
            if(player_==1&&(authority_&Commit)&&(own_&Saved)&&(peer_&Saved))authority_|=Complete;
            return;
        }
        if(ownReason_||peerReason_){authority_|=(authority_&Go)?Failed:Declined;}
        if(authority_&(Failed|Declined)){if((own_&Saved)&&(peer_&Saved))authority_|=Complete;return;}
        if(!Fresh(now))return;
        if((own_&Accepted)&&(peer_&Accepted)&&(own_&AtStart)&&(peer_&AtStart))authority_|=Prepare;
        if((authority_&Prepare)&&(own_&Armed)&&(peer_&Armed)&&(own_&AtStart)&&(peer_&AtStart))authority_|=Go;
        if((authority_&Go)&&(!Timed(spec_.kind)||((own_&Arrived)&&(peer_&Arrived))))authority_|=Release;
        if((authority_&Release)&&(own_&Done)&&(peer_&Done))authority_|=Commit;
    }
public:
    void Bind(uint64_t session,uint32_t ticket,uint8_t player,uint32_t nonce){*this=State{};session_=session;ticket_=ticket;player_=player;nonce_=nonce;}
    void Unbind(){*this=State{};}
    // LOCAL ONLY. The adapter must first validate its OWN durable Authorized
    // receipt. This does not accept network evidence or grant a reward; it
    // restores the immutable OLD commit scope for fresh native own replay.
    // Such a state can NEVER emit/receive gameplay packets in a new session.
    bool RestoreAuthorized(uint64_t session,uint32_t ticket,uint8_t player,uint32_t hostNonce,uint32_t clientNonce,
        uint32_t operation,Context context,const Spec& spec){
        if(!session||!ticket||(player!=1&&player!=2)||!hostNonce||!clientNonce||!operation||
            !Valid(context)||!Valid(spec)||spec.stage!=context.stage)return false;
        const uint32_t own=player==1?hostNonce:clientNonce;
        if(Active()&&(session_!=session||ticket_!=ticket||player_!=player||nonce_!=own||operation_!=operation||
            !Same(context_,context)||!Same(spec_,spec)))return false;
        *this=State{};session_=session;ticket_=ticket;player_=player;nonce_=own;peerNonce_=player==1?clientNonce:hostNonce;
        operation_=operation;context_=context;spec_=spec;initiator_=1;
        own_=peer_=Accepted|Armed|Arrived|Done;authority_=Prepare|Go|Release|Commit;offlineRecovery_=true;return true;
    }
    bool OfflineRecovery()const{return offlineRecovery_;}
    // LOCAL ONLY, after exact authenticated old Abort evidence. This cannot
    // transmit on a new session or turn any existing Commit into failure.
    bool RestoreAbort(uint64_t session,uint32_t ticket,uint8_t player,uint32_t hostNonce,uint32_t clientNonce,
        uint32_t operation,Context context,const Spec& spec){
        if(Active()&&(authority_&Commit))return false;
        if(!RestoreAuthorized(session,ticket,player,hostNonce,clientNonce,operation,context,spec))return false;
        authority_=Prepare|Go|Release|Failed;ownReason_=3;return true;
    }
    uint64_t Session()const{return session_;}uint32_t Ticket()const{return ticket_;}uint32_t Nonce()const{return nonce_;}
    uint32_t Operation()const{return operation_;}uint32_t RequestId()const{return request_;}uint8_t Player()const{return player_;}
    uint32_t PeerNonce()const{return peerNonce_;}
    uint16_t Phase()const{return authority_;}uint16_t Own()const{return own_;}const Spec& Current()const{return spec_;}
    const Context& CurrentContext()const{return context_;}
    bool Active()const{return (operation_||request_)&&!(authority_&Complete);}
    bool Fresh(uint32_t now)const{return seen_&&confirmed_&&peerContextMatch_&&coop_time::Elapsed(now,peerAt_)<=kFreshMs;}
    bool AwaitingDecision()const{return Active()&&!(own_&Accepted)&&!(authority_&(Failed|Declined|Commit));}
    bool LocalSaved()const{return (own_&Saved)!=0;}bool Terminal()const{return (authority_&(Failed|Declined))!=0;}
    bool AwaitingFailureDecision()const{return player_==2&&ownReason_&&!(authority_&(Commit|Failed|Declined));}
    bool SetContext(Context c){if(Active())return Same(context_,c);if(!Valid(c))c={};if(!Same(context_,c)){Reset();context_=c;peerContextMatch_=false;}return Valid(c);}
    bool Propose(const Spec& s,uint32_t now){
        if(Active()||!Valid(context_)||!Valid(s)||s.stage!=context_.stage||!Fresh(now))return false;
        Reset();spec_=s;initiator_=player_;own_=Accepted;remaining_=s.durationMs;
        if(player_==1){if(nextOperation_==UINT32_MAX)return false;operation_=++nextOperation_;}
        else {if(nextRequest_==UINT32_MAX)return false;request_=++nextRequest_;}
        return true;
    }
    bool Decide(bool accept,uint32_t now){if(!AwaitingDecision())return false;if(accept)own_|=Accepted;else ownReason_=1;Advance(now);return true;}
    void Presence(bool inside,uint32_t now){if(inside)own_|=AtStart;else own_&=~AtStart;Advance(now);}
    void MarkArmed(uint32_t now){if(authority_&Prepare)own_|=Armed;Advance(now);}
    void MarkArrived(uint32_t now){if(authority_&Go)own_|=Arrived;Advance(now);}
    void MarkDone(uint32_t now){if((authority_&Release)&&(own_&Arrived))own_|=Done;Advance(now);}
    void MarkSaved(uint32_t now){if((authority_&Commit)||Terminal()){own_|=Saved;if(Terminal()&&player_==2&&!ownReason_)ownReason_=3;}Advance(now);}
    void Fail(uint8_t reason,uint32_t now){if(Active()&&!(authority_&Commit)){ownReason_=reason?reason:2;Advance(now);}}
    void Clock(uint32_t remaining,uint32_t now){if(player_==1&&(authority_&Go)&&!(authority_&(Release|Failed|Commit))&&remaining<=remaining_){remaining_=remaining;if(!remaining)Fail(2,now);}}
    uint32_t Remaining()const{return remaining_;}
    bool Poll(uint32_t now,Packet& p){
        if(offlineRecovery_||!session_||!ticket_||!nonce_||!Valid(context_)||(sent_&&coop_time::Elapsed(now,lastSent_)<kRetryMs))return false;
        p={};p.size=sizeof(Packet);p.session=session_;p.ticket=ticket_;p.nonce=nonce_;p.echo=peerNonce_;if(!++sequence_)++sequence_;p.sequence=sequence_;
        p.campaign=context_.campaign;p.catalog=context_.catalog;p.stage=context_.stage;p.player=player_;
        if(operation_||request_){p.operation=operation_;p.request=request_;p.spec=spec_;p.initiator=initiator_;p.flags=own_;p.reason=ownReason_;p.remainingMs=remaining_;if(player_==1)p.authority=authority_;}
        sent_=true;lastSent_=now;return true;
    }
    bool Receive(const Packet& p,uint32_t receivedAt){
        if(offlineRecovery_||!Valid(p)||p.session!=session_||p.ticket!=ticket_||p.player==player_||!nonce_)return false;
        for(unsigned i=0;i<retiredCount_;++i)if(retired_[i]==p.nonce)return false;
        if(peerNonce_&&peerNonce_!=p.nonce){if(p.echo!=nonce_||retiredCount_==retired_.size())return false;
            retired_[retiredCount_++]=peerNonce_;peerSequence_=0;confirmed_=false;if(Active())ownReason_=4;}
        if(peerNonce_!=p.nonce){peerNonce_=p.nonce;peerSequence_=0;}
        if(seen_&&peerSequence_&&int32_t(p.sequence-peerSequence_)<=0)return false;
        peerSequence_=p.sequence;peerAt_=receivedAt;seen_=true;confirmed_=p.echo==nonce_;peerContextMatch_=false;
        if(!confirmed_||p.campaign!=context_.campaign||p.catalog!=context_.catalog||p.stage!=context_.stage)return true;
        if(!p.operation&&!p.request){peerContextMatch_=!Active();return true;}
        if(player_==1){
            if(!p.operation){if(!Active()&&p.request){if(nextOperation_==UINT32_MAX)return false;Reset();spec_=p.spec;request_=p.request;initiator_=2;remaining_=spec_.durationMs;operation_=++nextOperation_;}
                else if(request_!=p.request||initiator_!=2)return false;}
            if(p.operation&&p.operation!=operation_)return false;
        }else if(p.operation!=operation_){
            if(!p.operation||p.operation<=lastHostOperation_||(operation_&&Active()))return false;
            const bool requested=request_&&p.request==request_&&p.initiator==2&&Same(p.spec,spec_);
            Reset();spec_=p.spec;operation_=p.operation;request_=p.request;initiator_=p.initiator;lastHostOperation_=p.operation;
            if(requested)own_=Accepted;
        }
        if(!Same(p.spec,spec_)||p.request!=request_||p.initiator!=initiator_)return false;
        // All proofs except current position are monotonic in one operation.
        // In particular, a newer packet must not undo Commit after a native
        // payout/receipt has already started or erase a previous Done/Saved.
        if((peer_&uint16_t(Accepted|Armed|Arrived|Done|Saved))&~p.flags)return false;
        if(player_==2&&(authority_&~p.authority))return false;
        if(player_==1){
            if(((p.flags&Armed)&&!(authority_&Prepare))||((p.flags&Arrived)&&!(authority_&Go))||
               ((p.flags&Done)&&!(authority_&Release))||((p.flags&Saved)&&!(authority_&(Commit|Failed|Declined))))return false;
        }else if(!(p.authority&(Failed|Declined))){
            if(((p.authority&Prepare)&&!(own_&Accepted))||((p.authority&Go)&&!(own_&Armed))||
               ((p.authority&Release)&&Timed(spec_.kind)&&!(own_&Arrived))||
               ((p.authority&Commit)&&!(own_&Done))||((p.authority&Complete)&&!(own_&Saved)))return false;
        }
        peerContextMatch_=true;
        peer_=p.flags;peerReason_=p.reason;if(player_==2){authority_=p.authority;remaining_=p.remainingMs;}
        Advance(receivedAt);return true;
    }
};
} // namespace coop_special
