// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopTime.h"
#include <array>
#include <cstdint>
#include <cstring>

// Bounded old-operation decision exchange, independent of files/the engine.
// The transport must first authenticate the CURRENT joined peer endpoint.
// Receive never reads a receipt, grants a reward or performs native work.
// A new Host cannot invent the old Host's decision: Resolve requires evidence
// from a durable ORIGINAL-coordinator record with both saved old nonces.
namespace coop_special_recovery {
constexpr uint32_t kMagic=0x4453324E,kRetryMs=250,kFreshMs=3000; // N2SD
enum class Decision:uint8_t {Pending=0,Commit=1,Abort=2};
enum class Message:uint8_t {Hello=1,Query=2,Reply=3};
struct Context {uint32_t campaign=0,catalog=0;};
struct Key {
    uint64_t session=0;
    uint32_t ticket=0,operation=0,hostNonce=0,clientNonce=0,event=0;
    uint8_t stage=0,kind=0;uint16_t reserved=0;
};
inline bool Same(const Context& a,const Context& b){return a.campaign==b.campaign&&a.catalog==b.catalog;}
inline bool Same(const Key& a,const Key& b){return a.session==b.session&&a.ticket==b.ticket&&a.operation==b.operation&&
    a.hostNonce==b.hostNonce&&a.clientNonce==b.clientNonce&&a.event==b.event&&a.stage==b.stage&&a.kind==b.kind&&a.reserved==b.reserved;}
inline bool Valid(Context c){return c.campaign&&c.catalog;}
inline bool Valid(const Key& k){return k.session&&k.ticket&&k.operation&&k.hostNonce&&k.clientNonce&&k.event&&k.stage<8&&k.kind>=1&&k.kind<=5&&!k.reserved;}
inline bool Empty(const Key& k){return Same(k,Key{});}
#pragma pack(push,1)
struct Packet {
    uint32_t magic=kMagic;uint16_t version=1,size=84;
    uint64_t session=0;uint32_t ticket=0,nonce=0,echo=0,sequence=0;
    Context context{};Key old{};
    uint32_t request=0,beforeDigest=0;
    Message message=Message::Hello;Decision decision=Decision::Pending;uint8_t player=0,reserved=0;
};
#pragma pack(pop)
static_assert(sizeof(Context)==8&&sizeof(Key)==32&&sizeof(Packet)==84,"Recovery wire must have a fixed bounded layout");
inline bool Valid(const Packet& p){
    if(p.magic!=kMagic||p.version!=1||p.size!=sizeof(Packet)||!p.session||!p.ticket||!p.nonce||!p.sequence||
        !Valid(p.context)||(p.player!=1&&p.player!=2)||p.reserved||uint8_t(p.decision)>2)return false;
    if(p.message==Message::Hello)return Empty(p.old)&&!p.request&&!p.beforeDigest&&p.decision==Decision::Pending;
    if(!p.echo||!Valid(p.old)||!p.request)return false; // CRC zero is a valid fingerprint.
    return p.message==Message::Query?(p.player==2&&p.decision==Decision::Pending):
        p.message==Message::Reply&&p.player==1;
}
// Local-only work token. Serial/nonce fences prevent an asynchronous disk
// lookup from answering a replacement request/session after a UI reentry.
struct Request {
    Context context{};Key old{};
    uint64_t session=0;uint32_t ticket=0,hostNonce=0,clientNonce=0,id=0,beforeDigest=0,serial=0;
    explicit operator bool()const{return serial!=0;}
};
inline bool Valid(const Request& r){return bool(r)&&Valid(r.context)&&Valid(r.old)&&r.session&&r.ticket&&r.hostNonce&&r.clientNonce&&r.id;}
inline bool Same(const Request& a,const Request& b){return Same(a.context,b.context)&&Same(a.old,b.old)&&
    a.session==b.session&&a.ticket==b.ticket&&a.hostNonce==b.hostNonce&&a.clientNonce==b.clientNonce&&
    a.id==b.id&&a.beforeDigest==b.beforeDigest&&a.serial==b.serial;}
struct CoordinatorProof {Context context{};Key old{};Decision decision=Decision::Pending;uint8_t originalPlayer=0;};
struct Evidence {Request request{};Decision decision=Decision::Pending;explicit operator bool()const{return bool(request)&&decision!=Decision::Pending;}};
class State {
    uint64_t session_=0;uint32_t ticket_=0,nonce_=0,peerNonce_=0,sequence_=0,peerSequence_=0,peerAt_=0,lastSent_=0;
    uint32_t nextRequest_=0,nextSerial_=0;uint8_t player_=0;Context context_{};
    bool seen_=false,confirmed_=false,sent_=false,lookedUp_=false,helloNext_=false;
    std::array<uint32_t,8> retired_{};unsigned retiredCount_=0;
    Request request_{};Decision decision_=Decision::Pending;
    bool NextSerial(){if(nextSerial_==UINT32_MAX)return false;request_.serial=++nextSerial_;return true;}
    bool Matches(const Request& r)const{return r&&Same(r,request_)&&r.session==session_&&r.ticket==ticket_&&
        r.hostNonce==(player_==1?nonce_:peerNonce_)&&r.clientNonce==(player_==2?nonce_:peerNonce_);}
public:
    void Bind(uint64_t session,uint32_t ticket,uint8_t player,uint32_t nonce,Context context){
        *this=State{};if(!session||!ticket||(player!=1&&player!=2)||!nonce||!Valid(context))return;
        session_=session;ticket_=ticket;player_=player;nonce_=nonce;context_=context;
    }
    void Unbind(){*this=State{};}
    bool Bound()const{return session_&&ticket_&&nonce_&&Valid(context_);}
    bool Fresh(uint32_t now)const{return Bound()&&seen_&&confirmed_&&coop_time::Elapsed(now,peerAt_)<=kFreshMs;}
    bool Start(const Key& old,uint32_t beforeDigest,uint32_t now){
        if(player_!=2||!Fresh(now)||!Valid(old)||nextRequest_==UINT32_MAX||nextSerial_==UINT32_MAX)return false;
        if(request_)return Same(request_.old,old)&&request_.beforeDigest==beforeDigest;
        request_.context=context_;request_.old=old;request_.session=session_;request_.ticket=ticket_;
        request_.hostNonce=peerNonce_;request_.clientNonce=nonce_;request_.id=++nextRequest_;request_.beforeDigest=beforeDigest;
        NextSerial();decision_=Decision::Pending;lookedUp_=false;return true;
    }
    Request PendingLookup(uint32_t now)const{return player_==1&&Fresh(now)&&request_&&!lookedUp_?request_:Request{};}
    bool Resolve(const Request& token,const CoordinatorProof& proof,uint32_t now){
        if(player_!=1||!Fresh(now)||!Matches(token)||proof.originalPlayer!=1||
            !Same(proof.context,context_)||!Same(proof.old,request_.old)||
            (proof.decision!=Decision::Commit&&proof.decision!=Decision::Abort))return false;
        if(decision_!=Decision::Pending&&decision_!=proof.decision)return false;
        decision_=proof.decision;lookedUp_=true;return true;
    }
    bool Defer(const Request& token,uint32_t now){
        if(player_!=1||!Fresh(now)||!Matches(token)||decision_!=Decision::Pending)return false;
        // A missing/Prepared receipt is NOT evidence of abort. A repeated
        // query may request another game-thread lookup after retry cadence.
        lookedUp_=true;return true;
    }
    Evidence Result()const{return player_==2&&request_&&decision_!=Decision::Pending?Evidence{request_,decision_}:Evidence{};}
    bool Poll(uint32_t now,Packet& p){
        if(!Bound()||sequence_==UINT32_MAX||(sent_&&coop_time::Elapsed(now,lastSent_)<kRetryMs))return false;
        p={};p.session=session_;p.ticket=ticket_;p.nonce=nonce_;p.echo=peerNonce_;p.sequence=++sequence_;p.context=context_;p.player=player_;
        if(request_&&Fresh(now)&&Matches(request_)&&!helloNext_){
            p.old=request_.old;p.request=request_.id;p.beforeDigest=request_.beforeDigest;
            p.message=player_==1?Message::Reply:Message::Query;p.decision=player_==1?decision_:Decision::Pending;
            helloNext_=true;
        }else helloNext_=false;
        // Even an outstanding request alternates with canonical Hello. A
        // replacement process cannot echo a nonce it has never learned; only
        // sending Replies to the retired process would deadlock recovery.
        sent_=true;lastSent_=now;return true;
    }
    bool Receive(const Packet& p,uint32_t now){
        State next=*this;if(!next.ReceiveInner(p,now))return false;*this=next;return true;
    }
private:
    bool ReceiveInner(const Packet& p,uint32_t now){
        if(!Bound()||!Valid(p)||p.session!=session_||p.ticket!=ticket_||p.player==player_||!Same(p.context,context_))return false;
        for(unsigned i=0;i<retiredCount_;++i)if(retired_[i]==p.nonce)return false;
        if(peerNonce_&&p.nonce!=peerNonce_){
            if(p.echo!=nonce_||retiredCount_==retired_.size())return false;
            // Client's authenticated decision stays owed after a responder
            // restart. Host's outbound cache is not a personal receipt: a
            // replacement Client must re-query if it missed the resolved Reply.
            if(player_==2&&decision_!=Decision::Pending)return false;
            retired_[retiredCount_++]=peerNonce_;peerSequence_=0;confirmed_=false;
            if(player_==1){request_={};decision_=Decision::Pending;}
            else if(request_){
                if(nextRequest_==UINT32_MAX||nextSerial_==UINT32_MAX)return false;
                request_.id=++nextRequest_;NextSerial();request_.hostNonce=p.nonce;
            }
            lookedUp_=false;
        }
        if(peerNonce_!=p.nonce){peerNonce_=p.nonce;peerSequence_=0;}
        if(peerSequence_&&int32_t(p.sequence-peerSequence_)<=0)return false;
        // A malformed replacement Query/Reply must not refresh liveness or
        // consume the sequence of the legitimate response that follows it.
        if(p.message==Message::Query){
            if(player_!=1||p.echo!=nonce_)return false;
            if(request_&&p.request==request_.id&&(!Same(p.old,request_.old)||p.beforeDigest!=request_.beforeDigest))return false;
            if(request_&&p.request!=request_.id&&int32_t(p.request-request_.id)<=0)return false;
            if(!request_||p.request!=request_.id){
                if(nextSerial_==UINT32_MAX)return false;
                request_={context_,p.old,session_,ticket_,nonce_,p.nonce,p.request,p.beforeDigest,0};NextSerial();
                decision_=Decision::Pending;lookedUp_=false;
            }else if(decision_==Decision::Pending)lookedUp_=false;
        }else if(p.message==Message::Reply){
            if(player_!=2||p.echo!=nonce_||!request_||p.nonce!=request_.hostNonce||
                p.request!=request_.id||p.beforeDigest!=request_.beforeDigest||!Same(p.old,request_.old))return false;
            if(decision_!=Decision::Pending&&p.decision!=decision_)return false;
            decision_=p.decision;
        }
        peerSequence_=p.sequence;peerAt_=now;seen_=true;confirmed_=p.echo==nonce_;return true;
    }
};
} // namespace coop_special_recovery
