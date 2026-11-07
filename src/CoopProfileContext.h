// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Filesystem-independent profile context. Native callbacks must run on the game
// thread, while RoutePath may be called from native I/O threads. This module
// never opens, creates, deletes, copies, or replaces a file.
namespace coop_save {

constexpr size_t kNativeProfilePayloadSize = 0xD692;
constexpr size_t kNativeProfileFileSize = kNativeProfilePayloadSize + 0x24;

enum class Role { Host, Client };
enum class Domain { Singleplayer, Coop, Switching, Faulted };
enum class PathDecision { Unchanged, Redirect, Reject };

struct RoutedPath {
    PathDecision decision = PathDecision::Unchanged;
    std::wstring path;
};

struct NativeProfileSnapshot {
    // Produced by Player::WriteToSaveData (0x0052A280), not by copying
    // ProfileData's live object graph at 0x0083A9D0.
    std::vector<uint8_t> payload;
    // The adapter may encode only individually verified scalar/array metadata.
    // This is not a raw ProfileData memory image or a list of live pointers.
    std::vector<uint8_t> metadata;
};

struct NativeProfileAdapter {
    std::function<bool(NativeProfileSnapshot&)> captureSingleplayer;
    // Change the engine save-root context only; do not load or create profiles.
    std::function<bool(const std::wstring&)> setSaveRoot;
    std::function<bool(const NativeProfileSnapshot&)> restoreSingleplayer;
};

inline bool EqualPathText(const std::wstring& a, const std::wstring& b) {
#if defined(_WIN32)
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
#else
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
#endif
}

inline bool PathIsWithin(const std::wstring& path, const std::wstring& root) {
    if (path.size() < root.size() || !EqualPathText(path.substr(0, root.size()), root)) return false;
    return path.size() == root.size() || root.back() == L'\\' || path[root.size()] == L'\\';
}

inline std::wstring NormalizedPathSpelling(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.size() >= 8 && EqualPathText(path.substr(0, 8), L"\\\\?\\UNC\\")) {
        path = L"\\\\" + path.substr(8);
    } else if (path.size() >= 4 && path.substr(0, 4) == L"\\\\?\\") {
        path.erase(0, 4);
    }
    std::wstring collapsed;
    for (wchar_t ch : path) {
        if (ch == L'\\' && !collapsed.empty() && collapsed.back() == L'\\' &&
            collapsed.size() != 1) continue;
        collapsed.push_back(ch);
    }
    return collapsed;
}

inline bool IsReservedWindowsComponent(const std::wstring& value) {
    const std::wstring name = value.substr(0, value.find(L'.'));
    if (EqualPathText(name, L"CON") || EqualPathText(name, L"PRN") ||
        EqualPathText(name, L"AUX") || EqualPathText(name, L"NUL")) return true;
    if (name.size() == 4 && name[3] >= L'1' && name[3] <= L'9' &&
        (EqualPathText(name.substr(0, 3), L"COM") || EqualPathText(name.substr(0, 3), L"LPT"))) return true;
    return false;
}

