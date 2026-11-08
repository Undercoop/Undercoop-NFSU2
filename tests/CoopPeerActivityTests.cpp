// Copyright (C) 2026 batuhan.
// GNU GPL version 3 only, with LICENSE-7ZIP-EXCEPTION.txt.
#include "../src/CoopPeerActivity.h"
#include "../src/CoopConnectionHud.h"
#include "../src/CoopNetworkPolicy.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
static unsigned checks=0;
static void Check(bool value,const char* name){++checks;if(!value){std::fprintf(stderr,"FAIL: %s\n",name);std::exit(1);}}
int main(){
    using namespace coop_activity;
    for(unsigned i=0;i<unsigned(Scene::Count);++i){
        const auto scene=Scene(i);Check(Valid({scene}),"all local scenes have valid wire representations");
        Check(Label(scene)&&Label(scene)[0],"all scenes have fixed English labels");
    }
    struct Case {const char* screen;Scene scene;};
    for(const auto& c:{Case{"UI_CareerCarLot.fng",Scene::CarLot},{"UI_CareerCrib.fng",Scene::Garage},
        {"PC_OL_Lobby.fng",Scene::Lobby},{"UI_StartCareer.fng",Scene::CareerSelection},
        {"UI_InGame_WorldMap.fng",Scene::WorldMap},{"UI_Pause.fng",Scene::PauseMenu},
        {"UI_PostRace_TournStandings.fng",Scene::Results},{"UI_Sponsorship_new.fng",Scene::Sponsor},
        {"UI_ShowCase_Preview.fng",Scene::Photo},{"UI_MagazineReward.fng",Scene::Magazine},
        {"UI_ChooseUniquePart.fng",Scene::ChoosingReward},{"UI_SMS_Mailbox.fng",Scene::Messages},
        {"UI_Status_Career.fng",Scene::CareerStatus},{"UI_PerformanceDyno_Results.fng",Scene::Dyno},
        {"UI_PerformanceTuning_NOS.fng",Scene::Tuning},{"UI_ChoosePerformancePackage.fng",Scene::PerformanceShop},
        {"UI_Paint.fng",Scene::PaintShop},{"UI_ICEMAIN.fng",Scene::SpecialtiesShop},
        {"UI_GenericParts_Browser.fng",Scene::Customization},{"UI_CareerCarSelect.fng",Scene::CarSelection},
        {"UI_ProfileManager.fng",Scene::SaveLoad},{"UI_PauseOptionsMain.fng",Scene::Options},
        {"UI_ReplayControl.fng",Scene::Replay},{"UI_EngageRaceDialog.fng",Scene::RaceInvitation},
        {"UI_EngageEventDialog.fng",Scene::SpecialInvitation},{"UI_EngageShopDialog.fng",Scene::ShopEntrance}})
        Check(Screen(c.screen)==c.scene,"current native screen mapped");
    Check(Screen("ui_showcase_preview.FNG")==Scene::Photo,"case variants agree");
    Check(Screen("Custom_Mod_Menu.fng")==Scene::Unknown,"unknown mod UI is never mislabeled as a shop");
    for(const auto& c:{std::pair<unsigned,Scene>{0,Scene::Garage},{1,Scene::PaintShop},{2,Scene::BodyShop},
        {3,Scene::PerformanceShop},{4,Scene::CarLot},{5,Scene::SpecialtiesShop},{6,Scene::ChoosingReward}})
        Check(Shop(std::uint8_t(c.first))==c.second,"verified native shop kind mapped");
    Check(Shop(7)==Scene::Unknown&&Shop(255)==Scene::Unknown,"unknown modded shop kind not guessed");
    Observation o;o.flow=6;o.track=4000;
    Check(Classify(o)==Activity{Scene::FreeRoam},"uncovered native free roam");
    o.screenKnown=false;Check(Classify(o)==Activity{},"unreadable package list is unknown");o.screenKnown=true;
    o.screen=Scene::Customization;o.shop=Scene::BodyShop;
    Check(Classify(o).scene==Scene::BodyShop,"entered body shop refines its shared part browser");
    o.shop=Scene::Garage;o.screen=Scene::PaintShop;
    Check(Classify(o).scene==Scene::Garage,"shared customization menus retain actual garage location");
    o.screen=Scene::Unknown;o.shop=Scene::BodyShop;
    Check(Classify(o).scene==Scene::FreeRoam,"retained shop pointer alone never reports a shop while driving");
    o.shop=Scene::Unknown;o.screen=Scene::Customization;
    Check(Classify(o).scene==Scene::Customization,"ambiguous part browser keeps honest generic activity");
    o.screen=Scene::Options;o.shop=Scene::BodyShop;
    Check(Classify(o).scene==Scene::Options,"shop context does not hide actual options surface");
    o.shop=Scene::Unknown;
    o.screen=Scene::PauseMenu;Check(Classify(o).scene==Scene::PauseMenu,"local pause menu reported without shared pause");
    o.screen=Scene::Unknown;o.special=true;o.timedSpecial=true;
    Check(Classify(o).scene==Scene::DestinationDrive,"timed event travel");
    o.arrived=true;Check(Classify(o).scene==Scene::AtDestination,"arrival state");
    o.waiting=true;Check(Classify(o).scene==Scene::WaitingForTeammate,"own event proof waits for teammate");
    o.screen=Scene::Photo;Check(Classify(o).scene==Scene::Photo,"photo UI refines broad event activity");
    o.movie=true;Check(Classify(o).scene==Scene::Cutscene,"playing movie overrides covered UI");
    o.movie=false;o.flow=5;Check(Classify(o).scene==Scene::Loading,"loading overrides stale native screens");
    o={};o.flow=6;o.track=1001;o.raceType=5;
    Check(Classify(o)==Activity{Scene::Race,5},"drift is identified independently of own car motion");
    o.countdown=true;Check(Classify(o).scene==Scene::Countdown,"race countdown");
    o.raceFlow=7;Check(Classify(o)==Activity{Scene::Results,5},"result state beats countdown");
    o.flow=3;Check(Classify(o)==Activity{Scene::MainMenu},"frontend ignores retained race data");
    o.screen=Scene::Garage;Check(Classify(o)==Activity{Scene::Garage},"garage reports in frontend flow");
    o={};o.flow=6;o.track=4000;o.outrun=true;Check(Classify(o).scene==Scene::Outrun,"verified outrun engagement");
    State host,client;Packet p{};const coop_network::Generation g{42,7,0};
    Check(!host.Poll(0,p),"unbound observer cannot send");
    host.Bind(g,1);client.Bind(g,2);host.Observe({Scene::Lobby},0);
    Check(host.Poll(0,p)&&Valid(p),"first lobby status needs no career ticket or car");
    Check(client.Receive(p,100)&&client.Peer(100)==Activity{Scene::Lobby},"fresh peer lobby update");
    Check(!client.Receive(p,101),"duplicate ignored");
    auto bad=p;bad.reserved=1;Check(!client.Receive(bad,102),"reserved field rejected");
    bad=p;bad.player=2;Check(!client.Receive(bad,102),"own player rejected");
    bad=p;bad.version=2;Check(!client.Receive(bad,102),"unknown wire version rejected");
    bad=p;bad.scene=Scene::Count;Check(!client.Receive(bad,102),"invalid scene rejected");
    bad=p;bad.race=5;Check(!client.Receive(bad,102),"race suffix not accepted on lobby");
    bad=p;bad.membership=8;Check(!client.Receive(bad,102),"old other membership rejected");
    bad=p;bad.ticket=1;Check(!client.Receive(bad,102),"other career ticket rejected");
    bad=p;bad.session=43;Check(!client.Receive(bad,102),"other session rejected");
    Check(client.Peer(3099)==Activity{Scene::Lobby},"status valid below freshness limit");
    Check(client.Peer(3100)==Activity{},"stale UI never claims current activity");
    Check(!host.Poll(999,p),"bounded heartbeat frequency");host.Observe({Scene::CarLot},999);
    Check(host.Poll(999,p)&&client.Receive(p,1000),"changed state sent before heartbeat");
    const auto second=p;host.Observe({Scene::Garage},1000);
    Check(!host.Poll(1000,p),"rapid UI transitions coalesce");
    Check(host.Poll(1249,p)&&client.Receive(p,1250),"change emitted at bounded cadence");
    Check(!client.Receive(second,1260),"older out-of-order state cannot replace garage");
    Check(client.Peer(1260)==Activity{Scene::Garage},"latest remains after replay");
    Check(host.Poll(4000,p)&&p.scene==Scene::Unknown,"hung game observer withdraws stale local status");
    host.Bind({42,7,1},1);client.Bind({42,7,1},2);
    Check(client.Peer(4001)==Activity{},"new career immediately clears previous activity");
    host.Observe({Scene::Loading},4001);Check(host.Poll(4001,p)&&p.sequence==1,"new generation serial starts cleanly");
    Check(client.Receive(p,4002),"new generation update accepted");
    client.Bind({},0);Check(client.Peer(4003)==Activity{}&&!client.Receive(p,4003),"disconnect clears activity");
    client.Bind({42,8,1},2);Check(!client.Receive(p,4004),"reconnect never accepts retired membership");
    host.Bind({42,8,1},1);host.Observe({Scene::PaintShop},0xFFFFFF00u);
    Check(host.Poll(0xFFFFFF00u,p)&&client.Receive(p,0xFFFFFF00u),"clock-wrap publication");
    Check(client.Peer(0x100)==Activity{Scene::PaintShop},"local age wrap works");
    Check(client.Peer(0x1000)==Activity{},"local age expires across wrap");
    const auto policy=coop_network::Classify(&p,sizeof(p),0x1234,100);
    Check(policy.valid&&!policy.discovery&&policy.delivery==coop_transport::Delivery::LatestState&&policy.stream==kStream,"activity uses independent latest-state transport stream");
    Check(kStream!=coop_network::kHumanMotionStream,"activity never replaces motion");
    Check(!coop_network::Classify(&p,sizeof(p)-1,0x1234,100).valid,"truncated activity cannot queue");
    coop_connection_hud::Model hud;coop::SessionSnapshot s;s.joined=s.fresh=true;s.sessionId=42;
    coop_transport::Statistics wire;wire.ready=wire.connected=wire.rttMeasured=true;wire.rttMs=60;wire.rttSampleAtMs=100;
    auto v=hud.Update(s,wire,true,true,101,{Scene::PerformanceShop});
    Check(std::strstr(v.text,"60 ms")&&std::strstr(v.activity,"Performance shop"),"existing ping and teammate activity coexist");
    v=hud.Update(s,wire,true,true,102,{Scene::Race,5});Check(std::strstr(v.activity,"Drift")!=nullptr,"race type in activity");
    v=hud.Update(s,wire,true,true,103,{});Check(std::strstr(v.activity,"Status unavailable")!=nullptr,"no stale fallback from old motion");
    s.joined=false;v=hud.Update(s,wire,true,true,104,{Scene::Garage});Check(!v.activity[0],"disconnected teammate never has false live activity");
    std::printf("Peer activity: %u checks passed.\n",checks);
}
