// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Included inside CoopModManager, after the WinHTTP and live notice helpers.
namespace coop_developer {
enum class RequestKind { SignIn, VerifyPlay };
constexpr size_t kTokenLimit = 4096;
constexpr size_t kResponseLimit = 8192;

template<class String>
void ClearSecret(String& value) {
    if (!value.empty()) SecureZeroMemory(value.data(), value.size() * sizeof(typename String::value_type));
    value.clear();
}

inline bool AppendJsonString(std::string& body, const std::wstring& value) {
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return false;
    std::string utf8(static_cast<size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), utf8.data(), length, nullptr, nullptr)) {
        ClearSecret(utf8);
        return false;
    }
    static constexpr char hex[] = "0123456789abcdef";
    body += '"';
    for (unsigned char ch : utf8) {
        if (ch == '"' || ch == '\\') {
            body += '\\';
            body += static_cast<char>(ch);
        } else if (ch < 32) {
            body += "\\u00";
            body += hex[ch >> 4];
            body += hex[ch & 15];
        } else body += static_cast<char>(ch);
    }
    body += '"';
    ClearSecret(utf8);
    return true;
}

inline bool IsSessionToken(const std::string& token) {
    if (token.size() < 10 || token.size() > kTokenLimit) return false;
    unsigned separators = 0, partLength = 0;
    for (unsigned char ch : token) {
        if (ch == '.') {
            if (!partLength || ++separators > 2) return false;
            partLength = 0;
        } else {
            const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
            if (!valid) return false;
            ++partLength;
        }
    }
    return separators == 2 && partLength > 0;
}

inline bool DecodeSignIn(const std::string& response, std::string& token) {
    static constexpr char prefix[] = "{\"token\":\"";
    static constexpr char suffix[] = "\"}";
    const size_t prefixSize = sizeof(prefix) - 1, suffixSize = sizeof(suffix) - 1;
    if (response.size() <= prefixSize + suffixSize ||
        response.compare(0, prefixSize, prefix) != 0 ||
        response.compare(response.size() - suffixSize, suffixSize, suffix) != 0) return false;
    token.assign(response, prefixSize, response.size() - prefixSize - suffixSize);
    if (IsSessionToken(token)) return true;
    ClearSecret(token);
    return false;
}

struct Job {
    explicit Job(RequestKind kind) : kind(kind) {}
    ~Job() { ClearSecret(body); ClearSecret(token); }
    RequestKind kind;
    std::string body, token;
    DWORD status = 0;
    bool okay = false;
    std::atomic<bool> complete{false};
    HMODULE module = nullptr;
};

inline void Request(Job& job) {
    const bool signIn = job.kind == RequestKind::SignIn;
    WebHandle session(WinHttpOpen(L"Undercoop Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value || !WinHttpSetTimeouts(session.value, 2000, 3000, 4000, 12000)) return;
    WebHandle connection(WinHttpConnect(session.value, L"developer.undercoop.dev", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) return;
    WebHandle request(WinHttpOpenRequest(connection.value, signIn ? L"POST" : L"GET", L"/api/launcher/session",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.value) return;
    DWORD disabledFeatures = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabledFeatures, sizeof(disabledFeatures))) return;
    std::wstring headers = L"Accept: application/json\r\nCache-Control: no-store\r\n";
    if (signIn) headers += L"Content-Type: application/json\r\n";
    else {
        headers += L"Authorization: Bearer ";
        headers.append(job.token.begin(), job.token.end());
        headers += L"\r\n";
    }
    const auto bytes = static_cast<DWORD>(job.body.size());
    const BOOL sent = WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()),
        bytes ? job.body.data() : WINHTTP_NO_REQUEST_DATA, bytes, bytes, 0);
    ClearSecret(headers);
    ClearSecret(job.body);
    ClearSecret(job.token);
    if (!sent || !WinHttpReceiveResponse(request.value, nullptr)) return;
    DWORD size = sizeof(job.status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &job.status, &size, WINHTTP_NO_HEADER_INDEX) || job.status != 200) return;
    std::string response;
    const auto deadline = GetTickCount64() + 4000;
    char buffer[2048]{};
    while (GetTickCount64() < deadline) {
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer, sizeof(buffer), &read)) break;
        if (!read) {
            job.okay = signIn ? DecodeSignIn(response, job.token) : response == "{\"ok\":true}";
            break;
        }
        if (response.size() + read > kResponseLimit) break;
        response.append(buffer, read);
    }
    SecureZeroMemory(buffer, sizeof(buffer));
    ClearSecret(response);
}

