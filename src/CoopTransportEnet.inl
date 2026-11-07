// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once

namespace coop_transport {

inline bool Transport::Impl::OwnerLocked() {
    if (owner_ == std::this_thread::get_id()) return true;
    ++stats_.ownerViolations;
    return false;
}

inline bool Transport::Impl::Open(std::uint16_t localBasePort) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (socket_ && !OwnerLocked()) return false;
    CloseLocked();
    stats_ = {};
    owner_ = std::this_thread::get_id();
    if (!localBasePort || localBasePort > 65535u - kPortOffset) return false;
    if (enet_initialize() != 0) return false;
    initialized_ = true;
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = static_cast<enet_uint16>(localBasePort + kPortOffset);
    socket_ = enet_host_create(&address, 1, 3, 0, 512 * 1024);
    if (!socket_) { CloseLocked(); return false; }
    localBasePort_ = localBasePort;
    socket_->mtu = static_cast<enet_uint32>(kMtu);
    socket_->maximumPacketSize = sizeof(Header) + kMaxPayloadBytes;
    socket_->maximumWaitingData = 512 * 1024;
    socket_->checksum = enet_crc32; // Corruption detection, not authentication.
    stats_.open = true;
    return true;
}

inline void Transport::Impl::Close() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (socket_ && !OwnerLocked()) return;
    CloseLocked();
}

inline void Transport::Impl::CloseLocked() {
    if (socket_) {
        CollectSocketCountersLocked();
        enet_host_destroy(socket_);
        socket_ = nullptr;
    }
    if (initialized_) { enet_deinitialize(); initialized_ = false; }
    link_ = nullptr;
    RetireQueuesLocked();
    reliableInFlightBytes_ = 0;
    session_ = 0; nonce_ = 0; peer_ = {}; localBasePort_ = 0;
    connected_ = peerHello_ = helloAcknowledged_ = false;
    stats_.open = false;
}

inline void Transport::Impl::ClearPeerLocked() {
    // A host can already have an ACKNOWLEDGING_CONNECT peer before CONNECT
    // produces link_. Retire that pending handshake too, or a new membership
    // at the same endpoint can be refused until the old peer times out.
    if (socket_) for (std::size_t i = 0; i < socket_->peerCount; ++i)
        enet_peer_reset(&socket_->peers[i]);
    link_ = nullptr;
    RetireQueuesLocked();
    reliableInFlightBytes_ = 0;
    connected_ = peerHello_ = helloAcknowledged_ = false;
}

inline void Transport::Impl::RetireQueuesLocked() {
    stats_.retired += controls_.size() + bulk_.size() + states_.size() + incoming_.size();
    controls_.clear(); bulk_.clear(); states_.clear(); incoming_.clear();
    if (!++generation_) ++generation_;
}

inline bool Transport::Impl::CurrentLocked(const Message& message) const {
    return message.generation == generation_ && message.session == session_ &&
        message.membershipNonce == nonce_ && message.peer == peer_;
}

inline void Transport::Impl::SetPeer(std::uint64_t sessionId, std::uint32_t membershipNonce,
                               coop::Endpoint peer, bool host, std::uint32_t now) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!socket_ || !OwnerLocked()) return;
    if (!sessionId || !membershipNonce || !peer.Valid() || peer.port > 65535u - kPortOffset) {
        sessionId = 0; membershipNonce = 0; peer = {}; host = false;
    }
    if (session_ == sessionId && nonce_ == membershipNonce && peer_ == peer && host_ == host) return;
    ClearPeerLocked();
    session_ = sessionId; nonce_ = membershipNonce; peer_ = peer; host_ = host;
    lastConnect_ = now - 1000u;
}

