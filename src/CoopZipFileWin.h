// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <chrono>
#include "CoopZipImport.h"

// ZIP32 inspection on a caller-owned, pinned file. Only directory metadata and
// one entry are buffered. No file is created or executed; the borrowed handle
// remains owned by the caller. Calls on an instance must be serialized.
namespace coop_zip_file {
constexpr uint64_t kMaxArchive=2ull*1024*1024*1024;
constexpr uint64_t kMaxUnpacked=2ull*1024*1024*1024;
constexpr uint64_t kMaxFile=128ull*1024*1024;
constexpr uint64_t kMaxDirectory=8ull*1024*1024;
constexpr size_t kMaxEntries=4096;

class Archive {
    struct Record {uint64_t data=0;uint32_t crc=0;uint16_t method=0;};
    HANDLE file_=INVALID_HANDLE_VALUE;
    uint64_t size_=0;
    std::vector<coop_zip::Entry> entries_;
    std::vector<Record> records_;
    const std::atomic<bool>* cancel_=nullptr;
    std::chrono::steady_clock::time_point started_{};
    BY_HANDLE_FILE_INFORMATION identity_{};
    bool valid_=false;

    bool Okay(std::string& error)const {
        if(cancel_&&cancel_->load())return coop_zip::detail::Fail(error,"ZIP operation cancelled.");
        if(std::chrono::steady_clock::now()-started_>std::chrono::minutes(10))
            return coop_zip::detail::Fail(error,"ZIP operation exceeded its time budget.");
        return true;
    }
    bool Stable(std::string& error)const {
        BY_HANDLE_FILE_INFORMATION actual{};
        if(!GetFileInformationByHandle(file_,&actual)||actual.dwVolumeSerialNumber!=identity_.dwVolumeSerialNumber||
            actual.nFileIndexHigh!=identity_.nFileIndexHigh||actual.nFileIndexLow!=identity_.nFileIndexLow||
            actual.nFileSizeHigh!=identity_.nFileSizeHigh||actual.nFileSizeLow!=identity_.nFileSizeLow||
            actual.ftLastWriteTime.dwHighDateTime!=identity_.ftLastWriteTime.dwHighDateTime||
            actual.ftLastWriteTime.dwLowDateTime!=identity_.ftLastWriteTime.dwLowDateTime)
            return coop_zip::detail::Fail(error,"ZIP changed while being inspected or extracted.");
        return true;
    }
    bool Read(uint64_t at,size_t bytes,std::string& out,std::string& error)const {
        if(at>size_||uint64_t(bytes)>size_-at||bytes>kMaxFile)
            return coop_zip::detail::Fail(error,"ZIP read exceeds validated file bounds.");
        out.assign(bytes,0);
        LARGE_INTEGER position{};position.QuadPart=LONGLONG(at);
        if(!SetFilePointerEx(file_,position,nullptr,FILE_BEGIN))return coop_zip::detail::Fail(error,"ZIP file seek failed.");
        for(size_t done=0;done<bytes;){
            if(!Okay(error))return false;
            const DWORD wanted=DWORD((std::min)(bytes-done,size_t(65536)));DWORD got=0;
            if(!ReadFile(file_,out.data()+done,wanted,&got,nullptr)||got!=wanted)
                return coop_zip::detail::Fail(error,"ZIP file read failed or was truncated.");
            done+=got;
        }
        return true;
    }
    bool Parse(std::string& error) {
        using namespace coop_zip::detail;
        if(size_<22)return Fail(error,"ZIP end record is missing or truncated.");
        const size_t tailSize=size_t((std::min)(size_,uint64_t(65557)));
        const uint64_t tailAt=size_-tailSize;std::string tail;
        if(!Read(tailAt,tailSize,tail,error))return false;
        size_t end=SIZE_MAX;
        for(size_t at=tail.size()-22;;--at){
            if(U32(tail,at)==0x06054B50&&at+22+U16(tail,at+20)==tail.size()){
                const auto offset=U32(tail,at+16),bytes=U32(tail,at+12);
                if(uint64_t(offset)+bytes==tailAt+at){end=at;break;}
            }
            if(!at)break;
        }
        if(end==SIZE_MAX)return Fail(error,"ZIP end record is missing or has trailing data.");
        const auto disk=U16(tail,end+4),centralDisk=U16(tail,end+6),diskCount=U16(tail,end+8),count=U16(tail,end+10);
        const auto centralBytes=U32(tail,end+12),centralAt=U32(tail,end+16);
        if(disk||centralDisk||diskCount!=count)return Fail(error,"Multi-disk ZIP archives are not supported.");
        if(count==0xFFFF||centralAt==0xFFFFFFFF||centralBytes==0xFFFFFFFF)
            return Fail(error,"ZIP64 archives are not supported by the large modpack reader.");
        if(count>kMaxEntries||centralBytes>kMaxDirectory||uint64_t(centralAt)+centralBytes!=tailAt+end)
            return Fail(error,"ZIP central directory exceeds its budget or has inconsistent bounds.");
        std::string directory;if(!Read(centralAt,centralBytes,directory,error))return false;
        coop_zip::Limits limits;limits.compressedBytes=kMaxArchive;limits.unpackedBytes=kMaxUnpacked;
        limits.fileBytes=kMaxFile;limits.entries=kMaxEntries;
        std::map<std::string,Node> paths;std::vector<std::pair<uint64_t,uint64_t>> regions;
        uint64_t unpacked=0;size_t at=0;
        for(size_t i=0;i<count;++i){
            if(!Okay(error))return false;
            if(!Span(at,46,directory.size())||U32(directory,at)!=0x02014B50)
                return Fail(error,"ZIP central entry is truncated or invalid.");
            const auto needed=U16(directory,at+6),flags=U16(directory,at+8),method=U16(directory,at+10);
            const auto crc=U32(directory,at+16),compressed=U32(directory,at+20),bytes=U32(directory,at+24);
            const auto attributes=U32(directory,at+38),local=U32(directory,at+42);
            const size_t names=U16(directory,at+28),extras=U16(directory,at+30),comments=U16(directory,at+32);
            if(U16(directory,at+34))return Fail(error,"Multi-disk ZIP entries are not supported.");
            if(bytes==0xFFFFFFFF||compressed==0xFFFFFFFF||local==0xFFFFFFFF||needed>=45)
                return Fail(error,"ZIP64 or extended ZIP entries are not supported.");
            if(needed<10||needed>20||(flags&uint16_t(~0x080E))||(method!=0&&method!=8)||(method==0&&(flags&6)))
                return Fail(error,"ZIP uses encryption, unsupported flags or a compression method other than store/deflate.");
            if(bytes>kMaxFile||compressed>kMaxFile||bytes>kMaxUnpacked-unpacked)
                return Fail(error,"ZIP exceeds a file or total unpacked size budget.");
            if((method==0&&compressed!=bytes)||(method==8&&!compressed))return Fail(error,"ZIP entry sizes are inconsistent.");
            unpacked+=bytes;
            const size_t metadata=names+extras+comments;
            if(!Span(at+46,metadata,directory.size()))return Fail(error,"ZIP central metadata is truncated.");
            const auto name=directory.substr(at+46,names);
            coop_zip::Entry entry;entry.index=i;entry.bytes=bytes;entry.compressed=compressed;
            if(!Path(name,entry.path,entry.directory,error,limits))return false;
            const uint32_t type=(attributes>>16)&0170000;
            if((attributes&0x400)||(type&&type!=0100000&&type!=0040000)||
                ((attributes&0x10)&&!entry.directory)||(type==0040000&&!entry.directory)||(type==0100000&&entry.directory))
                return Fail(error,"ZIP contains links, special files or inconsistent directory attributes.");
            if(entry.directory&&(bytes||crc))return Fail(error,"ZIP directory entries contain file data.");
            if(!Extra(directory,at+46+names,extras,error)||!Claim(entry,paths,error))return false;
            std::string header;
            if(uint64_t(local)+30>centralAt||!Read(local,30,header,error)||U32(header,0)!=0x04034B50)
                return Fail(error,"ZIP local header is outside the payload or invalid.");
            const size_t localNames=U16(header,26),localExtras=U16(header,28);
            if(U16(header,4)!=needed||U16(header,6)!=flags||U16(header,8)!=method||
                U16(header,10)!=U16(directory,at+12)||U16(header,12)!=U16(directory,at+14)||localNames!=names)
                return Fail(error,"ZIP local and central headers disagree.");
            const uint64_t data=uint64_t(local)+30+localNames+localExtras;
            if(data>centralAt||compressed>uint64_t(centralAt)-data)return Fail(error,"ZIP entry exceeds payload bounds.");
            std::string localMetadata;
            if(!Read(uint64_t(local)+30,localNames+localExtras,localMetadata,error)||localMetadata.compare(0,localNames,name)!=0||
                !Extra(localMetadata,localNames,localExtras,error))return Fail(error,"ZIP local path or extra metadata differs.");
            uint64_t payloadEnd=data+compressed;
            const auto localCrc=U32(header,14),localCompressed=U32(header,18),localBytes=U32(header,22);
            if(flags&8){
                if((localCrc&&localCrc!=crc)||(localCompressed&&localCompressed!=compressed)||(localBytes&&localBytes!=bytes))
                    return Fail(error,"ZIP descriptor-mode local sizes disagree.");
                std::string descriptor;const size_t available=size_t((std::min)(uint64_t(16),uint64_t(centralAt)-payloadEnd));
                if(available<12||!Read(payloadEnd,available,descriptor,error))return Fail(error,"ZIP descriptor is truncated.");
                if(available>=16&&U32(descriptor,0)==0x08074B50&&U32(descriptor,4)==crc&&U32(descriptor,8)==compressed&&U32(descriptor,12)==bytes)payloadEnd+=16;
                else if(U32(descriptor,0)==crc&&U32(descriptor,4)==compressed&&U32(descriptor,8)==bytes)payloadEnd+=12;
                else return Fail(error,"ZIP data descriptor is missing or inconsistent.");
            }else if(localCrc!=crc||localCompressed!=compressed||localBytes!=bytes)
                return Fail(error,"ZIP local CRC or sizes disagree.");
            regions.emplace_back(local,payloadEnd);entries_.push_back(std::move(entry));records_.push_back({data,crc,method});
            at+=46+metadata;
        }
        if(at!=directory.size())return Fail(error,"ZIP entry count does not match its central directory.");
        std::sort(regions.begin(),regions.end());uint64_t previous=0;
        for(const auto& region:regions){if(region.first!=previous)return Fail(error,"ZIP payloads overlap, contain gaps or have an executable prefix.");previous=region.second;}
        if(previous!=centralAt)return Fail(error,"ZIP contains unreferenced payload bytes.");
        return Stable(error)&&Okay(error);
    }
    void Clear(){file_=INVALID_HANDLE_VALUE;size_=0;entries_.clear();records_.clear();valid_=false;}
public:
    Archive()=default;
    Archive(const Archive&)=delete;Archive& operator=(const Archive&)=delete;
    bool Open(HANDLE borrowed,std::string& error,const std::atomic<bool>* cancel=nullptr){
        Clear();error.clear();cancel_=cancel;started_=std::chrono::steady_clock::now();file_=borrowed;
        try{
            LARGE_INTEGER size{};
            if(file_==INVALID_HANDLE_VALUE||file_==nullptr||GetFileType(file_)!=FILE_TYPE_DISK||
                !GetFileSizeEx(file_,&size)||size.QuadPart<0||uint64_t(size.QuadPart)>kMaxArchive||
                !GetFileInformationByHandle(file_,&identity_)||
                (identity_.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
                return coop_zip::detail::Fail(error,"ZIP is unreadable, linked or larger than 2 GiB.");
            size_=uint64_t(size.QuadPart);
            if(!Okay(error)||!Parse(error)){Clear();return false;}
            valid_=true;return true;
        }catch(const std::bad_alloc&){Clear();return coop_zip::detail::Fail(error,"Not enough memory for ZIP metadata.");}
        catch(const std::length_error&){Clear();return coop_zip::detail::Fail(error,"ZIP metadata exceeds its allocation budget.");}
    }
    const std::vector<coop_zip::Entry>& Entries()const{return entries_;}
    bool Extract(size_t index,std::string& output,std::string& error)const {
        using namespace coop_zip::detail;error.clear();
        if(!valid_||index>=entries_.size())return Fail(error,"ZIP entry is not open or does not exist.");
        if(!Okay(error)||!Stable(error))return false;
        try{
            const auto& entry=entries_[index];const auto& record=records_[index];std::string input,decoded;
            if(!Read(record.data,size_t(entry.compressed),input,error))return false;
            if(!record.method)decoded=std::move(input);
            else{
                decoded.assign(size_t(entry.bytes),0);unsigned char empty=0;
                auto* destination=decoded.empty()?&empty:reinterpret_cast<unsigned char*>(decoded.data());
                size_t consumed=input.size(),written=decoded.size();tinfl_decompressor inflater{};tinfl_init(&inflater);
                const auto status=tinfl_decompress(&inflater,reinterpret_cast<const unsigned char*>(input.data()),&consumed,
                    destination,destination,&written,TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
                if(status!=TINFL_STATUS_DONE||consumed!=input.size()||written!=entry.bytes)
                    return Fail(error,"ZIP deflate stream is corrupt, incomplete or exceeds its declared size.");
            }
            if(Crc(decoded.data(),decoded.size())!=record.crc)return Fail(error,"ZIP entry CRC verification failed.");
            if(!Okay(error)||!Stable(error))return false;
            output.swap(decoded);return true;
        }catch(const std::bad_alloc&){return Fail(error,"Not enough memory for this ZIP entry.");}
        catch(const std::length_error&){return Fail(error,"ZIP entry exceeds its allocation budget.");}
    }
};
} // namespace coop_zip_file
