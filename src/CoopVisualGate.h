// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopTime.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Only current-car stars are observed. No garage, money, performance-rating,
// reward or unlock bits are transported. Native thresholds remain authoritative.
namespace coop_visual {
constexpr uint32_t kMagic=0x5653324E; // N2SV, distinct from all other protocols.
constexpr uint32_t kFreshMs=3000,kHeartbeatMs=500;
#pragma pack(push,1)
struct Packet {
    uint32_t magic=kMagic;uint16_t version=1,bytes=52;
    uint64_t session=0;
    uint32_t ticket=0,campaign=0,catalog=0,nonce=0,echo=0,sequence=0,slot=0;
    float stars=0;
    uint8_t stage=0,player=0,valid=0,reserved=0;
};
#pragma pack(pop)
static_assert(sizeof(Packet)==52,"Bounded visual-star heartbeat ABI");
struct Context {uint32_t campaign=0,catalog=0;uint8_t stage=0;};
struct Sample {float stars=0;uint32_t slot=0;bool valid=false;};
inline bool ValidStars(float stars){return std::isfinite(stars)&&stars>=0&&stars<=10;}
inline bool Valid(const Context& c){return c.campaign&&c.catalog&&c.stage<8;}
inline bool Same(const Context& a,const Context& b){return a.campaign==b.campaign&&a.catalog==b.catalog&&a.stage==b.stage;}
inline bool Valid(const Packet& p){
    return p.magic==kMagic&&p.version==1&&p.bytes==sizeof(Packet)&&p.session&&p.ticket&&
        p.campaign&&p.catalog&&p.stage<8&&p.nonce&&p.sequence&&(p.player==1||p.player==2)&&
        p.valid<=1&&!p.reserved&&ValidStars(p.stars)&&(p.valid?p.slot!=0:(!p.slot&&p.stars==0));
}
class State {
    uint64_t session_=0;uint32_t ticket_=0,nonce_=0,peerNonce_=0,sequence_=0,peerSequence_=0;
    uint32_t peerAt_=0,lastPoll_=0;uint8_t player_=0;
    bool peerSeen_=false,peerConfirmed_=false,dirty_=false,polled_=false;
    Context context_{},peerContext_{};Sample sample_{},peerSample_{};
    std::array<uint32_t,8> retired_{};unsigned retiredCount_=0;
public:
    void Bind(uint64_t session,uint32_t ticket,uint8_t player,uint32_t nonce){
        *this=State{};session_=session;ticket_=ticket;player_=player;nonce_=nonce;
    }
    void Unbind(){*this=State{};}
    uint64_t Session()const{return session_;}uint32_t Ticket()const{return ticket_;}
    uint32_t Nonce()const{return nonce_;}uint32_t PeerNonce()const{return peerNonce_;}
    const Context& Current()const{return context_;}
    void SetLocal(Context c,Sample s){
        if(!Valid(c))c={};
        if(!s.valid||!ValidStars(s.stars)||!s.slot)s={};
        if(!Same(context_,c)||sample_.valid!=s.valid||sample_.slot!=s.slot||sample_.stars!=s.stars)dirty_=true;
        context_=c;sample_=s;
    }
    bool Receive(const Packet& p,uint32_t now){
        if(!Valid(p)||p.session!=session_||p.ticket!=ticket_||p.player==player_||!nonce_)return false;
        for(unsigned i=0;i<retiredCount_;++i)if(retired_[i]==p.nonce)return false;
        if(peerNonce_&&peerNonce_!=p.nonce){
            // A restarted peer must have observed this incarnation's nonce.
            // Retired incarnations can never become current again by reordering.
            if(p.echo!=nonce_||retiredCount_==retired_.size())return false;
            retired_[retiredCount_++]=peerNonce_;peerSequence_=0;peerSeen_=false;peerConfirmed_=false;
        }
        if(peerNonce_!=p.nonce){peerNonce_=p.nonce;dirty_=true;}
        if(peerSeen_&&static_cast<int32_t>(p.sequence-peerSequence_)<=0)return false;
        peerSequence_=p.sequence;peerAt_=now;peerSeen_=true;peerConfirmed_=p.echo==nonce_;
        peerContext_={p.campaign,p.catalog,p.stage};peerSample_={p.stars,p.slot,p.valid!=0};
        return true;
    }
    bool Poll(uint32_t now,Packet& p){
        if(!session_||!ticket_||!nonce_||(player_!=1&&player_!=2)||!Valid(context_))return false;
        if(polled_&&!dirty_&&coop_time::Elapsed(now,lastPoll_)<kHeartbeatMs)return false;
        p={};p.session=session_;p.ticket=ticket_;p.player=player_;p.nonce=nonce_;p.echo=peerNonce_;
        if(!++sequence_)++sequence_;p.sequence=sequence_;p.campaign=context_.campaign;p.catalog=context_.catalog;
        p.stage=context_.stage;p.valid=sample_.valid?1:0;p.slot=sample_.slot;p.stars=sample_.stars;
        lastPoll_=now;polled_=true;dirty_=false;return true;
    }
    bool Effective(const Context& c,float localStars,uint32_t now,float& minimum)const{
        // Car stars do not depend on stage. A peer advancing one native frame
        // earlier must not deadlock the other's still-current stage import.
        if(!Valid(c)||!Same(c,context_)||!ValidStars(localStars)||!sample_.valid||
            !peerSeen_||!peerConfirmed_||coop_time::Elapsed(now,peerAt_)>kFreshMs||!peerSample_.valid||
            peerContext_.campaign!=c.campaign||peerContext_.catalog!=c.catalog)return false;
        minimum=(std::min)(localStars,peerSample_.stars);return true;
    }
};
} // namespace coop_visual
