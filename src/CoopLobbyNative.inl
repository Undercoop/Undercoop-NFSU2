// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Include inside NFSU2Coop.cpp's anonymous namespace, after CoopNativeSession.inl.
// These ABIs/layouts were checked against this installation's SPEED2.EXE:
// 4D1380 room-list refresh (__thiscall, one stack arg, RET4);
// 51F4E0 owned data-row clear (__thiscall, no args, RET);
// 4AEA60 formatted text-column append (__thiscall, three args, RET0C);
// 51F2E0/52EA50/5434D0 data-row add/bind/disable (__thiscall, one arg, RET4).
// Custom rows bypass native room metadata and use the existing FEng row templates.

using NativeCoopSnapshotProvider = coop::SessionSnapshot(*)();
static NativeCoopSnapshotProvider gNativeCoopSnapshotProvider = nullptr;
static void* gNativeCoopLobbyObject = nullptr; // Game-thread only; destructor hook clears it.
static void* gNativeCoopRefreshOriginal = nullptr;
static void* gNativeCoopJoinOriginal = nullptr;
static DWORD gNativeCoopLobbyLastUpdate = 0;
static void* gNativeCoopChoiceOwner = nullptr;
static bool gNativeCoopStartWasAllowed = false;
static bool gNativeCoopFocusPending = false;

struct NativeCoopTextRow {
    uintptr_t vtable;
    uintptr_t next, previous;
    uintptr_t firstColumn, lastColumn;
    uint32_t selectable;
    int32_t roomId;
};
static_assert(sizeof(NativeCoopTextRow) == 0x1c, "Native lobby rows require x86");

bool IsNativeCoopLobbyObject(void* lobby) {
    const uintptr_t object = reinterpret_cast<uintptr_t>(lobby);
    uintptr_t vtable = 0, package = 0;
    char packageName[32]{};
    return ReadGameMemory(object, &vtable, sizeof(vtable)) && vtable == 0x00795fd0 &&
        ReadGameMemory(object + 4, &package, sizeof(package)) &&
        ReadGameMemory(package, packageName, sizeof(packageName) - 1) &&
        std::strcmp(packageName, "PC_OL_Lobby.fng") == 0;
}

bool ValidateNativeCoopDataRows(uintptr_t scroller) {
    const uintptr_t sentinel = scroller + 0x10;
    uintptr_t node = 0, previous = sentinel, last = 0;
    uint32_t count = 0;
    if (!ReadGameMemory(sentinel, &node, sizeof(node)) ||
        !ReadGameMemory(sentinel + 4, &last, sizeof(last)) ||
        !ReadGameMemory(scroller + 0x1c, &count, sizeof(count)) || count > 15) return false;
    uint32_t seen = 0;
    while (node != sentinel && seen < 15) {
        uintptr_t links[2]{}, vtable = 0;
        if (node < 4 || !ReadGameMemory(node, links, sizeof(links)) || links[1] != previous ||
            !ReadGameMemory(node - 4, &vtable, sizeof(vtable)) || vtable != 0x00795cb0) return false;
        previous = node;
        node = links[0];
        ++seen;
    }
    return node == sentinel && previous == last && seen == count;
}

void SetNativeCoopElementVisible(void* element, bool visible) {
    uint32_t kind = 0;
    if (!element || !ReadGameMemory(reinterpret_cast<uintptr_t>(element) + 0x18, &kind, sizeof(kind)) ||
        kind > 8) return;
    const uintptr_t function = visible ? 0x0050ca50 : 0x0050ca00;
    reinterpret_cast<void(__cdecl*)(void*)>(function)(element);
}

int __cdecl NativeCoopCounterLabelHook(char* package, uint32_t hash, const char* format, uint32_t count) {
    if(gCoopUiActive && package && std::strcmp(package,"PC_OL_Lobby.fng")==0) {
        const auto state=CoopSnapshot();
        if(hash==0x04059a9c) count=1+(state.joined && state.fresh?1:0);
        if(hash==0x154d771c) count=state.role==coop::Role::Host || state.role==coop::Role::Client?1:0;
    }
    return reinterpret_cast<int(__cdecl*)(char*,uint32_t,const char*,...)>(0x00537b80)(
        package,hash,format,count);
}

