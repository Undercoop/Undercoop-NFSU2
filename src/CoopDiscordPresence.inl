// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// The game thread publishes only an enum snapshot. All Discord IPC runs on
// the observer thread and all reads/writes are overlapped.
#include "CoopDiscordRemoteConfig.inl"
namespace CoopDiscordPresence {
enum class Scene : uint8_t { Unknown, Startup, Menu, Lobby, CareerMenu, Loading,
    Cutscene, FreeRoam, SpecialEvent, Race, Results, Paused, Countdown };

static std::atomic<uint32_t> gPresence{0};

static const char* RaceName(uint8_t type) {
    switch(type) {
    case 2: return "Circuit";
    case 3: return "Sprint";
    case 4: return "Drag";
    case 5: return "Drift";
    case 8: return "Underground Racing League";
    case 9: return "Street X";
    default: return "Race";
    }
}

static uint32_t Encode(Scene scene,uint8_t type,unsigned players) {
    return static_cast<uint32_t>(scene)|(static_cast<uint32_t>(type)<<8)|(players<<16);
}

static std::string Activity(uint32_t state,uint32_t pid,uint32_t nonce,uint64_t start) {
    const auto scene=static_cast<Scene>(state&0xff);
    const auto type=static_cast<uint8_t>(state>>8);
    const auto players=state>>16;
    const char* details="Starting up";
    char raceDetails[72]{};
    switch(scene) {
    case Scene::Menu: details="Main menu"; break;
    case Scene::Lobby: details="Co-op lobby"; break;
    case Scene::CareerMenu: details="Choosing a career"; break;
    case Scene::Loading: details="Loading"; break;
    case Scene::Cutscene: details="Watching a cutscene"; break;
    case Scene::FreeRoam: details="Free roam"; break;
    case Scene::SpecialEvent: details="Career special event"; break;
    case Scene::Race: details=RaceName(type); break;
    case Scene::Results: details="Race results"; break;
    case Scene::Paused: details="Paused"; break;
    case Scene::Countdown: details="Starting a race"; break;
    default: break;
    }
    if(type && (scene==Scene::Results || scene==Scene::Paused || scene==Scene::Countdown)) {
        std::snprintf(raceDetails,sizeof(raceDetails),"%s | %s",details,RaceName(type));
        details=raceDetails;
    }
    const char* context=players==2?"Co-op career (2/2)":players==1?"Co-op career (1/2)":"Single player";
    if(scene==Scene::Lobby)context=players==2?"Party ready (2/2)":"Waiting for teammate (1/2)";
    else if(scene==Scene::Startup || scene==Scene::Menu)context="Need for Speed Underground 2";
    // Fields are from fixed English labels. No save names, network endpoints,
    // membership IDs or Discord credentials enter the presence payload.
    char json[512]{};
    std::snprintf(json,sizeof(json),
        "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%u\",\"args\":{\"pid\":%u,\"activity\":"
        "{\"details\":\"%s\",\"state\":\"%s\",\"timestamps\":{\"start\":%llu},\"instance\":false}}}",
        nonce,pid,details,context,static_cast<unsigned long long>(start));
    return json;
}

static void Observe(DWORD flow,uint32_t track) {
    static DWORD lastSample=0;
    const DWORD now=GetTickCount();
    if(lastSample && now-lastSample<250)return;
    lastSample=now;
    const auto session=CoopSnapshot();
    const bool coop=IsCoopGameplayActive() || gCoopUiActive;
    const unsigned players=coop?(session.joined?2u:1u):0;
    Scene scene=Scene::Startup;
    uint8_t type=0;
    uint32_t movieActive=0;
    uintptr_t movie=0;
    ReadGameMemory(CoopCareerNative::gMovieGlobals.active,&movieActive,4);
    ReadGameMemory(CoopCareerNative::gMovieGlobals.player,&movie,4);
    if(movieActive && IsValidUserPtr(movie))scene=Scene::Cutscene;
    else if(flow==4 || flow==5)scene=Scene::Loading;
    else if(flow==3) {
        if(gCoopUiActive && gCurrentNativeScreen=="PC_OL_Lobby.fng")scene=Scene::Lobby;
        else if(gCurrentNativeScreen=="UI_StartCareer.fng")scene=Scene::CareerMenu;
        else scene=Scene::Menu;
    } else if(flow==6) {
        uintptr_t native=0;
        uint32_t raceFlow=0;
        if(ReadGameMemory(gRaceContextAddresses.nativeFlow,&native,sizeof(native)) && IsValidUserPtr(native))
            ReadGameMemory(native,&raceFlow,4);
        if(track && track!=4000) {
            uint32_t mode=0;
            ReadGameMemory(CoopPeerRaceNative::addresses.params+0x10,&mode,4);
            type=static_cast<uint8_t>(CleanGameMode(mode));
            if(raceFlow==7)scene=Scene::Results;
            else if(raceFlow==6)scene=Scene::Paused;
            else if(coop && gCoopRaceState==COOP_RACE_STARTING)scene=Scene::Countdown;
            else scene=Scene::Race;
        } else if(CoopSpecialBusy())scene=Scene::SpecialEvent;
        else scene=raceFlow==6?Scene::Paused:Scene::FreeRoam;
    }
    gPresence.store(Encode(scene,type,players),std::memory_order_release);
}

enum Opcode : uint32_t { Handshake=0, Frame=1, Close=2, Ping=3, Pong=4 };
struct Header { uint32_t opcode,length; };
static_assert(sizeof(Header)==8,"Discord IPC header layout");

static bool ReadyEvent(const char* json) {
    const auto* event=std::strstr(json,"\"evt\"");
    if(!event)return false;
    event+=5;
    while(*event==' ' || *event=='\t' || *event=='\n' || *event=='\r')++event;
    if(*event++!=':')return false;
    while(*event==' ' || *event=='\t' || *event=='\n' || *event=='\r')++event;
    return std::strncmp(event,"\"READY\"",7)==0;
}

class Client {
    static constexpr DWORD kRetry=10000,kPoll=125,kTimeout=5000,kUpdate=5000;
    static constexpr DWORD kMaxFrame=64*1024;
    HANDLE pipe_=INVALID_HANDLE_VALUE;
    OVERLAPPED read_{},write_{};
    bool reading_=false,writing_=false,closing_=false,ready_=false;
    DWORD retryAt_=0,lastPoll_=0,openedAt_=0,writeAt_=0,sentAt_=0;
    DWORD writeBytes_=0;
    uint32_t sentState_=0,nonce_=0;
    uint64_t started_=0,applicationId_=0;
    DWORD received_=0;
    Header header_{};
    std::array<char,kMaxFrame+1> rx_{},tx_{};

