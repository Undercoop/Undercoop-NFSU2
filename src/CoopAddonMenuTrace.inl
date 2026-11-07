// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Passive addon menu witnesses. Never changes an unlock flag, calls a native
// menu function, patches another ASI, or writes a profile. UI thread only.
namespace CoopAddonMenuTrace {
static std::array<uint64_t,coop_car_catalog::kMaxModels> previous{};
struct GeometryEvidence {uintptr_t table=0;bool opened=false;};
static std::array<GeometryEvidence,coop_car_catalog::kMaxModels> geometryCache{};
static DWORD sampledAt=0;
static bool Membership(uintptr_t base,uint32_t offset,uint32_t count,uint32_t model,bool& found) {
    found=false;if(count>256)return false;
    for(uint32_t i=0;i<count;++i){uint32_t item=UINT32_MAX;
        if(!ReadGameMemory(base+offset+i*4,&item,4))return false;
        if(item==model)found=true;
    }return true;
}
static void Tick(DWORD flow) {
    const uint32_t count=CoopCarCatalogNative::Count();
    if((flow!=3&&flow!=6)||count<=coop_car_catalog::kStockCount||count>coop_car_catalog::kMaxModels)return;
    const bool runtimeReady=CoopCarCatalogNative::Ready();
    const DWORD now=GetTickCount();if(now-sampledAt<2000)return;sampledAt=now;
    coop_unlimiter_ride::detail::ErrorScope saved;
    uint8_t entry[6]{};uintptr_t target=0,base=0;
    if(ReadGameMemory(coop_unlimiter_ride::kEntry,entry,sizeof(entry))&&
        coop_unlimiter_ride::Approved(coop_unlimiter_ride::kEntry,entry,sizeof(entry))){
        std::array<uint8_t,6> branch{};std::memcpy(branch.data(),entry,6);
        if(coop_unlimiter_ride::Target(coop_unlimiter_ride::kEntry,branch,target))base=target-coop_unlimiter_ride::kTargetRva;
    }
    uint32_t nativeCount=UINT32_MAX,bootCount=UINT32_MAX,stage=UINT32_MAX;
    if(base){ReadGameMemory(base+0x44A1C,&nativeCount,4);ReadGameMemory(base+0x42AA0,&bootCount,4);}
    ReadGameMemory(CoopPersonalSave::career+0x7AF8,&stage,4);
    for(uint32_t model=coop_car_catalog::kStockCount;model<count;++model){
        const uintptr_t row=runtimeReady?CoopCarCatalogNative::validTable+model*coop_car_catalog::kRecordBytes:0;
        char geometry[32]{};uint8_t lot=0xFF;bool na=false,rowRegion=false;
        const bool pathRead=runtimeReady&&ReadGameMemory(row+0x40,geometry,sizeof(geometry))&&std::memchr(geometry,0,sizeof(geometry));
        if(base)ReadGameMemory(base+0x45628+model,&lot,1);
        const bool lists=base&&Membership(base,0x44A20,bootCount,model,na)&&Membership(base,0x443B8,bootCount,model,rowRegion);
        bool opened=false;DWORD error=ERROR_INVALID_NAME;
        // The ordinary loader's geometry name must stay inside this game.
        if(runtimeReady&&!CoopModSupport::invalidated.load()&&geometryCache[model].opened&&
            geometryCache[model].table==CoopCarCatalogNative::validTable){opened=true;error=ERROR_SUCCESS;}
        else if(pathRead&&!std::strncmp(geometry,"CARS\\",5)&&!std::strstr(geometry,"..")&&!std::strchr(geometry,':')&&
            !CoopModSupport::root.empty()){
            const auto path=CoopModSupport::root+L"\\"+WideText(geometry);
            HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            error=file==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;
            if(file!=INVALID_HANDLE_VALUE){BY_HANDLE_FILE_INFORMATION info{};
                const bool disk=GetFileType(file)==FILE_TYPE_DISK;
                const bool metadata=disk&&GetFileInformationByHandle(file,&info);
                error=!disk?ERROR_NOT_SUPPORTED:!metadata?GetLastError():
                    (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))?ERROR_ACCESS_DENIED:ERROR_SUCCESS;
                opened=metadata&&error==ERROR_SUCCESS;CloseHandle(file);
                // Positive file evidence belongs to this immutable validated
                // table/root. The mod watcher separately revokes file changes.
                if(opened)geometryCache[model]={CoopCarCatalogNative::validTable,true};
            }
        }
        uint64_t signature=14695981039346656037ULL;
        for(uint32_t item:{uint32_t(flow),nativeCount,bootCount,stage,uint32_t(lot),uint32_t(lists),uint32_t(na),uint32_t(rowRegion),uint32_t(opened),uint32_t(error),uint32_t(gProfileContext.IsCoopActive()),uint32_t(runtimeReady)})coop_car_catalog::Mix(signature,item);
        if(previous[model]==signature)continue;previous[model]=signature;
        char line[600]{};std::snprintf(line,sizeof(line),
            "[ADDON_MENU] model=%u name=%s fileCount=%u nativeLoaderCount=%u runtimeBounds=%u runtimeReady=%u usage=%u flow=%u stage=%u coopProfile=%u initiallyUnlockedCount=%u bootListsReadable=%u inNA=%u inROW=%u carLotFlag=%u geometry='%s' geometryOpen=%u error=%lu; passive only, no unlock/save writes.\r\n",
            model,CoopCarCatalogNative::installed.models[model].name,count,nativeCount,
            unsigned(CoopCarCatalogNative::NativeAddonBounds()),unsigned(runtimeReady),CoopCarCatalogNative::installed.models[model].usage,
            flow,stage,unsigned(gProfileContext.IsCoopActive()),bootCount,unsigned(lists),unsigned(na),unsigned(rowRegion),unsigned(lot),
            pathRead?geometry:"<unreadable>",unsigned(opened),error);LogMessage(line);
    }
}
}
void TraceCoopAddonMenus(DWORD flow){CoopAddonMenuTrace::Tick(flow);}