inline bool Transport::Impl::Queue(const void* data, std::size_t bytes, coop::Endpoint peer,
                             Delivery delivery, std::uint64_t streamKey, std::uint32_t now) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!ReadyLocked() || peer != peer_ || !data || !bytes || bytes > kMaxPayloadBytes ||
        static_cast<unsigned>(delivery) > static_cast<unsigned>(Delivery::ReliableBulk) ||
        (delivery == Delivery::LatestState && (!streamKey || bytes > kUnfragmentedPayloadBytes))) {
        ++stats_.rejected; return false;
    }
    if (delivery == Delivery::ReliableControl && bytes > kUnfragmentedPayloadBytes)
        delivery = Delivery::ReliableBulk;
    Message message{};
    message.bytes = bytes; message.delivery = delivery; message.streamKey = streamKey; message.produced = now;
    message.session = session_; message.membershipNonce = nonce_; message.peer = peer_; message.generation = generation_;
    std::memcpy(message.data.data(), data, bytes);
    if (delivery == Delivery::LatestState) {
        for (auto& old : states_) if (old.streamKey == streamKey) {
            old = message; ++stats_.coalesced; ++stats_.enqueued; return true;
        }
        if (states_.size() >= kStateLimit) { ++stats_.rejected; return false; }
        states_.push_back(message);
    } else {
        auto& queue = delivery == Delivery::ReliableControl ? controls_ : bulk_;
        const auto limit = delivery == Delivery::ReliableControl ? kControlLimit : kBulkLimit;
        if (queue.size() >= limit) { ++stats_.rejected; return false; }
        queue.push_back(message);
    }
    ++stats_.enqueued;
    return true;
}

inline bool Transport::Impl::ReadyLocked() const {
    return socket_ && link_ && connected_ && peerHello_ && helloAcknowledged_ && session_ && nonce_ && peer_.Valid();
}

inline bool Transport::Impl::Ready(std::uint64_t sessionId, std::uint32_t membershipNonce, coop::Endpoint peer) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return ReadyLocked() && session_ == sessionId && nonce_ == membershipNonce && peer_ == peer;
}

inline void Transport::Impl::ReleasedReliable(ENetPacket* packet) {
    auto* self = static_cast<Transport::Impl*>(packet->userData);
    if (self) self->reliableInFlightBytes_ -= (std::min)(self->reliableInFlightBytes_, packet->dataLength);
}

inline bool Transport::Impl::SubmitLocked(const Message& message) {
    if (!ReadyLocked() || !CurrentLocked(message)) return false;
    const bool reliable = message.delivery != Delivery::LatestState;
    const auto wireBytes = sizeof(Header) + message.bytes;
    // Reserve room for small control/handshake traffic while bulk is congested.
    const auto reserve = message.delivery == Delivery::ReliableBulk ? 16 * 1024u : 0u;
    if (reliable && reliableInFlightBytes_ + wireBytes + reserve > kReliableInFlightBytes) return false;
    std::array<std::uint8_t, sizeof(Header) + kMaxPayloadBytes> wire{};
    Header header{};
    header.session = message.session; header.membershipNonce = message.membershipNonce; header.sourcePort = localBasePort_;
    header.hostRole = host_ ? 1u : 0u; header.kind = Kind::Data; header.delivery = message.delivery;
    header.payloadBytes = static_cast<std::uint16_t>(message.bytes); header.streamKey = message.streamKey;
    std::memcpy(wire.data(), &header, sizeof(header));
    std::memcpy(wire.data() + sizeof(header), message.data.data(), message.bytes);
    // State is independently sequenced by each application entity. Unsequenced
    // ENet delivery avoids dropping entity A solely because entity B arrived first.
    const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(wire.data(), wireBytes, flags);
    if (!packet) return false;
    if (reliable) {
        reliableInFlightBytes_ += wireBytes;
        packet->userData = this; packet->freeCallback = ReleasedReliable;
    }
    if (enet_peer_send(link_, static_cast<enet_uint8>(message.delivery), packet) != 0) {
        enet_packet_destroy(packet); return false;
    }
    ++stats_.submitted;
    return true;
}

inline bool Transport::Impl::SendHandshakeLocked(Kind kind) {
    if (!link_ || !connected_) return false;
    Header header{};
    header.session = session_; header.membershipNonce = nonce_; header.sourcePort = localBasePort_;
    header.hostRole = host_ ? 1u : 0u; header.kind = kind;
    ENetPacket* packet = enet_packet_create(&header, sizeof(header), ENET_PACKET_FLAG_RELIABLE);
    if (!packet) return false;
    if (enet_peer_send(link_, 0, packet) != 0) { enet_packet_destroy(packet); return false; }
    return true;
}

