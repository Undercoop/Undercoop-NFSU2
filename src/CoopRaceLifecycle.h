// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace coop_race {
constexpr uint32_t kMagic = 0x5132434e; // NC2Q: session-scoped repeated race state.
constexpr uint16_t kVersion=4;
enum Flags : uint16_t { Ready=1, DeadlineAck=2, StartCommit=4, Finished=8,
    Result=16, ResultAck=32, CareerCommitted=64, ClockReady=128,
    CountdownReady=256, CountdownDeadlineAck=512, CountdownCommit=1024 };
#pragma pack(push,1)
struct Finish { uint32_t ticks=0; uint16_t status=0; uint8_t rank=0, valid=0;
    float driftScore=0;uint8_t drift=0; };
struct TimedOpponent { uint32_t ticks=0; uint16_t status=0,id=0; };
struct Packet {
    uint32_t magic=kMagic; uint16_t version=kVersion, size=sizeof(Packet);
    uint64_t session=0; uint32_t token=0, track=0, sequence=0;
    uint32_t sentAt=0, echoSent=0, echoReceived=0, deadline=0;
    uint16_t flags=0, player=0; Finish finish[2]{};
    uint8_t driftSealed=0,driftMask=0;float driftAi[5]{};
    uint32_t countdownDeadline=0;
    uint8_t timedSealed=0,timedCount=0;
    TimedOpponent timedAi[4]{};
};
#pragma pack(pop)
static_assert(sizeof(Packet)==134,"Race state v4 wire layout changed");
constexpr uint16_t kUnfinishedStatus=2;
inline bool TimedSuccess(uint16_t status,uint32_t ticks) {
    return (status==1 || status==6) && ticks>0 && ticks<0x7fffffffu;
}
inline bool TimedBefore(uint16_t aStatus,uint32_t aTicks,uint16_t aId,
    uint16_t bStatus,uint32_t bTicks,uint16_t bId) {
    const bool a=TimedSuccess(aStatus,aTicks),b=TimedSuccess(bStatus,bTicks);
    if(a!=b)return a;
    if(a && aTicks!=bTicks)return aTicks<bTicks;
    return aId<bId;
}
inline bool ValidTimedOpponents(unsigned count,const TimedOpponent* opponents) {
    if(count>4 || !opponents)return false;
    for(unsigned i=0;i<4;++i) {
        const auto& row=opponents[i];
        if(i>=count) {if(row.id || row.status || row.ticks)return false;continue;}
        if(row.id<2 || (i && row.id<=opponents[i-1].id) || !row.status || row.status>12 ||
            ((row.status==1 || row.status==6) && !TimedSuccess(row.status,row.ticks)))return false;
    }
    return true;
}
inline bool ValidFinish(const Finish& f) { return f.valid==1 && f.rank>=1 && f.rank<=22 && f.status!=0 &&
    f.drift<=1 && std::isfinite(f.driftScore) && f.driftScore>=0 && f.driftScore<=1.0e9f && (f.drift || f.driftScore==0); }
