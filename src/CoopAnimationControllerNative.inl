// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.

// Included in the game adapter after its bounded memory/focus helpers.
// Only the verified Animation Controller 1.2.1 query paths are adapted. The
// plugin's input loop, animation actions and settings remain its own.
namespace CoopAnimationControllerNative {
static uintptr_t stateResume = 0, blockedResume = 0;
static std::atomic<unsigned> busyQueries{0};
static coop_animation_module::Proof proof;
static std::atomic_flag installing = ATOMIC_FLAG_INIT;
static bool installed = false;
static HMODULE rejected = nullptr;
static DWORD attemptedAt = 0, reportedAt = 0;

static uint32_t __fastcall PartState(uint32_t part) {
    const auto value = IsGameWindowFocused() ? coop_animation_scene::Query(
        [](uintptr_t at, void* out, size_t bytes) { return ReadGameMemory(at, out, bytes); }, part)
        : coop_animation_scene::kBusy;
    if (value == coop_animation_scene::kBusy) busyQueries.fetch_add(1, std::memory_order_relaxed);
    return value;
}

// This path is inside the original command's established stack frame. ESI is
// its part argument; the two continuations respectively consume EAX as a valid
// state or leave through the original epilogue without starting an animation.
static __declspec(naked) void InlineState() {
    __asm {
        mov ecx, esi
        call PartState
        cmp eax, 2
        je blocked
        jmp dword ptr [stateResume]
    blocked:
        jmp dword ptr [blockedResume]
    }
}

// Prepare all handles before pausing threads. No allocation, loader calls or
// logging occurs while another thread can hold a runtime/loader lock. If any
// thread is already in the replaced query, postpone rather than moving its IP.
class PausedThreads {
    struct Entry { HANDLE handle = nullptr; bool paused = false; };
    std::array<Entry,256> threads{};
    size_t count = 0;
public:
    PausedThreads() = default;
    PausedThreads(const PausedThreads&) = delete;
    ~PausedThreads() {
        for (size_t i = count; i; --i) {
            auto& thread = threads[i-1];
            if (thread.paused) ResumeThread(thread.handle);
            if (thread.handle) CloseHandle(thread.handle);
        }
    }
    bool Pause(uintptr_t base) {
        const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        THREADENTRY32 row{}; row.dwSize = sizeof(row);
        bool okay = Thread32First(snapshot, &row) != FALSE;
        if (okay) {
            do {
                if (row.th32OwnerProcessID != GetCurrentProcessId() || row.th32ThreadID == GetCurrentThreadId()) continue;
                if (count == threads.size()) { okay = false; break; }
                const auto handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, row.th32ThreadID);
                if (!handle) { okay = false; break; }
                threads[count++].handle = handle;
            } while (Thread32Next(snapshot, &row));
            if (okay) okay = GetLastError() == ERROR_NO_MORE_FILES;
        }
        CloseHandle(snapshot);
        if (!okay) return false;
        for (size_t i = 0; i < count; ++i) {
            auto& thread = threads[i];
            if (SuspendThread(thread.handle) == DWORD(-1)) return false;
            thread.paused = true;
            CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(thread.handle, &context)) return false;
            const uintptr_t ip = context.Eip;
            if ((ip >= base+0x36A0 && ip < base+0x36E7) ||
                (ip >= base+0x370C && ip < base+0x3738) ||
                (ip >= base+0x37F5 && ip < base+0x380B)) return false;
        }
        return true;
    }
};

