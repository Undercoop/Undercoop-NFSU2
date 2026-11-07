// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Portable session state. The caller serializes all access and performs native
// UI/game work only after consuming actions on the game thread.
namespace coop {

constexpr std::uint32_t kSessionMagic = 0x5332434e; // NC2S
constexpr std::uint16_t kSessionVersion = 6; // Both peers use the same final timed leaderboard.
constexpr std::uint32_t kPeerTimeoutMs = 5000;
// A live career must survive an ENet reconnect even if membership datagrams
// were lost in the same burst. Discovery and initial selection stay on 5s.
constexpr std::uint32_t kPlayingPeerTimeoutMs = 15000;
constexpr std::uint32_t kRetryMs = 500;

// All session deadlines use a local monotonic clock, never a peer's uptime.
// A network loop can sample its clock just before the game thread publishes
// newer state under the session lock. Such a stale sample is age zero, not a
// 49-day timeout. The half-range comparison also retains DWORD wrap behavior.
inline std::uint32_t SessionElapsed(std::uint32_t now, std::uint32_t then) {
    const auto elapsed=now-then;
    return elapsed<0x80000000u?elapsed:0u;
}

struct Endpoint {
    std::uint32_t address = 0; // IPv4, network byte order.
    std::uint16_t port = 0;    // Host byte order.
    bool Valid() const { return address != 0 && port != 0; }
    friend bool operator==(const Endpoint& a, const Endpoint& b) {
        return a.address == b.address && a.port == b.port;
    }
    friend bool operator!=(const Endpoint& a, const Endpoint& b) { return !(a == b); }
};

enum class Role : std::uint8_t { Idle, Browser, Host, Joining, Client };
enum class Phase : std::uint8_t { Lobby, CareerSelection, ProfileSync, InitialLoading, Playing };
enum class MessageType : std::uint16_t {
    Beacon = 1, Join, JoinAck, Ack, Heartbeat, Leave,
    CareerSelect, CareerReady, CareerStart, SkipVote, SkipIntro, InitialReady,
    MainMenu, ResumeRequest, ResumeRejected
};

#pragma pack(push, 1)
struct SessionMessage {
    std::uint32_t magic = kSessionMagic;
    std::uint16_t version = kSessionVersion;
    std::uint16_t size = 96;
    std::uint64_t sessionId = 0;
    std::uint32_t sequence = 0;
    std::uint32_t ack = 0;
    MessageType type = MessageType::Beacon;
    std::uint16_t reserved = 0;
    std::uint32_t nonce = 0; // Join nonce, or career ticket for career messages.
    char name[32]{};
    std::uint32_t data[8]{};
};
#pragma pack(pop)
static_assert(sizeof(SessionMessage) == 96, "Session wire layout changed");
static_assert(sizeof(SessionMessage) <= 160, "Session messages must remain small");

enum class ActionType : std::uint8_t {
    CareerSelect, StartCareer, SkipIntro, PeerJoined, PeerLeft, Closed, InitialLoadReleased,
    PeerMainMenu, ResumeRequested, ResumeRejected
};
struct Action { ActionType type = ActionType::Closed; std::uint32_t ticket = 0; };
struct Outgoing { Endpoint endpoint{}; SessionMessage message{}; };
struct SessionSnapshot {
    Role role = Role::Idle;
    Phase phase = Phase::Lobby;
    std::uint64_t sessionId = 0;
    std::uint32_t ticket = 0;
    std::uint32_t membershipNonce = 0;
    Endpoint peer{};
    bool joined = false;
    bool fresh = false;
    std::uint32_t peerAgeMs = 0;
    bool startAllowed = false;
    bool localCareerReady = false;
    bool peerCareerReady = false;
    bool localSkipVote = false;
    bool peerSkipVote = false;
    bool skipDecided = false;
    bool initialBarrierActive = false;
    bool initialBarrierComplete = false;
    // Local diagnostics only; never serialized into SessionMessage.
    bool initialBarrierStarted = false;
    bool localInitialReady = false;
    bool peerInitialReady = false;
    // Membership survives main-menu visits, but readiness belongs to one ticket.
    bool careerLaunched = false;
    bool localInMainMenu = true;
    bool peerInMainMenu = true;
    bool localRetainsNative = false;
    bool peerRetainsNative = false;
    bool resumeRequestPending = false;
    std::uint32_t resumeRejectReason = 0;
    std::uint8_t retainPlayingMask = 0; // Host=1, Client=2; never copy a live profile.
    char campaignName[32]{};
    char requestedCampaignName[32]{};
    char hostName[32]{};
    char clientName[32]{};
    bool haveDiscoveredHost = false;
    Endpoint discoveredHost{};
    std::uint64_t discoveredSessionId = 0;
    char discoveredHostName[32]{};
};

class SessionCore {
public:
    bool BeginBrowse(const char* name, std::uint32_t now) {
        if (!ValidName(name, false) || (role_ != Role::Idle && role_ != Role::Browser)) return false;
        ResetSession();
        CopyName(localName_, name);
        role_ = Role::Browser;
        lastHeartbeat_ = now;
        return true;
    }

    bool Host(const char* name, std::uint64_t sessionId, std::uint32_t now) {
        if (!sessionId || !ValidName(name, false) ||
            (role_ != Role::Idle && role_ != Role::Browser)) return false;
        ResetSession();
        CopyName(localName_, name);
        CopyName(hostName_, name);
        sessionId_ = sessionId;
        role_ = Role::Host;
        lastHeartbeat_ = now;
        return true;
    }

