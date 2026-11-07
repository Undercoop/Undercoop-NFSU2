// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Native fields for the launcher session. Credentials are never part of Settings.
constexpr int kDeveloperSection = 3;
constexpr UINT kDeveloperSubmitMessage = WM_APP + 253;
constexpr UINT kDeveloperBackMessage = WM_APP + 254;
constexpr wchar_t kDeveloperEditPrevious[] = L"Undercoop.Developer.Previous";
constexpr int kDeveloperEmailId = 9214;
constexpr int kDeveloperPasswordId = 9215;

struct DeveloperPanel {
    HWND email = nullptr, password = nullptr;
    HFONT font = nullptr;
    HBRUSH brush = nullptr;
    UINT dpi = 0;
    bool open = false;
    bool fieldsUnavailable = false;

    ~DeveloperPanel() {
        if (email && IsWindow(email)) DestroyWindow(email);
        if (password && IsWindow(password)) DestroyWindow(password);
        if (font) DeleteObject(font);
        if (brush) DeleteObject(brush);
    }
};

struct DeveloperLayout {
    UiRect card, email, password, button;

    explicit DeveloperLayout(const UiLayout& base) {
        card = {base.content, 248, (std::min)(base.ContentWidth(), 640.0f), 258};
        email = {card.x + 22, card.y + 90, card.w - 44, 36};
        password = {email.x, card.y + 153, email.w, email.h};
        button = {card.x + card.w - 148, card.y + 209, 126, 34};
    }
};

inline void ClearDeveloperPassword(HWND password) {
    if (!password) return;
    SetWindowTextW(password, L"");
    SendMessageW(password, EM_EMPTYUNDOBUFFER, 0, 0);
}

inline LRESULT CALLBACK DeveloperEditProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    const auto previous = reinterpret_cast<WNDPROC>(GetPropW(window, kDeveloperEditPrevious));
    if ((message == WM_COPY || message == WM_CUT) && GetDlgCtrlID(window) == kDeveloperPasswordId)
        return 0;
    if (message == WM_KEYDOWN) {
        HWND parent = GetParent(window);
        if (wp == VK_RETURN || wp == VK_ESCAPE) {
            SendMessageW(parent, wp == VK_RETURN ? kDeveloperSubmitMessage : kDeveloperBackMessage, 0, 0);
            return 0;
        }
        if (wp == VK_TAB) {
            const auto next = GetNextDlgTabItem(parent, window, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            if (next) SetFocus(next);
            return 0;
        }
        if (wp == VK_F1) {
            SendMessageW(parent, message, wp, lp);
            return 0;
        }
    }
    if (message == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) return 0;
    if (message == WM_NCDESTROY) RemovePropW(window, kDeveloperEditPrevious);
    return previous ? CallWindowProcW(previous, window, message, wp, lp) : DefWindowProcW(window, message, wp, lp);
}

template<class V> inline bool DeveloperPageOpen(const V& view) {
    return view.tab == 7 && view.undercoop.section == kDeveloperSection;
}

template<class V> inline HWND CreateDeveloperEdit(V& view, int id, DWORD extraStyle) {
    const auto edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | extraStyle,
        0, 0, 10, 10, view.window, reinterpret_cast<HMENU>(UINT_PTR(id)), view.module, nullptr);
    if (!edit) return nullptr;
    const auto previous = GetWindowLongPtrW(edit, GWLP_WNDPROC);
    if (!previous || !SetPropW(edit, kDeveloperEditPrevious, reinterpret_cast<HANDLE>(previous))) {
        DestroyWindow(edit);
        return nullptr;
    }
    SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DeveloperEditProc));
    SendMessageW(edit, EM_SETLIMITTEXT, id == kDeveloperPasswordId ? 1024 : 254, 0);
    if (id == kDeveloperPasswordId) SendMessageW(edit, EM_SETPASSWORDCHAR, 0x2022, 0);
    return edit;
}