inline void CALLBACK RunRequest(PTP_CALLBACK_INSTANCE instance, void* context) {
    std::unique_ptr<std::shared_ptr<Job>> owner(static_cast<std::shared_ptr<Job>*>(context));
    auto& job = **owner;
    FreeLibraryWhenCallbackReturns(instance, job.module);
    try { Request(job); } catch (...) { job.okay = false; }
    ClearSecret(job.body);
    job.complete.store(true, std::memory_order_release);
}

inline bool Submit(const std::shared_ptr<Job>& job) {
    auto owner = std::make_unique<std::shared_ptr<Job>>(job);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&RunRequest), &job->module)) return false;
    auto* context = owner.release();
    if (TrySubmitThreadpoolCallback(&RunRequest, context, nullptr)) return true;
    delete context;
    FreeLibrary(job->module);
    return false;
}

class Session {
public:
    ~Session() { ClearSecret(token_); }
    bool SignedIn() const { return !token_.empty(); }
    bool Busy() const { return !!pending_; }
    const std::wstring& Message() const { return message_; }
    bool HasPlayApproval() const { return playApproved_; }
    bool TakePlayApproval() {
        const bool approved = playApproved_;
        playApproved_ = false;
        return approved;
    }

    bool SignIn(std::wstring email, std::wstring password) {
        if (Busy() || SignedIn() || email.empty() || email.size() > 254 || password.empty() || password.size() > 1024) {
            ClearSecret(password);
            message_ = L"Enter your developer email and password.";
            return false;
        }
        auto job = std::make_shared<Job>(RequestKind::SignIn);
        job->body.reserve(32 + 6 * (email.size() + password.size()));
        job->body = "{\"email\":";
        const bool emailOkay = AppendJsonString(job->body, email);
        job->body += ",\"password\":";
        const bool passwordOkay = AppendJsonString(job->body, password);
        ClearSecret(password);
        job->body += '}';
        if (!emailOkay || !passwordOkay) {
            message_ = L"The email or password contains invalid text.";
            return false;
        }
        return Start(job, L"Signing in to Undercoop...");
    }

    bool VerifyPlay() {
        if (Busy() || !SignedIn()) return false;
        auto job = std::make_shared<Job>(RequestKind::VerifyPlay);
        job->token = token_;
        return Start(job, L"Verifying developer Play access...");
    }

    void SignOut() {
        pending_.reset();
        ClearSecret(token_);
        playApproved_ = false;
        message_.clear();
    }

    bool Tick() {
        if (!pending_ || !pending_->complete.load(std::memory_order_acquire)) return false;
        auto result = std::move(pending_);
        if (result->okay) {
            if (result->kind == RequestKind::SignIn) {
                token_.swap(result->token);
                message_ = L"Signed in. Developer Play access is available.";
            } else {
                playApproved_ = true;
                message_.clear();
            }
        } else if (result->status == 401 || result->status == 403) {
            const bool signIn = result->kind == RequestKind::SignIn;
            ClearSecret(token_);
            message_ = signIn ? L"Sign-in failed. Check your developer credentials." :
                L"Developer session expired. Sign in again.";
        } else if (result->status == 429) {
            message_ = L"Too many attempts. Try again later.";
        } else {
            message_ = L"Could not verify developer access. Check your connection and try again.";
        }
        return true;
    }

private:
    bool Start(const std::shared_ptr<Job>& job, const wchar_t* message) {
        if (!Submit(job)) {
            message_ = L"Could not start the developer request. Try again.";
            return false;
        }
        pending_ = job;
        message_ = message;
        return true;
    }
    std::shared_ptr<Job> pending_;
    std::string token_;
    std::wstring message_;
    bool playApproved_ = false;
};
} // namespace coop_developer
