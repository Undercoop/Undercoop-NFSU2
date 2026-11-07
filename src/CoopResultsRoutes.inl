// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Included inside CoopResultsTrace after its bounded memory readers. Diagnostic
// reads only: native descriptor lookup51BB70 is NOT called (it relinks MRU).
static uint32_t NameHash(const char* text) {
    uint32_t hash=~uint32_t(0);const char* base=text;
    for(const char* p=text;*p;++p)if(*p=='\\')base=p+1;
    for(;*base;++base){unsigned char c=*base;if(c>='a'&&c<='z')c-=32;hash=hash*33+c;}
    return hash;
}
static void Events(const char* owner,uintptr_t head) {
    uintptr_t seen[24]{};char line[320];
    for(unsigned i=0;i<24&&IsValidUserPtr(head);++i) {
        bool repeated=false;for(unsigned j=0;j<i;++j)if(seen[j]==head)repeated=true;
        if(repeated){LogMessage("[RESULT_ROUTE] Event chain cycle; diagnostic traversal stopped.\r\n");break;}
        seen[i]=head;const uint32_t hash=Read<uint32_t>(head+0xC),count=Read<uint32_t>(head+0x10);
        const uintptr_t commands=Read<uintptr_t>(head+0x14);
        std::snprintf(line,sizeof(line),"[RESULT_ROUTE_EVENT] owner=%s index=%u node=%08X hash=%08X commands=%u array=%08X.\r\n",
            owner,i,unsigned(head),hash,count,unsigned(commands));LogMessage(line);
        if(count<=16 && IsValidUserPtr(commands))for(uint32_t j=0;j<count;++j) {
            const uintptr_t command=commands+j*12;
            std::snprintf(line,sizeof(line),"[RESULT_ROUTE_COMMAND] owner=%s event=%08X index=%u opcode=%u argument=%08X recipient=%08X.\r\n",
                owner,hash,j,Read<uint32_t>(command),Read<uint32_t>(command+4),Read<uint32_t>(command+8));LogMessage(line);
        }
        head=Read<uintptr_t>(head+4);
    }
}
static void RouteSnapshot(const char* reason) {
    const uintptr_t ui=Read<uintptr_t>(0x8384C4),engine=Read<uintptr_t>(ui+8),input=Read<uintptr_t>(0x8384D8);
    const uintptr_t manager=Read<uintptr_t>(kManager),page=Read<uintptr_t>(manager+0x38);
    char primary[64],fallback[64],current[64],line[512];
    Name(Read<uintptr_t>(input+0x58),primary);Name(Read<uintptr_t>(input+0x5C),fallback);Name(Read<uintptr_t>(page+8),current);
    const uintptr_t selected=Read<uintptr_t>(0x863470);
    std::snprintf(line,sizeof(line),
        "[RESULT_ROUTE] %s ms=%lu primary=%s fallback=%s current=%s allowed=%08X enabled=%u dispatchGate=%u lostFocus=%u selected=%08X event=%08X type=%u drift=%u queue=%u/%08X/%08X poll=%08X.\r\n",
        reason,GetTickCount(),primary,fallback,current,Read<uint32_t>(input+0x64),Read<uint8_t>(input+0x6C),
        Read<uint8_t>(0x838521),Read<uint8_t>(0x8709E0),unsigned(selected),Read<uint32_t>(selected+8),
        Read<uint8_t>(selected+0xF),Read<uint8_t>(0x89E7D9),Read<uint32_t>(engine+0x4118),
        unsigned(Read<uintptr_t>(engine+0x411C)),unsigned(Read<uintptr_t>(engine+0x4120)),unsigned(Read<uintptr_t>(engine+0x10C)));
    LogMessage(line);
    const uint32_t primaryHash=NameHash(primary),fallbackHash=NameHash(fallback),currentHash=NameHash(current);
    uintptr_t raw=Read<uintptr_t>(0x8637B4),registrySeen[256]{};unsigned matches=0;
    for(unsigned i=0;i<256 && IsValidUserPtr(raw) && raw!=0x8637B4;++i) {
        bool repeated=false;for(unsigned j=0;j<i;++j)if(registrySeen[j]==raw)repeated=true;if(repeated)break;
        registrySeen[i]=raw;const uintptr_t descriptor=raw-4,resource=Read<uintptr_t>(descriptor+0x10);
        const uint32_t tag=Read<uint32_t>(resource);uint32_t hash=0;
        if(tag==0x30210)hash=Read<uint32_t>(resource+8);
        else if(tag==0x30203) {
            const uintptr_t header=Read<uintptr_t>(descriptor+0xC);
            if(Read<uint32_t>(header)==0xE76E4546 && Read<uint32_t>(header+8)==0x64486B50 && Read<uint32_t>(header+0x10)>=0x20000) {
                char name[64];Name(header+0x28,name);if(name[0])hash=NameHash(name);
            }
        }
        if(hash && ((primary[0]&&hash==primaryHash)||(fallback[0]&&hash==fallbackHash)||(current[0]&&hash==currentHash))) {
            const uintptr_t bound=Read<uintptr_t>(descriptor+0x18),handler=Read<uintptr_t>(descriptor+0x14);
            char name[64];Name(Read<uintptr_t>(bound+0xC),name);
            std::snprintf(line,sizeof(line),
                "[RESULT_REGISTRY] match=%u order=%u descriptor=%08X tag=%08X hash=%08X handler=%08X bound=%08X(%s) boundDescriptor=%08X focus=%08X.\r\n",
                matches++,i,unsigned(descriptor),tag,hash,unsigned(handler),unsigned(bound),name,
                unsigned(Read<uintptr_t>(bound+0x40)),unsigned(Read<uintptr_t>(bound+0x8C)));LogMessage(line);
        }
        raw=Read<uintptr_t>(descriptor+4);
    }
    uintptr_t package=Read<uintptr_t>(engine+0xE4),seen[12]{};
    for(unsigned i=0;i<12&&IsValidUserPtr(package);++i) {
        bool repeated=false;for(unsigned j=0;j<i;++j)if(seen[j]==package)repeated=true;if(repeated)break;
        seen[i]=package;char name[64];Name(Read<uintptr_t>(package+0xC),name);
        const uint32_t hash=NameHash(name);
        if(name[0]&&((primary[0]&&hash==primaryHash)||(fallback[0]&&hash==fallbackHash)||(current[0]&&hash==currentHash))) {
            const uintptr_t focus=Read<uintptr_t>(package+0x8C);
            std::snprintf(line,sizeof(line),"[RESULT_FOCUS] package=%08X(%s) descriptor=%08X focus=%08X type=%u objectHash=%08X packageEvents=%08X focusEvents=%08X.\r\n",
                unsigned(package),name,unsigned(Read<uintptr_t>(package+0x40)),unsigned(focus),Read<uint32_t>(focus+0x18),
                Read<uint32_t>(focus+0x10),unsigned(Read<uintptr_t>(package+0x5C)),unsigned(Read<uintptr_t>(focus+0x3C)));LogMessage(line);
            Events("package",Read<uintptr_t>(package+0x5C));Events("focus",Read<uintptr_t>(focus+0x3C));
        }
        package=Read<uintptr_t>(package+4);
    }
}