// Lexically canonicalizes absolute drive/UNC Windows paths. The caller should
// configure roots obtained from the OS. Reparse points require a separate OS
// check before writing externally supplied checkpoint files.
inline bool CanonicalWindowsPath(const std::wstring& input, std::wstring& output,
    bool allowFinalWildcard = false) {
    output.clear();
    const std::wstring path = NormalizedPathSpelling(input);
    if (path.empty() || path.find(L'\0') != std::wstring::npos) return false;
    std::wstring prefix;
    size_t start = 0;
    size_t fixedParts = 0;
    const bool drive = path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && path[2] == L'\\';
    if (drive) {
        prefix = path.substr(0, 3);
        prefix[0] = static_cast<wchar_t>(std::towupper(prefix[0]));
        start = 3;
    } else if (path.size() > 2 && path.substr(0, 2) == L"\\\\" && path[2] != L'.' && path[2] != L'?') {
        prefix = L"\\\\";
        start = 2;
        fixedParts = 2; // UNC server/share cannot be escaped by '..'.
    } else {
        return false;
    }

    std::vector<std::wstring> components;
    while (start < path.size()) {
        const size_t slash = path.find(L'\\', start);
        const size_t end = slash == std::wstring::npos ? path.size() : slash;
        const std::wstring part = path.substr(start, end - start);
        start = slash == std::wstring::npos ? path.size() : slash + 1;
        if (part.empty() || part == L".") continue;
        if (part == L"..") {
            if (components.size() <= fixedParts) return false;
            components.pop_back();
            continue;
        }
        if (part.back() == L'.' || part.back() == L' ' || IsReservedWindowsComponent(part)) return false;
        for (wchar_t ch : part) {
            if (ch < 32 || ch == L':' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') return false;
            if ((ch == L'*' || ch == L'?') && (!allowFinalWildcard || start < path.size())) return false;
        }
        components.push_back(part);
    }
    if (components.size() < fixedParts) return false;
    output = prefix;
    for (const auto& part : components) {
        if (output.back() != L'\\') output.push_back(L'\\');
        output += part;
    }
    return true;
}

inline bool IsSafeProfileName(const std::wstring& name) {
    if (name.empty() || name.size() > 31 || name == L"." || name == L".." ||
        name.back() == L'.' || name.back() == L' ' || IsReservedWindowsComponent(name)) return false;
    for (wchar_t ch : name) {
        if (ch < 32 || ch == L'\\' || ch == L'/' || ch == L':' || ch == L'"' ||
            ch == L'<' || ch == L'>' || ch == L'|' || ch == L'*' || ch == L'?') return false;
    }
    return true;
}

inline uint32_t ReadLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

inline void WriteLe32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
    data[2] = static_cast<uint8_t>(value >> 16);
    data[3] = static_cast<uint8_t>(value >> 24);
}

// Matches 0x006FE173: EA's MSB-first CRC, seeded from the complement of the
// first four bytes in big-endian order, then complemented on completion.
// The native routine also uses this 256-entry table (SPEED2 1.2: 0x8145E0).
// Precompute exactly the former eight polynomial steps, not a different CRC
// variant. Every byte is still checked on every validation; nothing is cached.
constexpr std::array<uint32_t, 256> MakeEaCrc32Table() {
    std::array<uint32_t, 256> table{};
    for (size_t index = 0; index < table.size(); ++index) {
        uint32_t polynomial = static_cast<uint32_t>(index) << 24;
        for (unsigned bit = 0; bit < 8; ++bit) {
            polynomial = (polynomial & 0x80000000u)
                ? (polynomial << 1) ^ 0x04C11DB7u : polynomial << 1;
        }
        table[index] = polynomial;
    }
    return table;
}
inline constexpr auto kEaCrc32Table = MakeEaCrc32Table();
inline uint32_t EaCrc32(const uint8_t* data, size_t size) {
    if (!data || size < 4) return 0;
    uint32_t state = ~((static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) | (static_cast<uint32_t>(data[2]) << 8) | data[3]);
    for (size_t i = 4; i < size; ++i) {
        state = (state << 8 | data[i]) ^ kEaCrc32Table[state >> 24];
    }
    return ~state;
}

enum class SaveValidation { Valid, BadSize, BadMagic, BadLayout, BadHeaderCrc, BadChunkCrc };

