// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Runs off the game and network threads. The only accepted configuration is
// a public Discord application ID, fetched from the existing developer host.
#pragma comment(lib,"winhttp.lib")
namespace coop_discord_remote {
struct HttpHandle {
    HINTERNET value;
    explicit HttpHandle(HINTERNET handle):value(handle){}
    ~HttpHandle(){if(value)WinHttpCloseHandle(value);}
    HttpHandle(const HttpHandle&)=delete;
    HttpHandle& operator=(const HttpHandle&)=delete;
};

static bool ApplyResponse(DWORD status,std::string_view body){
    if(status==204)return body.empty() && coop_discord_config::SetApplicationId({});
    constexpr std::string_view prefix="{\"applicationId\":\"";
    constexpr std::string_view suffix="\"}";
    if(status!=200 || body.size()<=prefix.size()+suffix.size() ||
       body.substr(0,prefix.size())!=prefix || body.substr(body.size()-suffix.size())!=suffix)return false;
    return coop_discord_config::SetApplicationId(body.substr(prefix.size(),body.size()-prefix.size()-suffix.size()));
}

static bool Download(){
    HttpHandle session(WinHttpOpen(L"Undercoop Discord Presence",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if(!session.value || !WinHttpSetTimeouts(session.value,2000,2000,2000,2000))return false;
    HttpHandle connection(WinHttpConnect(session.value,L"developer.undercoop.dev",INTERNET_DEFAULT_HTTPS_PORT,0));
    if(!connection.value)return false;
    HttpHandle request(WinHttpOpenRequest(connection.value,L"GET",L"/api/launcher/rpc",
        nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
    if(!request.value)return false;
    DWORD disable=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_COOKIES;
    const wchar_t headers[]=L"Accept: application/json\r\n";
    if(!WinHttpSetOption(request.value,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable)) ||
       !WinHttpSendRequest(request.value,headers,DWORD(-1),WINHTTP_NO_REQUEST_DATA,0,0,0) ||
       !WinHttpReceiveResponse(request.value,nullptr))return false;
    DWORD status=0,size=sizeof(status);
    if(!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
       WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX))return false;
    if(status==204)return ApplyResponse(status,{});
    if(status!=200)return false;
    std::array<char,64> body{};
    DWORD used=0;
    const ULONGLONG deadline=GetTickCount64()+4000;
    while(used<body.size() && GetTickCount64()<deadline){
        DWORD count=0;
        if(!WinHttpReadData(request.value,body.data()+used,DWORD(body.size())-used,&count))return false;
        if(!count)return ApplyResponse(status,{body.data(),used});
        used+=count;
    }
    return false;
}

static std::atomic<DWORD> nextFetch{0};
static std::atomic<bool> fetching{false};
static void CALLBACK Work(PTP_CALLBACK_INSTANCE instance,void* module){
    FreeLibraryWhenCallbackReturns(instance,static_cast<HMODULE>(module));
    const bool received=Download();
    nextFetch.store(GetTickCount()+(received?600000:30000),std::memory_order_relaxed);
    fetching.store(false,std::memory_order_release);
}

static void Poll(DWORD now){
    if(fetching.load(std::memory_order_acquire))return;
    const DWORD next=nextFetch.load(std::memory_order_relaxed);
    if(next && static_cast<int32_t>(now-next)<0)return;
    if(fetching.exchange(true,std::memory_order_acq_rel))return;
    HMODULE module=nullptr;
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&Work),&module)){
        if(TrySubmitThreadpoolCallback(&Work,module,nullptr))return;
        FreeLibrary(module);
    }
    nextFetch.store(now+30000,std::memory_order_relaxed);
    fetching.store(false,std::memory_order_release);
}
}
