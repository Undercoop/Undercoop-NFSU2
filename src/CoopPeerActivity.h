// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.
#pragma once
#include "CoopNetworkGeneration.h"
#include "CoopTime.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

// Display-only, independent of a car, motion or native world ownership. No
// package names, save names, pointers or free-form peer text enter this wire.
namespace coop_activity {
constexpr std::uint32_t kMagic=0x5441324e; // N2AT, distinct from legacy NC2A join ACK.
constexpr std::uint32_t kFreshMs=3000,kHeartbeatMs=1000,kMinChangeMs=250;
constexpr std::uint64_t kStream=UINT64_MAX-1;
enum class Scene:std::uint8_t {
    Unknown,Startup,MainMenu,Lobby,CareerSelection,Loading,Cutscene,FreeRoam,
    Race,Countdown,Results,PauseMenu,WorldMap,Garage,CarLot,BodyShop,
    PerformanceShop,PaintShop,SpecialtiesShop,Customization,Dyno,Tuning,
    Sponsor,Messages,CareerStatus,Photo,Magazine,SpecialEvent,DestinationDrive,
    AtDestination,WaitingForTeammate,ChoosingReward,RaceInvitation,
    SpecialInvitation,ShopEntrance,CarSelection,Options,SaveLoad,Replay,Outrun,Menu,
    Count
};
struct Activity {
    Scene scene=Scene::Unknown;
    std::uint8_t race=0;
    friend bool operator==(Activity a,Activity b){return a.scene==b.scene&&a.race==b.race;}
    friend bool operator!=(Activity a,Activity b){return !(a==b);}
};
inline bool RaceType(std::uint8_t type){return !type||type==2||type==3||type==4||type==5||type==7||type==8||type==9;}
inline bool HasRace(Scene scene){return scene==Scene::Race||scene==Scene::Countdown||scene==Scene::Results||scene==Scene::PauseMenu||scene==Scene::WaitingForTeammate;}
inline bool Valid(Activity a){return a.scene<Scene::Count&&RaceType(a.race)&&(!a.race||HasRace(a.scene));}
inline const char* RaceName(std::uint8_t type){switch(type){case 2:return "Circuit";case 3:return "Sprint";case 4:return "Drag";
    case 5:return "Drift";case 7:return "Outrun";case 8:return "U.R.L.";case 9:return "Street X";default:return "Race";}}
inline const char* Label(Scene scene){switch(scene){
    case Scene::Startup:return "Starting up";case Scene::MainMenu:return "Main menu";
    case Scene::Lobby:return "CO-OP lobby";case Scene::CareerSelection:return "Choosing a career";
    case Scene::Loading:return "Loading";case Scene::Cutscene:return "Watching a cutscene";
    case Scene::FreeRoam:return "Free roam";case Scene::Race:return "Racing";
    case Scene::Countdown:return "Race countdown";case Scene::Results:return "Event results";
    case Scene::PauseMenu:return "Pause menu";case Scene::WorldMap:return "World map";
    case Scene::Garage:return "Garage";case Scene::CarLot:return "Car lot";
    case Scene::BodyShop:return "Body shop";case Scene::PerformanceShop:return "Performance shop";
    case Scene::PaintShop:return "Paint shop";case Scene::SpecialtiesShop:return "Car specialties shop";
    case Scene::Customization:return "Customizing a car";case Scene::Dyno:return "Dyno";
    case Scene::Tuning:return "Performance tuning";case Scene::Sponsor:return "Choosing a sponsor";
    case Scene::Messages:return "Reading messages";case Scene::CareerStatus:return "Career progress";
    case Scene::Photo:return "Photo shoot";case Scene::Magazine:return "Magazine reward";
    case Scene::SpecialEvent:return "Special event";case Scene::DestinationDrive:return "Driving to a destination";
    case Scene::AtDestination:return "At the event destination";case Scene::WaitingForTeammate:return "Waiting for teammate";
    case Scene::ChoosingReward:return "Choosing an event reward";case Scene::RaceInvitation:return "Race invitation";
    case Scene::SpecialInvitation:return "Special event invitation";case Scene::ShopEntrance:return "Shop entrance";
    case Scene::CarSelection:return "Choosing a car";case Scene::Options:return "Options";
    case Scene::SaveLoad:return "Save / load menu";case Scene::Replay:return "Watching a replay";
    case Scene::Outrun:return "Outrun challenge";case Scene::Menu:return "In a menu";default:return "Status unavailable";
}}
// Stock528560/50E2E0: the active career shop record's signed byte+50
// selects these native menu families. Nearby trigger records are not proof
// that a player has entered a shop.
inline Scene Shop(std::uint8_t kind){switch(kind){
    case 0:return Scene::Garage;case 1:return Scene::PaintShop;
    case 2:return Scene::BodyShop;case 3:return Scene::PerformanceShop;
    case 4:return Scene::CarLot;case 5:return Scene::SpecialtiesShop;
    case 6:return Scene::ChoosingReward;default:return Scene::Unknown;
}}
inline bool CustomizationScreen(Scene scene){return scene==Scene::Customization||scene==Scene::PerformanceShop||
    scene==Scene::PaintShop||scene==Scene::SpecialtiesShop;}
// Only current, active FNG names are passed here by the game-thread observer.
inline bool Equal(const char* a,const char* b){if(!a||!b)return false;for(;;++a,++b){
    char x=*a,y=*b;if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return false;if(!x)return true;}}
inline bool Prefix(const char* a,const char* b){if(!a||!b)return false;for(;*b;++a,++b){
    char x=*a,y=*b;if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return false;}return true;}
inline Scene Screen(const char* name){
    if(Equal(name,"UI_Main.fng"))return Scene::MainMenu;
    if(Equal(name,"PC_OL_Lobby.fng")||Equal(name,"PC_OL_SEARCH.fng"))return Scene::Lobby;
    if(Equal(name,"UI_StartCareer.fng"))return Scene::CareerSelection;
    if(Equal(name,"UI_CareerCrib.fng")||Equal(name,"GarageMain.fng")||Equal(name,"FEPkg_GarageMain.fng"))return Scene::Garage;
    if(Equal(name,"UI_CareerCarLot.fng")||Equal(name,"UI_OLCarLot.fng"))return Scene::CarLot;
    if(Equal(name,"UI_InGame_WorldMap.fng")||Equal(name,"UI_CareerWorldMap.fng")||Equal(name,"UI_PostraceWorldMap.fng"))return Scene::WorldMap;
    if(Equal(name,"UI_Pause.fng"))return Scene::PauseMenu;
    if(Prefix(name,"UI_PostRace"))return Scene::Results;
    if(Equal(name,"UI_SponsorPopup.fng")||Equal(name,"UI_Sponsorship_new.fng")||Equal(name,"UI_RewardsSponsor.fng"))return Scene::Sponsor;
    if(Equal(name,"UI_Showcase_Preview.fng")||Equal(name,"UI_Showcase_DPAD.fng"))return Scene::Photo;
    if(Equal(name,"UI_MagazineReward.fng"))return Scene::Magazine;
    if(Prefix(name,"UI_Magazine")||Prefix(name,"UI_Status_"))return Scene::CareerStatus;
    if(Equal(name,"UI_ChooseUniquePart.fng"))return Scene::ChoosingReward;
    if(Equal(name,"UI_EngageRaceDialog.fng"))return Scene::RaceInvitation;
    if(Equal(name,"UI_EngageEventDialog.fng"))return Scene::SpecialInvitation;
    if(Equal(name,"UI_EngageShopDialog.fng"))return Scene::ShopEntrance;
    if(Equal(name,"UI_SMS_Mailbox.fng")||Equal(name,"UI_EngageMessageDialog.fng"))return Scene::Messages;
    if(Prefix(name,"UI_PerformanceDyno_"))return Scene::Dyno;
    if(Prefix(name,"UI_PerformanceTuning_"))return Scene::Tuning;
    if(Equal(name,"UI_BuyPerformanceParts.fng")||Equal(name,"UI_PerformanceBrandSelect.fng")||
       Equal(name,"UI_ChoosePerformanceCategory.fng")||Equal(name,"UI_ChoosePerformancePackage.fng"))return Scene::PerformanceShop;
    if(Equal(name,"UI_Paint.fng")||Equal(name,"UI_ChoosePaintCategory.fng")||Equal(name,"UI_ChooseVinylLayer.fng")||
       Equal(name,"UI_DecalMain.fng")||Prefix(name,"UI_DecalsOverlay"))return Scene::PaintShop;
    if(Equal(name,"UI_ICEMAIN.fng")||Equal(name,"UI_IcePartsOverlay.fng")||Equal(name,"UI_CustomNeonMain.fng")||
       Equal(name,"UI_NeonPartsOverlay.fng")||Equal(name,"UI_ChooseCustomHUD.fng")||Equal(name,"UI_CustomHUDOverlay.fng"))return Scene::SpecialtiesShop;
    if(Equal(name,"UI_GenericParts_Browser.fng")||Equal(name,"UI_Rims_Browser.fng")||Equal(name,"UI_ChooseRimBrand.fng")||
       Equal(name,"UI_ChooseSpinner.fng")||Equal(name,"UI_Wheel_Options.fng")||Equal(name,"UI_ChooseCustomizeCategory.fng")||Equal(name,"UI_PC_Customize_Options.fng"))return Scene::Customization;
    if(Equal(name,"UI_CareerCarSelect.fng")||Equal(name,"UI_CarSelect.fng")||Equal(name,"UI_OLCarSelect.fng")||Equal(name,"UI_QRCarSelect.fng"))return Scene::CarSelection;
    if(Equal(name,"UI_ProfileManager.fng")||Equal(name,"UI_Deleteprofile.fng")||Prefix(name,"MC_"))return Scene::SaveLoad;
    if(Prefix(name,"UI_Options")||Prefix(name,"UI_PauseOptions")||Prefix(name,"PC_Keyboard_")||Prefix(name,"PC_JoyPad_")||Prefix(name,"PC_Wheel_"))return Scene::Options;
    if(Equal(name,"UI_ReplayControl.fng"))return Scene::Replay;
    return Scene::Unknown;
}
struct Observation {
    std::uint32_t flow=UINT32_MAX,track=0,raceFlow=0;
    std::uint8_t raceType=0;
    Scene screen=Scene::Unknown,shop=Scene::Unknown;
    bool screenKnown=true,movie=false,countdown=false,special=false,timedSpecial=false,arrived=false,waiting=false,outrun=false;
};
inline Activity Classify(const Observation& o){
    if(o.movie)return {Scene::Cutscene};
    if(o.flow==4||o.flow==5)return {Scene::Loading};
    if(o.flow!=3&&o.flow!=6)return {o.flow==UINT32_MAX?Scene::Unknown:Scene::Startup};
    if(!o.screenKnown)return {};
    if(o.screen!=Scene::Unknown){
        Scene s=o.screen;
        // Part browsers are shared by garage and shop menus. Only a current
        // verified shop context may refine their location.
        if(CustomizationScreen(s)&&o.shop!=Scene::Unknown)s=o.shop;
        if(s==Scene::SpecialInvitation&&o.special&&o.arrived)s=Scene::AtDestination;
        return {s,HasRace(s)&&RaceType(o.raceType)?o.raceType:std::uint8_t(0)};
    }
    if(o.flow==3)return {Scene::MainMenu};
    if(o.track&&o.track!=4000){
        Scene scene=o.raceFlow==7?Scene::Results:o.raceFlow==6?Scene::PauseMenu:
            o.waiting?Scene::WaitingForTeammate:o.countdown?Scene::Countdown:Scene::Race;
        return {scene,RaceType(o.raceType)?o.raceType:std::uint8_t(0)};
    }
    if(o.special)return {o.waiting?Scene::WaitingForTeammate:o.arrived?Scene::AtDestination:
        o.timedSpecial?Scene::DestinationDrive:Scene::SpecialEvent};
    if(o.outrun)return {Scene::Outrun};
    if(o.raceFlow==6)return {Scene::PauseMenu};
    return {Scene::FreeRoam};
}
#pragma pack(push,1)
struct Packet {
    std::uint32_t magic=kMagic;std::uint16_t version=1,size=32;
    std::uint64_t session=0;std::uint32_t membership=0,ticket=0,sequence=0;
    std::uint8_t player=0;Scene scene=Scene::Unknown;std::uint8_t race=0,reserved=0;
};
#pragma pack(pop)
static_assert(sizeof(Packet)==32,"Activity remains a small value-only snapshot");
inline bool Valid(const Packet& p){return p.magic==kMagic&&p.version==1&&p.size==sizeof(p)&&
    p.session&&p.membership&&p.sequence&&(p.player==1||p.player==2)&&!p.reserved&&Valid({p.scene,p.race});}
class State {
    coop_network::Generation scope_{};std::uint8_t player_=0;
    coop_network::SerialGate serial_;
    Activity local_{},peer_{},sentValue_{};
    std::uint32_t localAt_=0,peerAt_=0,sentAt_=0,sequence_=0;
    bool observed_=false,received_=false,sent_=false;
public:
    void Bind(coop_network::Generation scope,std::uint8_t player){
        if(!scope.Valid()||(player!=1&&player!=2)){*this=State{};return;}
        if(scope_==scope&&player_==player)return;
        *this=State{};scope_=scope;player_=player;
    }
    void Observe(Activity a,std::uint32_t now){if(Valid(a)){local_=a;localAt_=now;observed_=true;}}
    bool Poll(std::uint32_t now,Packet& p){
        if(!scope_.Valid()||!player_)return false;
        const auto value=observed_&&coop_time::Elapsed(now,localAt_)<kFreshMs?local_:Activity{};
        const auto age=coop_time::Elapsed(now,sentAt_);
        if(sent_&&age<kHeartbeatMs&&(value==sentValue_||age<kMinChangeMs))return false;
        p={};p.session=scope_.session;p.membership=scope_.membership;p.ticket=scope_.ticket;
        if(++sequence_==0)++sequence_;p.sequence=sequence_;p.player=player_;p.scene=value.scene;p.race=value.race;
        sentValue_=value;sentAt_=now;sent_=true;return true;
    }
    bool Receive(const Packet& p,std::uint32_t now){
        if(!Valid(p)||p.player==player_||!serial_.Accept({p.session,p.membership,p.ticket},scope_,p.sequence))return false;
        peer_={p.scene,p.race};peerAt_=now;received_=true;return true;
    }
    Activity Peer(std::uint32_t now)const{return scope_.Valid()&&received_&&coop_time::Elapsed(now,peerAt_)<kFreshMs?peer_:Activity{};}
};
}
