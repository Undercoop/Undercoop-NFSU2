// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Pointer-free, personal-data-free career replication for SPEED2.EXE v1.2.
// PlayerWrite 52A280 / PlayerRead 5344D0: GarageWrite 5166B0 produces
// 0x993E bytes, then CareerWrite 514530 / CareerRead 529230 produces 0x2AE5.
// StageWrite 500FA0 / StageRead 514400 use a 0x101 serialized stride (live104).
// Completion 513390 writes ONLY the 35/4/10 event-hash groups below;
// 500B80 tests event+6 == 1; 532C60 and 528320 complete/advance stages.
// Cash +7134, sponsor choices stage+E8, personal ratings stage+E0/E4,
// active event, car ownership/customization, profile identity/options/keymaps
// are deliberately absent from the wire format and untouched by merging.
namespace coop_career {
constexpr size_t kNativePayloadSize = 0xD692;
constexpr size_t kCareerOffset = 0x993E;
constexpr size_t kStageCount = 8, kStageStride = 0x101;
constexpr size_t kStageHashBytes = 0xC4, kStageCompletedOffset = 0xFC;
constexpr size_t kStageHashCount = kStageHashBytes / 4;
constexpr size_t kStageIndexOffset = 0xA151;
constexpr size_t kEventCountOffset = 0xA155, kEventsOffset = 0xA159;
constexpr size_t kEventStride = 8, kMaximumEvents = 384;
constexpr size_t kCashOffset = 0xA146;
constexpr size_t kActiveEventOffset = 0xC41F;
constexpr size_t kPersonalNameOffset = 0xD201;
constexpr uint32_t kPersonalEventA = 0xDD60E402, kPersonalEventB = 0xDD60E403;
constexpr uint32_t kMagic = 0x5032434E; // NC2P; distinct from native MC02.
constexpr uint16_t kVersion = 3, kSpecialVersion = 4, kHeaderSize = 56;
constexpr size_t kWireStageSize = kStageHashBytes + 1;
constexpr size_t kWireBaseSize = kHeaderSize + kStageCount * kWireStageSize;
constexpr size_t kMinimumWireSize = 40 + kStageCount * kWireStageSize;
constexpr size_t kMaximumOffers = 64;
constexpr size_t kMaximumWireSize = kWireBaseSize + kMaximumEvents * 8 + kMaximumOffers * 4;
constexpr size_t kSpecialSlots=6,kSpecialOffset=0xC4,kOutrunOffset=0xDC;
constexpr size_t kSpecialWireBytes=kStageCount*(kSpecialSlots*4+4);
constexpr size_t kMaximumExtendedWireSize=kMaximumWireSize+kSpecialWireBytes;
constexpr std::array<size_t, 3> kGroupSizes{{35, 4, 10}};

struct Outcome {
    uint32_t eventHash = 0, epoch = 0, ticks = 0;
    uint16_t status = 0;
    uint8_t rank = 0, reserved = 0;
};
struct Stage {
    std::array<uint32_t, kStageHashCount> events{};
    std::array<uint32_t,kSpecialSlots> specials{};
    uint32_t outrunWins=0;
    bool completed = false;
};
struct Progress {
    std::array<Stage, kStageCount> stages{};
    uint32_t stageIndex = 0;
    std::vector<uint32_t> completedEvents;
    std::vector<uint32_t> discoveredEvents; // Native entry+4==1 only, never inferred from completion.
    Outcome lastResult{}; // Receipt/display metadata; never changes money.
    // 527BB0 randomly chooses native offers; 514530 does NOT save that cache.
    // Persist its IDs, never process-local catalog pointers. Only Host authors it.
    bool offersKnown = false;
    uint8_t offerStage = 0;
    uint32_t offerRevision = 0, offerBasis = 0;
    std::vector<uint32_t> offeredEvents; // Includes native hidden slots, not visibility overrides.
    bool specialsKnown=false; // v4 extension; no personal unique-part selection.
};

inline uint16_t Read16(const uint8_t* p) {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}
inline uint32_t Read32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void Write16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
}
inline void Write32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline bool PersonalEvent(uint32_t hash) { return hash == kPersonalEventA || hash == kPersonalEventB; }
inline bool Contains(const std::vector<uint32_t>& values, uint32_t hash) {
    return std::find(values.begin(), values.end(), hash) != values.end();
}
inline bool ValidOutcome(const Outcome& o) {
    const bool empty = !o.eventHash && !o.epoch && !o.ticks && !o.status && !o.rank && !o.reserved;
    // Native Finish status is an opaque nonzero uint16, NOT a new enum.
    return empty || (o.eventHash && !PersonalEvent(o.eventHash) && o.epoch &&
        o.status && o.rank >= 1 && o.rank <= 22 && !o.reserved);
}
inline bool ValidStage(const Stage& stage, bool allowPersonal = false) {
    for (size_t i = 0; i < stage.events.size(); ++i) {
        const uint32_t hash = stage.events[i];
        if (!hash) continue;
        if (!allowPersonal && PersonalEvent(hash)) return false;
        for (size_t j = 0; j < i; ++j) if (stage.events[j] == hash) return false;
    }
    if(stage.outrunWins>1000000)return false;
    for(size_t i=0;i<stage.specials.size();++i){const auto hash=stage.specials[i];if(!hash)continue;
        if(PersonalEvent(hash)||std::find(stage.events.begin(),stage.events.end(),hash)!=stage.events.end())return false;
        for(size_t j=0;j<i;++j)if(stage.specials[j]==hash)return false;}
    return true;
}
inline bool Valid(const Progress& p) {
    if (p.stageIndex >= kStageCount || p.completedEvents.size() > kMaximumEvents || p.discoveredEvents.size()>kMaximumEvents || !ValidOutcome(p.lastResult)) return false;
    for (const auto& stage : p.stages) {
        if (!ValidStage(stage)) return false;
        if(!p.specialsKnown&&(stage.outrunWins||std::any_of(stage.specials.begin(),stage.specials.end(),[](uint32_t h){return h!=0;})))return false;
    }
    for (size_t i = 0; i < p.completedEvents.size(); ++i) {
        const uint32_t hash = p.completedEvents[i];
        if (!hash || PersonalEvent(hash)) return false;
        for (size_t j = 0; j < i; ++j) if (p.completedEvents[j] == hash) return false;
    }
    for(size_t i=0;i<p.discoveredEvents.size();++i) {
        const uint32_t hash=p.discoveredEvents[i];if(!hash || PersonalEvent(hash))return false;
        for(size_t j=0;j<i;++j)if(p.discoveredEvents[j]==hash)return false;
    }
    if(p.offeredEvents.size()>kMaximumOffers || p.offerStage>=kStageCount ||
        (p.offersKnown?!p.offerRevision:(p.offerRevision || p.offerBasis || p.offerStage || !p.offeredEvents.empty())))return false;
    for(size_t i=0;i<p.offeredEvents.size();++i) {
        const uint32_t hash=p.offeredEvents[i];if(!hash || PersonalEvent(hash))return false;
        for(size_t j=0;j<i;++j)if(p.offeredEvents[j]==hash)return false;
    }
    return true;
}
inline void ClearOffers(Progress& p) {
    p.offersKnown=false;p.offerStage=0;p.offerRevision=p.offerBasis=0;p.offeredEvents.clear();
}
inline uint32_t OfferBasis(const Progress& p) {
    // Set-order independent: importing the same facts in opposite directions
    // must not silently reroll this campaign's offered circuit/drift races.
    uint32_t digest=2166136261u;
    auto mix=[&](uint32_t value){for(unsigned i=0;i<4;++i){digest=(digest^uint8_t(value))*16777619u;value>>=8;}};
    mix(p.stageIndex);
    for(const auto& stage:p.stages) {
        mix(stage.completed);auto hashes=stage.events;std::sort(hashes.begin(),hashes.end());
        for(uint32_t hash:hashes)mix(hash);
    }
    for(const auto* list:{&p.completedEvents,&p.discoveredEvents}) {
        auto hashes=*list;std::sort(hashes.begin(),hashes.end());mix(uint32_t(hashes.size()));
        for(uint32_t hash:hashes)mix(hash);
    }
    return digest;
}
inline bool ValidNativeEvents(const std::vector<uint8_t>& payload, uint32_t& count) {
    if (payload.size() != kNativePayloadSize) return false;
    count = Read32(payload.data() + kEventCountOffset);
    if (count > kMaximumEvents) return false;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t hash = Read32(payload.data() + kEventsOffset + i * kEventStride);
        if (!hash) return false;
        for (size_t j = 0; j < i; ++j) {
            if (Read32(payload.data() + kEventsOffset + j * kEventStride) == hash) return false;
        }
    }
    return true;
}
inline bool Completed(const std::vector<uint8_t>& payload, uint32_t hash) {
    uint32_t count = 0;
    if (!hash || !ValidNativeEvents(payload, count)) return false;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* entry = payload.data() + kEventsOffset + i * kEventStride;
        if (Read32(entry) == hash) return entry[6] == 1;
    }
    return false;
}
inline bool Extract(const std::vector<uint8_t>& payload, Progress& out,
    bool (*discoveryEligible)(uint32_t) = nullptr,bool includeSpecials=false) {
    uint32_t count = 0;
    if (!ValidNativeEvents(payload, count)) return false;
    Progress next;next.specialsKnown=includeSpecials;
    next.stageIndex = Read32(payload.data() + kStageIndexOffset);
    if (next.stageIndex >= kStageCount) return false;
    for (size_t i = 0; i < kStageCount; ++i) {
        const uint8_t* src = payload.data() + kCareerOffset + i * kStageStride;
        if (src[kStageCompletedOffset] > 1) return false;
        auto& stage = next.stages[i];
        stage.completed = src[kStageCompletedOffset] != 0;
        for (size_t j = 0; j < kStageHashCount; ++j) {
            const uint32_t hash = Read32(src + j * 4);
            // A/B are player-local onboarding milestones, even if a native
            // stage list ever contains them. Keep them off the shared wire.
            stage.events[j] = PersonalEvent(hash) ? 0 : hash;
        }
        if (!ValidStage(stage)) return false;
        if(includeSpecials){for(size_t j=0;j<kSpecialSlots;++j)stage.specials[j]=Read32(src+kSpecialOffset+j*4);
            stage.outrunWins=Read32(src+kOutrunOffset);if(!ValidStage(stage))return false;}
    }
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* entry = payload.data() + kEventsOffset + i * kEventStride;
        const uint32_t hash = Read32(entry);
        if (entry[6] == 1 && !PersonalEvent(hash)) next.completedEvents.push_back(hash);
        if (entry[4] == 1 && !PersonalEvent(hash) && (!discoveryEligible || discoveryEligible(hash)))
            next.discoveredEvents.push_back(hash);
    }
    if (!Valid(next)) return false;
    out = std::move(next);
    return true;
}

