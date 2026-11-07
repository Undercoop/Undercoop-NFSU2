// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Game-thread-only menu/re-entry adapter. No native save bytes are kept here.
static uint32_t gCoopCareerMenuGeneration=0;
static uint32_t gPreparedHostCareerTicket=0;
static uint32_t gCoopReturningForTicket=0;
static bool gCoopAutoRejoinPending=false;
static bool gCoopMenuAnnouncementPending=false;
static bool gCoopResumePreviewAvailable=false;
static std::string gCoopRequestedHostCampaign;
static std::string gCoopExpectedMenuProfile;
struct CoopHistoryContext {
    std::wstring root;
    coop_career_history::History history;
    coop_career_history::Guard guard;
    bool readable=false;
    std::string recordedCampaign,recordedPersonal;
};
static CoopHistoryContext gCoopHistory[2];
static CoopHistoryContext& CareerHistory(coop::Role role) {
    auto& context=gCoopHistory[role==coop::Role::Client?1:0];
    const auto root=gProfileContext.CoopRoot(role==coop::Role::Client?coop_save::Role::Client:coop_save::Role::Host);
    if(context.root!=root) {
        context={};context.root=root;
        const auto result=coop_career_history::Load(root,context.history,context.guard);
        context.readable=result==coop_career_history::Result::Ok||result==coop_career_history::Result::Missing;
        if(!context.readable)LogMessage("[CAREER_HISTORY] Metadata unavailable; native saves are unchanged. Use Load Career to choose explicitly.\r\n");
    }
    return context;
}
static bool ResolveClientCampaign(const std::string& personal,std::string& campaign,const coop::SessionSnapshot& state) {
    auto& history=CareerHistory(coop::Role::Client);
    if(history.readable&&history.history.ResolveCampaign(personal,campaign))return true;
    if(CoopPersonalSave::SameName(CoopPersonalSave::personal,personal)&&CoopPersonalSave::Name(CoopPersonalSave::campaign)) {
        campaign=CoopPersonalSave::campaign;return true;
    }
    if(CoopPersonalSave::Name(state.campaignName)&&
        CoopPersonalSave::SameName(CoopPersonalSave::ClientName(state.campaignName),personal)) {
        campaign=state.campaignName;return true;
    }
    if(personal.size()>3&&_stricmp(personal.c_str()+personal.size()-3,"_P2")==0) {
        const auto base=personal.substr(0,personal.size()-3);
        if(base.size()<=28&&CoopPersonalSave::Name(base)&&CoopPersonalSave::SameName(CoopPersonalSave::ClientName(base),personal)) {
            // A hashed long alias cannot be reversed without a saved binding.
            const bool hashed=base.size()>=9&&base[base.size()-9]=='_'&&
                std::all_of(base.end()-8,base.end(),[](char c){return (c>='0'&&c<='9')||(c>='A'&&c<='F');});
            if(!hashed){campaign=base;return true;}
        }
        return false;
    }
    if(CoopPersonalSave::Name(personal)){campaign=personal;return true;} // Original legacy personal mirror.
    return false;
}
static std::string PreferredCoopPersonal(const coop::SessionSnapshot& state) {
    if(state.role==coop::Role::Host&&!gCoopRequestedHostCampaign.empty())return gCoopRequestedHostCampaign;
    auto& history=CareerHistory(state.role);std::string name;
    if(!history.readable)return {};
    if(history.history.LastPersonal(name))return name;
    if(state.role==coop::Role::Host)return gConfiguredCareerProfile;
    if(CoopPersonalSave::Name(state.campaignName))return CoopPersonalSave::ClientName(state.campaignName);
    return {};
}
static void RememberLoadedCoopCareer(DWORD flow,const coop::SessionSnapshot& state) {
    if(flow!=6||!state.initialBarrierComplete||!gLocalSessionStarted||!IsCoopGameplayActive()||
        !gProfileContext.IsCoopActive()||!CoopPersonalSave::Name(CoopPersonalSave::campaign)||!CoopPersonalSave::Name(CoopPersonalSave::personal)||
        !CoopPersonalSave::SameName(state.campaignName,CoopPersonalSave::campaign))return;
    char native[32]{};
    if(!ReadGameMemory(gCoopNativeProfileName,native,31)||!CoopPersonalSave::SameName(native,CoopPersonalSave::personal))return;
    auto& context=CareerHistory(state.role);
    if(!context.readable||(context.recordedCampaign==CoopPersonalSave::campaign&&context.recordedPersonal==CoopPersonalSave::personal))return;
    auto proposed=context.history;
    if(!proposed.Remember(CoopPersonalSave::campaign,CoopPersonalSave::personal))return;
    if(coop_career_history::Save(context.root,proposed,context.guard)!=coop_career_history::Result::Ok) {
        context.readable=false;LogMessage("[CAREER_HISTORY] Last-career metadata commit failed; native saves were not modified.\r\n");return;
    }
    context.history=std::move(proposed);context.recordedCampaign=CoopPersonalSave::campaign;context.recordedPersonal=CoopPersonalSave::personal;
    char line[180]{};std::snprintf(line,sizeof(line),"[CAREER_HISTORY] Last successfully loaded campaign=%s personal=%s role=%s; Resume preference committed.\r\n",
        context.recordedCampaign.c_str(),context.recordedPersonal.c_str(),state.role==coop::Role::Host?"Host":"Client");LogMessage(line);
}
static bool CanRetainCoopNative(const char* campaign,DWORD flow) {
    uint32_t track=0;
    return campaign&&CoopPersonalSave::Name(campaign)&&flow==6&&gLocalSessionStarted&&IsCoopGameplayActive()&&
        gProfileContext.IsCoopActive()&&CoopPersonalSave::SameName(campaign,CoopPersonalSave::campaign)&&
        ReadGameMemory(0x89E7A0,&track,4)&&track==4000&&CoopSharedCareerCanApply(flow)&&CoopSpecialMayImport();
}
// Exact stock RF6 quit/return path: 606AE6 calls 603DE0 (__thiscall, RET0).
// This retires native simulation/UI and requests normal FE/save teardown. It
// does NOT import a new profile, fake a flow byte or merely push a menu over a world.
static bool ReturnCoopNativeToMenu(uint32_t ticket) {
    uint32_t flow=0,mode=0,nativeState=0;uintptr_t native=0;
    const BYTE expected[]{0x56,0x8B,0xF1,0xB9,0xD0,0xA9,0x83,0x00};BYTE actual[sizeof(expected)]{};
    if(!IsCoopGameplayActive()||!gProfileContext.IsCoopActive()||!CoopSpecialMayImport()||
        !ReadGameMemory(kGameFlowState,&flow,4)||flow!=6||!ReadGameMemory(0x850078,&mode,4)||mode!=1||
        !ReadGameMemory(0x8900D8,&native,4)||!IsValidUserPtr(native)||!ReadGameMemory(native,&nativeState,4)||
        (nativeState!=4&&nativeState!=6&&nativeState!=7)||
        !ReadGameMemory(0x603DE0,actual,sizeof(actual))||std::memcmp(actual,expected,sizeof(actual)))return false;
    gCoopReturningForTicket=ticket;
    __try {reinterpret_cast<void(__thiscall*)(void*)>(0x603DE0)(reinterpret_cast<void*>(native));}
    __except(EXCEPTION_EXECUTE_HANDLER){gCoopReturningForTicket=0;return false;}
    LogMessage("[SESSION_REENTRY] Stock native return-to-menu requested before changing campaign; no live profile import.\r\n");return true;
}
