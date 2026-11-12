// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#pragma once
#include "CoopModInventory.h"

// Optional authorization of a VERIFIED file difference. This never verifies
// files, bypasses a failed scan, or asserts that mismatched mods are compatible.
// The caller supplies current runtime eligibility and authenticated endpoints.
namespace coop_mod_consent {
enum class Choice:uint8_t { Pending=0, Accept=1, Decline=2 };
struct Binding {
    uint64_t session=0;uint32_t membership=0,localToken=0,peerToken=0,localBytes=0,peerBytes=0;
    coop::Endpoint peer{};
    coop_mods::Hash localHash{},peerHash{};
    bool Valid()const{return session&&membership&&localToken&&peerToken&&peer.Valid()&&
        localBytes>=12&&localBytes<=coop_mods::kMaxManifest&&peerBytes>=12&&peerBytes<=coop_mods::kMaxManifest&&
        (localHash!=peerHash||localBytes!=peerBytes);}
    friend bool operator==(const Binding& a,const Binding& b){return a.session==b.session&&a.membership==b.membership&&
        a.localToken==b.localToken&&a.peerToken==b.peerToken&&a.localBytes==b.localBytes&&a.peerBytes==b.peerBytes&&
        a.peer==b.peer&&a.localHash==b.localHash&&a.peerHash==b.peerHash;}
    friend bool operator!=(const Binding& a,const Binding& b){return !(a==b);}
};
#pragma pack(push,1)
struct Packet {
    uint32_t magic=coop_mods::kMagic;uint16_t version=2,size=sizeof(Packet);
    uint64_t session=0;
    uint32_t membership=0,token=0,echo=0,localBytes=0,peerBytes=0,revision=0,ack=0;
    uint64_t round=0,echoRound=0;
    Choice choice=Choice::Pending;uint8_t reserved[3]{};
    coop_mods::Hash localHash{},peerHash{};
};
#pragma pack(pop)
static_assert(sizeof(Packet)==128&&sizeof(Packet)<1200,"Consent remains a bounded control datagram");
inline bool Valid(const Packet& p){return p.magic==coop_mods::kMagic&&p.version==2&&p.size==sizeof(Packet)&&
    p.session&&p.membership&&p.token&&p.echo&&p.revision&&p.round&&p.choice>=Choice::Pending&&p.choice<=Choice::Decline&&
    (p.choice==Choice::Pending||p.echoRound)&&
    !p.reserved[0]&&!p.reserved[1]&&!p.reserved[2]&&p.localBytes>=12&&p.localBytes<=coop_mods::kMaxManifest&&
    p.peerBytes>=12&&p.peerBytes<=coop_mods::kMaxManifest&&
    (p.localHash!=p.peerHash||p.localBytes!=p.peerBytes);}
class Consent {
public:
    void Reset(){const auto serial=serial_;*this=Consent{};serial_=serial;}
    void Bind(const Binding& binding){
        if(binding==binding_)return;
        Reset();if(binding.Valid()){binding_=binding;revision_=1;round_=++serial_;if(!round_)round_=++serial_;}
    }
    const Binding& Identity()const{return binding_;}
    bool Bound()const{return binding_.Valid();}
    // Every readiness check still needs current catalog/native/transport proof.
    // File changes and reconnects retire the binding, never just this gate.
    void SetEligible(bool value){eligible_=value&&Bound();}
    bool Eligible()const{return eligible_;}
    bool Declined()const{return declined_;}
    bool LocalAccepted()const{return local_==Choice::Accept;}
    bool CanDecide()const{return eligible_&&!declined_&&peerRound_&&peerEcho_;}
    bool Ready()const{return CanDecide()&&local_==Choice::Accept&&peer_==Choice::Accept&&peerAck_==revision_;}
    bool Decide(bool accept){
        if(!CanDecide())return false;
        const auto choice=accept?Choice::Accept:Choice::Decline;
        if(choice==local_)return false;
        local_=choice;declined_=!accept;++revision_;peerAck_=0;sent_=false;return true;
    }
    bool Receive(const Packet& p,coop::Endpoint sender){
        if(!eligible_||!Valid(p)||sender!=binding_.peer||p.session!=binding_.session||p.membership!=binding_.membership||
            p.token!=binding_.peerToken||p.echo!=binding_.localToken||p.localBytes!=binding_.peerBytes||p.peerBytes!=binding_.localBytes||
            p.localHash!=binding_.peerHash||p.peerHash!=binding_.localHash||p.ack>revision_||
            (p.echoRound&&p.echoRound!=round_&&(p.choice!=Choice::Pending||p.round<=peerRound_))||
            (!p.echoRound&&p.choice!=Choice::Pending)||
            (peerRound_&&p.round<peerRound_))return false;
        if(p.round!=peerRound_){
            if(p.choice!=Choice::Pending)return false;
            if(peerRound_&&local_==Choice::Accept&&!declined_){local_=Choice::Pending;++revision_;}
            peerRound_=p.round;peerRevision_=peerAck_=0;peer_=Choice::Pending;peerEcho_=false;sent_=false;
        }
        if(p.revision<peerRevision_)return false;
        if(p.echoRound==round_)peerEcho_=true;
        if(p.revision==peerRevision_&&p.choice!=peer_)return false;
        if(p.revision>peerRevision_){peerRevision_=p.revision;peer_=p.choice;sent_=false;}
        if(p.choice==Choice::Decline)declined_=true;
        if(p.ack>peerAck_)peerAck_=p.ack;
        return true;
    }
    bool Tick(uint32_t now,Packet& packet){
        if(!Bound()||!eligible_||(sent_&&uint32_t(now-lastSent_)<250))return false;
        packet=Packet{};packet.session=binding_.session;packet.membership=binding_.membership;
        packet.token=binding_.localToken;packet.echo=binding_.peerToken;
        packet.localBytes=binding_.localBytes;packet.peerBytes=binding_.peerBytes;
        packet.localHash=binding_.localHash;packet.peerHash=binding_.peerHash;
        packet.revision=revision_;packet.ack=peerRevision_;packet.choice=local_;
        packet.round=round_;packet.echoRound=peerRound_;
        lastSent_=now;sent_=true;return true;
    }
private:
    Binding binding_{};
    uint32_t revision_=0,peerRevision_=0,peerAck_=0,lastSent_=0;
    uint64_t serial_=0,round_=0,peerRound_=0;
    Choice local_=Choice::Pending,peer_=Choice::Pending;
    bool eligible_=false,declined_=false,sent_=false,peerEcho_=false;
};
}
