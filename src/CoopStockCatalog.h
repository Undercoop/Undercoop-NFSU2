// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Pure planning only. No game calls, process-memory access, or file IO. The
// native adapter owns address/ABI/pointer-table validation and atomic commit.
namespace coop_stock_catalog {
constexpr std::uint32_t kCapacity=48, kFirstAddon=46, kMaxModels=256;
constexpr std::uint32_t kVtable=0x0079AADC, kNoModel=0xFFFFFFFFu;
constexpr std::size_t kNameBytes=16, kRamRecordBytes=0x1C;
constexpr std::size_t kSerializedRecordBytes=24, kPayloadCountOffset=0x480;
constexpr std::size_t kPayloadBytes=0x484, kMaxPayloadBytes=64u*1024u*1024u;
struct Row {
    std::uint32_t vtable=kVtable, unknown=0, hash=0, performance=0;
    std::uint32_t active=0, category=1, model=kNoModel;
};
static_assert(sizeof(Row)==kRamRecordBytes,"Native stock template layout");
struct Snapshot { std::array<Row,kCapacity> rows{}; std::uint32_t count=0; };
struct Model {
    std::uint32_t id=0;
    char name[kNameBytes]{};
    std::uint32_t usage=0;
    bool gameEligible=false, geometryAvailable=false;
};
struct Plan {
    Snapshot after{};
    std::uint32_t added=0;
    std::array<std::uint32_t,kCapacity> addedModels{};
};
enum class Error { None, Input, Count, Vtable, Prefix, DuplicateModel,
    DuplicateHash, ModelId, Name, Capacity, Slot, PayloadSize };

inline bool StockHash(const char* name,std::uint32_t& output) {
    if(!name)return false;
    const auto* end=static_cast<const char*>(std::memchr(name,0,kNameBytes));
    if(!end||end==name)return false;
    for(const char* at=name;at<end;++at) {
        const auto c=static_cast<unsigned char>(*at);
        if(c<33||c>126||c=='/'||c=='\\')return false;
    }
    std::uint32_t hash=0xFFFFFFFFu;
    for(const unsigned char c:{'S','T','O','C','K','_'})hash=hash*33u+c;
    for(const char* at=name;at<end;++at)hash=hash*33u+static_cast<unsigned char>(*at);
    output=hash;return true;
}
inline bool Unused(const Row& row) {
    return row.vtable==kVtable&&!row.unknown&&!row.hash&&!row.performance&&
        !row.active&&row.category<=1&&row.model==kNoModel;
}
inline bool Validate(const Snapshot& snapshot,Error& error) {
    error=Error::None;
    if(snapshot.count>kCapacity){error=Error::Count;return false;}
    for(std::uint32_t i=0;i<kCapacity;++i) {
        const auto& row=snapshot.rows[i];
        if(row.vtable!=kVtable){error=Error::Vtable;return false;}
        if(i>=snapshot.count) {
            if(row.active){error=Error::Prefix;return false;}
            continue;
        }
        if(row.active!=1||row.category!=1||row.model>=kMaxModels||!row.hash) {
            error=Error::Prefix;return false;
        }
        for(std::uint32_t j=0;j<i;++j) {
            if(snapshot.rows[j].model==row.model){error=Error::DuplicateModel;return false;}
            if(snapshot.rows[j].hash==row.hash){error=Error::DuplicateHash;return false;}
        }
    }
    return true;
}
inline bool Build(const Snapshot& before,const Model* models,std::size_t modelCount,
                  Plan& output,Error& error) {
    error=Error::None;
    if(modelCount>kMaxModels||(modelCount&&!models)){error=Error::Input;return false;}
    if(!Validate(before,error))return false;
    // Validate the whole catalogue even when an earlier model would overflow.
    // Existing stock hashes deliberately need not match today's model names.
    for(std::size_t i=0;i<modelCount;++i) {
        if(models[i].id>=kMaxModels){error=Error::ModelId;return false;}
        std::uint32_t hash=0;
        if(!StockHash(models[i].name,hash)){error=Error::Name;return false;}
        for(std::size_t j=0;j<i;++j)
            if(models[i].id==models[j].id){error=Error::DuplicateModel;return false;}
    }
    Plan staged;staged.after=before;
    for(std::size_t i=0;i<modelCount;++i) {
        const auto& model=models[i];
        if(model.id<kFirstAddon||model.usage!=0||!model.gameEligible||!model.geometryAvailable)continue;
        bool present=false;
        for(std::uint32_t j=0;j<staged.after.count;++j)
            if(staged.after.rows[j].model==model.id)present=true;
        if(present)continue;
        std::uint32_t hash=0;StockHash(model.name,hash);
        if(!hash){error=Error::DuplicateHash;return false;}
        for(std::uint32_t j=0;j<staged.after.count;++j)
            if(staged.after.rows[j].hash==hash){error=Error::DuplicateHash;return false;}
        if(staged.after.count==kCapacity){error=Error::Capacity;return false;}
        auto& row=staged.after.rows[staged.after.count];
        if(!Unused(row)){error=Error::Slot;return false;}
        row={kVtable,0,hash,0,1,1,model.id};
        ++staged.after.count;staged.addedModels[staged.added++]=model.id;
    }
    output=staged;return true;
}
inline std::uint32_t Read32(const std::uint8_t* bytes) {
    return std::uint32_t(bytes[0])|(std::uint32_t(bytes[1])<<8)|
        (std::uint32_t(bytes[2])<<16)|(std::uint32_t(bytes[3])<<24);
}
inline void Write32(std::uint8_t* bytes,std::uint32_t value) {
    for(unsigned i=0;i<4;++i){bytes[i]=static_cast<std::uint8_t>(value);value>>=8;}
}
// A native profile payload starts with 48 six-field stock records (no vtables)
// and a count. These helpers never accept paths or re-encode an on-disk CRC.
inline bool DecodePayload(const void* payload,std::size_t bytes,Snapshot& output,Error& error) {
    error=Error::None;
    if(!payload||bytes<kPayloadBytes||bytes>kMaxPayloadBytes){error=Error::PayloadSize;return false;}
    const auto* input=static_cast<const std::uint8_t*>(payload);
    Snapshot staged;staged.count=Read32(input+kPayloadCountOffset);
    for(std::uint32_t i=0;i<kCapacity;++i) {
        const auto* at=input+i*kSerializedRecordBytes;
        staged.rows[i]={kVtable,Read32(at),Read32(at+4),Read32(at+8),
            Read32(at+12),Read32(at+16),Read32(at+20)};
    }
    if(!Validate(staged,error))return false;
    output=staged;return true;
}
inline bool PreviewPayload(const void* payload,std::size_t bytes,const Model* models,
                           std::size_t modelCount,std::vector<std::uint8_t>& preview,
                           Plan& output,Error& error) {
    Snapshot before;Plan staged;
    if(!DecodePayload(payload,bytes,before,error)||!Build(before,models,modelCount,staged,error))return false;
    const auto* input=static_cast<const std::uint8_t*>(payload);
    std::vector<std::uint8_t> copy(input,input+bytes);
    // Write only appended records, never the already existing template fields.
    for(std::uint32_t i=before.count;i<staged.after.count;++i) {
        const auto& row=staged.after.rows[i];auto* at=copy.data()+i*kSerializedRecordBytes;
        Write32(at,row.unknown);Write32(at+4,row.hash);Write32(at+8,row.performance);
        Write32(at+12,row.active);Write32(at+16,row.category);Write32(at+20,row.model);
    }
    Write32(copy.data()+kPayloadCountOffset,staged.after.count);
    preview.swap(copy);output=staged;return true;
}
inline const char* ErrorText(Error error) {
    switch(error) {
    case Error::None:return "ready";
    case Error::Input:return "invalid catalogue input";
    case Error::Count:return "stock template count exceeds 48";
    case Error::Vtable:return "unrecognized stock template vtable";
    case Error::Prefix:return "malformed active stock template prefix";
    case Error::DuplicateModel:return "duplicate stock model identity";
    case Error::DuplicateHash:return "stock configuration hash collision";
    case Error::ModelId:return "catalogue model index exceeds 255";
    case Error::Name:return "invalid catalogue model name";
    case Error::Capacity:return "stock template capacity exhausted";
    case Error::Slot:return "target stock template slot is not unused";
    case Error::PayloadSize:return "invalid private preview payload size";
    }return "unknown stock template error";
}
}
