// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopZipImport.h"
#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include "third_party/7zip/CPP/7zip/Archive/IArchive.h"
#include "third_party/7zip/CPP/7zip/IPassword.h"
#pragma comment(lib,"oleaut32.lib")
#pragma comment(lib,"ole32.lib")

// Only a bundled, verified decoder DLL is bound by the launcher. All archive
// input/output is memory-only; volume, password and filesystem APIs are absent.
namespace coop_archive {
using Entry=coop_zip::Entry;using Limits=coop_zip::Limits;
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
inline GUID Iid(BYTE group,BYTE id){return {0x23170F69,0x40C1,0x278A,{0,0,0,group,0,id,0,0}};}
struct Variant {
    PROPVARIANT value{};
    ~Variant(){PropVariantClear(&value);}
    bool Number(uint64_t& out)const{switch(value.vt){case VT_EMPTY:out=0;return true;case VT_UI1:out=value.bVal;return true;
        case VT_UI2:out=value.uiVal;return true;case VT_UI4:out=value.ulVal;return true;case VT_UI8:out=value.uhVal.QuadPart;return true;
        case VT_BOOL:out=value.boolVal!=VARIANT_FALSE;return true;default:return false;}}
    bool Text(std::string& out)const{
        out.clear();if(value.vt==VT_EMPTY)return true;if(value.vt!=VT_BSTR)return false;
        const auto n=SysStringLen(value.bstrVal);if(n>1024)return false;
        for(UINT i=0;i<n;++i){const auto c=value.bstrVal[i];if(c<32||c>126)return false;out+=char(c);}return true;
    }
};
using CreateFn=HRESULT(WINAPI*)(const GUID*,const GUID*,void**);
using FormatsFn=HRESULT(WINAPI*)(UInt32*);
using PropertyFn=HRESULT(WINAPI*)(UInt32,PROPID,PROPVARIANT*);
struct Engine {HMODULE module=nullptr;CreateFn create=nullptr;PropertyFn property=nullptr;UInt32 count=0;};
inline Engine engine;inline std::mutex engineMutex;
inline bool EngineReady(){std::lock_guard<std::mutex> guard(engineMutex);return engine.module!=nullptr;}
inline bool BindEngine(HMODULE module){
    std::lock_guard<std::mutex> guard(engineMutex);if(engine.module)return engine.module==module;if(!module)return false;
    const auto create=reinterpret_cast<CreateFn>(GetProcAddress(module,"CreateObject"));
    const auto formats=reinterpret_cast<FormatsFn>(GetProcAddress(module,"GetNumberOfFormats"));
    const auto property=reinterpret_cast<PropertyFn>(GetProcAddress(module,"GetHandlerProperty2"));UInt32 count=0;
    if(!create||!formats||!property||formats(&count)!=S_OK||!count||count>128)return false;
    using LimitFn=unsigned long long(__cdecl*)();
    const auto single=reinterpret_cast<LimitFn>(GetProcAddress(module,"UndercoopArchiveBudgetSingleLimit"));
    const auto total=reinterpret_cast<LimitFn>(GetProcAddress(module,"UndercoopArchiveBudgetTotalLimit"));
    if(!single||!total||single()!=128ull*1024*1024||total()!=256ull*1024*1024)return false;
    engine={module,create,property,count};return true;
}
inline std::string Stem(std::string name){
    const auto lower=coop_zip::detail::Fold(name);
    for(const auto* suffix:{".tar.bz2",".tar.gz",".tar.xz",".tar.zst",".7zip",".zip",".rar",".7z",".tgz",".tbz2",".txz",".tar",".gzip",".gz",".bz2",".xz",".cab",".arj",".lzh",".lha",".cpio",".iso",".udf",".wim",".xar",".zst",".lzma",".rpm",".deb",".ar",".z"}){
        const auto size=std::strlen(suffix);if(lower.size()>size&&lower.compare(lower.size()-size,size,suffix)==0){name.resize(name.size()-size);return name;}}
    const auto dot=name.find_last_of('.');if(dot!=std::string::npos&&dot)name.resize(dot);return name;
}
inline bool Zip64(const std::string& b){const auto start=b.size()>65557?b.size()-65557:0;
    return b.find(std::string("PK\6\6",4),start)!=std::string::npos||b.find(std::string("PK\6\7",4),start)!=std::string::npos;}
inline bool RequiresEngine(const std::string& b){
    if(Zip64(b))return true;
    return b.size()<10||b.compare(0,4,std::string("PK\3\4",4))||coop_zip::detail::U16(b,4)>20||
        (coop_zip::detail::U16(b,8)!=0&&coop_zip::detail::U16(b,8)!=8);
}
struct Budget {
    Limits limits;const std::atomic<bool>* cancel=nullptr;bool password=false,exceeded=false;
    std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    bool Okay(){return !(cancel&&cancel->load())&&std::chrono::steady_clock::now()-start<std::chrono::seconds(120);}
};
#define COOP_AR_REFS \
    std::atomic<ULONG> refs{1}; \
public: \
    ULONG STDMETHODCALLTYPE AddRef()throw()override{return ++refs;} \
    ULONG STDMETHODCALLTYPE Release()throw()override{const auto n=--refs;if(!n)delete this;return n;} \
private:
class Input final:public IInStream {
    COOP_AR_REFS
    const std::string& bytes;Budget& budget;uint64_t position=0;
public:
    Input(const std::string& b,Budget& v):bytes(b),budget(v){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)throw()override{
        if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown)&&id!=Iid(3,1)&&id!=Iid(3,3))return E_NOINTERFACE;
        *out=static_cast<IInStream*>(this);AddRef();return S_OK;}
    HRESULT STDMETHODCALLTYPE Read(void* data,UInt32 size,UInt32* written)throw()override{
        if(written)*written=0;if(!budget.Okay())return E_ABORT;if(size&&!data)return E_POINTER;
        const auto count=position>=bytes.size()?0:(std::min)(uint64_t(size),uint64_t(bytes.size())-position);
        if(count)std::memcpy(data,bytes.data()+size_t(position),size_t(count));position+=count;if(written)*written=UInt32(count);return S_OK;}
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset,UInt32 origin,UInt64* out)throw()override{
        if(!budget.Okay())return E_ABORT;if(origin>2)return STG_E_INVALIDFUNCTION;
        const uint64_t base=origin==0?0:origin==1?position:bytes.size();
        if(offset<0){const auto distance=uint64_t(-(offset+1))+1;if(distance>base)return STG_E_INVALIDFUNCTION;position=base-distance;}
        else{if(uint64_t(offset)>UINT64_MAX-base)return STG_E_INVALIDFUNCTION;position=base+uint64_t(offset);}
        if(out)*out=position;return S_OK;}
};
class OpenCallback final:public IArchiveOpenCallback,public ICryptoGetTextPassword {
    COOP_AR_REFS
    Budget& budget;
public:
    explicit OpenCallback(Budget& b):budget(b){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)throw()override{
        if(!out)return E_POINTER;*out=nullptr;
        if(id==__uuidof(IUnknown)||id==Iid(6,0x10))*out=static_cast<IArchiveOpenCallback*>(this);
        else if(id==Iid(5,0x10))*out=static_cast<ICryptoGetTextPassword*>(this);else return E_NOINTERFACE;AddRef();return S_OK;}
    HRESULT STDMETHODCALLTYPE SetTotal(const UInt64* files,const UInt64* bytes)throw()override{
        if(!budget.Okay())return E_ABORT;if((files&&*files>budget.limits.entries)||(bytes&&*bytes>budget.limits.unpackedBytes)){budget.exceeded=true;return E_ABORT;}return S_OK;}
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* files,const UInt64* bytes)throw()override{return SetTotal(files,bytes);}
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* out)throw()override{if(out)*out=nullptr;budget.password=true;return E_ACCESSDENIED;}
};
class Output final:public ISequentialOutStream {
    COOP_AR_REFS
    Budget& budget;std::string& bytes;uint64_t limit;
public:
    Output(Budget& b,std::string& data,uint64_t maximum):budget(b),bytes(data),limit(maximum){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)throw()override{
        if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown)&&id!=Iid(3,2))return E_NOINTERFACE;
        *out=static_cast<ISequentialOutStream*>(this);AddRef();return S_OK;}
    HRESULT STDMETHODCALLTYPE Write(const void* data,UInt32 size,UInt32* written)throw()override{
        if(written)*written=0;if(!budget.Okay())return E_ABORT;
        if(size>limit-bytes.size()){budget.exceeded=true;return E_ABORT;}if(size&&!data)return E_POINTER;
        try{if(size)bytes.append(static_cast<const char*>(data),size);}catch(...){return E_OUTOFMEMORY;}
        if(written)*written=size;return S_OK;}
};
class ExtractCallback final:public IArchiveExtractCallback,public ICryptoGetTextPassword,public IArchiveRequestMemoryUseCallback {
    COOP_AR_REFS
    Budget& budget;const std::vector<Entry>& entries;std::vector<std::string>& data;
    const std::vector<bool>& known;size_t current=SIZE_MAX;uint64_t total=0;std::vector<bool> finished;
    const std::vector<UInt32>& indices;
public:
    bool failed=false;Int32 operationResult=-1;
    ExtractCallback(Budget& b,const std::vector<Entry>& e,std::vector<std::string>& d,const std::vector<bool>& k,const std::vector<UInt32>& map):budget(b),entries(e),data(d),known(k),finished(e.size(),false),indices(map){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)throw()override{
        if(!out)return E_POINTER;*out=nullptr;
        if(id==__uuidof(IUnknown)||id==Iid(6,0x20)||id==Iid(0,5))*out=static_cast<IArchiveExtractCallback*>(this);
        else if(id==Iid(5,0x10))*out=static_cast<ICryptoGetTextPassword*>(this);
        else if(id==Iid(6,9))*out=static_cast<IArchiveRequestMemoryUseCallback*>(this);else return E_NOINTERFACE;AddRef();return S_OK;}
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64 value)throw()override{
        if(value>budget.limits.unpackedBytes){budget.exceeded=true;return E_ABORT;}return budget.Okay()?S_OK:E_ABORT;}
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* value)throw()override{return value?SetTotal(*value):(budget.Okay()?S_OK:E_ABORT);}
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index,ISequentialOutStream** out,Int32 mode)throw()override{
        if(!out)return E_POINTER;*out=nullptr;if(!budget.Okay())return E_ABORT;
        const auto found=std::lower_bound(indices.begin(),indices.end(),index);
        if(found==indices.end()||*found!=index||mode!=NArchive::NExtract::NAskMode::kExtract){failed=true;return E_FAIL;}
        current=size_t(found-indices.begin());if(finished[current]){failed=true;return E_FAIL;}if(entries[current].directory)return S_OK;
        const auto limit=known[current]?entries[current].bytes:budget.limits.fileBytes;
        try{*out=new Output(budget,data[current],(std::min)(limit,budget.limits.unpackedBytes-total));}catch(...){return E_OUTOFMEMORY;}return S_OK;}
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32 mode)throw()override{return budget.Okay()&&mode==NArchive::NExtract::NAskMode::kExtract?S_OK:E_ABORT;}
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 result)throw()override{
        operationResult=result;
        if(result!=NArchive::NExtract::NOperationResult::kOK||current>=entries.size()){failed=true;return E_FAIL;}
        const auto size=data[current].size();if((known[current]&&entries[current].bytes!=size)||size>budget.limits.unpackedBytes-total){failed=true;return E_FAIL;}
        total+=size;finished[current]=true;current=SIZE_MAX;return budget.Okay()?S_OK:E_ABORT;}
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* out)throw()override{if(out)*out=nullptr;budget.password=true;return E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE RequestMemoryUse(UInt32,UInt32,UInt32,const wchar_t*,UInt64 size,UInt64* allowed,UInt32* answer)throw()override{
        const auto maximum=(std::min)(budget.limits.unpackedBytes,uint64_t(128)*1024*1024);
        if(allowed)*allowed=maximum;if(answer)*answer=size<=maximum?NRequestMemoryAnswerFlags::k_Allow:NRequestMemoryAnswerFlags::k_Stop;
        if(size>maximum){budget.exceeded=true;return E_OUTOFMEMORY;}return S_OK;}
    bool Complete()const{return !failed&&std::find(finished.begin(),finished.end(),false)==finished.end();}
};
#undef COOP_AR_REFS
inline bool Allowed(std::string name){name=coop_zip::detail::Fold(name);
    for(const auto* format:{"7z","rar","rar5","zip","tar","gzip","bzip2","xz","zstd","cab","arj","lzh","cpio","iso","udf","wim","xar","z","lzma","lzma86","ar","rpm","deb"})if(name==format)return true;return false;}