// 4AEA60 forwards its text directly to the native printf formatter. Escape %
// in profile names before appending a column; no profile text becomes a format.
void EscapeNativeCoopColumn(const char* text, char (&format)[128]) {
    size_t output = 0;
    for (size_t input = 0; text && text[input] && output + 2 < sizeof(format); ++input) {
        if (text[input] == '%') format[output++] = '%';
        format[output++] = text[input];
    }
    format[output] = '\0';
}

bool AddNativeCoopTextRow(void* scroller, const char* name, const char* role, bool enabled) {
    auto* row = static_cast<NativeCoopTextRow*>(
        reinterpret_cast<void*(__cdecl*)(size_t)>(0x00575620)(sizeof(NativeCoopTextRow)));
    if (!row) return false;
    std::memset(row, 0, sizeof(*row));
    row->vtable = 0x00795cb0;
    row->firstColumn = row->lastColumn = reinterpret_cast<uintptr_t>(row) + 0xc;
    row->selectable = 1;
    row->roomId = -1; // Never pass custom rows to native room-join logic.
    const char* columns[5] = {name, role, "", "", ""};
    for (const char* column : columns) {
        char format[128]{};
        EscapeNativeCoopColumn(column, format);
        reinterpret_cast<void(__thiscall*)(void*, const char*, uint32_t, uint32_t)>(0x004aea60)(
            row, format, 0, 0);
    }
    reinterpret_cast<void(__thiscall*)(void*, void*)>(0x0051f2e0)(scroller, row);
    reinterpret_cast<void(__thiscall*)(void*, void*)>(enabled ? 0x0052ea50 : 0x005434d0)(scroller, row);
    return true;
}

void UpdateNativeCoopLobby(void* lobby, const coop::SessionSnapshot& snapshot) {
    if (!gCoopUiActive || !IsNativeCoopLobbyObject(lobby)) return;
    const uintptr_t object = reinterpret_cast<uintptr_t>(lobby);
    const uintptr_t scrollerAddress = object + 0x4c;
    if (!ValidateNativeCoopDataRows(scrollerAddress)) {
        LogMessage("[COOP_UI] Native lobby data-row ownership validation failed; refresh skipped.\r\n");
        return;
    }
    uintptr_t packageAddress = 0;
    if (!ReadGameMemory(object + 4, &packageAddress, sizeof(packageAddress))) return;
    auto* package = reinterpret_cast<char*>(packageAddress);
    auto* scroller = reinterpret_cast<void*>(scrollerAddress);
    gNativeCoopLobbyObject = lobby;
    NativeCoopCounterLabelHook(package,0x04059a9c,"%$d",0);
    NativeCoopCounterLabelHook(package,0x154d771c,"%$d",0);

    // Native Clear owns/destructs all prior text rows and their column cells.
    reinterpret_cast<void(__thiscall*)(void*)>(0x0051f4e0)(scroller);
    const bool connected = snapshot.joined && snapshot.fresh;
    const char* hostName = snapshot.hostName[0] ? snapshot.hostName : "Host";
    const char* clientName = connected && snapshot.clientName[0] ? snapshot.clientName : "Waiting for player";
    const auto statusLines=coop_lobby_text::Wrap(CoopModStatus());
    for (int index = 0; index < 15; ++index) {
        const char* name = index == 0 ? hostName : (index == 1 ? clientName :
            index>=2&&index<10?statusLines[index-2].data():
            index==11&&snapshot.role==coop::Role::Client&&!snapshot.careerLaunched?"HOST CHOOSES THE SAVE":
            index==12&&snapshot.role==coop::Role::Client&&!snapshot.careerLaunched?"WAIT FOR HOST TO LOAD":"");
        const char* role = index == 0 ? "HOST" : (index == 1 && connected ? "CLIENT" : "");
        // Membership rows are informational, not native LAN room records.
        if (!AddNativeCoopTextRow(scroller, name, role, false)) {
            LogMessage("[COOP_UI] Native lobby row allocation failed.\r\n");
            break;
        }
    }
    reinterpret_cast<void(__thiscall*)(void*, unsigned int)>(0x0054edd0)(scroller, 0);
    // Selection alone does not populate FEng labels. This is the native
    // scroller's data-to-visual flush (RET4), also used by 4BAFD0.
    reinterpret_cast<void(__thiscall*)(void*, int)>(0x00538500)(scroller, 1);

    // Reuse the four native button containers/text labels. Keep their handles
    // intact so a host's Start button can appear when membership is confirmed.
    const uint32_t labels[4] = {0x626eb826, 0xd4d9d042, 0x14b76941, 0x68cf7b18};
    for (int index = 0; index < 4; ++index) {
        uintptr_t container = 0, text = 0;
        ReadGameMemory(object + 0x144 + index * 4, &container, sizeof(container));
        ReadGameMemory(object + 0x154 + index * 4, &text, sizeof(text));
        const bool visible = index == 0 || (index == 1 && snapshot.startAllowed);
        SetNativeCoopElementVisible(reinterpret_cast<void*>(container), visible);
        SetNativeCoopElementVisible(reinterpret_cast<void*>(text), visible);
        if (index == 1) {
            reinterpret_cast<int(__cdecl*)(char*, uint32_t, const char*, ...)>(0x00537b80)(
                package, labels[index], "%s", snapshot.careerLaunched?"RESUME COOP CAREER":"START THE CAREER");
        }
        if(index==0) reinterpret_cast<int(__cdecl*)(char*,uint32_t,const char*,...)>(0x537b80)(
            package,labels[index],"%s",snapshot.role==coop::Role::Host?"CLOSE COOP SERVER":"LEAVE COOP");
    }
    uint32_t selectedButton=0;
    ReadGameMemory(object+0x118,&selectedButton,sizeof(selectedButton));
    const bool resetChoice=gNativeCoopChoiceOwner!=lobby ||
        gNativeCoopStartWasAllowed!=snapshot.startAllowed;
    if(resetChoice || selectedButton>1 || (!snapshot.startAllowed && selectedButton==1)) {
        selectedButton=resetChoice && snapshot.startAllowed?1:0;
        gNativeCoopFocusPending=true;
    }
    gNativeCoopChoiceOwner=lobby;
    gNativeCoopStartWasAllowed=snapshot.startAllowed;
    SafeWriteBytes(object + 0x118, &selectedButton, sizeof(selectedButton));
    reinterpret_cast<void(__thiscall*)(void*)>(0x0049ed30)(lobby);

    // Existing room-specific icon slots are unrelated to the two player rows.
    for (int index = 1; index <= 15; ++index) {
        const char* formats[2] = {"Lock_Icon_%d", "U2_mini_icon_%d"};
        for (const char* format : formats) {
            char name[32]{};
            std::snprintf(name, sizeof(name), format, index);
            const uint32_t hash = reinterpret_cast<uint32_t(__cdecl*)(const char*)>(0x00505450)(name);
            void* element = reinterpret_cast<void*(__cdecl*)(char*, uint32_t)>(0x005379c0)(package, hash);
            SetNativeCoopElementVisible(element, false);
        }
    }
}

