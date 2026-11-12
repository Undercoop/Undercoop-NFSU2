// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

// Exercise preparation and discovery without loading a plugin, opening the
// launcher, launching the game, or touching any existing game installation.
static std::atomic<unsigned> nativeLoads{0};
static HMODULE WINAPI NeverLoadLibraryExW(LPCWSTR, HANDLE, DWORD) { ++nativeLoads; return nullptr; }
static HMODULE WINAPI NeverLoadLibraryExA(LPCSTR, HANDLE, DWORD) { ++nativeLoads; return nullptr; }
static HMODULE WINAPI NeverLoadLibraryW(LPCWSTR) { ++nativeLoads; return nullptr; }
static HMODULE WINAPI NeverLoadLibraryA(LPCSTR) { ++nativeLoads; return nullptr; }
#define LoadLibraryExW NeverLoadLibraryExW
#define LoadLibraryExA NeverLoadLibraryExA
#define LoadLibraryW NeverLoadLibraryW
#define LoadLibraryA NeverLoadLibraryA
#include "../src/CoopModManagerWin.h"
#undef LoadLibraryExW
#undef LoadLibraryExA
#undef LoadLibraryW
#undef LoadLibraryA

namespace manager = CoopModManager;
static unsigned checks = 0;
static unsigned failures = 0;

static void Check(bool okay, const char* expression, int line) {
    ++checks;
    if (okay) return;
    ++failures;
    std::printf("FAIL line %d: %s (Win32 %lu)\n", line, expression, GetLastError());
}
#define CHECK(...) Check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

static void Need(bool okay, const char* why) {
    if (okay) return;
    std::printf("Fixture error: %s (Win32 %lu)\n", why, GetLastError());
    std::exit(2);
}

static void Reset() {
    manager::state = manager::State{};
    coop_mod_scan::effectivePath = nullptr;
    coop_mod_scan::managedChange = nullptr;
    coop_mod_scan::additionalFiles = nullptr;
}

class Fixture {
    std::wstring parent_;

    void Clean(const std::wstring& path) {
        Need(manager::Equal(path, root) || manager::Below(path, root), "cleanup containment");
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return;
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            Need(DeleteFileW(path.c_str()) != FALSE, "remove owned fixture file");
            return;
        }
        if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            WIN32_FIND_DATAW row{};
            const auto find = FindFirstFileW((path + L"\\*").c_str(), &row);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (std::wcscmp(row.cFileName, L".") && std::wcscmp(row.cFileName, L".."))
                        Clean(path + L"\\" + row.cFileName);
                } while (FindNextFileW(find, &row));
                FindClose(find);
            }
        }
        Need(RemoveDirectoryW(path.c_str()) != FALSE, "remove owned fixture directory");
    }