inline bool StreamFormat(std::string name){name=coop_zip::detail::Fold(name);return name=="gzip"||name=="bzip2"||name=="xz"||name=="zstd"||name=="z"||name=="lzma"||name=="lzma86";}
inline bool TarHeader(const std::string& data){
    if(data.size()<512)return false;uint64_t sum=0,expected=0;bool digit=false;
    for(size_t i=0;i<512;++i)sum+=i>=148&&i<156?32:uint8_t(data[i]);
    for(size_t i=148;i<156;++i){const auto c=data[i];if(c==' '||c=='\0')continue;if(c<'0'||c>'7')return false;expected=expected*8+uint64_t(c-'0');digit=true;}
    return digit&&sum==expected;
}
class Archive {
    Limits limits;std::unique_ptr<coop_zip::Archive> zip;
    std::vector<Entry> entries;std::vector<std::string> decoded;std::string format;bool valid=false;
    bool Fail(std::string& error,const char* message){valid=false;entries.clear();decoded.clear();zip.reset();error=message;return false;}
    bool Property(IInArchive* arc,UInt32 index,PROPID id,Variant& v){return arc->GetProperty(index,id,&v.value)==S_OK;}
    bool Decode(std::string raw,std::string& error,const std::string& sourceName,Budget& budget,unsigned depth){
        if(depth>2)return Fail(error,"Nested compression exceeds the supported depth.");
        if(raw.size()>(depth?limits.unpackedBytes:limits.compressedBytes))return Fail(error,"Archive exceeds its compressed or expanded size limit.");
        if(raw.size()>=2&&raw.compare(0,2,"MZ")==0)return Fail(error,"Executable/self-extracting installers are not supported. Choose the data archive instead.");
        Engine dll;{std::lock_guard<std::mutex> lock(engineMutex);dll=engine;}
        if(!dll.module)return Fail(error,"The bundled archive decoder is not available.");
        Ptr<IInArchive> arc;Ptr<Input> input;std::string openedFormat;
        for(UInt32 i=0;i<dll.count;++i){if(!budget.Okay())return Fail(error,"Archive inspection cancelled or timed out.");
            Variant name,cls;if(dll.property(i,NArchive::NHandlerPropID::kName,&name.value)!=S_OK||!name.Text(openedFormat)||!Allowed(openedFormat)||
                dll.property(i,NArchive::NHandlerPropID::kClassID,&cls.value)!=S_OK||cls.value.vt!=VT_BSTR||SysStringByteLen(cls.value.bstrVal)!=sizeof(GUID))continue;
            GUID id{};std::memcpy(&id,cls.value.bstrVal,sizeof(id));const auto iid=Iid(6,0x60);Ptr<IInArchive> candidate;
            if(dll.create(&id,&iid,reinterpret_cast<void**>(candidate.GetAddressOf()))!=S_OK||!candidate)continue;
            Ptr<ISetProperties> props;const auto pid=Iid(6,3);
            if(candidate->QueryInterface(pid,reinterpret_cast<void**>(props.GetAddressOf()))==S_OK){
                const wchar_t* option=L"memx";PROPVARIANT value{};value.vt=VT_UI8;value.uhVal.QuadPart=uint64_t(128)*1024*1024;props->SetProperties(&option,&value,1);}
            Ptr<Input> stream;stream.Attach(new Input(raw,budget));Ptr<OpenCallback> callback;callback.Attach(new OpenCallback(budget));const UInt64 offset=0;
            const auto result=candidate->Open(stream.Get(),&offset,callback.Get());
            if(result==E_OUTOFMEMORY)return Fail(error,"Archive headers or dictionary exceed the safe decoder memory budget.");
            if(budget.password)return Fail(error,"Password-protected/encrypted archives are not supported. Extract them first.");
            if(budget.exceeded)return Fail(error,"Archive exceeds its entry, dictionary or unpacked resource limits.");
            if(result==S_OK){arc=std::move(candidate);input=std::move(stream);break;}candidate->Close();
        }
        if(!arc)return Fail(error,"Archive format is unsupported, corrupt, encrypted or requires missing volumes.");
        struct Close {IInArchive* value;~Close(){value->Close();}} close{arc.Get()};
        for(auto prop:{kpidErrorFlags,kpidWarningFlags,kpidIsVolume,kpidVolume,kpidOffset,kpidNumVolumes}){
            Variant v;uint64_t n=0;if(arc->GetArchiveProperty(prop,&v.value)!=S_OK||!v.Number(n)||n>(prop==kpidNumVolumes?1u:0u))return Fail(error,"Archive is damaged, split into volumes, or has unsupported metadata.");}
        {Variant v;uint64_t n=0;if(arc->GetArchiveProperty(kpidPhySize,&v.value)!=S_OK||!v.Number(n)||n>raw.size())return Fail(error,"Archive payload is truncated or its size is inconsistent.");}
        UInt32 count=0;if(arc->GetNumberOfItems(&count)!=S_OK||count>limits.entries)return Fail(error,"Archive exceeds the entry count limit.");
        entries.clear();decoded.clear();std::vector<bool> known;std::vector<UInt32> indices;std::map<std::string,coop_zip::detail::Node> names;uint64_t total=0;
        for(UInt32 i=0;i<count;++i){
            if(!budget.Okay())return Fail(error,"Archive inspection cancelled or timed out.");
            Entry e;e.index=entries.size();Variant path,dir,size,attrib,posix;uint64_t directory=0,bytes=0,attributes=0,mode=0;
            if(!Property(arc.Get(),i,kpidPath,path)||!path.Text(e.path)||!Property(arc.Get(),i,kpidIsDir,dir)||!dir.Number(directory)||directory>1||
                !Property(arc.Get(),i,kpidSize,size)||!size.Number(bytes)||!Property(arc.Get(),i,kpidAttrib,attrib)||!attrib.Number(attributes)||
                !Property(arc.Get(),i,kpidPosixAttrib,posix)||!posix.Number(mode))return Fail(error,"Archive entry metadata or filename is unsupported.");
            if(e.path.empty()&&StreamFormat(openedFormat))e.path=Stem(sourceName.empty()?"resource.bin":sourceName);
            // './' is a conventional TAR/CPIO root, not a filesystem escape.
            // Omit only the empty root directory; never relax '..' or an
            // absolute path. Keep compact public indices separate from SDK IDs.
            const auto kind=coop_zip::detail::Fold(openedFormat);
            if(kind=="tar"||kind=="cpio"){
                while(e.path.rfind("./",0)==0)e.path.erase(0,2);
                if(directory&&(e.path.empty()||e.path==".")){if(bytes)return Fail(error,"Archive root directory contains data.");continue;}
            }
            if(directory&&e.path.size()&&e.path.back()!='/')e.path+='/';std::string canonical;bool isDir=false;
            if(!coop_zip::detail::Path(e.path,canonical,isDir,error,limits))return false;
            const auto unixAttributes=(attributes>>16)&0170000;
            if(isDir!=(directory!=0)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)||
                (unixAttributes&&unixAttributes!=(directory?0040000u:0100000u))||((mode&0170000)&&
                (mode&0170000)!=(directory?0040000u:0100000u)))return Fail(error,"Archive contains a link, special file or inconsistent directory.");
            e.path=std::move(canonical);e.directory=isDir;e.bytes=bytes;
            for(auto prop:{kpidSymLink,kpidHardLink,kpidNtReparse,kpidIsAltStream,kpidIsAnti,kpidEncrypted,kpidSplitBefore,kpidSplitAfter}){
                Variant v;if(!Property(arc.Get(),i,prop,v))return Fail(error,"Archive link/encryption metadata could not be verified.");
                uint64_t value=0;if(v.value.vt==VT_BSTR){if(SysStringLen(v.value.bstrVal))return Fail(error,"Links and alternate streams are not allowed in mod archives.");}
                else if(!v.Number(value)||value)return Fail(error,"Encrypted, split, linked or special archive entries are not supported.");}
            if(bytes>limits.fileBytes||bytes>limits.unpackedBytes-total||(directory&&bytes))return Fail(error,"Archive exceeds a file or total unpacked size limit.");total+=bytes;
            if(!coop_zip::detail::Claim(e,names,error))return false;known.push_back(size.value.vt!=VT_EMPTY);indices.push_back(i);entries.push_back(std::move(e));
        }
        decoded.resize(entries.size());Ptr<ExtractCallback> callback;callback.Attach(new ExtractCallback(budget,entries,decoded,known,indices));
        const auto extraction=indices.empty()?S_OK:arc->Extract(indices.data(),UInt32(indices.size()),0,callback.Get());
        if(extraction==E_OUTOFMEMORY)return Fail(error,"Archive dictionary or decoder allocation exceeds the safe memory budget.");
        if(!indices.empty()&&(extraction!=S_OK||!callback->Complete()))return Fail(error,
            budget.password?"Password-protected archives are not supported.":budget.exceeded?"Archive exceeds the extraction resource limits.":!budget.Okay()?"Archive extraction cancelled or timed out.":
            callback->operationResult==NArchive::NExtract::NOperationResult::kUnsupportedMethod?"This archive uses an unsupported or third-party compression codec.":"Archive CRC/checksum, declared length or compressed data verification failed.");
        for(size_t i=0;i<entries.size();++i)entries[i].bytes=decoded[i].size();
        format=openedFormat;
        // Compression-only wrappers (tar.gz, tar.xz, etc.) expose their payload,
        // then recurse only when it is a container. Ordinary compressed assets
        // keep their own safe filename and are not executed or guessed as code.
        if(entries.size()==1&&StreamFormat(openedFormat)&&!entries[0].directory){
            const auto& data=decoded[0];const bool container=data.size()>4&&(
                data.compare(0,2,"PK")==0||data.compare(0,4,"7z\xBC\xAF",4)==0||data.compare(0,4,"Rar!")==0||
                TarHeader(data));
            if(container){auto payload=std::move(decoded[0]);const auto nextName=entries[0].path;entries.clear();decoded.clear();
                return Decode(std::move(payload),error,nextName,budget,depth+1);}
        }
        valid=true;return true;
    }
