// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#ifndef COOP_PEER_LABEL_VISIBILITY_NATIVE_INCLUDED
#define COOP_PEER_LABEL_VISIBILITY_NATIVE_INCLUDED
#include "CoopPeerLabelVisibility.h"
namespace CoopPeerLabelVisibilityNative {
using Result=coop_peer_visibility::Result;
using Audit=coop_peer_visibility::Audit;
static bool Read(void*,uintptr_t address,void* output,size_t length){
    __try {std::memcpy(output,reinterpret_cast<const void*>(address),length);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool Supported(){
    // These are read-only identity checks, not hooks/calls to the old engine.
    static const uint8_t decode[]={0x83,0xEC,0x0C,0x0F,0xBF,0x41,0x10,0x0F,0xBF,0x51,0x04};
    static const uint8_t segment[]={0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x2C,0x8B,0x55,0x0C};
    uint8_t a[sizeof(decode)]{},b[sizeof(segment)]{};float scale=0;
    return Read(nullptr,0x5D2CC0,a,sizeof(a))&&Read(nullptr,0x5D36F0,b,sizeof(b))&&
        Read(nullptr,0x784278,&scale,sizeof(scale))&&scale==1.0f/65536.0f&&
        !std::memcmp(a,decode,sizeof(a))&&!std::memcmp(b,segment,sizeof(b));
}
// Game-thread only. Inputs and topology/pack reads must belong to the same
// current gameplay frame. Unknown MUST be treated as hidden, never as clear.
static Result Query(const float from[3],const float body[3],const float roof[3],uintptr_t localCar=0,Audit* audit=nullptr){
    if(audit)*audit={};
    if(!from||!body||!roof||!Supported())return Result::Unknown;
    const coop_peer_visibility::Reader memory{Read,nullptr};
    coop_peer_visibility::Addresses addresses{};addresses.localCar=localCar;
    return coop_peer_visibility::Query(memory,addresses,
        {from[0],from[1],from[2]},{body[0],body[1],body[2]},{roof[0],roof[1],roof[2]},audit);
}
}
#endif
