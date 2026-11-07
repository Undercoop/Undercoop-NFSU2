// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

// Career U.R.L. identities are independent of each process's native local0.
// Never add the two humans' points or replace one person's earlier heat rank
// by whichever human did better in that heat.
namespace coop_url {
constexpr unsigned kHeats=4,kRacers=6;
#pragma pack(push,1)
struct Context {
    uint32_t eventHash=0,rootEpoch=0,heatEpoch=0,track=0;
    uint8_t index=0,total=0,count=0,reserved=0;
    uint16_t ids[kRacers]{};
};
struct Row {
    uint16_t id=0,status=0;
    uint8_t rank=0,reserved[3]{};
    uint32_t ticks=0;
};
struct Heat {Context context{};Row rows[kRacers]{};};
#pragma pack(pop)
inline bool Same(const Context& a,const Context& b){return !std::memcmp(&a,&b,sizeof(a));}
inline bool Same(const Heat& a,const Heat& b){return !std::memcmp(&a,&b,sizeof(a));}
inline bool Valid(const Context& c) {
    if(!c.eventHash||!c.rootEpoch||!c.heatEpoch||!c.track||c.track==4000||c.reserved||
        !c.total||c.total>kHeats||c.index>=c.total||c.count<2||c.count>kRacers||c.ids[0]!=1||c.ids[1]!=2)return false;
    for(unsigned i=2;i<c.count;++i)if(c.ids[i]<100||c.ids[i]>=105||(i>2&&c.ids[i]<=c.ids[i-1]))return false;
    for(unsigned i=c.count;i<kRacers;++i)if(c.ids[i])return false;
    return true;
}
inline bool Compatible(const Context& a,const Context& b) {
    return a.eventHash==b.eventHash&&a.rootEpoch==b.rootEpoch&&a.total==b.total&&a.count==b.count&&
        !std::memcmp(a.ids,b.ids,sizeof(a.ids));
}
inline bool Valid(const Heat& h) {
    if(!Valid(h.context))return false;unsigned ranks=0;
    for(unsigned i=0;i<h.context.count;++i) {
        const auto& r=h.rows[i];
        if(r.id!=h.context.ids[i]||!r.rank||r.rank>h.context.count||
            r.reserved[0]||r.reserved[1]||r.reserved[2]||(i<2&&!r.status)||(ranks&(1u<<r.rank)))return false;
        ranks|=1u<<r.rank;
    }
    const Row zero{};for(unsigned i=h.context.count;i<kRacers;++i)if(std::memcmp(&h.rows[i],&zero,sizeof(zero)))return false;
    return true;
}
inline uint32_t Points(uint8_t rank) {constexpr uint8_t table[]={0,8,6,4,3,2,1};return rank<=6?table[rank]:0;}

constexpr uint32_t kMagic=0x5552324e; // N2RU: separate from motion/race/save wires.
enum Flags:uint16_t { Acknowledged=1,Committed=2,NextRequested=4,NextAcknowledged=8,NextCommitted=16 };
#pragma pack(push,1)
struct Packet {
    uint32_t magic=kMagic;uint16_t version=1,size=sizeof(Packet);
    uint64_t session=0;uint32_t sequence=0;
    uint16_t player=0,flags=0;
    Context context{};uint8_t mask=0,advancedMask=0;uint16_t reserved=0;
    uint32_t nextEpoch[kHeats]{};
    Heat heats[kHeats]{};
};
#pragma pack(pop)
static_assert(sizeof(Packet)<1200,"URL control must not fragment");
inline bool Valid(const Packet& p) {
    if(p.magic!=kMagic||p.version!=1||p.size!=sizeof(p)||!p.session||!p.sequence||
        (p.player!=1&&p.player!=2)||(p.flags&~31u)||p.reserved||!Valid(p.context)||
        (p.mask&~((1u<<(p.context.index+1))-1))||(p.advancedMask&~p.mask))return false;
    const Heat zero{};
    for(unsigned i=0;i<kHeats;++i) {
        if(p.mask&(1u<<i)) {
            if(!Valid(p.heats[i])||!Compatible(p.context,p.heats[i].context)||p.heats[i].context.index!=i)return false;
        }else if(std::memcmp(&p.heats[i],&zero,sizeof(zero)))return false;
        if(p.nextEpoch[i] && (!(p.mask&(1u<<i))||i+1>=p.context.total||p.nextEpoch[i]==p.heats[i].context.heatEpoch))return false;
        if((p.advancedMask&(1u<<i))&&!p.nextEpoch[i])return false;
    }
    const unsigned prefix=(1u<<p.context.index)-1;
    return (p.mask&prefix)==prefix && (p.advancedMask&prefix)==prefix;
}
class State {
public:
    uint64_t session=0;bool host=false;Context context{};
    Heat heats[kHeats]{};uint8_t mask=0,advancedMask=0;
    uint32_t nextEpoch[kHeats]{},sequence=0,lastSequence=0;
    bool acknowledged=false,committed=false,localNext=false,peerNext=false,nextAck=false,nextCommit=false;
    bool Bind(uint64_t s,bool h,const Context& c) {
        if(!s||!Valid(c))return false;
        if(!session) {
            if(c.index)return false;
            *this=State{};session=s;host=h;context=c;return true;
        }
        if(session!=s||host!=h||!Compatible(context,c))return false;
        if(Same(context,c))return true;
        if(c.index!=context.index+1 || !(advancedMask&(1u<<context.index))||
            nextEpoch[context.index]!=c.heatEpoch)return false;
        context=c;acknowledged=committed=localNext=peerNext=nextAck=nextCommit=false;return true;
    }
    bool Restart(const Context& c) {
        if(!session||!Valid(c)||!Compatible(context,c)||c.index!=context.index||c.heatEpoch==context.heatEpoch)return false;
        const unsigned keep=(1u<<c.index)-1;mask&=keep;advancedMask&=keep;
        for(unsigned i=c.index;i<kHeats;++i){heats[i]={};nextEpoch[i]=0;}
        context=c;acknowledged=committed=localNext=peerNext=nextAck=nextCommit=false;return true;
    }
    bool Seal(const Heat& h) {
        if(!host||!Valid(h)||!Same(h.context,context))return false;
        const unsigned bit=1u<<context.index;
        if(mask&bit)return Same(heats[context.index],h);
        heats[context.index]=h;mask|=bit;return true;
    }
    bool Ready()const{return session&&(mask&(1u<<context.index))&&(host?acknowledged:committed);}
    bool Final()const{return Ready()&&context.index+1==context.total&&mask==((1u<<context.total)-1);}
    bool RequestNext(){if(!Ready()||context.index+1>=context.total)return false;localNext=true;return true;}
    bool OfferNext(uint32_t epoch) {
        if(!host||!Ready()||!localNext||!peerNext||!epoch||epoch==context.heatEpoch||context.index+1>=context.total)return false;
        auto& offered=nextEpoch[context.index];if(offered&&offered!=epoch)return false;offered=epoch;return true;
    }
    bool NextReady()const{return Ready()&&localNext&&peerNext&&nextCommit&&nextEpoch[context.index];}
    bool Advance(){if(!NextReady())return false;advancedMask|=uint8_t(1u<<context.index);return true;}
    uint32_t Total(unsigned racer)const {
        if(racer>=context.count)return 0;uint32_t total=0;
        for(unsigned i=0;i<context.total;++i)if(mask&(1u<<i))total+=Points(heats[i].rows[racer].rank);return total;
    }
    unsigned BestHuman()const{return Total(1)>Total(0)?1:0;}
    uint8_t TeamRank()const {
        if(!Final())return 0;const auto player=BestHuman();const uint32_t score=Total(player);unsigned rank=1;
        for(unsigned i=0;i<context.count;++i)if(i!=player&&(Total(i)>score||(Total(i)==score&&i<player)))++rank;
        // Preserve native per-driver eligibility across the whole tournament.
        for(unsigned h=0;h<context.total;++h)if(heats[h].rows[player].status!=1&&heats[h].rows[player].status!=11)
            rank=std::max(rank,2u);
        return uint8_t(rank);
    }
    Packet Build() {
        Packet p;p.session=session;p.sequence=++sequence;if(!p.sequence)p.sequence=++sequence;
        p.player=host?1:2;p.context=context;p.mask=mask;p.advancedMask=advancedMask;
        p.flags=(acknowledged?Acknowledged:0)|(committed?Committed:0)|(localNext?NextRequested:0)|
            (nextAck?NextAcknowledged:0)|(nextCommit?NextCommitted:0);
        std::copy_n(heats,kHeats,p.heats);std::copy_n(nextEpoch,kHeats,p.nextEpoch);return p;
    }
    bool Receive(const Packet& p) {
        if(!Valid(p)||p.session!=session||p.player!=(host?2:1)||!Compatible(context,p.context)||
            (lastSequence&&int32_t(p.sequence-lastSequence)<=0))return false;
        const unsigned current=context.index,bit=1u<<current,prefix=bit-1;
        if((p.mask&prefix)!=prefix)return false;
        for(unsigned i=0;i<current;++i)if(!Same(heats[i],p.heats[i]))return false;
        // A Host that already loaded the next heat continues proving the old
        // advance in every later packet. Losing the final commit cannot strand
        // the Client on its old results page.
        if(!host&&p.context.index==current+1&&(p.advancedMask&bit)&&(p.mask&bit)&&
            (mask&bit)&&Same(heats[current],p.heats[current])&&p.nextEpoch[current]) {
            nextEpoch[current]=p.nextEpoch[current];peerNext=nextCommit=committed=true;
            lastSequence=p.sequence;return true;
        }
        if(!Same(context,p.context))return false;
        if(host) {
            if((p.flags&Acknowledged)&&(mask&bit)&&(p.mask&bit)&&Same(heats[current],p.heats[current]))acknowledged=committed=true;
            if(Ready()&&(p.flags&NextRequested))peerNext=true;
            if(nextEpoch[current]&&p.nextEpoch[current]==nextEpoch[current]&&(p.flags&NextAcknowledged)&&localNext&&peerNext)
                nextAck=nextCommit=true;
        }else {
            if(p.mask&bit) {
                if((mask&bit)&&!Same(heats[current],p.heats[current]))return false;
                heats[current]=p.heats[current];mask|=bit;acknowledged=true;
            }
            if(acknowledged&&(p.flags&Committed))committed=true;
            if(Ready()&&(p.flags&NextRequested))peerNext=true;
            if(Ready()&&localNext&&peerNext&&p.nextEpoch[current]){nextEpoch[current]=p.nextEpoch[current];nextAck=true;}
            if(nextAck&&(p.flags&NextCommitted)&&nextEpoch[current]==p.nextEpoch[current])nextCommit=true;
        }
        lastSequence=p.sequence;return true;
    }
};
}
