// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// 527D30 is called inside stock 533310 BEFORE icon bindings are built. Its
// chosen-event pointers cannot be copied across processes or saved profiles.
// The adapter maps the shared bounded IDs through each game's own catalog.
// Only this completed stock call proves a new selection exists. Resume/menu
// serialization must not invoke it early or publish its uninitialized cache.
bool InstallCoopCareerOfferHooks() {
    static constexpr BYTE entry[6]={0x81,0xEC,0xA0,0x01,0x00,0x00}; // SUB ESP,1A0; one complete instruction.
    // CareerNative owns the New Career button as part of its atomic native
    // menu batch. Installing it again here rejected our own JMP and kept all
    // career entry disabled. Require that exact owned hook and its original
    // trampoline instead of accepting arbitrary patched bytes or hooking twice.
    bool ownsNewCareer=false;
    if(CoopCareerNative::gHooksInstalled)for(const auto& native:CoopCareerNative::gDetours) {
        if(native.address!=0x4AF090)continue;
        BYTE current[8]{},original[8]{};int32_t relative=0;
        if(native.installed && native.trampoline && native.length==sizeof(current) &&
            native.replacement==reinterpret_cast<void*>(CoopCareerNative::HookNewCareerButton) &&
            CoopCareerNative::gNewCareerButton==reinterpret_cast<CoopCareerNative::NewCareerButtonFn>(native.trampoline) &&
            ReadGameMemory(native.address,current,sizeof(current)) &&
            ReadGameMemory(reinterpret_cast<uintptr_t>(native.trampoline),original,sizeof(original))) {
            std::memcpy(&relative,current+1,sizeof(relative));
            ownsNewCareer=current[0]==0xE9 &&
                uintptr_t(native.address+5+relative)==reinterpret_cast<uintptr_t>(native.replacement) &&
                current[5]==0x90 && current[6]==0x90 && current[7]==0x90 &&
                !std::memcmp(original,native.expected,sizeof(original));
        }
    }
    if(!ownsNewCareer) {
        LogMessage("[CAREER_OFFERS] Owned New Career menu hook unavailable; selector remains disabled.\r\n");return false;
    }
    static CoopCareerNative::Detour hooks[]={
        {0x527D30,entry,sizeof(entry),reinterpret_cast<void*>(CoopPersonalSave::HookOffersBuild)}
    };
    if(hooks[0].installed)return true;
    for(const auto& hook:hooks)if(!CoopCareerNative::MatchesNativeBytes(hook)) {
        LogMessage("[CAREER_OFFERS] Native offer selector mismatch at 00527D30; hooks unavailable.\r\n");return false;
    }
    for(auto& hook:hooks)if(!CoopCareerNative::InstallDetour(hook)) {
        for(auto& undo:hooks)CoopCareerNative::RollBackDetour(undo);
        LogMessage("[CAREER_OFFERS] Native selector/New Career hook failure; batch restored.\r\n");return false;
    }
    LogMessage("[CAREER_OFFERS] Native random offers scoped to durable Host-selected IDs; SP/hidden discovery gates preserved.\r\n");return true;
}
