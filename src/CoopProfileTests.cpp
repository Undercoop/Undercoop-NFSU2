// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include "CoopProfileContext.h"

#include <cstdlib>
#include <iostream>

namespace {
void Check(bool condition, const char* label) {
    if (!condition) {
        std::cerr << "FAILED: " << label << '\n';
        std::exit(1);
    }
}

uint32_t IndependentCrc(const std::vector<uint8_t>& data) {
    // Separate bit-by-bit reference for the executable's CRC32 lookup table.
    if (data.size() < 4) return 0;
    uint32_t remainder = ~((uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
        (uint32_t(data[2]) << 8) | uint32_t(data[3]));
    for (size_t i = 4; i < data.size(); ++i) {
        const uint8_t top = static_cast<uint8_t>(remainder >> 24);
        uint32_t tableEntry = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const bool carry = (tableEntry >> 31) != 0;
            tableEntry <<= 1;
            if ((top & (1u << (7 - bit))) != 0) tableEntry ^= 0x04C11DB7u;
            if (carry) tableEntry ^= 0x04C11DB7u;
        }
        remainder = (remainder << 8 | data[i]) ^ tableEntry;
    }
    return ~remainder;
}
} // namespace

int main() {
    using namespace coop_save;
    const std::wstring sp = L"C:\\Users\\Example\\AppData\\Local\\NFS Underground 2";
    const std::wstring coop = L"C:\\Users\\Example\\AppData\\Local\\NFS Underground 2 Co-op";
    const std::wstring profile = sp + L"\\NAME\\NAME";
    ProfileContext context;
    Check(context.Configure(sp, coop), "configure disjoint roots");
    Check(context.RoutePath(profile).decision == PathDecision::Unchanged, "startup singleplayer routing");

    int captureCount = 0, restoreCount = 0;
    bool nativeRestoreSucceeds = true;
    std::wstring nativeRoot = sp;
    NativeProfileAdapter adapter;
    adapter.captureSingleplayer = [&](NativeProfileSnapshot& snapshot) {
        ++captureCount;
        snapshot.payload.assign(kNativeProfilePayloadSize, 0x57);
        snapshot.metadata = {7, 11, 13};
        Check(context.RoutePath(profile).decision == PathDecision::Reject, "capture blocks profile I/O");
        return true;
    };
    adapter.setSaveRoot = [&](const std::wstring& root) {
        nativeRoot = root;
        Check(context.RoutePath(profile).decision == PathDecision::Reject, "root switch blocks profile I/O");
        return true;
    };
    adapter.restoreSingleplayer = [&](const NativeProfileSnapshot& snapshot) {
        ++restoreCount;
        Check(snapshot.payload.size() == kNativeProfilePayloadSize && snapshot.payload.front() == 0x57,
            "restore original serialized profile");
        Check(snapshot.metadata == std::vector<uint8_t>({7, 11, 13}), "restore original metadata");
        Check(context.RoutePath(profile).decision == PathDecision::Reject, "restoration blocks profile I/O");
        return nativeRestoreSucceeds;
    };

    Check(!context.BeginCoopProfileSelection(Role::Host, false, adapter) && captureCount == 0,
        "peer required before profile switch");
    Check(context.BeginCoopProfileSelection(Role::Host, true, adapter), "explicit COOP switch");
    Check(context.HasSingleplayerSnapshot(), "singleplayer snapshot retained");
    Check(nativeRoot == coop + L"\\Host", "host native save root");
    Check(context.BeginCoopProfileSelection(Role::Host, true, adapter) && captureCount == 1,
        "repeat selection does not overwrite singleplayer snapshot");
    Check(!context.BeginCoopProfileSelection(Role::Client, true, adapter), "role cannot change during active profile");

    auto routed = context.RoutePath(profile);
    Check(routed.decision == PathDecision::Redirect && routed.path == coop + L"\\Host\\NAME\\NAME",
        "original profile redirects to isolated host root");
    Check(context.RoutePath(sp + L" Other\\NAME").decision == PathDecision::Unchanged,
        "similar root name is unrelated");
    Check(context.RoutePath(L"C:\\Game\\NFS Underground 2\\GLOBALB.BUN").decision == PathDecision::Unchanged,
        "game asset folder name is unrelated");
    Check(context.RoutePath(coop + L"\\Client\\NAME\\NAME").decision == PathDecision::Reject,
        "host cannot write client profile");
    Check(context.RoutePath(sp + L"\\..\\outside\\save").decision == PathDecision::Reject,
        "profile traversal cannot escape routing");
    Check(context.RoutePath(profile + L":alternate").decision == PathDecision::Reject,
        "alternate data stream rejected in profile scope");
    Check(context.RoutePath(L"C:\\Users\\\\Example\\AppData\\Local\\NFS Underground 2\\NAME\\NAME:alternate").decision ==
        PathDecision::Reject, "alternate data stream cannot bypass scope through repeated separators");
    Check(context.RoutePath(L"C:\\Users\\Example\\AppData\\Local\\.\\NFS Underground 2\\NAME\\NAME:alternate").decision ==
        PathDecision::Reject, "alternate data stream cannot bypass scope through normalized ancestor");
    Check(context.RoutePath(sp + L"\\NAME\\NUL").decision == PathDecision::Reject,
        "device filename rejected in profile scope");
    Check(context.RoutePath(sp + L"\\*").decision == PathDecision::Reject,
        "wildcards rejected for file writes");
    Check(context.RoutePath(sp + L"\\*", true).decision == PathDecision::Redirect,
        "profile enumeration wildcard supported");
    Check(context.RoutePath(L"\\\\?\\" + profile).decision == PathDecision::Redirect,
        "extended drive path has same profile routing");
    Check(context.RoutePath(sp + L"\\NAME\\.\\NAME").decision == PathDecision::Redirect,
        "dot path normalizes before routing");

    nativeRestoreSucceeds = false;
    Check(!context.RestoreSingleplayer(adapter), "failed native restore surfaced");
    Check(context.CurrentDomain() == Domain::Faulted && context.HasSingleplayerSnapshot(),
        "failed restore retains original snapshot");
    Check(context.RoutePath(profile).decision == PathDecision::Reject, "failed restore cannot fall back to SP writes");
    nativeRestoreSucceeds = true;
    Check(context.RestoreSingleplayer(adapter), "failed restore can retry");
    Check(nativeRoot == sp && !context.HasSingleplayerSnapshot(), "successful restore resets native root and snapshot");
    Check(context.RoutePath(profile).decision == PathDecision::Unchanged, "singleplayer routing restored");
    Check(context.BeginCoopProfileSelection(Role::Client, true, adapter), "client enters own context");
    Check(context.RoutePath(profile).path == coop + L"\\Client\\NAME\\NAME", "client root isolated from host");
    Check(context.RestoreSingleplayer(adapter), "client SP restoration");

    ProfileContext badRoots;
    Check(!badRoots.Configure(sp, sp + L"\\COOP"), "nested save roots rejected");
    std::wstring normalized;
    Check(!CanonicalWindowsPath(L"C:relative", normalized), "drive relative path rejected");
    Check(CanonicalWindowsPath(L"\\\\server\\share\\profiles\\NAME", normalized), "UNC save root supported");
    Check(!CanonicalWindowsPath(L"\\\\server\\share\\..\\outside", normalized), "UNC traversal cannot escape share");
    Check(IsSafeProfileName(L"NAME") && !IsSafeProfileName(L"..") && !IsSafeProfileName(L"x/y"),
        "checkpoint profile name guard");

    const std::vector<uint8_t> crcInput = {0x32, 0x30, 0x43, 0x4D, 0x12, 0xAB, 0xFE, 0x99};
    Check(EaCrc32(crcInput.data(), crcInput.size()) == IndependentCrc(crcInput), "CRC independent reference");
    Check(EaCrc32(crcInput.data(), 4) == 0x3230434Du, "CRC four-byte native seed vector");
    std::vector<uint8_t> payload(kNativeProfilePayloadSize);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 17 + 3);
    std::vector<uint8_t> file;
    Check(EncodeNativeProfileFile(payload, file), "native MC02 encoding");
    Check(ValidateNativeProfileFile(file.data(), file.size()) == SaveValidation::Valid, "native MC02 format validation");
    file[0x400] ^= 1;
    Check(ValidateNativeProfileFile(file.data(), file.size()) == SaveValidation::BadChunkCrc,
        "corrupt checkpoint payload rejected");
    file[0x400] ^= 1;
    file[0x18] ^= 1;
    Check(ValidateNativeProfileFile(file.data(), file.size()) == SaveValidation::BadHeaderCrc,
        "corrupt checkpoint header rejected");
    Check(ValidateNativeProfileFile(file.data(), file.size() - 1) == SaveValidation::BadSize,
        "truncated checkpoint rejected");
    std::cout << "CoopProfileTests passed (synthetic memory only; no save files opened).\n";
    return 0;
}
