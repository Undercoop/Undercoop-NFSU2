// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// A passive peer movie must not take the input focus of an interactive native
// dialog. The player's own captured tutorial producer is a separate path and
// retains its original source/continuation. No native menu is accepted/closed.
namespace CoopMovieSurface {
struct Addresses {uintptr_t ui=0x8384C4;};
static Addresses addresses;
static bool ReadName(uintptr_t at,char (&out)[96]) {
    if(!IsValidUserPtr(at))return false;
    for(size_t i=0;i<sizeof(out);++i){
        if(!ReadGameMemory(at+i,out+i,1))return false;
        if(!out[i])return i!=0;
    }
    return false;
}
static bool Passive(const char* name) {
    return !_strnicmp(name,"HUD_",4) || !_stricmp(name,"ScreenPrintf.fng") ||
        !_stricmp(name,"UI_PC_Help_Bar.fng") || !_stricmp(name,"PC_Loading.fng");
}
static bool Ready() {
    uintptr_t ui=0,engine=0;
    if(!ReadGameMemory(addresses.ui,&ui,4)||!IsValidUserPtr(ui)||
        !ReadGameMemory(ui+8,&engine,4)||!IsValidUserPtr(engine))return false;
    for(unsigned list:{0xE4u,0xF4u}) {
        uintptr_t cursor=0,seen[64]{};size_t count=0;
        if(!ReadGameMemory(engine+list,&cursor,4))return false;
        while(cursor) {
            if(!IsValidUserPtr(cursor)||count==std::size(seen)||
                std::find(seen,seen+count,cursor)!=seen+count)return false;
            seen[count++]=cursor;
            uintptr_t next=0,name=0;uint8_t active=0,removing=0;char text[96]{};
            if(!ReadGameMemory(cursor+4,&next,4)||!ReadGameMemory(cursor+0xC,&name,4)||
                !ReadGameMemory(cursor+0x14,&active,1)||active>1||
                !ReadGameMemory(cursor+0x15,&removing,1)||removing>1||!ReadName(name,text))return false;
            // Pending native operations and removal still own their surfaces.
            // Inactive cached packages in the active list do not own focus.
            if((list==0xF4 || active || removing) && !Passive(text))return false;
            cursor=next;
        }
    }
    return true;
}
static bool SourceGone(const char* source) {
    if(!source || !source[0] || !std::memchr(source,0,64))return false;
    uintptr_t ui=0,engine=0;
    if(!ReadGameMemory(addresses.ui,&ui,4)||!IsValidUserPtr(ui)||
        !ReadGameMemory(ui+8,&engine,4)||!IsValidUserPtr(engine))return false;
    // Stock51CCD0: circular UIData queue, sentinel UIData+10, node next+0,
    // name+8. A close can leave the source in this queue or FEng teardown.
    uintptr_t cursor=0,seen[64]{};size_t count=0;const uintptr_t sentinel=ui+0x10;
    if(!ReadGameMemory(sentinel,&cursor,4))return false;
    while(cursor!=sentinel) {
        if(!IsValidUserPtr(cursor)||count==std::size(seen)||
            std::find(seen,seen+count,cursor)!=seen+count)return false;
        seen[count++]=cursor;uintptr_t next=0,name=0;char text[96]{};
        if(!ReadGameMemory(cursor,&next,4)||!ReadGameMemory(cursor+8,&name,4)||!ReadName(name,text))return false;
        if(!_stricmp(text,source))return false;cursor=next;
    }
    for(unsigned list:{0xE4u,0xF4u}) {
        if(!ReadGameMemory(engine+list,&cursor,4))return false;count=0;
        while(cursor) {
            if(!IsValidUserPtr(cursor)||count==std::size(seen)||
                std::find(seen,seen+count,cursor)!=seen+count)return false;
            seen[count++]=cursor;uintptr_t next=0,name=0;char text[96]{};
            if(!ReadGameMemory(cursor+4,&next,4)||!ReadGameMemory(cursor+0xC,&name,4)||!ReadName(name,text))return false;
            if(!_stricmp(text,source))return false;cursor=next;
        }
    }
    return true;
}
}
bool CoopPeerMoviePresentationReady() {return CoopMovieSurface::Ready();}
bool CoopMovieSourceGone(const char* source) {return CoopMovieSurface::SourceGone(source);}
