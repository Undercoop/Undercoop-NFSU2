// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopTime.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Pointer-free sponsor agreement. The game thread owns State and native work.
// Transport authenticates the joined endpoint before passing a packet here.
namespace coop_sponsor {
constexpr uint32_t kMagic=0x5053324E,kRetryMs=200; // N2SP; never the NC2S session wire.
constexpr size_t kMaximumOptions=8;
struct Context {
    uint32_t campaign=0,stage=0,catalog=0,count=0;
    std::array<uint32_t,kMaximumOptions> options{};
};
inline bool Same(const Context& a,const Context& b){return a.campaign==b.campaign && a.stage==b.stage &&
    a.catalog==b.catalog && a.count==b.count && a.options==b.options;}
inline bool Valid(const Context& c){
    if(!c.campaign || c.stage>=8 || !c.catalog || !c.count || c.count>kMaximumOptions)return false;
    for(size_t i=0;i<kMaximumOptions;++i){
        if(i>=c.count){if(c.options[i])return false;continue;}
        if(!c.options[i])return false;for(size_t j=0;j<i;++j)if(c.options[j]==c.options[i])return false;
    }return true;
}
inline bool ContainsOffer(const Context& c,uint32_t offers,uint32_t option){
    if(!option || offers>c.count || offers>c.options.size())return false;
    for(size_t i=0;i<offers;++i)if(c.options[i]==option)return true;
    return false;
}
enum Flags:uint16_t { Proposed=1,Approved=2,Declined=4,Commit=8,Applied=16 };
constexpr uint16_t kVersion=2;
#pragma pack(push,1)
struct Packet {
    uint32_t magic=kMagic;uint16_t version=kVersion,size=sizeof(Packet);
    uint64_t session=0;uint32_t ticket=0,senderNonce=0,recipientNonce=0,sequence=0;
    uint16_t player=0,flags=0;Context context{};float previousRating=0;uint32_t offers=0,proposal=0,option=0;
};
#pragma pack(pop)
static_assert(sizeof(Packet)<256,"Sponsor protocol must remain a bounded datagram");
inline bool Valid(const Packet& p){
    if(p.magic!=kMagic || p.version!=kVersion || p.size!=sizeof(Packet) || !p.session || !p.ticket ||
       !p.senderNonce || !p.sequence || (p.player!=1 && p.player!=2) || (p.flags&~31u) || !Valid(p.context))return false;
    if(!std::isfinite(p.previousRating) || p.offers>p.context.count || (p.flags && (!p.recipientNonce || !p.proposal ||
        !ContainsOffer(p.context,p.offers,p.option))))return false;
    if(!p.flags && (p.proposal || p.option))return false;
    if((p.flags&Approved) && (p.flags&Declined))return false;
    if((p.flags&(Commit|Applied)) && !(p.flags&Approved))return false;
    if((p.flags&Applied) && !(p.flags&Commit))return false;
    return !p.flags || (p.flags&Proposed)!=0;
}
class State {
public:
    void Bind(uint64_t session,uint32_t ticket,bool host,uint32_t nonce){
        if(session_==session && ticket_==ticket && nonce_==nonce && host_==host)return;
        *this=State{};session_=session;ticket_=ticket;host_=host;nonce_=nonce;
    }
    void Unbind(){*this=State{};}
    uint64_t Session()const{return session_;}uint32_t Ticket()const{return ticket_;}
    bool IsHost()const{return host_;}bool Ready()const{return ready_;}
    const Context& Current()const{return context_;}
    bool HasHostOffers()const{return Valid(context_) && (host_ || (ready_ && Same(context_,peerContext_)));}
    uint32_t OfferCount()const{return HasHostOffers()?offers_:0;}
    float HostPreviousRating()const{return previousRating_;}
    bool Offers(uint32_t option)const{return ContainsOffer(context_,OfferCount(),option);}
    uint32_t Proposal()const{return proposal_;}uint32_t Option()const{return option_;}
    bool Pending()const{return proposal_ && !declined_ && !committed_;}
    bool AwaitingDecision()const{return !host_ && Pending() && !approved_;}
    bool Rejected()const{return declined_;}bool Committed()const{return committed_;}
    bool LocalApplied()const{return applied_;}bool Complete()const{return applied_ && peerApplied_;}
    bool DueCommit()const{return committed_ && approved_ && !declined_ && !applied_ && Valid(context_);}
    void SetContext(const Context& context){
        if(Same(context_,context))return;
        context_=Valid(context)?context:Context{};ResetAgreement();peerContext_={};offers_=peerOffers_=0;previousRating_=0;
    }
    void SetHostOffers(uint32_t offers,float previousRating=0){
        if(host_ && !proposal_ && offers<=context_.count && std::isfinite(previousRating)){
            offers_=offers;previousRating_=previousRating;
        }
    }
    bool Propose(uint32_t option){
        if(!host_ || !ready_ || !Same(context_,peerContext_) || offers_!=peerOffers_ ||
           !Offers(option) || committed_ || Pending())return false;
        if(nextProposal_==UINT32_MAX)return false;
        ResetAgreement();proposal_=++nextProposal_;option_=option;return true;
    }
    bool Decide(bool approve){
        if(!AwaitingDecision())return false;approved_=approve;declined_=!approve;return true;
    }
    void MarkApplied(){if(DueCommit())applied_=true;}
    bool Poll(uint32_t now,Packet& p){
        if(!session_ || !ticket_ || !nonce_ || !Valid(context_) || (sent_ && coop_time::Elapsed(now,lastSent_)<kRetryMs))return false;
        sent_=true;lastSent_=now;p={};p.session=session_;p.ticket=ticket_;p.senderNonce=nonce_;
        p.recipientNonce=peerNonce_;p.player=host_?1:2;p.sequence=++sequence_;if(!p.sequence)p.sequence=++sequence_;
        p.context=context_;p.offers=offers_;p.previousRating=previousRating_;
        if(ready_ && proposal_){p.proposal=proposal_;p.option=option_;p.flags=Proposed |
            (approved_?Approved:0)|(declined_?Declined:0)|(committed_?Commit:0)|(applied_?Applied:0);}
        return true;
    }
    bool Receive(const Packet& p){
        if(!Valid(p) || p.session!=session_ || p.ticket!=ticket_ || p.player==(host_?1:2))return false;
        for(uint32_t retired:retired_)if(retired && retired==p.senderNonce)return false;
        if(p.recipientNonce && p.recipientNonce!=nonce_){
            // A newly bound peer can learn the established sender from its
            // hello, then echo that sender's nonce in its own fresh hello.
            if(!ready_){peerNonce_=p.senderNonce;return true;}
            return false;
        }
        if(p.senderNonce!=peerNonce_){
            // A peer restart must first echo this binding's fresh nonce. A
            // delayed unaddressed hello cannot replace an established peer.
            if(ready_ && p.recipientNonce!=nonce_)return false;
            if(peerNonce_ && ready_){
                // Never evict an old incarnation and make its delayed commit
                // eligible again. A long sequence of process restarts needs a
                // fresh session/ticket once this bounded retirement set fills.
                if(retiredNext_==retired_.size())return false;
                retired_[retiredNext_++]=peerNonce_;ResetAgreement();peerContext_={};peerOffers_=0;lastHostProposal_=0;
                if(!host_){offers_=0;previousRating_=0;}
            }
            peerNonce_=p.senderNonce;lastSequence_=0;ready_=false;
        }
        if(lastSequence_ && int32_t(p.sequence-lastSequence_)<=0)return false;
        lastSequence_=p.sequence;
        if(p.recipientNonce==nonce_)ready_=true;
        if(!ready_)return true;
        peerContext_=p.context;peerOffers_=p.offers;
        if(!Same(context_,p.context))return true;
        if(!host_){
            if(proposal_ && (p.offers!=offers_ || p.previousRating!=previousRating_))return false;
            offers_=p.offers;previousRating_=p.previousRating;
        }
        if(!p.flags)return true;
        if(host_){
            if(p.proposal!=proposal_ || p.option!=option_ || !proposal_ || p.offers!=offers_)return false;
            if(p.flags&Declined){if(!committed_)declined_=true;return true;}
            if((p.flags&Approved) && !declined_){approved_=true;committed_=true;}
            if(committed_ && (p.flags&Applied))peerApplied_=true;
        }else{
            if(p.proposal!=proposal_){
                if(p.proposal<=lastHostProposal_ || (proposal_ && !declined_ && !Complete()))return false;
                ResetAgreement();proposal_=p.proposal;lastHostProposal_=p.proposal;option_=p.option;
            }
            if(p.option!=option_)return false;
            if((p.flags&Commit) && approved_ && !declined_)committed_=true;
            if(committed_ && (p.flags&Applied))peerApplied_=true;
        }return true;
    }
private:
    void ResetAgreement(){proposal_=option_=0;approved_=declined_=committed_=applied_=peerApplied_=false;}
    uint64_t session_=0;uint32_t ticket_=0,nonce_=0,peerNonce_=0,sequence_=0,lastSequence_=0;
    uint32_t proposal_=0,option_=0,nextProposal_=0,lastHostProposal_=0,lastSent_=0,offers_=0,peerOffers_=0;
    float previousRating_=0;
    bool host_=false,ready_=false,sent_=false,approved_=false,declined_=false,committed_=false,applied_=false,peerApplied_=false;
    Context context_{},peerContext_{};std::array<uint32_t,8> retired_{};size_t retiredNext_=0;
};
}
