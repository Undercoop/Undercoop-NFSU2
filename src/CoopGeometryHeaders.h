// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace coop_geometry {
// Only native, game-relative car geometry files belong to this lease domain.
// Keep indexes available, without inventing a file name from an untrusted peer.
inline bool GeometryPath(const char* path) {
    if (!path) return false;
    char normalized[128]{};
    size_t length=0;
    for (; length<sizeof(normalized); ++length) {
        unsigned char ch=static_cast<unsigned char>(path[length]);
        if (!ch) break;
        if (ch<32 || ch>=127 || ch==':') return false;
        if (ch=='/') ch='\\';
        normalized[length]=static_cast<char>(ch>='a'&&ch<='z'?ch-'a'+'A':ch);
    }
    if (length==sizeof(normalized) || length<19 || std::strncmp(normalized,"CARS\\",5)) return false;
    const char* leaf=nullptr;
    for (char* item=normalized; *item; ++item) if (*item=='\\') {
        if (item==normalized || item[1]=='\\' || !item[1]) return false;
        leaf=item+1;
    }
    if (std::strstr(normalized,"..") || !leaf || std::strncmp(leaf,"GEOMETRY",8)) return false;
    const size_t leafLength=std::strlen(leaf);
    return leafLength>=12 && !std::strcmp(leaf+leafLength-4,".BIN");
}
inline bool SamePath(const char* left,const char* right) {
    for (size_t i=0;i<128;++i) {
        unsigned char a=left[i],b=right[i];
        if(a=='/')a='\\';if(b=='/')b='\\';
        if(a>='a'&&a<='z')a-=32;if(b>='a'&&b<='z')b-=32;
        if(a!=b)return false;if(!a)return true;
    }
    return false;
}
struct Header { uintptr_t address=0; int32_t references=0; char path[128]{}; };
template<size_t Capacity=256> class HeaderLeases {
    struct Lease {uintptr_t address=0;char path[128]{};};
    std::array<Lease,Capacity> leases_{};
public:
    size_t Size() const {size_t n=0;for(const auto& l:leases_)if(l.address)++n;return n;}
    // read(path, Header&) must find a currently linked native header, not just
    // readable old memory. open/close acquire/release exactly our own reference.
    template<class Read,class Open> bool Retain(const char* path,Read read,Open open) {
        if(!GeometryPath(path))return false;
        Header header{};
        if(!read(path,header)||!header.address||header.references<=0||header.references>=65535||!SamePath(path,header.path))return false;
        Lease* empty=nullptr;
        for(auto& l:leases_) {
            if(l.address && SamePath(l.path,path)) {
                if(l.address==header.address)return true;
                l={}; // Native teardown already consumed it. Never release a replacement.
            }
            if(!l.address&&!empty)empty=&l;
        }
        if(!empty || !open(path))return false;
        // Record ownership immediately after native Open succeeds. If a
        // subsequent diagnostic read fails, retirement can still release it.
        empty->address=header.address;std::memcpy(empty->path,header.path,sizeof(empty->path));
        Header after{};
        return read(path,after)&&after.address==header.address&&after.references==header.references+1;
    }
    template<class Read,class Close> bool Drop(const char* path,Read read,Close close) {
        for(auto& l:leases_)if(l.address&&SamePath(l.path,path)) {
            Header header{};bool released=false;
            if(read(l.path,header)&&header.address==l.address&&header.references>0&&SamePath(l.path,header.path)) {
                close(l.path);released=true;
            }
            l={};return released;
        }
        return false;
    }
    template<class Read,class Close> size_t Release(Read read,Close close) {
        size_t released=0;
        for(auto& l:leases_) if(l.address) {
            Header header{};
            if(read(l.path,header)&&header.address==l.address&&header.references>0&&SamePath(l.path,header.path)) {
                close(l.path);++released;
            }
            l={};
        }
        return released;
    }
};
// Generated LOD hashes are not a list of mandatory files. A missing optional
// LOD uses the engine's existing fallback. An indexed but nonresident mesh is
// different: it must finish streaming before we replace a working visual.
struct Residency {unsigned indexed=0,resident=0,missing=0;uint32_t firstMissing=0;};
template<class Probe> inline Residency Inspect(const uint32_t* keys,size_t count,Probe probe) {
    Residency result{};
    for(size_t i=0;i<count;++i) {
        if(!keys[i])continue;
        const unsigned state=probe(keys[i]); // 0=optional/unindexed,1=indexed,2=drawable
        if(!state)continue;
        ++result.indexed;
        if(state==2)++result.resident;
        else {++result.missing;if(!result.firstMissing)result.firstMissing=keys[i];}
    }
    return result;
}
}
