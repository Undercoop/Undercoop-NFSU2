// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Pointer-free, bounded two-player chat. Callers authenticate the joined UDP
// endpoint before Receive; this is session binding, not cryptographic identity.
namespace coop_chat {
constexpr std::uint32_t kMagic=0x5443324e; // N2CT
constexpr std::uint16_t kVersion=1;
constexpr std::size_t kMaxTextBytes=240,kQueueSize=4,kEventSize=16;
constexpr std::uint32_t kMessageIntervalMs=500,kRetryMs=250;
constexpr std::uint32_t kCandidateTimeoutMs=5000;
enum class Kind : std::uint8_t { Hello=1,Data=2,Ack=3,Challenge=4,Ready=5 };
enum class EventKind : std::uint8_t { Message=1,Undelivered=2 };
#pragma pack(push,1)
struct Packet {
    std::uint32_t magic=kMagic;
    std::uint16_t version=kVersion,size=281;
    std::uint64_t session=0;
    std::uint32_t senderNonce=0,recipientNonce=0,sequence=0,ack=0;
    std::uint16_t player=0;
    Kind kind=Kind::Hello;
    std::uint8_t reserved=0;
    std::uint16_t length=0,reserved2=0;
    char text[kMaxTextBytes+1]{};
};
#pragma pack(pop)
static_assert(sizeof(Packet)==281,"Chat wire layout changed");
static_assert(sizeof(Packet)<1200,"Chat must fit one ordinary UDP datagram");
struct Event { EventKind kind=EventKind::Message;std::uint16_t player=0;char text[kMaxTextBytes+1]{}; };

inline bool ForbiddenCodepoint(std::uint32_t c) {
    // Reject line/control/format characters, including directional spoofing.
    return c<0x20 || (c>=0x7F&&c<=0x9F) || c==0xAD || c==0x61C ||
        (c>=0x200B&&c<=0x200F)||(c>=0x2028&&c<=0x202F)||
        (c>=0x2060&&c<=0x206F)||c==0xFEFF||(c>=0xFFF9&&c<=0xFFFB)||
        (c>=0xE0000&&c<=0xE007F);
}
inline bool ValidText(const char* text,std::size_t length) {
    if(!text||!length||length>kMaxTextBytes)return false;
    bool visible=false;
    for(std::size_t i=0;i<length;) {
        const auto first=static_cast<unsigned char>(text[i++]);std::uint32_t cp=0,minimum=0;unsigned extra=0;
        if(first<0x80)cp=first;
        else if(first>=0xC2&&first<=0xDF){cp=first&31;extra=1;minimum=0x80;}
        else if(first>=0xE0&&first<=0xEF){cp=first&15;extra=2;minimum=0x800;}
        else if(first>=0xF0&&first<=0xF4){cp=first&7;extra=3;minimum=0x10000;}
        else return false;
        if(i+extra>length)return false;
        for(unsigned j=0;j<extra;++j){const auto next=static_cast<unsigned char>(text[i++]);if((next&0xC0)!=0x80)return false;cp=(cp<<6)|(next&63);}
        if(cp<minimum||cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF)||ForbiddenCodepoint(cp))return false;
        if(cp!=0x20&&cp!=0xA0&&cp!=0x3000)visible=true;
    }
    return visible;
}
inline bool Valid(const Packet& p) {
    if(p.magic!=kMagic||p.version!=kVersion||p.size!=sizeof(Packet)||!p.session||
        !p.senderNonce||(p.player!=1&&p.player!=2)||p.reserved||p.reserved2)return false;
    if(p.kind!=Kind::Hello&&p.kind!=Kind::Data&&p.kind!=Kind::Ack&&p.kind!=Kind::Challenge&&p.kind!=Kind::Ready)return false;
    if(p.kind==Kind::Hello||p.kind==Kind::Challenge||p.kind==Kind::Ready) {if(p.sequence||p.ack||p.length)return false;}
    else if(!p.recipientNonce)return false;
    if((p.kind==Kind::Challenge||p.kind==Kind::Ready)&&!p.recipientNonce)return false;
    if(p.kind==Kind::Ack&&(p.sequence||p.length))return false;
    if(p.kind==Kind::Data&&(!p.sequence||!ValidText(p.text,p.length)))return false;
    for(std::size_t i=p.length;i<sizeof(p.text);++i)if(p.text[i])return false;
    return true;
}

