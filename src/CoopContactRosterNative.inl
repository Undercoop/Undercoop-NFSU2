// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Car contact lookup 60C0B0 resolves native contact IDs via 89E970.
// Replicated Free Roam contacts can outlive an optional registry entry.
// Skip only empty entries in COOP; do not fabricate collision/racer objects.
namespace CoopContactRosterNative {
struct Addresses {
    uintptr_t compare = 0x0060C0D0;
    uintptr_t next = 0x0060C0D5;
    uintptr_t match = 0x0060C0E0;
    uintptr_t registry = 0x0089E970;
};
static Addresses addresses{};
static volatile LONG skipped = 0;

__declspec(naked) static void CompareHook() {
    __asm {
        cmp dword ptr [gCoopGameplayActive], 0
        je nativeCompare
        test edi, edi
        jnz nativeCompare
        inc dword ptr [skipped]
        jmp nextEntry
    nativeCompare:
        cmp dword ptr [edi + 0x1C], esi
        je matched
    nextEntry:
        jmp dword ptr [addresses.next]
    matched:
        jmp dword ptr [addresses.match]
    }
}

static bool Install() {
    // Include the preceding registry resolution, not just a generic cmp/je.
    uint8_t expected[] = {0x8B,0x3C,0xBD,0x70,0xE9,0x89,0x00,0x39,0x77,0x1C,0x74,0x0B};
    std::memcpy(expected + 3, &addresses.registry, 4);
    uint8_t observed[sizeof(expected)]{};
    if (!ReadGameMemory(addresses.compare - 7, observed, sizeof(observed)) ||
        std::memcmp(observed, expected, sizeof(expected)) != 0) return false;
    uint8_t patch[5] = {0xE9};
    const int32_t delta = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&CompareHook) - (addresses.compare + 5));
    std::memcpy(patch + 1, &delta, sizeof(delta));
    DWORD oldProtection = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addresses.compare), sizeof(patch),
        PAGE_EXECUTE_READWRITE, &oldProtection)) return false;
    const bool written = SafeWriteBytes(addresses.compare, patch, sizeof(patch));
    if (written) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addresses.compare), sizeof(patch));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(addresses.compare), sizeof(patch), oldProtection, &unused);
    return written;
}
}