template<class V> inline void SyncDeveloperEditors(V& view) {
    auto& panel = view.developerPanel;
    if (!view.window || !IsWindow(view.window)) return;
    const bool open = DeveloperPageOpen(view) && !view.notice.open && !view.developer.SignedIn();
    if (!open) {
        if (panel.open) ClearDeveloperPassword(panel.password);
        panel.open = false;
        if (panel.email) ShowWindow(panel.email, SW_HIDE);
        if (panel.password) ShowWindow(panel.password, SW_HIDE);
        return;
    }

    const UINT dpi = UINT(coop_overlay::WindowDpi(view.window));
    if (!panel.email) panel.email = CreateDeveloperEdit(view, kDeveloperEmailId, 0);
    if (!panel.password) panel.password = CreateDeveloperEdit(view, kDeveloperPasswordId, ES_PASSWORD);
    if (!panel.brush) panel.brush = CreateSolidBrush(RGB(30, 34, 40));
    panel.fieldsUnavailable = !panel.email || !panel.password || !panel.brush;
    if (panel.fieldsUnavailable) {
        if (panel.email) ShowWindow(panel.email, SW_HIDE);
        if (panel.password) ShowWindow(panel.password, SW_HIDE);
        return;
    }
    if (!panel.font || panel.dpi != dpi) {
        auto font = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (font) {
            SendMessageW(panel.email, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            SendMessageW(panel.password, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            if (panel.font) DeleteObject(panel.font);
            panel.font = font;
            panel.dpi = dpi;
        }
    }
    RECT client{};
    GetClientRect(view.window, &client);
    const DeveloperLayout layout(UiLayout(client.right * 96.0f / dpi, client.bottom * 96.0f / dpi));
    auto position = [&](HWND edit, const UiRect& field) {
        SetWindowPos(edit, nullptr, MulDiv(int(field.x + 10), dpi, 96), MulDiv(int(field.y + 8), dpi, 96),
            MulDiv(int(field.w - 20), dpi, 96), MulDiv(22, dpi, 96), SWP_NOZORDER | SWP_NOACTIVATE);
        EnableWindow(edit, !view.developer.Busy() && !view.busy);
        ShowWindow(edit, SW_SHOW);
    };
    position(panel.email, layout.email);
    position(panel.password, layout.password);
    if (!panel.open && !view.developer.Busy() && !view.busy && GetActiveWindow() == view.window)
        SetFocus(panel.email);
    panel.open = true;
}

inline std::wstring ReadDeveloperEdit(HWND edit, size_t reserve = 0) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text;
    text.reserve(reserve);
    text.resize(size_t(length) + 1);
    text.resize(size_t(GetWindowTextW(edit, text.data(), length + 1)));
    return text;
}

template<class V> inline bool DispatchDeveloper(V& view, UiHit hit) {
    if (view.tab != 7 || view.notice.open) return false;
    if (hit.action == UiAction::DeveloperSignIn) {
        view.undercoop.section = kDeveloperSection;
        return true;
    }
    if (!DeveloperPageOpen(view)) return false;
    if (hit.action == UiAction::DeveloperBack) {
        view.undercoop.section = 0;
        return true;
    }
    if (hit.action == UiAction::DeveloperSubmit) {
        auto& panel = view.developerPanel;
        if (!view.developer.SignedIn() && !view.developer.Busy() && panel.open && !panel.fieldsUnavailable) {
            auto email = ReadDeveloperEdit(panel.email);
            // Keep the password in allocated storage so ownership moves to the session even for short input.
            auto password = ReadDeveloperEdit(panel.password, 32);
            ClearDeveloperPassword(panel.password);
            view.developer.SignIn(std::move(email), std::move(password));
        }
        return true;
    }
    if (hit.action == UiAction::DeveloperSignOut && view.developer.SignedIn()) {
        view.developer.SignOut();
        return true;
    }
    return false;
}

template<class V> inline UiHit DeveloperHitTest(const V& view, float x, float y, const UiLayout& base) {
    if (!DeveloperLayout(base).button.Contains(x, y)) return {};
    if (view.developer.SignedIn()) return {UiAction::DeveloperSignOut};
    if (!view.developer.Busy() && !view.developerPanel.fieldsUnavailable) return {UiAction::DeveloperSubmit};
    return {};
}

template<class V> inline void PaintDeveloper(V& view, const UiLayout& base) {
    const DeveloperLayout l(base);
    const auto card = l.card;
    auto& r = view.renderer;
    const bool signedIn = view.developer.SignedIn();
    const bool waiting = view.developer.Busy();
    const bool enabled = signedIn || (!waiting && !view.developerPanel.fieldsUnavailable);
    const bool hover = enabled && view.hover.action == (signedIn ? UiAction::DeveloperSignOut : UiAction::DeveloperSubmit);
    const float x = card.x + 22, w = card.w - 44;
    r.Box(card.x, card.y, card.w, card.h, 0x191C21, 9, 0x2C323B);
    r.Text(L"DEVELOPER ACCESS", x, card.y + 16, w, 25, 15, 0xE9EDF4, UiFace::Heading);
    r.Text(L"Sign in with your Undercoop developer account to unlock Play.",
        x, card.y + 44, w, 22, 13, 0xA0A8B5);

    if (signedIn) {
        r.Box(l.email.x, l.email.y, l.email.w, 74, 0x202B28, 7, 0x344C44);
        r.Circle(x + 17, l.email.y + 23, 4, 0x94CFB0);
        r.Text(L"Developer account verified", x + 30, l.email.y + 11, w - 46, 24, 14, 0xD8EDE2, UiFace::Bold);
        r.Text(L"Your developer session is active in this launcher.",
            x + 30, l.email.y + 39, w - 46, 22, 13, 0xAFC9BE);
    } else {
        r.Text(L"Email", x, l.email.y - 21, w, 20, 12, 0xC9D1DF, UiFace::Bold);
        r.Text(L"Password", x, l.password.y - 21, w, 20, 12, 0xC9D1DF, UiFace::Bold);
        for (const auto field : {l.email, l.password})
            r.Box(field.x, field.y, field.w, field.h, 0x1E2228, 6, 0x414A58);
    }

    const auto& message = view.developer.Message();
    const bool fieldsUnavailable = !signedIn && view.developerPanel.fieldsUnavailable;
    if (fieldsUnavailable || (!message.empty() && (!signedIn || waiting))) {
        r.Text(fieldsUnavailable ? L"Could not open the sign-in fields." : message.c_str(),
            x, l.button.y - 2, l.button.x - x - 16, 38, 12, waiting ? 0xA8BDDA : 0xE0B9AF,
            UiFace::Regular, true);
    }
    const auto button = l.button;
    r.Box(button.x, button.y, button.w, button.h,
        signedIn ? (hover ? 0x3B4C5E : 0x293441) : !enabled ? 0x303947 : hover ? 0xFFFFFF : 0xDCE5F0, 6);
    r.Text(signedIn ? L"Sign out" : waiting ? L"Signing in..." : L"Sign in", button.x, button.y, button.w, button.h,
        13, signedIn || !enabled ? 0xC4D3E5 : 0x1D252F, UiFace::Bold, false, true);
}
