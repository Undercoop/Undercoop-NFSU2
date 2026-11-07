// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// Screen-facing text only: neither a world object nor a physics/network entity.
namespace coop_peer_label {
constexpr float kMaxDistance=75.0f;
struct Projection {float x=0,y=0,distance=0,fontPixels=0;uint8_t alpha=0;};
inline bool Finite(const float* values,unsigned count,float limit=1000000.0f) {
    if(!values)return false;
    for(unsigned i=0;i<count;++i)if(!std::isfinite(values[i])||std::fabs(values[i])>limit)return false;
    return true;
}
inline bool Eye(const float view[16],float eye[3]) {
    if(!Finite(view,16)||!eye||std::fabs(view[3])+std::fabs(view[7])+std::fabs(view[11])>0.001f||
        std::fabs(view[15]-1)>0.001f)return false;
    for(unsigned row=0;row<3;++row){float len=0;
        for(unsigned col=0;col<3;++col)len+=view[row*4+col]*view[row*4+col];
        if(std::fabs(len-1)>0.03f)return false;
        for(unsigned other=0;other<row;++other){float dot=0;
            for(unsigned col=0;col<3;++col)dot+=view[row*4+col]*view[other*4+col];
            if(std::fabs(dot)>0.03f)return false;}
        eye[row]=-(view[12]*view[row*4]+view[13]*view[row*4+1]+view[14]*view[row*4+2]);
    }
    return Finite(eye,3);
}
inline bool Project(const float matrix[16],const float eye[3],const float anchor[3],
    const float body[3],int width,int height,Projection& out) {
    out={};
    if(width<240||height<180||width>16384||height>16384||!Finite(matrix,16)||
        !Finite(eye,3)||!Finite(anchor,3)||!Finite(body,3))return false;
    double distanceSquared=0;
    for(unsigned i=0;i<3;++i){const double d=double(body[i])-eye[i];distanceSquared+=d*d;}
    const float distance=float(std::sqrt(distanceSquared));
    if(distance<1.5f||distance>=kMaxDistance)return false;
    double clip[4]{};
    for(unsigned col=0;col<4;++col)clip[col]=double(anchor[0])*matrix[col]+
        double(anchor[1])*matrix[4+col]+double(anchor[2])*matrix[8+col]+matrix[12+col];
    // Actual D3D left-handed view/projection, including the native negative-Y
    // projection, widescreen changes and look-back. Reject behind/near/far.
    if(!std::isfinite(clip[3])||clip[3]<=0.05||clip[2]<0||clip[2]>clip[3])return false;
    const double x=clip[0]/clip[3],y=clip[1]/clip[3];
    if(!std::isfinite(x)||!std::isfinite(y)||x<=-1||x>=1||y<=-1||y>=1)return false;
    const float size=std::clamp(22.0f*std::sqrt(10.0f/distance),9.0f,25.0f)*height/720.0f;
    out.x=float((x+1)*0.5*width);out.y=float((1-y)*0.5*height);out.distance=distance;
    out.fontPixels=std::round(size*4)*0.25f;
    out.alpha=uint8_t(std::clamp((kMaxDistance-distance)/12.0f,0.0f,1.0f)*240+0.5f);
    return out.alpha!=0;
}
inline bool PassivePackage(const char* name) {
    if(!name)return false;
    char upper[96]{};unsigned n=0;
    for(;n<sizeof(upper)-1&&name[n];++n){const unsigned char c=uint8_t(name[n]);
        if(c<32||c>126)return false;upper[n]=c>='a'&&c<='z'?char(c-32):char(c);}
    if(n==sizeof(upper)-1&&name[n])return false;
    return (n>8&&!std::strncmp(upper,"HUD_",4)&&!std::strcmp(upper+n-4,".FNG"))||
        !std::strcmp(upper,"SCREENPRINTF.FNG")||!std::strcmp(upper,"UI_PC_HELP_BAR.FNG");
}
}