enum class PatchResult { Applied, Retry, Changed, Failed };
static void Jump(uint8_t* out, uintptr_t site, uintptr_t target, size_t size) {
    std::memset(out, 0x90, size); out[0] = 0xE9;
    const uint32_t relative = uint32_t(target) - uint32_t(site+5);
    std::memcpy(out+1, &relative, sizeof(relative));
}
static PatchResult Apply(uintptr_t base) {
    const auto query = base + 0x36A0, inlined = base + 0x370C;
    std::array<uint8_t,5> first{};
    std::array<uint8_t,8> second{};
    Jump(first.data(), query, reinterpret_cast<uintptr_t>(&PartState), first.size());
    Jump(second.data(), inlined, reinterpret_cast<uintptr_t>(&InlineState), second.size());
    PausedThreads paused;
    if (!paused.Pause(base)) return PatchResult::Retry;
    std::array<uint8_t,5> currentQuery{}, currentExit{}; std::array<uint8_t,8> currentInline{};
    std::array<uint8_t,3> currentResume{};
    if (!ReadGameMemory(query,currentQuery.data(),currentQuery.size()) ||
        !ReadGameMemory(inlined,currentInline.data(),currentInline.size()) ||
        !ReadGameMemory(base+0x3738,currentResume.data(),currentResume.size()) ||
        !ReadGameMemory(base+0x37F0,currentExit.data(),currentExit.size()) ||
        currentQuery != coop_animation_module::kQuery || currentInline != coop_animation_module::kInline ||
        currentResume != coop_animation_module::kResume || currentExit != coop_animation_module::kExit)
        return PatchResult::Changed;
    DWORD previous = 0;
    auto* page = reinterpret_cast<void*>(base+0x3000);
    if (!VirtualProtect(page,0x1000,PAGE_EXECUTE_READWRITE,&previous)) return PatchResult::Failed;
    stateResume = base+0x3738; blockedResume = base+0x37F0;
    std::memcpy(reinterpret_cast<void*>(query),first.data(),first.size());
    std::memcpy(reinterpret_cast<void*>(inlined),second.data(),second.size());
    const bool flushed = FlushInstructionCache(GetCurrentProcess(),page,0x1000) != FALSE;
    if (!flushed) {
        std::memcpy(reinterpret_cast<void*>(query),currentQuery.data(),currentQuery.size());
        std::memcpy(reinterpret_cast<void*>(inlined),currentInline.data(),currentInline.size());
        FlushInstructionCache(GetCurrentProcess(),page,0x1000);
    }
    DWORD unused = 0;
    bool restored = VirtualProtect(page,0x1000,previous,&unused) != FALSE;
    if (!restored) restored = VirtualProtect(page,0x1000,previous,&unused) != FALSE;
    if (!restored) {
        // Both failed restore calls leave the page writable. Withdraw both
        // detours before any thread resumes, rather than reporting success.
        std::memcpy(reinterpret_cast<void*>(query),currentQuery.data(),currentQuery.size());
        std::memcpy(reinterpret_cast<void*>(inlined),currentInline.data(),currentInline.size());
        FlushInstructionCache(GetCurrentProcess(),page,0x1000);
        VirtualProtect(page,0x1000,previous,&unused);
        return PatchResult::Failed;
    }
    return flushed ? PatchResult::Applied : PatchResult::Failed;
}

static void Poll() {
    if (installing.test_and_set(std::memory_order_acquire)) return;
    struct Release { ~Release(){installing.clear(std::memory_order_release);} } release;
    const DWORD now = GetTickCount();
    if (installed) {
        if (now-reportedAt >= 5000) {
            reportedAt=now; const auto count=busyQueries.exchange(0,std::memory_order_relaxed);
            if (count) {
                char line[176]{}; std::snprintf(line,sizeof(line),
                    "[ANIMATION_COMPAT] Busy/unavailable scene queries deferred=%u; native animation actions retained.\r\n",count);
                LogMessage(line);
            }
        }
        return;
    }
    if (attemptedAt && now-attemptedAt < 500) return;
    attemptedAt=now;
    const auto module=GetModuleHandleW(L"AnimationController.asi");
    if (!module || module==rejected) return;
    if (!proof.module && !coop_animation_module::Verify(module,proof)) {
        rejected=module;
        LogMessage("[ANIMATION_COMPAT] Plugin identity or owned query code differs from supported 1.2.1; no code changed.\r\n");
        return;
    }
    HMODULE resident=nullptr;
    if (proof.module!=module || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(proof.base),&resident) || resident!=module) return;
    const auto result=Apply(proof.base);
    if (result==PatchResult::Retry) return;
    if (result!=PatchResult::Applied) {
        rejected=module; proof={};
        LogMessage("[ANIMATION_COMPAT] Query installation refused or failed; original plugin code retained.\r\n");
        return;
    }
    installed=true; reportedAt=now;
    LogMessage("[ANIMATION_COMPAT] Verified Animation Controller 1.2.1: both scene-query paths guarded; absent/loading scenes defer animation.\r\n");
}
}