    void Disconnect(DWORD now) {
        if(!closing_)CancelIoEx(pipe_,nullptr);
        closing_=true;
        retryAt_=now+kRetry;
    }

    bool IoReady(OVERLAPPED& io,bool& pending,DWORD& done,DWORD now) {
        if(!pending)return true;
        if(GetOverlappedResult(pipe_,&io,&done,FALSE)) {
            pending=false;
            return true;
        }
        if(GetLastError()==ERROR_IO_INCOMPLETE)return false;
        pending=false;
        Disconnect(now);
        return false;
    }

    void Drain(DWORD now) {
        DWORD bytes=0;
        IoReady(read_,reading_,bytes,now);
        IoReady(write_,writing_,bytes,now);
        if(reading_ || writing_)return;
        CloseHandle(pipe_);
        CloseHandle(read_.hEvent);
        CloseHandle(write_.hEvent);
        pipe_=INVALID_HANDLE_VALUE;
        closing_=ready_=false;
    }

    bool Send(uint32_t opcode,const char* body,DWORD length,DWORD now) {
        if(writing_ || length>kMaxFrame-sizeof(Header))return false;
        const Header header{opcode,length};
        std::memcpy(tx_.data(),&header,sizeof(header));
        std::memcpy(tx_.data()+sizeof(header),body,length);
        writeBytes_=static_cast<DWORD>(sizeof(header))+length;
        DWORD bytes=0;
        if(WriteFile(pipe_,tx_.data(),writeBytes_,&bytes,&write_))return bytes==writeBytes_;
        writing_=GetLastError()==ERROR_IO_PENDING;
        writeAt_=now;
        return writing_;
    }