void TickNativeCoopLobby(const coop::SessionSnapshot& snapshot, DWORD now) {
    if (!gNativeCoopLobbyObject || !gCoopUiActive || now - gNativeCoopLobbyLastUpdate < 250) return;
    gNativeCoopLobbyLastUpdate = now;
    UpdateNativeCoopLobby(gNativeCoopLobbyObject, snapshot);
    // Constructor updates can be followed by a native focus reset. Perform
    // this only on a regular game-thread tick, after construction completes.
    // Preserve navigation thereafter; never force Start every 250 ms.
    if(gNativeCoopFocusPending && IsNativeCoopLobbyObject(gNativeCoopLobbyObject)) {
        const uintptr_t object=reinterpret_cast<uintptr_t>(gNativeCoopLobbyObject);
        uintptr_t package=0,text=0;
        uint32_t selected=0;
        if(ReadGameMemory(object+4,&package,sizeof(package)) &&
            ReadGameMemory(object+0x118,&selected,sizeof(selected)) && selected<=1 &&
            ReadGameMemory(object+0x154+selected*4,&text,sizeof(text)) && package && text) {
            reinterpret_cast<void(__cdecl*)(char*,void*)>(0x0052cec0)(
                reinterpret_cast<char*>(package),reinterpret_cast<void*>(text));
            gNativeCoopFocusPending=false;
        }
    }
}

void __fastcall NativeCoopLobbyRefreshHook(void* lobby, void*, int clearOnly) {
    if (gCoopUiActive && gNativeCoopSnapshotProvider && IsNativeCoopLobbyObject(lobby)) {
        UpdateNativeCoopLobby(lobby, gNativeCoopSnapshotProvider());
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*, int)>(gNativeCoopRefreshOriginal)(lobby, clearOnly);
}