    bool Join(Endpoint endpoint, std::uint64_t sessionId, const char* name, std::uint32_t now,
              std::uint32_t membershipNonce=0) {
        if (role_ != Role::Browser || !endpoint.Valid() || !sessionId || !ValidName(name, false))
            return false;
        ResetSession();
        CopyName(localName_, name);
        CopyName(clientName_, name);
        role_ = Role::Joining;
        peer_ = endpoint;
        sessionId_ = sessionId;
        peerReceived_ = now;
        // Native adapters supply a fresh process-independent nonce. The
        // deterministic fallback keeps the portable state-machine API usable
        // in fixtures; real Local and remote joins take the same explicit path.
        joinNonce_ = membershipNonce?membershipNonce:NextNonce();
        SessionMessage message = Make(MessageType::Join);
        message.nonce = joinNonce_;
        CopyName(message.name, localName_);
        return SendReliable(message, now);
    }

    void Leave(std::uint32_t now) {
        const Endpoint oldPeer = peer_;
        const std::uint64_t oldId = sessionId_;
        const std::uint32_t oldMembership = joinNonce_;
        const bool active = role_ != Role::Idle && role_ != Role::Browser;
        ResetSession();
        role_ = Role::Idle;
        if (active && oldPeer.Valid() && oldId) {
            SessionMessage message = Make(MessageType::Leave);
            message.sessionId = oldId;
            message.data[7] = oldMembership;
            SendReliableTo(message, oldPeer, now);
        }
        if (active) PushAction(ActionType::Closed, 0);
    }

    // Only call on a real native return, not while opening career selection.
    // Preserve membership and last campaign; retire this ticket's local load.
    bool AcrossMainMenu(std::uint32_t now) {
        if (!joined_ || (role_ != Role::Host && role_ != Role::Client)) return false;
        if (localInMainMenu_ && phase_==Phase::Lobby) return true;
        SessionMessage message = Make(MessageType::MainMenu);
        message.nonce = ticket_ ? ticket_ : joinNonce_;
        message.data[0] = ticket_;
        if (!SendReliable(message, now)) return false;
        localInMainMenu_ = true;
        localRetainsNative_ = false;
        localCareerReady_ = localInitialReady_ = false;
        initialBarrierStarted_ = initialBarrierComplete_ = false;
        careerStarted_ = false;
        phase_ = Phase::Lobby;
        RetireLocalLoadWork();
        return true;
    }
    // Compatibility for callers without a clock. New native adapters pass now.
    void AcrossMainMenu() { AcrossMainMenu(peerReceived_); }

    // A client can request Resume/Load only after this membership launched once.
    // The host decides whether this is a hot rejoin or coordinated save switch.
    bool RequestResume(const char* campaign, std::uint32_t now) {
        if (role_ != Role::Client || !Fresh(now) || !careerLaunched_ ||
            !localInMainMenu_ || !ticket_ || resumeRequestPending_ || !ValidName(campaign, false)) return false;
        for (const auto& pending : pending_)
            if (pending.used && pending.outgoing.message.type == MessageType::ResumeRequest)
                return false;
        SessionMessage message = Make(MessageType::ResumeRequest);
        message.nonce = ticket_;
        CopyName(message.name, campaign);
        if (!SendReliable(message, now)) return false;
        CopyName(requestedCampaignName_, campaign);
        resumeRequestPending_ = true;
        resumeRejectReason_=0;
        return true;
    }
    bool RejectResume(std::uint32_t now,std::uint32_t reasonCode) {
        if(role_!=Role::Host||!Fresh(now)||!resumeRequestPending_||!ticket_||!reasonCode)return false;
        SessionMessage message=Make(MessageType::ResumeRejected);
        message.nonce=ticket_;message.data[0]=reasonCode;CopyName(message.name,requestedCampaignName_);
        if(!SendReliable(message,now))return false;
        resumeRequestPending_=false;resumeRejectReason_=reasonCode;return true;
    }
    // Explicit native proof for an already-running Host after a fresh peer join.
    // This never deserializes a profile or assumes a vanished peer is loaded.
    bool AdoptPlayingCareer(const char* campaign,std::uint32_t now) {
        if(role_!=Role::Host||!Fresh(now)||phase_!=Phase::Lobby||ticket_||
            careerLaunched_||!ValidName(campaign,false))return false;
        CopyName(campaignName_,campaign);careerLaunched_=true;localInMainMenu_=false;
        peerInMainMenu_=true;return true;
    }

    // Only authenticated, newly accepted gameplay datagrams may refresh this.
    // Does not join, change peer/role, acknowledge a command or revive a leave.
    bool ObserveGameplay(std::uint64_t session, Endpoint sender, std::uint32_t now) {
        if(!joined_ || !session || session!=sessionId_ || sender!=peer_ ||
            (role_!=Role::Host && role_!=Role::Client))return false;
        peerReceived_=now;return true;
    }

