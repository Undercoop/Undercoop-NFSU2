// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// The status endpoint exposes public data only. All launcher state is owned by
// the UI thread. A bounded HTTPS worker publishes a single immutable result.
namespace live_notice {
enum class Severity { None, Blue, Yellow, Red };
struct Status {
    std::wstring revision, title, message;
    Severity severity = Severity::None;
    bool playEnabled = true;

    bool operator==(const Status& other) const {
        return revision == other.revision && title == other.title &&
            message == other.message && severity == other.severity &&
            playEnabled == other.playEnabled;
    }
};

constexpr size_t kResponseLimit = 16 * 1024;
constexpr ULONGLONG kPollIntervalMs = 5000;
constexpr ULONGLONG kFirstCheckMs = 4000;
constexpr UINT_PTR kTimer = 7214;
#ifndef UNDERCOOP_NOTICE_HOST
#define UNDERCOOP_NOTICE_HOST L"developer.undercoop.dev"
#endif

// This small parser accepts the public status schema in any property order.
// Reject malformed or duplicate properties before replacing a known status.
class Parser {
public:
    explicit Parser(const std::wstring& input) : input_(input) {}

    bool Read(Status& result) {
        if (!Take(L'{')) return false;
        unsigned fields = 0;
        do {
            std::wstring key;
            if (!String(key, 32) || !Take(L':')) return false;
            if (key == L"revision" && !(fields & 1)) {
                if (!String(result.revision, 128) || result.revision.empty()) return false;
                for (auto ch : result.revision)
                    if (!((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z') ||
                        (ch >= L'a' && ch <= L'z') || ch == L'-' || ch == L'_')) return false;
                fields |= 1;
            } else if (key == L"playEnabled" && !(fields & 2)) {
                if (Literal(L"true")) result.playEnabled = true;
                else if (Literal(L"false")) result.playEnabled = false;
                else return false;
                fields |= 2;
            } else if (key == L"notice" && !(fields & 4)) {
                if (!Literal(L"null") && !ReadNotice(result)) return false;
                fields |= 4;
            } else return false;
        } while (Take(L','));
        if (fields != 7 || !Take(L'}')) return false;
        SkipSpace();
        return pos_ == input_.size();
    }

private:
    void SkipSpace() {
        while (pos_ < input_.size()) {
            const auto ch = input_[pos_];
            if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n') break;
            ++pos_;
        }
    }

    bool Take(wchar_t ch) {
        SkipSpace();
        if (pos_ == input_.size() || input_[pos_] != ch) return false;
        ++pos_;
        return true;
    }

    bool Literal(const wchar_t* text) {
        SkipSpace();
        const auto size = std::wcslen(text);
        if (input_.compare(pos_, size, text) != 0) return false;
        pos_ += size;
        return true;
    }

    bool Hex(wchar_t& ch) {
        if (input_.size() - pos_ < 4) return false;
        unsigned code = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const auto digit = input_[pos_++];
            code *= 16;
            if (digit >= L'0' && digit <= L'9') code += digit - L'0';
            else if (digit >= L'a' && digit <= L'f') code += digit - L'a' + 10;
            else if (digit >= L'A' && digit <= L'F') code += digit - L'A' + 10;
            else return false;
        }
        ch = static_cast<wchar_t>(code);
        return true;
    }

    bool Escape(wchar_t& ch) {
        if (pos_ == input_.size()) return false;
        ch = input_[pos_++];
        switch (ch) {
        case L'"': case L'\\': case L'/': return true;
        case L'n': ch = L'\n'; return true;
        case L'r': ch = L'\r'; return true;
        case L't': ch = L'\t'; return true;
        case L'u': return Hex(ch);
        default: return false;
        }
    }

    bool String(std::wstring& out, size_t limit) {
        if (!Take(L'"')) return false;
        while (pos_ < input_.size()) {
            auto ch = input_[pos_++];
            if (ch == L'"') {
                if (out.empty()) return true;
                return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                    out.data(), static_cast<int>(out.size()), nullptr, 0, nullptr, nullptr);
            }
            if (ch < 0x20 || (ch == L'\\' && !Escape(ch))) return false;
            if (ch < 0x20 && ch != L'\n' && ch != L'\r' && ch != L'\t') return false;
            if (out.size() >= limit) return false;
            out.push_back(ch);
        }
        return false;
    }

    bool ReadNotice(Status& result) {
        if (!Take(L'{')) return false;
        unsigned fields = 0;
        do {
            std::wstring key;
            if (!String(key, 32) || !Take(L':')) return false;
            if (key == L"severity" && !(fields & 1)) {
                std::wstring severity;
                if (!String(severity, 8)) return false;
                if (severity == L"blue") result.severity = Severity::Blue;
                else if (severity == L"yellow") result.severity = Severity::Yellow;
                else if (severity == L"red") result.severity = Severity::Red;
                else return false;
                fields |= 1;
            } else if (key == L"title" && !(fields & 2)) {
                if (!String(result.title, 100) || result.title.empty() ||
                    result.title.find_first_of(L"\r\n\t") != std::wstring::npos) return false;
                fields |= 2;
            } else if (key == L"message" && !(fields & 4)) {
                if (!String(result.message, 1600) || result.message.empty()) return false;
                fields |= 4;
            } else return false;
        } while (Take(L','));
        return fields == 7 && Take(L'}');
    }