void __fastcall NativeCoopLobbyDestructorHook(void* lobby, void*) {
    if (gNativeCoopLobbyObject == lobby) gNativeCoopLobbyObject = nullptr;
    if(gNativeCoopChoiceOwner==lobby) {
        gNativeCoopChoiceOwner=nullptr;
        gNativeCoopFocusPending=false;
    }
    if (gNativeCoopLobby == lobby) gNativeCoopLobby = nullptr;
    // Verified target of the CALL at 4E3EB3. No arguments; caller owns delete flags.
    reinterpret_cast<void(__thiscall*)(void*)>(0x004d12c0)(lobby);
}

void __fastcall NativeCoopLobbyJoinHook(void* lobby, void*) {
    if (gCoopUiActive) {
        // Root callback queues the guarded session-core request for game-thread handling.
        StartCoopCareerFromLobby();
        return; // Original 49EDD0 tail-calls a __thiscall function with RET0.
    }
    reinterpret_cast<void(__thiscall*)(void*)>(gNativeCoopJoinOriginal)(lobby);
}

// Preserve the native UI/manager allocation, but do not start its unrelated
// TCP connection monitor. SessionCore owns COOP membership and discovery.
void __fastcall NativeCoopMonitorInitHook(void* monitor, void*, void* owner, int showDialog) {
    if (gCoopUiActive && IsNativeCoopLobbyObject(owner)) {
        gNativeCoopLobbyObject=owner;
        auto* bytes = static_cast<BYTE*>(monitor);
        *reinterpret_cast<void**>(bytes + 8) = owner;
        bytes[4] = bytes[0xd] = bytes[0xe] = bytes[0x10] = 0;
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*, void*, int)>(0x004cfd80)(monitor, owner, showDialog);
}

int __fastcall NativeCoopMonitorNotifyHook(void* monitor, void*, uint32_t event,
    uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, int target) {
    // Ownership lasts until the native destructor, even after gCoopUiActive
    // becomes false to navigate to the career/profile screens.
    void* owner=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(monitor)-0x120);
    if (owner==gNativeCoopLobbyObject && IsNativeCoopLobbyObject(owner)) return 0;
    return reinterpret_cast<int(__thiscall*)(void*, uint32_t, uintptr_t, uintptr_t, uintptr_t, int)>(
        0x004efe40)(monitor, event, arg1, arg2, arg3, target);
}

void __cdecl RefreshNativeCoopBrowser(void* browser) {
    uintptr_t vtable=0;
    if(!ReadGameMemory(reinterpret_cast<uintptr_t>(browser), &vtable, sizeof(vtable)) ||
        vtable!=0x00796420) return;
    static DWORD lastRefresh=0;
    const DWORD now=GetTickCount();
    if(now-lastRefresh<500) return;
    lastRefresh=now;
    // Both query/record adapters use SessionCore, not the native backend handle.
    reinterpret_cast<void(__thiscall*)(void*)>(0x004f3040)(browser);
}

__declspec(naked) void NativeCoopBrowserBackendHook() {
    __asm {
        cmp byte ptr [gCoopUiActive], 0
        je vanilla_browser_backend
        // Visual rows have already been initialized; skip backend/error dialogs.
        mov dword ptr [esp], 0x004FB662
        ret
    vanilla_browser_backend:
        push 0x00585640
        ret
    }
}

__declspec(naked) void NativeCoopBrowserFrameHook() {
    __asm {
        cmp byte ptr [gCoopUiActive], 0
        je vanilla_browser_frame
        pushfd
        pushad
        push esi
        call RefreshNativeCoopBrowser
        add esp, 4
        popad
        popfd
        // Return through the frame handler's original epilogue; no native pump.
        push 0x004FB8E5
        ret
    vanilla_browser_frame:
        mov eax, dword ptr [esi + 0xfc]
        test eax, eax
        push 0x004FB825
        ret
    }
}

// Native ServerSelect accept and double-click dispatch both reach 4FB6D4.
// This is an explicit user action; server-record lookup at 4F31EF is only
// list population and must never initiate a join as a side effect.
__declspec(naked) void NativeCoopServerSelectJoinHook() {
    __asm {
        cmp byte ptr [gCoopUiActive], 0
        je vanilla_server_join
        pushfd
        pushad
        call BeginNativeCoopJoin
        popad
        popfd
        push 0x004FB8E5 // Original epilogue: pop edi; pop esi; ret 0x10.
        ret
    vanilla_server_join:
        lea edi, [esi + 0x4c]
        mov ecx, edi
        push 0x004FB6D9
        ret
    }
}

