// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopSpecialRecoveryReceipt.h"
// Include after ReceiptStore and RecoveryLookup. The class is intentionally
// disabled by default and is not connected to Main by this standalone module.
// Source/catalog/receipt work belongs ONLY to game-thread Tick/Result; UDP sees
// bounded numeric publications. This module never mutates a receipt or save.
namespace CoopSpecialNative { namespace RecoveryRuntime {
namespace wire=coop_special_recovery;
namespace bridge=coop_special_recovery_receipt;
namespace receipt=coop_special_receipt;
struct Source {
    std::string personal,campaign;uint32_t catalog=0;
    const receipt::Transaction* own=nullptr; // Ephemeral GAME THREAD only; never published.
    bool faulted=false;
    bool queryEligible=true; // Existing authenticated evidence is NOT revoked by this flag.
};
enum class Status {Disabled,WaitingContext,Ready,AwaitingDecision,Resolved,HeldOriginalRole,HeldContext,Faulted};
static bool CurrentNormalOwnOperation(const receipt::Scope& own){
    if(!state.Active()||(state.Player()!=1&&state.Player()!=2)||own.localPlayer!=state.Player())return false;
    const uint32_t host=state.Player()==1?state.Nonce():state.PeerNonce();
    const uint32_t client=state.Player()==2?state.Nonce():state.PeerNonce();
    return own.session==state.Session()&&own.ticket==state.Ticket()&&own.operation==state.Operation()&&
        own.hostNonce==host&&own.clientNonce==client&&own.event==state.Current().id&&own.kind==uint8_t(state.Current().kind)&&
        own.stage==state.Current().stage&&own.catalog==state.CurrentContext().catalog&&
        bridge::Campaign(own.campaign)==state.CurrentContext().campaign;
}
static bool ProductionSource(Source& out){
    out={};
    if(!OwnCareer()||!CoopPersonalSave::Name(CoopPersonalSave::personal)||!CoopPersonalSave::Name(CoopPersonalSave::campaign))return false;
    out.faulted=ReceiptStore::Status()==ReceiptStore::LoadState::Faulted;
    if(!ReceiptStore::Ready())return false;
    const auto* catalog=FactsCatalog();if(!catalog)return false;
    out.personal=CoopPersonalSave::personal;out.campaign=CoopPersonalSave::campaign;out.catalog=catalog->digest;
    out.own=&ReceiptStore::tx;
    // Current normal lifecycle owns this decision. Mere matching session is
    // not enough: an old incarnation/op still needs independent recovery.
    const auto session=CoopSnapshot();
    out.queryEligible=receipt::Empty(*out.own)||!CurrentNormalOwnOperation(out.own->scope)||state.OfflineRecovery()||
        !session.joined||session.sessionId!=state.Session()||session.ticket!=state.Ticket();return true;
}
static uint32_t ProductionNonce(uint32_t serial,DWORD now){
    uint32_t value=now^(GetCurrentProcessId()*0x9E3779B9u)^(serial*0x85EBCA6Bu)^0x4E325344u;
    return value?value:serial;
}
struct Ports {
    std::function<coop::SessionSnapshot()> snapshot=CoopSnapshot; // SRW-safe in UDP.
    std::function<bool(Source&)> source=ProductionSource; // NEVER called by UDP.
    ReceiptStore::Ports files=ReceiptStore::ProductionPorts(); // Game-thread read-only lookup.
    std::function<void(const void*,size_t,coop::Endpoint)> send=SendSessionBytes;
    std::function<uint32_t(uint32_t,DWORD)> nonce=ProductionNonce;
};
class Runtime {
    struct Binding {
        bool ready=false;uint64_t session=0;uint32_t ticket=0,nonce=0,generation=0;
        uint8_t player=0;coop::Endpoint peer{};wire::Context context{};
    };
    struct Received {wire::Packet packet{};DWORD at=0;};
    Ports ports_;bool enabled_=false,ticking_=false,fault_=false;
    uint32_t incarnation_=0,generation_=0;wire::State state_;
    Binding binding_;std::string personal_,campaign_;
    bool haveTarget_=false;wire::Key target_{};uint32_t before_=0;
    Status status_=Status::Disabled;RecoveryLookup::Status lookup_=RecoveryLookup::Status::Missing;
    mutable SRWLOCK mailbox_=SRWLOCK_INIT;
    Binding publication_;std::array<Received,16> inbox_{};size_t count_=0;
    wire::Packet outbound_{};bool haveOutbound_=false;DWORD publishedAt_=0;uint32_t sent_=0;
    std::atomic<uint32_t> dropped_{0};
    static uint8_t Player(coop::Role role){return role==coop::Role::Host?1:role==coop::Role::Client?2:0;}
    static bool Session(const coop::SessionSnapshot& s){return s.joined&&s.sessionId&&s.ticket&&s.peer.Valid()&&Player(s.role);}
    static bool Same(const Binding& b,const coop::SessionSnapshot& s){return b.ready&&Session(s)&&
        b.session==s.sessionId&&b.ticket==s.ticket&&b.player==Player(s.role)&&b.peer==s.peer;}
    static bool SourceValid(const Source& s){
        return !s.faulted&&receipt::SafeName(s.personal)&&receipt::SafeName(s.campaign)&&s.catalog&&s.own&&
            (receipt::Empty(*s.own)||(receipt::Valid(*s.own)&&s.own->scope.personal==s.personal&&s.own->scope.campaign==s.campaign));
    }
    bool SameSource(const Source& s)const{return SourceValid(s)&&s.personal==personal_&&s.campaign==campaign_&&
        s.catalog==binding_.context.catalog&&bridge::Campaign(s.campaign)==binding_.context.campaign;}
    void Clear(){
        state_.Unbind();binding_={};personal_.clear();campaign_.clear();haveTarget_=false;target_={};before_=0;fault_=false;
        lookup_=RecoveryLookup::Status::Missing;
        AcquireSRWLockExclusive(&mailbox_);publication_={};count_=0;haveOutbound_=false;sent_=0;publishedAt_=0;
        ReleaseSRWLockExclusive(&mailbox_);
    }
    bool Bind(const coop::SessionSnapshot& session,const Source& source,DWORD now){
        Clear();
        if(!enabled_||incarnation_==UINT32_MAX||generation_==UINT32_MAX||!ports_.nonce)return false;
        const uint32_t nonce=ports_.nonce(++incarnation_,now);if(!nonce)return false;
        binding_={true,session.sessionId,session.ticket,nonce,++generation_,Player(session.role),session.peer,
            {bridge::Campaign(source.campaign),source.catalog}};
        personal_=source.personal;campaign_=source.campaign;
        state_.Bind(binding_.session,binding_.ticket,binding_.player,binding_.nonce,binding_.context);
        if(!state_.Bound()){Clear();return false;}
        AcquireSRWLockExclusive(&mailbox_);publication_=binding_;ReleaseSRWLockExclusive(&mailbox_);return true;
    }
    bool StillCurrent()const{
        if(!enabled_||!ports_.snapshot||!ports_.source||!Same(binding_,ports_.snapshot()))return false;
        Source source;return ports_.source(source)&&SameSource(source);
    }
    void Publish(DWORD now,bool allowClientWork){
        wire::Packet packet;if(!state_.Poll(now,packet))return;
        // Once its local obligation is no longer Prepared/ownDone, Client
        // retains authenticated evidence but stops repeatedly asking Host to
        // reread old archives. Keep canonical Hello for nonce/liveness recovery.
        if(binding_.player==2&&!allowClientWork&&packet.message==wire::Message::Query){
            packet.old={};packet.request=packet.beforeDigest=0;packet.message=wire::Message::Hello;packet.decision=wire::Decision::Pending;
        }
        AcquireSRWLockExclusive(&mailbox_);
        if(publication_.ready&&publication_.generation==binding_.generation){outbound_=packet;haveOutbound_=true;publishedAt_=now;}
        ReleaseSRWLockExclusive(&mailbox_);
    }
public:
    explicit Runtime(Ports ports=Ports{}):ports_(std::move(ports)){}
    Runtime(const Runtime&)=delete;Runtime& operator=(const Runtime&)=delete;
    void Enable(bool enabled){enabled_=enabled;if(!enabled){Clear();status_=Status::Disabled;}}
    bool Enabled()const{return enabled_;}
    Status CurrentStatus()const{return status_;} // Game thread.
    RecoveryLookup::Status LastLookup()const{return lookup_;} // Game thread.
    uint32_t Dropped()const{return dropped_.load(std::memory_order_relaxed);}
    // Explicit game-thread retry after external file state was repaired or its
    // own Store Reload resolved ambiguous IO. Ordinary Tick never clears faults.
    bool Retry(){if(!enabled_||!binding_.ready||!fault_||!StillCurrent())return false;fault_=false;return true;}
    void Tick(DWORD now){
        if(ticking_)return;
        struct Guard {bool& flag;explicit Guard(bool& value):flag(value){flag=true;}~Guard(){flag=false;}} guard(ticking_);
        if(!enabled_){status_=Status::Disabled;return;}
        if(!ports_.snapshot||!ports_.source){Clear();status_=Status::Faulted;return;}
        const auto session=ports_.snapshot();Source source;
        if(!Session(session)){Clear();status_=Status::WaitingContext;return;}
        if(!ports_.source(source)||!SourceValid(source)){
            const bool failed=source.faulted||(source.own&&!receipt::Empty(*source.own)&&!receipt::Valid(*source.own));
            Clear();status_=failed?Status::Faulted:Status::WaitingContext;return;
        }
        if(!enabled_){Clear();status_=Status::Disabled;return;}
        if(!Same(binding_,session)||!SameSource(source)){
            if(!Bind(session,source,now)){status_=Status::Faulted;return;}
        }
        const auto* own=source.own;
        const bool pending=!receipt::Empty(*own)&&own->phase==receipt::Phase::Prepared&&own->ownDone;
        const bool oldClient=pending&&own->scope.localPlayer==2;
        const bool matching=pending&&own->scope.catalog==source.catalog&&bridge::Campaign(own->scope.campaign)==binding_.context.campaign;
        const bool query=oldClient&&matching&&binding_.player==2&&source.queryEligible;
        const wire::Key key=query?bridge::Key(own->scope):wire::Key{};
        const uint32_t digest=query?receipt::Digest(own->before):0;
        if(query&&haveTarget_&&(!wire::Same(target_,key)||before_!=digest)){
            // A later independent recovery obligation cannot reuse a terminal
            // request's old evidence. New nonce retires its entire old stream.
            if(!Bind(session,source,now)){status_=Status::Faulted;return;}
        }
        std::array<Received,16> received{};size_t count=0;
        AcquireSRWLockExclusive(&mailbox_);count=count_;std::copy_n(inbox_.begin(),count,received.begin());count_=0;
        ReleaseSRWLockExclusive(&mailbox_);
        for(size_t i=0;i<count;++i){
            if(coop_time::Elapsed(now,received[i].at)>wire::kFreshMs){dropped_.fetch_add(1,std::memory_order_relaxed);continue;}
            state_.Receive(received[i].packet,received[i].at);
        }
        if(query&&session.fresh&&!fault_&&state_.Start(key,digest,now)){haveTarget_=true;target_=key;before_=digest;}
        if(binding_.player==1&&session.fresh&&!fault_){
            const auto request=state_.PendingLookup(now);
            if(request){
                const auto result=RecoveryLookup::Read(request,personal_,campaign_,ports_.files);
                // A port may reenter UI/session selection. Never resolve an old
                // disk callback in a replacement personal/context/session.
                if(!StillCurrent()){Clear();status_=enabled_?Status::WaitingContext:Status::Disabled;return;}
                lookup_=result.status;
                if(result.status==RecoveryLookup::Status::Faulted)fault_=true;
                else if(result.status==RecoveryLookup::Status::Proven){if(!state_.Resolve(request,result.proof,now))fault_=true;}
                else state_.Defer(request,now); // Missing/Prepared NEVER invent Abort.
            }
        }
        if(fault_)status_=Status::Faulted;
        else if(pending&&own->scope.localPlayer!=binding_.player)status_=Status::HeldOriginalRole;
        else if(pending&&!matching)status_=Status::HeldContext;
        else if(query){const auto evidence=state_.Result();status_=bridge::MatchesPending(*own,evidence)?Status::Resolved:Status::AwaitingDecision;}
        else status_=Status::Ready;
        Publish(now,query&&!fault_);
    }
    wire::Evidence Result()const{ // GAME THREAD ONLY; never a raw wire decision.
        if(!enabled_||fault_||binding_.player!=2||!StillCurrent())return {};
        Source source;if(!ports_.source(source)||!SameSource(source)||!source.own)return {};
        const auto evidence=state_.Result();
        return bridge::MatchesPending(*source.own,evidence)&&evidence.request.session==binding_.session&&
            evidence.request.ticket==binding_.ticket&&evidence.request.clientNonce==binding_.nonce?evidence:wire::Evidence{};
    }
    bool Receive(const char* bytes,int length,const sockaddr_in& sender,DWORD now){
        uint32_t magic=0;if(!bytes||length<4)return false;std::memcpy(&magic,bytes,4);if(magic!=wire::kMagic)return false;
        // No catalog, Source, receipt object, disk IO or State work on UDP.
        if(length!=sizeof(wire::Packet)||sender.sin_family!=AF_INET||!ports_.snapshot)return true;
        wire::Packet packet;std::memcpy(&packet,bytes,sizeof(packet));const auto session=ports_.snapshot();
        if(!Session(session)||session.peer.address!=sender.sin_addr.s_addr||session.peer.port!=ntohs(sender.sin_port)||
            !wire::Valid(packet)||packet.session!=session.sessionId||packet.ticket!=session.ticket||packet.player==(Player(session.role)))return true;
        AcquireSRWLockExclusive(&mailbox_);
        if(Same(publication_,session)&&wire::Same(packet.context,publication_.context)){
            if(count_==inbox_.size()){std::move(inbox_.begin()+1,inbox_.end(),inbox_.begin());--count_;dropped_.fetch_add(1,std::memory_order_relaxed);}
            inbox_[count_++]={packet,now};
        }
        ReleaseSRWLockExclusive(&mailbox_);return true;
    }
    void NetworkTick(DWORD now){
        if(!ports_.snapshot||!ports_.send)return;const auto session=ports_.snapshot();
        if(!Session(session)||!session.fresh)return;
        wire::Packet packet;bool send=false;
        AcquireSRWLockExclusive(&mailbox_);
        if(Same(publication_,session)&&haveOutbound_&&outbound_.sequence!=sent_&&coop_time::Elapsed(now,publishedAt_)<=wire::kFreshMs&&
            outbound_.session==session.sessionId&&outbound_.ticket==session.ticket&&wire::Same(outbound_.context,publication_.context)){
            packet=outbound_;sent_=packet.sequence;send=true;
        }
        ReleaseSRWLockExclusive(&mailbox_);
        if(send)ports_.send(&packet,sizeof(packet),session.peer);
    }
};
// Root will activate/wire these only with complete bootstrap/receipt ownership.
static Runtime runtime;
inline void Enable(bool enabled){runtime.Enable(enabled);}
inline void Tick(DWORD now){runtime.Tick(now);}
inline bool Receive(const char* data,int size,const sockaddr_in& sender,DWORD now){return runtime.Receive(data,size,sender,now);}
inline void NetworkTick(DWORD now){runtime.NetworkTick(now);}
inline wire::Evidence Result(){return runtime.Result();}
inline Status CurrentStatus(){return runtime.CurrentStatus();}
inline bool Retry(){return runtime.Retry();}
}} // namespace CoopSpecialNative::RecoveryRuntime
