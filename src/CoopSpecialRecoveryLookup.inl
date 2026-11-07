// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
// Include after ReceiptStore. Game-thread read-only coordinator lookup, using
// fixed numeric archive IDs and the SELECTED own personal filename. A network
// packet cannot choose a filename, role root, suffix, car or reward image.
namespace CoopSpecialNative { namespace RecoveryLookup {
enum class Status {Missing,Pending,Proven,Faulted};
struct Result {Status status=Status::Missing;coop_special_recovery::CoordinatorProof proof{};};
static Result Read(const coop_special_recovery::Request& query,const std::string& personal,const std::string& campaign,
    const ReceiptStore::Ports& io){
    using coop_special_receipt::Transaction;using ReceiptStore::Bytes;
    if(!coop_special_recovery::Valid(query)||!coop_special_receipt::SafeName(personal)||
        !coop_special_receipt::SafeName(campaign)||query.context.campaign!=coop_special_recovery_receipt::Campaign(campaign)||
        !query.context.catalog||!io.probe||!io.read)return {Status::Faulted,{}};
    auto fetch=[&](const ReceiptStore::FileId& id,Transaction& out,Bytes& bytes)->int{
        const int found=io.probe(personal,id);if(!found)return 0;
        if(found!=1||!io.read(personal,id,bytes)||bytes.empty()||bytes.size()>coop_special_receipt::kMaximumBytes||
            !coop_special_receipt::Decode(bytes,out)||out.scope.personal!=personal||out.scope.campaign!=campaign)return -1;
        return 1;
    };
    Transaction current,archived;Bytes currentBody,archiveBody;
    const int haveCurrent=fetch(ReceiptStore::CurrentId(),current,currentBody);
    if(haveCurrent<0)return {Status::Faulted,{}};
    const ReceiptStore::FileId archiveId{ReceiptStore::FileKind::Archive,query.old.session,query.old.ticket,query.old.operation};
    const int haveArchive=fetch(archiveId,archived,archiveBody);
    if(haveArchive<0)return {Status::Faulted,{}};
    if(haveArchive&&(!coop_special_receipt::Terminal(archived.phase)||!coop_special_recovery_receipt::SharedIdentity(archived.scope,query)))
        return {Status::Faulted,{}};
    const bool matchingCurrent=haveCurrent&&coop_special_recovery_receipt::SharedIdentity(current.scope,query);
    if(matchingCurrent&&haveArchive&&currentBody!=archiveBody)return {Status::Faulted,{}}; // Preserve contradictory copies; never guess.
    const auto* source=matchingCurrent?&current:haveArchive?&archived:nullptr;
    if(!source)return {Status::Missing,{}}; // Missing does NOT mean abort.
    coop_special_recovery::CoordinatorProof proof;
    if(!coop_special_recovery_receipt::CoordinatorProof(*source,query,personal,campaign,proof))return {Status::Pending,{}};
    return {Status::Proven,proof};
}
}} // namespace CoopSpecialNative::RecoveryLookup
