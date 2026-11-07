// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Special dialogs live in the FEng active-package list, NOT Career/RaceFlow's
// optional 8363DC result-page manager (which is normally null in free roam).
// Only current read-only identities; no package/widget pointer is retained.
namespace CoopSpecialNative { namespace Feng {
static bool Name(uintptr_t at,const char* expected){
    if(!expected)return false;
    for(unsigned i=0;i<96;++i){unsigned char c=0,e=uint8_t(expected[i]);
        if(!Read(at+i,c))return false;
        if(c>='a'&&c<='z')c-=32;if(e>='a'&&e<='z')e-=32;
        if(c!=e)return false;if(!c)return true;
    }
    return false;
}
static bool Passive(uintptr_t name){return Name(name,"HUD_SingleRace.fng")||
    Name(name,"ScreenPrintf.fng")||Name(name,"UI_PC_Help_Bar.fng");}
struct Package {uintptr_t address=0,next=0,name=0;bool active=false;};
static bool First(uintptr_t source,uintptr_t& first){
    uintptr_t ui=0,engine=0;
    return Read(source,ui)&&IsValidUserPtr(ui)&&Read(ui+8,engine)&&IsValidUserPtr(engine)&&Read(engine+0xE4,first);
}
static bool ReadPackage(uintptr_t at,Package& out){
    Package p;p.address=at;uint8_t enabled=0,removing=0;
    if(!IsValidUserPtr(at)||!Read(at+4,p.next)||!Read(at+0xC,p.name)||!IsValidUserPtr(p.name)||
        !Read(at+0x14,enabled)||!Read(at+0x15,removing)||enabled>1||removing>1)return false;
    p.active=enabled==1&&removing==0;out=p;return true;
}
static bool Owned(const char* name,uintptr_t handler,bool requireTop,uintptr_t uiGlobal){
    if(!name||!IsValidUserPtr(handler))return false;
    uintptr_t cursor=0;if(!First(uiGlobal,cursor))return false;
    std::array<uintptr_t,64> seen{};unsigned count=0,hits=0;bool blocked=false,matchBlocked=false;
    while(cursor){
        if(count==seen.size()||std::find(seen.begin(),seen.begin()+count,cursor)!=seen.begin()+count)return false;
        seen[count++]=cursor;Package p;if(!ReadPackage(cursor,p))return false;
        if(p.active){
            if(Name(p.name,name)){
                uintptr_t descriptor=0,bound=0,registered=0,handlerName=0;
                if(!Read(cursor+0x40,descriptor)||!IsValidUserPtr(descriptor)||!Read(descriptor+0x18,bound)||bound!=cursor||
                    !Read(descriptor+0x14,registered)||registered!=handler||!Read(handler+4,handlerName)||!Name(handlerName,name))return false;
                ++hits;matchBlocked=blocked;
            }else if(!Passive(p.name))blocked=true;
        }
        cursor=p.next;
    }
    return hits==1&&(!requireTop||!matchBlocked);
}
static bool Idle(uintptr_t uiGlobal){
    uintptr_t cursor=0;if(!First(uiGlobal,cursor))return false;
    std::array<uintptr_t,64> seen{};unsigned count=0;bool hud=false;
    while(cursor){
        if(count==seen.size()||std::find(seen.begin(),seen.begin()+count,cursor)!=seen.begin()+count)return false;
        seen[count++]=cursor;Package p;if(!ReadPackage(cursor,p))return false;
        if(p.active){if(!Passive(p.name))return false;if(Name(p.name,"HUD_SingleRace.fng"))hud=true;}
        cursor=p.next;
    }
    return hud;
}
// A registry handler can outlive its screen. Conversely, a queued/disabled
// package can still own teardown. Inspect BOTH real lists, not FindHandler's
// cache, before declaring a native surface gone.
static bool Absent(const char* const* names,size_t nameCount,uintptr_t uiGlobal){
    uintptr_t ui=0,engine=0;
    if(!names||!nameCount||!Read(uiGlobal,ui)||!IsValidUserPtr(ui)||
        !Read(ui+8,engine)||!IsValidUserPtr(engine))return false;
    for(unsigned offset:{0xE4u,0xF4u}){
        uintptr_t cursor=0;if(!Read(engine+offset,cursor))return false;
        std::array<uintptr_t,64> seen{};unsigned count=0;
        while(cursor){
            if(count==seen.size()||std::find(seen.begin(),seen.begin()+count,cursor)!=seen.begin()+count)return false;
            seen[count++]=cursor;Package p;if(!ReadPackage(cursor,p))return false;
            for(size_t i=0;i<nameCount;++i)if(Name(p.name,names[i]))return false;
            cursor=p.next;
        }
    }
    return true;
}
}}
