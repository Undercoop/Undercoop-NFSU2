// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "../src/CoopAnimationScene.h"
#include "../src/CoopAnimationModule.h"

static unsigned checks=0,failures=0;
static bool focused=true;
static std::map<uintptr_t,std::vector<uint8_t>> game;
static void Check(bool value,const char* text,int line){++checks;if(!value){++failures;std::printf("FAIL %d %s\n",line,text);}}
#define CHECK(...) Check(bool((__VA_ARGS__)),#__VA_ARGS__,__LINE__)
static bool IsGameWindowFocused(){return focused;}
static void LogMessage(const char*){}
static bool ReadGameMemory(uintptr_t at,void* out,size_t bytes){
    for(const auto& item:game)if(at>=item.first&&at-item.first<=item.second.size()&&bytes<=item.second.size()-(at-item.first)){
        std::memcpy(out,item.second.data()+at-item.first,bytes);return true;
    }
    // Engine globals are simulated. Never inspect a real running game.
    if(at==coop_animation_scene::kFlow||at==coop_animation_scene::kScene||at==coop_animation_scene::kPlayers)return false;
    SIZE_T read=0;return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),out,bytes,&read)&&read==bytes;
}
#include "../src/CoopAnimationControllerNative.inl"

template<class T> static void Put(uintptr_t at,const T& value){
    const auto* bytes=reinterpret_cast<const uint8_t*>(&value);game[at]={bytes,bytes+sizeof(value)};
}
static uint32_t InvokeInline(uintptr_t target,uint32_t part){
    return reinterpret_cast<uint32_t(__fastcall*)(uint32_t)>(target)(part);
}
int main(){
    namespace native=CoopAnimationControllerNative;
    native::Poll(); CHECK(!native::installed); // No plugin is loaded by this harness.
    constexpr size_t bytes=0x42000;
    auto* data=static_cast<uint8_t*>(VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!data)return 2;
    const auto base=reinterpret_cast<uintptr_t>(data);
    std::memset(data,0xCC,bytes);
    std::memcpy(data+0x36A0,coop_animation_module::kQuery.data(),5);
    std::memcpy(data+0x370C,coop_animation_module::kInline.data(),8);
    // Only test-owned return stubs execute. No AnimationController image is
    // mapped or initialized, and its original instructions are never called.
    const uint8_t frame[]={0x55,0x8B,0xEC,0x51,0x56,0x8B,0xF1};
    std::memcpy(data+0x36F0,frame,sizeof(frame));
    native::Jump(data+0x36F7,base+0x36F7,base+0x370C,5);
    const uint8_t load[]={0xF3,0x0F,0x10,0x05};
    std::memcpy(data+0x3738,load,sizeof(load));
    const uint32_t scalar=uint32_t(base+0x3DE04);
    std::memcpy(data+0x373C,&scalar,4);
    const float speed=1.0f;std::memcpy(data+0x3DE04,&speed,4);
    native::Jump(data+0x3740,base+0x3740,base+0x37F0,5);
    std::memcpy(data+0x37F0,coop_animation_module::kExit.data(),coop_animation_module::kExit.size());
    std::vector<uint8_t> before(data,data+bytes);
    DWORD old=0;CHECK(VirtualProtect(data,bytes,PAGE_EXECUTE_READ,&old));
    Put(coop_animation_scene::kFlow,uint32_t(0));
    std::atomic<bool> stop{false};
    std::thread witness([&]{while(!stop.load())Sleep(1);});
    CHECK(native::Apply(base)==native::PatchResult::Applied);
    stop=true;witness.join();
    MEMORY_BASIC_INFORMATION page{};CHECK(VirtualQuery(data+0x3000,&page,sizeof(page))==sizeof(page));
    CHECK(page.Protect==PAGE_EXECUTE_READ);
    CHECK(data[0x36A0]==0xE9&&data[0x370C]==0xE9);
    CHECK(data[0x3711]==0x90&&data[0x3712]==0x90&&data[0x3713]==0x90);
    bool unchanged=true;for(size_t i=0;i<before.size();++i)
        if(!(i>=0x36A0&&i<0x36A5)&&!(i>=0x370C&&i<0x3714)&&data[i]!=before[i])unchanged=false;
    CHECK(unchanged);
    const auto query=reinterpret_cast<uint32_t(__fastcall*)(uint32_t)>(base+0x36A0);
    CHECK(query(0)==2);CHECK(InvokeInline(base+0x36F0,0)==2);
    Put(coop_animation_scene::kFlow,uint32_t(3));
    Put(coop_animation_scene::kScene,uint32_t(0x120000));
    struct Links{uint32_t next,previous;};
    Put(0x120008,Links{0x120008,0x120008});
    CHECK(query(0)==1);CHECK(InvokeInline(base+0x36F0,0)==1);
    Put(0x120008,Links{0x130000,0x130000});
    struct Node{uint32_t next,previous,speed,part,state;};
    Put(0x130000,Node{0x120008,0x120008,0,0,0});
    CHECK(query(0)==0);CHECK(InvokeInline(base+0x36F0,0)==0);
    CHECK(query(1)==1);CHECK(InvokeInline(base+0x36F0,1)==1);
    CHECK(query(99)==2);CHECK(InvokeInline(base+0x36F0,99)==2);
    focused=false;CHECK(query(0)==2);CHECK(InvokeInline(base+0x36F0,0)==2);focused=true;
    std::vector<uint8_t> active(data,data+bytes);
    CHECK(native::Apply(base)==native::PatchResult::Changed);
    CHECK(std::memcmp(data,active.data(),active.size())==0);
    CHECK(VirtualProtect(data,bytes,PAGE_READWRITE,&old));
    std::memcpy(data,before.data(),before.size());data[0x370C]^=1;
    std::vector<uint8_t> incompatible(data,data+bytes);
    CHECK(VirtualProtect(data,bytes,PAGE_EXECUTE_READ,&old));
    CHECK(native::Apply(base)==native::PatchResult::Changed);
    CHECK(std::memcmp(data,incompatible.data(),incompatible.size())==0);
    CHECK(VirtualQuery(data+0x3000,&page,sizeof(page))==sizeof(page));CHECK(page.Protect==PAGE_EXECUTE_READ);
    CHECK(VirtualProtect(data,bytes,PAGE_READWRITE,&old));
    std::memcpy(data,before.data(),before.size());data[0x37F0]^=1;
    incompatible.assign(data,data+bytes);
    CHECK(VirtualProtect(data,bytes,PAGE_EXECUTE_READ,&old));
    CHECK(native::Apply(base)==native::PatchResult::Changed);
    CHECK(std::memcmp(data,incompatible.data(),incompatible.size())==0);
    CHECK(VirtualFree(data,0,MEM_RELEASE));
    std::printf("Animation native adapter: %u checks, %u failures; test-owned stubs only.\n",checks,failures);
    return failures?1:0;
}
