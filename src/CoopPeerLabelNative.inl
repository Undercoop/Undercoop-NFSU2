// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopPeerLabelVisibilityNative.inl"
namespace CoopPeerLabelNative {
struct Addresses {uintptr_t view=0x832E50,flow=0x8654A4,mode=0x850078,ui=0x8384C4,director=0x8900AC;
    uintptr_t lookup=0x48B1E0,caller=0x5D294E,multiply=0x5C80B0;};
static Addresses addresses{};
enum class State { OutsideWorld,NoPeer,Menu,Camera,Offscreen,Wall,Unknown,Shown,Surface };
static uint32_t counts[9]{},lastTrace=0;
struct Evidence {
    bool valid=false;float eye[3]{},body[3]{},anchor[3]{},distance=0,nx=0,ny=0,w=0;
    CoopPeerLabelVisibilityNative::Audit visibility{};
};
static Evidence latest{},lastWall{};
template<class T> static bool Read(uintptr_t at,T& value){return ReadGameMemory(at,&value,sizeof(value));}
static bool Supported(){
    // View lookup48B1E0, setup caller5D294E, projection5C7EC0 and the exact
    // View*Projection destination5C80B0. No hooks or guessed/FOV matrices.
    static const BYTE lookup[]{0x55,0x8B,0xEC,0x8B,0x45,0x08,0x6B,0xC0,0x70,0x05,0xE0,0x2D,0x83,0x00};
    static const BYTE caller[]{0x8B,0x1E,0x75,0x0B,0x6A,0x00,0x56};
    static const BYTE multiply[]{0x53,0x33,0xFF,0x56,0x8D,0xAE,0x80,0x00,0x00,0x00};
    BYTE actual[sizeof(lookup)]{};
    return ReadGameMemory(addresses.lookup,actual,sizeof(lookup))&&!std::memcmp(actual,lookup,sizeof(lookup))&&
        ReadGameMemory(addresses.caller,actual,sizeof(caller))&&!std::memcmp(actual,caller,sizeof(caller))&&
        ReadGameMemory(addresses.multiply,actual,sizeof(multiply))&&!std::memcmp(actual,multiply,sizeof(multiply));
}
static bool UiReady(){
    uintptr_t ui=0,engine=0;if(!Read(addresses.ui,ui)||!IsValidUserPtr(ui)||!Read(ui+8,engine)||!IsValidUserPtr(engine))return false;
    bool hud=false;
    for(unsigned list:{0xE4u,0xF4u}){
        uintptr_t node=0,seen[64]{};unsigned count=0;if(!Read(engine+list,node))return false;
        while(node){
            if(!IsValidUserPtr(node)||count==64||std::find(seen,seen+count,node)!=seen+count)return false;
            seen[count++]=node;uintptr_t next=0,namePointer=0;uint8_t active=0,removing=0;char name[96]{};
            if(!Read(node+4,next)||!Read(node+0xC,namePointer)||!IsValidUserPtr(namePointer)||
                !Read(node+0x14,active)||!Read(node+0x15,removing)||active>1||removing>1)return false;
            bool ended=false;
            for(unsigned n=0;n<sizeof(name);++n){if(!Read(namePointer+n,name[n]))return false;if(!name[n]){ended=true;break;}}
            if(!ended)return false;
            // Pending non-HUD transitions hide on THIS frame, before the native
            // loading/shop/photo/cutscene screen becomes the active package.
            if(list==0xF4||(active&&!removing)){
                if(!coop_peer_label::PassivePackage(name))return false;
                if(list==0xE4&&(name[0]=='H'||name[0]=='h')&&(name[1]=='U'||name[1]=='u')&&
                    (name[2]=='D'||name[2]=='d')&&name[3]=='_')hud=true;
            }
            node=next;
        }
    }
    return hud;
}
static bool PeerPosition(uintptr_t local,float body[3],float roof[3],float anchor[3]){
    const uintptr_t car=CoopRaceCar(true);uintptr_t parameters=0,ride=0;uint32_t type=0;
    uint8_t remote=0,index=0,active=0;
    if(!IsValidUserPtr(local)||!IsValidUserPtr(car)||car==local||!Read(car+0x14,parameters)||!IsValidUserPtr(parameters)||
        !Read(car+0x10,ride)||ride!=parameters+0x10||!Read(parameters+4,type)||type!=1||
        !Read(parameters+0x900,remote)||remote!=1||!Read(parameters+1,index)||index!=kPeerRenderPlayerIndex||
        !Read(car+0x550,active)||active!=1||!ReadGameMemory(car+0x60,body,3*sizeof(float))||
        !coop_peer_label::Finite(body,3))return false;
    // Use the actually rendered/smoothed peer Car, not its newest UDP snapshot.
    // World Z stays upright through spins; no name rotation/extruded geometry.
    for(unsigned i=0;i<3;++i)roof[i]=anchor[i]=body[i];
    body[2]+=0.45f;roof[2]+=1.45f;anchor[2]+=2.10f;
    return true;
}
static bool Camera(float matrix[16],float eye[3]){
    if(!Supported())return false;
    uint8_t enabled=0;uint32_t id=0,width=0,height=0,mode=0;uintptr_t plat=0,camera=0,screen=0,director=0,mover=0,linked=0;
    float view[16]{};
    if(!Read(addresses.view+8,enabled)||enabled!=1||!Read(addresses.view+4,id)||id!=1||
        !Read(addresses.view,plat)||!IsValidUserPtr(plat)||!Read(addresses.view+0x40,camera)||!IsValidUserPtr(camera)||
        !Read(addresses.view+0x58,screen)||!IsValidUserPtr(screen)||!Read(screen+0x14,width)||width<240||width>16384||
        !Read(screen+0x18,height)||height<180||height>16384||
        !Read(addresses.director,director)||!IsValidUserPtr(director)||!Read(director+0x238,mover)||!IsValidUserPtr(mover)||
        !Read(mover+0xC,mode)||mode!=1||!Read(mover+0x1C,linked)||linked!=camera||
        !ReadGameMemory(plat,view,sizeof(view))||!ReadGameMemory(plat+0x80,matrix,16*sizeof(float))||
        !coop_peer_label::Eye(view,eye))return false;
    // Reject partially constructed/unprojected views. Camera and projection
    // must be finite and have a perspective W column, not an identity/menu view.
    float projection[16]{};
    return ReadGameMemory(plat+0x40,projection,sizeof(projection))&&coop_peer_label::Finite(projection,16)&&
        projection[0]>0.001f&&projection[5]< -0.001f&&projection[11]>0.99f&&projection[11]<1.01f&&std::fabs(projection[15])<0.001f;
}
static void Name(const coop::SessionSnapshot& session,wchar_t text[40]){
    const char* source=session.role==coop::Role::Host?session.clientName:session.hostName;
    const int n=int(strnlen_s(source,32));int length=0;
    if(n>0&&n<32)length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,source,n,text,39);
    if(length<=0){std::wcscpy(text,session.role==coop::Role::Host?L"Player 2":L"Player 1");return;}
    text[length]=0;for(int i=0;i<length;++i)if(text[i]<32||text[i]==127)text[i]=L' ';
}
static State Update(HWND game,bool focused,const coop::SessionSnapshot& session){
    latest={};
    uint32_t flow=0,mode=0;
    if(!IsCoopGameplayActive()||!gRemoteSyncEnabled||!Read(addresses.flow,flow)||flow!=6||
        !Read(addresses.mode,mode)||mode!=1)return State::OutsideWorld;
    RemotePlayerState remote{};
    if(!session.joined||!session.fresh||(session.role!=coop::Role::Host&&session.role!=coop::Role::Client)||
        !GetRemotePlayerState(remote,false)||!remote.isFromPeer||remote.current.sessionId!=session.sessionId)return State::NoPeer;
    if(!UiReady())return State::Menu;
    float body[3]{},roof[3]{},anchor[3]{},matrix[16]{},eye[3]{};
    const uintptr_t local=CoopRaceCar(false);
    if(!PeerPosition(local,body,roof,anchor))return State::NoPeer;
    if(!Camera(matrix,eye))return State::Camera;
    latest.valid=true;std::memcpy(latest.eye,eye,sizeof(eye));std::memcpy(latest.body,body,sizeof(body));
    std::memcpy(latest.anchor,anchor,sizeof(anchor));double distance2=0,clip[4]{};
    for(unsigned i=0;i<3;++i){const double d=double(body[i])-eye[i];distance2+=d*d;}
    latest.distance=float(std::sqrt(distance2));
    for(unsigned col=0;col<4;++col)clip[col]=double(anchor[0])*matrix[col]+double(anchor[1])*matrix[4+col]+
        double(anchor[2])*matrix[8+col]+matrix[12+col];
    latest.w=float(clip[3]);if(std::fabs(clip[3])>0.001){latest.nx=float(clip[0]/clip[3]);latest.ny=float(clip[1]/clip[3]);}
    RECT client{};
    if(!game||!IsWindow(game)||!IsWindowVisible(game)||IsIconic(game)||
        !coop_overlay::PhysicalClient(game,client))return State::Surface;
    coop_peer_label::Projection point{};
    if(!coop_peer_label::Project(matrix,eye,anchor,body,client.right-client.left,client.bottom-client.top,point))return State::Offscreen;
    const auto visibility=CoopPeerLabelVisibilityNative::Query(eye,body,anchor,local,&latest.visibility);
    if(visibility==CoopPeerLabelVisibilityNative::Result::Occluded){lastWall=latest;return State::Wall;}
    if(visibility!=CoopPeerLabelVisibilityNative::Result::Clear)return State::Unknown;
    wchar_t text[40]{};Name(session,text);
    return CoopPeerLabelOverlay::Show(game,focused,client,point,text)?State::Shown:State::Surface;
}
static void TickFrame(HWND game,bool focused){
    const auto session=CoopSnapshot();const auto state=Update(game,focused,session);++counts[unsigned(state)];
    if(state!=State::Shown)CoopPeerLabelOverlay::Hide();
    const uint32_t now=GetTickCount();
    if(!IsCoopGameplayActive()||!session.joined){std::memset(counts,0,sizeof(counts));latest={};lastWall={};lastTrace=now;return;}
    if(now-lastTrace<10000)return;
    char line[256]{};std::snprintf(line,sizeof(line),
        "[PEER_LABEL] shown=%u wall=%u geometryUnknown=%u camera=%u offscreen=%u menu=%u noPeer=%u outsideWorld=%u surface=%u; private 2D text only.\r\n",
        counts[7],counts[5],counts[6],counts[3],counts[4],counts[2],counts[1],counts[0],counts[8]);
    LogMessage(line);
    if(latest.valid){char detail[320]{};std::snprintf(detail,sizeof(detail),
        "[PEER_LABEL_VIEW] state=%u distance=%.2f ndc=(%.3f,%.3f) W=%.3f eye=(%.2f,%.2f,%.2f) peerBody=(%.2f,%.2f,%.2f) tested=%u skippedDisabled=%u.\r\n",
        unsigned(state),latest.distance,latest.nx,latest.ny,latest.w,latest.eye[0],latest.eye[1],latest.eye[2],
        latest.body[0],latest.body[1],latest.body[2],latest.visibility.tested,latest.visibility.skippedDisabled);LogMessage(detail);}
    if(counts[unsigned(State::Wall)]&&lastWall.valid){const auto& a=lastWall.visibility;char detail[360]{};std::snprintf(detail,sizeof(detail),
        "[PEER_LABEL_BLOCK] section=%d group=%u flags=%02X normal=(%.4f,%.4f,%.4f) sidesEyeBodyAnchor=(%.4f,%.4f,%.4f) distance=%.2f tested=%u skippedDisabled=%u.\r\n",
        a.hitSection,a.hitGroup,a.hitFlags,a.hitNormal.x,a.hitNormal.y,a.hitNormal.z,a.eyeSide,a.bodySide,a.anchorSide,
        lastWall.distance,a.tested,a.skippedDisabled);LogMessage(detail);}
    std::memset(counts,0,sizeof(counts));lastWall={};lastTrace=now;
}
static void Tick(HWND game,bool focused){
    // This is called from the frame epilogue's noexcept destructor. A private
    // text/geometry allocation failure must never abort the game or its save.
    static uint32_t failedAt=0;const uint32_t now=GetTickCount();
    if(failedAt&&now-failedAt<10000){CoopPeerLabelOverlay::Hide();return;}
    try{TickFrame(game,focused);failedAt=0;}
    catch(...){failedAt=now?now:1;CoopPeerLabelOverlay::Hide();
        OutputDebugStringA("Undercoop: peer label resources unavailable; hidden, game continues.\n");}
}
}
