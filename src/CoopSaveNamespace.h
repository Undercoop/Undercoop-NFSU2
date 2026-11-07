// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
namespace coop_build {
// Test-only builds keep modded garage records away from normal CO-OP saves.
// Ordinary builds retain exactly the established save namespace.
#if defined(UNDERCOOP_MOD_MANAGER)
inline char kCoopSaveFolder[128]="NFSU2 Undercoop Mods Unbound";
inline wchar_t kCoopSaveFolderWide[128]=L"NFSU2 Undercoop Mods Unbound";
constexpr const char* kSaveDomain="isolated-managed-pack";
#elif defined(UNDERCOOP_ADDON_SANDBOX)
constexpr const char* kCoopSaveFolder="NFS Underground 2 Addon";
constexpr const wchar_t* kCoopSaveFolderWide=L"NFS Underground 2 Addon";
constexpr const char* kSaveDomain="isolated-addon-test";
#else
constexpr const char* kCoopSaveFolder="NFS Underground 2 Co-op";
constexpr const wchar_t* kCoopSaveFolderWide=L"NFS Underground 2 Co-op";
constexpr const char* kSaveDomain="normal-coop";
#endif
}