    void Tick(std::uint32_t now) {
        if (peer_.Valid() && (role_ == Role::Host || role_ == Role::Client || role_ == Role::Joining) &&
            SessionElapsed(now,peerReceived_) >= PeerTimeout()) {
            RemovePeer(role_ != Role::Host);
        }
        for (auto& pending : pending_) {
            if (pending.used && pending.outgoing.message.type == MessageType::Leave &&
                SessionElapsed(now,pending.createdAt) >= kPeerTimeoutMs) {
                pending.used = false;
                continue;
            }
            if (pending.used && SessionElapsed(now,pending.sentAt) >= kRetryMs) {
                QueueOutgoing(pending.outgoing);
                pending.sentAt = now;
            }
        }
        if (joined_ && peer_.Valid() && SessionElapsed(now,lastHeartbeat_) >= 1000) {
            QueueOutgoing({peer_, Make(MessageType::Heartbeat)});
            lastHeartbeat_ = now;
        }
        if (haveDiscoveredHost_ && SessionElapsed(now,discoveredAt_) >= kPeerTimeoutMs) ClearDiscovery();
    }

    bool BuildBeacon(SessionMessage& message, std::uint32_t /*now*/) const {
        if (role_ != Role::Host || !sessionId_) return false;
        message = Make(MessageType::Beacon);
        CopyName(message.name, hostName_);
        message.data[0] = static_cast<std::uint32_t>(phase_);
        message.data[1] = joined_ ? 2u : 1u;
        return true;
    }

    void Receive(const void* bytes, std::size_t size, Endpoint sender, std::uint32_t now) {
        if (!bytes || size != sizeof(SessionMessage) || !sender.Valid()) return;
        SessionMessage message{};
        std::memcpy(&message, bytes, sizeof(message));
        if (!ValidMessage(message)) return;

        if (message.type == MessageType::Leave && sender == departedPeer_ &&
            message.sessionId == departedId_ && message.sequence == departedSequence_ &&
            message.data[7] == departedMembership_ &&
            SessionElapsed(now,departedAt_) < kPeerTimeoutMs) {
            SessionMessage ack{};
            ack.type = MessageType::Ack;
            ack.sessionId = departedId_;
            ack.ack = departedSequence_;
            ack.data[7] = departedMembership_;
            QueueOutgoing({sender, ack});
            return;
        }

        // A closing session can still acknowledge its final reliable Leave.
        if (message.type == MessageType::Ack) {
            Acknowledge(message.ack, message.sessionId, sender, now, message.data[7]);
            return;
        }
        if (message.type == MessageType::Beacon) {
            if (role_ != Role::Browser) return;
            haveDiscoveredHost_ = true;
            discoveredHost_ = sender;
            discoveredSessionId_ = message.sessionId;
            discoveredAt_ = now;
            CopyName(discoveredHostName_, message.name);
            return;
        }
        if (message.sessionId != sessionId_) return;
        if (message.type == MessageType::Join) {
            ReceiveJoin(message, sender, now);
            return;
        }
        if (sender != peer_ || (role_ != Role::Host && role_ != Role::Joining && role_ != Role::Client))
            return;

        if (message.type == MessageType::JoinAck) {
            if ((role_ != Role::Joining && role_ != Role::Client) || message.nonce != joinNonce_ ||
                message.data[7] != joinNonce_) return;
            Acknowledge(message.ack, message.sessionId, sender, now, message.data[7]);
            SendAck(message.sequence);
            peerReceived_ = now;
            if (!joined_) {
                role_ = Role::Client;
                joined_ = true;
                CopyName(hostName_, message.name);
                PushAction(ActionType::PeerJoined, 0);
                QueueOutgoing({peer_, Make(MessageType::Heartbeat)});
            }
            return;
        }
        if (!joined_) return;
        if ((message.type == MessageType::Heartbeat || message.type == MessageType::Leave) &&
            message.data[7] != joinNonce_) return;
        if (message.type == MessageType::Heartbeat) {
            peerReceived_ = now;
            return;
        }
        if (message.type == MessageType::Leave) {
            // Keep this ACK after removing other packets destined for the departed peer.
            const Outgoing ack{peer_, AckFor(message.sequence)};
            departedPeer_ = peer_;
            departedId_ = sessionId_;
            departedSequence_ = message.sequence;
            departedMembership_ = message.data[7];
            departedAt_ = now;
            RemovePeer(role_ != Role::Host);
            QueueOutgoing(ack);
            return;
        }
        if ((!CareerMessage(message.type) && message.type != MessageType::MainMenu &&
             message.type != MessageType::ResumeRequest && message.type != MessageType::ResumeRejected) || message.nonce == 0 ||
            message.data[7] != joinNonce_) return;
        // A retry is ACKed only if this exact control was accepted before. An
        // out-of-order command which cannot be applied is left pending to retry.
        if (SeenSequence(message.sequence)) {
            peerReceived_ = now;
            SendAck(message.sequence);
            return;
        }
        if (OldSequence(message.sequence)) return;
        if (message.type == MessageType::MainMenu) {
            if (message.data[0] != ticket_ ||
                message.nonce != (ticket_ ? ticket_ : joinNonce_) ||
                (lastPeerMenuSequence_ && !SequenceAfter(message.sequence,lastPeerMenuSequence_))) return;
            lastPeerMenuSequence_ = message.sequence;
            peerInMainMenu_ = true;
            peerReturnedForTicket_ = true;
            peerRetainsNative_ = false;
            peerCareerReady_ = peerInitialReady_ = false;
            AcceptControl(message, now);
            PushAction(ActionType::PeerMainMenu, ticket_);
            return;
        }
        if (message.type == MessageType::ResumeRequest) {
            if (role_ != Role::Host || !careerLaunched_ || !peerInMainMenu_ ||
                message.nonce != ticket_ || !ValidName(message.name,false) ||
                resumeRequestPending_) return;
            CopyName(requestedCampaignName_,message.name);
            resumeRequestPending_=true;
            AcceptControl(message, now);
            PushAction(ActionType::ResumeRequested,ticket_);
            return;
        }
        if(message.type==MessageType::ResumeRejected) {
            if(role_!=Role::Client||message.nonce!=ticket_||!resumeRequestPending_||!message.data[0]||
               std::strcmp(message.name,requestedCampaignName_)!=0)return;
            resumeRequestPending_=false;resumeRejectReason_=message.data[0];
            for(auto& pending:pending_)
                if(pending.used&&pending.outgoing.message.type==MessageType::ResumeRequest)pending.used=false;
            AcceptControl(message,now);PushAction(ActionType::ResumeRejected,ticket_);return;
        }
        if (message.type == MessageType::CareerSelect) {
            if (role_ != Role::Client || message.data[0] > 3 ||
                (lastCareerSelectSequence_ &&
                 !SequenceAfter(message.sequence,lastCareerSelectSequence_))) return;
        } else if (message.nonce != ticket_) return;
        if ((message.type == MessageType::CareerStart || message.type == MessageType::SkipIntro) &&
            role_ != Role::Client) return;

        switch (message.type) {
        case MessageType::CareerSelect:
            // Never retain an incompatible live campaign even if a buggy host
            // proposes it. Native return-to-menu must precede that load.
            if ((message.data[0] & 2u) && (!careerLaunched_ ||
                !SameCareer(message.name,campaignName_))) return;
            lastCareerSelectSequence_ = message.sequence;
            // Both players may reach FE before the Host receives the Client's
            // old-ticket MainMenu. Downgrade only OUR retention promise; the
            // fresh ticket still loads both and Ready reports the actual bit.
            StartSelection(message.nonce,message.name,static_cast<std::uint8_t>(
                localInMainMenu_?(message.data[0]&~2u):message.data[0]));
            break;
        case MessageType::CareerReady:
            if (peerReturnedForTicket_ ||
                (phase_ != Phase::CareerSelection && phase_ != Phase::ProfileSync)) return;
            {
                const std::uint8_t bit=role_==Role::Host?2:1;
                if(message.data[0]&&(message.data[0]!=bit||!(retainPlayingMask_&bit)))return;
                peerRetainsNative_=message.data[0]!=0;
                retainPlayingMask_=static_cast<std::uint8_t>((retainPlayingMask_&~bit)|message.data[0]);
            }
            peerCareerReady_ = true;
            MaybeStartCareer(now);
            break;
        case MessageType::CareerStart:
            if (peerReturnedForTicket_ || !localCareerReady_ ||
                (phase_ != Phase::CareerSelection && phase_ != Phase::ProfileSync)) return;
            StartCareer();
            break;
        case MessageType::SkipVote:
            if (phase_ == Phase::Lobby || phase_ == Phase::Playing) return;
            peerSkipVote_ = true;
            MaybeSkip(now);
            break;
        case MessageType::SkipIntro:
            if (!localSkipVote_ || skipDecided_ || phase_ == Phase::Lobby || phase_ == Phase::Playing) return;
            peerSkipVote_ = true;
            skipDecided_ = true;
            PushAction(ActionType::SkipIntro, ticket_);
            break;
        case MessageType::InitialReady:
            // Ready may overtake CareerStart in UDP; retain it for the ticket.
            if (peerReturnedForTicket_ || (phase_ != Phase::CareerSelection && phase_ != Phase::ProfileSync &&
                phase_ != Phase::InitialLoading && phase_ != Phase::Playing)) return;
            peerInitialReady_ = true;
            MaybeReleaseInitialLoad();
            break;
        default: return;
        }
        AcceptControl(message, now);
    }

