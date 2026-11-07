// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <atomic>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace coop_discord_config {
inline std::atomic<uint64_t> applicationId{0};

inline bool SetApplicationId(std::string_view value) {
    uint64_t parsed=0;
    if(!value.empty()) {
        if(value.size()>20 || value.front()<'1' || value.front()>'9')return false;
        const auto end=value.data()+value.size();
        const auto result=std::from_chars(value.data(),end,parsed);
        if(result.ec!=std::errc{} || result.ptr!=end)return false;
    }
    applicationId.store(parsed,std::memory_order_release);
    return true;
}

inline uint64_t GetApplicationId() {
    return applicationId.load(std::memory_order_acquire);
}
}
