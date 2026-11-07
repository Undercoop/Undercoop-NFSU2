// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <cstdint>

namespace coop_motion {
// Convert a monotonic performance counter without ever comparing two PCs'
// uptimes. Split before multiplication; the public millisecond counter wraps
// exactly like the sequence/timestamp fields already used by the protocol.
inline bool CounterMilliseconds(std::uint64_t counter,std::uint64_t origin,
    std::uint64_t frequency,std::uint32_t originMs,std::uint32_t& output) {
    if(!frequency || counter<origin) return false;
    const auto elapsed=counter-origin;
    const auto whole=static_cast<std::uint32_t>(elapsed/frequency);
    const auto fraction=elapsed%frequency;
    // QPC frequencies are well below this guard. Keep the helper defined for
    // arbitrary/fuzzed counter metadata as well.
    if(fraction>UINT64_MAX/1000) return false;
    output=originMs+whole*1000U+static_cast<std::uint32_t>((fraction*1000)/frequency);
    return true;
}
}

#if defined(_WIN32)
#include <windows.h>
namespace coop_motion {
inline std::uint32_t MotionMilliseconds() {
    struct Anchor {
        LARGE_INTEGER frequency{},counter{};
        DWORD uptime=GetTickCount();
        Anchor() {
            if(!QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0 ||
                !QueryPerformanceCounter(&counter)) frequency.QuadPart=0;
        }
    };
    static const Anchor anchor;
    LARGE_INTEGER counter{};std::uint32_t now=0;
    if(!anchor.frequency.QuadPart || !QueryPerformanceCounter(&counter) ||
        !CounterMilliseconds(static_cast<std::uint64_t>(counter.QuadPart),
            static_cast<std::uint64_t>(anchor.counter.QuadPart),
            static_cast<std::uint64_t>(anchor.frequency.QuadPart),anchor.uptime,now)) return GetTickCount();
    return now;
}
}
#endif