    void Receive(const SessionMessage& message, std::size_t size, Endpoint sender, std::uint32_t now) {
        Receive(static_cast<const void*>(&message), size, sender, now);
    }

    bool RequestCareer(std::uint32_t now,bool replaceCurrent=false,
                       const char* campaign=nullptr,std::uint8_t retainPlayingMask=0) {
        if (role_ != Role::Host || !Fresh(now) ||
            (phase_ != Phase::Lobby && !replaceCurrent) || retainPlayingMask>3 ||
            (campaign && !ValidName(campaign,false))) return false;
        if (retainPlayingMask && (!campaign || !careerLaunched_ ||
            !SameCareer(campaign,campaignName_) ||
            ((retainPlayingMask&1u) && localInMainMenu_) ||
            ((retainPlayingMask&2u) && peerInMainMenu_))) return false;
        const std::uint32_t ticket = NextNonce();
        SessionMessage message = Make(MessageType::CareerSelect);
        message.nonce = ticket;
        message.data[0]=retainPlayingMask;
        CopyName(message.name,campaign);
        if (!SendReliable(message, now)) return false;
        StartSelection(ticket,campaign,retainPlayingMask);
        return true;
    }

    // The game-thread adapter must prove FreeRoam retention before advertising
    // readiness. A race, shop or pending event uses normal native teardown/load.
    bool DeclineNativeRetention(std::uint32_t ticket) {
        if(!ticket||ticket!=ticket_||!localRetainsNative_||localCareerReady_||
           (phase_!=Phase::CareerSelection&&phase_!=Phase::ProfileSync))return false;
        localRetainsNative_=false;
        retainPlayingMask_=static_cast<std::uint8_t>(retainPlayingMask_&~(role_==Role::Host?1u:2u));
        return true;
    }

