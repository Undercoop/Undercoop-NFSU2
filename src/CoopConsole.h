// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "CoopBuildVersion.h"

namespace coop_console {
constexpr unsigned kAcceptKey=0x74,kDeclineKey=0x75; // F5 / F6, never native Enter/Esc.
constexpr std::size_t kMessages=32,kMessageBytes=240;
constexpr const char* kVersion=UNDERCOOP_BUILD_VERSION;
enum class Channel { Chat, System };
struct Message {
    uint16_t player=0;uint32_t at=0;
    std::array<char,24> name{};
    std::array<char,kMessageBytes+1> text{};
};
struct ChatHistory {
    std::array<Message,kMessages> entries{};
    std::size_t count=0,next=0,scroll=0;
    uint32_t revision=1,until=0,unread=0;
    const Message& Recent(std::size_t i)const{return entries[(next+kMessages-1-i)%kMessages];}
    void Add(uint16_t player,const char* name,const char* text,uint32_t now,bool reading) {
        if((player!=1&&player!=2)||!text||!*text)return;
        Message entry{};entry.player=player;entry.at=now;
        std::snprintf(entry.name.data(),entry.name.size(),"%s",name?name:"Player");
        std::snprintf(entry.text.data(),entry.text.size(),"%s",text);
        entries[next]=entry;next=(next+1)%kMessages;count=std::min(kMessages,count+1);
        if(scroll)scroll=std::min(scroll+1,count-1);
        if(!reading)unread=std::min(999u,unread+1);
        until=now+7000;++revision;
    }
    void Scroll(int delta) {
        if(!count)return;
        const auto nextScroll=std::size_t(std::clamp<int64_t>(int64_t(scroll)+delta,0,count-1));
        if(nextScroll!=scroll){scroll=nextScroll;++revision;}
    }
    bool Transient(uint32_t now)const{return count&&int32_t(until-now)>0;}
};
// UTF-16 editor; byte budget is checked before append, including surrogate pairs.
// No game scan-code assumptions and no allocation while typing.
struct Editor {
    std::array<wchar_t,kMessageBytes+1> text{};
    std::size_t length=0,bytes=0;uint16_t surrogate=0;
    bool active=false;
    void Begin(){*this={};active=true;}
    void Clear(){*this={};}
    bool Append(uint32_t c) {
        if(!active)return false;
        if(c>=0xD800&&c<=0xDBFF){surrogate=uint16_t(c);return false;}
        if(c>=0xDC00&&c<=0xDFFF) {
            if(!surrogate)return false;
            c=0x10000+((surrogate-0xD800)<<10)+(c-0xDC00);
        }
        surrogate=0;
        if(c<32||c==127||c>0x10FFFF||(c>=0xD800&&c<=0xDFFF))return false;
        const std::size_t add=c<0x80?1:c<0x800?2:c<0x10000?3:4;
        if(bytes+add>kMessageBytes)return false;
        if(c<0x10000)text[length++]=wchar_t(c);
        else {c-=0x10000;text[length++]=wchar_t(0xD800+(c>>10));text[length++]=wchar_t(0xDC00+(c&1023));}
        bytes+=add;text[length]=0;return true;
    }
    bool Backspace() {
        surrogate=0;if(!active||!length)return false;
        const uint32_t last=uint16_t(text[--length]);
        if(last>=0xDC00&&last<=0xDFFF&&length&&uint16_t(text[length-1])>=0xD800&&uint16_t(text[length-1])<=0xDBFF){--length;bytes-=4;}
        else bytes-=last<0x80?1:last<0x800?2:3;
        text[length]=0;return true;
    }
};
inline void BuildInfo(char* output,std::size_t size,const char* date,const char* time) {
    static constexpr const char* months="JanFebMarAprMayJunJulAugSepOctNovDec";
    static constexpr const char* days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static constexpr int offsets[]={0,3,2,5,0,3,5,1,4,6,2,4};
    int month=0,day=0,year=0;
    if(date&&std::strlen(date)==11) {
        for(int i=0;i<12;++i)if(!std::strncmp(date,months+i*3,3)){month=i+1;break;}
        std::sscanf(date+4,"%d %d",&day,&year);
    }
    const bool valid=month>=1&&month<=12&&day>=1&&day<=31&&year>=1970;
    int weekday=0;if(valid){const int y=year-(month<3);weekday=(y+y/4-y/100+y/400+offsets[month-1]+day)%7;}
    std::snprintf(output,size,"Undercoop: NFSU2 | v%s | Beta | Pre-release\nBuild %02d/%02d/%04d %s %s",
        kVersion,month,day,year,time?time:"--:--:--",valid?days[weekday]:"---");
}
}