inline bool Combine(const Progress& a, const Progress& b, Progress& out) {
    if (!Valid(a) || !Valid(b)) return false;
    auto next = a; next.stageIndex = (std::max)(a.stageIndex, b.stageIndex);next.specialsKnown=a.specialsKnown||b.specialsKnown;
    for (size_t stage = 0; stage < kStageCount; ++stage) {
        auto& target = next.stages[stage]; target.completed = target.completed || b.stages[stage].completed;
        size_t start = 0;
        for (size_t capacity : kGroupSizes) {
            auto first = target.events.begin() + start, end = first + capacity;
            for (size_t i = start; i < start + capacity; ++i) {
                const uint32_t hash = b.stages[stage].events[i]; if (!hash) continue;
                if (std::find(first, end, hash) != end) continue;
                if (std::find(target.events.begin(), target.events.end(), hash) != target.events.end()) return false;
                auto empty = std::find(first, end, uint32_t(0)); if (empty == end) return false; *empty = hash;
            }
            start += capacity;
        }
        if(b.specialsKnown){for(uint32_t hash:b.stages[stage].specials){if(!hash||std::find(target.specials.begin(),target.specials.end(),hash)!=target.specials.end())continue;
            auto empty=std::find(target.specials.begin(),target.specials.end(),uint32_t(0));if(empty==target.specials.end())return false;*empty=hash;}
            target.outrunWins=(std::max)(target.outrunWins,b.stages[stage].outrunWins);}
    }
    for (uint32_t hash : b.completedEvents) if (!Contains(next.completedEvents, hash)) {
        if (next.completedEvents.size() == kMaximumEvents) return false; next.completedEvents.push_back(hash);
    }
    for (uint32_t hash : b.discoveredEvents) if (!Contains(next.discoveredEvents, hash)) {
        if (next.discoveredEvents.size() == kMaximumEvents) return false; next.discoveredEvents.push_back(hash);
    }
    if (b.lastResult.eventHash) next.lastResult = b.lastResult;
    if(b.offersKnown && (!next.offersKnown || b.offerRevision>next.offerRevision)) {
        next.offersKnown=true;next.offerStage=b.offerStage;next.offerRevision=b.offerRevision;
        next.offerBasis=b.offerBasis;next.offeredEvents=b.offeredEvents;
    } else if(b.offersKnown && next.offersKnown && b.offerRevision==next.offerRevision &&
        (b.offerStage!=next.offerStage || b.offerBasis!=next.offerBasis || b.offeredEvents!=next.offeredEvents))return false;
    if (!Valid(next)) return false; out = std::move(next); return true;
}