inline SaveValidation ValidateNativeProfileFile(const uint8_t* data, size_t size) {
    if (!data || size != kNativeProfileFileSize) return SaveValidation::BadSize;
    if (ReadLe32(data) != 0x4D433032u) return SaveValidation::BadMagic;
    if (ReadLe32(data + 4) != size || ReadLe32(data + 8) != 8 ||
        ReadLe32(data + 0xC) != kNativeProfilePayloadSize ||
        ReadLe32(data + 0x1C) != 0x10D || ReadLe32(data + 0x20) != kNativeProfilePayloadSize) {
        return SaveValidation::BadLayout;
    }
    if (EaCrc32(data, 0x18) != ReadLe32(data + 0x18)) return SaveValidation::BadHeaderCrc;
    if (EaCrc32(data + 0x1C, 8) != ReadLe32(data + 0x10) ||
        EaCrc32(data + 0x24, kNativeProfilePayloadSize) != ReadLe32(data + 0x14)) {
        return SaveValidation::BadChunkCrc;
    }
    return SaveValidation::Valid;
}

inline bool EncodeNativeProfileFile(const std::vector<uint8_t>& payload, std::vector<uint8_t>& file) {
    file.clear();
    if (payload.size() != kNativeProfilePayloadSize) return false;
    file.resize(kNativeProfileFileSize, 0);
    WriteLe32(file.data(), 0x4D433032u);
    WriteLe32(file.data() + 4, static_cast<uint32_t>(file.size()));
    WriteLe32(file.data() + 8, 8);
    WriteLe32(file.data() + 0xC, static_cast<uint32_t>(payload.size()));
    WriteLe32(file.data() + 0x1C, 0x10D);
    WriteLe32(file.data() + 0x20, static_cast<uint32_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), file.begin() + 0x24);
    WriteLe32(file.data() + 0x10, EaCrc32(file.data() + 0x1C, 8));
    WriteLe32(file.data() + 0x14, EaCrc32(file.data() + 0x24, payload.size()));
    WriteLe32(file.data() + 0x18, EaCrc32(file.data(), 0x18));
    return true;
}

class ProfileContext {
public:
    bool Configure(const std::wstring& singleplayerRoot, const std::wstring& coopBaseRoot) {
        std::wstring sp, coop;
        if (!CanonicalWindowsPath(singleplayerRoot, sp) || !CanonicalWindowsPath(coopBaseRoot, coop) ||
            PathIsWithin(sp, coop) || PathIsWithin(coop, sp)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (domain_ != Domain::Singleplayer || hasSnapshot_) return false;
        singleplayerRoot_ = std::move(sp);
        coopBaseRoot_ = std::move(coop);
        configured_ = true;
        return true;
    }

    bool BeginCoopProfileSelection(Role role, bool peerJoined, const NativeProfileAdapter& adapter) {
        std::wstring coopRoot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!configured_ || !peerJoined) return false;
            if (domain_ == Domain::Coop) return role == role_;
            if (domain_ != Domain::Singleplayer || !adapter.captureSingleplayer || !adapter.setSaveRoot) return false;
            role_ = role;
            coopRoot = CoopRootUnlocked(role);
            domain_ = Domain::Switching;
        }
        NativeProfileSnapshot captured;
        if (!adapter.captureSingleplayer(captured) || captured.payload.size() != kNativeProfilePayloadSize) {
            std::lock_guard<std::mutex> lock(mutex_);
            domain_ = Domain::Singleplayer;
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_ = std::move(captured);
            hasSnapshot_ = true;
        }
        if (!adapter.setSaveRoot(coopRoot)) {
            const bool rolledBack = adapter.setSaveRoot(SingleplayerRoot());
            std::lock_guard<std::mutex> lock(mutex_);
            domain_ = rolledBack ? Domain::Singleplayer : Domain::Faulted;
            if (rolledBack) { snapshot_ = {}; hasSnapshot_ = false; }
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        domain_ = Domain::Coop;
        return true;
    }

