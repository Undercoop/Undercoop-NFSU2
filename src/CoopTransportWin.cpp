// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include "CoopTransport.h"
#include "CoopTime.h"
#include <enet/enet.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace coop_transport {

struct Transport::Impl {
    enum class Kind : std::uint8_t { Hello = 1, HelloAck = 2, Data = 3 };
#pragma pack(push, 1)
    struct Header {
        std::uint32_t magic = kMagic;
        std::uint16_t version = kVersion, headerBytes = sizeof(Header);
        std::uint64_t session = 0;
        std::uint32_t membershipNonce = 0;
        std::uint16_t sourcePort = 0;
        Kind kind = Kind::Hello;
        Delivery delivery = Delivery::ReliableControl;
        std::uint16_t payloadBytes = 0;
        std::uint8_t hostRole = 0, reserved = 0;
        std::uint64_t streamKey = 0;
    };
#pragma pack(pop)
    static_assert(sizeof(Header) == 36, "Transport envelope layout changed");
    struct Message {
        std::array<std::uint8_t, kMaxPayloadBytes> data{};
        std::size_t bytes = 0;
        std::uint64_t streamKey = 0;
        std::uint64_t session = 0, generation = 0;
        std::uint32_t membershipNonce = 0;
        coop::Endpoint peer{};
        std::uint32_t produced = 0;
        Delivery delivery = Delivery::ReliableControl;
    };

    bool Open(std::uint16_t localBasePort);
    void Close();
    void SetPeer(std::uint64_t sessionId, std::uint32_t membershipNonce,
                 coop::Endpoint peer, bool host, std::uint32_t now);
    bool Queue(const void* data, std::size_t bytes, coop::Endpoint peer,
               Delivery delivery, std::uint64_t streamKey, std::uint32_t now);
    void Poll(std::uint32_t now);
    int Receive(void* data, std::size_t capacity, coop::Endpoint& canonicalSender);
    bool Ready(std::uint64_t sessionId, std::uint32_t membershipNonce, coop::Endpoint peer) const;
    Statistics Snapshot() const;
    bool OwnerLocked();
    bool ReadyLocked() const;
    void CloseLocked();
    void ClearPeerLocked();
    void RetireQueuesLocked();
    bool CurrentLocked(const Message& message) const;
    void CollectSocketCountersLocked();
    bool SubmitLocked(const Message& message);
    bool SendHandshakeLocked(Kind kind);
    bool ValidateLocked(const ENetEvent& event, Header& header) const;
    void EventLocked(ENetEvent& event, std::uint32_t now);
    void DrainLocked(std::uint32_t now);
    static void ReleasedReliable(ENetPacket* packet);

    mutable std::mutex mutex_;
    std::thread::id owner_{};
    ENetHost* socket_ = nullptr;
    ENetPeer* link_ = nullptr;
    bool initialized_ = false, host_ = false, connected_ = false;
    bool peerHello_ = false, helloAcknowledged_ = false;
    std::uint16_t localBasePort_ = 0;
    std::uint64_t session_ = 0;
    std::uint64_t generation_ = 0;
    std::uint32_t nonce_ = 0, lastConnect_ = 0;
    coop::Endpoint peer_{};
    std::size_t reliableInFlightBytes_ = 0;
    std::deque<Message> controls_, bulk_, incoming_;
    std::vector<Message> states_;
    Statistics stats_{};
};

} // namespace coop_transport

#include "CoopTransportEnet.inl"

namespace coop_transport {
Transport::Transport() : impl_(new Impl()) {}
Transport::~Transport() {
    // The worker must be stopped before destruction. Final lifetime cleanup
    // can then run on the creating thread without leaking a host merely
    // because its former owner thread has already exited. No event dispatch
    // occurs here; packet callbacks only retire private byte accounting.
    { std::lock_guard<std::mutex> guard(impl_->mutex_); impl_->CloseLocked(); }
    delete impl_;
}
bool Transport::Open(std::uint16_t port) { return impl_->Open(port); }
void Transport::Close() { impl_->Close(); }
void Transport::SetPeer(std::uint64_t session, std::uint32_t nonce, coop::Endpoint peer, bool host, std::uint32_t now) {
    impl_->SetPeer(session, nonce, peer, host, now);
}
bool Transport::Queue(const void* data, std::size_t bytes, coop::Endpoint peer, Delivery delivery,
                      std::uint64_t stream, std::uint32_t now) {
    return impl_->Queue(data, bytes, peer, delivery, stream, now);
}
void Transport::Poll(std::uint32_t now) { impl_->Poll(now); }
int Transport::Receive(void* data, std::size_t capacity, coop::Endpoint& sender) { return impl_->Receive(data, capacity, sender); }
bool Transport::Ready(std::uint64_t session, std::uint32_t nonce, coop::Endpoint peer) const { return impl_->Ready(session, nonce, peer); }
Statistics Transport::Snapshot() const { return impl_->Snapshot(); }
} // namespace coop_transport