    bool Open(DWORD now) {
        wchar_t name[]=L"\\\\.\\pipe\\discord-ipc-0";
        for(wchar_t index=L'0';index<=L'9';++index) {
            name[std::size(name)-2]=index;
            const auto pipe=CreateFileW(name,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
            if(pipe!=INVALID_HANDLE_VALUE) {
                Attach(pipe,now,applicationId_);
                return true;
            }
        }
        return false;
    }

    void ReadFrame(DWORD now) {
        DWORD bytes=0;
        if(reading_) {
            if(!IoReady(read_,reading_,bytes,now))return;
            if(!bytes) { Disconnect(now);return; }
            received_+=bytes;
        }
        const auto headerSize=static_cast<DWORD>(sizeof(header_));
        if(received_>=headerSize) {
            std::memcpy(&header_,rx_.data(),headerSize);
            if(header_.length>kMaxFrame-headerSize) { Disconnect(now);return; }
            if(received_==headerSize+header_.length) {
                char* body=rx_.data()+headerSize;
                body[header_.length]='\0';
                if(header_.opcode==Close)Disconnect(now);
                else if(header_.opcode==Ping && !Send(Pong,body,header_.length,now))Disconnect(now);
                else if(header_.opcode==Frame && !ready_ && ReadyEvent(body))ready_=true;
                received_=0;
                return;
            }
        }
        const DWORD target=received_<headerSize?headerSize:headerSize+header_.length;
        if(ReadFile(pipe_,rx_.data()+received_,target-received_,&bytes,&read_)) {
            if(bytes)received_+=bytes;
            else Disconnect(now);
        } else {
            reading_=GetLastError()==ERROR_IO_PENDING;
            if(!reading_)Disconnect(now);
        }
    }

public:
    // Adopt an already open, overlapped named pipe. Production discovers the
    // local Discord client via Open.
    void Attach(HANDLE pipe,DWORD now,uint64_t applicationId) {
        if(!applicationId) { CloseHandle(pipe);return; }
        applicationId_=applicationId;
        pipe_=pipe;
        read_={};
        write_={};
        read_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        write_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        openedAt_=now;
        sentState_=received_=0;
        char hello[48]{};
        const int length=std::snprintf(hello,sizeof(hello),"{\"v\":1,\"client_id\":\"%llu\"}",
            static_cast<unsigned long long>(applicationId_));
        if(!read_.hEvent || !write_.hEvent || length<=0 || size_t(length)>=sizeof(hello) ||
            !Send(Handshake,hello,static_cast<DWORD>(length),now))Disconnect(now);
    }

    void Poll(DWORD now,uint32_t state,uint64_t applicationId) {
        if(!state || (lastPoll_ && now-lastPoll_<kPoll))return;
        lastPoll_=now;
        if(applicationId!=applicationId_) {
            applicationId_=applicationId;
            if(pipe_!=INVALID_HANDLE_VALUE && !closing_)Disconnect(now);
            retryAt_=0;
        }
        if(closing_) { Drain(now);return; }
        if(!applicationId_)return;
        if(!started_) {
            FILETIME time{};
            GetSystemTimeAsFileTime(&time);
            ULARGE_INTEGER ticks{};
            ticks.LowPart=time.dwLowDateTime;
            ticks.HighPart=time.dwHighDateTime;
            started_=ticks.QuadPart/10000000-11644473600ULL;
        }
        if(pipe_==INVALID_HANDLE_VALUE) {
            if((!retryAt_ || static_cast<int32_t>(now-retryAt_)>=0) && !Open(now))retryAt_=now+kRetry;
            return;
        }
        DWORD bytes=0;
        if(writing_) {
            if(!IoReady(write_,writing_,bytes,now)) {
                if(!closing_ && now-writeAt_>=kTimeout)Disconnect(now);
                return;
            }
            if(bytes!=writeBytes_) { Disconnect(now);return; }
        }
        if(!ready_ && now-openedAt_>=kTimeout) { Disconnect(now);return; }
        ReadFrame(now);
        if(closing_ || writing_ || !ready_ || sentState_==state || (sentState_ && now-sentAt_<kUpdate))return;
        const auto payload=Activity(state,GetCurrentProcessId(),++nonce_,started_);
        if(!Send(Frame,payload.data(),static_cast<DWORD>(payload.size()),now)) { Disconnect(now);return; }
        sentState_=state;
        sentAt_=now;
    }
};

static void Poll(DWORD now) {
    static Client client;
    coop_discord_remote::Poll(now);
    client.Poll(now,gPresence.load(std::memory_order_acquire),coop_discord_config::GetApplicationId());
}
}
