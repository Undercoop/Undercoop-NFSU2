// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Include after CoopSpecialRuntime.inl. The native timer hook only mirrors the
// shared value. Host authority advances on the UI thread, not the paused local
// simulation or a wall-clock deadline. Arrival menus must not stop the peer.
bool CoopSpecialClockPaused();
namespace CoopSpecialNative { namespace Timer {
struct Addresses {uintptr_t menuClock=0x8651AC;};
static Addresses addresses;
struct Clock {
    uint64_t session=0;uint32_t ticket=0,nonce=0,operation=0,request=0,event=0,tick=0,subticks=0;
    bool bound=false;
};
static Clock clock;
struct DisplayClock {Clock identity{};uint32_t authoritative=0,base=0,shown=0,tick=0;};
static DisplayClock display;
static const Record* (*record)()=ActiveRecord;
static bool (*paused)()=::CoopSpecialClockPaused;
static bool SameOperation(){return clock.bound&&clock.session==state.Session()&&clock.ticket==state.Ticket()&&
    clock.nonce==state.Nonce()&&clock.operation==state.Operation()&&clock.request==state.RequestId()&&clock.event==state.Current().id;}
static bool Owned(uintptr_t self,uint32_t& nativePhase){
    if(!installed||!OwnCareer()||!state.Active()||!coop_special::Timed(state.Current().kind)||
        state.OfflineRecovery()||self!=CoopSpecialNative::addresses.career+0xC00||!lifecycle.NativeProofWindow(state))return false;
    const auto* current=record();uintptr_t nativeMission=0;
    return current&&coop_special::Same(current->spec,state.Current())&&Read(self+8,nativeMission)&&
        nativeMission==current->pointer&&Read(self+0x11C,nativePhase);
}
static uint32_t DisplayRemaining(){
    const uint32_t authoritative=state.Remaining();
    if(state.Player()!=2||!(state.Phase()&coop_special::Go)||
        (state.Phase()&(coop_special::Release|coop_special::Commit|coop_special::Failed|coop_special::Declined)))return authoritative;
    uint32_t tick=0;if(!Read(addresses.menuClock,tick)||!paused)return authoritative;
    const auto& d=display.identity;
    const bool same=d.bound&&d.session==state.Session()&&d.ticket==state.Ticket()&&d.nonce==state.Nonce()&&
        d.operation==state.Operation()&&d.request==state.RequestId()&&d.event==state.Current().id;
    if(!same){
        display={};display.identity={state.Session(),state.Ticket(),state.Nonce(),state.Operation(),state.RequestId(),state.Current().id,tick,0,true};
        display.authoritative=display.base=display.shown=authoritative;display.tick=tick;return authoritative;
    }
    if(paused()){
        display.tick=tick;display.authoritative=authoritative;display.base=display.shown=(std::min)(display.shown,authoritative);return display.shown;
    }
    if(display.authoritative!=authoritative){
        display.authoritative=authoritative;display.base=display.shown=(std::min)(display.shown,authoritative);display.tick=tick;
    }
    const uint32_t elapsed=tick-display.tick;
    // Visual-only interpolation between Host's200ms samples. Never extrapolate
    // a disconnected peer indefinitely and NEVER mutate State/expire on Client.
    const uint32_t ms=(std::min)(elapsed/4u,500u);
    const uint32_t predicted=display.base>ms?display.base-ms:0u;
    display.shown=(std::min)(display.shown,predicted);
    return display.shown;
}
static void Advance(){
    uint32_t nativePhase=0;
    if(state.Player()!=1||!IsCoopGameplayActive()||!Owned(CoopSpecialNative::addresses.career+0xC00,nativePhase)||
        (nativePhase!=1&&nativePhase!=2&&nativePhase!=3)||!(state.Phase()&coop_special::Go)||
        (state.Phase()&(coop_special::Release|coop_special::Commit|coop_special::Failed|coop_special::Declined)))return;
    uint32_t tick=0;
    if(!Read(addresses.menuClock,tick)){state.Fail(4,runtimeNow());return;}
    if(!SameOperation()){
        clock={state.Session(),state.Ticket(),state.Nonce(),state.Operation(),state.RequestId(),state.Current().id,tick,0,true};
        return;
    }
    // 57A050: round(frameSeconds * [784264=4000] + [784260=0.5]).
    // These are QUARTER-MILLISECOND units, NOT milliseconds. 784268=1/4000
    // is the matching native conversion back to seconds. Carry sub-ms ticks
    // instead of flooring each frame or guessing a speed-dependent divider.
    // Update even while genuinely paused: Resume must not spend paused time.
    const uint32_t elapsed=tick-clock.tick;clock.tick=tick;
    if(!paused){state.Fail(4,runtimeNow());return;}
    if(paused()||!elapsed)return;
    if(elapsed>14400000u){state.Fail(4,runtimeNow());return;} // One hour in native units; reject resets.
    const uint32_t fractional=elapsed+clock.subticks;
    const uint32_t milliseconds=fractional/4u;clock.subticks=fractional%4u;
    if(!milliseconds)return;
    const uint32_t remaining=state.Remaining();
    state.Clock(milliseconds>=remaining?0u:remaining-milliseconds,runtimeNow());
}
static bool Tick(void* self){
    uint32_t nativePhase=0;
    if(!Owned(uintptr_t(self),nativePhase))return false;
    // Suppress vanilla's independent solo expiry on BOTH peers, including an
    // arrived Host. The UI-thread callback above is the only countdown owner.
    const float shown=float(DisplayRemaining())/1000.0f;
    if(!SafeWriteBytes(uintptr_t(self)+4,&shown,sizeof(shown)))state.Fail(4,runtimeNow());
    return true;
}
}} // namespace CoopSpecialNative::Timer