    const std::wstring& input_;
    size_t pos_ = 0;
};

inline bool Decode(const std::string& body, Status& result) {
    if (body.empty() || body.size() > kResponseLimit) return false;
    const int size = static_cast<int>(body.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body.data(), size, nullptr, 0);
    if (!count) return false;
    std::wstring input(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body.data(), size, input.data(), count)) return false;
    Status next;
    if (!Parser(input).Read(next)) return false;
    result = std::move(next);
    return true;
}

inline bool Download(Status& status) {
    WebHandle session(WinHttpOpen(L"Undercoop Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value || !WinHttpSetTimeouts(session.value, 2000, 2000, 2000, 2000)) return false;
    WebHandle connection(WinHttpConnect(session.value, UNDERCOOP_NOTICE_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) return false;
    WebHandle request(WinHttpOpenRequest(connection.value, L"GET", L"/api/status",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.value) return false;
    DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    const wchar_t headers[] = L"Accept: application/json\r\nCache-Control: no-cache\r\n";
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)) ||
        !WinHttpSendRequest(request.value, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) return false;
    DWORD code = 0, size = sizeof(code);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX) || code != 200) return false;
    std::string body;
    const auto deadline = GetTickCount64() + 4000;
    char block[2048];
    while (GetTickCount64() < deadline) {
        DWORD count = 0;
        if (!WinHttpReadData(request.value, block, sizeof(block), &count)) return false;
        if (!count) return Decode(body, status);
        if (body.size() + count > kResponseLimit) return false;
        body.append(block, count);
    }
    return false;
}

struct Fetch {
    std::atomic<bool> complete{false};
    bool success = false;
    HMODULE module = nullptr;
    Status status;
};

inline void CALLBACK Work(PTP_CALLBACK_INSTANCE instance, void* context) {
    std::unique_ptr<std::shared_ptr<Fetch>> job(static_cast<std::shared_ptr<Fetch>*>(context));
    auto& fetch = **job;
    FreeLibraryWhenCallbackReturns(instance, fetch.module);
    try { fetch.success = Download(fetch.status); } catch (...) { fetch.success = false; }
    fetch.complete.store(true, std::memory_order_release);
}

inline std::shared_ptr<Fetch> StartFetch() {
    auto fetch = std::make_shared<Fetch>();
    auto job = std::make_unique<std::shared_ptr<Fetch>>(fetch);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&Work), &fetch->module)) return {};
    // Give ownership to the callback before submission. It can run immediately.
    auto* context = job.release();
    if (TrySubmitThreadpoolCallback(&Work, context, nullptr)) return fetch;
    delete context;
    FreeLibrary(fetch->module);
    return {};
}

struct Panel {
    Status status;
    std::shared_ptr<Fetch> pending;
    bool enabled = false, checked = false, online = false;
    bool open = false, acknowledged = false, seen = false;
    size_t generation = 1, displayed = 0;
    ULONGLONG started = 0, nextPoll = 0, pulseUntil = 0;
    float scroll = 0, maxScroll = 0;

    bool Offline() const { return enabled && checked && !online; }
    bool Visible() const {
        return enabled && (!checked || Offline() || !status.playEnabled || status.severity != Severity::None);
    }
    bool NeedsAcknowledgment() const {
        return status.playEnabled && !acknowledged &&
            (status.severity == Severity::Yellow || status.severity == Severity::Red);
    }
    bool CanPlay() const {
        return !enabled || (checked && status.playEnabled && !NeedsAcknowledgment());
    }
    bool Pulsing(ULONGLONG now) const { return Visible() && !seen && now < pulseUntil; }
    bool CanAcknowledge() const {
        return open && NeedsAcknowledgment() && displayed == generation && maxScroll - scroll < 1;
    }

    void Accept(const Status& latest, ULONGLONG now) {
        if (!(status == latest)) {
            status = latest;
            ++generation;
            seen = acknowledged = false;
            scroll = maxScroll = 0;
            pulseUntil = now + 8000;
        }
        online = checked = true;
        if (!Visible()) open = false;
    }
};

inline bool Tick(Panel& panel, ULONGLONG now) {
    if (!panel.enabled) return false;
    bool redraw = false;
    if (panel.pending && panel.pending->complete.load(std::memory_order_acquire)) {
        if (panel.pending->success) panel.Accept(panel.pending->status, now);
        else {
            panel.checked = true;
            panel.online = false;
        }
        panel.pending.reset();
        panel.nextPoll = now + kPollIntervalMs;
        redraw = true;
    }
    if (!panel.checked && now - panel.started >= kFirstCheckMs) {
        panel.checked = true;
        redraw = true;
    }
    if (!panel.pending && now >= panel.nextPoll) {
        try { panel.pending = StartFetch(); } catch (...) { panel.pending.reset(); }
        if (!panel.pending) {
            panel.checked = true;
            panel.online = false;
            panel.nextPoll = now + kPollIntervalMs;
            redraw = true;
        }
    }
    return redraw;
}
} // namespace live_notice
