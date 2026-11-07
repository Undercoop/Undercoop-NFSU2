// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include "CoopSpecial.h"
namespace CoopSpecialNative {
struct Addresses{uintptr_t career=0x85AD40,specialTable=0x83840C,specialCount=0x838408,
    missionTable=0x838424,missionCount=0x838420,strings=0x838428,mode=0x850078,flow=0x8654A4;};
static Addresses addresses;
static bool installed=false;
// The eventual native receipt adapter adds a durable fence surviving Unbind
// and process restart. Transient state.Active() alone is not a paid receipt.
static bool (*receiptMayImport)()=nullptr;
static coop_special::State state;
static bool ReadSpan(uintptr_t at,void* out,size_t bytes){
    if(!IsValidUserPtr(at)||!bytes||bytes>=0x7FFE0000u-at)return false;
    for(uintptr_t cursor=at;cursor<at+bytes;){
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(cursor),&info,sizeof(info))||info.State!=MEM_COMMIT||
            (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
            !(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
        cursor=(std::min)(at+bytes,uintptr_t(info.BaseAddress)+uintptr_t(info.RegionSize));
    }
    return ReadGameMemory(at,out,bytes);
}
template<class T>static bool Read(uintptr_t at,T& value){return ReadSpan(at,&value,sizeof(value));}
static bool Text(uintptr_t at,char(&text)[96]){std::memset(text,0,sizeof(text));if(!IsValidUserPtr(at))return false;
    for(unsigned i=0;i<sizeof(text);++i){if(!Read(at+i,text[i]))return false;if(!text[i])return i!=0;if(uint8_t(text[i])<32||uint8_t(text[i])>126)return false;}return false;}
static uint32_t NativeHash(const char* text){uint32_t h=~uint32_t(0);for(;*text;++text)h=h*33+uint8_t(*text);return h;}
static uint32_t CampaignHash(const std::string& text){uint32_t h=2166136261u;for(uint8_t c:text){if(c>='a'&&c<='z')c-=32;h=(h^c)*16777619u;}return h?h:1;}
static void Mix(uint32_t& h,uint32_t v){for(unsigned i=0;i<4;++i){h=(h^uint8_t(v))*16777619u;v>>=8;}}
struct Record{coop_special::Spec spec{};uintptr_t pointer=0;uint8_t nativeType=0,mandatory=0;float stars=0;};
struct Catalog{std::array<Record,64> covers{},missions{};unsigned coverCount=0,missionCount=0;uint32_t digest=0;};
static bool ReadCatalog(Catalog& out){
    out={};uintptr_t covers=0,missions=0,strings=0;uint32_t nc=0,nm=0;
    if(!Read(addresses.specialTable,covers)||!Read(addresses.specialCount,nc)||!nc||nc>64||!IsValidUserPtr(covers)||
        !Read(addresses.missionTable,missions)||!Read(addresses.missionCount,nm)||nm>64||(nm&&!IsValidUserPtr(missions))||
        !Read(addresses.strings,strings)||!IsValidUserPtr(strings))return false;
    uint32_t digest=2166136261u;Mix(digest,nc);Mix(digest,nm);
    for(unsigned i=0;i<nc;++i){BYTE row[0x40]{};if(!ReadSpan(covers+i*0x40,row,sizeof(row)))return false;
        Record r{};r.pointer=covers+i*0x40;r.nativeType=row[0x24];r.mandatory=row[0x35];r.spec.stage=row[0x25];
        std::memcpy(&r.spec.id,row+0x20,4);std::memcpy(&r.spec.start,row+0x2C,4);std::memcpy(&r.stars,row+0x3C,4);
        if(r.nativeType<1||r.nativeType>4||r.mandatory>1||!std::isfinite(r.stars)||r.stars<0||r.stars>10)return false;
        r.spec.destination=r.spec.id;r.spec.kind=r.nativeType<=2?coop_special::Kind::Photo:coop_special::Kind::Frontend;
        if(!coop_special::Valid(r.spec))return false;
        for(unsigned j=0;j<i;++j)if(out.covers[j].spec.id==r.spec.id)return false;
        out.covers[out.coverCount++]=r;uint32_t starBits=0;std::memcpy(&starBits,&r.stars,4);
        Mix(digest,r.spec.id);Mix(digest,r.spec.start);Mix(digest,r.spec.stage);Mix(digest,r.nativeType);Mix(digest,r.mandatory);Mix(digest,starBits);
    }
    for(unsigned i=0;i<nm;++i){BYTE row[0x18]{};if(!ReadSpan(missions+i*0x18,row,sizeof(row)))return false;
        uint16_t name=0,start=0;int16_t stage=0;int32_t seconds=0;Record r{};r.pointer=missions+i*0x18;
        std::memcpy(&name,row,2);std::memcpy(&start,row+2,2);std::memcpy(&stage,row+4,2);
        std::memcpy(&r.spec.destination,row+0xC,4);std::memcpy(&seconds,row+0x10,4);char text[96];
        if(stage<0||stage>=8||seconds<1||seconds>3600||!Text(strings+name,text))return false;
        r.spec.id=NativeHash(text);if(!Text(strings+start,text))return false;r.spec.start=NativeHash(text);
        r.spec.durationMs=uint32_t(seconds)*1000;r.spec.stage=uint8_t(stage);
        r.spec.kind=row[0x14]==1?coop_special::Kind::Visual:row[0x14]==2?coop_special::Kind::Performance:coop_special::Kind::Show;
        if((row[0x14]!=1&&row[0x14]!=2&&row[0x14]!=4)||!coop_special::Valid(r.spec))return false;
        for(unsigned j=0;j<i;++j)if(out.missions[j].spec.id==r.spec.id)return false;
        for(unsigned j=0;j<nc;++j)if(out.covers[j].spec.id==r.spec.id)return false;
        out.missions[out.missionCount++]=r;
        Mix(digest,r.spec.id);Mix(digest,r.spec.start);Mix(digest,r.spec.destination);Mix(digest,r.spec.stage);Mix(digest,r.spec.durationMs);Mix(digest,row[0x14]);Mix(digest,row[6]);Mix(digest,row[7]);
    }
    out.digest=digest?digest:1;return true;
}
static const Record* Find(const Catalog& catalog,uint32_t hash){for(unsigned i=0;i<catalog.coverCount;++i)if(catalog.covers[i].spec.id==hash)return &catalog.covers[i];
    for(unsigned i=0;i<catalog.missionCount;++i)if(catalog.missions[i].spec.id==hash)return &catalog.missions[i];return nullptr;}
// GLOBALB catalogs are immutable after load. Revalidate their identity before
// every use and rebuild on unload/reload; do not parse all names once per earned
// hash on every save/manifest. Failed/partial loads are never cached as ready.
struct CatalogIdentity {
    uintptr_t covers=0,missions=0,strings=0;uint32_t coverCount=0,missionCount=0;
    bool operator==(const CatalogIdentity& b)const{return covers==b.covers&&missions==b.missions&&strings==b.strings&&coverCount==b.coverCount&&missionCount==b.missionCount;}
};
static Catalog cachedCatalog;static CatalogIdentity cachedIdentity;static bool haveCatalog=false;
static const Catalog* FactsCatalog(){
    CatalogIdentity key;
    if(!Read(addresses.specialTable,key.covers)||!Read(addresses.specialCount,key.coverCount)||
        !Read(addresses.missionTable,key.missions)||!Read(addresses.missionCount,key.missionCount)||
        !Read(addresses.strings,key.strings)||!key.coverCount||key.coverCount>64||key.missionCount>64||
        !IsValidUserPtr(key.covers)||!IsValidUserPtr(key.strings)||(key.missionCount&&!IsValidUserPtr(key.missions))){haveCatalog=false;return nullptr;}
    if(!haveCatalog||!(key==cachedIdentity)){
        haveCatalog=false;Catalog candidate;if(!ReadCatalog(candidate))return nullptr;
        cachedCatalog=candidate;cachedIdentity=key;haveCatalog=true;
    }
    return &cachedCatalog;
}
static bool OwnCareer(){return gProfileContext.IsCoopActive()&&!gRestoreSpPending;}
} // namespace CoopSpecialNative
bool CoopSpecialHooksReady(){return CoopSpecialNative::installed;}
bool CoopSpecialFactsReady(){return CoopSpecialNative::FactsCatalog()!=nullptr;}
bool CoopSpecialClassify(uint32_t hash,uint32_t& kind,uint32_t& stage){
    using namespace CoopSpecialNative;kind=stage=0;if(!hash)return false;
    const auto* catalog=FactsCatalog();if(!catalog)return false;const auto* row=Find(*catalog,hash);if(!row)return false;
    kind=coop_special::Timed(row->spec.kind)?2:1;stage=row->spec.stage;return true;
}
bool CoopSpecialMayImport(){using namespace CoopSpecialNative;
    if(!installed||!OwnCareer())return true;
    if(!receiptMayImport||!receiptMayImport())return false;
    return !state.Active()||state.LocalSaved();
}
bool CoopSpecialFilterUnpaired(coop_career::Progress& progress,const coop_career::Progress& history){
    using namespace CoopSpecialNative;
    const auto* catalog=FactsCatalog();if(!catalog||!coop_career::Valid(progress)||!coop_career::Valid(history))return false;
    auto next=progress;
    // Only initial history is authoritative until the native shared mission
    // lifecycle is installed. Neither a local photo nor a peer packet can
    // manufacture a reward receipt for the other player's car/part selection.
    next.completedEvents.erase(std::remove_if(next.completedEvents.begin(),next.completedEvents.end(),[&](uint32_t hash){
        return Find(*catalog,hash)&&!coop_career::Contains(history.completedEvents,hash);
    }),next.completedEvents.end());
    next.specialsKnown=history.specialsKnown;
    for(size_t s=0;s<coop_career::kStageCount;++s){
        next.stages[s].specials=history.stages[s].specials;
        next.stages[s].outrunWins=history.stages[s].outrunWins;
        bool proven=true;
        for(unsigned i=0;i<catalog->coverCount;++i){const auto& row=catalog->covers[i];
            if(row.spec.stage==s&&row.mandatory&&(row.nativeType==2||row.nativeType==4)&&
                !coop_career::Contains(history.completedEvents,row.spec.id)){proven=false;break;}
        }
        // Preserve all old completed chapters. Only new DVD-dependent chapter
        // facts are withheld; new normal race hashes/discoveries still travel.
        if(!proven&&!history.stages[s].completed){
            next.stages[s].completed=false;
            if(next.stageIndex>s)next.stageIndex=uint32_t(s);
        }
    }
    if(next.offersKnown&&(next.offerStage!=next.stageIndex||next.offerBasis!=coop_career::OfferBasis(next)))coop_career::ClearOffers(next);
    if(!coop_career::Valid(next))return false;progress=std::move(next);return true;
}