    bool ReadyForCareer(std::uint32_t ticket, std::uint32_t now) {
        if (!Fresh(now) || !ticket || ticket != ticket_ || localCareerReady_ || peerReturnedForTicket_ ||
            (phase_ != Phase::CareerSelection && phase_ != Phase::ProfileSync)) return false;
        SessionMessage message = Make(MessageType::CareerReady);
        message.nonce = ticket;
        message.data[0]=localRetainsNative_?(role_==Role::Host?1u:2u):0u;
        if (!SendReliable(message, now)) return false;
        localCareerReady_ = true;
        phase_ = Phase::ProfileSync;
        MaybeStartCareer(now);
        return true;
    }

    bool VoteSkip(std::uint32_t ticket, std::uint32_t now) {
        if (!Fresh(now) || !ticket || ticket != ticket_ || localSkipVote_ || peerReturnedForTicket_ ||
            phase_==Phase::Lobby || phase_==Phase::Playing) return false;
        SessionMessage message = Make(MessageType::SkipVote);
        message.nonce = ticket;
        if (!SendReliable(message, now)) return false;
        localSkipVote_ = true;
        MaybeSkip(now);
        return true;
    }

    bool BeginInitialLoad(std::uint32_t ticket, std::uint32_t now) {
        if (!Fresh(now) || ticket != ticket_ || !ticket || phase_ != Phase::InitialLoading ||
            initialBarrierStarted_ || initialBarrierComplete_) return false;
        initialBarrierStarted_ = true;
        MaybeReleaseInitialLoad();
        return true;
    }

    bool MarkInitialLoaded(std::uint32_t ticket, std::uint32_t now) {
        if (!Fresh(now) || !initialBarrierStarted_ || initialBarrierComplete_ ||
            ticket != ticket_ || localInitialReady_) return false;
        SessionMessage message = Make(MessageType::InitialReady);
        message.nonce = ticket;
        if (!SendReliable(message, now)) return false;
        localInitialReady_ = true;
        MaybeReleaseInitialLoad();
        return true;
    }

    SessionSnapshot Snapshot(std::uint32_t now) const {
        SessionSnapshot snapshot{};
        snapshot.role = role_;
        snapshot.phase = phase_;
        snapshot.sessionId = sessionId_;
        snapshot.ticket = ticket_;
        snapshot.membershipNonce = joinNonce_;
        snapshot.peer = peer_;
        snapshot.joined = joined_;
        snapshot.fresh = Fresh(now);
        if (joined_) snapshot.peerAgeMs = SessionElapsed(now, peerReceived_);
        snapshot.startAllowed = role_ == Role::Host && snapshot.fresh && phase_ == Phase::Lobby;
        snapshot.localCareerReady = localCareerReady_;
        snapshot.peerCareerReady = peerCareerReady_;
        snapshot.localSkipVote = localSkipVote_;
        snapshot.peerSkipVote = peerSkipVote_;
        snapshot.skipDecided = skipDecided_;
        snapshot.initialBarrierActive = joined_ && initialBarrierStarted_ && !initialBarrierComplete_;
        snapshot.initialBarrierComplete = initialBarrierComplete_;
        snapshot.initialBarrierStarted = initialBarrierStarted_;
        snapshot.localInitialReady = localInitialReady_;
        snapshot.peerInitialReady = peerInitialReady_;
        snapshot.careerLaunched = careerLaunched_;
        snapshot.localInMainMenu = localInMainMenu_;
        snapshot.peerInMainMenu = peerInMainMenu_;
        snapshot.localRetainsNative = localRetainsNative_;
        snapshot.peerRetainsNative = peerRetainsNative_;
        snapshot.retainPlayingMask = retainPlayingMask_;
        snapshot.resumeRequestPending = resumeRequestPending_;
        snapshot.resumeRejectReason = resumeRejectReason_;
        CopyName(snapshot.campaignName,campaignName_);
        CopyName(snapshot.requestedCampaignName,requestedCampaignName_);
        CopyName(snapshot.hostName, hostName_);
        CopyName(snapshot.clientName, clientName_);
        snapshot.haveDiscoveredHost = haveDiscoveredHost_ && SessionElapsed(now,discoveredAt_) < kPeerTimeoutMs;
        if (snapshot.haveDiscoveredHost) {
            snapshot.discoveredHost = discoveredHost_;
            snapshot.discoveredSessionId = discoveredSessionId_;
            CopyName(snapshot.discoveredHostName, discoveredHostName_);
        }
        return snapshot;
    }

    bool PopOutgoing(Outgoing& outgoing) { return Pop(outgoing_, outgoingHead_, outgoingCount_, outgoing); }
    bool PopAction(Action& action) { return Pop(actions_, actionHead_, actionCount_, action); }

