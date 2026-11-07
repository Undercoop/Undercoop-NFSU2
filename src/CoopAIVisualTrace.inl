// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Game-thread diagnostics only. Included after CoopAIWorldRuntime.inl.
// This file never calls native constructors, loaders, or write helpers.
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace CoopAIVisualTrace {
static_assert(sizeof(uintptr_t)==4,"Native visual trace requires SPEED2's x86 layout");
struct Addresses { uintptr_t loader=0x008A3288; };
static Addresses addresses{};
static coop_world::Identity lastScope{};
static coop::Role lastRole=coop::Role::Idle;
static DWORD lastAt=0;
static bool haveAt=false;

template<class T> static bool Read(uintptr_t base,size_t offset,T& value) {
    const uintptr_t target=base+offset;
    return IsValidUserPtr(base) && target>=base &&
        target+sizeof(T)>=target && ReadGameMemory(target,&value,sizeof(value));
}
struct Line {
    char data[1800]{};size_t used=0;
    template<class... Args> void Add(const char* format,Args... args) {
        if(used>=sizeof(data)-1) return;
        const int count=std::snprintf(data+used,sizeof(data)-used,format,args...);
        if(count>0) used+=static_cast<size_t>(count)<sizeof(data)-used?
            static_cast<size_t>(count):sizeof(data)-used-1;
    }
    void Finish() { Add("\r\n");LogMessage(data); }
};
static void Pointer(Line& line,const char* name,uintptr_t pointer) {
    line.Add(" %s=%08lX",name,static_cast<unsigned long>(pointer));
}
static void ModelTable(Line& line,const char* name,uintptr_t table) {
    if(!IsValidUserPtr(table)) return;
    uint32_t model=0;
    if(Read(table,0x840,model)) line.Add(" %s_model=%lu",name,static_cast<unsigned long>(model));
    char text[16]{};
    if(!Read(table,0,text) || !std::memchr(text,0,sizeof(text))) return;
    uint32_t hash=2166136261u;
    for(size_t i=0;i<sizeof(text) && text[i];++i) { hash^=static_cast<uint8_t>(text[i]);hash*=16777619u; }
    line.Add(" %s_name_fnv=%08lX",name,static_cast<unsigned long>(hash));
}
static void Ride(Line& line,const char* name,uintptr_t ride,bool appearance) {
    if(!IsValidUserPtr(ride)) return;
    uint32_t model=0,dummy[3]{};uint8_t variant=0;
    if(Read(ride,0,model)) line.Add(" %s_model=%lu",name,static_cast<unsigned long>(model));
    if(Read(ride,7,variant)) line.Add(" %s_variant=%u",name,static_cast<unsigned>(variant));
    if(Read(ride,0x584,dummy)) line.Add(" %s_dummy=%08lX/%08lX/%08lX",name,
        static_cast<unsigned long>(dummy[0]),static_cast<unsigned long>(dummy[1]),static_cast<unsigned long>(dummy[2]));
    if(appearance) {
        uint8_t portable[0x940]{};
        if(ReadNativeAppearancePortable(ride,portable)) line.Add(" %s_appearance=%016llX",name,
            static_cast<unsigned long long>(ComputePeerAppearanceFingerprint(portable)));
    }
}
struct Resource { const char* status="unknown";uintptr_t node=0; };
static Resource FindResource(uint32_t id) {
    if(!id) return {"zero",0};
    const uintptr_t head=addresses.loader+0x58;
    if(head<addresses.loader || !IsValidUserPtr(head)) return {"unreadable",0};
    uintptr_t node=0,tail=0;
    if(!Read(head,0,node) || !Read(head,4,tail)) return {"unreadable",0};
    uintptr_t previous=head,seen[128]{},found=0;
    size_t visited=0;
    while(node!=head && visited<128) {
        if(!IsValidUserPtr(node)) return {"unreadable",0};
        for(size_t i=0;i<visited;++i) if(seen[i]==node) return {"cycle",0};
        seen[visited++]=node;
        uintptr_t next=0,back=0;uint32_t observed=0;
        if(!Read(node,0,next) || !Read(node,4,back) || !Read(node,0x123C,observed)) return {"unreadable",0};
        if(back!=previous || next==node) return {"bad_links",0};
        if(observed==id) {
            if(found) return {"duplicate_id",0};
            found=node;
        }
        previous=node;node=next;
    }
    if(node!=head) return {"limit",0};
    if(tail!=previous) return {"bad_links",0};
    return {found?"found":"missing",found};
}
static void LoadedResource(Line& line,uint32_t id) {
    line.Add(" resource_id=%08lX",static_cast<unsigned long>(id));
    const auto resource=FindResource(id);line.Add(" resource=%s",resource.status);
    if(!resource.node) return;
    Pointer(line,"resource_node",resource.node);
    uint32_t state=0;
    if(Read(resource.node,0x0C,state)) line.Add(" resource_state=%lu",static_cast<unsigned long>(state));
    // 637840 copies its RideInfo to node+20 and computes node+910 from that
    // ride's model. +123C is the ID returned by 63BC70, not a raw pointer.
    Ride(line,"loaded",resource.node+0x20,false);
    uintptr_t table=0;
    if(Read(resource.node,0x910,table)) { Pointer(line,"loaded_table",table);ModelTable(line,"loaded_table",table); }
}
static void Vehicle(const char* label,uintptr_t car,uintptr_t expected,uint32_t ordinal,uint32_t expectedModel,bool bindingMatches) {
    Line line;line.Add("[AI_VISUAL_CAR] actor=%s ordinal=%lu binding_match=%u",label,
        static_cast<unsigned long>(ordinal),bindingMatches?1u:0u);
    Pointer(line,"car",car);Pointer(line,"expected_param",expected);
    if(!IsValidUserPtr(car)) { line.Add(" car=unavailable");line.Finish();return; }
    uintptr_t parameter=0,ride=0,table=0,visual=0;
    const bool haveParameter=Read(car,0x14,parameter),haveRide=Read(car,0x10,ride);
    if(haveParameter) Pointer(line,"param",parameter);
    if(haveRide) { Pointer(line,"ride",ride);Ride(line,"actual",ride,true); }
    if(Read(car,0x0C,table)) { Pointer(line,"car_table",table);ModelTable(line,"car_table",table); }
    if(IsValidUserPtr(parameter)) {
        uint32_t type=0,model=0,id=0;uint8_t driver=0,remote=0;
        if(Read(parameter,4,type)) line.Add(" type=%lu",static_cast<unsigned long>(type));
        if(Read(parameter,1,driver)) line.Add(" driver=%u",static_cast<unsigned>(driver));
        if(Read(parameter,0x900,remote)) line.Add(" remote=%u",static_cast<unsigned>(remote));
        if(Read(parameter,0x10,model)) line.Add(" param_model=%lu",static_cast<unsigned long>(model));
        if(ordinal) line.Add(" expected_model=%lu",static_cast<unsigned long>(expectedModel));
        if(haveRide) line.Add(" ride_owns_param=%u",ride==parameter+0x10?1u:0u);
        if(Read(parameter,0x8F8,id)) LoadedResource(line,id);
    }
    if(Read(car,0x554,visual)) {
        Pointer(line,"visual",visual);
        if(IsValidUserPtr(visual)) {
            uintptr_t visualRide=0,visualTable=0;
            if(Read(visual,4,visualRide)) {
                Pointer(line,"visual_ride",visualRide);Ride(line,"visual",visualRide,true);
                if(haveRide) line.Add(" visual_owns_ride=%u",visualRide==ride?1u:0u);
            }
            if(Read(visual,8,visualTable)) { Pointer(line,"visual_table",visualTable);ModelTable(line,"visual_table",visualTable); }
        }
    }
    line.Finish();
}
} // namespace CoopAIVisualTrace

