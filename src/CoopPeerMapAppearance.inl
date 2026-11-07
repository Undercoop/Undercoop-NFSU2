// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Appearance only: copy the local human arrow onto the owned peer marker.
// Keep native pool types, IDs, discovery filters and transforms untouched.
// 50C990 sets FEImage+24 and its dirty flag; 51D7F0 sets packed ARGB.
// 50CCC0/5054D0 verify FEObject+2C points to B,G,R,A integer components.
namespace CoopPeerMapAppearance {
using Setter = void(__cdecl*)(void*, uint32_t);
static Setter setTexture = reinterpret_cast<Setter>(0x50C990);
static Setter setColor = reinterpret_cast<Setter>(0x51D7F0);
struct Appearance { uint32_t texture = 0, color = 0; };
struct Lease { uintptr_t element = 0, token = 0; Appearance original{}; };
// Eight native AI templates and eight player templates at most. Keep each
// lease until a current non-peer marker uses it, including rapid re-leases.
static Lease minimap[16]{}, worldMap{};
static uintptr_t minimapOwner = 0;
static uint32_t worldMapElementHash = 0;
static char worldMapPackage[64]{};

static bool Read(uintptr_t element, Appearance& out) {
    uint32_t type = 0; uintptr_t components = 0; int32_t bgra[4]{};
    if (!IsValidUserPtr(element) || !ReadGameMemory(element + 0x18, &type, 4) || type != 1 ||
        !ReadGameMemory(element + 0x24, &out.texture, 4) || !out.texture ||
        !ReadGameMemory(element + 0x2C, &components, 4) ||
        !ReadGameMemory(components, bgra, sizeof(bgra))) return false;
    for (const auto value : bgra) if (value < 0 || value > 255) return false;
    out.color = uint32_t(bgra[3]) << 24 | uint32_t(bgra[2]) << 16 |
        uint32_t(bgra[1]) << 8 | uint32_t(bgra[0]);
    return true;
}
static bool Write(uintptr_t element, const Appearance& appearance) {
    Appearance current{};
    if (!setTexture || !setColor || !Read(element, current)) return false;
    // Avoid dirtying/rebuilding unchanged FEng image resources every frame.
    if (current.texture != appearance.texture)
        setTexture(reinterpret_cast<void*>(element), appearance.texture);
    if (current.color != appearance.color)
        setColor(reinterpret_cast<void*>(element), appearance.color);
    return true;
}
static bool Apply(Lease& lease, uintptr_t element, uintptr_t token, uintptr_t local) {
    Appearance own{}, previous{};
    if (element == local || !Read(local, own) || !Read(element, previous)) return false;
    if (lease.element != element || lease.token != token)
        lease = {element, token, previous};
    return Write(element, own);
}
static uintptr_t MinimapElement(uintptr_t marker, uintptr_t* nativeIcon = nullptr) {
    uintptr_t icon = 0, element = 0;
    if (!ReadGameMemory(marker + 0x1C, &icon, 4) || !ReadGameMemory(icon, &element, 4)) return 0;
    if (nativeIcon) *nativeIcon = icon;
    return element;
}
static uintptr_t LocalMinimapElement() {
    uintptr_t manager = 0; NativeMapLinks head{}; NativeMinimapMarker local{};
    uint32_t type = 0;
    if (!ReadGameMemory(gMinimapManagerPointer, &manager, 4) ||
        !ReadGameMemory(manager, &head, sizeof(head)) ||
        !ReadGameMemory(head.next, &local, sizeof(local)) || local.id != 0 ||
        local.links.previous != manager ||
        !ReadGameMemory(local.icon + 4, &type, 4) || type != 4) return 0;
    return MinimapElement(head.next);
}
// Never dereference a stored UI pointer after screen teardown. Restore only
// when a CURRENT native marker/FindElement lookup proves that widget is live.
static void RestoreMinimapForCurrentMarker(uintptr_t element, uintptr_t icon) {
    if (!element || !icon) return;
    for (auto& lease : minimap) if (lease.element == element && lease.token == icon) {
        Write(element, lease.original); lease = {};
    }
}
static void ApplyMinimap(uintptr_t element, uintptr_t icon) {
    uintptr_t owner = 0;
    if (!ReadGameMemory(gMinimapManagerPointer, &owner, 4)) return;
    if (owner != minimapOwner) {
        // The native HUD teardown destroys the old manager and its icon pool.
        for (auto& lease : minimap) lease = {};
        minimapOwner = owner;
    }
    Lease* available = nullptr;
    for (auto& lease : minimap) {
        if (lease.element == element && lease.token == icon) {
            Apply(lease, element, icon, LocalMinimapElement()); return;
        }
        if (!lease.element && !available) available = &lease;
    }
    if (available) Apply(*available, element, icon, LocalMinimapElement());
}
static void RestoreWorldMap() {
    if (worldMap.element && gMapFindElement &&
        reinterpret_cast<uintptr_t>(gMapFindElement(worldMapPackage, worldMapElementHash)) == worldMap.element)
        Write(worldMap.element, worldMap.original);
    worldMap = {}; worldMapElementHash = 0; worldMapPackage[0] = 0;
}
} // namespace CoopPeerMapAppearance

using NativeMinimapAppearanceFn = void(__thiscall*)(void*);
static NativeMinimapAppearanceFn gOriginalMinimapAppearance = nullptr;
void __fastcall HookedMinimapAppearance(void* object, void*) {
    using namespace CoopPeerMapAppearance;
    const uintptr_t marker = reinterpret_cast<uintptr_t>(object);
    uintptr_t icon = 0; const uintptr_t element = MinimapElement(marker, &icon);
    const bool peer = IsOwnedPeerMinimapMarker(marker);
    if (!peer) RestoreMinimapForCurrentMarker(element, icon);
    if (gOriginalMinimapAppearance) gOriginalMinimapAppearance(object);
    // Run AFTER native AI styling, which otherwise overwrites a leased arrow.
    if (peer) ApplyMinimap(element, icon);
}