    static bool ValidMessage(const SessionMessage& message) {
        const auto type = static_cast<std::uint16_t>(message.type);
        if (message.magic != kSessionMagic || message.version != kSessionVersion ||
            message.size != sizeof(message) || message.reserved || !message.sessionId ||
            type < static_cast<std::uint16_t>(MessageType::Beacon) ||
            type > static_cast<std::uint16_t>(MessageType::ResumeRejected) ||
            !ValidName(message.name, true)) return false;
        if (message.type == MessageType::Ack) return message.ack != 0 && message.sequence == 0;
        if (message.type == MessageType::Beacon || message.type == MessageType::Heartbeat)
            return message.sequence == 0 && (message.type != MessageType::Beacon || message.name[0] != 0);
        if (!message.sequence) return false;
        if (message.type == MessageType::Join || message.type == MessageType::JoinAck)
            return message.nonce != 0 && message.name[0] != 0;
        if (message.type == MessageType::ResumeRequest && !message.name[0]) return false;
        return message.type == MessageType::Leave || message.nonce != 0;
    }

private:
    struct Pending { bool used = false; Outgoing outgoing{}; std::uint32_t sentAt = 0, createdAt = 0; };
    template <class T, std::size_t N>
    static bool Pop(std::array<T, N>& queue, std::size_t& head, std::size_t& count, T& value) {
        if (!count) return false;
        value = queue[head];
        head = (head + 1) % N;
        --count;
        return true;
    }
    template <class T, std::size_t N>
    static void Push(std::array<T, N>& queue, std::size_t& head, std::size_t& count, const T& value) {
        if (count == N) { head = (head + 1) % N; --count; }
        queue[(head + count) % N] = value;
        ++count;
    }
    static bool ValidName(const char* name, bool allowEmpty) {
        if (!name || (!allowEmpty && !name[0])) return false;
        for (std::size_t i = 0; i < 32; ++i) {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            if (!c) return true;
            if (c < 32 || c > 126) return false;
        }
        return false;
    }
    static void CopyName(char (&destination)[32], const char* source) {
        std::memset(destination, 0, sizeof(destination));
        if (!source) return;
        for (std::size_t i = 0; i < 31 && source[i]; ++i) destination[i] = source[i];
    }
    static bool SameCareer(const char* a,const char* b) {
        if(!a||!b)return false;
        for(std::size_t i=0;i<32;++i) {
            auto x=static_cast<unsigned char>(a[i]),y=static_cast<unsigned char>(b[i]);
            if(x>='a'&&x<='z')x-=32;if(y>='a'&&y<='z')y-=32;
            if(x!=y)return false;if(!x)return true;
        }
        return false;
    }
    static bool CareerMessage(MessageType type) {
        return type >= MessageType::CareerSelect && type <= MessageType::InitialReady;
    }
    SessionMessage Make(MessageType type) const {
        SessionMessage message{};
        message.sessionId = sessionId_;
        message.type = type;
        message.data[7] = joinNonce_;
        return message;
    }
    std::uint32_t NextNonce() { if (!++nonceCounter_) ++nonceCounter_; return nonceCounter_; }
    std::uint32_t NextSequence() { if (!++sequenceCounter_) ++sequenceCounter_; return sequenceCounter_; }
    std::uint32_t PeerTimeout() const {
        return phase_ == Phase::Playing ? kPlayingPeerTimeoutMs : kPeerTimeoutMs;
    }
    bool Fresh(std::uint32_t now) const {
        return joined_ && SessionElapsed(now, peerReceived_) < PeerTimeout();
    }
    void QueueOutgoing(const Outgoing& outgoing) { Push(outgoing_, outgoingHead_, outgoingCount_, outgoing); }
    void PushAction(ActionType type, std::uint32_t ticket) {
        Push(actions_, actionHead_, actionCount_, Action{type, ticket});
    }
    bool SendReliable(SessionMessage message, std::uint32_t now) {
        return peer_.Valid() && SendReliableTo(message, peer_, now);
    }
    bool SendReliableTo(SessionMessage message, Endpoint endpoint, std::uint32_t now) {
        for (auto& pending : pending_) {
            if (pending.used) continue;
            message.sequence = NextSequence();
            pending.used = true;
            pending.outgoing = {endpoint, message};
            pending.sentAt = now;
            pending.createdAt = now;
            QueueOutgoing(pending.outgoing);
            return true;
        }
        return false;
    }
    SessionMessage AckFor(std::uint32_t sequence) const {
        SessionMessage ack = Make(MessageType::Ack);
        ack.ack = sequence;
        return ack;
    }
    void SendAck(std::uint32_t sequence) { QueueOutgoing({peer_, AckFor(sequence)}); }
    void Acknowledge(std::uint32_t sequence, std::uint64_t id, Endpoint sender, std::uint32_t now,
                     std::uint32_t membership) {
        if (!sequence) return;
        for (auto& pending : pending_) {
            if (!pending.used || pending.outgoing.endpoint != sender ||
                pending.outgoing.message.sessionId != id || pending.outgoing.message.sequence != sequence ||
                pending.outgoing.message.data[7] != membership)
                continue;
            const MessageType type = pending.outgoing.message.type;
            pending.used = false;
            if (type == MessageType::JoinAck && role_ == Role::Host && sender == peer_ && !joined_) {
                joined_ = true;
                peerReceived_ = now;
                PushAction(ActionType::PeerJoined, 0);
                QueueOutgoing({peer_, Make(MessageType::Heartbeat)});
            }
            return;
        }
    }
    void ReceiveJoin(const SessionMessage& message, Endpoint sender, std::uint32_t now) {
        if (role_ != Role::Host || (peer_.Valid() && (sender != peer_ || message.nonce != joinNonce_))) return;
        if (!peer_.Valid()) {
            peer_ = sender;
            joinNonce_ = message.nonce;
            CopyName(clientName_, message.name);
        }
        peerReceived_ = now;
        if (lastJoinAck_.sequence != 0 && lastJoinAck_.nonce == joinNonce_) {
            QueueOutgoing({peer_, lastJoinAck_});
            return;
        }
        for (const auto& pending : pending_) {
            if (pending.used && pending.outgoing.message.type == MessageType::JoinAck) {
                QueueOutgoing(pending.outgoing);
                return;
            }
        }
        SessionMessage reply = Make(MessageType::JoinAck);
        reply.nonce = joinNonce_;
        reply.ack = message.sequence;
        CopyName(reply.name, hostName_);
        if (SendReliable(reply, now)) {
            for (const auto& pending : pending_) {
                if (pending.used && pending.outgoing.message.type == MessageType::JoinAck)
                    lastJoinAck_ = pending.outgoing.message;
            }
        }
    }
    static bool SequenceAfter(std::uint32_t a,std::uint32_t b) {
        return a!=b && a-b<0x80000000u;
    }
    bool SeenSequence(std::uint32_t sequence) const {
        if (!haveReceivedSequence_ || SequenceAfter(sequence,lastReceivedSequence_)) return false;
        const auto behind=lastReceivedSequence_-sequence;
        return behind<32 && (receivedWindow_&(1u<<behind))!=0;
    }
    bool OldSequence(std::uint32_t sequence) const {
        return haveReceivedSequence_ && !SequenceAfter(sequence,lastReceivedSequence_) &&
            lastReceivedSequence_-sequence>=32;
    }
    void AcceptControl(const SessionMessage& message,std::uint32_t now) {
        Duplicate(message.sequence);
        peerReceived_=now;
        SendAck(message.sequence);
    }
    bool Duplicate(std::uint32_t sequence) {
        if (!haveReceivedSequence_) {
            haveReceivedSequence_ = true;
            lastReceivedSequence_ = sequence;
            receivedWindow_ = 1;
            return false;
        }
        const std::uint32_t ahead = sequence - lastReceivedSequence_;
        if (ahead != 0 && ahead < 0x80000000u) {
            receivedWindow_ = ahead >= 32 ? 1u : (receivedWindow_ << ahead) | 1u;
            lastReceivedSequence_ = sequence;
            return false;
        }
        const std::uint32_t behind = lastReceivedSequence_ - sequence;
        if (behind >= 32) return true;
        const std::uint32_t bit = 1u << behind;
        if (receivedWindow_ & bit) return true;
        receivedWindow_ |= bit;
        return false;
    }
    void RetireLocalLoadWork() {
        for (auto& pending : pending_)
            if (pending.used && CareerMessage(pending.outgoing.message.type)) pending.used=false;
        const auto queued=outgoingCount_;
        Outgoing outgoing{};
        for(std::size_t i=0;i<queued && PopOutgoing(outgoing);++i)
            if(!CareerMessage(outgoing.message.type)) QueueOutgoing(outgoing);
        const auto actions=actionCount_;
        Action action{};
        for(std::size_t i=0;i<actions && PopAction(action);++i)
            if(action.type==ActionType::PeerJoined || action.type==ActionType::PeerLeft ||
               action.type==ActionType::Closed || action.type==ActionType::PeerMainMenu)
                PushAction(action.type,action.ticket);
    }
    void StartSelection(std::uint32_t ticket,const char* campaign=nullptr,std::uint8_t retainMask=0) {
        // Retire only old career work, not Join/Leave or the new selection.
        for (auto& pending : pending_)
            if (pending.used && ((CareerMessage(pending.outgoing.message.type) &&
                pending.outgoing.message.nonce != ticket) ||
                pending.outgoing.message.type==MessageType::ResumeRequest ||
                pending.outgoing.message.type==MessageType::ResumeRejected ||
                (pending.outgoing.message.type==MessageType::MainMenu &&
                 pending.outgoing.message.data[0]!=ticket))) pending.used = false;
        const std::size_t queued = outgoingCount_;
        Outgoing outgoing{};
        for (std::size_t i = 0; i < queued && PopOutgoing(outgoing); ++i)
            if ((!CareerMessage(outgoing.message.type) || outgoing.message.nonce == ticket) &&
                outgoing.message.type!=MessageType::ResumeRequest &&
                outgoing.message.type!=MessageType::ResumeRejected &&
                (outgoing.message.type!=MessageType::MainMenu || outgoing.message.data[0]==ticket))
                QueueOutgoing(outgoing);
        // Neither a pending StartCareer nor a queued old UI action may survive.
        const std::size_t actions = actionCount_;
        Action action{};
        for (std::size_t i = 0; i < actions && PopAction(action); ++i)
            if (action.type == ActionType::PeerJoined || action.type == ActionType::PeerLeft ||
                action.type == ActionType::Closed) PushAction(action.type, action.ticket);
        ResetCareer();
        ticket_ = ticket;
        CopyName(campaignName_,campaign);
        requestedCampaignName_[0]=0;
        resumeRequestPending_=false;
        resumeRejectReason_=0;
        retainPlayingMask_=retainMask;
        localRetainsNative_=(retainMask & (role_==Role::Host?1u:2u))!=0;
        peerRetainsNative_=(retainMask & (role_==Role::Host?2u:1u))!=0;
        phase_ = Phase::CareerSelection;
        PushAction(ActionType::CareerSelect, ticket);
    }
    void MaybeStartCareer(std::uint32_t now) {
        if (role_ != Role::Host || !localCareerReady_ || !peerCareerReady_ || careerStarted_) return;
        SessionMessage message = Make(MessageType::CareerStart);
        message.nonce = ticket_;
        if (SendReliable(message, now)) StartCareer();
    }
    void StartCareer() {
        if (careerStarted_) return;
        careerStarted_ = true;
        careerLaunched_ = true;
        localInMainMenu_=peerInMainMenu_=false;
        phase_ = Phase::InitialLoading;
        PushAction(ActionType::StartCareer, ticket_);
    }
    void MaybeSkip(std::uint32_t now) {
        if (role_ != Role::Host || !localSkipVote_ || !peerSkipVote_ || skipDecided_) return;
        SessionMessage message = Make(MessageType::SkipIntro);
        message.nonce = ticket_;
        if (SendReliable(message, now)) {
            skipDecided_ = true;
            PushAction(ActionType::SkipIntro, ticket_);
        }
    }
    void MaybeReleaseInitialLoad() {
        if (!initialBarrierStarted_ || initialBarrierComplete_ || !localInitialReady_ || !peerInitialReady_) return;
        initialBarrierComplete_ = true;
        phase_ = Phase::Playing;
        PushAction(ActionType::InitialLoadReleased, ticket_);
    }
    void ResetCareer() {
        ticket_ = 0;
        phase_ = Phase::Lobby;
        localCareerReady_ = peerCareerReady_ = careerStarted_ = false;
        localSkipVote_ = peerSkipVote_ = skipDecided_ = false;
        initialBarrierStarted_ = initialBarrierComplete_ = localInitialReady_ = peerInitialReady_ = false;
        localRetainsNative_=peerRetainsNative_=false;
        peerReturnedForTicket_=false;
        retainPlayingMask_=0;
    }
    void ClearDiscovery() {
        haveDiscoveredHost_ = false;
        discoveredHost_ = {};
        discoveredSessionId_ = 0;
        discoveredAt_ = 0;
        std::memset(discoveredHostName_, 0, sizeof(discoveredHostName_));
    }
    void RemovePeer(bool returnToBrowser) {
        const bool wasJoined = joined_;
        peer_ = {};
        joined_ = false;
        joinNonce_ = 0;
        lastJoinAck_ = {};
        peerReceived_ = 0;
        haveReceivedSequence_ = false;
        lastReceivedSequence_ = receivedWindow_ = 0;
        lastCareerSelectSequence_ = 0;
        lastPeerMenuSequence_=0;
        for (auto& pending : pending_) pending.used = false;
        outgoingHead_ = outgoingCount_ = 0;
        // Queued launch actions must not survive a peer departure.
        actionHead_ = actionCount_ = 0;
        ResetCareer();
        careerLaunched_=false;
        localInMainMenu_=peerInMainMenu_=true;
        resumeRequestPending_=false;
        resumeRejectReason_=0;
        campaignName_[0]=requestedCampaignName_[0]=0;
        if (returnToBrowser) {
            role_ = Role::Browser;
            sessionId_ = 0;
            std::memset(hostName_, 0, sizeof(hostName_));
        } else std::memset(clientName_, 0, sizeof(clientName_));
        if (wasJoined) PushAction(ActionType::PeerLeft, 0);
    }
    void ResetSession() {
        RemovePeer(false);
        role_ = Role::Idle;
        sessionId_ = 0;
        std::memset(hostName_, 0, sizeof(hostName_));
        std::memset(clientName_, 0, sizeof(clientName_));
        actionHead_ = actionCount_ = 0;
        ClearDiscovery();
    }

