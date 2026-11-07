// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Read-only track-wall visibility. Layouts/coordinate decode verified against
// SPEED2.EXE 1.2 f9dd86c0... and WCollision{,Ray}.cpp in troikoss/speed-re.
// No WCollider constructors, native cache calls, refcounts or world writes.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace coop_peer_visibility {
enum class Result : uint8_t { Unknown, Occluded, Clear };
struct Vec3 { double x=0,y=0,z=0; };
struct Audit {uint32_t tested=0,skippedDisabled=0;int32_t hitSection=0;uint32_t hitGroup=0,hitFlags=0;
    Vec3 hitNormal{};double eyeSide=0,bodySide=0,anchorSide=0;};
struct Point2 { float x=0,y=0; };
using ReadFn=bool(*)(void*,uintptr_t,void*,size_t);
struct Reader {
    ReadFn read=nullptr; void* context=nullptr;
    bool Bytes(uintptr_t at,void* out,size_t n) const {
        return read&&at>=0x10000&&n&&at<=UINT32_MAX&&n<=UINT32_MAX-at&&read(context,at,out,n);
    }
    template<class T> bool Get(uintptr_t at,T& out) const {return Bytes(at,&out,sizeof(out));}
};
struct Addresses {uintptr_t manager=0x88F768,topology=0x883E70,sectionOffset=0x800DFC,localCar=0;};
#pragma pack(push,1)
struct Polygon {uint8_t group,extents,surface,flags;int16_t height,cache,x[4],y[4],z[4];};
#pragma pack(pop)
static_assert(sizeof(Polygon)==0x20,"native compressed polygon size");
struct Face {std::array<Vec3,4> v{};Vec3 normal{};unsigned count=0;};
inline Vec3 Sub(const Vec3& a,const Vec3& b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline Vec3 Cross(const Vec3& a,const Vec3& b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline double Dot(const Vec3& a,const Vec3& b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline bool Finite(const Vec3& a){return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z)&&
    std::abs(a.x)<100000&&std::abs(a.y)<100000&&std::abs(a.z)<10000;}
inline bool Decode(const Polygon& p,Face& f){
    f.count=(p.flags&0x10)?4u:3u;
    for(unsigned i=0;i<4;++i){
        f.v[i]={double(p.x[i])*0.125,double(p.y[i])*0.125,double(p.height)+double(p.z[i])/256.0};
        if(p.flags&4) f.v[i].z=(i==0||i==3)?-2048.0:2048.0;
    }
    f.normal=Cross(Sub(f.v[1],f.v[0]),Sub(f.v[2],f.v[0]));
    const double n=std::sqrt(Dot(f.normal,f.normal));
    if(!std::isfinite(n)||n<1e-10)return false; // Native zero-normal face cannot occlude.
    f.normal={f.normal.x/n,f.normal.y/n,f.normal.z/n};return true;
}
inline bool SegmentHits(const Face& f,const Vec3& from,const Vec3& to){
    const double a=Dot(Sub(from,f.v[0]),f.normal),b=Dot(Sub(to,f.v[0]),f.normal);
    // Two-sided, including a conservative coplanar/end-contact case.
    constexpr double planeEpsilon=1e-5;
    if((a>planeEpsilon&&b>planeEpsilon)||(a< -planeEpsilon&&b< -planeEpsilon))return false;
    const double denominator=a-b;
    auto Inside=[&](const Vec3& p){
        for(unsigned i=0;i<f.count;++i){
            const Vec3 edge=Sub(f.v[(i+1)%f.count],f.v[i]);
            if(Dot(Cross(edge,Sub(p,f.v[i])),f.normal)< -1e-5)return false;
        }return true;
    };
    if(std::abs(denominator)<=1e-12){
        if(std::abs(a)>planeEpsilon)return false;
        // Entire segment lies in the wall plane. Conservatively hide rather
        // than interpreting the native strict-side rejection as visibility.
        double enter=0,leave=1;
        for(unsigned i=0;i<f.count;++i){
            const Vec3 edge=Sub(f.v[(i+1)%f.count],f.v[i]);
            const double low=Dot(Cross(edge,Sub(from,f.v[i])),f.normal);
            const double high=Dot(Cross(edge,Sub(to,f.v[i])),f.normal);
            const double delta=high-low;
            if(std::abs(delta)<1e-12){if(low< -1e-5)return false;continue;}
            const double t=(-1e-5-low)/delta;
            if(delta>0)enter=(std::max)(enter,t);else leave=(std::min)(leave,t);
            if(enter>leave)return false;
        }return true;
    }
    const double t=a/denominator;
    if(t< -1e-7||t>1.0000001)return false;
    return Inside({from.x+(to.x-from.x)*t,from.y+(to.y-from.y)*t,from.z+(to.z-from.z)*t});
}
inline int SectionIndex(int section,int offset){
    if(section==0)return 0;
    int number=section%100;if(number>=offset&&number<2*offset)section-=offset;
    number=section%100;const int letter=section/100;
    const int index=number+(letter-1)*20;
    return section>0&&letter>=1&&letter<=26&&number>=0&&number<20&&index<520?index:-1;
}
template<class T> inline T At(const uint8_t* bytes,size_t offset){T v{};std::memcpy(&v,bytes+offset,sizeof(v));return v;}
inline bool Bounds(float x0,float y0,float x1,float y1){return std::isfinite(x0)&&std::isfinite(y0)&&
    std::isfinite(x1)&&std::isfinite(y1)&&x0<=x1&&y0<=y1;}
inline bool Overlap(double x0,double y0,double x1,double y1,double a0,double b0,double a1,double b1){
    return x0<=a1+1e-4&&a0<=x1+1e-4&&y0<=b1+1e-4&&b0<=y1+1e-4;
}
struct Section {std::vector<Point2> points;bool loaded=false;uint32_t pack=0;};
inline bool Contains(const Section& s,double x,double y){
    bool inside=false;const size_t n=s.points.size();
    for(size_t i=0,j=n-1;i<n;j=i++){
        const auto a=s.points[j],b=s.points[i];
        const double dx=double(b.x)-a.x,dy=double(b.y)-a.y;
        const double cross=dx*(y-a.y)-dy*(x-a.x);
        if(std::abs(cross)<=1e-5*(std::abs(dx)+std::abs(dy)+1)&&
            x>=(std::min)(a.x,b.x)-1e-5&&x<=(std::max)(a.x,b.x)+1e-5&&
            y>=(std::min)(a.y,b.y)-1e-5&&y<=(std::max)(a.y,b.y)+1e-5)return true;
        if((a.y>y)!=(b.y>y)&&x<(double(b.x)-a.x)*(y-a.y)/(double(b.y)-a.y)+a.x)inside=!inside;
    }return inside;
}
inline bool Covered(const std::vector<Section>& sections,const Vec3& from,const Vec3& to,std::vector<uint32_t>* needed=nullptr){
    std::vector<double> cuts{0,1};const double dx=to.x-from.x,dy=to.y-from.y;
    for(const auto& s:sections)for(size_t i=0,j=s.points.size()-1;i<s.points.size();j=i++){
        const auto a=s.points[j],b=s.points[i];const double ex=double(b.x)-a.x,ey=double(b.y)-a.y;
        const double denominator=dx*ey-dy*ex;
        if(std::abs(denominator)<=1e-12){
            if(std::abs((a.x-from.x)*dy-(a.y-from.y)*dx)<1e-7){
                const double length2=dx*dx+dy*dy;
                if(length2>1e-12)for(const auto& p:{a,b}){
                    const double t=((p.x-from.x)*dx+(p.y-from.y)*dy)/length2;
                    if(t>0&&t<1){cuts.push_back(t);if(cuts.size()>4096)return false;}
                }
            }continue;
        }
        const double ax=double(a.x)-from.x,ay=double(a.y)-from.y;
        const double t=(ax*ey-ay*ex)/denominator,u=(ax*dy-ay*dx)/denominator;
        if(t>0&&t<1&&u>=-1e-9&&u<=1.000000001)cuts.push_back(t);
        if(cuts.size()>4096)return false;
    }
    std::sort(cuts.begin(),cuts.end());
    auto Check=[&](double t){
        bool found=false;for(const auto& s:sections)if(Contains(s,from.x+dx*t,from.y+dy*t)){
            if(!s.loaded)return false;found=true;
            if(needed&&std::find(needed->begin(),needed->end(),s.pack)==needed->end())needed->push_back(s.pack);
        }return found;
    };
    if(!Check(0)||!Check(1))return false;
    for(size_t i=1;i<cuts.size();++i)if(!Check((cuts[i-1]+cuts[i])*0.5))return false;
    return true;
}
inline Result Query(const Reader& memory,const Addresses& addresses,const Vec3& from,const Vec3& body,const Vec3& roof,Audit* audit=nullptr){
    if(audit)*audit={};
    if(!Finite(from)||!Finite(body)||!Finite(roof)||Dot(Sub(body,from),Sub(body,from))>14400)return Result::Unknown;
    // Native FillArea5E1027 tests group0 unconditionally, then group.enabled8.
    // RefreshArea5E58D2 temporarily enables only this local Car collider's
    // groups and disables them again after caching faces. Read the equivalent
    // eligibility without touching those shared flags; other courses' disabled
    // barrier planes are not visible walls and must not hide the peer's name.
    int32_t barrierCount=0;uint32_t barrierBase=0;std::array<uint8_t,256> eligible{};
    if(!memory.Get(addresses.manager+0x828,barrierCount)||barrierCount<0||barrierCount>4096||
        !memory.Get(addresses.manager+0x82C,barrierBase))return Result::Unknown;
    if(barrierCount){
        if(barrierBase<0x10000||uintptr_t(barrierBase)>UINT32_MAX-size_t(barrierCount)*0x20)return Result::Unknown;
        std::array<uint8_t,256*0x20> groups{};const size_t count=(std::min)(size_t(256),size_t(barrierCount));
        if(!memory.Bytes(barrierBase,groups.data(),count*0x20))return Result::Unknown;
        for(size_t i=0;i<count;++i)eligible[i]=groups[i*0x20+8]!=0;
    }
    eligible[0]=1;
    if(addresses.localCar){
        int32_t count=0;std::array<uint32_t,4> groups{};
        if(!memory.Get(addresses.localCar+0x8B4,count)||count<0||count>4||
            (count&&!memory.Bytes(addresses.localCar+0x8B8,groups.data(),size_t(count)*4)))return Result::Unknown;
        for(int32_t i=0;i<count;++i){
            const uintptr_t group=groups[size_t(i)];
            if(!barrierCount||group<barrierBase||(group-barrierBase)%0x20||
                (group-barrierBase)/0x20>=size_t(barrierCount))return Result::Unknown;
            const size_t index=(group-barrierBase)/0x20;if(index<eligible.size())eligible[index]=1;
        }
    }
    std::array<uint32_t,520> packs{};int offset=0;
    if(!memory.Bytes(addresses.manager+8,packs.data(),sizeof(packs))||
        !memory.Get(addresses.sectionOffset,offset)||offset<1||offset>20)return Result::Unknown;
    const double x0=(std::min)({from.x,body.x,roof.x}),x1=(std::max)({from.x,body.x,roof.x});
    const double y0=(std::min)({from.y,body.y,roof.y}),y1=(std::max)({from.y,body.y,roof.y});
    std::vector<Section> sections;std::vector<uint32_t> seen;size_t pointsRead=0;
    uint32_t node=0;if(!memory.Get(addresses.topology,node)||!node)return Result::Unknown;
    for(size_t visits=0;node!=addresses.topology;++visits){
        if(visits>=520||std::find(seen.begin(),seen.end(),node)!=seen.end())return Result::Unknown;
        seen.push_back(node);uint8_t row[0x24]{};
        if(!memory.Bytes(node,row,sizeof(row)))return Result::Unknown;
        const int id=At<int16_t>(row,8),count=At<int16_t>(row,0xA);
        const float a0=At<float>(row,0xC),b0=At<float>(row,0x10),a1=At<float>(row,0x14),b1=At<float>(row,0x18);
        if(count<3||count>512||!Bounds(a0,b0,a1,b1))return Result::Unknown;
        if(Overlap(x0,y0,x1,y1,a0,b0,a1,b1)){
            if(pointsRead+size_t(count)>8192)return Result::Unknown;pointsRead+=size_t(count);
            Section s;s.points.resize(size_t(count));
            if(!memory.Bytes(uintptr_t(node)+0x24,s.points.data(),size_t(count)*sizeof(Point2)))return Result::Unknown;
            for(const auto& p:s.points)if(!std::isfinite(p.x)||!std::isfinite(p.y)||std::abs(p.x)>100000||std::abs(p.y)>100000)return Result::Unknown;
            const int index=SectionIndex(id,offset);s.pack=index>=0?packs[size_t(index)]:0;s.loaded=s.pack>=0x10000;
            sections.push_back(std::move(s));
        }
        node=At<uint32_t>(row,0);if(!node)return Result::Unknown;
    }
    std::vector<uint32_t> needed;
    if(sections.empty()||!Covered(sections,from,body,&needed)||!Covered(sections,from,roof,&needed))return Result::Unknown;
    seen.clear();size_t polygonsRead=0;bool relevantPack=false;
    for(size_t slot=0;slot<packs.size();++slot){const uint32_t pack=packs[slot];
        if(!pack||std::find(seen.begin(),seen.end(),pack)!=seen.end())continue;seen.push_back(pack);
        uint8_t header[0x70]{};if(!memory.Bytes(pack,header,sizeof(header)))return Result::Unknown;
        const int count=At<int32_t>(header,0xC);const uint32_t polygons=At<uint32_t>(header,8);
        const float a0=At<float>(header,0x40),b0=At<float>(header,0x44),a1=At<float>(header,0x50),b1=At<float>(header,0x54);
        if(count<0||count>65536||!Bounds(a0,b0,a1,b1)||SectionIndex(At<int16_t>(header,0x30),offset)!=int(slot))return Result::Unknown;
        if(std::find(needed.begin(),needed.end(),pack)!=needed.end()&&(!count||polygons<0x10000))return Result::Unknown;
        if(!Overlap(x0,y0,x1,y1,a0,b0,a1,b1))continue;
        relevantPack=true;if(!count||polygons<0x10000)return Result::Unknown;
        if(polygonsRead+size_t(count)>65536)return Result::Unknown;polygonsRead+=size_t(count);
        for(size_t first=0;first<size_t(count);first+=64){
            std::array<Polygon,64> block{};const size_t n=(std::min)(size_t(64),size_t(count)-first);
            if(!memory.Bytes(uintptr_t(polygons)+first*sizeof(Polygon),block.data(),n*sizeof(Polygon)))return Result::Unknown;
            for(size_t i=0;i<n;++i){
                const auto& p=block[i];const unsigned e=p.extents;
                const double px0=double(p.x[e&3])*0.125,py0=double(p.y[(e>>2)&3])*0.125;
                const double px1=double(p.x[(e>>4)&3])*0.125,py1=double(p.y[e>>6])*0.125;
                if(px0>px1||py0>py1)return Result::Unknown;
                if(!Overlap(x0,y0,x1,y1,px0,py0,px1,py1))continue;
                if(p.group&&p.group>=barrierCount)return Result::Unknown;
                if(!eligible[p.group]){if(audit)++audit->skippedDisabled;continue;}
                Face face;if(!Decode(p,face))continue;
                if(audit)++audit->tested;
                if(SegmentHits(face,from,body)||SegmentHits(face,from,roof)){
                    if(audit){audit->hitSection=At<int16_t>(header,0x30);audit->hitGroup=p.group;audit->hitFlags=p.flags;
                        audit->hitNormal=face.normal;audit->eyeSide=Dot(Sub(from,face.v[0]),face.normal);
                        audit->bodySide=Dot(Sub(body,face.v[0]),face.normal);audit->anchorSide=Dot(Sub(roof,face.v[0]),face.normal);}
                    return Result::Occluded;
                }
            }
        }
    }
    return relevantPack?Result::Clear:Result::Unknown;
}
} // namespace coop_peer_visibility
