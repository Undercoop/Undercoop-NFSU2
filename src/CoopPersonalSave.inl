// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Included after the native profile/file adapters. All engine/FS operations run
// on the game thread; only the bounded checkpoint transport uses the UDP lock.
namespace CoopPersonalSave {
static std::string campaign,personal;
static coop_career::Progress common;
static coop_career::Progress historicalSpecials;
static bool haveHistoricalSpecials=false;
static bool haveCommon=false,dirty=false;
static bool personalStarter=false;
static bool mapRefreshPending=false;
using MapCareerFn=void(__thiscall*)(void*);
using MapIconsFn=void(__cdecl*)();
static MapCareerFn refreshCareerMap=reinterpret_cast<MapCareerFn>(0x533310);
static MapIconsFn refreshMapIcons=reinterpret_cast<MapIconsFn>(0x4F92A0);
static uint32_t lastQueuedDigest=0;
static DWORD lastAttempt=0;
static DWORD lastObservedAt=0;
static uint32_t lastObservedDigest=0;
static uint32_t lastHostOfferRevision=0; // Session-local, not another peer's prior campaign authority.
static uintptr_t eventTableAddress=0x8383E0,eventCountAddress=0x8383E4;
static uintptr_t shopTableAddress=0x8383F0,shopCountAddress=0x8383F4;
static uintptr_t career=0x85AD40;
using LookupFn=void*(__cdecl*)(uint32_t);
static LookupFn lookup=reinterpret_cast<LookupFn>(0x4FFC90);
static LookupFn lookupShop=reinterpret_cast<LookupFn>(0x4FFE20);
using VisibilityFn=bool(__thiscall*)(void*,void*);
static VisibilityFn raceVisible=reinterpret_cast<VisibilityFn>(0x532C20),shopVisible=reinterpret_cast<VisibilityFn>(0x513110);
// Default to the runtime's verified hooks/catalog; focused private-arena tests
// replace these ports without calling stock code or opening a real profile.
// Catalog facts are safe independently of the not-yet-installed shared mission
// lifecycle. Imported native profiles can contain already earned covers/drives.
static bool (*specialHooksReady)()=CoopSpecialFactsReady;
static bool (*specialLifecycleReady)()=CoopSpecialHooksReady;
static bool (*specialFilterUnpaired)(coop_career::Progress&,const coop_career::Progress&)=CoopSpecialFilterUnpaired;
static bool (*specialClassify)(uint32_t,uint32_t&,uint32_t&)=CoopSpecialClassify;
static bool (*specialMayImport)()=CoopSpecialMayImport;
using OffersBuildFn=void(__thiscall*)(void*);
static OffersBuildFn buildOffers=reinterpret_cast<OffersBuildFn>(0x527D30);
struct OffersBuildWitness {
    uint64_t session=0;uint32_t ticket=0,stage=0;
    std::vector<uint32_t> hashes;
    bool ready=false;
};
static OffersBuildWitness offersBuilt;
static bool DiscardEmptyOffers(coop_career::Progress& progress) {
    // 6.11.26 could mistake a zeroed, not-yet-built menu cache for the
    // authoritative selection. An empty manifest has no positive native
    // selection proof, so keep genuine empty stock stages local as well.
    if(!progress.offersKnown || !progress.offeredEvents.empty())return false;
    coop_career::ClearOffers(progress);return true;
}
static bool OffersScope() {
    const auto state=CoopSnapshot();
    if(!state.joined || !state.fresh || !IsCoopGameplayActive() || !gProfileContext.IsCoopActive() || gRestoreSpPending)return false;
    uint32_t count=0;if(!ReadGameMemory(career+0x7AFC,&count,4) || count>coop_career::kMaximumEvents)return false;
    for(uint32_t i=0;i<count;++i) {uint8_t row[8]{};
        if(!ReadGameMemory(career+0x7B00+i*8,row,8))return false;
        if(coop_career::Read32(row)==coop_career::kPersonalEventB)return row[6]==1;
    }
    return false; // Preserve each player's starter/tutorial gates, not Host's.
}
static bool OfferRecord(uint32_t hash,uint8_t stage,uintptr_t& ptr,uint8_t& bucket) {
    ptr=0;uint32_t actual=0;uint8_t category=0,type=0,flag=0,tier=0;
    __try {ptr=reinterpret_cast<uintptr_t>(lookup(hash));} __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
    if(!hash || coop_career::PersonalEvent(hash) || !IsValidUserPtr(ptr) ||
        !ReadGameMemory(ptr+8,&actual,4) || actual!=hash || !ReadGameMemory(ptr+0x37,&tier,1) || tier!=stage ||
        !ReadGameMemory(ptr+0x34,&category,1) || category<1 || category>3 ||
        !ReadGameMemory(ptr+0x0F,&type,1) || !ReadGameMemory(ptr+0x0D,&flag,1))return false;
    // Native 5002E0 / 527FC9: retain exactly its quota counters.
    static const uint8_t buckets[6]={0,4,2,0,1,3};
    bucket=flag==1?7:category==2?5:type<=5?buckets[type]:0;return true;
}
static bool OffersUsable(const coop_career::Progress& progress,uint32_t stage) {
    if(!progress.offersKnown || progress.offeredEvents.empty() || progress.offerStage!=stage ||
        progress.offerBasis!=coop_career::OfferBasis(progress))return false;
    for(uint32_t hash:progress.offeredEvents) {
        uintptr_t ptr=0;uint8_t bucket=0;
        if(coop_career::Contains(progress.completedEvents,hash) || !OfferRecord(hash,uint8_t(stage),ptr,bucket))return false;
        uint32_t count=0;if(!ReadGameMemory(career+0x7AFC,&count,4) || count>coop_career::kMaximumEvents)return false;
        for(uint32_t i=0;i<count;++i) {uint8_t row[8]{};if(!ReadGameMemory(career+0x7B00+i*8,row,8))return false;
            if(coop_career::Read32(row)==hash && row[6]==1)return false;}
    }
    return true;
}
static bool ApplyOffers(const coop_career::Progress& progress) {
    uint32_t stage=0,mission=0,pointers[64]{},before[64]{};uint8_t counts[16]{},oldCounts[16]{};
    if(!ReadGameMemory(career+0xC08,&mission,4) || mission ||
        !ReadGameMemory(career+0x7AF8,&stage,4) || !OffersUsable(progress,stage))return false;
    size_t slot=0;
    for(uint32_t hash:progress.offeredEvents) {
        uintptr_t ptr=0;uint8_t bucket=0;if(!OfferRecord(hash,uint8_t(stage),ptr,bucket))return false;
        pointers[slot++]=uint32_t(ptr);++counts[bucket];
    }
    if(!ReadGameMemory(career+0x71C8,before,sizeof(before)) || !ReadGameMemory(career+0x72C8,oldCounts,sizeof(oldCounts)))return false;
    // Table and counters cross a native page boundary. Probe BOTH before the
    // first write; otherwise a guarded second page could leave a half-roster.
    for(uintptr_t at=career+0x71C8,limit=career+0x72D8;at<limit;) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
            !(info.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
        at=(std::min)(limit,uintptr_t(info.BaseAddress)+uintptr_t(info.RegionSize));
    }
    uint8_t cache[0x110]{},oldCache[0x110]{};
    std::memcpy(cache,pointers,sizeof(pointers));std::memcpy(cache+0x100,counts,sizeof(counts));
    std::memcpy(oldCache,before,sizeof(before));std::memcpy(oldCache+0x100,oldCounts,sizeof(oldCounts));
    if(!SafeWriteBytes(career+0x71C8,cache,sizeof(cache))) {SafeWriteBytes(career+0x71C8,oldCache,sizeof(oldCache));return false;}
    // No catalog+7F, found flags, ratings, sponsor choices or native profile
    // pointers outside this derived offered-race cache are modified.
    return true;
}
static bool ReadOfferCache(uint32_t& stage,std::vector<uint32_t>& hashes) {
    uint32_t mission=0,pointers[64]{};
    if(!ReadGameMemory(career+0x7AF8,&stage,4) || stage>=coop_career::kStageCount ||
        !ReadGameMemory(career+0xC08,&mission,4) || mission ||
        !ReadGameMemory(career+0x71C8,pointers,sizeof(pointers)))return false;
    hashes.clear();
    for(uint32_t address:pointers) {
        if(!address)continue;uint32_t hash=0;uintptr_t actual=0;uint8_t bucket=0;
        if(!ReadGameMemory(address+8,&hash,4) || !OfferRecord(hash,uint8_t(stage),actual,bucket) || actual!=address ||
            coop_career::Contains(hashes,hash))return false;
        hashes.push_back(hash);
    }
    return true;
}
static bool CaptureOffers(coop_career::Progress& progress) {
    const auto state=CoopSnapshot();
    if(!OffersScope() || state.role!=coop::Role::Host || !offersBuilt.ready ||
        offersBuilt.session!=state.sessionId || offersBuilt.ticket!=state.ticket)return false;
    uint32_t stage=0;std::vector<uint32_t> hashes;
    if(!ReadOfferCache(stage,hashes) || stage!=progress.stageIndex || stage!=offersBuilt.stage ||
        hashes.empty() || hashes!=offersBuilt.hashes)return false;
    for(uint32_t hash:hashes)if(coop_career::Contains(progress.completedEvents,hash))return false;
    const uint32_t basis=coop_career::OfferBasis(progress);
    if(progress.offersKnown && progress.offerStage==stage && progress.offerBasis==basis && progress.offeredEvents==hashes)return false;
    progress.offersKnown=true;progress.offerStage=uint8_t(stage);progress.offerBasis=basis;
    if(!++progress.offerRevision)++progress.offerRevision;progress.offeredEvents=std::move(hashes);return true;
}
static void __fastcall HookOffersBuild(void* manager,void*) {
    const bool ownManager=reinterpret_cast<uintptr_t>(manager)==career;
    if(ownManager)offersBuilt={};
    buildOffers(manager);
    if(!ownManager || !OffersScope())return;
    uint32_t stage=0,mission=0;
    if(!ReadGameMemory(career+0xC08,&mission,4) || mission || !ReadGameMemory(career+0x7AF8,&stage,4))return;
    if(haveCommon && DiscardEmptyOffers(common))dirty=true;
    const bool restored=haveCommon && OffersUsable(common,stage) && ApplyOffers(common);
    std::vector<uint32_t> hashes;
    if(!ReadOfferCache(stage,hashes))return;
    const auto state=CoopSnapshot();
    offersBuilt.session=state.sessionId;offersBuilt.ticket=state.ticket;offersBuilt.stage=stage;
    offersBuilt.hashes=std::move(hashes);offersBuilt.ready=true;
    char line[192]{};std::snprintf(line,sizeof(line),"[CAREER_OFFERS] Verified stock build role=%s stage=%u count=%zu restored=%u; empty stock lists remain non-authoritative.\r\n",
        state.role==coop::Role::Host?"Host":"Client",stage,offersBuilt.hashes.size(),restored?1u:0u);LogMessage(line);
    // A changed stage/completion/discovery basis intentionally permits stock
    // Host selection once; Client random selection is never published.
    if(CoopSnapshot().role==coop::Role::Host)dirty=true;
}
static bool NativeVisible(VisibilityFn fn,uintptr_t item) {
    __try {return fn(reinterpret_cast<void*>(career),reinterpret_cast<void*>(item));}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
// 512F70 also marks temporary mission rows; 5139C0 clears those again.
// Only actual race (native category3) and shop discoveries are monotonic.
static bool DiscoveryEligible(uint32_t hash) {
    if(!hash || coop_career::PersonalEvent(hash))return false;
    uintptr_t ptr=0;uint32_t actual=0;uint8_t type=0;
    __try {ptr=reinterpret_cast<uintptr_t>(lookup(hash));} __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
    if(IsValidUserPtr(ptr) && ReadGameMemory(ptr+8,&actual,4) && actual==hash &&
        ReadGameMemory(ptr+0x34,&type,1) && type==3)return true;
    __try {ptr=reinterpret_cast<uintptr_t>(lookupShop(hash));} __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
    return IsValidUserPtr(ptr) && ReadGameMemory(ptr+0x38,&actual,4) && actual==hash;
}
static bool ReadNamed(const std::string& name,const wchar_t* suffix,std::vector<uint8_t>& bytes) {
    if(!coop_save::IsSafeProfileName(WideText(name.c_str())) || !suffix || !gProfileContext.IsCoopActive()) return false;
    const auto directory=gProfileContext.ActiveRoot()+L"\\"+WideText(name.c_str());
    // A read may not traverse a junction to another profile/domain.
    if(!SafeCoopDirectory(directory)) return false;
    const auto path=directory+L"\\"+WideText(name.c_str())+suffix;
    const DWORD attrs=GetFileAttributesW(path.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    if(!*suffix) return ReadCoopFile(path,bytes);
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    const DWORD length=GetFileSize(file,nullptr);DWORD got=0;
    bool okay=length>=coop_career::kMinimumWireSize && length<=coop_career::kMaximumExtendedWireSize;
    if(okay) {bytes.resize(length);okay=ReadFile(file,bytes.data(),length,&got,nullptr)!=FALSE && got==length;}
    CloseHandle(file);coop_career::Progress p;
    return okay && coop_career::Decode(bytes,p);
}
static bool WriteNamed(const std::string& name,const wchar_t* suffix,const std::vector<uint8_t>& bytes) {
    if(!suffix) return false;
    if(!*suffix) return CommitCoopFile(name.c_str(),bytes);
    if(std::wcscmp(suffix,L".coop-progress")) return false;
    coop_career::Progress p;
    return coop_career::Decode(bytes,p) && CommitCoopBytes(name.c_str(),bytes,suffix);
}
static bool (*read)(const std::string&,const wchar_t*,std::vector<uint8_t>&)=ReadNamed;
static bool (*write)(const std::string&,const wchar_t*,const std::vector<uint8_t>&)=WriteNamed;
static bool (*fresh)()=PrepareFreshCoopCareer;
// A failed read is NOT proof that a save is absent. Inaccessible/corrupt files
// must never trigger a fresh profile that replaces their recoverable contents.
static int ProbeNamed(const std::string& name,const wchar_t* suffix) {
    if(!coop_save::IsSafeProfileName(WideText(name.c_str())) || !suffix || !gProfileContext.IsCoopActive()) return -1;
    const auto directory=gProfileContext.ActiveRoot()+L"\\"+WideText(name.c_str());
    DWORD attrs=GetFileAttributesW(directory.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES) {
        const DWORD error=GetLastError();return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND?0:-1;
    }
    if(!(attrs&FILE_ATTRIBUTE_DIRECTORY) || (attrs&FILE_ATTRIBUTE_REPARSE_POINT)) return -1;
    const auto path=directory+L"\\"+WideText(name.c_str())+suffix;
    attrs=GetFileAttributesW(path.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES) {
        const DWORD error=GetLastError();return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND?0:-1;
    }
    return (attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))?-1:1;
}
static int (*probe)(const std::string&,const wchar_t*)=ProbeNamed;
static bool Name(const std::string& name) {return coop_save::IsSafeProfileName(WideText(name.c_str()));}
static bool SameName(const std::string& a,const std::string& b) {
    return coop_save::EqualPathText(WideText(a.c_str()),WideText(b.c_str()));
}
static std::string ClientName(const std::string& name) {
    if(!Name(name)) return {};
    if(name.size()<=28) return name+"_P2";
    uint32_t hash=2166136261u;
    for(unsigned char ch:name) {if(ch>='a' && ch<='z') ch-=32;hash^=ch;hash*=16777619u;}
    char suffix[16]{};std::snprintf(suffix,sizeof(suffix),"_%08X_P2",hash);
    return name.substr(0,19)+suffix;
}
static void ResetContext() {
    SetCoopBackupIdentity({},{});
    campaign.clear();personal.clear();common={};haveCommon=dirty=personalStarter=mapRefreshPending=false;lastQueuedDigest=0;lastAttempt=0;
    lastObservedAt=lastObservedDigest=lastHostOfferRevision=0;
    offersBuilt={};
    historicalSpecials={};haveHistoricalSpecials=false;
}
static bool FilterLiveSpecials(coop_career::Progress& progress){
    if(!specialHooksReady()||specialLifecycleReady())return true;
    return haveHistoricalSpecials&&specialFilterUnpaired(progress,historicalSpecials);
}
static void BindNames(const std::string& team,const std::string& own) {
    if(!Name(team) || !Name(own)) return;
    if(!SameName(campaign,team) || !SameName(personal,own)) ResetContext();
    campaign=team;personal=own;
    SetCoopBackupIdentity(team,own);
}
static bool ValidateNative(const coop_career::Progress& p) {
    if(!coop_career::Valid(p)) return false;
    auto event=[&](uint32_t hash,int stage,int group) {
        uintptr_t ptr=0;uint32_t actual=0;uint8_t type=0,flag=0;int8_t tier=0;
        __try {ptr=reinterpret_cast<uintptr_t>(lookup(hash));} __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
        if(!IsValidUserPtr(ptr)) {
            // Covers and timed missions have separate native catalogs. Never
            // reinterpret a real race's category/stage as a special record.
            uint32_t kind=0,specialStage=0;
            if(!specialClassify(hash,kind,specialStage) || (kind!=1 && kind!=2) ||
                specialStage>=coop_career::kStageCount)return false;
            return stage<0 || (group==3 && kind==1 && specialStage==uint32_t(stage));
        }
        if(!ReadGameMemory(ptr+8,&actual,4) || actual!=hash || group==3) return false;
        if(stage<0) return true;
        if(!ReadGameMemory(ptr+0x37,&tier,1) || tier!=stage ||
            !ReadGameMemory(ptr+0x34,&type,1) || !ReadGameMemory(ptr+0x0D,&flag,1)) return false;
        return group==0?(type==3 && flag!=1):group==1?type==1:type==2;
    };
    for(uint32_t hash:p.completedEvents) if(!event(hash,-1,-1)) return false;
    for(uint32_t hash:p.discoveredEvents) if(!DiscoveryEligible(hash)) return false;
    for(uint32_t hash:p.offeredEvents) {uintptr_t ptr=0;uint8_t bucket=0;
        if(!OfferRecord(hash,p.offerStage,ptr,bucket))return false;}
    for(size_t stage=0;stage<coop_career::kStageCount;++stage) {
        for(size_t i=0;i<coop_career::kStageHashCount;++i) {
            const uint32_t hash=p.stages[stage].events[i];
            if(hash && !event(hash,int(stage),i<35?0:i<39?1:2)) return false;
        }
        for(uint32_t hash:p.stages[stage].specials)
            if(hash && !event(hash,int(stage),3))return false;
    }
    return !p.lastResult.eventHash || event(p.lastResult.eventHash,-1,-1);
}
static bool BuildSharedBlob(const std::vector<uint8_t>& payload,std::vector<uint8_t>& blob) {
    coop_career::Progress local,next;
    if(!coop_career::Extract(payload,local,DiscoveryEligible,specialHooksReady()) || !ValidateNative(local)) return false;
    if(haveCommon) {if(!Combine(common,local,next)) return false;} else next=local;
    DiscardEmptyOffers(next);
    // Resume may serialize BEFORE native 527D30 has built any offers. Never
    // call that selector early (its stock rules/catalog may not be ready), or
    // turn the menu's zeroed cache into an authoritative empty roster. A
    // durable nonempty journal can travel now; a new selection is published
    // after the real native hook supplies a build witness.
    if(OffersScope() && CoopSnapshot().role==coop::Role::Host) {
        if(CaptureOffers(next)) {
            if(!coop_career::Encode(next,blob) || !Name(personal) || !write(personal,L".coop-progress",blob))return false;
            common=next;haveCommon=true;
            LogMessage("[CAREER_OFFERS] Canonical Host offered IDs committed in initial Resume checkpoint before either load barrier releases.\r\n");
        }
    }
    if(!coop_career::Encode(next,blob))return false;
    // BuildSharedBlob is the pre-load Host manifest boundary. Freeze its
    // existing history even when the native offer selector has not run yet.
    if(!CoopSnapshot().initialBarrierComplete){
        historicalSpecials=next;haveHistoricalSpecials=true;
        common=next;haveCommon=true;
    }
    return true;
}
static void LoadCommonCache() {
    if(!Name(personal)) return;
    std::vector<uint8_t> blob;coop_career::Progress stored,next;
    if(!read(personal,L".coop-progress",blob) || !coop_career::Decode(blob,stored) || !ValidateNative(stored)) return;
    if(DiscardEmptyOffers(stored))dirty=true;
    if(haveCommon) {if(!Combine(common,stored,next)) return;} else next=stored;
    common=next;haveCommon=true;
}
struct Patch {uintptr_t address=0;std::vector<uint8_t> before,after;};
static bool ApplyLive(const std::vector<uint8_t>& before,const std::vector<uint8_t>& after,std::vector<Patch>& undo) {
    if(before.size()!=coop_career::kNativePayloadSize || after.size()!=before.size() || !IsValidUserPtr(career)) return false;
    auto add=[&](size_t serialized,size_t native,size_t size) {
        if(!std::memcmp(before.data()+serialized,after.data()+serialized,size)) return true;
        Patch p;p.address=career+native;p.before.assign(before.begin()+serialized,before.begin()+serialized+size);
        p.after.assign(after.begin()+serialized,after.begin()+serialized+size);std::vector<uint8_t> live(size);
        if(!ReadGameMemory(p.address,live.data(),size) || live!=p.before) return false;
        // Probe write access before modifying ANY field. Protection is never
        // changed and no pointers/garage/cash allocations enter this patch set.
        MEMORY_BASIC_INFORMATION info{};uintptr_t at=p.address,limit=at+size;
        while(at<limit) {
            if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
                (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
                !(info.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return false;
            at=(std::min)(limit,uintptr_t(info.BaseAddress)+uintptr_t(info.RegionSize));
        }
        undo.push_back(std::move(p));return true;
    };
    for(size_t i=0;i<coop_career::kStageCount;++i) {
        const size_t saved=coop_career::kCareerOffset+i*coop_career::kStageStride,live=0x72D8+i*0x104;
        if(!add(saved,live,coop_career::kStageHashBytes) ||
            // 541090 records six completed covers at C4..DB; DC is the
            // monotonic outrun count. Stop before personal E0/E4 ratings,
            // E8 sponsor selection and every personal part/reward byte.
            !add(saved+coop_career::kSpecialOffset,live+coop_career::kSpecialOffset,
                coop_career::kSpecialSlots*4+4) ||
            !add(saved+coop_career::kStageCompletedOffset,live+0xFC,1)) return false;
    }
    if(!add(coop_career::kStageIndexOffset,0x7AF8,4) || !add(coop_career::kEventCountOffset,0x7AFC,4) ||
        !add(coop_career::kEventsOffset,0x7B00,coop_career::kMaximumEvents*coop_career::kEventStride)) return false;
    size_t written=0;
    for(const auto& p:undo) {
        if(!SafeWriteBytes(p.address,p.after.data(),p.after.size())) {
            while(written) {const auto& old=undo[--written];SafeWriteBytes(old.address,old.before.data(),old.before.size());}
            return false;
        }
        ++written;
    }
    return true;
}
static void Undo(const std::vector<Patch>& patches) {
    for(auto i=patches.rbegin();i!=patches.rend();++i) SafeWriteBytes(i->address,i->before.data(),i->before.size());
}
static bool SavePayload(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> file;
    return specialMayImport()&&Name(personal) && coop_save::EncodeNativeProfileFile(payload,file) && write(personal,L"",file);
}
static bool FitsRestoreProgress(const coop_career::Progress& own,const coop_career::Progress& host) {
    const auto subset=[](const auto& a,const auto& b) {
        return std::all_of(a.begin(),a.end(),[&](uint32_t hash) {
            return !hash || std::find(b.begin(),b.end(),hash)!=b.end();
        });
    };
    if(own.stageIndex>host.stageIndex || !subset(own.completedEvents,host.completedEvents) ||
        !subset(own.discoveredEvents,host.discoveredEvents))return false;
    for(size_t index=0;index<own.stages.size();++index) {
        const auto& old=own.stages[index];
        const auto& target=host.stages[index];
        if((old.completed && !target.completed) || old.outrunWins>target.outrunWins ||
            !subset(old.events,target.events) || !subset(old.specials,target.specials))return false;
    }
    return true;
}
static bool RestoreClientCareer(const char* campaignName,const std::string& own,
    const coop_backup::RestorePoint& point,const coop_career::Progress& host) {
    const auto campaignWide=WideText(campaignName),profile=WideText(own.c_str());
    coop_backup::RestorePoint existing;
    if(coop_backup::PendingRestorePoint(gCoopBaseRoot,coop_backup::Role::Client,profile,existing) &&
        coop_backup::SamePoint(existing,point))return true;
    // The Host sends only the original save point. The personal archive is
    // selected on this computer, from the same completed CO-OP ticket.
    for(const auto& record:coop_backup::List(gCoopBaseRoot,coop_backup::Role::Client)) {
        const bool later=record.utc>point.utc;
        const bool nearRaceResult=later && point.action==coop_backup::Action::RaceResult && record.action==point.action &&
            record.utc-point.utc<=10u*10000000u && host.lastResult.eventHash && host.lastResult.epoch;
        if((later && !nearRaceResult) || record.session!=point.session || record.ticket!=point.ticket ||
            !coop_save::EqualPathText(record.campaign,campaignWide) || !coop_save::EqualPathText(record.profile,profile))continue;
        coop_backup::Snapshot snapshot;
        if(!coop_backup::Read(record.path,snapshot) || snapshot.record.utc!=record.utc || snapshot.record.action!=record.action ||
            snapshot.record.session!=point.session || snapshot.record.ticket!=point.ticket ||
            snapshot.record.role!=coop_backup::Role::Client || !coop_save::EqualPathText(snapshot.record.profile,profile) ||
            !coop_save::EqualPathText(snapshot.record.campaign,campaignWide))continue;
        const auto& file=snapshot.files.front().bytes;
        const std::vector<uint8_t> payload(file.begin()+0x24,file.end());
        coop_career::Progress native;
        if(!coop_career::Extract(payload,native,DiscoveryEligible,specialHooksReady()) || !FitsRestoreProgress(native,host))continue;
        bool compatible=!later;
        for(const auto& entry:snapshot.files)if(coop_save::EqualPathText(entry.suffix,L".coop-progress")) {
            coop_career::Progress saved;
            compatible=coop_career::Decode(entry.bytes,saved) && FitsRestoreProgress(saved,host);
            if(compatible && later)compatible=saved.lastResult.eventHash==host.lastResult.eventHash &&
                saved.lastResult.epoch==host.lastResult.epoch && FitsRestoreProgress(host,saved);
            break;
        }
        if(compatible && coop_backup::Restore(gCoopBaseRoot,snapshot,point)) {
            LogMessage("[COOP_BACKUP] Client's own backup restored from the selected Host ticket.\r\n");
            return true;
        }
    }
    static DWORD lastNotice=0;
    const DWORD now=GetTickCount();
    if(!lastNotice || now-lastNotice>=3000) {
        lastNotice=now;
        CoopAnnouncements::Post("No compatible Player 2 backup was found for the Host's selected point. Ask the Host to choose another backup.");
    }
    return false;
}
static bool Initial(const coop_checkpoint::Completed& packet,const coop_career::Progress& received) {
    auto incoming=received;DiscardEmptyOffers(incoming);
    const uint32_t kind=packet.launchKind==coop_checkpoint::kRestoreCareer?1:packet.launchKind;
    const auto own=ClientName(packet.name);if(!Name(own)) return false;
    std::vector<uint8_t> file,payload,merged;bool found=false;
    if(kind==1) {
        // An exact campaign/role key, never the newest unrelated Client or SP.
        found=read(own,L"",file);
        if(!found) {
            if(probe(own,L"")!=0) return false;
            found=read(packet.name,L"",file); // Keep legacy mirror recoverable.
            if(!found && probe(packet.name,L"")!=0) return false;
        }
    }
    if(found) {
        if(coop_save::ValidateNativeProfileFile(file.data(),file.size())!=coop_save::SaveValidation::Valid) return false;
        payload.assign(file.begin()+0x24,file.end());
    }
    struct InitialRollback {
        std::vector<uint8_t> payload;
        uint8_t loaded=0,started=0;
        bool armed=false;
        ~InitialRollback() {
            if(!armed) return;
            const bool restored=NativeDeserializeProfile(payload.data());
            const bool flags=SafeWriteBytes(gFreshCareerAddresses.loaded,&loaded,1) &&
                SafeWriteBytes(gFreshCareerAddresses.started,&started,1);
            if(!restored || !flags) gRestoreSpPending=true; // Fail closed; no native career replay.
        }
    } rollback;
    rollback.payload.resize(coop_career::kNativePayloadSize);
    if(!NativeSerializeProfile(rollback.payload.data()) ||
        !ReadGameMemory(gFreshCareerAddresses.loaded,&rollback.loaded,1) ||
        !ReadGameMemory(gFreshCareerAddresses.started,&rollback.started,1)) return false;
    if(!found) {
        rollback.armed=true;
        if(!fresh()) return false;payload.resize(coop_career::kNativePayloadSize);
        if(!NativeSerializeProfile(payload.data())) return false;
        coop_career::Write32(payload.data()+coop_career::kActiveEventOffset,0); // No inherited SP active event.
    }
    auto next=incoming;
    if(kind==1) {
        std::vector<uint8_t> cache;coop_career::Progress stored;
        if(read(own,L".coop-progress",cache) && coop_career::Decode(cache,stored) && ValidateNative(stored)) {
            // Saved Client facts survive; its previous Host selection does not
            // outrank the current authenticated Host's first manifest.
            coop_career::ClearOffers(stored);if(!Combine(stored,incoming,next))return false;
        }
    }
    if(!CoopSpecialPrepareInitial(packet.name,own.c_str(),kind,false,payload,&next))return false;
    const bool specialHeld=CoopSpecialInitialHeld();
    if(specialHeld)merged=payload;
    else if(!coop_career::MergePayload(payload,next,merged)) return false;
    // This is a per-campaign filename, not the Session nickname. It prevents
    // starting a new team career from overwriting an unrelated personal one.
    if(!specialHeld){
        std::fill(merged.begin()+coop_career::kPersonalNameOffset,merged.begin()+coop_career::kPersonalNameOffset+32,uint8_t(0));
        std::memcpy(merged.data()+coop_career::kPersonalNameOffset,own.c_str(),own.size());
    }
    rollback.armed=true;
    if(!NativeDeserializeProfile(merged.data())) return false;
    const bool needsStarter=!coop_career::Completed(merged,coop_career::kPersonalEventB);
    if(needsStarter) {
        const uint8_t unloaded=0,started=coop_career::Completed(merged,coop_career::kPersonalEventA)?1:0;
        if(!SafeWriteBytes(gFreshCareerAddresses.loaded,&unloaded,1) ||
            !SafeWriteBytes(gFreshCareerAddresses.started,&started,1)) return false;
    }
    std::vector<uint8_t> progressBlob;
    const CoopBackupActionScope backupAction(coop_backup::Action::CareerStart);
    if(!coop_career::Encode(next,progressBlob) || !write(own,L".coop-progress",progressBlob) ||
        !coop_save::EncodeNativeProfileFile(merged,file) || (!specialHeld&&!write(own,L"",file))) return false;
    if(!specialHeld) {
        // The canonical OWN profile has now been durably committed. Restore
        // native UserProfile selection identity, independently of its DDAY
        // starter eligibility; started/A/B and personalStarter still route
        // the player's own onboarding. Never claim this after a failed write
        // or while a Special transaction owns the old native profile.
        const uint8_t selected=1;
        if(!SafeWriteBytes(gFreshCareerAddresses.loaded,&selected,1))return false;
        LogMessage("[SAVE_IDENTITY] Client own canonical profile committed; native selected-profile flag restored without changing starter progress.\r\n");
    }
    rollback.armed=false;
    BindNames(packet.name,own);common=next;haveCommon=true;dirty=true;personalStarter=needsStarter;
    historicalSpecials=next;haveHistoricalSpecials=true;
    lastHostOfferRevision=incoming.offersKnown?incoming.offerRevision:0;
    LogMessage(needsStarter?"[SAVE_PERSONAL] Player 2 will choose its own starter; shared stages cached until that choice.\r\n":
        "[SAVE_PERSONAL] Player 2 own garage/cash/customization restored; shared progress merged.\r\n");
    return true;
}
static bool MergeLive(const coop_career::Progress& incoming,bool forceSave) {
    // A shared completion must not retire this player's pending native cover
    // payout/unique choice. The local replay scope releases this gate after
    // each player's own reward; imports never replay or copy any reward.
    if(!specialMayImport())return false;
    std::vector<uint8_t> payload(coop_career::kNativePayloadSize),merged,blob;
    coop_career::Progress next;
    if(!NativeSerializeProfile(payload.data())) return false;
    auto accepted=incoming;
    if(!FilterLiveSpecials(accepted))return false;
    DiscardEmptyOffers(accepted);
    // The peer may propose earned facts, not arbitrary future offers/revisions.
    const bool host=CoopSnapshot().role==coop::Role::Host;
    if(host)coop_career::ClearOffers(accepted);
    else if(accepted.offersKnown && accepted.offerRevision<lastHostOfferRevision)return false;
    auto baseline=common;
    if(haveCommon&&!FilterLiveSpecials(baseline))return false;
    DiscardEmptyOffers(baseline);
    if(!host && accepted.offersKnown)coop_career::ClearOffers(baseline);
    if(haveCommon) {if(!Combine(baseline,accepted,next)) return false;} else next=accepted;
    if(!CoopVisualMayMerge(payload,next))return false;
    if(!coop_career::MergePayload(payload,next,merged) || !coop_career::Encode(next,blob)) return false;
    std::vector<Patch> patches;
    if(!ApplyLive(payload,merged,patches)) return false;
    std::vector<uint8_t> oldBlob;
    if(!forceSave && patches.empty() && haveCommon && coop_career::Encode(common,oldBlob) && oldBlob==blob) return true;
    if(!write(personal,L".coop-progress",blob) || ((forceSave || !patches.empty()) && !SavePayload(merged))) {
        Undo(patches);return false;
    }
    // Stock 541411/541416 use this pair after an event changes. A direct
    // shared-progress patch otherwise leaves the old eligible-icon cache.
    // Only a committed nonempty patch queues it; no-op merges never reroll.
    if(!patches.empty() || next.offersKnown && (!haveCommon || next.offerRevision!=common.offerRevision)) mapRefreshPending=true;
    common=next;haveCommon=true;
    if(!host && accepted.offersKnown)lastHostOfferRevision=accepted.offerRevision;
    return true;
}
static void RefreshEarnedMap(DWORD flow) {
    if(CoopVisualNeedsRefresh())mapRefreshPending=true;
    uint32_t track=0,nativeState=0,mission=0;uintptr_t native=0;
    if(!mapRefreshPending || flow!=6 || !IsCoopGameplayActive() || !gProfileContext.IsCoopActive() ||
        RaceIntentPending() || gCoopRaceState==COOP_RACE_STARTING || gTriggerRaceLoading ||
        !ReadGameMemory(gRaceContextAddresses.track,&track,4) || track!=4000 ||
        !ReadGameMemory(gRaceContextAddresses.nativeFlow,&native,4) || !IsValidUserPtr(native) ||
        !ReadGameMemory(native,&nativeState,4) || nativeState!=4 ||
        !ReadGameMemory(career+0xC08,&mission,4) || mission) return;
    // Preserve native 532C20 hidden-event rejection and shop discovery gates.
    // No entry flags, stage requirements or hidden-race visibility are changed.
    if(!CoopVisualRefreshGate())return;
    refreshCareerMap(reinterpret_cast<void*>(career));refreshMapIcons();
    mapRefreshPending=false;
    LogMessage("[CAREER_MAP] Earned map cache refreshed with stock visibility/discovery/hidden-event rules unchanged.\r\n");
}
static bool Receive(const coop_checkpoint::Completed& packet,DWORD flow,const coop::SessionSnapshot& state) {
    coop_career::Progress incoming;
    if(!state.joined || !state.fresh || packet.session!=state.sessionId || packet.ticket!=state.ticket ||
        !IsCoopGameplayActive() || !gProfileContext.IsCoopActive() || !Name(packet.name))return false;
    coop_backup::RestorePoint point;
    const bool restore=packet.launchKind==coop_checkpoint::kRestoreCareer;
    if(restore) {
        if(state.role!=coop::Role::Client || flow!=3 || state.initialBarrierComplete ||
            state.localRetainsNative || packet.file.size()<sizeof(point))return false;
        std::memcpy(&point,packet.file.data(),sizeof(point));
        const std::vector<uint8_t> progress(packet.file.begin()+sizeof(point),packet.file.end());
        if(!coop_backup::ValidPoint(point) || !coop_career::Decode(progress,incoming) || !ValidateNative(incoming) ||
            !RestoreClientCareer(packet.name,ClientName(packet.name),point,incoming))return false;
    } else if(!coop_career::Decode(packet.file,incoming) || !ValidateNative(incoming))return false;
    const CoopBackupActionScope backupAction(coop_backup::Action::SharedProgress);
    if(!state.initialBarrierComplete) {
        if(state.localRetainsNative) {
            if(state.role!=coop::Role::Client||flow!=6||packet.launchKind!=1||!SameName(campaign,packet.name)||
                !CoopSharedCareerCanApply(flow))return false;
            const auto oldRevision=lastHostOfferRevision;
            lastHostOfferRevision=0; // A fresh authenticated Host ticket owns its offer revision scope.
            const bool okay=MergeLive(incoming,true);
            if(!okay)lastHostOfferRevision=oldRevision;
            return okay;
        }
        if(state.role!=coop::Role::Client || flow!=3 || (packet.launchKind!=1 && packet.launchKind!=2 && !restore))return false;
        const bool okay=Initial(packet,incoming);
        if(!okay && restore) {
            const auto profile=WideText(ClientName(packet.name).c_str());
            if(!coop_backup::RevertRestore(gCoopBaseRoot,coop_backup::Role::Client,profile,point))
                LogMessage("[COOP_BACKUP] Client restore remains pending recovery before the next career entry.\r\n");
        }
        return okay;
    }
    if(packet.launchKind!=3 || !SameName(campaign,packet.name) || !CoopSharedCareerCanApply(flow)) return false;
    return MergeLive(incoming,true);
}
static bool CaptureAndQueue(DWORD now,bool save,const coop_career::Outcome* result=nullptr) {
    const auto state=CoopSnapshot();
    if(!state.joined || !state.fresh || !state.initialBarrierComplete || !Name(campaign) || !Name(personal)) return false;
    std::vector<uint8_t> payload(coop_career::kNativePayloadSize),blob;
    coop_career::Progress local,next;
    if(!NativeSerializeProfile(payload.data()) ||
        !coop_career::Extract(payload,local,DiscoveryEligible,specialHooksReady()) || !ValidateNative(local)) return false;
    if(result) local.lastResult=*result;
    if(!FilterLiveSpecials(local))return false;
    if(haveCommon) {if(!Combine(common,local,next)) return false;} else next=local;
    DiscardEmptyOffers(next);
    const bool offersChanged=CaptureOffers(next);
    // Manifest-only updates are durable even when no native personal save was
    // requested. The old policy could update common in RAM before its journal.
    if(offersChanged) {
        if(!coop_career::Encode(next,blob) || !write(personal,L".coop-progress",blob))return false;
        char line[192]{};std::snprintf(line,sizeof(line),"[CAREER_OFFERS] Host revision=%u stage=%u basis=%08X count=%zu durable canonical selection.\r\n",
            next.offerRevision,next.offerStage,next.offerBasis,next.offeredEvents.size());LogMessage(line);
    }
    const CoopBackupActionScope backupAction(result?coop_backup::Action::RaceResult:coop_backup::Action::CareerSave);
    if(!coop_career::Encode(next,blob) || (save && (!MergeLive(next,true)))) return false;
    common=next;haveCommon=true;
    const uint32_t digest=coop_career::Digest(blob);
    if(digest==lastQueuedDigest) {dirty=false;return true;}
    AcquireSRWLockExclusive(&gSessionLock);
    bool queued=false;
    if(!gCheckpointSender.Active()) {
        if(!++gCheckpointSerial) ++gCheckpointSerial;
        queued=gCheckpointSender.Start(state.sessionId,gCheckpointSerial,state.ticket,campaign.c_str(),blob,now,3);
    }
    ReleaseSRWLockExclusive(&gSessionLock);
    if(queued) {lastQueuedDigest=digest;dirty=false;LogMessage("[SAVE_SHARED] Shared completion/stage, verified cover/mission facts, max outruns and genuine race/shop discoveries queued; no car, money, rating, unique choice or option bytes.\r\n");}
    if(!queued) dirty=true;
    // A busy network transfer must not postpone/redo the local native save.
    // The receipt/common state remains dirty and is sent after that transfer.
    return queued || save;
}
// Read-only catalog observer. Native predicates remain the sole visibility
// authority; logging a hidden record never unlocks it. Changes also publish
// genuine discoveries even when the engine did not trigger an autosave.
static void ObserveCareer(DWORD now) {
    if(now-lastObservedAt<1500)return;lastObservedAt=now;
    uint32_t count=0,stage=0;uintptr_t events=0,shops=0;uint32_t eventCount=0,shopCount=0;
    uint8_t rows[coop_career::kMaximumEvents*coop_career::kEventStride]{};
    if(!ReadGameMemory(career+0x7AFC,&count,4) || count>coop_career::kMaximumEvents ||
        !ReadGameMemory(career+0x7AF8,&stage,4) || stage>=coop_career::kStageCount ||
        !ReadGameMemory(career+0x7B00,rows,count*8) ||
        !ReadGameMemory(eventTableAddress,&events,4) || !IsValidUserPtr(events) ||
        !ReadGameMemory(eventCountAddress,&eventCount,4) || eventCount>4096 ||
        !ReadGameMemory(shopTableAddress,&shops,4) || !IsValidUserPtr(shops) ||
        !ReadGameMemory(shopCountAddress,&shopCount,4) || shopCount>256)return;
    auto status=[&](uint32_t hash,unsigned offset) {
        for(uint32_t i=0;i<count;++i)if(coop_career::Read32(rows+i*8)==hash)return unsigned(rows[i*8+offset]);return 0u;
    };
    struct Row {uint32_t hash;uint8_t shop,type,tier,hidden,found,completed,visible,offered,category;};
    uint32_t offers[64]{};if(!ReadGameMemory(career+0x71C8,offers,sizeof(offers)))return;
    std::vector<Row> snapshot;uint32_t digest=2166136261u;
    auto mix=[&](uint32_t v){digest=(digest^v)*16777619u;};mix(stage);
    for(uint32_t i=0;i<eventCount+shopCount;++i) {
        const bool shop=i>=eventCount;const uintptr_t item=shop?shops+(i-eventCount)*0xA0:events+i*0x88;
        uint32_t hash=0;uint8_t category=3,type=0,tier=0,hidden=0;
        if(!ReadGameMemory(item+(shop?0x38:8),&hash,4) || !hash || coop_career::PersonalEvent(hash) ||
            (!shop && (!ReadGameMemory(item+0x34,&category,1) || category<1 || category>3)) ||
            !ReadGameMemory(item+(shop?0x50:0x0F),&type,1) ||
            !ReadGameMemory(item+(shop?0x9D:0x37),&tier,1) ||
            (!shop && !ReadGameMemory(item+0x7F,&hidden,1)))continue;
        Row row{hash,uint8_t(shop),type,tier,hidden,uint8_t(status(hash,4)==1),uint8_t(status(hash,6)==1),
            uint8_t(NativeVisible(shop?shopVisible:raceVisible,item)),uint8_t(!shop && std::find(std::begin(offers),std::end(offers),uint32_t(item))!=std::end(offers)),category};
        mix(hash);mix(row.shop);mix(type);mix(tier);mix(hidden);mix(row.found);mix(row.completed);mix(row.visible);mix(row.offered);mix(category);
        snapshot.push_back(row);
    }
    if(digest==lastObservedDigest)return;
    lastObservedDigest=digest;dirty=true;
    char line[256]{};
    std::snprintf(line,sizeof(line),"[CAREER_OBSERVE] role=%s profile=%s stage=%u digest=%08X rows=%zu (native visibility; no unlock writes).\r\n",
        gIsSecondInstance?"Client":"Host",personal.c_str(),stage,digest,snapshot.size());LogMessage(line);
    for(const auto& row:snapshot) {
        // Log current-stage/earned facts, including hidden classification. Avoid
        // dumping untouched future campaign chapters on every real discovery.
        if(row.tier>stage && !row.found && !row.completed && !row.visible)continue;
        std::snprintf(line,sizeof(line),"[CAREER_EVENT] hash=%08X kind=%s type=%u stage=%u hidden=%u discovered=%u completed=%u visible=%u offered=%u category=%u\r\n",
            row.hash,row.shop?"shop":"race",row.type,row.tier,row.hidden,row.found,row.completed,row.visible,row.offered,row.category);LogMessage(line);
    }
}
static void Tick(DWORD flow,const coop::SessionSnapshot& state) {
    if(!state.initialBarrierComplete || !IsCoopGameplayActive() || !gProfileContext.IsCoopActive() ||
        !CoopSharedCareerCanApply(flow) || !Name(personal)) return;
    if(gNativeSaveCompleted.exchange(false)) dirty=true;
    const DWORD now=GetTickCount();if(now-lastAttempt<300) return;lastAttempt=now;
    AcquireSRWLockShared(&gSessionLock);
    const bool failed=lastQueuedDigest && !gCheckpointSender.Active() && !gCheckpointSender.Done();
    ReleaseSRWLockShared(&gSessionLock);
    if(failed) {lastQueuedDigest=0;dirty=true;}
    // Cached advanced stages apply only AFTER this player's own first-car
    // milestone. This does not invoke CareerRead/Reset or replay any reward.
    if(haveCommon && !MergeLive(common,false)) {dirty=true;return;}
    RefreshEarnedMap(flow);
    if(flow==6)ObserveCareer(now);
    if(dirty || !lastQueuedDigest) CaptureAndQueue(now,false);
}
} // namespace CoopPersonalSave
