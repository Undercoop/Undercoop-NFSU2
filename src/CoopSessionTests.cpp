// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include "CoopSession.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int checks = 0;
void Check(bool passed, const char* expression, int line) {
    ++checks;
    if (!passed) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

constexpr coop::Endpoint hostEndpoint{0x0100007f, 48930};
constexpr coop::Endpoint clientEndpoint{0x0100007f, 48931};
constexpr coop::Endpoint strangerEndpoint{0x0200007f, 50000};
constexpr std::uint64_t sessionId = 0x0123456789abcdefULL;

struct Pair {
    coop::SessionCore host, client;
    std::uint32_t now = 100;
    Pair() {
        CHECK(host.Host("Host", sessionId, now));
        CHECK(client.BeginBrowse("Client", now));
    }
    void Pump() {
        for (int round = 0; round < 64; ++round) {
            bool delivered = false;
            coop::Outgoing outgoing{};
            while (host.PopOutgoing(outgoing)) {
                CHECK(outgoing.endpoint == clientEndpoint);
                client.Receive(outgoing.message, sizeof(outgoing.message), hostEndpoint, now);
                delivered = true;
            }
            while (client.PopOutgoing(outgoing)) {
                CHECK(outgoing.endpoint == hostEndpoint);
                host.Receive(outgoing.message, sizeof(outgoing.message), clientEndpoint, now);
                delivered = true;
            }
            if (!delivered) return;
        }
        CHECK(false); // An ACK must never trigger another ACK.
    }
    void Join() {
        CHECK(client.Join(hostEndpoint, sessionId, "Client", now));
        Pump();
        CHECK(host.Snapshot(now).joined);
        CHECK(client.Snapshot(now).joined);
    }
    std::uint32_t Select() {
        CHECK(host.RequestCareer(now));
        Pump();
        const auto ticket = host.Snapshot(now).ticket;
        CHECK(ticket != 0 && client.Snapshot(now).ticket == ticket);
        return ticket;
    }
};

int CountActions(coop::SessionCore& core, coop::ActionType type) {
    int count = 0;
    coop::Action action{};
    while (core.PopAction(action)) if (action.type == type) ++count;
    return count;
}

void DiscoveryAndExplicitRoles() {
    Pair pair;
    coop::SessionMessage beacon{};
    CHECK(pair.host.BuildBeacon(beacon, pair.now));
    pair.client.Receive(beacon, sizeof(beacon), hostEndpoint, pair.now);
    auto snapshot = pair.client.Snapshot(pair.now);
    CHECK(snapshot.role == coop::Role::Browser);
    CHECK(snapshot.haveDiscoveredHost && snapshot.discoveredHost == hostEndpoint);
    CHECK(snapshot.discoveredSessionId == sessionId);
    CHECK(!snapshot.joined && !snapshot.startAllowed && !snapshot.peer.Valid());
    CHECK(!pair.host.Snapshot(pair.now).joined);
    CHECK(!pair.host.RequestCareer(pair.now));
    CHECK(!pair.client.RequestCareer(pair.now));
    CHECK(!pair.client.BuildBeacon(beacon, pair.now));
    pair.now += coop::kPeerTimeoutMs;
    pair.client.Tick(pair.now);
    CHECK(!pair.client.Snapshot(pair.now).haveDiscoveredHost);
    // Either process can explicitly host; there is no instance-number input.
    CHECK(pair.client.Host("Other Host", sessionId + 1, pair.now));
    CHECK(pair.client.Snapshot(pair.now).role == coop::Role::Host);
    CHECK(!pair.client.Join(hostEndpoint, sessionId, "Client", pair.now));
    CHECK(!pair.client.Host("Second", 99, pair.now));
}

void JoinAckAndRetries() {
    Pair pair;
    CHECK(pair.client.Join(hostEndpoint, sessionId, "Client", pair.now));
    coop::Outgoing join{};
    CHECK(pair.client.PopOutgoing(join));
    pair.host.Receive(join.message, sizeof(join.message), clientEndpoint, pair.now);
    CHECK(!pair.host.Snapshot(pair.now).joined);
    CHECK(!pair.client.Snapshot(pair.now).joined);
    coop::Outgoing reply{};
    CHECK(pair.host.PopOutgoing(reply));
    CHECK(reply.message.type == coop::MessageType::JoinAck);
    pair.client.Receive(reply.message, sizeof(reply.message), hostEndpoint, pair.now);
    CHECK(pair.client.Snapshot(pair.now).joined);
    CHECK(!pair.host.Snapshot(pair.now).joined);
    // Drop the ACK and heartbeat. Host resends the same JoinAck command.
    coop::Outgoing ignored{};
    while (pair.client.PopOutgoing(ignored)) {}
    pair.now += coop::kRetryMs - 1;
    pair.host.Tick(pair.now);
    CHECK(!pair.host.PopOutgoing(ignored));
    ++pair.now;
    pair.host.Tick(pair.now);
    CHECK(pair.host.PopOutgoing(ignored));
    CHECK(ignored.message.sequence == reply.message.sequence);
    pair.client.Receive(ignored.message, sizeof(ignored.message), hostEndpoint, pair.now);
    pair.Pump();
    CHECK(pair.host.Snapshot(pair.now).joined);
    CHECK(CountActions(pair.host, coop::ActionType::PeerJoined) == 1);
    CHECK(CountActions(pair.client, coop::ActionType::PeerJoined) == 1);
    pair.host.Receive(join.message, sizeof(join.message), clientEndpoint, pair.now);
    pair.Pump();
    CHECK(CountActions(pair.host, coop::ActionType::PeerJoined) == 0);
    CHECK(CountActions(pair.client, coop::ActionType::PeerJoined) == 0);
    CHECK(std::strcmp(pair.host.Snapshot(pair.now).clientName, "Client") == 0);
    CHECK(std::strcmp(pair.client.Snapshot(pair.now).hostName, "Host") == 0);
}

void RejectMalformedAndWrongPeer() {
    Pair pair;
    coop::SessionMessage beacon{};
    CHECK(pair.host.BuildBeacon(beacon, pair.now));
    auto invalid = beacon;
    invalid.version += 1;
    pair.client.Receive(invalid, sizeof(invalid), hostEndpoint, pair.now);
    CHECK(!pair.client.Snapshot(pair.now).haveDiscoveredHost);
    invalid = beacon;
    std::memset(invalid.name, 'x', sizeof(invalid.name));
    CHECK(!coop::SessionCore::ValidMessage(invalid));
    pair.client.Receive(invalid, sizeof(invalid), hostEndpoint, pair.now);
    pair.client.Receive(beacon, sizeof(beacon) - 1, hostEndpoint, pair.now);
    CHECK(!pair.client.Snapshot(pair.now).haveDiscoveredHost);
    invalid = beacon;
    invalid.size -= 1;
    pair.client.Receive(invalid, sizeof(invalid), hostEndpoint, pair.now);
    invalid = beacon;
    invalid.name[0] = '\n';
    pair.client.Receive(invalid, sizeof(invalid), hostEndpoint, pair.now);
    CHECK(!pair.client.Snapshot(pair.now).haveDiscoveredHost);
    pair.Join();
    coop::SessionMessage heartbeat{};
    heartbeat.type = coop::MessageType::Heartbeat;
    heartbeat.sessionId = sessionId;
    pair.now += 4000;
    pair.host.Receive(heartbeat, sizeof(heartbeat), strangerEndpoint, pair.now);
    heartbeat.sessionId += 1;
    pair.host.Receive(heartbeat, sizeof(heartbeat), clientEndpoint, pair.now);
    pair.now += 1000;
    CHECK(!pair.host.Snapshot(pair.now).fresh);
    pair.host.Tick(pair.now);
    CHECK(!pair.host.Snapshot(pair.now).joined && !pair.host.Snapshot(pair.now).peer.Valid());
    CHECK(CountActions(pair.host, coop::ActionType::PeerLeft) == 1);
}

void HeartbeatPersistenceAndReconnect() {
    Pair pair;
    pair.Join();
    pair.host.AcrossMainMenu();
    pair.client.AcrossMainMenu();
    CHECK(pair.host.Snapshot(pair.now).joined && pair.client.Snapshot(pair.now).joined);
    CHECK(!pair.host.BeginBrowse("Host", pair.now));
    for (int i = 0; i < 8; ++i) {
        pair.now += 1000;
        pair.host.Tick(pair.now);
        pair.client.Tick(pair.now);
        pair.Pump();
        CHECK(pair.host.Snapshot(pair.now).fresh && pair.client.Snapshot(pair.now).fresh);
    }
    pair.now += coop::kPeerTimeoutMs;
    pair.host.Tick(pair.now);
    pair.client.Tick(pair.now);
    CHECK(pair.host.Snapshot(pair.now).role == coop::Role::Host);
    CHECK(pair.client.Snapshot(pair.now).role == coop::Role::Browser);
    CHECK(!pair.host.Snapshot(pair.now).peer.Valid() && !pair.client.Snapshot(pair.now).peer.Valid());
    pair.Join();
    CHECK(pair.host.Snapshot(pair.now).startAllowed);
}

void CareerTransactionAndDuplicateSuppression() {
    Pair pair;
    pair.Join();
    CountActions(pair.host, coop::ActionType::PeerJoined);
    CountActions(pair.client, coop::ActionType::PeerJoined);
    CHECK(pair.host.RequestCareer(pair.now));
    coop::Outgoing selection{};
    CHECK(pair.host.PopOutgoing(selection));
    pair.client.Receive(selection.message, sizeof(selection.message), hostEndpoint, pair.now);
    pair.client.Receive(selection.message, sizeof(selection.message), hostEndpoint, pair.now);
    pair.Pump();
    const auto ticket = pair.host.Snapshot(pair.now).ticket;
    CHECK(CountActions(pair.host, coop::ActionType::CareerSelect) == 1);
    CHECK(CountActions(pair.client, coop::ActionType::CareerSelect) == 1);
    CHECK(!pair.host.RequestCareer(pair.now));
    CHECK(!pair.client.ReadyForCareer(ticket + 1, pair.now));
    CHECK(pair.host.ReadyForCareer(ticket, pair.now));
    pair.Pump();
    CHECK(CountActions(pair.host, coop::ActionType::StartCareer) == 0);
    CHECK(CountActions(pair.client, coop::ActionType::StartCareer) == 0);
    CHECK(pair.client.ReadyForCareer(ticket, pair.now));
    CHECK(!pair.client.ReadyForCareer(ticket, pair.now));
    pair.Pump();
    CHECK(pair.host.Snapshot(pair.now).phase == coop::Phase::InitialLoading);
    CHECK(pair.client.Snapshot(pair.now).phase == coop::Phase::InitialLoading);
    CHECK(CountActions(pair.host, coop::ActionType::StartCareer) == 1);
    CHECK(CountActions(pair.client, coop::ActionType::StartCareer) == 1);
    CHECK(!pair.client.RequestCareer(pair.now));
    pair.now += coop::kRetryMs;
    pair.host.Tick(pair.now);
    pair.client.Tick(pair.now);
    pair.Pump();
    CHECK(CountActions(pair.client, coop::ActionType::StartCareer) == 0);
}

void BothSkipVotesAndInitialBarrier() {
    Pair pair;
    pair.Join();
    const auto ticket = pair.Select();
    CountActions(pair.host, coop::ActionType::CareerSelect);
    CountActions(pair.client, coop::ActionType::CareerSelect);
    CHECK(pair.client.VoteSkip(ticket, pair.now));
    pair.Pump();
    CHECK(!pair.host.Snapshot(pair.now).skipDecided);
    CHECK(CountActions(pair.host, coop::ActionType::SkipIntro) == 0);
    CHECK(!pair.host.VoteSkip(ticket + 1, pair.now));
    CHECK(pair.host.VoteSkip(ticket, pair.now));
    pair.Pump();
    CHECK(pair.host.Snapshot(pair.now).skipDecided && pair.client.Snapshot(pair.now).skipDecided);
    CHECK(CountActions(pair.host, coop::ActionType::SkipIntro) == 1);
    CHECK(CountActions(pair.client, coop::ActionType::SkipIntro) == 1);
    CHECK(!pair.host.VoteSkip(ticket, pair.now));
    CHECK(pair.host.ReadyForCareer(ticket, pair.now));
    CHECK(pair.client.ReadyForCareer(ticket, pair.now));
    pair.Pump();
    CHECK(!pair.host.BeginInitialLoad(ticket + 1, pair.now));
    CHECK(pair.host.BeginInitialLoad(ticket, pair.now));
    CHECK(pair.client.BeginInitialLoad(ticket, pair.now));
    CHECK(pair.host.Snapshot(pair.now).initialBarrierStarted);
    CHECK(!pair.host.Snapshot(pair.now).localInitialReady && !pair.host.Snapshot(pair.now).peerInitialReady);
    CHECK(pair.host.MarkInitialLoaded(ticket, pair.now));
    pair.Pump();
    CHECK(pair.host.Snapshot(pair.now).initialBarrierActive);
    CHECK(pair.client.Snapshot(pair.now).initialBarrierActive);
    CHECK(pair.host.Snapshot(pair.now).localInitialReady && !pair.host.Snapshot(pair.now).peerInitialReady);
    CHECK(!pair.client.Snapshot(pair.now).localInitialReady && pair.client.Snapshot(pair.now).peerInitialReady);
    CHECK(pair.client.MarkInitialLoaded(ticket, pair.now));
    pair.Pump();
    CHECK(pair.host.Snapshot(pair.now).initialBarrierComplete);
    CHECK(pair.client.Snapshot(pair.now).initialBarrierComplete);
    CHECK(pair.host.Snapshot(pair.now).localInitialReady && pair.host.Snapshot(pair.now).peerInitialReady);
    CHECK(!pair.host.Snapshot(pair.now).initialBarrierActive);
    CHECK(pair.host.Snapshot(pair.now).phase == coop::Phase::Playing);
    CHECK(CountActions(pair.host, coop::ActionType::InitialLoadReleased) == 1);
    CHECK(CountActions(pair.client, coop::ActionType::InitialLoadReleased) == 1);
    pair.host.AcrossMainMenu();
    CHECK(!pair.host.BeginInitialLoad(ticket, pair.now));
    CHECK(!pair.client.BeginInitialLoad(ticket, pair.now));
    CHECK(pair.host.Snapshot(pair.now).joined);
}

void UdpReorderingAndSequenceWrap() {
    Pair pair;
    pair.Join();
    const auto ticket = pair.Select();
    CHECK(pair.host.ReadyForCareer(ticket, pair.now));
    CHECK(pair.client.ReadyForCareer(ticket, pair.now));
    // Deliver client Ready; then deliver host's InitialReady before CareerStart.
    coop::Outgoing outgoing{};
    while (pair.client.PopOutgoing(outgoing))
        pair.host.Receive(outgoing.message, sizeof(outgoing.message), clientEndpoint, pair.now);
    CHECK(pair.host.BeginInitialLoad(ticket, pair.now));
    CHECK(pair.host.MarkInitialLoaded(ticket, pair.now));
    coop::Outgoing start{}, initial{};
    while (pair.host.PopOutgoing(outgoing)) {
        if (outgoing.message.type == coop::MessageType::CareerStart) start = outgoing;
        else if (outgoing.message.type == coop::MessageType::InitialReady) initial = outgoing;
        else pair.client.Receive(outgoing.message, sizeof(outgoing.message), hostEndpoint, pair.now);
    }
    CHECK(start.message.sequence != 0 && initial.message.sequence != 0);
    pair.client.Receive(initial.message, sizeof(initial.message), hostEndpoint, pair.now);
    pair.client.Receive(start.message, sizeof(start.message), hostEndpoint, pair.now);
    CHECK(pair.client.BeginInitialLoad(ticket, pair.now));
    CHECK(pair.client.MarkInitialLoaded(ticket, pair.now));
    pair.Pump();
    CHECK(pair.client.Snapshot(pair.now).initialBarrierComplete);
    CHECK(pair.host.Snapshot(pair.now).initialBarrierComplete);

    Pair wrap;
    wrap.Join();
    const auto wrapTicket = wrap.Select();
    CHECK(wrap.host.VoteSkip(wrapTicket, wrap.now));
    coop::SessionMessage vote{};
    vote.sessionId = sessionId;
    vote.type = coop::MessageType::SkipVote;
    vote.nonce = wrapTicket;
    vote.data[7] = wrap.host.Snapshot(wrap.now).membershipNonce;
    vote.sequence = 0xfffffffeu;
    wrap.host.Receive(vote, sizeof(vote), clientEndpoint, wrap.now);
    CHECK(wrap.host.Snapshot(wrap.now).skipDecided);
    vote.type = coop::MessageType::CareerReady;
    vote.sequence = 1;
    wrap.host.Receive(vote, sizeof(vote), clientEndpoint, wrap.now);
    CHECK(wrap.host.Snapshot(wrap.now).peerCareerReady);
    CHECK(CountActions(wrap.host, coop::ActionType::SkipIntro) == 1);
}

void LeaveCancelsActionsAndRetriesAck() {
    Pair pair;
    pair.Join();
    pair.Select();
    pair.host.Leave(pair.now);
    CHECK(pair.host.Snapshot(pair.now).role == coop::Role::Idle);
    CHECK(!pair.host.Snapshot(pair.now).peer.Valid());
    CHECK(CountActions(pair.host, coop::ActionType::Closed) == 1);
    coop::Outgoing leave{};
    CHECK(pair.host.PopOutgoing(leave));
    CHECK(leave.message.type == coop::MessageType::Leave);
    pair.client.Receive(leave.message, sizeof(leave.message), hostEndpoint, pair.now);
    CHECK(pair.client.Snapshot(pair.now).role == coop::Role::Browser);
    CHECK(!pair.client.Snapshot(pair.now).joined && !pair.client.Snapshot(pair.now).ticket);
    CHECK(CountActions(pair.client, coop::ActionType::PeerLeft) == 1);
    coop::Outgoing dropped{};
    while (pair.client.PopOutgoing(dropped)) {}
    pair.now += coop::kRetryMs;
    pair.host.Tick(pair.now);
    pair.Pump(); // Departed peer re-acknowledges the repeated Leave.
    pair.now += coop::kRetryMs;
    pair.host.Tick(pair.now);
    CHECK(!pair.host.PopOutgoing(dropped));
    CHECK(!pair.host.ReadyForCareer(1, pair.now));
}

void BoundedQueuesAndClockWrap() {
    Pair pair;
    pair.Join();
    const auto ticket = pair.Select();
    coop::SessionMessage vote{};
    vote.sessionId = sessionId;
    vote.type = coop::MessageType::SkipVote;
    vote.nonce = ticket;
    vote.data[7] = pair.host.Snapshot(pair.now).membershipNonce;
    vote.sequence = 40;
    for (int i = 0; i < 100; ++i)
        pair.host.Receive(vote, sizeof(vote), clientEndpoint, pair.now);
    int count = 0;
    coop::Outgoing outgoing{};
    while (pair.host.PopOutgoing(outgoing)) ++count;
    CHECK(count <= 16);

    Pair clockWrap;
    clockWrap.now = 0xffffff00u;
    clockWrap.Join();
    clockWrap.now += 1000;
    clockWrap.host.Tick(clockWrap.now);
    clockWrap.client.Tick(clockWrap.now);
    clockWrap.Pump();
    CHECK(clockWrap.host.Snapshot(clockWrap.now).fresh);
    clockWrap.now += coop::kPeerTimeoutMs;
    clockWrap.host.Tick(clockWrap.now);
    CHECK(!clockWrap.host.Snapshot(clockWrap.now).joined);
}
} // namespace

