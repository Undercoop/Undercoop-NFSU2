// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopTime.h"
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Repeated, bounded snapshots make every transition recoverable after UDP loss.
// Native pointers and local continuation/source packages never cross the wire.
namespace coop_movie {
// NC2M belongs to the mod inventory wire. The dispatcher visits that receiver
// first, so sharing its identity discarded every movie snapshot before here.
constexpr uint32_t kMagic=0x564D434E; // NCMV: dedicated movie protocol identity.
constexpr uint16_t kVersion=3;
constexpr uint32_t kRepeatMs=100,kLeadMs=600;
constexpr size_t kMaximumClips=4,kNameSize=64;
inline bool Reached(uint32_t now,uint32_t deadline){return int32_t(now-deadline)>=0;}
inline bool Newer(uint32_t a,uint32_t b){return int32_t(a-b)>0;}
struct Playlist {
    uint8_t count=0;
    char names[kMaximumClips][kNameSize]{};
};
inline bool SafeName(const char* name) {
    if(!name || !name[0]) return false;
    for(size_t i=0;i<kNameSize;++i) {
        const unsigned char c=static_cast<unsigned char>(name[i]);
        if(!c) return true;
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_')) return false;
    }
    return false;
}
inline bool Valid(const Playlist& p) {
    if(p.count>kMaximumClips) return false;
    for(size_t i=0;i<kMaximumClips;++i) {
        if(i<p.count) {if(!SafeName(p.names[i])) return false;}
        else for(char c:p.names[i]) if(c) return false;
    }
    return true;
}
inline bool Add(Playlist& p,const char* name) {
    if(p.count>=kMaximumClips || !SafeName(name)) return false;
    std::strcpy(p.names[p.count++],name);return true;
}
inline bool SameName(const char* a,const char* b) {
    for(size_t i=0;i<kNameSize;++i) {unsigned char x=a[i],y=b[i];
        if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;
        if(x!=y)return false;if(!x)return true;}return false;
}
inline bool Join(const Playlist& a,const Playlist& b,Playlist& result) {
    if(!Valid(a)||!Valid(b))return false;Playlist next=a;
    for(size_t i=0;i<b.count;++i) {bool found=false;
        for(size_t j=0;j<next.count;++j)if(SameName(next.names[j],b.names[i]))found=true;
        if(!found&&!Add(next,b.names[i]))return false;}
    result=next;return true;
}
inline bool SameContext(uint8_t flow,uint32_t event,uint32_t epoch,
    uint8_t otherFlow,uint32_t otherEvent,uint32_t otherEpoch) {
    return flow==otherFlow && epoch==otherEpoch && event==otherEvent;
}
inline uint32_t Digest(const Playlist& p) {
    uint32_t hash=2166136261u;
    hash=(hash^p.count)*16777619u;
    for(const auto& name:p.names) for(unsigned char c:name) hash=(hash^c)*16777619u;
    return hash;
}
enum class Phase:uint8_t { Idle,Preparing,Playing,Advancing,Done };
enum Flag:uint8_t { Offered=1,Ready=2,Voted=4,Ended=8,AdvanceApplied=16 };
#pragma pack(push,1)
struct Context {uint32_t event=0,epoch=0;uint8_t flow=0;};
struct Packet {
    uint32_t magic=kMagic;
    uint16_t version=kVersion,size=sizeof(Packet);
    uint64_t session=0;
    uint32_t ticket=0,sequence=0,scene=0,request=0,peerRequest=0,event=0,epoch=0;
    uint32_t sentAt=0,echoAt=0,deadline=0,digest=0;
    uint8_t host=0,flow=0,ordinal=0,flags=0,phase=0,skip=0,reserved[2]{};
    Playlist playlist{};
    uint8_t alternativeCount=0;Context alternatives[8]{};
};
#pragma pack(pop)
static_assert(sizeof(Packet)<512,"Movie datagram must remain bounded");
enum class Action:uint8_t { None,Start,Advance,FinishWithoutMovie };
class State {
public:
    void Bind(uint64_t session,uint32_t ticket,bool host) {
        if(session_==session && ticket_==ticket && host_==host) return;
        *this=State{};session_=session;ticket_=ticket;host_=host;
    }
    bool Offer(uint8_t flow,uint32_t event,const Playlist& p,uint32_t epoch=0) {
        if(!session_ || !ticket_ || !Valid(p) || (flow!=1&&flow!=2&&flow!=4&&flow!=6) || localOffered_) return false;
        local_=p;localFlow_=flow;localEvent_=event;localEpoch_=epoch;localOffered_=true;
        if(++request_==0) ++request_;
        if(host_) Begin();else peerOffered_=peerKnown_&&LocalMatchesPeer();
        return true;
    }
    // Reorder bounded native requests while PREPARING, never during playback.
    // A new request id invalidates every readiness ACK for the displaced offer.
    bool ReplaceOffer(uint8_t flow,uint32_t event,const Playlist& p,uint32_t epoch) {
        if(phase_!=Phase::Preparing || !localOffered_ || !peerKnown_ || !Valid(p) ||
           !SameContext(flow,event,epoch,peerFlow_,peerEvent_,peerEpoch_))return false;
        local_=p;localFlow_=flow;localEvent_=event;localEpoch_=epoch;
        if(++request_==0)++request_;ClearClip();peerOffered_=true;
        if(host_)Select();return true;
    }
    // Only the adapter may accept a peer-only request, after proving that its
    // local native continuation is already safe (e.g. a resumed Host's intro).
    bool AcceptPeerOnly(uint8_t flow) {Playlist empty;return Offer(flow,peerEvent_,empty,peerEpoch_);}
    bool Receive(const Packet& p,uint32_t now) {
        if(p.magic!=kMagic || p.version!=kVersion || p.size!=sizeof(Packet) ||
           p.session!=session_ || p.ticket!=ticket_ || p.host==uint8_t(host_) ||
           p.host>1 || p.phase>uint8_t(Phase::Done) || p.ordinal>kMaximumClips ||
           (p.flow!=1&&p.flow!=2&&p.flow!=4&&p.flow!=6) ||
           p.flags&~uint8_t(31) || p.skip>1 || p.reserved[0] || p.reserved[1] ||
           !Valid(p.playlist) || p.digest!=Digest(p.playlist) ||
           p.ordinal>p.playlist.count ||
           p.alternativeCount>8 ||
           (haveSequence_&&!Newer(p.sequence,receivedSequence_))) return false;
        for(size_t i=0;i<8;++i) {
            const auto& c=p.alternatives[i];
            if(i<p.alternativeCount) {if(c.flow!=1&&c.flow!=2&&c.flow!=4&&c.flow!=6)return false;}
            else if(c.flow||c.event||c.epoch)return false;
        }
        receivedSequence_=p.sequence;haveSequence_=true;echoAt_=p.sentAt;
        peerAlternativeCount_=p.alternativeCount;std::copy_n(p.alternatives,8,peerAlternatives_.begin());
        if(host_) {
            if((p.flags&Offered) && p.request && p.request!=peerRequest_) {
                if(phase_==Phase::Playing || phase_==Phase::Advancing) return false;
                if(peerRequest_&&!Newer(p.request,peerRequest_)) return false;
                if(phase_==Phase::Done) BeginNext();
                peerRequest_=p.request;peer_=p.playlist;peerOffered_=peerKnown_=true;
                peerFlow_=p.flow;peerEvent_=p.event;peerEpoch_=p.epoch;
                if(phase_==Phase::Idle || phase_==Phase::Done) {event_=p.event;epoch_=p.epoch;flow_=p.flow;Begin();}
                Select();
            }
            if(LocalMatchesPeer() && SameContext(localFlow_,localEvent_,localEpoch_,p.flow,p.event,p.epoch) &&
               p.request==peerRequest_ && p.scene==scene_ && p.ordinal==ordinal_ && p.peerRequest==request_ &&
               p.digest==Digest(plan_)) {peerReady_=(p.flags&Ready)!=0;peerVoted_=(p.flags&Voted)!=0;
                peerEnded_=(p.flags&Ended)!=0;peerApplied_=(p.flags&AdvanceApplied)!=0;}
        } else {
            if(p.scene && (scene_==0 || Newer(p.scene,scene_))) {
                if(phase_==Phase::Playing || phase_==Phase::Advancing) return false;
                if(phase_==Phase::Done) BeginNext();
                scene_=p.scene;ordinal_=0;ClearClip();phase_=Phase::Preparing;
            }
            if(p.scene!=scene_) return false;
            peerRequest_=p.request;peerKnown_=(p.flags&Offered)!=0;
            peerFlow_=p.flow;peerEvent_=p.event;peerEpoch_=p.epoch;
            peerOffered_=peerKnown_&&p.peerRequest==request_&&localOffered_&&LocalMatchesPeer();
            if(phase_==Phase::Preparing) {
                if(Digest(plan_)!=p.digest) localReady_=false;
                plan_=p.playlist;flow_=p.flow;event_=p.event;epoch_=p.epoch;
            }
            // RTT correction uses this Client's timestamp echoed by the Host.
            // The offset is frozen while a deadline is pending.
            if(!deadline_ && p.echoAt && uint32_t(now-p.echoAt)<2000) {
                offset_=int32_t(p.sentAt-(now-uint32_t(now-p.echoAt)/2));haveClock_=true;
            }
            if(p.ordinal==ordinal_) {
                peerReady_=(p.flags&Ready)!=0;peerVoted_=(p.flags&Voted)!=0;
                peerEnded_=(p.flags&Ended)!=0;
                if(peerOffered_ && p.deadline && ((phase_==Phase::Preparing&&p.phase==uint8_t(Phase::Playing)) ||
                   (phase_==Phase::Playing&&p.phase==uint8_t(Phase::Advancing)))) {
                    deadline_=p.deadline;skip_=p.skip!=0;
                    phase_=Phase(p.phase);
                }
            }
            if(p.ordinal==uint8_t(ordinal_+1) && localApplied_) {
                ordinal_=p.ordinal;ClearClip();phase_=ordinal_>=plan_.count?Phase::Done:Phase::Playing;
            }
            if(p.phase==uint8_t(Phase::Done)&&!plan_.count&&localApplied_) phase_=Phase::Done;
        }
        return true;
    }
    void ReadyForStart(){if(phase_==Phase::Preparing && localOffered_ && peerOffered_ && LocalMatchesPeer() && planValid_ && (host_||haveClock_)) localReady_=true;}
    void Vote(){if(phase_==Phase::Playing && started_) localVoted_=true;}
    void End(){if(phase_==Phase::Playing && started_) localEnded_=true;}
    void Tick(uint32_t now) {
        if(!host_) return;
        if(phase_==Phase::Playing && !plan_.count && localApplied_ && peerApplied_) {
            phase_=Phase::Done;
        } else if(phase_==Phase::Preparing && localReady_ && peerReady_ && peerOffered_ && LocalMatchesPeer() && planValid_) {
            phase_=Phase::Playing;deadline_=now+kLeadMs;if(!deadline_)deadline_=1;
        } else if(phase_==Phase::Playing && started_ &&
                  ((localVoted_&&peerVoted_)||(localEnded_&&peerEnded_))) {
            skip_=localVoted_&&peerVoted_;phase_=Phase::Advancing;deadline_=now+kLeadMs;if(!deadline_)deadline_=1;
        } else if(phase_==Phase::Advancing && localApplied_ && peerApplied_) {
            ++ordinal_;ClearClip();phase_=ordinal_>=plan_.count?Phase::Done:Phase::Playing;
        }
    }
    Action Due(uint32_t now) const {
        if(!deadline_ || (!host_&&!haveClock_) || !Reached(now,deadline_-uint32_t(host_?0:offset_))) return Action::None;
        if(phase_==Phase::Playing && !started_) return plan_.count?Action::Start:Action::FinishWithoutMovie;
        if(phase_==Phase::Advancing && !localApplied_) return Action::Advance;
        return Action::None;
    }
    // Keep the original deadline in repeated snapshots until both peers have
    // advanced; a lost Start snapshot must still recover after Host playback.
    void Started(){started_=true;}
    void Applied(){localApplied_=true;}
    void FinishedWithoutMovie(){started_=localApplied_=true;}
    bool Build(uint32_t now,Packet& p) {
        if(!session_ || !ticket_ || (!localOffered_ && !scene_) ||
           (!localOffered_&&phase_==Phase::Idle&&flow_==0) ||
           (lastSent_ && coop_time::Elapsed(now,lastSent_)<kRepeatMs)) return false;
        lastSent_=now;p=Packet{};p.session=session_;p.ticket=ticket_;p.sequence=++sequence_;
        p.scene=scene_;p.request=request_;p.peerRequest=peerRequest_;p.event=host_?event_:localEvent_;p.epoch=host_?epoch_:localEpoch_;
        p.sentAt=now;p.echoAt=echoAt_;p.deadline=host_?deadline_:0;
        p.host=uint8_t(host_);p.flow=host_?flow_:localFlow_;p.ordinal=ordinal_;
        p.flags=(localOffered_?Offered:0)|(localReady_?Ready:0)|(localVoted_?Voted:0)|
            (localEnded_?Ended:0)|(localApplied_?AdvanceApplied:0);
        p.phase=uint8_t(phase_);p.skip=uint8_t(skip_);p.playlist=host_?plan_:local_;
        // Client sends the canonical plan after acknowledging a scene.
        if(!host_ && scene_ && peerOffered_) p.playlist=plan_;
        p.digest=Digest(p.playlist);p.alternativeCount=alternativeCount_;std::copy_n(alternatives_.begin(),8,p.alternatives);return true;
    }
    bool BeginNext() {
        if(phase_!=Phase::Done) return false;
        localOffered_=peerOffered_=peerKnown_=false;local_={};peer_={};plan_={};phase_=Phase::Idle;
        ClearClip();return true;
    }
    // A finalized same-event restart replaces the native world. Retire only
    // its old race movie/return transaction while retaining monotonic wire
    // counters: old packets must not become a fresh scene after a reset.
    bool RetireRace(uint32_t epoch,uint32_t event=0) {
        if(!epoch)return false;
        const auto retired=[&](uint8_t flow,uint32_t value,uint32_t hash){return value==epoch&&(!event||hash==event)&&(flow==1||flow==4);};
        const bool current=retired(flow_,epoch_,event_),local=retired(localFlow_,localEpoch_,localEvent_),peer=retired(peerFlow_,peerEpoch_,peerEvent_);
        if(!current&&!local&&!peer)return false;
        if(local){localOffered_=false;local_={};localFlow_=0;localEvent_=localEpoch_=0;}
        if(peer){peerKnown_=peerOffered_=false;peer_={};peerFlow_=0;peerEvent_=peerEpoch_=0;}
        if(current){
            plan_={};phase_=Phase::Idle;flow_=0;event_=epoch_=0;ordinal_=0;ClearClip();
            // The adapter parks distinct native requests separately. An
            // unrelated in-flight peer offer may not resurrect this plan.
            peerOffered_=false;
        }
        auto compact=[&](auto& items,uint8_t& count){
            uint8_t next=0;for(uint8_t i=0;i<count;++i)if(!retired(items[i].flow,items[i].epoch,items[i].event))items[next++]=items[i];
            count=next;for(size_t i=next;i<items.size();++i)items[i]={};
        };
        compact(alternatives_,alternativeCount_);compact(peerAlternatives_,peerAlternativeCount_);
        return true;
    }
    const Playlist& Plan() const{return plan_;}
    uint32_t Scene()const{return scene_;} uint32_t Event()const{return event_;}
    uint32_t Epoch()const{return epoch_;}
    bool PeerKnown()const{return peerKnown_;}
    uint8_t PeerFlow()const{return peerFlow_;}uint32_t PeerEvent()const{return peerEvent_;}uint32_t PeerEpoch()const{return peerEpoch_;}
    bool LocalMatchesPeer()const{return localOffered_&&peerKnown_&&SameContext(localFlow_,localEvent_,localEpoch_,peerFlow_,peerEvent_,peerEpoch_);}
    void Alternatives(const Context* contexts,size_t count) {
        alternativeCount_=uint8_t(count<=8?count:0);alternatives_={};
        if(contexts&&alternativeCount_)std::copy_n(contexts,alternativeCount_,alternatives_.begin());
    }
    bool PeerCanOffer(uint8_t flow,uint32_t event,uint32_t epoch)const {
        if(peerKnown_&&SameContext(flow,event,epoch,peerFlow_,peerEvent_,peerEpoch_))return true;
        for(size_t i=0;i<peerAlternativeCount_;++i)if(SameContext(flow,event,epoch,peerAlternatives_[i].flow,
            peerAlternatives_[i].event,peerAlternatives_[i].epoch))return true;return false;
    }
    uint8_t Flow()const{return flow_;} uint8_t Ordinal()const{return ordinal_;}
    Phase CurrentPhase()const{return phase_;}
    bool LocalOffered()const{return localOffered_;} bool IsHost()const{return host_;}
    bool Skip()const{return skip_;} bool LocalVote()const{return localVoted_;}
    bool PeerVote()const{return peerVoted_;} bool Active()const{return phase_!=Phase::Idle&&phase_!=Phase::Done;}
private:
    void Begin(){if(phase_==Phase::Preparing) {Select();return;}
        if(++scene_==0) ++scene_;phase_=Phase::Preparing;ordinal_=0;ClearClip();Select();}
    void Select(){Playlist next=local_;planValid_=!localOffered_||!peerOffered_||!LocalMatchesPeer()||Join(local_,peer_,next);
        if(!localOffered_)next=peer_;
        if(Digest(next)!=Digest(plan_)) {plan_=next;localReady_=peerReady_=false;}
        if(localOffered_) {flow_=localFlow_;event_=localEvent_;epoch_=localEpoch_;}}
    void ClearClip(){localReady_=peerReady_=localVoted_=peerVoted_=localEnded_=peerEnded_=
        localApplied_=peerApplied_=false;deadline_=0;skip_=false;started_=ordinal_!=0;}
    uint64_t session_=0;uint32_t ticket_=0,request_=0,peerRequest_=0,scene_=0,event_=0,localEvent_=0,epoch_=0,localEpoch_=0,peerEvent_=0,peerEpoch_=0;
    uint32_t sequence_=0,receivedSequence_=0,lastSent_=0,echoAt_=0,deadline_=0;
    int32_t offset_=0;uint8_t localFlow_=0,flow_=0,peerFlow_=0,ordinal_=0;
    bool host_=false,haveSequence_=false,haveClock_=false,localOffered_=false,peerOffered_=false;
    bool peerKnown_=false,planValid_=true;
    uint8_t alternativeCount_=0,peerAlternativeCount_=0;std::array<Context,8> alternatives_{},peerAlternatives_{};
    bool localReady_=false,peerReady_=false,localVoted_=false,peerVoted_=false;
    bool localEnded_=false,peerEnded_=false,localApplied_=false,peerApplied_=false,started_=false,skip_=false;
    Phase phase_=Phase::Idle;Playlist local_{},peer_{},plan_{};
};
} // namespace coop_movie
