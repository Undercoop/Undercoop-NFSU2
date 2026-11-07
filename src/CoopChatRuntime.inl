// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include "CoopChat.h"
// Only TickCoopChatUi calls presentation. The network thread owns no HWND,
// native-game pointer, save state, or console model.
void ShowCoopChatMessage(std::uint16_t player,const char* name,const char* text);
void ShowCoopChatStatus(const char* text);
namespace CoopChatRuntime {
static SRWLOCK lock=SRWLOCK_INIT;
static coop_chat::State state;
static coop::Endpoint peer{};
static std::uint32_t nonceCounter=0;
static std::uint32_t NewNonce() {
    // A local incarnation separates delayed datagrams from a prior binding.
    // Endpoint/session authentication remains mandatory at the receive gate.
    ++nonceCounter;const auto value=static_cast<std::uint32_t>(GetTickCount())^
        (static_cast<std::uint32_t>(GetCurrentProcessId())*0x9E3779B9u)^(nonceCounter*0x85EBCA6Bu);
    return value?value:nonceCounter;
}
static void Bind(const coop::SessionSnapshot& session) {
    const bool active=session.joined&&session.fresh&&session.peer.Valid()&&
        (session.role==coop::Role::Host||session.role==coop::Role::Client);
    if(!active){state.Unbind();peer={};return;}
    const std::uint16_t player=session.role==coop::Role::Host?1:2;
    if(state.Session()!=session.sessionId||state.Player()!=player||peer!=session.peer) {
        state.Bind(session.sessionId,player,NewNonce());peer=session.peer;
    }
}
}
bool SendCoopChatMessage(const char* text) {
    const auto session=CoopSnapshot();
    AcquireSRWLockExclusive(&CoopChatRuntime::lock);
    CoopChatRuntime::Bind(session);const bool accepted=CoopChatRuntime::state.Submit(text,GetTickCount());
    ReleaseSRWLockExclusive(&CoopChatRuntime::lock);return accepted;
}
bool ReceiveCoopChatPacket(const char* data,int bytes,const sockaddr_in& sender,DWORD now) {
    std::uint32_t magic=0;if(!data||bytes<4)return false;std::memcpy(&magic,data,4);
    if(magic!=coop_chat::kMagic)return false;
    if(bytes!=sizeof(coop_chat::Packet)||sender.sin_family!=AF_INET||!IsJoinedCoopSender(sender))return true;
    coop_chat::Packet packet{};std::memcpy(&packet,data,sizeof(packet));const auto session=CoopSnapshot();
    if(!coop_chat::Valid(packet)||packet.session!=session.sessionId||
        packet.player!=(session.role==coop::Role::Host?2:1))return true;
    AcquireSRWLockExclusive(&CoopChatRuntime::lock);
    CoopChatRuntime::Bind(session);CoopChatRuntime::state.Receive(packet,now);
    ReleaseSRWLockExclusive(&CoopChatRuntime::lock);return true;
}
void TickCoopChatNetwork(DWORD now) {
    const auto session=CoopSnapshot();coop_chat::Packet packet{};coop::Endpoint destination{};bool send=false;
    AcquireSRWLockExclusive(&CoopChatRuntime::lock);
    CoopChatRuntime::Bind(session);send=CoopChatRuntime::state.Poll(now,packet);destination=CoopChatRuntime::peer;
    ReleaseSRWLockExclusive(&CoopChatRuntime::lock);
    if(send)SendSessionBytes(&packet,sizeof(packet),destination);
}
void TickCoopChatUi() {
    coop_chat::Event events[coop_chat::kEventSize]{};std::size_t count=0;const auto session=CoopSnapshot();
    AcquireSRWLockExclusive(&CoopChatRuntime::lock);CoopChatRuntime::Bind(session);
    while(count<coop_chat::kEventSize&&CoopChatRuntime::state.Pop(events[count]))++count;
    ReleaseSRWLockExclusive(&CoopChatRuntime::lock);
    for(std::size_t i=0;i<count;++i) {
        const auto& event=events[i];
        if(event.kind==coop_chat::EventKind::Message)
            ShowCoopChatMessage(event.player,event.player==1?"Player 1":"Player 2",event.text);
        else ShowCoopChatStatus("Chat disconnected. Pending messages may not have been delivered.");
    }
}
