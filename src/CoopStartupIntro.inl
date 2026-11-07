// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shlwapi.lib")

// WidescreenFix can replace GameFrame's CreateWindow call, bypassing the EXE
// import table entirely. Show from the first native UI tick with a ready game
// window instead. DllMain only records the resource module; no startup API hooks.
namespace CoopStartupIntro {
constexpr WORD kImageResource = 301;
constexpr DWORD kVisibleMilliseconds = 3000;
constexpr wchar_t kWindowClass[] = L"NFSU2Coop.StartupIntro";
static HMODULE gModule = nullptr;
static volatile LONG gStarted = 0;
static DWORD gLastError = 0;

struct ImageResource {
    IStream* stream = nullptr;
    Gdiplus::Image* image = nullptr;
    ~ImageResource() {
        // GDI+ can use the stream until the image has been destroyed.
        delete image;
        if (stream) stream->Release();
    }
    bool Load(HMODULE module) {
        const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(kImageResource), MAKEINTRESOURCEW(10));
        if (!resource) return false;
        const DWORD size = SizeofResource(module, resource);
        const HGLOBAL data = LoadResource(module, resource);
        const BYTE* bytes = data ? static_cast<const BYTE*>(LockResource(data)) : nullptr;
        if (!bytes || !size) return false;
        stream = SHCreateMemStream(bytes, size);
        if (!stream) return false;
        image = Gdiplus::Image::FromStream(stream);
        return image && image->GetLastStatus() == Gdiplus::Ok &&
            image->GetWidth() != 0 && image->GetHeight() != 0;
    }
};

static Gdiplus::Rect FitImage(int width, int height, UINT imageWidth, UINT imageHeight) {
    if (width <= 0 || height <= 0 || !imageWidth || !imageHeight)
        return Gdiplus::Rect(0, 0, 0, 0);
    const double scale = std::min(static_cast<double>(width) / imageWidth,
        static_cast<double>(height) / imageHeight);
    const int fittedWidth = std::max(1, static_cast<int>(imageWidth * scale + 0.5));
    const int fittedHeight = std::max(1, static_cast<int>(imageHeight * scale + 0.5));
    return Gdiplus::Rect((width - fittedWidth) / 2, (height - fittedHeight) / 2,
        fittedWidth, fittedHeight);
}

struct SplashState {
    Gdiplus::Image* image = nullptr;
    ULONGLONG firstPaintAt = 0;
    bool painted = false;
    bool done = false;
};

static LRESULT CALLBACK SplashWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    auto* state = reinterpret_cast<SplashState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        if (state && state->image) {
            Gdiplus::Graphics graphics(dc);
            graphics.Clear(Gdiplus::Color(255, 0, 0, 0));
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            const auto fit = FitImage(client.right, client.bottom,
                state->image->GetWidth(), state->image->GetHeight());
            if (graphics.DrawImage(state->image, fit) == Gdiplus::Ok && !state->painted) {
                state->firstPaintAt = GetTickCount64();
                state->painted = true;
            }
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_TIMER:
        if (state && state->painted &&
            GetTickCount64() - state->firstPaintAt >= kVisibleMilliseconds) {
            state->done = true;
        }
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        if (state) state->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

static bool WaitForSplash(HWND window, SplashState& state) {
    if (!state.painted || !SetTimer(window, 1, 20, nullptr)) return false;
    MSG message{};
    while (!state.done) {
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0) {
            if (received == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    KillTimer(window, 1);
    return GetTickCount64() - state.firstPaintAt >= kVisibleMilliseconds;
}

static bool Show(HWND gameWindow) {
    if (!gModule) return false;
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    const auto startupStatus = Gdiplus::GdiplusStartup(&token, &input, nullptr);
    if (startupStatus != Gdiplus::Ok) {
        gLastError = 0x10000 + static_cast<DWORD>(startupStatus);
        return false;
    }
    struct Shutdown {
        ULONG_PTR token;
        ~Shutdown() { Gdiplus::GdiplusShutdown(token); }
    } shutdown{token};
    ImageResource photo;
    if (!photo.Load(gModule)) {
        gLastError = 0x20000;
        return false; // Missing/corrupt resource must never block startup.
    }

    RECT client{};
    POINT origin{};
    if (!GetClientRect(gameWindow, &client) || !ClientToScreen(gameWindow, &origin) ||
        client.right <= 0 || client.bottom <= 0) {
        gLastError = GetLastError();
        return false;
    }
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = SplashWindowProc;
    windowClass.hInstance = gModule;
    windowClass.lpszClassName = kWindowClass;
    const ATOM classAtom = RegisterClassW(&windowClass);
    if (!classAtom) {
        gLastError = GetLastError();
        return false;
    }
    SplashState state;
    state.image = photo.image;
    // Owned, non-activating popup covers only the game's client area, preserving
    // focus and the normal window/title. Main-thread simulation resumes afterward.
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        MAKEINTRESOURCEW(classAtom), L"UNDERCOOP", WS_POPUP,
        origin.x, origin.y, client.right, client.bottom, gameWindow, nullptr, gModule, &state);
    bool completed = false;
    if (window) {
        ShowWindow(window, SW_SHOWNOACTIVATE);
        UpdateWindow(window);
        completed = WaitForSplash(window, state);
        if (!completed) gLastError = state.painted ? GetLastError() : 0x30000;
        DestroyWindow(window);
    } else {
        gLastError = GetLastError();
    }
    UnregisterClassW(kWindowClass, gModule);
    return completed;
}

static void Initialize(HMODULE module) {
    gModule = module;
}

// 0 = not ready/already attempted, 1 = displayed for three seconds, -1 = fail open.
static int OnFirstGameFrame(HWND gameWindow) {
    if (gStarted != 0) return 0; // No extra window queries during normal gameplay.
    if (!gModule || !gameWindow || !IsWindow(gameWindow) || !IsWindowVisible(gameWindow) ||
        GetWindowThreadProcessId(gameWindow, nullptr) != GetCurrentThreadId()) return 0;
    if (InterlockedCompareExchange(&gStarted, 1, 0) != 0) return 0;
    try {
        return Show(gameWindow) ? 1 : -1;
    } catch (...) {
        gLastError = 0x40000;
        return -1;
    }
}
} // namespace CoopStartupIntro
