// Copyright (C) 2026 batuhan.
// GNU GPL version 3 only, with LICENSE-7ZIP-EXCEPTION.txt.
#include "../src/CoopConnectionHud.h"
#include <cstdlib>
#include <cstring>
static unsigned checks=0;
static void Check(bool value,const char* name){++checks;if(!value){std::fprintf(stderr,"FAIL: %s\n",name);std::exit(1);}}
int main(){
    using namespace coop_connection_hud;
    coop::SessionSnapshot s{};coop_transport::Statistics w{};Model m;
    Check(!m.Update(s,w,false,false,0).visible,"singleplayer has no misleading connection");
    s.sessionId=1;
    Check(m.Update(s,w,false,false,1).status==Status::Waiting,"host waits for teammate");
    s.joined=s.fresh=true;
    Check(m.Update(s,w,false,true,2).status==Status::Connecting,"discovery join alone is not a gameplay link");
    w.ready=w.connected=true;w.rttMs=500;
    auto v=m.Update(s,w,true,true,3);
    Check(v.status==Status::Measuring&&v.rgb==kOrange,"ENet default500 is not a measured ping");
    w.rttMeasured=true;w.rttSampleAtMs=10;
    for(auto ping:{0u,1u,100u,101u,200u,201u,2000u}){
        w.rttMs=ping;v=m.Update(s,w,true,true,11);
        Check(v.status==Status::Connected,"fresh ACK produces a live value");
        Check(v.rgb==(ping<=100?kGreen:ping<=200?kOrange:kRed),"quality thresholds");
        Check(std::strstr(v.text,ping==0?"<1 ms":" ms")!=nullptr,"real submillisecond vs measured value");
    }
    Check(m.Update(s,w,true,true,2509).status==Status::Connected,"sample remains live below deadline");
    Check(m.Update(s,w,true,true,2510).status==Status::Interrupted,"stale ping never remains green");
    w.rttSampleAtMs=2600;w.rttMs=60;
    Check(m.Update(s,w,true,true,2600).rgb==kGreen,"fresh recovery restores valid quality");
    w.connected=w.ready=false;
    Check(m.Update(s,w,false,true,2700).status==Status::Interrupted,"lost gameplay link is visible even with discovery alive");
    s.joined=false;
    Check(m.Update(s,w,false,true,2800).status==Status::Disconnected,"teammate departure is explicit");
    s={};
    Check(m.Update(s,w,false,false,3000).visible,"notice survives session teardown");
    Check(m.Update(s,w,false,false,17999).status==Status::Disconnected,"detached notice retained for fifteen seconds");
    Check(!m.Update(s,w,false,false,18000).visible,"singleplayer eventually clears old notice");
    s.sessionId=2;
    Check(m.Update(s,w,false,false,19000).status==Status::Waiting,"new lobby never inherits departed teammate");
    s.joined=s.fresh=true;w.ready=w.connected=true;w.rttMeasured=false;
    Check(m.Update(s,w,true,true,19001).status==Status::Measuring,"reconnected link must measure again");
    w.rttMeasured=true;w.rttSampleAtMs=0xFFFFFF00u;w.rttMs=12;
    Check(m.Update(s,w,true,true,0x100).status==Status::Connected,"local clock wrap preserves fresh sample");
    w.rttSampleAtMs=500;
    Check(m.Update(s,w,true,true,499).status==Status::Connected,"concurrent future sample is not49-day loss");
    std::printf("Connection HUD: %u checks passed.\n",checks);
}
