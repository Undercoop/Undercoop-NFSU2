// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.

// Included inside CoopModManager after the confined file helpers. A loader
// already running in the game is a prerequisite, not a downloaded executable
// to trust by one preview build's hash. Bind its required overlay ABI and keep
// its actual game-root file locked for the lifetime of the activated setup.
struct LoaderApi {
    decltype(&::GetModuleHandleW) module = &::GetModuleHandleW;
    decltype(&::GetModuleFileNameW) path = &::GetModuleFileNameW;
    decltype(&::GetProcAddress) procedure = &::GetProcAddress;
};
struct LoaderBinding {
    AddPath add = nullptr;
    RemovePath remove = nullptr;
    std::unique_ptr<Handle> file;
};
inline bool BindActiveLoader(LoaderBinding& binding, const LoaderApi& api = {}) {
    binding = LoaderBinding{};
    const auto module = api.module(L"dinput8.dll");
    if (!module) return Fail(L"The ASI loader is not active. Install Ultimate ASI Loader x86 as dinput8.dll beside SPEED2.EXE.");
    wchar_t path[MAX_PATH]{};
    const auto length = api.path(module, path, DWORD(std::size(path)));
    if (!length || length >= std::size(path) || !Equal(path, state.root + L"\\dinput8.dll"))
        return Fail(L"The active ASI loader must be dinput8.dll beside SPEED2.EXE. Check the loader installation.");
    auto file = Pin(path);
    if (!file) return Fail(L"Cannot verify the active ASI loader file. Check access to dinput8.dll and restart the game.");
    const auto add = reinterpret_cast<AddPath>(api.procedure(module, "AddVirtualPathForOverloadW"));
    const auto remove = reinterpret_cast<RemovePath>(api.procedure(module, "RemoveVirtualPathFromOverloadW"));
    const auto marker = reinterpret_cast<bool(WINAPI*)()>(api.procedure(module, "IsUltimateASILoader"));
    if (!add || !remove || !marker)
        return Fail(L"This ASI loader lacks the required virtual-file APIs. Install a current Ultimate ASI Loader x86 as dinput8.dll beside SPEED2.EXE, then restart.");
    if (!marker()) return Fail(L"The active ASI loader did not identify as Ultimate ASI Loader. Install a compatible x86 loader and restart.");
    binding.add = add;
    binding.remove = remove;
    binding.file = std::move(file);
    return true;
}