    bool RestoreSingleplayer(const NativeProfileAdapter& adapter) {
        NativeProfileSnapshot captured;
        std::wstring spRoot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (domain_ == Domain::Singleplayer) return true;
            if (domain_ == Domain::Switching || !hasSnapshot_ ||
                !adapter.restoreSingleplayer || !adapter.setSaveRoot) return false;
            captured = snapshot_;
            spRoot = singleplayerRoot_;
            domain_ = Domain::Switching;
        }
        // Routing rejects profile I/O throughout restoration so intermediate
        // COOP state cannot be written into the singleplayer profile directory.
        const bool profileRestored = adapter.restoreSingleplayer(captured);
        const bool rootRestored = profileRestored && adapter.setSaveRoot(spRoot);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!profileRestored || !rootRestored) { domain_ = Domain::Faulted; return false; }
        snapshot_ = {};
        hasSnapshot_ = false;
        domain_ = Domain::Singleplayer;
        return true;
    }

    RoutedPath RoutePath(const std::wstring& input, bool allowFinalWildcard = false) const {
        std::lock_guard<std::mutex> lock(mutex_);
        RoutedPath result{PathDecision::Unchanged, input};
        if (!configured_ || domain_ == Domain::Singleplayer) return result;
        const auto spelling = NormalizedPathSpelling(input);
        const bool spelledSp = PathIsWithin(spelling, singleplayerRoot_);
        const bool spelledCoop = PathIsWithin(spelling, coopBaseRoot_);
        std::wstring canonical;
        if (!CanonicalWindowsPath(input, canonical, allowFinalWildcard)) {
            bool scopedAncestor = false;
            std::wstring ancestor = spelling;
            for (size_t slash = ancestor.find_last_of(L'\\'); slash != std::wstring::npos;
                slash = ancestor.find_last_of(L'\\')) {
                ancestor.resize(slash);
                std::wstring resolved;
                if (CanonicalWindowsPath(ancestor, resolved) &&
                    (PathIsWithin(resolved, singleplayerRoot_) || PathIsWithin(resolved, coopBaseRoot_))) {
                    scopedAncestor = true;
                    break;
                }
                if (ancestor.size() <= 2) break;
            }
            if (spelledSp || spelledCoop || scopedAncestor) result.decision = PathDecision::Reject;
            return result;
        }
        const bool inSp = PathIsWithin(canonical, singleplayerRoot_);
        const bool inCoop = PathIsWithin(canonical, coopBaseRoot_);
        if ((spelledSp && !inSp) || (spelledCoop && !inCoop)) {
            result.decision = PathDecision::Reject;
            return result;
        }
        if ((domain_ == Domain::Switching || domain_ == Domain::Faulted) && (inSp || inCoop)) {
            result.decision = PathDecision::Reject;
            return result;
        }
        const std::wstring active = CoopRootUnlocked(role_);
        if (inCoop) {
            if (!PathIsWithin(canonical, active)) result.decision = PathDecision::Reject;
            return result;
        }
        if (inSp) {
            result.decision = PathDecision::Redirect;
            result.path = active + canonical.substr(singleplayerRoot_.size());
        }
        return result;
    }

    Domain CurrentDomain() const { std::lock_guard<std::mutex> lock(mutex_); return domain_; }
    bool IsCoopActive() const { return CurrentDomain() == Domain::Coop; }
    bool HasSingleplayerSnapshot() const { std::lock_guard<std::mutex> lock(mutex_); return hasSnapshot_; }
    std::wstring SingleplayerRoot() const { std::lock_guard<std::mutex> lock(mutex_); return singleplayerRoot_; }
    std::wstring CoopRoot(Role role) const { std::lock_guard<std::mutex> lock(mutex_); return CoopRootUnlocked(role); }
    std::wstring ActiveRoot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return domain_ == Domain::Singleplayer ? singleplayerRoot_ : CoopRootUnlocked(role_);
    }

private:
    std::wstring CoopRootUnlocked(Role role) const {
        return coopBaseRoot_ + (role == Role::Host ? L"\\Host" : L"\\Client");
    }
    mutable std::mutex mutex_;
    Domain domain_ = Domain::Singleplayer;
    Role role_ = Role::Host;
    bool configured_ = false;
    bool hasSnapshot_ = false;
    std::wstring singleplayerRoot_;
    std::wstring coopBaseRoot_;
    NativeProfileSnapshot snapshot_;
};

} // namespace coop_save
