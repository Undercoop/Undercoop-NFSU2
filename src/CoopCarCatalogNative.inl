// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopCarCatalog.h"
#include "CoopCarCatalogInput.h"
#include <cerrno>
#include <cstdio>
#include <vector>
// Include after ReadGameMemory and IsValidUserPtr. Initialization and full
// validation run on the game thread at preflight, never in network callbacks.
namespace CoopCarCatalogNative {
using coop_car_catalog::Catalog;
static Catalog installed;
static bool initialized = false, parsed = false;
static uintptr_t tableAddress = 0x008A1CCC, validTable = 0;
static const char* failure = "car catalog has not been initialized";
static bool NativeAddonBounds() {
    if (installed.count <= coop_car_catalog::kStockCount) return true;
    // nlgxzef/NFSU2Unlimiter Game.h patches these exact native operands.
    // A populated file/table alone does not prove the engine can use addons.
    uint8_t loader[6]{}, lookup[6]{};
    if (!ReadGameMemory(0x00636C22, loader, sizeof(loader)) ||
        !ReadGameMemory(0x0061014E, lookup, sizeof(lookup)) ||
        loader[0] != 0x81 || loader[1] != 0xFE ||
        coop_car_catalog::U32(loader + 2) != installed.count * coop_car_catalog::kRecordBytes) return false;
    // 83 F8 imm8 sign-extends its operand. Do not treat 0x80..FF as a
    // positive model count. A loader using an imm32 compare can represent it.
    return (lookup[0] == 0x83 && lookup[1] == 0xF8 && lookup[2] < 0x80 && lookup[2] == installed.count) ||
        (lookup[0] == 0x81 && lookup[1] == 0xF8 && coop_car_catalog::U32(lookup + 2) == installed.count);
}
static bool ReadRecord(uintptr_t table, uint32_t model, coop_car_catalog::Model& out) {
    uint8_t identity[32]{}; uint32_t index = UINT32_MAX, usage = 0;
    const uintptr_t row = table + model * coop_car_catalog::kRecordBytes;
    if (!IsValidUserPtr(row) || !ReadGameMemory(row, identity, sizeof(identity)) ||
        !ReadGameMemory(row + 0x840, &index, 4) || index != model ||
        !ReadGameMemory(row + 0x844, &usage, 4)) return false;
    const auto* end = static_cast<const uint8_t*>(std::memchr(identity, 0, sizeof(identity)));
    if (!end || end == identity || static_cast<size_t>(end - identity) >= sizeof(out.name)) return false;
    out = {}; out.usage = usage;
    for (auto* p = identity; p < end; ++p)
        if (*p < 33 || *p > 126 || *p == '/' || *p == '\\') return false;
    std::memcpy(out.name, identity, static_cast<size_t>(end - identity)); return true;
}
static bool InitializeFromBytes(const void* bytes, size_t size) {
    initialized = true; parsed = false; installed = {}; validTable = 0;
    coop_car_catalog::Error error;
    parsed = coop_car_catalog::Parse(bytes, size, installed, error);
    coop_car_catalog::PublishCount(parsed ? installed.count : 0);
    failure = parsed ? "native car catalog is not ready" : coop_car_catalog::ErrorText(error);
    return parsed;
}
static bool InitializeFromGameDirectory(const char* directory) {
    initialized = true; parsed = false; installed = {}; validTable = 0;
    coop_car_catalog::PublishCount(0); failure = "GLOBAL/GLOBALB.LZC cannot be read";
    if (!directory || !*directory) return false;
    char path[MAX_PATH]{};
    const char* separator = directory[std::strlen(directory) - 1] == '\\' ||
        directory[std::strlen(directory) - 1] == '/' ? "" : "\\";
    auto makePath = [&](const char* filename) {
        const int written = std::snprintf(path, sizeof(path), "%s%sGLOBAL\\%s", directory, separator, filename);
        const bool fits=written >= 0 && static_cast<size_t>(written) < sizeof(path);
#ifdef UNDERCOOP_MOD_MANAGER
        return fits&&CoopModManager::ResolveAnsi(path,sizeof(path));
#else
        return fits;
#endif
    };
    if (!makePath("GLOBALB.LZC")) return false;
    FILE* file = std::fopen(path, "rb");
    // GLOBALB.LZC is the game's input. A stale auxiliary BUN must never mask
    // an unreadable or malformed LZC; fallback is only for an absent LZC.
    if (!file && errno == ENOENT) {
        failure = "GLOBAL/GLOBALB.BUN cannot be read";
        if (!makePath("GLOBALB.BUN")) return false;
        file = std::fopen(path, "rb");
    }
    if (!file) return false;
    bool okay = false;
    if (!std::fseek(file, 0, SEEK_END)) {
        const long size = std::ftell(file);
        if (size >= 8 && static_cast<size_t>(size) <= coop_car_catalog::kMaxFileBytes && !std::fseek(file, 0, SEEK_SET)) {
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            if (std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size() && std::fgetc(file) == EOF && !std::ferror(file)) {
                coop_car_catalog_input::Error inputError;
                coop_car_catalog::Error catalogError;
                parsed = coop_car_catalog_input::Parse(bytes.data(), bytes.size(), installed, inputError, catalogError);
                coop_car_catalog::PublishCount(parsed ? installed.count : 0);
                failure = parsed ? "native car catalog is not ready" :
                    coop_car_catalog_input::ErrorText(inputError, catalogError);
                okay = parsed;
            }
        }
    }
    std::fclose(file); return okay;
}
static bool PrepareRuntimeCatalog() {
    if (!initialized || !parsed) return false;
    uintptr_t table = 0;
    if (!ReadGameMemory(tableAddress, &table, sizeof(table)) || !IsValidUserPtr(table)) {
        validTable = 0; failure = "native car catalog is not ready"; return false;
    }
    // The caller throttles this preflight to 500ms. Failed mappings may simply
    // be an in-place table still loading, so cache only successful validation.
    // Recheck native bounds even for a cached success: a loader may repatch them.
    if (!NativeAddonBounds()) {
        validTable = 0; failure = "addon catalog requires matching native loader bounds"; return false;
    }
    if (table == validTable) return true;
    validTable = 0; failure = "native and installed car catalogs do not match";
    for (uint32_t model = 0; model < installed.count; ++model) {
        coop_car_catalog::Model actual;
        if (!ReadRecord(table, model, actual) || actual.usage != installed.models[model].usage ||
            std::strcmp(actual.name, installed.models[model].name)) return false;
    }
    validTable = table; failure = "ready"; return true;
}
static bool Supported(uint32_t model) {
    if (!coop_car_catalog::PortableModelIndex(model)) return false;
    uintptr_t table = 0; coop_car_catalog::Model actual;
    if (!ReadGameMemory(tableAddress, &table, sizeof(table)) || !IsValidUserPtr(table)) return false;
    // Before explicit initialization preserve the stock-only, positively read
    // record path used by standalone native fixtures. Initialization failure
    // publishes zero; it can never reopen the stock path in a real session.
    if (!initialized) return model < coop_car_catalog::kStockCount && ReadRecord(table, model, actual);
    return parsed && table == validTable && ReadRecord(table, model, actual) &&
        actual.usage == installed.models[model].usage && !std::strcmp(actual.name, installed.models[model].name);
}
static bool ReadName(uint32_t model, char* output, size_t capacity) {
    if (!output || !capacity) return false; output[0] = 0;
    if (!Supported(model)) return false;
    coop_car_catalog::Model actual;
    if (initialized) actual = installed.models[model];
    else { uintptr_t table = 0;
        if (!ReadGameMemory(tableAddress, &table, sizeof(table)) || !ReadRecord(table, model, actual)) return false;
    }
    const size_t bytes = std::strlen(actual.name) + 1;
    if (bytes > capacity) return false; std::memcpy(output, actual.name, bytes); return true;
}
static bool Ready() { return parsed && validTable != 0; }
static uint32_t Count() { return parsed ? installed.count : 0; }
static uint64_t Fingerprint() { return parsed ? installed.fingerprint : 0; }
static uint64_t Footprint() { return Fingerprint(); }
static const char* Failure() { return failure; }
}
