// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Metadata only. This module never opens or modifies a native career save.
namespace coop_career_history {
constexpr std::size_t kMaxEntries=128;
constexpr std::size_t kHeaderBytes=24;
constexpr std::size_t kMaxFileBytes=kHeaderBytes+kMaxEntries*64;
struct Entry { std::string campaign,personal; };

inline char Fold(char c) { return c>='a'&&c<='z'?char(c-'a'+'A'):c; }
inline bool Equal(const std::string& a,const std::string& b) {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)if(Fold(a[i])!=Fold(b[i]))return false;
    return true;
}
inline bool SafeName(const std::string& name) {
    if(name.empty()||name.size()>31||name=="."||name==".."||name.back()=='.'||name.back()==' ')return false;
    for(unsigned char c:name)
        if(c<32||c>126||c=='/'||c=='\\'||c==':'||c=='"'||c=='<'||c=='>'||c=='|'||c=='*'||c=='?')return false;
    const auto stem=name.substr(0,name.find('.'));
    if(Equal(stem,"CON")||Equal(stem,"PRN")||Equal(stem,"AUX")||Equal(stem,"NUL"))return false;
    return !(stem.size()==4&&stem[3]>='1'&&stem[3]<='9'&&
        (Equal(stem.substr(0,3),"COM")||Equal(stem.substr(0,3),"LPT")));
}

class History {
public:
    // Call only after the requested native career actually reached gameplay.
    // A campaign/personal alias is one-to-one: conflicting binds fail closed.
    bool Remember(const std::string& campaign,const std::string& personal) {
        if(!SafeName(campaign)||!SafeName(personal))return false;
        std::size_t found=entries_.size();
        for(std::size_t i=0;i<entries_.size();++i) {
            const bool sameCampaign=Equal(entries_[i].campaign,campaign);
            const bool samePersonal=Equal(entries_[i].personal,personal);
            if(sameCampaign!=samePersonal)return false;
            if(sameCampaign)found=i;
        }
        Entry item{campaign,personal};
        if(found!=entries_.size()) {item=entries_[found];entries_.erase(entries_.begin()+found);}
        entries_.insert(entries_.begin(),std::move(item));
        if(entries_.size()>kMaxEntries)entries_.pop_back();
        return true;
    }
    bool ResolveCampaign(const std::string& personal,std::string& campaign) const {
        campaign.clear();if(!SafeName(personal))return false;
        for(const auto& item:entries_)if(Equal(item.personal,personal)){campaign=item.campaign;return true;}
        return false;
    }
    bool ResolvePersonal(const std::string& campaign,std::string& personal) const {
        personal.clear();if(!SafeName(campaign))return false;
        for(const auto& item:entries_)if(Equal(item.campaign,campaign)){personal=item.personal;return true;}
        return false;
    }
    bool LastCampaign(std::string& out) const {out.clear();if(entries_.empty())return false;out=entries_.front().campaign;return true;}
    bool LastPersonal(std::string& out) const {out.clear();if(entries_.empty())return false;out=entries_.front().personal;return true;}
    bool PreferredCampaign(const std::string& configured,std::string& out) const {
        if(LastCampaign(out))return true;
        out.clear();if(!SafeName(configured))return false;out=configured;return true;
    }
    const std::vector<Entry>& Entries() const {return entries_;}
private:
    std::vector<Entry> entries_;
};

inline std::uint16_t Read16(const std::uint8_t* p){return std::uint16_t(p[0])|(std::uint16_t(p[1])<<8);}
inline std::uint32_t Read32(const std::uint8_t* p){return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);}
inline void Write16(std::uint8_t* p,std::uint16_t x){p[0]=std::uint8_t(x);p[1]=std::uint8_t(x>>8);}
inline void Write32(std::uint8_t* p,std::uint32_t x){for(unsigned i=0;i<4;++i)p[i]=std::uint8_t(x>>(i*8));}
inline std::uint32_t Checksum(const std::uint8_t* data,std::size_t size) {
    std::uint32_t crc=0xffffffffu;
    for(std::size_t i=0;i<size;++i) {
        const auto byte=i>=20&&i<24?0:data[i];
        crc^=byte;
        for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&std::uint32_t(0-int(crc&1)));
    }
    return ~crc;
}
inline bool Encode(const History& history,std::vector<std::uint8_t>& bytes) {
    bytes.clear();const auto& entries=history.Entries();if(entries.size()>kMaxEntries)return false;
    std::size_t payload=0;
    for(const auto& item:entries) {
        if(!SafeName(item.campaign)||!SafeName(item.personal))return false;
        payload+=2+item.campaign.size()+item.personal.size();
    }
    bytes.assign(kHeaderBytes+payload,0);std::memcpy(bytes.data(),"UCARHST1",8);
    Write16(bytes.data()+8,1);Write16(bytes.data()+10,std::uint16_t(kHeaderBytes));
    Write32(bytes.data()+12,std::uint32_t(payload));Write16(bytes.data()+16,std::uint16_t(entries.size()));
    std::size_t cursor=kHeaderBytes;
    for(const auto& item:entries) {
        bytes[cursor++]=std::uint8_t(item.campaign.size());bytes[cursor++]=std::uint8_t(item.personal.size());
        std::memcpy(bytes.data()+cursor,item.campaign.data(),item.campaign.size());cursor+=item.campaign.size();
        std::memcpy(bytes.data()+cursor,item.personal.data(),item.personal.size());cursor+=item.personal.size();
    }
    Write32(bytes.data()+20,Checksum(bytes.data(),bytes.size()));return true;
}
inline bool Decode(const void* source,std::size_t size,History& history) {
    if(!source||size<kHeaderBytes||size>kMaxFileBytes)return false;
    const auto* bytes=static_cast<const std::uint8_t*>(source);
    if(std::memcmp(bytes,"UCARHST1",8)||Read16(bytes+8)!=1||Read16(bytes+10)!=kHeaderBytes||
       Read32(bytes+12)!=size-kHeaderBytes||Read16(bytes+16)>kMaxEntries||Read16(bytes+18)!=0||
       Read32(bytes+20)!=Checksum(bytes,size))return false;
    std::vector<Entry> entries;std::size_t cursor=kHeaderBytes;
    for(unsigned i=0;i<Read16(bytes+16);++i) {
        if(cursor+2>size)return false;
        const auto a=bytes[cursor++],b=bytes[cursor++];
        if(!a||a>31||!b||b>31||cursor+std::size_t(a)+b>size)return false;
        Entry item{std::string(reinterpret_cast<const char*>(bytes+cursor),a),
                   std::string(reinterpret_cast<const char*>(bytes+cursor+a),b)};cursor+=a+b;
        if(!SafeName(item.campaign)||!SafeName(item.personal))return false;
        for(const auto& prior:entries)
            if(Equal(prior.campaign,item.campaign)||Equal(prior.personal,item.personal))return false;
        entries.push_back(std::move(item));
    }
    if(cursor!=size)return false;
    History staged;
    for(auto i=entries.rbegin();i!=entries.rend();++i)if(!staged.Remember(i->campaign,i->personal))return false;
    history=std::move(staged);return true;
}
} // namespace coop_career_history
