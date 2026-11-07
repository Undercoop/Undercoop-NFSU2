// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include "CoopCheckpoint.h"
#include "CoopProfileContext.h"
#include "CoopCareerProgress.h"

#include <cstdlib>
#include <iostream>
#include <cstring>

namespace {
unsigned checks = 0;
void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::cerr << "FAILED: " << label << '\n';
        std::exit(1);
    }
}
bool EmptyAckData(const coop_checkpoint::Packet& p) {
    if (p.bytes) return false;
    for (uint8_t byte : p.data) if (byte) return false;
    return true;
}
void TypedPurposeTests() {
    using namespace coop_checkpoint;
    coop_career::Progress progress;
    progress.stageIndex = 3;
    progress.completedEvents = {0x11111111, 0x22222222};
    progress.stages[0].events[0] = 0x11111111;
    progress.stages[0].completed = true;
    progress.lastResult = {0x22222222, 411, 93200, 3, 2, 0};
    std::vector<uint8_t> blob;
    Check(coop_career::Encode(progress, blob), "compact shared-only snapshot encoding");
    Check(blob.size() < 4 * kBlock && blob.size() != coop_save::kNativeProfileFileSize,
        "shared transfer carries no native personal-profile clone");
    // Two independent channels exercise Client->Host steady update and
    // Host->Client bootstrap without conflating either receiver's state.
    Sender hostSender, clientSender;
    Receiver clientReceiver, hostReceiver;
    const uint64_t session = 0xABCD0123456789EFULL;
    auto run = [&](Sender& sender, Receiver& receiver, uint32_t transfer, uint32_t purpose) {
        Packet packet{}, ack{}, first{}, commit{};
        Completed completed;
        Check(sender.Start(session, transfer, 42, "CAMPAIGN", blob, 100, purpose), "typed transfer starts in either direction");
        Check(sender.Tick(100, first) && receiver.Receive(first, ack), "first typed block accepted");
        Check(Valid(ack) && EmptyAckData(ack), "normal ACK never echoes personal or shared payload bytes");
        Packet wrong = ack;
        wrong.launchKind = purpose == 3 ? 2 : 3;
        Check(Valid(wrong) && !sender.Ack(wrong, 100), "changed ACK purpose rejected despite matching digest/session");
        Check(sender.Tick(400, packet) && packet.index == 0, "wrong-purpose ACK cannot advance sender");
        wrong = packet; ++wrong.ticket;
        Check(!receiver.Receive(wrong, ack), "foreign ticket cannot append to active transfer");
        wrong = packet; ++wrong.session;
        Check(!receiver.Receive(wrong, ack), "foreign session cannot append to active transfer");
        wrong = packet; ++wrong.transfer;
        wrong.index = 1;
        Check(!receiver.Receive(wrong, ack), "new transfer may not start from a nonzero block");
        wrong = packet; ++wrong.transfer; wrong.launchKind = purpose == 3 ? 2 : 3;
        Check(Valid(wrong) && !receiver.Receive(wrong, ack),
            "newer transfer cannot change purpose while previous bootstrap is partial");
        Check(receiver.Receive(packet, ack), "correct repeated first block still accepted");
        sender.Ack(ack, 400);
        uint32_t now = 400;
        while (sender.Tick(now, packet) && packet.type == Type::Data) {
            wrong = packet; wrong.launchKind = purpose == 3 ? 2 : 3;
            Check(Valid(wrong) && !receiver.Receive(wrong, ack), "midstream data cannot switch payload purpose");
            Check(!receiver.Take(completed), "rejected purpose does not produce partial new-career state");
            Check(receiver.Receive(packet, ack) && Valid(ack) && EmptyAckData(ack), "correct typed data accepted after rejected substitute");
            wrong = ack; ++wrong.ticket; Check(!sender.Ack(wrong, now), "sender binds ticket on ACK");
            wrong = ack; ++wrong.session; Check(!sender.Ack(wrong, now), "sender binds session on ACK");
            sender.Ack(ack, now);
        }
        Check(packet.type == Type::Commit && packet.index == packet.count && Valid(packet), "typed terminal commit shape");
        commit = packet;
        wrong = packet; wrong.launchKind = purpose == 3 ? 2 : 3;
        Check(Valid(wrong) && !receiver.Receive(wrong, ack), "terminal commit cannot change bootstrap/runtime purpose");
        Check(!receiver.Receive(packet, ack), "typed commit still waits for runtime validation/persistence");
        Check(receiver.Take(completed) && completed.file == blob && completed.session == session &&
            completed.ticket == 42 && completed.transfer == transfer && completed.launchKind == purpose,
            "completed shared blob retains exact purpose/scope metadata");
        coop_career::Progress decoded;
        Check(coop_career::Decode(completed.file, decoded) && decoded.lastResult.eventHash == 0x22222222 &&
            decoded.lastResult.epoch == 411 && decoded.lastResult.rank == 2 && decoded.lastResult.status == 3 &&
            decoded.lastResult.ticks == 93200 && decoded.stageIndex == 3,
            "transport preserves shared progress and native-result receipt metadata");
        Check(receiver.Commit(true, ack) && Valid(ack) && EmptyAckData(ack), "final durable ACK is canonical and payload-free");
        wrong = ack; wrong.launchKind = purpose == 3 ? 2 : 3;
        Check(!sender.Ack(wrong, now) && !sender.Done(), "wrong-purpose final ACK cannot authorize a career launch");
        Check(sender.Ack(ack, now) && sender.Done(), "correct typed final ACK completes sender");
        Check(receiver.Receive(first, ack) && ack.index == 0 && Valid(ack) && EmptyAckData(ack),
            "committed duplicate DATA ACK strips all source payload data");
        Check(receiver.Receive(commit, ack) && ack.index == kComplete && Valid(ack) && EmptyAckData(ack),
            "committed duplicate COMMIT ACK strips payload data");
        wrong = first; wrong.launchKind = purpose == 3 ? 2 : 3;
        Check(!receiver.Receive(wrong, ack), "committed duplicate cannot change purpose");
        wrong = commit; wrong.launchKind = purpose == 3 ? 2 : 3;
        Check(!receiver.Receive(wrong, ack), "committed changed-purpose terminal packet rejected");
        Check(!receiver.Take(completed), "typed committed blob cannot be applied twice");
    };
    run(hostSender, clientReceiver, 20, 1);
    run(hostSender, clientReceiver, 23, 3); // Completed bootstrap may transition to steady progress.
    run(clientSender, hostReceiver, 21, 3);
    Sender newCareerSender; Receiver newCareerReceiver;
    run(newCareerSender, newCareerReceiver, 22, 2);
}
void PacketShapeTests() {
    using namespace coop_checkpoint;
    Sender sender; Packet data{}, ack{};
    Check(sender.Start(17, 5, 0, "SHAPE", std::vector<uint8_t>(kBlock + 3, 0x58), 0, 2) && sender.Tick(0, data),
        "packet-shape fixture starts");
    Check(Valid(data), "canonical DATA shape valid");
    auto invalid = [&](Packet p) { Check(!Valid(p), "malformed packet rejected before receiver allocation/state mutation"); };
    Packet bad = data; --bad.bytes; invalid(bad);
    bad = data; ++bad.bytes; invalid(bad);
    bad = data; bad.index = data.count; invalid(bad);
    bad = data; bad.index = kComplete; invalid(bad);
    bad = data; bad.count = 0; invalid(bad);
    bad = data; ++bad.count; invalid(bad);
    bad = data; bad.total = 0; invalid(bad);
    bad = data; bad.total = uint32_t(kMaxFile + 1); invalid(bad);
    bad = data; bad.version = 2; invalid(bad);
    bad = data; --bad.size; invalid(bad);
    bad = data; bad.type = static_cast<Type>(0); invalid(bad);
    bad = data; bad.session = 0; invalid(bad);
    bad = data; bad.transfer = 0; invalid(bad);
    bad = data; std::memset(bad.name, 'X', sizeof(bad.name)); invalid(bad);
    bad = data; std::memcpy(bad.name, "../SHAPE", sizeof("../SHAPE")); invalid(bad);
    data.index = 1; data.bytes = 3;
    Check(Valid(data), "last DATA exact remaining byte length accepted");
    bad = data; bad.bytes = kBlock; invalid(bad);
    ack = data; ack.type = Type::Ack; ack.bytes = 0; std::memset(ack.data, 0, sizeof(ack.data));
    Check(Valid(ack), "canonical ACK accepted");
    bad = ack; bad.bytes = 1; invalid(bad);
    bad = ack; bad.data[767] = 1; invalid(bad);
    bad = ack; bad.index = ack.count; invalid(bad);
    bad = ack; bad.index = 0xFFFD; invalid(bad);
    ack.index = kComplete; Check(Valid(ack), "existing complete ACK marker accepted");
    ack.index = 0xFFFE; Check(Valid(ack), "existing Fleet rejection ACK marker preserved without wire version bump");
    Check(!sender.Ack(ack, 0) && sender.Active() && !sender.Done(), "Fleet rejection marker cannot advance generic checkpoint sender");
    auto commit = ack; commit.type = Type::Commit; commit.index = commit.count;
    Check(Valid(commit), "canonical COMMIT accepted");
    bad = commit; bad.index = kComplete; invalid(bad);
    bad = commit; --bad.index; invalid(bad);
    bad = commit; bad.bytes = 1; invalid(bad);
    bad = commit; bad.data[0] = 1; invalid(bad);
}
} // namespace