void GameplayLiveness() {
    Pair pair;pair.Join();
    for(unsigned i=0;i<12;++i) {
        pair.now+=1000;
        CHECK(pair.host.ObserveGameplay(sessionId,clientEndpoint,pair.now));
        CHECK(pair.client.ObserveGameplay(sessionId,hostEndpoint,pair.now));
        pair.host.Tick(pair.now);pair.client.Tick(pair.now);
        CHECK(pair.host.Snapshot(pair.now).joined && pair.client.Snapshot(pair.now).fresh);
    }
    CHECK(!pair.host.ObserveGameplay(sessionId,strangerEndpoint,pair.now));
    CHECK(!pair.host.ObserveGameplay(sessionId+1,clientEndpoint,pair.now));
    pair.now+=coop::kPeerTimeoutMs;pair.host.Tick(pair.now);pair.client.Tick(pair.now);
    CHECK(!pair.host.Snapshot(pair.now).joined && !pair.client.Snapshot(pair.now).joined);
    CHECK(!pair.host.ObserveGameplay(sessionId,clientEndpoint,pair.now));
}
void ConfirmedNewCareerReplacesOldTicket() {
    for (unsigned phase = 0; phase < 4; ++phase) {
        Pair pair; pair.Join(); const auto old = pair.Select();
        if (phase >= 1) { CHECK(pair.host.ReadyForCareer(old,pair.now)); pair.Pump(); }
        if (phase >= 2) { CHECK(pair.client.ReadyForCareer(old,pair.now)); pair.Pump(); }
        if (phase >= 3) {
            CHECK(pair.host.BeginInitialLoad(old,pair.now)); CHECK(pair.client.BeginInitialLoad(old,pair.now));
            CHECK(pair.host.MarkInitialLoaded(old,pair.now)); CHECK(pair.client.MarkInitialLoaded(old,pair.now)); pair.Pump();
            CHECK(pair.host.Snapshot(pair.now).phase==coop::Phase::Playing);
        }
        CHECK(!pair.host.RequestCareer(pair.now)); // Resume is not an implicit replacement.
        CHECK(!pair.client.RequestCareer(pair.now,true));
        CHECK(pair.host.RequestCareer(pair.now,true)); pair.Pump();
        const auto fresh=pair.host.Snapshot(pair.now).ticket;
        CHECK(fresh && fresh!=old && pair.client.Snapshot(pair.now).ticket==fresh);
        CHECK(pair.host.Snapshot(pair.now).phase==coop::Phase::CareerSelection);
        CHECK(pair.client.Snapshot(pair.now).phase==coop::Phase::CareerSelection);
        CHECK(!pair.host.ReadyForCareer(old,pair.now));
        CHECK(!pair.client.MarkInitialLoaded(old,pair.now));
        CHECK(CountActions(pair.host,coop::ActionType::StartCareer)==0);
        CHECK(CountActions(pair.client,coop::ActionType::StartCareer)==0);
        CHECK(pair.host.ReadyForCareer(fresh,pair.now)); CHECK(pair.client.ReadyForCareer(fresh,pair.now)); pair.Pump();
        CHECK(pair.host.BeginInitialLoad(fresh,pair.now)); CHECK(pair.client.BeginInitialLoad(fresh,pair.now));
        CHECK(pair.host.MarkInitialLoaded(fresh,pair.now)); CHECK(pair.client.MarkInitialLoaded(fresh,pair.now)); pair.Pump();
        CHECK(pair.host.Snapshot(pair.now).phase==coop::Phase::Playing && pair.client.Snapshot(pair.now).phase==coop::Phase::Playing);
    }
    Pair reordered; reordered.Join(); CHECK(reordered.host.RequestCareer(reordered.now));
    coop::Outgoing late{}; CHECK(reordered.host.PopOutgoing(late));
    CHECK(late.message.type==coop::MessageType::CareerSelect);
    CHECK(reordered.host.RequestCareer(reordered.now,true)); reordered.Pump();
    const auto fresh=reordered.client.Snapshot(reordered.now).ticket;
    reordered.client.Receive(late.message,sizeof(late.message),hostEndpoint,reordered.now);
    reordered.Pump();
    CHECK(reordered.client.Snapshot(reordered.now).ticket==fresh);
    CHECK(CountActions(reordered.client,coop::ActionType::CareerSelect)==1);
}