// FNV journal/transfer digest, not authentication. Session/ticket/sender
// ownership must be validated by the caller before this policy is invoked.
inline uint32_t Digest(const std::vector<uint8_t>& blob) {
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < blob.size(); ++i) {
        if (i >= 12 && i < 16) continue;
        value ^= blob[i]; value *= 16777619u;
    }
    return value;
}
inline bool Encode(const Progress& progress, std::vector<uint8_t>& blob) {
    if (!Valid(progress)) return false;
    std::vector<uint8_t> next(kWireBaseSize+(progress.specialsKnown?kSpecialWireBytes:0) + (progress.completedEvents.size()+progress.discoveredEvents.size()+progress.offeredEvents.size()) * 4, 0);
    Write32(next.data(), kMagic); Write16(next.data() + 4, progress.specialsKnown?kSpecialVersion:kVersion); Write16(next.data() + 6, kHeaderSize);
    Write32(next.data() + 8, uint32_t(next.size()));
    Write32(next.data() + 16, progress.stageIndex);
    Write32(next.data() + 20, uint32_t(progress.completedEvents.size()));
    Write32(next.data() + 24, progress.lastResult.eventHash);
    Write32(next.data() + 28, progress.lastResult.epoch);
    Write32(next.data() + 32, progress.lastResult.ticks);
    Write16(next.data() + 36, progress.lastResult.status);
    next[38] = progress.lastResult.rank; next[39] = progress.lastResult.reserved;
    Write32(next.data()+40,uint32_t(progress.discoveredEvents.size()));
    Write32(next.data()+44,progress.offerRevision);Write32(next.data()+48,progress.offerBasis);
    Write16(next.data()+52,uint16_t(progress.offeredEvents.size()));next[54]=uint8_t(progress.offersKnown);next[55]=progress.offerStage;
    size_t cursor = kHeaderSize;
    for (const auto& stage : progress.stages) {
        for (uint32_t hash : stage.events) { Write32(next.data() + cursor, hash); cursor += 4; }
        next[cursor++] = stage.completed ? 1 : 0;
    }
    if(progress.specialsKnown)for(const auto& stage:progress.stages){
        for(uint32_t hash:stage.specials){Write32(next.data()+cursor,hash);cursor+=4;}
        Write32(next.data()+cursor,stage.outrunWins);cursor+=4;}
    for (uint32_t hash : progress.completedEvents) { Write32(next.data() + cursor, hash); cursor += 4; }
    for (uint32_t hash : progress.discoveredEvents) { Write32(next.data() + cursor, hash); cursor += 4; }
    for (uint32_t hash : progress.offeredEvents) { Write32(next.data() + cursor, hash); cursor += 4; }
    Write32(next.data() + 12, Digest(next));
    blob = std::move(next);
    return true;
}
inline bool Decode(const std::vector<uint8_t>& blob, Progress& progress) {
    if (blob.size() < kMinimumWireSize || blob.size() > kMaximumExtendedWireSize ||
        Read32(blob.data()) != kMagic ||
        !(((Read16(blob.data()+4)==kVersion||Read16(blob.data()+4)==kSpecialVersion) && Read16(blob.data()+6)==kHeaderSize) ||
          (Read16(blob.data()+4)==2 && Read16(blob.data()+6)==44) || (Read16(blob.data()+4)==1 && Read16(blob.data()+6)==40)) ||
        Read32(blob.data() + 8) != blob.size() ||
        Read32(blob.data() + 12) != Digest(blob)) return false;
    const bool extended=Read16(blob.data()+4)==kSpecialVersion;
    const size_t header=Read16(blob.data()+6),base=header+kStageCount*kWireStageSize+(extended?kSpecialWireBytes:0);
    const uint32_t count = Read32(blob.data() + 20);
    const uint32_t found=header>=44?Read32(blob.data()+40):0;
    const uint32_t offers=header==kHeaderSize?Read16(blob.data()+52):0;
    if (count > kMaximumEvents || found>kMaximumEvents || offers>kMaximumOffers || blob.size() != base + (size_t(count)+found+offers) * 4) return false;
    Progress next;next.specialsKnown=extended;
    next.stageIndex = Read32(blob.data() + 16);
    next.lastResult = {Read32(blob.data() + 24), Read32(blob.data() + 28), Read32(blob.data() + 32),
        Read16(blob.data() + 36), blob[38], blob[39]};
    if(header==kHeaderSize) {
        if(blob[54]>1)return false;
        next.offerRevision=Read32(blob.data()+44);next.offerBasis=Read32(blob.data()+48);
        next.offersKnown=blob[54]!=0;next.offerStage=blob[55];
    }
    size_t cursor = header;
    for (auto& stage : next.stages) {
        for (auto& hash : stage.events) { hash = Read32(blob.data() + cursor); cursor += 4; }
        if (blob[cursor] > 1) return false;
        stage.completed = blob[cursor++] != 0;
    }
    if(extended)for(auto& stage:next.stages){for(auto& hash:stage.specials){hash=Read32(blob.data()+cursor);cursor+=4;}
        stage.outrunWins=Read32(blob.data()+cursor);cursor+=4;}
    next.completedEvents.reserve(count);
    for (size_t i = 0; i < count; ++i) { next.completedEvents.push_back(Read32(blob.data() + cursor)); cursor += 4; }
    for (size_t i = 0; i < found; ++i) { next.discoveredEvents.push_back(Read32(blob.data() + cursor)); cursor += 4; }
    for (size_t i = 0; i < offers; ++i) { next.offeredEvents.push_back(Read32(blob.data() + cursor)); cursor += 4; }
    if (!Valid(next)) return false;
    progress = std::move(next);
    return true;
}