public:
    std::wstring root;

    Fixture() {
        wchar_t temp[MAX_PATH]{};
        const auto length = GetTempPathW(MAX_PATH, temp);
        Need(length && length < MAX_PATH, "get TEMP path");
        parent_ = temp;
        while (!parent_.empty() && parent_.back() == L'\\') parent_.pop_back();
        Need(manager::RealPath(parent_, true), "plain TEMP parent");
        for (unsigned index = 0; index < 100; ++index) {
            root = parent_ + L"\\UndercoopBootstrapTests-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(index);
            Need(root.size() < 150 && manager::Below(root, parent_), "unique TEMP child");
            if (CreateDirectoryW(root.c_str(), nullptr)) return;
            Need(GetLastError() == ERROR_ALREADY_EXISTS, "create TEMP fixture");
        }
        Need(false, "unique fixture budget");
    }

    ~Fixture() {
        Reset();
        Need(manager::Below(root, parent_) && manager::RealPath(root, true), "final cleanup boundary");
        Clean(root);
    }

    std::wstring Path(const std::wstring& relative) const {
        const auto path = root + L"\\" + relative;
        wchar_t absolute[MAX_PATH]{};
        const auto length = GetFullPathNameW(path.c_str(), MAX_PATH, absolute, nullptr);
        Need(length && length < MAX_PATH && manager::Equal(path, absolute) && manager::Below(path, root),
            "fixture path containment");
        return path;
    }

    std::wstring Directory(const std::wstring& relative) {
        const auto path = Path(relative);
        for (auto index = root.size() + 1; index <= path.size(); ++index) {
            if (index != path.size() && path[index] != L'\\') continue;
            const auto prefix = path.substr(0, index);
            if (!CreateDirectoryW(prefix.c_str(), nullptr))
                Need(GetLastError() == ERROR_ALREADY_EXISTS && manager::RealPath(prefix, true), "create plain directory");
        }
        return path;
    }

    std::wstring File(const std::wstring& relative, const std::string& bytes) {
        const auto split = relative.find_last_of(L'\\');
        if (split != std::wstring::npos) Directory(relative.substr(0, split));
        const auto path = Path(relative);
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            Need(manager::RealPath(path), "replace only own plain file");
        manager::Handle handle(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        Need(handle.value != INVALID_HANDLE_VALUE, "create owned fixture file");
        DWORD written = 0;
        Need(WriteFile(handle.value, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size(),
            "write fixture data");
        return path;
    }
};

static std::string Read(const std::wstring& path) {
    std::string bytes;
    Need(manager::ReadText(path, bytes), "read fixture file");
    return bytes;
}

static bool Missing(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
}

static bool Junction(const std::wstring& link, const std::wstring& target) {
    struct MountPoint {
        DWORD tag;
        WORD bytes, reserved, subOffset, subBytes, printOffset, printBytes;
        wchar_t paths[MAX_PATH * 2 + 8];
    } data{};
    const auto substitute = L"\\??\\" + target;
    const auto characters = substitute.size() + target.size() + 2;
    if (characters > std::size(data.paths)) return false;
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.subBytes = WORD(substitute.size() * sizeof(wchar_t));
    data.printOffset = WORD(data.subBytes + sizeof(wchar_t));
    data.printBytes = WORD(target.size() * sizeof(wchar_t));
    data.bytes = WORD(8 + characters * sizeof(wchar_t));
    std::memcpy(data.paths, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(reinterpret_cast<BYTE*>(data.paths) + data.printOffset, target.c_str(),
        (target.size() + 1) * sizeof(wchar_t));
    manager::Handle handle(CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    DWORD done = 0;
    return handle.value != INVALID_HANDLE_VALUE && DeviceIoControl(handle.value, FSCTL_SET_REPARSE_POINT,
        &data, DWORD(8 + data.bytes), nullptr, 0, &done, nullptr);
}

static void FreshInstall() {
    Fixture fixture;
    fixture.File(L"scripts\\NFSU2Coop.ini", "[COOP]\nProfile=NAME\n");
    CHECK(Missing(fixture.Path(L"mods")));
    CHECK(Missing(fixture.Path(L"global.ini")));
    CHECK(manager::Discover(fixture.root));
    CHECK(manager::state.catalog.empty());
    CHECK(manager::state.selected.empty());
    CHECK(manager::MakePlan());
    CHECK(manager::SaveSelection());
    CHECK(manager::RealPath(fixture.Path(L"mods"), true));
    CHECK(manager::RealPath(fixture.Path(L"mods\\_undercoop"), true));
    CHECK(manager::RealPath(fixture.Path(L"mods\\_undercoop\\state"), true));
    CHECK(GetPrivateProfileIntW(L"GlobalSets", L"DontLoadFromDllMain", 0,
        fixture.Path(L"global.ini").c_str()) == 1);
    CHECK(Read(fixture.Path(L"mods\\_undercoop\\state\\selection.txt")).empty());
    CHECK(Read(fixture.Path(L"scripts\\NFSU2Coop.ini")) == "[COOP]\nProfile=NAME\n");
    CHECK(Missing(fixture.Path(L"mods\\_undercoop\\no_static_overlay")));
    CHECK(nativeLoads == 0);
}

static void ExistingSetup() {
    Fixture fixture;
    const std::string global = "; User-owned loader settings\r\n[GlobalSets]\r\nDontLoadFromDllMain=1\r\nDebugLog=0\r\n[Other]\r\nValue=42\r\n";
    fixture.File(L"global.ini", global);
    fixture.File(L"scripts\\NFSU2Coop.ini", "[Network]\nPort=54321\n");
    fixture.File(L"mods\\retained\\undercoop.mod.ini",
        "[Package]\nId=retained\nName=Retained car\nVersion=1\nKind=assets\n[Files]\ncars/retained/geometry.bin=geometry.bin\n");
    fixture.File(L"mods\\retained\\geometry.bin", "personal-mod-resource");
    fixture.File(L"mods\\_undercoop\\state\\selection.txt", "retained\n");
    fixture.File(L"mods\\_undercoop\\modpacks\\user-pack\\current.txt", "user-revision");
    fixture.File(L"saves\\NAME\\NAME", "existing-career-progress");
    const auto first = GetFileAttributesW(fixture.Path(L"global.ini").c_str());
    CHECK(manager::BootstrapInstallation(fixture.root));
    CHECK(manager::BootstrapInstallation(fixture.root));
    CHECK(manager::Discover(fixture.root));
    CHECK(manager::state.catalog.size() == 1);
    CHECK(manager::state.selected == std::vector<std::string>{"retained"});
    CHECK(manager::MakePlan());
    CHECK(Read(fixture.Path(L"global.ini")) == global);
    CHECK(GetFileAttributesW(fixture.Path(L"global.ini").c_str()) == first);
    CHECK(Read(fixture.Path(L"scripts\\NFSU2Coop.ini")) == "[Network]\nPort=54321\n");
    CHECK(Read(fixture.Path(L"mods\\retained\\geometry.bin")) == "personal-mod-resource");
    CHECK(Read(fixture.Path(L"mods\\_undercoop\\state\\selection.txt")) == "retained\n");
    CHECK(Read(fixture.Path(L"mods\\_undercoop\\modpacks\\user-pack\\current.txt")) == "user-revision");
    CHECK(Read(fixture.Path(L"saves\\NAME\\NAME")) == "existing-career-progress");
    CHECK(nativeLoads == 0);
}

static void PartialInstall() {
    Fixture fixture;
    fixture.Directory(L"mods\\_undercoop");
    fixture.File(L"mods\\_undercoop\\archive-engine\\keep.txt", "existing-embedded-engine");
    CHECK(manager::BootstrapInstallation(fixture.root));
    CHECK(manager::RealPath(fixture.Path(L"mods\\_undercoop\\state"), true));
    CHECK(Read(fixture.Path(L"mods\\_undercoop\\archive-engine\\keep.txt")) == "existing-embedded-engine");
    CHECK(manager::Discover(fixture.root));
    CHECK(manager::MakePlan());
    CHECK(manager::SaveSelection());
}

static void OccupiedPaths() {
    for (const auto* conflict : {L"mods", L"mods\\_undercoop", L"mods\\_undercoop\\state"}) {
        Fixture fixture;
        fixture.File(conflict, "existing-file-at-directory-path");
        CHECK(!manager::BootstrapInstallation(fixture.root));
        CHECK(Read(fixture.Path(conflict)) == "existing-file-at-directory-path");
        CHECK(!manager::state.error.empty());
        CHECK(nativeLoads == 0);
    }
    Fixture fixture;
    fixture.Directory(L"global.ini");
    fixture.File(L"global.ini\\keep.txt", "existing-directory-at-file-path");
    CHECK(!manager::BootstrapInstallation(fixture.root));
    CHECK(Read(fixture.Path(L"global.ini\\keep.txt")) == "existing-directory-at-file-path");
}

static void LinkedPaths() {
    for (const auto* conflict : {L"mods", L"mods\\_undercoop", L"mods\\_undercoop\\state"}) {
        Fixture fixture;
        const auto destination = fixture.Directory(L"untouched-target");
        fixture.File(L"untouched-target\\keep.txt", "target-preserved");
        const auto root = fixture.Directory(L"game");
        const auto link = fixture.Directory(std::wstring(L"game\\") + conflict);
        Need(Junction(link, destination), "create fixture junction");
        CHECK(!manager::BootstrapInstallation(root));
        CHECK(Read(destination + L"\\keep.txt") == "target-preserved");
        CHECK(Missing(destination + L"\\state"));
        CHECK(Missing(destination + L"\\global.ini"));
    }
}

static void ConcurrentPreparation() {
    Fixture fixture;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> begin{false};
    bool result[2]{};
    auto prepare = [&](unsigned index) {
        ++ready;
        while (!begin.load()) SwitchToThread();
        result[index] = manager::BootstrapInstallation(fixture.root);
    };
    std::thread first(prepare, 0), second(prepare, 1);
    while (ready.load() != 2) SwitchToThread();
    begin = true;
    first.join();
    second.join();
    CHECK(result[0]);
    CHECK(result[1]);
    CHECK(manager::Discover(fixture.root));
    CHECK(manager::MakePlan());
    CHECK(manager::SaveSelection());
    CHECK(GetPrivateProfileIntW(L"GlobalSets", L"DontLoadFromDllMain", 0,
        fixture.Path(L"global.ini").c_str()) == 1);
    CHECK(nativeLoads == 0);
}

static void StartupMetadataInventory() {
    Fixture fixture;
    const auto freshRoot = fixture.Directory(L"fresh");
    const auto existingRoot = fixture.Directory(L"existing");
    for (const auto* setup : {L"fresh", L"existing"}) {
        fixture.File(std::wstring(setup) + L"\\SPEED2.EXE", "identical-game-bytes");
        fixture.File(std::wstring(setup) + L"\\GLOBAL.BIN", "identical-game-resource");
        fixture.File(std::wstring(setup) + L"\\scripts\\ExampleMod.ini", "[Tuning]\nMass=1000\n");
    }
    const std::string customGlobal = "; Existing loader configuration\r\n[GlobalSets]\r\nDontLoadFromDllMain=1\r\nDebugLog=1\r\nLoadRecursively=0\r\n[FileLoader]\r\nOverloadFromFolder=custom-folder\r\n";
    fixture.File(L"existing\\global.ini", customGlobal);
    CHECK(manager::BootstrapInstallation(freshRoot));
    CHECK(manager::BootstrapInstallation(existingRoot));
    CHECK(Read(existingRoot + L"\\global.ini") == customGlobal);
    CHECK(Read(freshRoot + L"\\global.ini") != customGlobal);
    CHECK(manager::Discover(freshRoot));
    CHECK(manager::MakePlan());
    CHECK(manager::SaveSelection());
    Reset();

    CHECK(!coop_mods::IncludeFile("global.ini"));
    CHECK(coop_mods::IncludeFile("scripts/global.ini"));
    const auto fresh = coop_mod_scan::Scan(freshRoot, "same-runtime-settings");
    const auto existing = coop_mod_scan::Scan(existingRoot, "same-runtime-settings");
    CHECK(fresh.Ready());
    CHECK(existing.Ready());
    CHECK(fresh.hash == existing.hash);
    CHECK(fresh.blob == existing.blob);
    CHECK(std::none_of(fresh.entries.begin(), fresh.entries.end(),
        [](const auto& entry) { return entry.path == "global.ini"; }));

    fixture.File(L"existing\\GLOBAL.BIN", "changed-game-resource");
    const auto changedGame = coop_mod_scan::Scan(existingRoot, "same-runtime-settings");
    CHECK(changedGame.Ready());
    CHECK(changedGame.hash != fresh.hash);
    fixture.File(L"existing\\GLOBAL.BIN", "identical-game-resource");

    fixture.File(L"existing\\scripts\\ExampleMod.ini", "[Tuning]\nMass=2000\n");
    const auto changedPlugin = coop_mod_scan::Scan(existingRoot, "same-runtime-settings");
    CHECK(changedPlugin.Ready());
    CHECK(changedPlugin.hash != fresh.hash);
    fixture.File(L"existing\\scripts\\ExampleMod.ini", "[Tuning]\nMass=1000\n");
    const auto restored = coop_mod_scan::Scan(existingRoot, "same-runtime-settings");
    CHECK(restored.Ready());
    CHECK(restored.hash == fresh.hash);
    CHECK(Read(existingRoot + L"\\global.ini") == customGlobal);
    CHECK(nativeLoads == 0);
}

int main() {
    FreshInstall();
    ExistingSetup();
    PartialInstall();
    OccupiedPaths();
    LinkedPaths();
    ConcurrentPreparation();
    StartupMetadataInventory();
    CHECK(nativeLoads == 0);
    std::printf("Manager bootstrap tests: %u checks, %u failures, %u native loads.\n",
        checks, failures, nativeLoads.load());
    return failures ? 1 : 0;
}
