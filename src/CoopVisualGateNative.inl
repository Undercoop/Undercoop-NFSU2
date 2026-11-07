// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include "CoopVisualGate.h"
// Included after the personal career adapter. Engine work belongs to UiTick;
// network callbacks only exchange pointer-free observations through a mailbox.
namespace CoopVisualGateNative {
using RatingFn=float(__thiscall*)(void*);
using RefreshFn=void(__thiscall*)(void*);
static RatingFn rating=reinterpret_cast<RatingFn>(0x528410);
static RefreshFn refresh=reinterpret_cast<RefreshFn>(0x5331C0);
struct Addresses {uintptr_t career=0x85AD40,table=0x83840C,count=0x838408,mode=0x850078;};
static Addresses addresses;
struct Rule {uint32_t hash=0;float minimum=0;uint8_t stage=0,type=0,mandatory=0;};
struct Live {coop_visual::Context context{};std::array<Rule,64> rules{};unsigned count=0;uint32_t slot=0;};
static coop_visual::State state;
static bool installed=false,refreshPending=false,maskKnown=false;
static uint32_t nonceSerial=0,lastRead=0,lastSent=0;
static uint64_t lastMask=0;static coop_visual::Context maskContext{};
static SRWLOCK mailboxLock=SRWLOCK_INIT;
struct Received {coop_visual::Packet packet{};DWORD at=0;};
static std::array<Received,16> inbox{};static size_t inboxCount=0;
static coop_visual::Packet outbound{};static bool haveOutbound=false;
static uintptr_t callSite=0x5331C6;
static BYTE originalCall[5]{0xE8,0x45,0x52,0xFF,0xFF};
template<class T>static bool Read(uintptr_t at,T& out){return ReadGameMemory(at,&out,sizeof(out));}
static void Mix(uint32_t& hash,uint32_t v){for(unsigned i=0;i<4;++i){hash=(hash^uint8_t(v))*16777619u;v>>=8;}}
static uint32_t Hash(const std::string& text){uint32_t h=2166136261u;for(unsigned char c:text){
    if(c>='a'&&c<='z')c-=32;h=(h^c)*16777619u;}return h?h:1;}
static bool OwnCareer(){return gProfileContext.IsCoopActive()&&!gRestoreSpPending;}
static bool Scope(coop::SessionSnapshot& session){
    session=CoopSnapshot();return installed&&OwnCareer()&&IsCoopGameplayActive()&&session.joined&&session.fresh&&
        session.initialBarrierComplete&&session.sessionId&&session.ticket&&
        (session.role==coop::Role::Host||session.role==coop::Role::Client);
}
static bool ReadLive(Live& out){
    out={};uint32_t stage=0,count=0;uintptr_t table=0;
    if(!CoopPersonalSave::Name(CoopPersonalSave::campaign)||!Read(addresses.mode,count)||count!=1||
        !Read(addresses.career+0x7AF8,stage)||stage>=8||!Read(addresses.count,count)||count>128||!count||
        !Read(addresses.table,table)||!IsValidUserPtr(table)||!Read(addresses.career+0x8740,out.slot))return false;
    uint32_t digest=2166136261u;Mix(digest,count);
    for(uint32_t i=0;i<count;++i){
        BYTE row[0x40]{};if(!ReadGameMemory(table+i*0x40,row,sizeof(row)))return false;
        const uint8_t type=row[0x24];if(type!=2&&type!=4)continue;
        Rule rule{};std::memcpy(&rule.hash,row+0x20,4);std::memcpy(&rule.minimum,row+0x3C,4);
        rule.type=type;rule.stage=row[0x25];rule.mandatory=row[0x35];
        if(!rule.hash||rule.stage>=8||rule.mandatory>1||!coop_visual::ValidStars(rule.minimum)||out.count==out.rules.size())return false;
        for(unsigned j=0;j<out.count;++j)if(out.rules[j].hash==rule.hash)return false;
        out.rules[out.count++]=rule;uint32_t bits=0;std::memcpy(&bits,&rule.minimum,4);
        Mix(digest,rule.hash);Mix(digest,type);Mix(digest,rule.stage);Mix(digest,rule.mandatory);Mix(digest,bits);
    }
    out.context={Hash(CoopPersonalSave::campaign),digest?digest:1,uint8_t(stage)};return out.count!=0;
}
static bool ReadStars(float& value){
    __try{value=rating(reinterpret_cast<void*>(addresses.career));}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    return coop_visual::ValidStars(value);
}
static bool Effective(const Live& live,float local,float& minimum){
    coop::SessionSnapshot session;
    return Scope(session)&&session.sessionId==state.Session()&&session.ticket==state.Ticket()&&
        live.slot&&state.Effective(live.context,local,GetTickCount(),minimum);
}
static float __fastcall Gate(void* career,void*){
    // The ONLY patched caller is the native DVD eligibility pass5331C0.
    // Display, customization and payout callers still use untouched528410.
    if(!installed||!OwnCareer()||reinterpret_cast<uintptr_t>(career)!=addresses.career)return rating(career);
    Live live;float local=0,minimum=0;
    if(!ReadLive(live)||!ReadStars(local)||!Effective(live,local,minimum))return -1.0f;
    return minimum;
}
static void ClearMailbox(){AcquireSRWLockExclusive(&mailboxLock);inboxCount=0;haveOutbound=false;lastSent=0;ReleaseSRWLockExclusive(&mailboxLock);}
static void Bind(const coop::SessionSnapshot& session){
    if(state.Session()==session.sessionId&&state.Ticket()==session.ticket)return;
    uint32_t nonce=GetTickCount()^(GetCurrentProcessId()*0x9E3779B9u)^(++nonceSerial*0x85EBCA6Bu);if(!nonce)nonce=nonceSerial;
    state.Bind(session.sessionId,session.ticket,session.role==coop::Role::Host?1:2,nonce);
    ClearMailbox();lastRead=0;maskKnown=false;refreshPending=true;
}
static uint64_t Mask(const Live& live,bool known,float minimum){
    uint64_t mask=0;for(unsigned i=0;i<live.count;++i)
        if(live.rules[i].stage==live.context.stage&&known&&minimum>=live.rules[i].minimum)mask|=uint64_t(1)<<i;
    return mask;
}
static void Trace(const Live& live,bool known,float local,float minimum,uint64_t mask){
    char text[320];std::snprintf(text,sizeof(text),
        "[VISUAL_GATE] campaign=%08X stage=%u catalog=%08X localStars=%.4f paired=%u effectiveStars=%.4f eligibleMask=%016llX; personal car/stars/cash unchanged.\r\n",
        live.context.campaign,unsigned(live.context.stage),live.context.catalog,double(local),unsigned(known),
        double(known?minimum:-1),static_cast<unsigned long long>(mask));LogMessage(text);
}
// Protect NEW stage imports too: max(stage)/OR(completed) must not evade the
// native star gate. Already earned/bootstrap progress is never rolled back.
static bool MayMerge(const std::vector<uint8_t>& before,const coop_career::Progress& next){
    if(!installed||!OwnCareer())return true;
    if(before.size()!=coop_career::kNativePayloadSize)return false;
    const uint32_t oldStage=coop_career::Read32(before.data()+coop_career::kStageIndexOffset);
    if(oldStage>=8||next.stageIndex>=8)return false;
    const uint32_t established=CoopPersonalSave::haveCommon?CoopPersonalSave::common.stageIndex:oldStage;
    bool check[8]{},needed=false;
    for(unsigned i=oldStage;i<8;++i){
        const bool wasComplete=before[coop_career::kCareerOffset+i*coop_career::kStageStride+coop_career::kStageCompletedOffset]!=0;
        // Initial Resume explicitly adopts established team history, including
        // a newly joined player who is still choosing their personal starter.
        // Do not demand today's car rating again for that already-earned past.
        if(i<established||(CoopPersonalSave::haveCommon&&CoopPersonalSave::common.stages[i].completed))continue;
        if((i<next.stageIndex)||(next.stages[i].completed&&!wasComplete)){check[i]=true;needed=true;}
    }
    if(!needed)return true;
    Live live;if(!ReadLive(live))return false;
    float local=0,minimum=0;const bool paired=ReadStars(local)&&Effective(live,local,minimum);
    for(unsigned i=0;i<live.count;++i){const auto& rule=live.rules[i];
        if(!check[rule.stage]||!rule.mandatory||coop_career::Completed(before,rule.hash))continue;
        if(!paired||minimum<rule.minimum)return false;
    }
    return true;
}
} // namespace CoopVisualGateNative
bool CoopVisualMayMerge(const std::vector<uint8_t>& before,const coop_career::Progress& next){return CoopVisualGateNative::MayMerge(before,next);}
bool CoopVisualNeedsRefresh(){return CoopVisualGateNative::installed&&CoopVisualGateNative::refreshPending&&CoopVisualGateNative::OwnCareer();}
bool CoopVisualRefreshGate(){
    using namespace CoopVisualGateNative;if(!CoopVisualNeedsRefresh())return true;
    Live live;if(!ReadLive(live))return false;
    // Called only inside PersonalSave's existing safe Free Roam map boundary:
    // flow6, track4000, RF4, no race transition and no timed mission.
    refresh(reinterpret_cast<void*>(addresses.career));refreshPending=false;return true;
}
void TickCoopVisualUi(DWORD flow){
    using namespace CoopVisualGateNative;coop::SessionSnapshot session;
    if(!Scope(session)){
        if(state.Session()){state.Unbind();ClearMailbox();maskKnown=false;refreshPending=OwnCareer();}
        return;
    }
    Bind(session);const DWORD now=GetTickCount();
    if(lastRead&&now-lastRead<200)return;lastRead=now;
    Live live;const bool have=ReadLive(live);float local=0;coop_visual::Sample sample{};
    if(have&&live.slot&&(flow==3||flow==6)&&CoopPersonalSave::OffersScope()&&ReadStars(local))sample={local,live.slot,true};
    // Withdraw a previously advertised car immediately on an unreadable local
    // profile/catalog. Do not leave the peer relying on the last valid sample
    // until its normal TTL, and do not advertise a new guessed campaign.
    state.SetLocal(have?live.context:state.Current(),sample);
    std::array<Received,16> received;size_t count=0;
    AcquireSRWLockExclusive(&mailboxLock);count=inboxCount;std::copy_n(inbox.begin(),count,received.begin());inboxCount=0;ReleaseSRWLockExclusive(&mailboxLock);
    for(size_t i=0;i<count;++i)state.Receive(received[i].packet,received[i].at);
    if(have){float minimum=0;const bool known=sample.valid&&Effective(live,local,minimum);const uint64_t mask=Mask(live,known,minimum);
        if(!maskKnown||!coop_visual::Same(maskContext,live.context)||lastMask!=mask){
            refreshPending=true;maskKnown=true;maskContext=live.context;lastMask=mask;Trace(live,known,local,minimum,mask);
        }
    }else if(maskKnown){maskKnown=false;refreshPending=true;}
    coop_visual::Packet packet;
    if(state.Poll(now,packet)){AcquireSRWLockExclusive(&mailboxLock);outbound=packet;haveOutbound=true;ReleaseSRWLockExclusive(&mailboxLock);}
}
bool ReceiveCoopVisualPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now){
    using namespace CoopVisualGateNative;uint32_t magic=0;if(!data||bytes<4)return false;
    std::memcpy(&magic,data,4);if(magic!=coop_visual::kMagic)return false;
    if(bytes!=sizeof(coop_visual::Packet)||sender.sin_family!=AF_INET||!IsJoinedCoopSender(sender))return true;
    coop_visual::Packet packet{};std::memcpy(&packet,data,sizeof(packet));const auto session=CoopSnapshot();
    if(!coop_visual::Valid(packet)||packet.session!=session.sessionId||packet.ticket!=session.ticket||
        packet.player!=(session.role==coop::Role::Host?2:1))return true;
    AcquireSRWLockExclusive(&mailboxLock);if(inboxCount<inbox.size())inbox[inboxCount++]={packet,now};ReleaseSRWLockExclusive(&mailboxLock);return true;
}
void TickCoopVisualNetwork(DWORD now){
    using namespace CoopVisualGateNative;(void)now;const auto session=CoopSnapshot();
    if(!session.joined||!session.fresh||!session.peer.Valid())return;
    coop_visual::Packet packet{};bool send=false;AcquireSRWLockExclusive(&mailboxLock);
    if(haveOutbound&&outbound.sequence!=lastSent&&outbound.session==session.sessionId&&outbound.ticket==session.ticket){
        packet=outbound;lastSent=packet.sequence;send=true;}
    ReleaseSRWLockExclusive(&mailboxLock);if(send)SendSessionBytes(&packet,sizeof(packet),session.peer);
}
bool InstallCoopVisualGate(){
    using namespace CoopVisualGateNative;if(installed)return true;BYTE bytes[5]{};
    if(!ReadGameMemory(callSite,bytes,5)||std::memcmp(bytes,originalCall,5)){
        LogMessage("[VISUAL_GATE] Native DVD rating call differs; hook not installed.\r\n");return false;}
    DWORD old=0,ignored=0;if(!VirtualProtect(reinterpret_cast<void*>(callSite),5,PAGE_EXECUTE_READWRITE,&old))return false;
    BYTE patch[5]{0xE8};const int32_t rel=static_cast<int32_t>(reinterpret_cast<uintptr_t>(&Gate)-(callSite+5));std::memcpy(patch+1,&rel,4);
    std::memcpy(reinterpret_cast<void*>(callSite),patch,5);FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(callSite),5);
    VirtualProtect(reinterpret_cast<void*>(callSite),5,old,&ignored);installed=true;
    LogMessage("[VISUAL_GATE] Both-player current-car minimum enforced at native DVD eligibility only; display/payout and personal modifications untouched.\r\n");return true;
}