bool WriteNativeCoopLobbyPatch(uintptr_t address, const BYTE* expected, size_t length,
    void* replacement, BYTE opcode) {
    BYTE actual[8]{};
    if (length < 5 || length > sizeof(actual) || !ReadGameMemory(address, actual, length) ||
        std::memcmp(actual, expected, length) != 0) return false;
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), length, PAGE_EXECUTE_READWRITE, &protect)) return false;
    BYTE* target = reinterpret_cast<BYTE*>(address);
    target[0] = opcode;
    const int32_t relative = static_cast<int32_t>(reinterpret_cast<uintptr_t>(replacement) - address - 5);
    std::memcpy(target + 1, &relative, sizeof(relative));
    for (size_t i = 5; i < length; ++i) target[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, length);
    DWORD ignored = 0;
    VirtualProtect(target, length, protect, &ignored);
    return true;
}

void RestoreNativeCoopLobbyPatch(uintptr_t address, const BYTE* bytes, size_t length) {
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), length, PAGE_EXECUTE_READWRITE, &protect)) return;
    std::memcpy(reinterpret_cast<void*>(address), bytes, length);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), length);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(address), length, protect, &ignored);
}

void* MakeNativeCoopLobbyTrampoline(uintptr_t address, const BYTE* bytes, size_t length) {
    BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, length + 5,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return nullptr;
    std::memcpy(trampoline, bytes, length); // Verified stolen spans contain no relative instructions.
    trampoline[length] = 0xe9;
    const int32_t back = static_cast<int32_t>(address + length -
        (reinterpret_cast<uintptr_t>(trampoline) + length + 5));
    std::memcpy(trampoline + length + 1, &back, sizeof(back));
    FlushInstructionCache(GetCurrentProcess(), trampoline, length + 5);
    return trampoline;
}