int main() {
    using namespace coop_checkpoint;
    TypedPurposeTests();
    PacketShapeTests();
    std::vector<uint8_t> payload(coop_save::kNativeProfilePayloadSize);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 29 + 7);
    std::vector<uint8_t> source;
    Check(coop_save::EncodeNativeProfileFile(payload, source), "synthetic native checkpoint encoding");
    Sender sender;
    Receiver receiver;
    Check(!sender.Start(0, 1, 1, "NAME", source, 0), "checkpoint session is required");
    Check(!sender.Start(9, 1, 1, "../NAME", source, 0), "network filename traversal is rejected");
    Check(!sender.Start(9, 1, 1, "NAME", std::vector<uint8_t>(kMaxFile + 1), 0), "transfer memory is bounded");
    Check(sender.Start(9, 1, 42, "NAME", source, 1000), "start bounded native checkpoint transfer");

    bool droppedData = false, droppedAck = false, persisted = false, droppedCommitAck = false;
    int takenCount = 0;
    uint32_t now = 1000;
    for (; now < 60000 && sender.Active(); now += 25) {
        Packet packet, ack;
        if (!sender.Tick(now, packet)) continue;
        Check(Valid(packet), "outbound packet layout valid");
        if (packet.type == Type::Data && packet.index == 3 && !droppedData) {
            droppedData = true;
            continue;
        }
        if (packet.type == Type::Commit && !persisted) {
            Check(!receiver.Receive(packet, ack), "final ACK withheld before validation and atomic commit");
            Completed completed;
            Check(receiver.Take(completed), "completed checkpoint available exactly once");
            ++takenCount;
            Check(!receiver.Take(completed), "completed checkpoint cannot be applied twice");
            Check(completed.session == 9 && completed.transfer == 1 && completed.ticket == 42,
                "completed checkpoint preserves session and launch ticket");
            Check(completed.file == source, "received checkpoint matches every source byte");
            Check(coop_save::ValidateNativeProfileFile(completed.file.data(), completed.file.size()) ==
                coop_save::SaveValidation::Valid, "native file CRC validated before commit");
            coop_career::Progress rejectedLegacy;
            rejectedLegacy.stageIndex = 7;
            Check(!coop_career::Decode(completed.file, rejectedLegacy) && rejectedLegacy.stageIndex == 7,
                "generic transport may receive old MC02 but shared-only runtime policy rejects it");
            Check(receiver.Commit(true, ack), "caller validates and commits checkpoint");
            persisted = true;
            droppedCommitAck = true;
            continue; // Simulate losing the first acknowledgement of disk commit.
        }
        if (!receiver.Receive(packet, ack)) continue;
        if (packet.type == Type::Data) {
            Packet duplicateAck;
            Check(receiver.Receive(packet, duplicateAck), "duplicate data packet receives acknowledgement");
            Check(duplicateAck.index == ack.index, "duplicate data does not advance receive sequence");
            if (packet.index == 5 && !droppedAck) { droppedAck = true; continue; }
            Packet wrongAck = ack;
            wrongAck.session = 11;
            Check(!sender.Ack(wrongAck, now), "foreign session acknowledgement rejected");
            wrongAck = ack;
            wrongAck.transfer = 2;
            Check(!sender.Ack(wrongAck, now), "foreign transfer acknowledgement rejected");
        }
        sender.Ack(ack, now);
    }
    Check(droppedData && droppedAck && droppedCommitAck, "loss scenarios exercised");
    Check(sender.Done() && !sender.Active() && persisted && takenCount == 1,
        "loss and duplicate recovery finishes only after validated commit");

    Sender timedOut;
    Packet packet, ack;
    Check(timedOut.Start(9, 2, 0, "NAME", source, 0), "start disconnected transfer");
    Check(timedOut.Tick(0, packet), "first packet sent immediately");
    Check(!timedOut.Tick(60000, packet) && !timedOut.Active() && !timedOut.Done(),
        "disconnected transfer expires without claiming success");

    Receiver reordered;
    Sender ordering;
    Check(ordering.Start(9, 3, 0, "NAME", source, 0), "start order test");
    Check(ordering.Tick(0, packet), "order test first packet");
    Packet future = packet;
    future.index = 1;
    Check(!reordered.Receive(future, ack), "transfer cannot begin from future block");
    Check(reordered.Receive(packet, ack), "transfer begins from block zero");
    future.index = 2;
    Check(!reordered.Receive(future, ack), "out of order block cannot skip a missing block");
    Packet truncated = packet;
    --truncated.bytes;
    Check(!reordered.Receive(truncated, ack), "incorrect block length rejected");

    Receiver corrupt;
    Sender corruptSender;
    std::vector<uint8_t> small(40, 0x34);
    Check(corruptSender.Start(9, 4, 0, "NAME", small, 0), "start digest rejection test");
    Check(corruptSender.Tick(0, packet), "digest rejection packet generated");
    packet.data[0] ^= 1;
    Check(!corrupt.Receive(packet, ack), "transport digest rejects altered payload");
    Completed completed;
    Check(!corrupt.Take(completed), "corrupt transfer cannot reach filesystem commit");

    receiver.Reset();
    sender.Reset();
    Check(sender.Start(9, 5, 0, "NEXT", small, 0), "new checkpoint after explicit session transport reset");
    Check(sender.Tick(0, packet) && receiver.Receive(packet, ack), "reset receiver accepts fresh transfer");

    std::cout << "CoopCheckpointTests passed: " << checks << " checks (simulated packets and synthetic memory only).\n";
    return 0;
}