void TraceCoopAIVisual(DWORD flow) {
    using namespace CoopAIVisualTrace;
    // Disabled/SP/menu paths do not touch any native address. Ownership and
    // the runtime identity must agree before inspecting registered objects.
    if(flow!=6 || !gCoopAIEnabled || !IsCoopGameplayActive()) { haveAt=false;return; }
    const auto session=CoopSnapshot();uint32_t track=0;coop_world::Identity scope{};
    if(!ReadGameMemory(gRaceContextAddresses.track,&track,4) ||
        !CoopAIWorldRuntime::Scope(session,track,scope) || scope!=CoopAIWorldRuntime::identity ||
        session.role!=CoopAIWorldRuntime::ownerRole) { haveAt=false;return; }
    const DWORD now=GetTickCount();
    if(haveAt && scope==lastScope && session.role==lastRole && static_cast<DWORD>(now-lastAt)<5000) return;
    lastScope=scope;lastRole=session.role;lastAt=now;haveAt=true;
    Line summary;summary.Add("[AI_VISUAL] role=%s session=%016llX track=%lu token=%lu event=%08lX race_count=%lu",
        session.role==coop::Role::Host?"host":"client",static_cast<unsigned long long>(scope.sessionId),
        static_cast<unsigned long>(track),static_cast<unsigned long>(scope.raceEpoch),
        static_cast<unsigned long>(gPendingRaceEvent.careerEventHash),static_cast<unsigned long>(CoopAIRosterRaceCount()));
    summary.Finish();
    Vehicle("human_local",CoopRaceCar(false),0,0,0,true);
    Vehicle("human_peer",CoopRaceCar(true),0,0,0,true);
    for(auto& binding:CoopAIWorldRuntime::bindings) {
        if(!binding.car) continue;
        const bool matches=CoopAIWorldRuntime::Matches(binding);
        Vehicle("race_ai",binding.car,binding.parameter,binding.ordinal,binding.model,matches);
    }
}
