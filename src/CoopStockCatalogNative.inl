// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Imported native saves contain a derived STOCK template cache. Reconcile
// missing addon templates without rebuilding owned/custom cars or career data.
namespace CoopStockCatalogNative {
using namespace coop_stock_catalog;
struct Addresses {uintptr_t owners[2]{0x83AD90,0x8458DC};uintptr_t reader=0x516850;};
static Addresses addresses;
using Reader=void*(__thiscall*)(void*,const void*,uint32_t);
static Reader original=nullptr;
static bool installed=false,commitFault=false;
static SRWLOCK readerLock=SRWLOCK_INIT;
static std::atomic<uint32_t> pending{3};
struct Cache {uintptr_t table=0;uint64_t fingerprint=0;uint32_t count=UINT32_MAX;DWORD retryAt=0;int error=-1;};
static Cache cache[2];
static bool (*write)(uintptr_t,const void*,size_t)=SafeWriteBytes;
static bool (*catalogBounds)()=CoopCarCatalogNative::NativeAddonBounds;
static unsigned Owner(uintptr_t value){for(unsigned i=0;i<2;++i)if(addresses.owners[i]==value)return i;return 2;}
static void* __fastcall HookReader(void* self,void*,const void* bytes,uint32_t size){
    const unsigned owner=Owner(reinterpret_cast<uintptr_t>(self));
    if(owner==2)return original(self,bytes,size);
    AcquireSRWLockExclusive(&readerLock);
    void* result=nullptr;DWORD error=0;bool completed=false;
    __try {result=original(self,bytes,size);error=GetLastError();completed=true;}
    __finally {
        if(completed)pending.fetch_or(1u<<owner);
        ReleaseSRWLockExclusive(&readerLock);
        if(completed)SetLastError(error);
    }
    return result;
}
static bool OwnedReader(){
    if(!installed||!original)return false;uint8_t code[5]{};int32_t distance=0;
    if(!ReadGameMemory(addresses.reader,code,sizeof(code))||code[0]!=0xE9)return false;
    std::memcpy(&distance,code+1,4);
    return addresses.reader+5+distance==reinterpret_cast<uintptr_t>(&HookReader);
}
static bool SnapshotAt(uintptr_t owner,Snapshot& value){
    if(Owner(owner)==2||!ReadGameMemory(owner,value.rows.data(),sizeof(value.rows))||
        !ReadGameMemory(owner+0x540,&value.count,4))return false;
    uintptr_t pointers[kCapacity]{};
    if(!ReadGameMemory(owner+0x9BD8,pointers,sizeof(pointers)))return false;
    for(uint32_t i=0;i<kCapacity;++i)if(pointers[i]!=owner+i*sizeof(Row))return false;
    return true;
}
static bool Writable(uintptr_t at,size_t size){
    if(!size||!IsValidUserPtr(at)||size>0x7FFE0000u-at)return false;
    const uintptr_t end=at+size;
    while(at<end){MEMORY_BASIC_INFORMATION page{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&page,sizeof(page))||page.State!=MEM_COMMIT||
            (page.Protect&(PAGE_GUARD|PAGE_NOACCESS))||!(page.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
        const uintptr_t next=reinterpret_cast<uintptr_t>(page.BaseAddress)+page.RegionSize;
        if(next<=at)return false;at=(std::min)(next,end);
    }return true;
}
static bool Same(const Snapshot& a,const Snapshot& b){return a.count==b.count&&!std::memcmp(a.rows.data(),b.rows.data(),sizeof(a.rows));}
static bool Commit(uintptr_t owner,const Snapshot& before,const Plan& plan){
    // Revalidate the complete destination before any write. Fields are staged
    // while inactive; active is published last per row, count last for the DB.
    Snapshot current;Error error;
    if(!plan.added)return true;
    if(commitFault||!OwnedReader()||!Validate(before,error)||!Validate(plan.after,error)||plan.after.count!=before.count+plan.added||
        !SnapshotAt(owner,current)||!Same(before,current)||!Writable(owner+0x540,4))return false;
    if(std::memcmp(before.rows.data(),plan.after.rows.data(),before.count*sizeof(Row))||
        std::memcmp(before.rows.data()+plan.after.count,plan.after.rows.data()+plan.after.count,(kCapacity-plan.after.count)*sizeof(Row)))return false;
    for(uint32_t i=before.count;i<plan.after.count;++i){
        if(!Unused(before.rows[i])||plan.after.rows[i].vtable!=before.rows[i].vtable||
            !Writable(owner+i*sizeof(Row)+4,sizeof(Row)-4))return false;
    }
    bool okay=true;
    for(uint32_t i=before.count;i<plan.after.count&&okay;++i){Row staged=plan.after.rows[i];staged.active=0;
        okay=write(owner+i*sizeof(Row)+4,&staged.unknown,sizeof(Row)-4);
    }
    for(uint32_t i=before.count;i<plan.after.count&&okay;++i){const uint32_t active=1;okay=write(owner+i*sizeof(Row)+0x10,&active,4);}
    if(okay)okay=write(owner+0x540,&plan.after.count,4);
    Snapshot after;if(okay)okay=SnapshotAt(owner,after)&&Same(after,plan.after);
    if(okay)return true;
    bool restored=true;
    for(uint32_t i=before.count;i<plan.after.count;++i)
        restored=write(owner+i*sizeof(Row)+4,&before.rows[i].unknown,sizeof(Row)-4)&&restored;
    restored=write(owner+0x540,&before.count,4)&&restored;
    Snapshot rolledBack;restored=restored&&SnapshotAt(owner,rolledBack)&&Same(before,rolledBack);
    if(!restored){commitFault=true;LogMessage("[ADDON_STOCK] Cache rollback could not be verified; further cache writes stopped. Restart required.\r\n");}
    return false;
}
static bool Models(std::vector<Model>& models){
    if(!CoopCarCatalogNative::Ready()||!catalogBounds())return false;
    const uint32_t count=CoopCarCatalogNative::Count();if(count<=kFirstAddon||count>kMaxModels)return false;
    uintptr_t table=0;
    if(!ReadGameMemory(CoopCarCatalogNative::tableAddress,&table,sizeof(table))||table!=CoopCarCatalogNative::validTable)return false;
    for(uint32_t id=kFirstAddon;id<count;++id){Model item;item.id=id;
        const auto& source=CoopCarCatalogNative::installed.models[id];std::memcpy(item.name,source.name,sizeof(item.name));item.usage=source.usage;
        coop_car_catalog::Model actual;
        if(!CoopCarCatalogNative::ReadRecord(table,id,actual)||actual.usage!=source.usage||std::strcmp(actual.name,source.name))return false;
        const uintptr_t row=table+id*coop_car_catalog::kRecordBytes;
        uint8_t game=0;char geometry[32]{};
        if(!ReadGameMemory(row+0xDA,&game,1)||!ReadGameMemory(row+0x40,geometry,sizeof(geometry))||!std::memchr(geometry,0,sizeof(geometry)))return false;
        item.gameEligible=(game&2)!=0;item.geometryAvailable=geometry[0]!=0;models.push_back(item);
    }return true;
}
static void Tick(DWORD flow){
    if((flow!=3&&flow!=6)||commitFault||CoopModSupport::invalidated.load()||
        CoopCarCatalogNative::Count()<=kFirstAddon||!CoopCarCatalogNative::Ready()||!OwnedReader())return;
    const DWORD now=GetTickCount();
    if(!TryAcquireSRWLockExclusive(&readerLock))return;
    coop_unlimiter_ride::detail::ErrorScope saved;
    struct Lease {~Lease(){ReleaseSRWLockExclusive(&readerLock);}} lease;
    for(unsigned i=0;i<2;++i){auto& remembered=cache[i];const uintptr_t owner=addresses.owners[i];uint32_t count=0;
        if(!ReadGameMemory(owner+0x540,&count,4)||!count)continue; // Uninitialized second profile is not a loaded cache.
        const uint32_t bit=1u<<i;const uint64_t fingerprint=CoopCarCatalogNative::Fingerprint();
        if(!(pending.load()&bit)&&remembered.table==CoopCarCatalogNative::validTable&&remembered.fingerprint==fingerprint&&remembered.count==count)continue;
        if(remembered.retryAt&&now-remembered.retryAt<1000)continue;remembered.retryAt=now;
        Snapshot before;Plan plan;Error error=Error::Input;std::vector<Model> models;
        const bool planned=SnapshotAt(owner,before)&&Models(models)&&Build(before,models.data(),models.size(),plan,error);
        if(!planned||!Commit(owner,before,plan)){
            const int code=planned?100:int(error);
            if(remembered.error!=code){remembered.error=code;char line[260]{};std::snprintf(line,sizeof(line),
                "[ADDON_STOCK] DB=%08X repair refused: %s; existing templates, garage and career retained.\r\n",unsigned(owner),planned?"destination changed or write refused":ErrorText(error));LogMessage(line);}
            continue;
        }
        remembered.table=CoopCarCatalogNative::validTable;remembered.fingerprint=fingerprint;remembered.count=plan.after.count;remembered.error=-1;pending.fetch_and(~bit);
        if(plan.added){char line[260]{};std::snprintf(line,sizeof(line),
            "[ADDON_STOCK] DB=%08X missing addon STOCK templates appended=%u count=%u->%u; existing stock metadata and owned/custom garage/cash/career preserved. RAM cache; normal autosave may persist it.\r\n",
            unsigned(owner),plan.added,before.count,plan.after.count);LogMessage(line);}
    }
}
static bool Install(){
    if(installed)return OwnedReader();
    const uint8_t expected[]{0x8B,0x44,0x24,0x04,0x53,0x55,0x8B,0xD9,0x56,0x57};uint8_t bytes[sizeof(expected)]{};
    if(!ReadGameMemory(addresses.reader,bytes,sizeof(bytes))||std::memcmp(bytes,expected,sizeof(bytes)))return false;
    auto* trampoline=static_cast<uint8_t*>(VirtualAlloc(nullptr,16,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!trampoline)return false;
    std::memcpy(trampoline,bytes,5);trampoline[5]=0xE9;
    const intptr_t back=intptr_t(addresses.reader+5)-intptr_t(trampoline+10),jump=intptr_t(&HookReader)-intptr_t(addresses.reader+5);
    if(back<INT32_MIN||back>INT32_MAX||jump<INT32_MIN||jump>INT32_MAX){VirtualFree(trampoline,0,MEM_RELEASE);return false;}
    const int32_t relativeBack=int32_t(back);std::memcpy(trampoline+6,&relativeBack,4);
    DWORD unused=0;if(!VirtualProtect(trampoline,16,PAGE_EXECUTE_READ,&unused)){VirtualFree(trampoline,0,MEM_RELEASE);return false;}
    FlushInstructionCache(GetCurrentProcess(),trampoline,16);original=reinterpret_cast<Reader>(trampoline);
    DWORD old=0;if(!VirtualProtect(reinterpret_cast<void*>(addresses.reader),5,PAGE_EXECUTE_READWRITE,&old)){
        original=nullptr;VirtualFree(trampoline,0,MEM_RELEASE);return false;
    }
    uint8_t current[sizeof(expected)]{};
    if(!ReadGameMemory(addresses.reader,current,sizeof(current))||std::memcmp(current,expected,sizeof(current))){
        VirtualProtect(reinterpret_cast<void*>(addresses.reader),5,old,&unused);original=nullptr;VirtualFree(trampoline,0,MEM_RELEASE);return false;
    }
    uint8_t patch[5]{0xE9};const int32_t relative=int32_t(jump);std::memcpy(patch+1,&relative,4);
    const bool patched=SafeWriteBytes(addresses.reader,patch,sizeof(patch));
    bool restored=true;
    if(!patched){uint8_t probe[5]{};restored=SafeWriteBytes(addresses.reader,bytes,5)&&
        ReadGameMemory(addresses.reader,probe,sizeof(probe))&&!std::memcmp(probe,bytes,sizeof(probe));}
    FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(addresses.reader),5);
    VirtualProtect(reinterpret_cast<void*>(addresses.reader),5,old,&unused);
    if(!patched){
        if(restored){original=nullptr;VirtualFree(trampoline,0,MEM_RELEASE);}
        else {commitFault=true;LogMessage("[ADDON_STOCK] Reader hook restoration could not be verified; call-through retained, cache writes disabled. Restart required.\r\n");}
        return false;
    }
    installed=true;pending.store(3);LogMessage("[ADDON_STOCK] Native stock-cache import observer installed; append-only reconciliation preserves custom/owned records.\r\n");return true;
}
}
bool InstallCoopStockCatalogReaderHook(){return CoopStockCatalogNative::Install();}
bool CoopStockCatalogReaderReady(){return !CoopStockCatalogNative::commitFault&&CoopStockCatalogNative::cache[0].error<0&&
    CoopStockCatalogNative::cache[1].error<0&&CoopStockCatalogNative::OwnedReader();}
void TickCoopStockCatalog(DWORD flow){CoopStockCatalogNative::Tick(flow);}
