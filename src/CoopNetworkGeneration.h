// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <cstdint>

// Network identity is supplied by the joined session, never inferred from an
// IP being loopback, a local file, an uptime or a previously spawned vehicle.
namespace coop_network {
struct Generation {
    std::uint64_t session=0;
    std::uint32_t membership=0,ticket=0;
    bool Valid() const {return session&&membership;}
    friend bool operator==(const Generation& a,const Generation& b){return a.session==b.session&&a.membership==b.membership&&a.ticket==b.ticket;}
    friend bool operator!=(const Generation& a,const Generation& b){return !(a==b);}
};
inline bool Newer(std::uint32_t value,std::uint32_t previous){const auto delta=value-previous;return delta&&delta<0x80000000u;}
class SerialGate {
public:
    bool Accept(Generation incoming,Generation live,std::uint32_t serial){
        if(!live.Valid()||incoming!=live||!serial)return false;
        if(generation_!=live){generation_=live;seen_=false;}
        if(seen_&&!Newer(serial,serial_))return false;
        serial_=serial;seen_=true;return true;
    }
    void Clear(){generation_={};serial_=0;seen_=false;}
private:
    Generation generation_{};std::uint32_t serial_=0;bool seen_=false;
};
}