bool InstallNativeCoopLobbyHook(NativeCoopSnapshotProvider snapshotProvider) {
    if (!snapshotProvider) return false;
    if (gNativeCoopRefreshOriginal) return true;
    const BYTE refresh[] = {0x81, 0xec, 0x90, 0x00, 0x00, 0x00};
    const BYTE destructor[] = {0xe8, 0x08, 0xd4, 0xfe, 0xff}; // CALL 4D12C0 at 4E3EB3.
    const BYTE join[] = {0x80, 0x3d, 0x98, 0x00, 0x85, 0x00, 0x01};
    const BYTE serverJoin[] = {0x8d, 0x7e, 0x4c, 0x8b, 0xcf};
    const BYTE monitorInit[] = {0xe8, 0x80, 0xea, 0xfd, 0xff};
    const BYTE monitorNotify[] = {0xe8, 0x26, 0x2a, 0xff, 0xff};
    const BYTE browserBackend[] = {0xe8, 0x92, 0xa0, 0x08, 0x00};
    const BYTE browserFrame[] = {0x8b, 0x86, 0xfc, 0x00, 0x00, 0x00, 0x85, 0xc0};
    const BYTE playersCounter[] = {0xe8, 0x5d, 0xa9, 0x03, 0x00};
    const BYTE gamesCounter[] = {0xe8, 0x3c, 0xa9, 0x03, 0x00};
    BYTE monitorActual[5]{};
    BYTE browserActual[8]{};
    if (!ReadGameMemory(0x004f12fb, monitorActual, 5) ||
        std::memcmp(monitorActual, monitorInit, 5) != 0 ||
        !ReadGameMemory(0x004fd415, monitorActual, 5) ||
        std::memcmp(monitorActual, monitorNotify, 5) != 0 ||
        !ReadGameMemory(0x004fb5a9, browserActual, 5) ||
        std::memcmp(browserActual, browserBackend, 5) != 0 ||
        !ReadGameMemory(0x004fb81d, browserActual, 8) ||
        std::memcmp(browserActual, browserFrame, 8) != 0 ||
        !ReadGameMemory(0x004fd21e, browserActual, 5) ||
        std::memcmp(browserActual, playersCounter, 5) != 0 ||
        !ReadGameMemory(0x004fd23f, browserActual, 5) ||
        std::memcmp(browserActual, gamesCounter, 5) != 0) goto failed;
    gNativeCoopRefreshOriginal = MakeNativeCoopLobbyTrampoline(0x004d1380, refresh, sizeof(refresh));
    gNativeCoopJoinOriginal = MakeNativeCoopLobbyTrampoline(0x0049edd0, join, sizeof(join));
    if (!gNativeCoopRefreshOriginal || !gNativeCoopJoinOriginal) goto failed;
    gNativeCoopSnapshotProvider = snapshotProvider;
    if (!WriteNativeCoopLobbyPatch(0x004d1380, refresh, sizeof(refresh), &NativeCoopLobbyRefreshHook, 0xe9))
        goto failed;
    if (!WriteNativeCoopLobbyPatch(0x004e3eb3, destructor, sizeof(destructor), &NativeCoopLobbyDestructorHook, 0xe8)) {
        RestoreNativeCoopLobbyPatch(0x004d1380, refresh, sizeof(refresh));
        goto failed;
    }
    if (!WriteNativeCoopLobbyPatch(0x0049edd0, join, sizeof(join), &NativeCoopLobbyJoinHook, 0xe9)) {
        RestoreNativeCoopLobbyPatch(0x004e3eb3, destructor, sizeof(destructor));
        RestoreNativeCoopLobbyPatch(0x004d1380, refresh, sizeof(refresh));
        goto failed;
    }
    if (!WriteNativeCoopLobbyPatch(0x004fb6d4, serverJoin, sizeof(serverJoin), &NativeCoopServerSelectJoinHook, 0xe9)) {
        RestoreNativeCoopLobbyPatch(0x0049edd0, join, sizeof(join));
        RestoreNativeCoopLobbyPatch(0x004e3eb3, destructor, sizeof(destructor));
        RestoreNativeCoopLobbyPatch(0x004d1380, refresh, sizeof(refresh));
        goto failed;
    }
    if (!WriteNativeCoopLobbyPatch(0x004f12fb, monitorInit, 5, &NativeCoopMonitorInitHook, 0xe8) ||
        !WriteNativeCoopLobbyPatch(0x004fd415, monitorNotify, 5, &NativeCoopMonitorNotifyHook, 0xe8) ||
        !WriteNativeCoopLobbyPatch(0x004fb5a9, browserBackend, 5, &NativeCoopBrowserBackendHook, 0xe8) ||
        !WriteNativeCoopLobbyPatch(0x004fb81d, browserFrame, 8, &NativeCoopBrowserFrameHook, 0xe9) ||
        !WriteNativeCoopLobbyPatch(0x004fd21e, playersCounter, 5, &NativeCoopCounterLabelHook, 0xe8) ||
        !WriteNativeCoopLobbyPatch(0x004fd23f, gamesCounter, 5, &NativeCoopCounterLabelHook, 0xe8)) {
        RestoreNativeCoopLobbyPatch(0x004f12fb, monitorInit, 5);
        RestoreNativeCoopLobbyPatch(0x004fd415, monitorNotify, 5);
        RestoreNativeCoopLobbyPatch(0x004fb5a9, browserBackend, 5);
        RestoreNativeCoopLobbyPatch(0x004fb81d, browserFrame, 8);
        RestoreNativeCoopLobbyPatch(0x004fd21e, playersCounter, 5);
        RestoreNativeCoopLobbyPatch(0x004fd23f, gamesCounter, 5);
        RestoreNativeCoopLobbyPatch(0x004fb6d4, serverJoin, sizeof(serverJoin));
        RestoreNativeCoopLobbyPatch(0x0049edd0, join, sizeof(join));
        RestoreNativeCoopLobbyPatch(0x004e3eb3, destructor, sizeof(destructor));
        RestoreNativeCoopLobbyPatch(0x004d1380, refresh, sizeof(refresh));
        goto failed;
    }
    LogMessage("[COOP_UI] Native row text flush and UDP browser refresh enabled; native TCP lifecycle bypassed for COOP only.\r\n");
    return true;
failed:
    if (gNativeCoopRefreshOriginal) VirtualFree(gNativeCoopRefreshOriginal, 0, MEM_RELEASE);
    if (gNativeCoopJoinOriginal) VirtualFree(gNativeCoopJoinOriginal, 0, MEM_RELEASE);
    gNativeCoopRefreshOriginal = gNativeCoopJoinOriginal = nullptr;
    gNativeCoopSnapshotProvider = nullptr;
    LogMessage("[COOP_UI] Native lobby byte checks failed; hooks not installed.\r\n");
    return false;
}
