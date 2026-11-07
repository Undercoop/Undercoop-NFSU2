// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <cstdint>
namespace coop_time {
// Local duration only. A caller can acquire a lock after another producer has
// timestamped a publication. That small future sample is age zero, not 49 days.
// Valid intervals must be below the uint32 half range, as all protocol timers are.
inline std::uint32_t Elapsed(std::uint32_t now,std::uint32_t then){
    const auto value=now-then;return value<0x80000000u?value:0;
}
}