inline bool Transport::Impl::ValidateLocked(const ENetEvent& event, Header& header) const {
    if (!session_ || !nonce_ || event.peer != link_ || !event.packet || event.packet->dataLength < sizeof(Header)) return false;
    std::memcpy(&header, event.packet->data, sizeof(header));
    if (header.magic != kMagic || header.version != kVersion || header.headerBytes != sizeof(Header) ||
        header.session != session_ || header.membershipNonce != nonce_ || header.sourcePort != peer_.port ||
        header.hostRole != (host_ ? 0u : 1u) || header.reserved ||
        header.payloadBytes > kMaxPayloadBytes || event.packet->dataLength != sizeof(Header) + header.payloadBytes ||
        static_cast<unsigned>(header.delivery) > static_cast<unsigned>(Delivery::ReliableBulk)) return false;
    if (header.kind == Kind::Hello || header.kind == Kind::HelloAck)
        return !header.payloadBytes && !header.streamKey && header.delivery == Delivery::ReliableControl && event.channelID == 0;
    if (header.kind != Kind::Data || !header.payloadBytes || event.channelID != static_cast<enet_uint8>(header.delivery)) return false;
    return header.delivery != Delivery::LatestState || (header.streamKey && header.payloadBytes <= kUnfragmentedPayloadBytes);
}

inline void Transport::Impl::EventLocked(ENetEvent& event, std::uint32_t now) {
    if (event.type == ENET_EVENT_TYPE_CONNECT) {
        const bool expected = session_ && nonce_ && peer_.Valid() &&
            event.peer->address.host == peer_.address && event.peer->address.port == peer_.port + kPortOffset &&
            (!link_ || link_ == event.peer);
        if (!expected) { ++stats_.helloRejected; enet_peer_reset(event.peer); return; }
        // Even a reconnect at the same canonical endpoint and membership is
        // a new delivery generation. Old application work is never replayed.
        RetireQueuesLocked();
        link_ = event.peer; connected_ = true; peerHello_ = helloAcknowledged_ = false;
        enet_peer_timeout(link_, 8, 2000, 5000);
        enet_peer_ping_interval(link_, 500);
        ++stats_.connects;
        if (!SendHandshakeLocked(Kind::Hello)) { ClearPeerLocked(); lastConnect_ = now; }
    } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
        if (event.peer == link_) {
            ++stats_.disconnects; ClearPeerLocked(); lastConnect_ = now;
        }
    } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        Header header{};
        if (!ValidateLocked(event, header)) { ++stats_.invalid; enet_packet_destroy(event.packet); return; }
        if (header.kind == Kind::Hello) {
            peerHello_ = true;
            if (!SendHandshakeLocked(Kind::HelloAck)) ++stats_.helloRejected;
        } else if (header.kind == Kind::HelloAck) {
            helloAcknowledged_ = true;
        } else if (!peerHello_) {
            ++stats_.invalid;
        } else if (incoming_.size() >= kReceiveLimit) {
            ++stats_.receiveOverflow;
        } else {
            Message message{};
            message.bytes = header.payloadBytes; message.delivery = header.delivery; message.streamKey = header.streamKey;
            message.session = header.session; message.membershipNonce = header.membershipNonce;
            message.peer = peer_; message.generation = generation_;
            message.produced = now;
            std::memcpy(message.data.data(), event.packet->data + sizeof(Header), message.bytes);
            incoming_.push_back(message); ++stats_.received;
        }
        enet_packet_destroy(event.packet);
    }
}