inline bool Same(const Finish& a,const Finish& b) { return !std::memcmp(&a,&b,sizeof(a)); }
inline uint32_t CountdownOrigin(uint32_t simulationTick,uint32_t now,uint32_t localDeadline) {
    // 86518C counts quarter milliseconds. 0 and INT_MAX are native "unset"
    // sentinels, so avoid them by one quarter-ms without moving the deadline.
    uint32_t origin=simulationTick-(now-localDeadline)*4u;
    if(!origin || origin==0x7fffffffu)--origin;
    return origin;
}
class State {
public:
    uint64_t session=0; uint32_t token=0,track=0;
    bool host=false,ready=false,peerReady=false,clockReady=false,startCommit=false;
    bool deadlineAck=false,resultAck=false,careerCommitted=false;
    uint32_t deadline=0,peerSent=0,peerReceived=0,sequence=0,lastSequence=0;
    int32_t remoteOffset=0; uint32_t bestRtt=0xffffffffu;
    // The NPC starter and actual HUD 3/2/1 are separate native boundaries.
    // One PC finishing its starter later must not restart its own three seconds.
    bool countdownReady=false,peerCountdownReady=false,countdownDeadlineAck=false,countdownCommit=false;
    uint32_t countdownDeadline=0;
    int32_t starterOffset=0,countdownOffset=0;
    Finish finish[2]{};
    bool driftSealed=false;uint8_t driftMask=0;float driftAi[5]{};
    bool timed=false,timedSealed=false;uint8_t timedCount=0;TimedOpponent timedAi[4]{};
    void Begin(uint64_t s,uint32_t t,uint32_t tr,bool h,bool timedResult=false) {
        *this=State{};session=s;token=t;track=tr;host=h;timed=timedResult;
    }
    bool Capture(unsigned player,uint8_t rank,uint32_t ticks,uint16_t status,float score=0,bool drift=false) {
        if(player>1 || !session || !token || finish[player].valid || rank<1 || rank>22 || !status) return false;
        Finish next{ticks,status,rank,1,score,uint8_t(drift)};if(!ValidFinish(next))return false;
        finish[player]=next;return true;
    }
    bool BothFinished() const { return ValidFinish(finish[0]) && ValidFinish(finish[1]); }
    bool Drift() const {return BothFinished() && finish[0].drift && finish[1].drift;}
    bool ResultReady() const { return BothFinished() && resultAck && (!Drift() || driftSealed) && (!timed || timedSealed); }
    bool SealTimed(uint8_t count,const TimedOpponent* rows) {
        if(!host || !timed || timedSealed || !BothFinished() || Drift() || !ValidTimedOpponents(count,rows))return false;
        timedCount=count;std::copy_n(rows,4,timedAi);timedSealed=true;return true;
    }
    uint8_t TimedRank(uint16_t id,uint16_t status,uint32_t ticks) const {
        if(!timedSealed || !BothFinished())return 0;
        uint8_t rank=1;
        for(unsigned i=0;i<2;++i)if(TimedBefore(finish[i].status,finish[i].ticks,uint16_t(i),status,ticks,id))++rank;
        for(unsigned i=0;i<timedCount;++i) {
            const auto& row=timedAi[i];
            if(TimedBefore(row.status,row.ticks,row.id,status,ticks,id))++rank;
        }
        return rank;
    }
    bool DriftScore(unsigned slot,float& score) const {
        if(!Drift() || !driftSealed)return false;
        if(slot<2) {score=finish[slot].driftScore;return true;}
        if(slot>=7 || !(driftMask&(1u<<(slot-2))))return false;
        score=driftAi[slot-2];return true;
    }
    uint8_t DriftRank(unsigned slot) const {
        float score=0;
        if(!DriftScore(slot,score))return 0;
        unsigned rank=1;
        for(unsigned other=0;other<7;++other) {
            float candidate=0;
            if(DriftScore(other,candidate) && (candidate>score || (candidate==score && other<slot)))++rank;
        }
        return static_cast<uint8_t>(rank);
    }
    bool SealDrift(uint8_t mask,const float* scores) {
        if(!host || !Drift() || driftSealed || (mask&~31u) || !scores)return false;
        for(unsigned i=0;i<5;++i)if(!std::isfinite(scores[i]) || scores[i]<0 || scores[i]>1.0e9f || (!(mask&(1u<<i)) && scores[i]!=0))return false;
        driftMask=mask;std::copy_n(scores,5,driftAi);driftSealed=true;return true;
    }
    Finish Team() const {
        if(!BothFinished()) return {};
        const auto& a=finish[0]; const auto& b=finish[1];
        if(timedSealed) {
            const uint16_t id=TimedBefore(b.status,b.ticks,1,a.status,a.ticks,0)?1:0;
            auto best=finish[id];best.rank=TimedRank(id,best.status,best.ticks);
            if(!TimedSuccess(best.status,best.ticks)) {
                // Two DNFs can occupy the first row when nobody completed.
                // A display position of one cannot award a career win.
                best.rank=(std::max)(uint8_t(2),best.rank);
                if(best.status==1 || best.status==6)best.status=kUnfinishedStatus;
            }
            return best;
        }
        if(Drift()) {const unsigned player=b.driftScore>a.driftScore?1:0;Finish best=finish[player];
            if(driftSealed)best.rank=DriftRank(player);return best;}
        return b.rank<a.rank || (b.rank==a.rank && b.ticks<a.ticks)?b:a;
    }
    bool LocalDeadline(uint32_t& out) const {
        if(!startCommit || !deadline || (!host && !clockReady)) return false;
        out=host?deadline:deadline-static_cast<uint32_t>(starterOffset);return true;
    }
    bool LocalCountdownDeadline(uint32_t& out) const {
        if(!countdownCommit || !countdownReady || !countdownDeadline || (!host && !clockReady))return false;
        out=host?countdownDeadline:countdownDeadline-static_cast<uint32_t>(countdownOffset);return true;
    }
    Packet Build(uint32_t now) {
        // Give the peer time to acknowledge and receive the repeated commit.
        if(host && ready && peerReady && clockReady && !startCommit) {
            if(!deadline || static_cast<int32_t>(deadline-now)<500) {
                deadline=now+1800;deadlineAck=false;
            }
            if(deadlineAck) startCommit=true;
        }
        if(host && startCommit && countdownReady && peerCountdownReady && clockReady && !countdownCommit) {
            if(!countdownDeadline || static_cast<int32_t>(countdownDeadline-now)<500) {
                countdownDeadline=now+1800;countdownDeadlineAck=false;
            }
            if(countdownDeadlineAck)countdownCommit=true;
        }
        Packet p; p.session=session;p.token=token;p.track=track;p.player=host?1:2;
        p.sequence=++sequence;p.sentAt=now;p.echoSent=peerSent;p.echoReceived=peerReceived;
        p.deadline=deadline;p.flags=(ready?Ready:0)|(clockReady?ClockReady:0)|
            (deadlineAck?DeadlineAck:0)|(startCommit?StartCommit:0)|
            (finish[host?0:1].valid?Finished:0)|((BothFinished() && (!Drift() || driftSealed) && (!timed || timedSealed))?Result:0)|
            (resultAck?ResultAck:0)|(careerCommitted?CareerCommitted:0)|
            (countdownReady?CountdownReady:0)|(countdownDeadlineAck?CountdownDeadlineAck:0)|
            (countdownCommit?CountdownCommit:0);
        p.finish[0]=finish[0];p.finish[1]=finish[1];p.driftSealed=driftSealed?1:0;p.driftMask=driftMask;
        std::copy_n(driftAi,5,p.driftAi);p.countdownDeadline=countdownDeadline;
        p.timedSealed=timedSealed?1:0;p.timedCount=timedCount;std::copy_n(timedAi,4,p.timedAi);return p;
    }
    bool Receive(const Packet& p,uint32_t now) {
        if(p.magic!=kMagic || p.version!=kVersion || p.size!=sizeof(Packet) || p.driftSealed>1 || (p.driftMask&~31u) ||
            p.timedSealed>1 || !ValidTimedOpponents(p.timedCount,p.timedAi) ||
            (!p.timedSealed && p.timedCount) || (p.timedSealed && (!timed || p.driftSealed || !(p.flags&Result))) ||
            (p.flags&~uint16_t(Ready|DeadlineAck|StartCommit|Finished|Result|ResultAck|CareerCommitted|ClockReady|
                CountdownReady|CountdownDeadlineAck|CountdownCommit)) ||
            ((p.flags&CountdownReady) && (p.flags&(Ready|StartCommit))!=(Ready|StartCommit)) ||
            ((p.flags&CountdownDeadlineAck) && (!p.countdownDeadline ||
                (p.flags&(CountdownReady|ClockReady))!=(CountdownReady|ClockReady))) ||
            ((p.flags&CountdownCommit) && (!p.countdownDeadline ||
                (p.flags&(CountdownReady|ClockReady|StartCommit))!=(CountdownReady|ClockReady|StartCommit))) ||
            p.session!=session || p.token!=token || p.track!=track ||
            p.player!=(host?2:1) || !p.sequence ||
            (lastSequence && static_cast<int32_t>(p.sequence-lastSequence)<=0)) return false;
        for(unsigned i=0;i<5;++i)if(!std::isfinite(p.driftAi[i]) || p.driftAi[i]<0 || p.driftAi[i]>1.0e9f ||
            (!(p.driftMask&(1u<<i)) && p.driftAi[i]!=0) || (!p.driftSealed && p.driftAi[i]!=0))return false;
        if(!p.driftSealed && p.driftMask)return false;
        for(const auto& f:p.finish)if(f.valid && !ValidFinish(f))return false;
        if(p.driftSealed && (!ValidFinish(p.finish[0]) || !ValidFinish(p.finish[1]) || !p.finish[0].drift || !p.finish[1].drift))return false;
        lastSequence=p.sequence;peerSent=p.sentAt;peerReceived=now;
        // NTP four-timestamp estimate. No cross-PC GetTickCount epoch assumption.
        if(p.echoSent && p.echoReceived) {
            const int32_t rtt=static_cast<int32_t>(now-p.echoSent)-static_cast<int32_t>(p.sentAt-p.echoReceived);
            if(rtt>=0 && rtt<=2000 && static_cast<uint32_t>(rtt)<=bestRtt) {
                bestRtt=rtt;remoteOffset=static_cast<int32_t>((
                    static_cast<int64_t>(static_cast<int32_t>(p.echoReceived-p.echoSent))+
                    static_cast<int32_t>(p.sentAt-now))/2);clockReady=true;
            }
        }
        peerReady=(p.flags&Ready)!=0;
        peerCountdownReady=(p.flags&CountdownReady)!=0;
        const unsigned peer=host?1:0;
        if((p.flags&Finished) && ValidFinish(p.finish[peer]) && !finish[peer].valid) finish[peer]=p.finish[peer];
        if(host) {
            if(deadline && p.deadline==deadline && (p.flags&DeadlineAck)) deadlineAck=true;
            if(countdownDeadline && p.countdownDeadline==countdownDeadline && peerCountdownReady &&
                (p.flags&CountdownDeadlineAck))countdownDeadlineAck=true;
            if((p.flags&ResultAck) && BothFinished() && Same(p.finish[0],finish[0]) && Same(p.finish[1],finish[1]) &&
                (!timed || (timedSealed && p.timedSealed && p.timedCount==timedCount && !std::memcmp(p.timedAi,timedAi,sizeof(timedAi)))) &&
                (!Drift() || (driftSealed && p.driftSealed && p.driftMask==driftMask && !std::memcmp(p.driftAi,driftAi,sizeof(driftAi))))) resultAck=true;
        } else {
            if(p.deadline && !startCommit) { deadline=p.deadline;deadlineAck=clockReady; }
            if(!startCommit && (p.flags&StartCommit) && p.deadline==deadline && clockReady) {
                starterOffset=remoteOffset;startCommit=true;
            }
            if(p.countdownDeadline && !countdownCommit && startCommit && (p.flags&StartCommit)) {
                countdownDeadline=p.countdownDeadline;countdownDeadlineAck=clockReady&&countdownReady;
            }
            if(!countdownCommit && (p.flags&CountdownCommit) && p.countdownDeadline==countdownDeadline &&
                clockReady && countdownReady) {
                countdownOffset=remoteOffset;countdownCommit=true;
            }
            if((p.flags&Result) && ValidFinish(p.finish[0]) && ValidFinish(p.finish[1]) &&
                (!timed || p.timedSealed) && (!timedSealed || (p.timedCount==timedCount && !std::memcmp(p.timedAi,timedAi,sizeof(timedAi)))) &&
                (!finish[1].valid || Same(finish[1],p.finish[1])) &&
                (!(p.finish[0].drift || p.finish[1].drift) || (p.finish[0].drift && p.finish[1].drift && p.driftSealed))) {
                finish[0]=p.finish[0];finish[1]=p.finish[1];
                driftSealed=p.driftSealed!=0;driftMask=p.driftMask;std::copy_n(p.driftAi,5,driftAi);
                timedSealed=p.timedSealed!=0;timedCount=p.timedCount;std::copy_n(p.timedAi,4,timedAi);resultAck=true;
            }
        }
        return true;
    }
};
}
