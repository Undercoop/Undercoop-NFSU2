// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopModPackages.h"
#include <cstdint>
#include <limits>
#include <sstream>

namespace coop_import {
struct File{std::string source,destination,hash;uint64_t bytes=0;bool module=false,config=false;};
struct Receipt{std::string id,version,archive;std::vector<File> files;};
inline bool Hash(const std::string& s){if(s.size()!=64)return false;for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;}
inline bool Number(const std::string& s,uint64_t& value){value=0;if(s.empty()||s.size()>16)return false;for(char c:s){if(c<'0'||c>'9'||value>(UINT64_MAX-9)/10)return false;value=value*10+uint64_t(c-'0');}return true;}
inline bool Valid(const Receipt& r){
    coop_packages::Limits limits;if(!coop_packages::detail::Id(r.id,limits)||!coop_packages::detail::Version(r.version,limits)||!Hash(r.archive)||r.files.empty()||r.files.size()>4096)return false;
    std::set<std::string> paths,dest;std::map<std::string,std::string> libraries;size_t modules=0;uint64_t total=0;
    for(const auto& f:r.files){std::string a,b;
        if(!coop_packages::CanonicalPath(f.source,a)||a!=f.source||a.rfind("files/",0)!=0||!coop_packages::CanonicalPath(f.destination,b)||b!=f.destination||
            coop_packages::detail::ReservedDestination(b)||!Hash(f.hash)||f.bytes>128ull*1024*1024||!paths.insert(a).second||!dest.insert(b).second)return false;
        const auto ext=coop_packages::detail::Extension(a);if(ext!=coop_packages::detail::Extension(b)||b=="scripts/nfsu2coop.asi"||b=="scripts/dinput8.dll")return false;
        const auto first=b.substr(0,b.find('/'));if(first!="scripts"&&first!="cars"&&first!="global"&&first!="frontend"&&first!="languages"&&first!="movies"&&first!="sound"&&first!="tracks")return false;
        if((ext==".asi"||ext==".dll")&&first!="scripts")return false;
        if(ext==".dll"){const auto name=a.substr(a.find_last_of('/')+1);const auto found=libraries.find(name);if(found!=libraries.end()&&found->second!=f.hash)return false;libraries[name]=f.hash;}
        if(f.module){if(ext!=".asi"||++modules>8||f.config)return false;}
        else if(ext!=".dll"&&!coop_packages::detail::Asset(a))return false;
        if(f.config&&ext!=".ini")return false;total+=f.bytes;if(total>512ull*1024*1024)return false;
    }
    for(const auto& path:paths)for(size_t slash=path.find('/');slash!=std::string::npos;slash=path.find('/',slash+1))if(paths.count(path.substr(0,slash)))return false;
    for(const auto& path:dest)for(size_t slash=path.find('/');slash!=std::string::npos;slash=path.find('/',slash+1))if(dest.count(path.substr(0,slash)))return false;
    return modules>0;
}
inline std::string Encode(const Receipt& r){
    std::string text="Undercoop native import 1\n"+r.id+"\n"+r.version+"\n"+r.archive+"\n";
    for(const auto& f:r.files)text+=f.source+"|"+f.destination+"|"+f.hash+"|"+std::to_string(f.bytes)+"|"+(f.module?"m":f.config?"c":"d")+"\n";return text;
}
inline bool Decode(const std::string& text,Receipt& r){
    r={};if(text.size()>1024*1024||text.find('\r')!=std::string::npos||text.find('\0')!=std::string::npos)return false;
    std::istringstream in(text);std::string line;if(!std::getline(in,line)||line!="Undercoop native import 1"||!std::getline(in,r.id)||!std::getline(in,r.version)||!std::getline(in,r.archive))return false;
    while(std::getline(in,line)){std::vector<std::string> parts;size_t start=0;for(;;){const auto end=line.find('|',start);parts.push_back(line.substr(start,end==std::string::npos?end:end-start));if(end==std::string::npos)break;start=end+1;}
        if(parts.size()!=5||parts[4].size()!=1||(parts[4]!="m"&&parts[4]!="c"&&parts[4]!="d"))return false;
        File f;f.source=parts[0];f.destination=parts[1];f.hash=parts[2];if(!Number(parts[3],f.bytes))return false;f.module=parts[4]=="m";f.config=parts[4]=="c";r.files.push_back(std::move(f));if(r.files.size()>4096)return false;
    }return Valid(r);
}
// Read-only PE32 import inspection, never LoadLibrary/DllMain.
inline uint16_t U16(const std::string& b,size_t p){return uint16_t(uint8_t(b[p]))|uint16_t(uint8_t(b[p+1]))<<8;}
inline uint32_t U32(const std::string& b,size_t p){return uint32_t(U16(b,p))|uint32_t(U16(b,p+2))<<16;}
inline bool PE(const std::string& b,std::vector<std::string>& imports){
    imports.clear();if(b.size()<64||b.compare(0,2,"MZ"))return false;const size_t pe=U32(b,60);
    if(pe>b.size()-24||b.compare(pe,4,std::string("PE\0\0",4))||U16(b,pe+4)!=0x14c||!(U16(b,pe+22)&0x2000))return false;
    const size_t count=U16(b,pe+6),optional=pe+24,osize=U16(b,pe+20),sections=optional+osize;
    if(!count||count>96||osize<112||optional>b.size()||osize>b.size()-optional||U16(b,optional)!=0x10b||sections>b.size()||count>(b.size()-sections)/40)return false;
    auto offset=[&](uint32_t rva,size_t bytes)->size_t{for(size_t i=0;i<count;++i){const size_t at=sections+i*40;const uint32_t va=U32(b,at+12),raw=U32(b,at+20),length=U32(b,at+16);
            if(rva>=va&&uint64_t(rva-va)+bytes<=length&&uint64_t(raw)+(rva-va)+bytes<=b.size())return size_t(raw)+(rva-va);}return SIZE_MAX;};
    auto string=[&](uint32_t rva,std::string& name){const auto p=offset(rva,1);if(p==SIZE_MAX)return false;const auto end=b.find('\0',p);if(end==std::string::npos||end-p>128||end==p)return false;
        if(offset(rva,end-p+1)!=p)return false;
        name=coop_packages::detail::Lower(b.substr(p,end-p));for(char c:name)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return false;return true;};
    auto directory=[&](uint32_t rva,uint32_t bytes,bool delay){
        const size_t stride=delay?32:20;if(!rva&&!bytes)return true;if(!rva||bytes<stride||bytes>65536)return false;
        for(size_t i=0;i<256&&i<bytes/stride;++i){
            if(uint64_t(rva)+i*stride>UINT32_MAX)return false;const auto at=offset(rva+uint32_t(i*stride),stride);if(at==SIZE_MAX)return false;
            bool empty=true;for(size_t n=0;n<stride;n+=4)empty=empty&&!U32(b,at+n);if(empty)return true;
            uint32_t nameRva=U32(b,at+(delay?4:12));
            if(delay){const auto attributes=U32(b,at);if(attributes>1)return false;if(!attributes){const auto imageBase=U32(b,optional+28);if(nameRva<imageBase)return false;nameRva-=imageBase;}}
            std::string name;if(!string(nameRva,name))return false;if(std::find(imports.begin(),imports.end(),name)==imports.end())imports.push_back(std::move(name));
        }return false;
    };
    return directory(U32(b,optional+104),U32(b,optional+108),false)&&
        (osize<208||directory(U32(b,optional+200),U32(b,optional+204),true));
}
}
