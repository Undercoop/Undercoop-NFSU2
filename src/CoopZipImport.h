// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Only the vetted raw inflater participates. No miniz archive/filesystem,
// compression, zlib compatibility or unbounded allocating API is used here.
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_MALLOC
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4127 4244 4706)
#endif
#include "third_party/miniz/miniz_tinfl.c"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#undef MINIZ_NO_STDIO
#undef MINIZ_NO_TIME
#undef MINIZ_NO_DEFLATE_APIS
#undef MINIZ_NO_ARCHIVE_APIS
#undef MINIZ_NO_ARCHIVE_WRITING_APIS
#undef MINIZ_NO_ZLIB_APIS
#undef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#undef MINIZ_NO_MALLOC

// Pure ZIP32 validation/extraction, never filesystem writes or execution.
// Version 1 deliberately permits ASCII Windows-safe names only, matching the
// package manifest path format. Non-ASCII (including valid UTF-8) is explicitly
// unsupported, not guessed via the machine's active ANSI codepage.
namespace coop_zip {
constexpr uint64_t kMaxCompressed=128ull*1024*1024;
constexpr uint64_t kMaxUnpacked=512ull*1024*1024;
constexpr uint64_t kMaxFile=128ull*1024*1024;
constexpr size_t kMaxEntries=4096;
struct Limits {
    uint64_t compressedBytes=kMaxCompressed,unpackedBytes=kMaxUnpacked,fileBytes=kMaxFile;
    size_t entries=kMaxEntries,pathBytes=1024;
};
struct Entry {
    std::string path;bool directory=false;size_t index=0;uint64_t bytes=0,compressed=0;
};
namespace detail {
inline bool Fail(std::string& error,const char* why){error=why;return false;}
inline uint16_t U16(const std::string& bytes,size_t at){
    return uint16_t(static_cast<unsigned char>(bytes[at]))|
        uint16_t(uint16_t(static_cast<unsigned char>(bytes[at+1]))<<8);
}
inline uint32_t U32(const std::string& bytes,size_t at){
    return uint32_t(U16(bytes,at))|(uint32_t(U16(bytes,at+2))<<16);
}
inline bool Span(size_t at,size_t bytes,size_t end){return at<=end&&bytes<=end-at;}
inline std::string Fold(std::string value){for(auto& ch:value)if(ch>='A'&&ch<='Z')ch=char(ch-'A'+'a');return value;}
inline bool Utf8(const std::string& value){
    for(size_t i=0;i<value.size();){const auto first=static_cast<unsigned char>(value[i++]);
        if(first<0x80)continue;uint32_t code=0,minimum=0;size_t more=0;
        if(first>=0xc2&&first<=0xdf){code=first&31;minimum=0x80;more=1;}
        else if(first>=0xe0&&first<=0xef){code=first&15;minimum=0x800;more=2;}
        else if(first>=0xf0&&first<=0xf4){code=first&7;minimum=0x10000;more=3;}else return false;
        if(more>value.size()-i)return false;
        while(more--){const auto next=static_cast<unsigned char>(value[i++]);if((next&0xc0)!=0x80)return false;code=(code<<6)|(next&63);}
        if(code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
    }return true;
}
inline bool Device(std::string component){
    component=Fold(component.substr(0,component.find('.')));
    while(!component.empty()&&(component.back()==' '||component.back()=='.'))component.pop_back();
    return component=="con"||component=="prn"||component=="aux"||component=="nul"||component=="clock$"||
        component=="conin$"||component=="conout$"||(component.size()==4&&
        (component.compare(0,3,"com")==0||component.compare(0,3,"lpt")==0)&&component[3]>='1'&&component[3]<='9');
}
inline bool Path(const std::string& raw,std::string& path,bool& directory,std::string& error,const Limits& limits){
    path.clear();directory=false;
    if(raw.empty()||raw.size()>limits.pathBytes)return Fail(error,"ZIP path is empty or too long.");
    if(!Utf8(raw))return Fail(error,"ZIP filename is not valid UTF-8/ASCII.");
    for(unsigned char ch:raw){
        if(ch>=128)return Fail(error,"Non-ASCII ZIP paths are not supported in this preview.");
        if(ch<32||ch==127||ch==':'||ch=='*'||ch=='?'||ch=='\"'||ch=='<'||ch=='>'||ch=='|')
            return Fail(error,"ZIP contains an unsafe Windows filename.");
    }
    path=raw;for(auto& ch:path)if(ch=='\\')ch='/';
    if(path.front()=='/')return Fail(error,"Absolute ZIP paths are not allowed.");
    directory=path.back()=='/';if(directory)path.pop_back();
    if(path.empty())return Fail(error,"ZIP contains an empty root path.");
    size_t begin=0;
    for(size_t at=0;at<=path.size();++at)if(at==path.size()||path[at]=='/'){
        const auto component=path.substr(begin,at-begin);
        if(component.empty()||component=="."||component==".."||component.back()=='.'||component.back()==' '||
            component.size()>255||Device(component))return Fail(error,"ZIP contains an unsafe path component or device name.");
        begin=at+1;
    }
    return true;
}
struct Node{std::string path;bool directory=false,explicitEntry=false;};
inline bool Claim(const Entry& entry,std::map<std::string,Node>& nodes,std::string& error){
    for(size_t at=0;at<=entry.path.size();++at)if(at==entry.path.size()||entry.path[at]=='/'){
        const bool full=at==entry.path.size(),directory=!full||entry.directory;
        const auto path=entry.path.substr(0,at),key=Fold(path);const auto previous=nodes.find(key);
        if(previous!=nodes.end()){
            if(previous->second.path!=path)return Fail(error,"ZIP filenames collide after Windows case folding.");
            if(!previous->second.directory||!directory||(full&&previous->second.explicitEntry))
                return Fail(error,"ZIP contains duplicate names or a file/directory conflict.");
            if(full)previous->second.explicitEntry=true;
        }else nodes.emplace(key,Node{path,directory,full});
    }return true;
}
inline uint32_t Crc(const char* bytes,size_t count){
    static const std::array<uint32_t,256> table=[](){std::array<uint32_t,256> out{};
        for(uint32_t i=0;i<256;++i){uint32_t crc=i;for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320u:0);out[i]=crc;}return out;}();
    uint32_t crc=0xffffffffu;for(size_t i=0;i<count;++i)crc=table[(crc^static_cast<unsigned char>(bytes[i]))&255]^(crc>>8);
    return crc^0xffffffffu;
}
inline bool Extra(const std::string& raw,size_t at,size_t count,std::string& error){
    const size_t end=at+count;
    while(at<end){if(!Span(at,4,end))return Fail(error,"ZIP extra metadata is truncated.");
        const auto kind=U16(raw,at),length=U16(raw,at+2);at+=4;
        if(!Span(at,length,end))return Fail(error,"ZIP extra metadata exceeds its header.");
        if(kind==0x0001)return Fail(error,"ZIP64 archives are not supported.");
        if(kind==0x9901)return Fail(error,"Encrypted ZIP archives are not supported.");
        if(kind==0x7075)return Fail(error,"Alternative Unicode ZIP paths are not supported in this preview.");
        at+=length;
    }return true;
}
} // namespace detail

class Archive {
    struct Record{size_t data=0;uint32_t crc=0;uint16_t method=0;};
    std::string raw_;std::vector<Entry> entries_;std::vector<Record> records_;Limits limits_;bool valid_=false;
    void Clear(){valid_=false;std::string{}.swap(raw_);entries_.clear();records_.clear();}
    bool Parse(std::string& error){
        using namespace detail;
        if(raw_.size()>limits_.compressedBytes)return Fail(error,"ZIP exceeds the compressed archive size limit.");
        if(raw_.size()<22)return Fail(error,"ZIP end record is missing or truncated.");
        const size_t search=raw_.size()>65557?raw_.size()-65557:0;size_t eocd=SIZE_MAX;
        for(size_t at=raw_.size()-22;;--at){
            if(U32(raw_,at)==0x06054b50&&Span(at+22,U16(raw_,at+20),raw_.size())&&at+22+U16(raw_,at+20)==raw_.size()){
                const uint32_t offset=U32(raw_,at+16),countBytes=U32(raw_,at+12);
                if(Span(offset,countBytes,at)&&size_t(offset)+countBytes==at){eocd=at;break;}
            }
            if(at==search)break;
        }
        if(eocd==SIZE_MAX)return Fail(error,"ZIP end record is missing or has trailing data.");
        const auto disk=U16(raw_,eocd+4),centralDisk=U16(raw_,eocd+6),diskEntries=U16(raw_,eocd+8),count=U16(raw_,eocd+10);
        const uint32_t centralBytes=U32(raw_,eocd+12),centralOffset=U32(raw_,eocd+16);
        if(disk||centralDisk||diskEntries!=count)return Fail(error,"Multi-disk ZIP archives are not supported.");
        if(count==0xffff||centralBytes==0xffffffffu||centralOffset==0xffffffffu)return Fail(error,"ZIP64 archives are not supported.");
        if(count>limits_.entries)return Fail(error,"ZIP exceeds the entry count limit.");
        if(!Span(centralOffset,centralBytes,eocd)||size_t(centralOffset)+centralBytes!=eocd)
            return Fail(error,"ZIP central directory bounds are inconsistent.");
        std::map<std::string,Node> paths;std::vector<std::pair<size_t,size_t>> regions;uint64_t unpacked=0;size_t at=centralOffset;
        for(size_t i=0;i<count;++i){
            if(!Span(at,46,eocd)||U32(raw_,at)!=0x02014b50)return Fail(error,"ZIP central entry is truncated or invalid.");
            const uint16_t needed=U16(raw_,at+6),flags=U16(raw_,at+8),method=U16(raw_,at+10);
            const uint32_t crc=U32(raw_,at+16),compressed=U32(raw_,at+20),bytes=U32(raw_,at+24),attributes=U32(raw_,at+38),local=U32(raw_,at+42);
            const size_t names=U16(raw_,at+28),extras=U16(raw_,at+30),comments=U16(raw_,at+32);
            if(U16(raw_,at+34))return Fail(error,"Multi-disk ZIP entries are not supported.");
            if(bytes==0xffffffffu||compressed==0xffffffffu||local==0xffffffffu||needed>=45)return Fail(error,"ZIP64 or extended ZIP entries are not supported.");
            if(needed<10||needed>20||(flags&uint16_t(~0x080e))||(method!=0&&method!=8)||(method==0&&(flags&6)))
                return Fail(error,"ZIP uses encryption, unsupported flags or a compression method other than store/deflate.");
            if(bytes>limits_.fileBytes||compressed>limits_.compressedBytes||bytes>limits_.unpackedBytes-unpacked)
                return Fail(error,"ZIP exceeds a file or total unpacked size limit.");
            if((method==0&&compressed!=bytes)||(method==8&&!compressed))return Fail(error,"ZIP compressed and unpacked sizes are inconsistent.");
            unpacked+=bytes;
            const size_t metadata=names+extras+comments;
            if(!Span(at+46,metadata,eocd))return Fail(error,"ZIP central filename or metadata is truncated.");
            const auto name=raw_.substr(at+46,names);Entry entry;entry.index=i;entry.bytes=bytes;entry.compressed=compressed;
            if(!Path(name,entry.path,entry.directory,error,limits_))return false;
            const uint32_t mode=attributes>>16,type=mode&0170000;
            // Never materialize symlinks, devices, sockets, FIFOs or reparse
            // metadata. This also rejects misleading DOS directory attributes.
            if((attributes&0x400)||(type&&type!=0100000&&type!=0040000)||
                ((attributes&0x10)&&!entry.directory)||(type==0040000&&!entry.directory)||
                (type==0100000&&entry.directory))return Fail(error,"ZIP contains a link, special file or mismatched directory metadata.");
            if(entry.directory&&(bytes||crc))return Fail(error,"ZIP directory entries cannot contain file data.");
            if(!Extra(raw_,at+46+names,extras,error)||!Claim(entry,paths,error))return false;
            if(!Span(local,30,centralOffset)||U32(raw_,local)!=0x04034b50)return Fail(error,"ZIP local header is outside the payload or invalid.");
            const size_t localNames=U16(raw_,local+26),localExtras=U16(raw_,local+28);
            if(U16(raw_,local+4)!=needed||U16(raw_,local+6)!=flags||U16(raw_,local+8)!=method||
                U16(raw_,local+10)!=U16(raw_,at+12)||U16(raw_,local+12)!=U16(raw_,at+14)||localNames!=names)
                return Fail(error,"ZIP local and central headers disagree.");
            if(!Span(size_t(local)+30,localNames+localExtras,centralOffset)||raw_.compare(size_t(local)+30,localNames,name)!=0)
                return Fail(error,"ZIP local and central filenames disagree.");
            if(!Extra(raw_,size_t(local)+30+localNames,localExtras,error))return false;
            const size_t data=size_t(local)+30+localNames+localExtras;
            if(!Span(data,compressed,centralOffset))return Fail(error,"ZIP entry payload exceeds its bounds.");
            size_t end=data+compressed;
            const uint32_t localCrc=U32(raw_,local+14),localCompressed=U32(raw_,local+18),localBytes=U32(raw_,local+22);
            if(flags&8){
                if((localCrc&&localCrc!=crc)||(localCompressed&&localCompressed!=compressed)||(localBytes&&localBytes!=bytes))
                    return Fail(error,"ZIP descriptor-mode local sizes disagree with the central entry.");
                bool descriptor=false;
                if(Span(end,16,centralOffset)&&U32(raw_,end)==0x08074b50&&U32(raw_,end+4)==crc&&
                    U32(raw_,end+8)==compressed&&U32(raw_,end+12)==bytes){end+=16;descriptor=true;}
                else if(Span(end,12,centralOffset)&&U32(raw_,end)==crc&&U32(raw_,end+4)==compressed&&U32(raw_,end+8)==bytes){end+=12;descriptor=true;}
                if(!descriptor)return Fail(error,"ZIP data descriptor is missing or inconsistent.");
            }else if(localCrc!=crc||localCompressed!=compressed||localBytes!=bytes)
                return Fail(error,"ZIP local CRC or sizes disagree with the central entry.");
            regions.push_back({local,end});entries_.push_back(std::move(entry));records_.push_back({data,crc,method});at+=46+metadata;
        }
        if(at!=eocd)return Fail(error,"ZIP central entry count does not match its directory size.");
        std::sort(regions.begin(),regions.end());size_t end=0;
        for(const auto& region:regions){if(region.first!=end)return Fail(error,"ZIP payloads overlap, contain gaps or contain an unsupported executable prefix.");end=region.second;}
        if(end!=centralOffset)return Fail(error,"ZIP contains unreferenced payload bytes.");
        return true;
    }
public:
    explicit Archive(Limits limits=Limits{}):limits_(limits){
        limits_.compressedBytes=(std::min)(limits_.compressedBytes,kMaxCompressed);limits_.unpackedBytes=(std::min)(limits_.unpackedBytes,kMaxUnpacked);
        limits_.fileBytes=(std::min)(limits_.fileBytes,kMaxFile);limits_.entries=(std::min)(limits_.entries,kMaxEntries);limits_.pathBytes=(std::min)(limits_.pathBytes,size_t(1024));
    }
    Archive(const Archive&)=delete;Archive& operator=(const Archive&)=delete;
    Archive(Archive&&)=default;Archive& operator=(Archive&&)=default;
    bool Open(std::string bytes,std::string& error){
        Clear();error.clear();
        try{raw_=std::move(bytes);if(!Parse(error)){Clear();return false;}valid_=true;return true;}
        catch(const std::bad_alloc&){Clear();return detail::Fail(error,"Not enough memory to inspect this ZIP safely.");}
        catch(const std::length_error&){Clear();return detail::Fail(error,"ZIP allocation exceeds the supported size.");}
    }
    const std::vector<Entry>& Entries()const{return entries_;}
    // entryIndex is the zero-based Entry.index, not an untrusted filename.
    // Failure leaves output unchanged. No partial decompressed data is returned.
    bool Extract(size_t entryIndex,std::string& output,std::string& error)const{
        error.clear();if(!valid_||entryIndex>=entries_.size())return detail::Fail(error,"ZIP entry is not open or does not exist.");
        try{const auto& entry=entries_[entryIndex];const auto& record=records_[entryIndex];std::string decoded;
            if(record.method==0)decoded.assign(raw_,record.data,size_t(entry.bytes));
            else{decoded.assign(size_t(entry.bytes),0);unsigned char empty=0;
                auto* destination=decoded.empty()?&empty:reinterpret_cast<unsigned char*>(decoded.data());
                size_t input=size_t(entry.compressed),written=size_t(entry.bytes);tinfl_decompressor inflater{};tinfl_init(&inflater);
                const auto status=tinfl_decompress(&inflater,reinterpret_cast<const unsigned char*>(raw_.data()+record.data),&input,
                    destination,destination,&written,TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
                if(status!=TINFL_STATUS_DONE||input!=entry.compressed||written!=entry.bytes)
                    return detail::Fail(error,"ZIP deflate stream is corrupt, incomplete or larger than declared.");
            }
            if(detail::Crc(decoded.data(),decoded.size())!=record.crc)return detail::Fail(error,"ZIP entry CRC check failed.");
            output.swap(decoded);return true;
        }catch(const std::bad_alloc&){return detail::Fail(error,"Not enough memory to extract this ZIP entry safely.");}
        catch(const std::length_error&){return detail::Fail(error,"ZIP entry exceeds the supported allocation size.");}
    }
};
} // namespace coop_zip
