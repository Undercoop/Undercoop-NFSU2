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
            root = parent_ + L"\\UndercoopLoaderTests-" + std::to_wstring(GetCurrentProcessId()) +
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

// A fake already-loaded module exports functions with the real UAL ABI. No
// operating-system loader is invoked and no plugin code is loaded by tests.
struct Scenario {
    std::wstring path;
    bool present = true, add = true, remove = true, marker = true, accepted = true;
    DWORD pathResult = MAXDWORD;
    unsigned markerCalls = 0, exportCalls = 0, pathCalls = 0;
    bool expectedName = true, expectedModule = true;
};
static Scenario scenario;
static HMODULE Module() { return reinterpret_cast<HMODULE>(uintptr_t(0x12340000)); }
static HMODULE WINAPI FakeModule(LPCWSTR name) {
    scenario.expectedName = scenario.expectedName && std::wcscmp(name, L"dinput8.dll") == 0;
    return scenario.present ? Module() : nullptr;
}
static DWORD WINAPI FakePath(HMODULE module, LPWSTR output, DWORD size) {
    ++scenario.pathCalls;
    scenario.expectedModule = scenario.expectedModule && module == Module();
    if (size) {
        const auto characters = (std::min)(scenario.path.size(), size_t(size - 1));
        std::copy_n(scenario.path.data(), characters, output);
        output[characters] = 0;
    }
    return scenario.pathResult == MAXDWORD ? DWORD(scenario.path.size()) : scenario.pathResult;
}
static bool WINAPI FakeAdd(const wchar_t*, const wchar_t*, int) { return true; }
static void WINAPI FakeRemove(const wchar_t*) {}
static bool WINAPI FakeMarker() { ++scenario.markerCalls; return scenario.accepted; }
static FARPROC WINAPI FakeProcedure(HMODULE module, LPCSTR name) {
    ++scenario.exportCalls;
    scenario.expectedModule = scenario.expectedModule && module == Module();
    if (!std::strcmp(name, "AddVirtualPathForOverloadW") && scenario.add) return reinterpret_cast<FARPROC>(&FakeAdd);
    if (!std::strcmp(name, "RemoveVirtualPathFromOverloadW") && scenario.remove) return reinterpret_cast<FARPROC>(&FakeRemove);
    if (!std::strcmp(name, "IsUltimateASILoader") && scenario.marker) return reinterpret_cast<FARPROC>(&FakeMarker);
    return nullptr;
}
static const manager::LoaderApi api{FakeModule, FakePath, FakeProcedure};
static void Setup(Fixture& fixture) {
    Reset();
    manager::state.root = fixture.root;
    scenario = Scenario{};
    scenario.path = fixture.Path(L"dinput8.dll");
}
static void Empty(const manager::LoaderBinding& binding) {
    CHECK(!binding.add);
    CHECK(!binding.remove);
    CHECK(!binding.file);
}
static void NoChanges(Fixture& fixture) {
    CHECK(GetFileAttributesW(fixture.Path(L"mods").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(fixture.Path(L"global.ini").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(fixture.Path(L"Saves").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(nativeLoads == 0);
}
static void CompatibleVersions() {
    Fixture fixture;
    Setup(fixture);
    for (const auto* bytes : {"compatible first version", "different compatible version, no SHA allowlist"}) {
        fixture.File(L"dinput8.dll", bytes);
        manager::LoaderBinding binding;
        CHECK(manager::BindActiveLoader(binding, api));
        CHECK(binding.add == &FakeAdd);
        CHECK(binding.remove == &FakeRemove);
        CHECK(binding.file && binding.file->value != INVALID_HANDLE_VALUE);
        CHECK(scenario.expectedName);
        CHECK(scenario.expectedModule);
        std::string actual;
        CHECK(manager::ReadText(fixture.Path(L"dinput8.dll"), actual));
        CHECK(actual == bytes);
        NoChanges(fixture);
    }
    CHECK(scenario.markerCalls == 2);
    CHECK(scenario.exportCalls == 6);
}
static void MissingCapabilities() {
    Fixture fixture;
    fixture.File(L"dinput8.dll", "plain compatible fixture");
    for (unsigned missing = 0; missing != 4; ++missing) {
        Setup(fixture);
        if (missing == 0) scenario.present = false;
        if (missing == 1) scenario.add = false;
        if (missing == 2) scenario.remove = false;
        if (missing == 3) scenario.marker = false;
        manager::LoaderBinding binding;
        CHECK(!manager::BindActiveLoader(binding, api));
        Empty(binding);
        CHECK(!manager::state.error.empty());
        CHECK(scenario.markerCalls == 0);
        CHECK(scenario.exportCalls == (missing ? 3u : 0u));
        NoChanges(fixture);
    }
    Setup(fixture);
    scenario.accepted = false;
    manager::LoaderBinding binding;
    CHECK(!manager::BindActiveLoader(binding, api));
    CHECK(scenario.markerCalls == 1);
    Empty(binding);
    CHECK(manager::state.error.find(L"identify") != std::wstring::npos);
    NoChanges(fixture);
}
static void ModuleLocation() {
    Fixture fixture;
    fixture.File(L"dinput8.dll", "plain compatible fixture");
    for (unsigned failure = 0; failure != 5; ++failure) {
        Setup(fixture);
        if (failure == 0) scenario.path = fixture.Path(L"other.dll");
        if (failure == 1) scenario.path = L"C:\\Windows\\System32\\dinput8.dll";
        if (failure == 2) scenario.pathResult = 0;
        if (failure == 3) scenario.pathResult = MAX_PATH;
        if (failure == 4) scenario.path = std::wstring(MAX_PATH + 50, L'x');
        manager::LoaderBinding binding;
        CHECK(!manager::BindActiveLoader(binding, api));
        Empty(binding);
        CHECK(scenario.markerCalls == 0);
        CHECK(scenario.exportCalls == 0);
        CHECK(!manager::state.error.empty());
        NoChanges(fixture);
    }
    Setup(fixture);
    for (auto& character : scenario.path) if (character >= L'a' && character <= L'z') character -= 32;
    manager::LoaderBinding binding;
    CHECK(manager::BindActiveLoader(binding, api));
    CHECK(scenario.markerCalls == 1);
}
static void MissingAndLinkedFiles() {
    Fixture fixture;
    Setup(fixture);
    manager::LoaderBinding binding;
    CHECK(!manager::BindActiveLoader(binding, api));
    Empty(binding);
    CHECK(scenario.exportCalls == 0);
    CHECK(scenario.markerCalls == 0);
    CHECK(manager::state.error.find(L"verify") != std::wstring::npos);
    NoChanges(fixture);
    fixture.Directory(L"dinput8.dll");
    CHECK(!manager::BindActiveLoader(binding, api));
    Empty(binding);
    CHECK(RemoveDirectoryW(fixture.Path(L"dinput8.dll").c_str()) != FALSE);
    const auto actual = fixture.Directory(L"actual");
    fixture.File(L"actual\\dinput8.dll", "real loader file behind junction");
    const auto linked = fixture.Directory(L"linked");
    Need(Junction(linked, actual), "create confined junction fixture");
    manager::state.root = linked;
    scenario.path = linked + L"\\dinput8.dll";
    CHECK(!manager::BindActiveLoader(binding, api));
    Empty(binding);
    CHECK(scenario.exportCalls == 0);
    CHECK(scenario.markerCalls == 0);
    NoChanges(fixture);
}
static void PinLifetimeAndRetry() {
    Fixture fixture;
    Setup(fixture);
    const auto loader = fixture.File(L"dinput8.dll", "leave this fixture intact");
    manager::LoaderBinding binding;
    CHECK(manager::BindActiveLoader(binding, api));
    const auto writer = CreateFileW(loader.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(writer == INVALID_HANDLE_VALUE);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    CHECK(!DeleteFileW(loader.c_str()));
    CHECK(GetLastError() == ERROR_SHARING_VIOLATION);
    scenario.present = false;
    CHECK(!manager::BindActiveLoader(binding, api));
    Empty(binding);
    CHECK(scenario.markerCalls == 1);
    const auto after = CreateFileW(loader.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(after != INVALID_HANDLE_VALUE);
    if (after != INVALID_HANDLE_VALUE) CloseHandle(after);
    CHECK(DeleteFileW(loader.c_str()) != FALSE);
    NoChanges(fixture);
}
int main() {
    CompatibleVersions();
    MissingCapabilities();
    ModuleLocation();
    MissingAndLinkedFiles();
    PinLifetimeAndRetry();
    CHECK(nativeLoads == 0);
    std::printf("Manager loader tests: %u checks, %u failures, %u native loads.\n",
        checks, failures, nativeLoads.load());
    return failures ? 1 : 0;
}
