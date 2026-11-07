// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once

#include "CoopSession.h"
#include <cstddef>
#include <cstdint>

// The existing UDP socket remains responsible for discovery and membership.
// This transport owns a separate socket on basePort + 1000. ENet operations
// belong to the network thread; producer threads can only enqueue copies.
namespace coop_transport {

constexpr std::uint16_t kPortOffset = 1000;
constexpr std::uint32_t kMagic = 0x5433434e; // NC3T
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kMtu = 1200;
constexpr std::size_t kMaxPayloadBytes = 4096;
constexpr std::size_t kUnfragmentedPayloadBytes = 1100;
constexpr std::size_t kControlLimit = 128, kBulkLimit = 64, kStateLimit = 96, kReceiveLimit = 256;
constexpr std::size_t kReliableInFlightBytes = 256 * 1024;

enum class Delivery : std::uint8_t { ReliableControl = 0, LatestState = 1, ReliableBulk = 2 };

struct Statistics {
    std::uint64_t enqueued = 0, rejected = 0, coalesced = 0, expired = 0, retired = 0;
    std::uint64_t submitted = 0, received = 0, invalid = 0, receiveOverflow = 0;
    std::uint64_t connects = 0, disconnects = 0, helloRejected = 0, ownerViolations = 0;
    std::uint64_t socketTxBytes = 0, socketTxDatagrams = 0, socketRxBytes = 0, socketRxDatagrams = 0;
    std::size_t queuedControl = 0, queuedState = 0, queuedBulk = 0, queuedReceive = 0;
    std::size_t reliableInFlightBytes = 0;
    std::uint32_t rttMs = 0, rttVarianceMs = 0, lossPartsPerMillion = 0;
    std::uint64_t generation = 0;
    bool open = false, connected = false, ready = false;
};

class Transport {
public:
    Transport();
    // As with other C++ objects, all worker calls must have stopped before
    // destruction. Explicit Close/Open/Poll remain network-owner operations.
    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    bool Open(std::uint16_t localBasePort);
    void Close();
    void SetPeer(std::uint64_t sessionId, std::uint32_t membershipNonce,
                 coop::Endpoint peer, bool host, std::uint32_t now);
    // False until the current reciprocal handshake is Ready. True means a
    // copy bound to this exact connection generation entered a bounded local
    // queue, never remote receipt or acknowledgement. Application ACK/commit
    // loops must keep retrying until their own actual receive/commit occurs.
    bool Queue(const void* data, std::size_t bytes, coop::Endpoint peer,
               Delivery delivery, std::uint64_t streamKey, std::uint32_t now);
    void Poll(std::uint32_t now);
    // Returns an actual received application message, 0 when empty, -1 when
    // the caller's buffer cannot hold the next message (which is discarded).
    int Receive(void* data, std::size_t capacity, coop::Endpoint& canonicalSender);
    bool Ready(std::uint64_t sessionId, std::uint32_t membershipNonce, coop::Endpoint peer) const;
    Statistics Snapshot() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace coop_transport

