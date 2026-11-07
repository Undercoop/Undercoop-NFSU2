// Copyright (C) 2026 batuhan.
// GNU GPL version 3 only, with LICENSE-7ZIP-EXCEPTION.txt.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../src/CoopTransport.h"
#include "../src/CoopTime.h"
#include <cstdio>
#include <cstdlib>
static unsigned checks=0;
static void Check(bool value,const char* name){++checks;if(!value){std::fprintf(stderr,"FAIL: %s\n",name);std::exit(1);}}
int main(){
    coop_transport::Transport host,client;const coop::Endpoint h{0x0100007Fu,51070},c{0x0100007Fu,51071};
    Check(host.Open(h.port)&&client.Open(c.port),"loopback transports open");
    auto connect=[&](unsigned nonce){
        auto now=GetTickCount();host.SetPeer(42,nonce,c,true,now);client.SetPeer(42,nonce,h,false,now);
        Check(!host.Snapshot().rttMeasured&&!client.Snapshot().rttMeasured,"new link has no inherited RTT");
        const auto start=now;
        while(coop_time::Elapsed(GetTickCount(),start)<5000){
            now=GetTickCount();host.Poll(now);client.Poll(now);
            const auto hs=host.Snapshot(),cs=client.Snapshot();
            if(host.Ready(42,nonce,c)&&client.Ready(42,nonce,h)&&hs.rttMeasured&&cs.rttMeasured){
                Check(coop_time::Elapsed(now,hs.rttSampleAtMs)<2500,"Host measurement uses local Poll clock");
                Check(coop_time::Elapsed(now,cs.rttSampleAtMs)<2500,"Client measurement uses local Poll clock");
                return;
            }
            Sleep(1);
        }
        Check(false,"actual ENet ACKs produce measured RTT on both peers");
    };
    connect(7);
    auto now=GetTickCount();host.SetPeer(0,0,{},true,now);client.SetPeer(0,0,{},false,now);
    Check(!host.Snapshot().rttMeasured&&!client.Snapshot().rttMeasured,"disconnect retires measured RTT");
    connect(8);host.Close();client.Close();
    Check(!host.Snapshot().rttMeasured&&!client.Snapshot().rttMeasured,"close retires measured RTT");
    std::printf("Connection RTT loopback: %u checks passed.\n",checks);
}