    Role role_ = Role::Idle;
    Phase phase_ = Phase::Lobby;
    Endpoint peer_{};
    std::uint64_t sessionId_ = 0;
    std::uint32_t ticket_ = 0, joinNonce_ = 0, nonceCounter_ = 0, sequenceCounter_ = 0;
    std::uint32_t peerReceived_ = 0, lastHeartbeat_ = 0;
    SessionMessage lastJoinAck_{};
    Endpoint departedPeer_{};
    std::uint32_t departedMembership_=0;
    std::uint64_t departedId_ = 0;
    std::uint32_t departedSequence_ = 0, departedAt_ = 0;
    bool joined_ = false;
    char localName_[32]{}, hostName_[32]{}, clientName_[32]{};
    bool localCareerReady_ = false, peerCareerReady_ = false, careerStarted_ = false;
    bool localSkipVote_ = false, peerSkipVote_ = false, skipDecided_ = false;
    bool initialBarrierStarted_ = false, initialBarrierComplete_ = false;
    bool localInitialReady_ = false, peerInitialReady_ = false;
    bool careerLaunched_=false,localInMainMenu_=true,peerInMainMenu_=true;
    bool localRetainsNative_=false,peerRetainsNative_=false,resumeRequestPending_=false;
    bool peerReturnedForTicket_=false;
    std::uint32_t resumeRejectReason_=0;
    std::uint8_t retainPlayingMask_=0;
    char campaignName_[32]{},requestedCampaignName_[32]{};
    bool haveReceivedSequence_ = false;
    std::uint32_t lastReceivedSequence_ = 0, receivedWindow_ = 0;
    std::uint32_t lastCareerSelectSequence_ = 0;
    std::uint32_t lastPeerMenuSequence_=0;
    bool haveDiscoveredHost_ = false;
    Endpoint discoveredHost_{};
    std::uint64_t discoveredSessionId_ = 0;
    std::uint32_t discoveredAt_ = 0;
    char discoveredHostName_[32]{};
    std::array<Pending, 16> pending_{};
    std::array<Outgoing, 16> outgoing_{};
    std::size_t outgoingHead_ = 0, outgoingCount_ = 0;
    std::array<Action, 16> actions_{};
    std::size_t actionHead_ = 0, actionCount_ = 0;
};

} // namespace coop