class State {
public:
    void Bind(std::uint64_t session,std::uint16_t player,std::uint32_t nonce) {
        if(session_==session&&player_==player&&localNonce_==nonce)return;
        const bool pending=count_!=0;*this=State{};
        if(session&&nonce&&(player==1||player==2)){session_=session;player_=player;localNonce_=nonce;}
        if(pending)PushUndelivered();
    }
    void Unbind(){Bind(0,0,0);}
    bool Bound()const{return session_!=0;}
    bool Ready()const{return peerReady_&&!awaitingCommit_;}
    std::uint64_t Session()const{return session_;}
    std::uint16_t Player()const{return player_;}
    std::uint32_t Nonce()const{return localNonce_;}
    std::size_t Pending()const{return count_;}
    bool Submit(const char* text,std::uint32_t now) {
        if(!Bound()||!text||count_==kQueueSize||eventCount_==kEventSize||
            (submitted_&&now-lastSubmit_<kMessageIntervalMs)||nextSequence_==0)return false;
        std::size_t length=0;while(length<=kMaxTextBytes&&text[length])++length;
        if(!ValidText(text,length))return false;
        auto& item=queue_[count_++];item.sequence=nextSequence_++;item.length=static_cast<std::uint16_t>(length);
        std::memcpy(item.text,text,length);item.text[length]=0;
        Event event;event.player=player_;std::memcpy(event.text,item.text,length+1);events_[eventCount_++]=event;
        submitted_=true;lastSubmit_=now;return true;
    }
    bool Receive(const Packet& p,std::uint32_t now) {
        if(!Bound()||!Valid(p)||p.session!=session_||p.player==player_)return false;
        ExpireCandidate(now);
        if(candidateRemote_&&p.senderNonce==candidateRemote_&&p.recipientNonce==candidateLocal_&&
            (p.kind==Kind::Hello||p.kind==Kind::Data)) {
            // A fresh challenge echo proves a restarted peer actually received
            // this attempt; a delayed Hello alone never replaces live state.
            CommitCandidate();if(p.kind==Kind::Hello)return true;
        }
        if(p.kind==Kind::Challenge) {
            if(p.recipientNonce!=localNonce_||(peerReady_&&!awaitingCommit_&&p.senderNonce!=peerNonce_))return false;
            peerNonce_=p.senderNonce;awaitingCommit_=true;helloPending_=true;return true;
        }
        if(p.kind==Kind::Ready) {
            if(p.recipientNonce!=localNonce_||!peerNonce_||p.senderNonce!=peerNonce_)return false;
            peerReady_=true;awaitingCommit_=false;helloPending_=false;return true;
        }
        if(p.kind==Kind::Hello) {
            // An old broadcast hello must not poison a new binding before the
            // real peer echoes this binding's fresh local nonce.
            if(peerReady_&&peerNonce_!=p.senderNonce) {
                if(candidateRemote_&&candidateRemote_!=p.senderNonce)return false;
                if(!candidateRemote_) {
                    candidateRemote_=p.senderNonce;candidateAt_=now;candidateSent_=false;
                    if(!challengeSeed_)challengeSeed_=localNonce_;
                    do {challengeSeed_=challengeSeed_*1664525u+1013904223u;}while(!challengeSeed_||challengeSeed_==localNonce_);
                    candidateLocal_=challengeSeed_;
                }
                return true;
            }
            if(p.recipientNonce&&p.recipientNonce!=localNonce_)return false;
            peerNonce_=p.senderNonce;
            if(p.recipientNonce==localNonce_){peerReady_=true;awaitingCommit_=false;readyPending_=true;helloPending_=false;}
            else helloPending_=true;
            return true;
        }
        if(!peerNonce_||p.senderNonce!=peerNonce_||p.recipientNonce!=localNonce_)return false;
        peerReady_=true;awaitingCommit_=false;
        // Only the single currently transmitted head may be acknowledged.
        if(p.ack&&count_&&queue_[0].sent&&p.ack==queue_[0].sequence) {
            for(std::size_t i=1;i<count_;++i)queue_[i-1]=queue_[i];--count_;queue_[count_]={};
        }
        if(p.kind!=Kind::Data)return true;
        if(p.sequence==lastReceived_){ackPending_=true;return true;}
        if(lastReceived_==UINT32_MAX||p.sequence!=lastReceived_+1||eventCount_==kEventSize||
            (received_&&now-lastReceive_<kMessageIntervalMs))return false;
        Event event;event.player=p.player;std::memcpy(event.text,p.text,p.length+1);events_[eventCount_++]=event;
        received_=true;lastReceive_=now;lastReceived_=p.sequence;ackPending_=true;return true;
    }
    bool Poll(std::uint32_t now,Packet& p) {
        if(!Bound())return false;
        ExpireCandidate(now);
        p={};p.session=session_;p.player=player_;p.senderNonce=localNonce_;p.recipientNonce=peerNonce_;
        if(candidateRemote_&&(!candidateSent_||now-candidateLastSent_>=kRetryMs)) {
            p.kind=Kind::Challenge;p.senderNonce=candidateLocal_;p.recipientNonce=candidateRemote_;
            candidateSent_=true;candidateLastSent_=now;return true;
        }
        if(readyPending_){p.kind=Kind::Ready;readyPending_=false;return true;}
        // A matching echo is repeatedly sent until a peer packet proves it was
        // received. This lets either initial hello/echo be lost safely.
        if(!peerReady_||awaitingCommit_||helloPending_) {
            if(helloSent_&&now-lastHello_<kRetryMs)return false;
            p.kind=Kind::Hello;helloSent_=true;lastHello_=now;helloPending_=false;return true;
        }
        p.ack=lastReceived_;
        if(count_) {
            auto& item=queue_[0];
            if((item.sent&&now-item.lastSent>=kRetryMs)||(!item.sent&&(!dataSent_||now-lastData_<kMessageIntervalMs))) {
                p.kind=Kind::Data;p.sequence=item.sequence;p.length=item.length;std::memcpy(p.text,item.text,item.length+1);
                item.sent=true;item.lastSent=now;dataSent_=true;lastData_=now;ackPending_=false;return true;
            }
        }
        if(ackPending_){p.kind=Kind::Ack;ackPending_=false;return true;}
        return false;
    }
    bool Pop(Event& event) {
        if(!eventCount_)return false;event=events_[0];
        for(std::size_t i=1;i<eventCount_;++i)events_[i-1]=events_[i];--eventCount_;events_[eventCount_]={};return true;
    }
private:
    struct Item {std::uint32_t sequence=0,lastSent=0;std::uint16_t length=0;bool sent=false;char text[kMaxTextBytes+1]{};};
    void PushUndelivered(){Event event;event.kind=EventKind::Undelivered;events_[eventCount_++]=event;}
    void ExpireCandidate(std::uint32_t now) {
        if(candidateRemote_&&now-candidateAt_>=kCandidateTimeoutMs){candidateRemote_=candidateLocal_=0;candidateSent_=false;}
    }
    void CommitCandidate() {
        const auto session=session_;const auto player=player_;const auto local=candidateLocal_,remote=candidateRemote_,seed=challengeSeed_;
        const bool pending=count_!=0;*this=State{};session_=session;player_=player;localNonce_=local;peerNonce_=remote;
        challengeSeed_=seed;peerReady_=true;readyPending_=true;if(pending)PushUndelivered();
    }
    std::uint64_t session_=0;
    std::uint16_t player_=0;
    std::uint32_t localNonce_=0,peerNonce_=0,nextSequence_=1,lastReceived_=0,lastHello_=0,lastData_=0,lastSubmit_=0,lastReceive_=0;
    std::uint32_t candidateRemote_=0,candidateLocal_=0,candidateAt_=0,candidateLastSent_=0,challengeSeed_=0;
    bool candidateSent_=false;
    bool peerReady_=false,awaitingCommit_=false,readyPending_=false,helloPending_=false,helloSent_=false,dataSent_=false,submitted_=false,received_=false,ackPending_=false;
    Item queue_[kQueueSize]{};std::size_t count_=0;
    Event events_[kEventSize]{};std::size_t eventCount_=0;
};
} // namespace coop_chat