inline void Transport::Impl::DrainLocked(std::uint32_t now) {
    if (!ReadyLocked()) return;
    std::size_t sent = 0;
    while (!controls_.empty() && sent < 16) {
        if (!CurrentLocked(controls_.front())) { controls_.pop_front(); ++stats_.retired; continue; }
        if (coop_time::Elapsed(now, controls_.front().produced) > 5000u) { controls_.pop_front(); ++stats_.expired; continue; }
        if (!SubmitLocked(controls_.front())) break;
        controls_.pop_front(); ++sent;
    }
    for (auto it = states_.begin(); it != states_.end() && sent < 40;) {
        if (!CurrentLocked(*it)) { it = states_.erase(it); ++stats_.retired; continue; }
        if (coop_time::Elapsed(now, it->produced) > 250u) { it = states_.erase(it); ++stats_.expired; continue; }
        if (!SubmitLocked(*it)) break;
        it = states_.erase(it); ++sent;
    }
    for (unsigned sentBulk = 0; !bulk_.empty() && sentBulk < 2; ++sentBulk) {
        if (!CurrentLocked(bulk_.front())) { bulk_.pop_front(); ++stats_.retired; continue; }
        if (coop_time::Elapsed(now, bulk_.front().produced) > 5000u) { bulk_.pop_front(); ++stats_.expired; continue; }
        if (!SubmitLocked(bulk_.front())) break;
        bulk_.pop_front();
    }
}

inline void Transport::Impl::CollectSocketCountersLocked() {
    if (!socket_) return;
    stats_.socketTxBytes += socket_->totalSentData; stats_.socketTxDatagrams += socket_->totalSentPackets;
    stats_.socketRxBytes += socket_->totalReceivedData; stats_.socketRxDatagrams += socket_->totalReceivedPackets;
    socket_->totalSentData = socket_->totalSentPackets = socket_->totalReceivedData = socket_->totalReceivedPackets = 0;
}

inline void Transport::Impl::Poll(std::uint32_t now) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!socket_ || !OwnerLocked()) return;
    if (session_ && peer_.Valid() && !host_ && !link_ && coop_time::Elapsed(now, lastConnect_) >= 1000u) {
        ENetAddress address{peer_.address, static_cast<enet_uint16>(peer_.port + kPortOffset)};
        link_ = enet_host_connect(socket_, &address, 3, kMagic);
        if (link_) enet_peer_timeout(link_, 8, 2000, 5000);
        lastConnect_ = now;
    }
    ENetEvent event{};
    for (unsigned i = 0; i < 256; ++i) {
        // Stop dispatching before the bounded application queue fills. The
        // caller drains Receive each network iteration; never ACK and then
        // discard a reliable application message merely due to our queue.
        if (incoming_.size() >= kReceiveLimit) break;
        const int result = enet_host_service(socket_, &event, 0);
        if (result <= 0) break;
        EventLocked(event, now);
    }
    DrainLocked(now);
    enet_host_flush(socket_);
    CollectSocketCountersLocked();
}

inline int Transport::Impl::Receive(void* data, std::size_t capacity, coop::Endpoint& canonicalSender) {
    std::lock_guard<std::mutex> guard(mutex_);
    // A bulk channel can overtake the final HelloAck. Preserve that received
    // message until admission finishes instead of ACKing and discarding it.
    if (!socket_ || !OwnerLocked() || !ReadyLocked() || incoming_.empty()) return 0;
    while (!incoming_.empty() && !CurrentLocked(incoming_.front())) { incoming_.pop_front(); ++stats_.retired; }
    if (incoming_.empty()) return 0;
    Message message = incoming_.front(); incoming_.pop_front();
    if (!data || capacity < message.bytes) { ++stats_.invalid; return -1; }
    std::memcpy(data, message.data.data(), message.bytes);
    canonicalSender = message.peer;
    return static_cast<int>(message.bytes);
}

inline Statistics Transport::Impl::Snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    Statistics result = stats_;
    result.open = socket_ != nullptr; result.connected = connected_; result.ready = ReadyLocked();
    result.queuedControl = controls_.size(); result.queuedBulk = bulk_.size(); result.queuedState = states_.size();
    result.queuedReceive = incoming_.size(); result.reliableInFlightBytes = reliableInFlightBytes_;
    result.generation = generation_;
    if (link_ && connected_) {
        result.rttMs = link_->roundTripTime; result.rttVarianceMs = link_->roundTripTimeVariance;
        result.lossPartsPerMillion = static_cast<std::uint32_t>(static_cast<std::uint64_t>(link_->packetLoss) * 1000000u / ENET_PEER_PACKET_LOSS_SCALE);
    }
    return result;
}

} // namespace coop_transport