inline bool MergePayload(const std::vector<uint8_t>& localPayload, const Progress& incoming,
    std::vector<uint8_t>& mergedPayload) {
    uint32_t count = 0;
    if (!Valid(incoming) || !ValidNativeEvents(localPayload, count)) return false;
    const uint32_t localStageIndex = Read32(localPayload.data() + kStageIndexOffset);
    if (localStageIndex >= kStageCount) return false;
    std::vector<uint8_t> next = localPayload;
    // Determine onboarding from the LOCAL payload, never from the sender.
    if (Completed(localPayload, kPersonalEventB)) {
        for (size_t i = 0; i < kStageCount; ++i) {
            uint8_t* target = next.data() + kCareerOffset + i * kStageStride;
            Stage local;
            if (target[kStageCompletedOffset] > 1) return false;
            for (size_t j = 0; j < kStageHashCount; ++j) local.events[j] = Read32(target + j * 4);
            if (!ValidStage(local, true)) return false;
            size_t start = 0;
            for (size_t capacity : kGroupSizes) {
                for (size_t j = start; j < start + capacity; ++j) {
                    const uint32_t hash = incoming.stages[i].events[j];
                    if (!hash) continue;
                    auto first = local.events.begin() + start, end = first + capacity;
                    if (std::find(first, end, hash) != end) continue;
                    // A hash cannot change native event category while merging.
                    if (std::find(local.events.begin(), local.events.end(), hash) != local.events.end()) return false;
                    auto empty = std::find(first, end, uint32_t(0));
                    if (empty == end) return false; // Bounded set union, no partial output.
                    *empty = hash;
                }
                start += capacity;
            }
            for (size_t j = 0; j < kStageHashCount; ++j) Write32(target + j * 4, local.events[j]);
            if(incoming.specialsKnown){
                for(size_t j=0;j<kSpecialSlots;++j)local.specials[j]=Read32(target+kSpecialOffset+j*4);
                local.outrunWins=Read32(target+kOutrunOffset);if(!ValidStage(local,true))return false;
                for(uint32_t hash:incoming.stages[i].specials){if(!hash||std::find(local.specials.begin(),local.specials.end(),hash)!=local.specials.end())continue;
                    auto empty=std::find(local.specials.begin(),local.specials.end(),uint32_t(0));if(empty==local.specials.end())return false;*empty=hash;}
                if(!ValidStage(local,true))return false;
                for(size_t j=0;j<kSpecialSlots;++j)Write32(target+kSpecialOffset+j*4,local.specials[j]);
                Write32(target+kOutrunOffset,(std::max)(local.outrunWins,incoming.stages[i].outrunWins));
            }
            target[kStageCompletedOffset] = uint8_t(target[kStageCompletedOffset] || incoming.stages[i].completed);
        }
        Write32(next.data() + kStageIndexOffset, (std::max)(localStageIndex, incoming.stageIndex));
    }
    for (uint32_t hash : incoming.completedEvents) {
        size_t index = 0;
        for (; index < count; ++index) {
            if (Read32(next.data() + kEventsOffset + index * kEventStride) == hash) break;
        }
        if (index == count) {
            if (count == kMaximumEvents) return false;
            uint8_t* entry = next.data() + kEventsOffset + size_t(count) * kEventStride;
            std::fill(entry, entry + kEventStride, uint8_t(0));
            Write32(entry, hash); entry[6] = 1; ++count;
        } else {
            uint8_t* entry = next.data() + kEventsOffset + index * kEventStride;
            // Do not overwrite native active/loading/results-pending status:
            // runtime also waits for this player's native payout before merge.
            if (entry[6] != 2 && entry[6] != 5) entry[6] = 1;
        }
    }
    for(uint32_t hash:incoming.discoveredEvents) {
        size_t index=0;for(;index<count;++index)if(Read32(next.data()+kEventsOffset+index*kEventStride)==hash)break;
        if(index==count) {
            if(count==kMaximumEvents)return false;
            uint8_t* entry=next.data()+kEventsOffset+size_t(count)*kEventStride;
            std::fill(entry,entry+kEventStride,uint8_t(0));Write32(entry,hash);++count;
        }
        // Share only genuine discovery. Do not import transient shop/race status,
        // inferred hidden-race completion, rewards, options or cash pickups.
        next[kEventsOffset+index*kEventStride+4]=1;
    }
    Write32(next.data() + kEventCountOffset, count);
    mergedPayload = std::move(next);
    return true;
}
} // namespace coop_career
