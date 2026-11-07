// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>

// Bounded, value-only notice history. No game, font or window calls here.
namespace coop_notices {
constexpr std::size_t kHistory=32, kText=768;
enum class AutoShow : std::uint8_t { Manual=0, Invitations=1, All=2 };
struct Entry { std::array<char,kText> text{}; std::uint32_t at=0; };
inline void Normalize(const char* input,char* output,std::size_t size) {
    if(!output || !size) return;
    std::size_t n=0;
    if(input) for(;*input && n+1<size;++input) {
        const unsigned char c=static_cast<unsigned char>(*input);
        if(c=='^' || c=='\n') { if(n && output[n-1]!='\n') output[n++]='\n'; }
        else if(c>=32 || c=='\t') output[n++]=c=='\t'?' ':static_cast<char>(c);
    }
    while(n && (output[n-1]=='\n' || output[n-1]==' ')) --n;
    output[n]=0;
}
struct Model {
    std::array<Entry,kHistory> entries{};
    std::array<char,kText> live{},debug{},liveTitle{},sponsorPrompt{};
    std::size_t count=0,next=0,scroll=0;
    std::uint32_t revision=1,until=0;
    bool expanded=false,transient=false;
    AutoShow autoShow=AutoShow::Invitations;
    std::uint8_t backgroundAlpha=179; // 70%; text remains legible.
    void Configure(int automatic,int opacity) {
        const auto show=static_cast<AutoShow>(std::clamp(automatic,0,2));
        const auto alpha=static_cast<std::uint8_t>((std::clamp(opacity,10,100)*255+50)/100);
        if(autoShow!=show || backgroundAlpha!=alpha) {autoShow=show;backgroundAlpha=alpha;++revision;}
    }
    void Reset() {
        const auto version=revision+1;const auto show=autoShow;const auto alpha=backgroundAlpha;
        *this={};revision=version?version:1;autoShow=show;backgroundAlpha=alpha;
    }
    const Entry& Recent(std::size_t index) const { return entries[(next+kHistory-1-index)%kHistory]; }
    bool Visible(std::uint32_t now) const {
        return expanded || sponsorPrompt[0] || (autoShow!=AutoShow::Manual && live[0]) ||
            (autoShow==AutoShow::All && transient && static_cast<std::int32_t>(until-now)>0);
    }
    void Add(const char* text,std::uint32_t now) {
        Entry e{};Normalize(text,e.text.data(),e.text.size());if(!e.text[0]) return;
        if(count && !std::strcmp(Recent(0).text.data(),e.text.data())) return;
        e.at=now;entries[next]=e;next=(next+1)%kHistory;count=std::min(count+1,kHistory);
        if(scroll) scroll=std::min(scroll+1,count-1);
        until=now+6000;transient=true;++revision;
    }
    void SetLive(const char* text,std::uint32_t now) {
        std::array<char,kText> normalized{};Normalize(text,normalized.data(),normalized.size());
        if(!std::strcmp(live.data(),normalized.data())) return;
        live=normalized;++revision;
        if(!live[0]) {liveTitle={};return;}
        std::array<char,kText> title=normalized;
        if(char* end=std::strchr(title.data(),'\n')) *end=0;
        // Invitation countdown changes must not consume history every second.
        if(std::strcmp(liveTitle.data(),title.data())) {liveTitle=title;Add(normalized.data(),now);}
    }
    void SetDebug(const char* text) {
        std::array<char,kText> normalized{};Normalize(text,normalized.data(),normalized.size());
        if(std::strcmp(debug.data(),normalized.data())) {debug=normalized;++revision;}
    }
    void SetSponsorPrompt(const char* text) {
        std::array<char,kText> normalized{};Normalize(text,normalized.data(),normalized.size());
        if(sponsorPrompt!=normalized){sponsorPrompt=normalized;++revision;}
    }
    void Toggle() {expanded=!expanded;scroll=0;++revision;}
    bool Close() {if(!expanded) return false;expanded=false;++revision;return true;}
    void Scroll(int delta) {
        if(!expanded || !count) return;
        const auto value=static_cast<std::int64_t>(scroll)+delta;
        const auto bounded=static_cast<std::size_t>(std::clamp<std::int64_t>(value,0,count-1));
        if(scroll!=bounded) {scroll=bounded;++revision;}
    }
};
}