public:
    explicit Archive(Limits value=Limits{}):limits(value){
        limits.compressedBytes=(std::min)(limits.compressedBytes,coop_zip::kMaxCompressed);limits.unpackedBytes=(std::min)(limits.unpackedBytes,coop_zip::kMaxUnpacked);
        limits.fileBytes=(std::min)(limits.fileBytes,coop_zip::kMaxFile);limits.entries=(std::min)(limits.entries,coop_zip::kMaxEntries);limits.pathBytes=(std::min)(limits.pathBytes,size_t(1024));}
    bool Open(std::string bytes,std::string& error,const std::string& sourceName="",const std::atomic<bool>* cancel=nullptr){
        valid=false;zip.reset();entries.clear();decoded.clear();format.clear();error.clear();if(cancel&&cancel->load())return Fail(error,"Archive inspection cancelled.");
        try{
            if(!RequiresEngine(bytes)){
                zip=std::make_unique<coop_zip::Archive>(limits);if(!zip->Open(std::move(bytes),error)){zip.reset();return false;}
                valid=true;entries=zip->Entries();format="ZIP";return true;
            }
            Budget budget;budget.limits=limits;budget.cancel=cancel;
            if(!Decode(std::move(bytes),error,sourceName,budget,0)){entries.clear();decoded.clear();zip.reset();valid=false;return false;}return true;
        }catch(const std::bad_alloc&){return Fail(error,"Not enough memory to inspect this archive safely.");}
        catch(...){return Fail(error,"Archive inspection failed safely; no resources were installed.");}
    }
    const std::vector<Entry>& Entries()const{return entries;}
    const std::string& Format()const{return format;}
    bool Extract(size_t index,std::string& output,std::string& error)const{
        error.clear();if(!valid||index>=entries.size()){error="Archive entry is not open or does not exist.";return false;}
        if(zip)return zip->Extract(index,output,error);
        try{auto copy=decoded[index];output.swap(copy);return true;}catch(...){error="Not enough memory to read the archive entry safely.";return false;}
    }
};
} // namespace coop_archive