void DrainActions(Pair& pair) {
    coop::Action action{};
    while(pair.host.PopAction(action)) {}
    while(pair.client.PopAction(action)) {}
}
void FinishGeneration(Pair& pair,std::uint32_t ticket) {
    CHECK(pair.host.ReadyForCareer(ticket,pair.now));
    CHECK(pair.client.ReadyForCareer(ticket,pair.now)); pair.Pump();
    CHECK(pair.host.BeginInitialLoad(ticket,pair.now));
    CHECK(pair.client.BeginInitialLoad(ticket,pair.now));
    CHECK(pair.host.MarkInitialLoaded(ticket,pair.now));
    CHECK(pair.client.MarkInitialLoaded(ticket,pair.now)); pair.Pump();
    CHECK(pair.host.Snapshot(pair.now).phase==coop::Phase::Playing);
    CHECK(pair.client.Snapshot(pair.now).phase==coop::Phase::Playing);
}
std::uint32_t PlayNamed(Pair& pair,const char* name) {
    CHECK(pair.host.RequestCareer(pair.now,true,name)); pair.Pump();
    const auto ticket=pair.host.Snapshot(pair.now).ticket;
    FinishGeneration(pair,ticket); DrainActions(pair);return ticket;
}

void PlayingCareerSurvivesReconnectWindow() {
    Pair pair;
    pair.now = 0xfffff000u;
    pair.Join();
    const auto ticket = PlayNamed(pair, "NAME");
    const auto membership = pair.host.Snapshot(pair.now).membershipNonce;

    // Lose both heartbeat and gameplay traffic across the old five-second
    // deadline, then receive fresh gameplay on the existing membership.
    pair.now += coop::kPeerTimeoutMs + 1000;
    pair.host.Tick(pair.now);
    pair.client.Tick(pair.now);
    for (const auto& snapshot : {pair.host.Snapshot(pair.now), pair.client.Snapshot(pair.now)}) {
        CHECK(snapshot.joined && snapshot.fresh && snapshot.phase == coop::Phase::Playing);
        CHECK(snapshot.ticket == ticket && snapshot.membershipNonce == membership);
        CHECK(snapshot.peerAgeMs == coop::kPeerTimeoutMs + 1000);
    }
    CHECK(pair.host.ObserveGameplay(sessionId, clientEndpoint, pair.now));
    CHECK(pair.client.ObserveGameplay(sessionId, hostEndpoint, pair.now));
    CHECK(CountActions(pair.host, coop::ActionType::PeerLeft) == 0);
    CHECK(CountActions(pair.client, coop::ActionType::PeerLeft) == 0);

    pair.now += coop::kPlayingPeerTimeoutMs - 1;
    pair.host.Tick(pair.now);
    pair.client.Tick(pair.now);
    CHECK(pair.host.Snapshot(pair.now).fresh && pair.client.Snapshot(pair.now).fresh);
    ++pair.now;
    pair.host.Tick(pair.now);
    pair.client.Tick(pair.now);
    CHECK(!pair.host.Snapshot(pair.now).joined && !pair.client.Snapshot(pair.now).joined);
    CHECK(CountActions(pair.host, coop::ActionType::PeerLeft) == 1);
    CHECK(CountActions(pair.client, coop::ActionType::PeerLeft) == 1);
    CHECK(!pair.host.ObserveGameplay(sessionId, clientEndpoint, pair.now));
}
void MainMenuPresenceAndHotRejoin() {
    for(unsigned retained=1;retained<=2;++retained) {
        Pair pair;pair.Join();
        CHECK(!pair.client.RequestResume("NAME",pair.now)); // Initial client has no career authority.
        CHECK(!pair.client.RequestCareer(pair.now,true,"NAME"));
        const auto old=PlayNamed(pair,"NAME");
        auto& returning=retained==1?pair.client:pair.host;
        auto& active=retained==1?pair.host:pair.client;
        CHECK(returning.AcrossMainMenu(pair.now)); pair.Pump();
        CHECK(returning.Snapshot(pair.now).joined && returning.Snapshot(pair.now).localInMainMenu);
        CHECK(!active.Snapshot(pair.now).localInMainMenu);
        CHECK(active.Snapshot(pair.now).peerInMainMenu);
        CHECK(active.Snapshot(pair.now).phase==coop::Phase::Playing); // No forced native reload.
        CHECK(returning.Snapshot(pair.now).careerLaunched);
        CHECK(!returning.BeginInitialLoad(old,pair.now));
        CHECK(!returning.MarkInitialLoaded(old,pair.now));
        if(retained==1) {
            CHECK(pair.client.RequestResume("NAME",pair.now)); pair.Pump();
            CHECK(CountActions(pair.host,coop::ActionType::ResumeRequested)==1);
            CHECK(std::strcmp(pair.host.Snapshot(pair.now).requestedCampaignName,"NAME")==0);
            CHECK(!pair.client.RequestResume("Player",pair.now));
        }
        CHECK(pair.host.RequestCareer(pair.now,true,"NAME",static_cast<std::uint8_t>(retained)));
        pair.Pump();const auto fresh=pair.host.Snapshot(pair.now).ticket;
        CHECK(fresh!=old && pair.client.Snapshot(pair.now).ticket==fresh);
        CHECK(active.Snapshot(pair.now).localRetainsNative);
        CHECK(!returning.Snapshot(pair.now).localRetainsNative);
        CHECK(!pair.host.Snapshot(pair.now).initialBarrierComplete);
        CHECK(!pair.client.Snapshot(pair.now).resumeRequestPending);
        // Native contract: retained side acknowledges its existing world; it is
        // not required to return to menu or deserialize its profile again.
        FinishGeneration(pair,fresh);
        CHECK(CountActions(pair.host,coop::ActionType::InitialLoadReleased)==1);
        CHECK(CountActions(pair.client,coop::ActionType::InitialLoadReleased)==1);
        CHECK(std::strcmp(pair.host.Snapshot(pair.now).campaignName,"NAME")==0);
    }
}
void SimultaneousMenuAndDifferentCampaignSwitch() {
    Pair pair;pair.Join();const auto old=PlayNamed(pair,"NAME");
    CHECK(pair.host.AcrossMainMenu(pair.now)); CHECK(pair.client.AcrossMainMenu(pair.now)); pair.Pump();
    CHECK(pair.host.Snapshot(pair.now).localInMainMenu && pair.host.Snapshot(pair.now).peerInMainMenu);
    CHECK(pair.client.Snapshot(pair.now).localInMainMenu && pair.client.Snapshot(pair.now).peerInMainMenu);
    CHECK(pair.client.RequestResume("Player",pair.now));pair.Pump();
    CHECK(std::strcmp(pair.host.Snapshot(pair.now).requestedCampaignName,"Player")==0);
    CHECK(!pair.host.RequestCareer(pair.now,true,"Player",1));
    CHECK(!pair.host.RequestCareer(pair.now,true,"Player",2));
    CHECK(pair.host.RequestCareer(pair.now,true,"Player"));pair.Pump();
    const auto fresh=pair.host.Snapshot(pair.now).ticket;
    CHECK(fresh!=old && !pair.host.Snapshot(pair.now).localInitialReady &&
          !pair.host.Snapshot(pair.now).peerInitialReady);
    CHECK(!pair.host.ReadyForCareer(old,pair.now));
    FinishGeneration(pair,fresh);
    CHECK(std::strcmp(pair.client.Snapshot(pair.now).campaignName,"Player")==0);
    CHECK(pair.client.AcrossMainMenu(pair.now));pair.Pump();
    CHECK(pair.client.RequestResume("NAME",pair.now));pair.Pump();
    CHECK(!pair.host.RequestCareer(pair.now,true,"NAME",1)); // Live Player cannot be retained as NAME.
    CHECK(pair.host.Snapshot(pair.now).phase==coop::Phase::Playing);
    CHECK(std::strcmp(pair.host.Snapshot(pair.now).requestedCampaignName,"NAME")==0);
}
void AcceptedControlsOnlyAreAcknowledged() {
    Pair pair;pair.Join();const auto ticket=pair.Select();DrainActions(pair);
    coop::SessionMessage start{};
    start.sessionId=sessionId;start.type=coop::MessageType::CareerStart;
    start.nonce=ticket;start.sequence=100;
    start.data[7]=pair.client.Snapshot(pair.now).membershipNonce;
    pair.client.Receive(start,sizeof(start),hostEndpoint,pair.now);
    coop::Outgoing outgoing{};
    CHECK(!pair.client.PopOutgoing(outgoing)); // Not ready: sender must retry.
    CHECK(pair.client.ReadyForCareer(ticket,pair.now));
    while(pair.client.PopOutgoing(outgoing)) {} // Hold its profile-ready packet.
    pair.client.Receive(start,sizeof(start),hostEndpoint,pair.now);
    CHECK(pair.client.Snapshot(pair.now).phase==coop::Phase::InitialLoading);
    CHECK(pair.client.PopOutgoing(outgoing) && outgoing.message.type==coop::MessageType::Ack);
    CHECK(outgoing.message.ack==100);
    pair.client.Receive(start,sizeof(start),hostEndpoint,pair.now);
    CHECK(pair.client.PopOutgoing(outgoing) && outgoing.message.type==coop::MessageType::Ack);
    CHECK(CountActions(pair.client,coop::ActionType::StartCareer)==1);
}
void ReorderedResumeAndReliableRetry() {
    Pair pair;pair.Join();PlayNamed(pair,"NAME");
    CHECK(pair.client.AcrossMainMenu(pair.now));
    coop::Outgoing menu{},request{},outgoing{};
    CHECK(pair.client.PopOutgoing(menu) && menu.message.type==coop::MessageType::MainMenu);
    CHECK(pair.client.RequestResume("NAME",pair.now));
    CHECK(pair.client.PopOutgoing(request) && request.message.type==coop::MessageType::ResumeRequest);
    pair.host.Receive(request.message,sizeof(request.message),clientEndpoint,pair.now);
    CHECK(!pair.host.PopOutgoing(outgoing)); // Cannot authorize before menu presence.
    pair.host.Receive(menu.message,sizeof(menu.message),clientEndpoint,pair.now);
    CHECK(pair.host.PopOutgoing(outgoing) && outgoing.message.type==coop::MessageType::Ack);
    pair.client.Receive(outgoing.message,sizeof(outgoing.message),hostEndpoint,pair.now);
    pair.now+=coop::kRetryMs;pair.client.Tick(pair.now);
    pair.Pump();
    CHECK(CountActions(pair.host,coop::ActionType::ResumeRequested)==1);
    CHECK(pair.host.RequestCareer(pair.now,true,"NAME",1));pair.Pump();
    const auto ticket=pair.host.Snapshot(pair.now).ticket;
    // A reordered old menu or request cannot invalidate the new generation.
    pair.host.Receive(menu.message,sizeof(menu.message),clientEndpoint,pair.now);
    pair.host.Receive(request.message,sizeof(request.message),clientEndpoint,pair.now);
    pair.Pump();
    CHECK(pair.host.Snapshot(pair.now).ticket==ticket);
    CHECK(!pair.host.Snapshot(pair.now).resumeRequestPending);
    CHECK(CountActions(pair.host,coop::ActionType::ResumeRequested)==0);
    pair.now+=coop::kRetryMs;pair.client.Tick(pair.now);
    while(pair.client.PopOutgoing(outgoing))
        CHECK(outgoing.message.type!=coop::MessageType::ResumeRequest && outgoing.message.type!=coop::MessageType::MainMenu);
}
void OldReadyAndMembershipCannotReleaseFreshLoad() {
    Pair pair;pair.Join();const auto old=PlayNamed(pair,"NAME");
    const auto oldMembership=pair.host.Snapshot(pair.now).membershipNonce;
    CHECK(pair.host.RequestCareer(pair.now,true,"Player"));pair.Pump();
    const auto fresh=pair.host.Snapshot(pair.now).ticket;
    coop::SessionMessage ready{};
    ready.sessionId=sessionId;ready.type=coop::MessageType::InitialReady;
    ready.sequence=100;ready.nonce=old;ready.data[7]=oldMembership;
    pair.host.Receive(ready,sizeof(ready),clientEndpoint,pair.now);
    CHECK(!pair.host.Snapshot(pair.now).peerInitialReady);
    coop::Outgoing outgoing{};CHECK(!pair.host.PopOutgoing(outgoing));
    CHECK(pair.host.ReadyForCareer(fresh,pair.now));CHECK(pair.client.ReadyForCareer(fresh,pair.now));pair.Pump();
    CHECK(pair.host.BeginInitialLoad(fresh,pair.now));CHECK(pair.host.MarkInitialLoaded(fresh,pair.now));pair.Pump();
    CHECK(!pair.host.Snapshot(pair.now).initialBarrierComplete);
    ready.nonce=fresh;ready.data[7]=oldMembership+1;
    pair.host.Receive(ready,sizeof(ready),clientEndpoint,pair.now);
    CHECK(!pair.host.Snapshot(pair.now).peerInitialReady);
    CHECK(!pair.host.PopOutgoing(outgoing));
    CHECK(pair.client.BeginInitialLoad(fresh,pair.now));CHECK(pair.client.MarkInitialLoaded(fresh,pair.now));pair.Pump();
    CHECK(pair.host.Snapshot(pair.now).initialBarrierComplete);
    pair.client.Leave(pair.now);pair.Pump();
    CHECK(!pair.host.Snapshot(pair.now).careerLaunched);
    CHECK(!pair.host.Snapshot(pair.now).ticket && pair.host.Snapshot(pair.now).campaignName[0]==0);
    CHECK(pair.client.BeginBrowse("Client",pair.now));pair.Join();
    CHECK(pair.host.Snapshot(pair.now).membershipNonce!=oldMembership);
    CHECK(!pair.client.RequestResume("NAME",pair.now));
    CHECK(pair.host.RequestCareer(pair.now,true,"NAME"));pair.Pump();
    ready.nonce=pair.host.Snapshot(pair.now).ticket;ready.data[7]=oldMembership;
    pair.host.Receive(ready,sizeof(ready),clientEndpoint,pair.now);
    CHECK(!pair.host.Snapshot(pair.now).peerInitialReady);
}
void ReturnedPeerCannotFinishAbandonedBarrier() {
    Pair pair;pair.Join();const auto ticket=pair.Select();
    CHECK(pair.host.ReadyForCareer(ticket,pair.now));CHECK(pair.client.ReadyForCareer(ticket,pair.now));pair.Pump();
    CHECK(pair.host.BeginInitialLoad(ticket,pair.now));CHECK(pair.client.BeginInitialLoad(ticket,pair.now));
    CHECK(pair.host.MarkInitialLoaded(ticket,pair.now));pair.Pump();DrainActions(pair);
    CHECK(pair.client.AcrossMainMenu(pair.now));pair.Pump();
    CHECK(pair.host.Snapshot(pair.now).peerInMainMenu);
    coop::SessionMessage stale{};
    stale.sessionId=sessionId;stale.sequence=100;stale.type=coop::MessageType::InitialReady;
    stale.nonce=ticket;stale.data[7]=pair.host.Snapshot(pair.now).membershipNonce;
    pair.host.Receive(stale,sizeof(stale),clientEndpoint,pair.now);
    CHECK(!pair.host.Snapshot(pair.now).peerInitialReady);
    CHECK(!pair.host.Snapshot(pair.now).initialBarrierComplete);
    CHECK(CountActions(pair.host,coop::ActionType::InitialLoadReleased)==0);
    coop::Outgoing outgoing{};CHECK(!pair.host.PopOutgoing(outgoing));
    CHECK(!pair.client.VoteSkip(ticket,pair.now));
    CHECK(pair.host.RequestCareer(pair.now,true,"NAME"));pair.Pump();
    FinishGeneration(pair,pair.host.Snapshot(pair.now).ticket);
}
void ResumeRejectionIsReliableAndTicketBound() {
    Pair pair;pair.Join();PlayNamed(pair,"NAME");
    CHECK(pair.client.AcrossMainMenu(pair.now));pair.Pump();
    CHECK(pair.client.RequestResume("Missing",pair.now));pair.Pump();
    CHECK(!pair.client.RejectResume(pair.now,1));
    CHECK(pair.host.RejectResume(pair.now,7));
    coop::Outgoing reject{};CHECK(pair.host.PopOutgoing(reject));
    CHECK(reject.message.type==coop::MessageType::ResumeRejected);
    CHECK(pair.client.Snapshot(pair.now).resumeRequestPending);
    pair.now+=coop::kRetryMs;pair.host.Tick(pair.now);pair.Pump();
    CHECK(!pair.client.Snapshot(pair.now).resumeRequestPending);
    CHECK(pair.client.Snapshot(pair.now).resumeRejectReason==7);
    CHECK(CountActions(pair.client,coop::ActionType::ResumeRejected)==1);
    CHECK(pair.client.RequestResume("NAME",pair.now));pair.Pump();
    CHECK(pair.host.RequestCareer(pair.now,true,"NAME",1));pair.Pump();
    const auto ticket=pair.client.Snapshot(pair.now).ticket;
    reject.message.sequence+=100;
    pair.client.Receive(reject.message,sizeof(reject.message),hostEndpoint,pair.now);
    CHECK(pair.client.Snapshot(pair.now).ticket==ticket);
    CHECK(pair.client.Snapshot(pair.now).resumeRejectReason==0);
    CHECK(CountActions(pair.client,coop::ActionType::ResumeRejected)==0);
}
void HostCanAdoptProvenLiveCareerAfterPeerRejoin() {
    Pair pair;pair.Join();PlayNamed(pair,"NAME");
    CHECK(!pair.host.AdoptPlayingCareer("Player",pair.now));
    pair.client.Leave(pair.now);pair.Pump();
    CHECK(!pair.host.AdoptPlayingCareer("NAME",pair.now));
    CHECK(pair.client.BeginBrowse("Client",pair.now));pair.Join();
    CHECK(pair.host.Snapshot(pair.now).ticket==0);
    CHECK(!pair.client.AdoptPlayingCareer("NAME",pair.now));
    CHECK(pair.host.AdoptPlayingCareer("NAME",pair.now));
    CHECK(!pair.host.AdoptPlayingCareer("NAME",pair.now));
    CHECK(!pair.client.RequestResume("NAME",pair.now));
    CHECK(pair.host.RequestCareer(pair.now,true,"NAME",1));pair.Pump();
    CHECK(pair.host.Snapshot(pair.now).localRetainsNative);
    CHECK(!pair.client.Snapshot(pair.now).localRetainsNative);
    FinishGeneration(pair,pair.host.Snapshot(pair.now).ticket);
}
void ReturnDuringNativeMenuSelectionCancelsTicketWork() {
    for(unsigned clientReturns=0;clientReturns<2;++clientReturns) {
        Pair pair;pair.Join();const auto ticket=pair.Select();DrainActions(pair);
        auto& returning=clientReturns?pair.client:pair.host;
        auto& peer=clientReturns?pair.host:pair.client;
        CHECK(returning.Snapshot(pair.now).localInMainMenu);
        CHECK(returning.ReadyForCareer(ticket,pair.now));
        CHECK(returning.AcrossMainMenu(pair.now));pair.Pump();
        CHECK(returning.Snapshot(pair.now).phase==coop::Phase::Lobby);
        CHECK(!returning.Snapshot(pair.now).localCareerReady);
        CHECK(peer.Snapshot(pair.now).peerInMainMenu);
        CHECK(!peer.ReadyForCareer(ticket,pair.now));
        CHECK(!returning.VoteSkip(ticket,pair.now));
        CHECK(CountActions(returning,coop::ActionType::StartCareer)==0);
        pair.now+=coop::kRetryMs;returning.Tick(pair.now);
        coop::Outgoing outgoing{};
        while(returning.PopOutgoing(outgoing))CHECK(outgoing.message.type!=coop::MessageType::CareerReady);
        CHECK(pair.host.RequestCareer(pair.now,true,"NAME"));pair.Pump();
        FinishGeneration(pair,pair.host.Snapshot(pair.now).ticket);
    }
    // Host Start was valid when sent, but its later return reaches Client first.
    Pair pair;pair.Join();const auto ticket=pair.Select();
    CHECK(pair.host.ReadyForCareer(ticket,pair.now));pair.Pump();
    CHECK(pair.client.ReadyForCareer(ticket,pair.now));
    coop::Outgoing outgoing{},start{},menu{};
    while(pair.client.PopOutgoing(outgoing))pair.host.Receive(outgoing.message,sizeof(outgoing.message),clientEndpoint,pair.now);
    while(pair.host.PopOutgoing(outgoing)) {
        if(outgoing.message.type==coop::MessageType::CareerStart)start=outgoing;
        else pair.client.Receive(outgoing.message,sizeof(outgoing.message),hostEndpoint,pair.now);
    }
    CHECK(start.message.sequence!=0);
    CHECK(pair.host.AcrossMainMenu(pair.now));
    while(pair.host.PopOutgoing(outgoing))if(outgoing.message.type==coop::MessageType::MainMenu)menu=outgoing;
    CHECK(menu.message.sequence!=0);
    pair.client.Receive(menu.message,sizeof(menu.message),hostEndpoint,pair.now);
    pair.client.Receive(start.message,sizeof(start.message),hostEndpoint,pair.now);
    CHECK(pair.client.Snapshot(pair.now).phase==coop::Phase::ProfileSync);
    CHECK(CountActions(pair.client,coop::ActionType::StartCareer)==0);
}
void SimultaneousReturnCanDowngradeStaleRetainProposal() {
    Pair pair;pair.Join();PlayNamed(pair,"NAME");
    CHECK(pair.host.AcrossMainMenu(pair.now));pair.Pump();
    CHECK(pair.client.AcrossMainMenu(pair.now));
    // Host has not received Client's old-ticket return yet.
    CHECK(!pair.host.Snapshot(pair.now).peerInMainMenu);
    CHECK(pair.host.RequestCareer(pair.now,true,"NAME",2));
    pair.Pump();
    const auto ticket=pair.host.Snapshot(pair.now).ticket;
    CHECK(ticket==pair.client.Snapshot(pair.now).ticket);
    CHECK(!pair.client.Snapshot(pair.now).localRetainsNative);
    CHECK(pair.host.ReadyForCareer(ticket,pair.now));
    CHECK(pair.client.ReadyForCareer(ticket,pair.now));pair.Pump();
    CHECK(!pair.host.Snapshot(pair.now).peerRetainsNative);
    CHECK(pair.host.Snapshot(pair.now).retainPlayingMask==0);
    CHECK(pair.client.Snapshot(pair.now).retainPlayingMask==0);
    CHECK(pair.host.BeginInitialLoad(ticket,pair.now));
    CHECK(pair.client.BeginInitialLoad(ticket,pair.now));
    CHECK(pair.host.MarkInitialLoaded(ticket,pair.now));
    CHECK(pair.client.MarkInitialLoaded(ticket,pair.now));pair.Pump();
    CHECK(pair.host.Snapshot(pair.now).initialBarrierComplete);
    CHECK(pair.client.Snapshot(pair.now).initialBarrierComplete);
}
int main() {
    {
        Pair pair;pair.Join();PlayNamed(pair,"NAME");
        CHECK(pair.host.AcrossMainMenu(pair.now));pair.Pump();
        CHECK(pair.host.RequestCareer(pair.now,true,"Name",2));pair.Pump();
        CHECK(pair.client.Snapshot(pair.now).localRetainsNative);
    }
    {
        Pair pair;pair.Join();PlayNamed(pair,"NAME");
        CHECK(pair.host.AcrossMainMenu(pair.now));pair.Pump();
        CHECK(pair.host.RequestCareer(pair.now,true,"NAME",2));pair.Pump();
        const auto ticket=pair.client.Snapshot(pair.now).ticket;
        CHECK(pair.client.Snapshot(pair.now).localRetainsNative);
        CHECK(!pair.client.DeclineNativeRetention(ticket+1));
        CHECK(pair.client.DeclineNativeRetention(ticket));
        CHECK(!pair.client.DeclineNativeRetention(ticket));
        CHECK(pair.host.ReadyForCareer(ticket,pair.now));
        CHECK(pair.client.ReadyForCareer(ticket,pair.now));pair.Pump();
        CHECK(!pair.host.Snapshot(pair.now).peerRetainsNative);
        CHECK(pair.host.Snapshot(pair.now).retainPlayingMask==0);
        CHECK(!pair.client.DeclineNativeRetention(ticket));
    }
    SimultaneousReturnCanDowngradeStaleRetainProposal();
    ReturnDuringNativeMenuSelectionCancelsTicketWork();
    ResumeRejectionIsReliableAndTicketBound();
    HostCanAdoptProvenLiveCareerAfterPeerRejoin();
    ReturnedPeerCannotFinishAbandonedBarrier();
    MainMenuPresenceAndHotRejoin();
    SimultaneousMenuAndDifferentCampaignSwitch();
    AcceptedControlsOnlyAreAcknowledged();
    ReorderedResumeAndReliableRetry();
    OldReadyAndMembershipCannotReleaseFreshLoad();
    ConfirmedNewCareerReplacesOldTicket();
    GameplayLiveness();
    PlayingCareerSurvivesReconnectWindow();
    DiscoveryAndExplicitRoles();
    JoinAckAndRetries();
    RejectMalformedAndWrongPeer();
    HeartbeatPersistenceAndReconnect();
    CareerTransactionAndDuplicateSuppression();
    BothSkipVotesAndInitialBarrier();
    UdpReorderingAndSequenceWrap();
    LeaveCancelsActionsAndRetriesAck();
    BoundedQueuesAndClockWrap();
    std::printf("CoopSession: %d checks passed\n", checks);
    return 0;
}
