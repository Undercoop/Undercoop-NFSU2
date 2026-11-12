// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#include "../src/CoopModConsent.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
namespace mc=coop_mod_consent;
static unsigned checks=0;
static void Check(bool v,const char* text,int line){++checks;if(!v){std::printf("FAIL %d %s\n",line,text);std::exit(1);}}
#define CHECK(x) Check(bool(x),#x,__LINE__)
struct Pair {
    mc::Consent a,b;mc::Binding ka,kb;uint32_t now=1;
    coop::Endpoint alice{0x0100007F,48940},bob{0x0100007F,48941};
    Pair(){
        ka.session=kb.session=12345;ka.membership=kb.membership=987;
        ka.localToken=kb.peerToken=13;ka.peerToken=kb.localToken=29;
        ka.localBytes=kb.peerBytes=100;ka.peerBytes=kb.localBytes=200;
        ka.localHash.fill(1);ka.peerHash.fill(2);kb.localHash=ka.peerHash;kb.peerHash=ka.localHash;
        ka.peer=bob;kb.peer=alice;Bind();
    }
    void Bind(){a.Bind(ka);b.Bind(kb);a.SetEligible(true);b.SetEligible(true);}
    void Pump(unsigned rounds=8,unsigned loss=0){
        for(unsigned i=0;i<rounds;++i){
            mc::Packet pa,pb;const bool sa=a.Tick(now,pa),sb=b.Tick(now,pb);now+=251;
            if(sa&&(!loss||i%loss))b.Receive(pa,alice);
            if(sb&&(!loss||(i+1)%loss))a.Receive(pb,bob);
        }
    }
    mc::Packet FromA(){now+=251;mc::Packet p;CHECK(a.Tick(now,p));return p;}
    mc::Packet FromB(){now+=251;mc::Packet p;CHECK(b.Tick(now,p));return p;}
    void Accept(){Pump();CHECK(a.CanDecide());CHECK(b.CanDecide());CHECK(a.Decide(true));CHECK(b.Decide(true));Pump();CHECK(a.Ready());CHECK(b.Ready());}
};
static void Basic(){
    Pair p;CHECK(p.a.Bound());CHECK(!p.a.CanDecide());CHECK(!p.a.Decide(true));CHECK(!p.a.Ready());
    p.Pump();CHECK(p.a.CanDecide());CHECK(p.b.CanDecide());CHECK(!p.a.Ready());CHECK(!p.b.Ready());
    CHECK(p.a.Decide(true));CHECK(!p.a.Decide(true));p.Pump();CHECK(!p.a.Ready());CHECK(!p.b.Ready());
    CHECK(p.b.Decide(true));CHECK(!p.b.Ready());
    auto pb=p.FromB();CHECK(p.a.Receive(pb,p.bob));CHECK(!p.b.Ready());
    auto pa=p.FromA();CHECK(p.b.Receive(pa,p.alice));CHECK(p.a.Ready());CHECK(p.b.Ready());
    CHECK(p.a.Receive(pb,p.bob));CHECK(p.a.Ready()); // duplicate is harmless
}
static void LossAndReorder(){
    Pair p;p.Pump(60,2);CHECK(p.a.CanDecide());CHECK(p.b.CanDecide());
    auto pending=p.FromA();CHECK(p.a.Decide(true));auto accepted=p.FromA();
    CHECK(p.b.Receive(accepted,p.alice));CHECK(!p.b.Receive(pending,p.alice));CHECK(!p.b.Ready());
    CHECK(p.b.Decide(true));p.Pump(70,3);CHECK(p.a.Ready());CHECK(p.b.Ready());
    auto oldAccept=p.FromA();CHECK(p.a.Decide(false));auto decline=p.FromA();CHECK(p.b.Receive(decline,p.alice));
    CHECK(!p.b.Ready());CHECK(p.b.Declined());CHECK(!p.b.Receive(oldAccept,p.alice));CHECK(!p.b.Decide(true));
    p.Pump(8);CHECK(!p.a.Ready());CHECK(!p.b.Ready());CHECK(!p.a.Decide(true));
}
static void Rebinding(){
    Pair p;p.Accept();auto oldA=p.FromA(),oldB=p.FromB();
    p.ka.membership++;p.kb.membership++;p.Bind();CHECK(!p.a.Ready());CHECK(!p.b.Ready());
    CHECK(!p.a.Receive(oldB,p.bob));CHECK(!p.b.Receive(oldA,p.alice));p.Accept();
    oldA=p.FromA();oldB=p.FromB();p.ka.session++;p.kb.session++;p.Bind();
    CHECK(!p.a.Receive(oldB,p.bob));CHECK(!p.b.Receive(oldA,p.alice));CHECK(!p.a.Ready());p.Accept();
    oldA=p.FromA();oldB=p.FromB();p.ka.localToken++;p.kb.peerToken++;p.Bind();
    CHECK(!p.a.Receive(oldB,p.bob));CHECK(!p.b.Receive(oldA,p.alice));p.Accept();
    oldA=p.FromA();oldB=p.FromB();const auto ka=p.ka,kb=p.kb;
    p.ka.localHash.fill(3);p.kb.peerHash=p.ka.localHash;p.Bind();
    CHECK(!p.a.Receive(oldB,p.bob));CHECK(!p.b.Receive(oldA,p.alice));p.Accept();
    p.ka=ka;p.kb=kb;p.Bind(); // Back to same hashes must require a NEW round.
    CHECK(!p.a.Receive(oldB,p.bob));CHECK(!p.b.Receive(oldA,p.alice));CHECK(!p.a.Ready());CHECK(!p.b.Ready());p.Accept();
    oldB=p.FromB();p.a.Reset();p.a.Bind(p.ka);p.a.SetEligible(true);CHECK(!p.a.Receive(oldB,p.bob));CHECK(!p.a.Ready());
}
static void InitialReplayRecovery(){
    Pair p;const auto oldA=p.FromA(),oldB=p.FromB();
    p.Pump();CHECK(p.a.CanDecide());
    const auto ka=p.ka,kb=p.kb;
    p.ka.localHash.fill(4);p.kb.peerHash=p.ka.localHash;p.Bind();p.Pump();
    p.ka=ka;p.kb=kb;p.Bind();
    CHECK(p.a.Receive(oldB,p.bob));CHECK(p.b.Receive(oldA,p.alice)); // stale pending carries NO approval
    CHECK(!p.a.CanDecide());CHECK(!p.b.CanDecide());p.Pump(24);
    CHECK(p.a.CanDecide());CHECK(p.b.CanDecide());CHECK(!p.a.Ready());CHECK(!p.b.Ready());p.Accept();
}
static void GatesAndMalformed(){
    Pair p;p.a.SetEligible(false);CHECK(!p.a.Decide(true));auto pb=p.FromB();CHECK(!p.a.Receive(pb,p.bob));
    mc::Packet blocked;CHECK(!p.a.Tick(1000,blocked));p.a.SetEligible(true);p.Accept();
    p.a.SetEligible(false);CHECK(!p.a.Ready());CHECK(!p.a.Decide(true));CHECK(!p.a.Receive(p.FromB(),p.bob));
    p.a.SetEligible(true);CHECK(p.a.Ready()); // same files and all gates genuinely recovered
    auto packet=p.FromB();CHECK(mc::Valid(packet));
    auto bad=packet;bad.magic^=1;CHECK(!mc::Valid(bad));CHECK(!p.a.Receive(bad,p.bob));
    bad=packet;bad.version=1;CHECK(!mc::Valid(bad));
    bad=packet;bad.size--;CHECK(!mc::Valid(bad));
    bad=packet;bad.reserved[1]=1;CHECK(!mc::Valid(bad));
    bad=packet;bad.choice=static_cast<mc::Choice>(3);CHECK(!mc::Valid(bad));
    bad=packet;bad.round=0;CHECK(!mc::Valid(bad));
    bad=packet;bad.echoRound=0;CHECK(!mc::Valid(bad));
    bad=packet;bad.ack=UINT32_MAX;CHECK(!p.a.Receive(bad,p.bob));
    bad=packet;bad.membership++;CHECK(!p.a.Receive(bad,p.bob));
    bad=packet;bad.echo++;CHECK(!p.a.Receive(bad,p.bob));
    bad=packet;bad.token++;CHECK(!p.a.Receive(bad,p.bob));
    bad=packet;bad.localHash[1]++;CHECK(!p.a.Receive(bad,p.bob));
    bad=packet;bad.peerHash[1]++;CHECK(!p.a.Receive(bad,p.bob));
    bad=packet;bad.localBytes++;CHECK(!p.a.Receive(bad,p.bob));
    bad=packet;bad.echoRound++;CHECK(!p.a.Receive(bad,p.bob));
    CHECK(!p.a.Receive(packet,{p.bob.address,uint16_t(p.bob.port+1)}));CHECK(p.a.Ready());
    auto invalid=p.ka;invalid.peer={};p.a.Bind(invalid);CHECK(!p.a.Bound());CHECK(!p.a.Ready());CHECK(!p.a.Decide(true));
}
static void MatchedIsNotMismatch(){
    Pair p;auto binding=p.ka;binding.peerHash=binding.localHash;binding.peerBytes=binding.localBytes;
    CHECK(!binding.Valid());p.a.Bind(binding);p.a.SetEligible(true);CHECK(!p.a.Bound());CHECK(!p.a.Decide(true));
}
int main(){Basic();LossAndReorder();Rebinding();InitialReplayRecovery();GatesAndMalformed();MatchedIsNotMismatch();
    std::printf("Mod mismatch consent: %u checks, 0 failures.\n",checks);return 0;}
