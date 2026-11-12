// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/CoopBuildVersion.h"
#include "../src/CoopModScanWin.h"
static coop::SessionSnapshot session;
static bool connected=true,expanded=true;
static std::atomic<bool> gCoopCareerHooksReady{true},gCoopTransportOpened{true};
static bool gCoopAIEnabled=true,gCoopFleetEnabled=true;
namespace coop_build {constexpr char kSaveDomain[]="consent-fixture";}
namespace coop_car_catalog {constexpr unsigned kStockCount=32;}
namespace CoopCarCatalogNative {
static bool InitializeFromGameDirectory(const char*){return false;}
static const char* Failure(){return "test only";}
static unsigned Count(){return 32;}static unsigned Footprint(){return 1;}
static bool PrepareRuntimeCatalog(){return true;}
}
namespace CoopAnnouncements {static bool Expanded(){return expanded;}static void Post(const char*){}}
static bool ProbeCoopVehicleAssetAdaptersReady(){return true;}
static bool InstallCoopStockCatalogReaderHook(){return true;}
static void TickCoopStockCatalog(DWORD){}static void TraceCoopAddonMenus(DWORD){}
static std::string NarrowText(const std::wstring& text){return coop_mod_scan::Utf8(text);}
static void LogMessage(const char*){}
static coop::SessionSnapshot CoopSnapshot(){return session;}
static bool CoopConnectedTransportReady(const coop::SessionSnapshot&){return connected;}
static bool IsJoinedCoopSender(const sockaddr_in& from){return session.joined&&from.sin_addr.s_addr==session.peer.address&&ntohs(from.sin_port)==session.peer.port;}
static void SendSessionBytes(const void*,size_t,coop::Endpoint){}
#include "../src/CoopModSupportNative.inl"
static unsigned checks=0;
static void Check(bool good,const char* text,int line){++checks;if(!good){std::printf("FAIL %d %s\n",line,text);std::exit(1);}}
#define CHECK(x) Check(bool(x),#x,__LINE__)
struct Fixture {
    coop_mod_consent::Consent other;coop::Endpoint own{0x0100007F,48940};sockaddr_in from{};DWORD now=1;
    Fixture(){
        session={};session.role=coop::Role::Host;session.phase=coop::Phase::Lobby;session.joined=session.fresh=true;
        session.sessionId=121;session.membershipNonce=19;session.peer={0x0100007F,48941};
        from.sin_family=AF_INET;from.sin_addr.s_addr=session.peer.address;from.sin_port=htons(session.peer.port);
        connected=expanded=true;gCoopCareerHooksReady=true;
        auto& m=CoopModSupport::inventory;m={};m.blob.assign(100,1);m.hash.fill(1);
        CoopModSupport::ResetPeer();CoopModSupport::scanDone=true;CoopModSupport::invalidated=false;
        CoopModSupport::catalogReady=true;CoopModSupport::vehicleAssetsReady=true;CoopModSupport::status=3;
        CoopModSupport::boundSession=session.sessionId;CoopModSupport::boundMembership=session.membershipNonce;CoopModSupport::boundPeer=session.peer;
        CoopModSupport::exchange.Bind(session.sessionId,13,m.blob,m.hash);
        coop_mods::Packet hello;hello.session=session.sessionId;hello.token=29;hello.echo=13;hello.total=200;hello.hash.fill(2);
        coop_mods::Packet ack;CoopModSupport::exchange.Receive(hello,now,ack);
        CoopModSupport::UpdateConsent(session);
        CoopModSupport::remoteVerified=true;CoopModSupport::verifiedBinding=CoopModSupport::CurrentBinding(session);
        CoopModSupport::UpdateConsent(session);
        RebindOther();Pump();
    }
    void RebindOther(){auto binding=CoopModSupport::consent.Identity();std::swap(binding.localToken,binding.peerToken);std::swap(binding.localBytes,binding.peerBytes);
        std::swap(binding.localHash,binding.peerHash);binding.peer=own;other.Bind(binding);other.SetEligible(true);}
    void Pump(){
        for(unsigned i=0;i<8;++i){coop_mod_consent::Packet a,b;
            const bool sa=CoopModSupport::consent.Tick(now,a),sb=other.Tick(now,b);now+=251;
            if(sa)other.Receive(a,own);
            if(sb)ReceiveCoopModPacket(reinterpret_cast<const char*>(&b),sizeof(b),from,now);
        }
    }
    void Accept(){CHECK(HandleCoopModDecisionKey(VK_F5));CHECK(other.Decide(true));Pump();CHECK(CoopModsReady(session));}
};
static void VerifiedInventory(bool corrupt){
    Fixture f;coop_mods::Entry entry;entry.path="global/globalb.lzc";entry.size=32;entry.hash.fill(3);
    CoopModSupport::inventory.entries={entry};CHECK(coop_mods::Encode({entry},CoopModSupport::inventory.blob));
    coop_mod_scan::Sha256 sha;auto& inv=CoopModSupport::inventory;
    CHECK(sha.Bytes(inv.blob.data(),inv.blob.size(),inv.hash));
    entry.hash.fill(9);std::vector<uint8_t> peer;CHECK(coop_mods::Encode({entry},peer));coop_mods::Hash peerHash{};
    CHECK(sha.Bytes(peer.data(),peer.size(),peerHash));
    CoopModSupport::exchange.Bind(session.sessionId,13,inv.blob,inv.hash);
    coop_mods::Packet hello,ack;hello.session=session.sessionId;hello.token=29;hello.echo=13;
    hello.total=uint32_t(peer.size());hello.hash=peerHash;CoopModSupport::exchange.Receive(hello,f.now,ack);
    CoopModSupport::UpdateConsent(session);CHECK(!CoopModSupport::remoteVerified);CHECK(!HandleCoopModDecisionKey(VK_F5));
    coop_mods::Packet block=hello;block.type=coop_mods::Type::Block;block.bytes=uint16_t(peer.size());
    std::memcpy(block.data.data(),peer.data(),peer.size());if(corrupt)block.data[12]^=1;
    CHECK(ReceiveCoopModPacket(reinterpret_cast<const char*>(&block),sizeof(block),f.from,f.now));
    CHECK(CoopModSupport::root.empty());TickCoopModNetwork(f.now+300);
    if(corrupt){CHECK(CoopModSupport::remoteRejected);CHECK(!CoopModSupport::remoteVerified);
        CHECK(!HandleCoopModDecisionKey(VK_F5));CHECK(!CoopModsReady(session));}
    else{CHECK(CoopModSupport::remoteVerified);CHECK(!CoopModSupport::remoteRejected);f.RebindOther();f.Pump();f.Accept();}
}
int main(){
    {sockaddr_in sender{};CHECK(!ReceiveCoopModPacket(nullptr,128,sender,0));const char shortData[3]{};
        CHECK(!ReceiveCoopModPacket(shortData,3,sender,0));}
    {Fixture f;CHECK(!CoopModsReady(session));expanded=false;CHECK(!HandleCoopModDecisionKey(VK_F5));CHECK(!CoopModSupport::consent.LocalAccepted());expanded=true;
        CHECK(!HandleCoopModDecisionKey(VK_RETURN));f.Accept();CHECK(std::string(CoopModStatus()).find("MISMATCH ACCEPTED")!=std::string::npos);}
    {Fixture f;f.Accept();CoopModSupport::scanDone=false;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();CoopModSupport::invalidated=true;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();CoopModSupport::catalogReady=false;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();CoopModSupport::vehicleAssetsReady=false;CHECK(!CoopModsReady(session));}
    {Fixture f;f.Accept();gCoopCareerHooksReady=false;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();CoopModSupport::peerFailed=true;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();CoopModSupport::remoteRejected=true;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();CoopModSupport::remoteVerified=false;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();CoopModSupport::inventory.error="failed scan";CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();CoopModSupport::status=4;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();CoopModSupport::status=5;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();connected=false;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();session.fresh=false;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();session.membershipNonce++;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;f.Accept();session.peer.port++;CHECK(!CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));}
    {Fixture f;CHECK(HandleCoopModDecisionKey(VK_F6));f.Pump();CHECK(!CoopModsReady(session));CHECK(CoopModSupport::consent.Declined());CHECK(f.other.Declined());}
    {Fixture f;CoopModSupport::remoteVerified=false;CHECK(!HandleCoopModDecisionKey(VK_F5));CHECK(!CoopModsReady(session));}
    {Fixture f;session.phase=coop::Phase::Playing;CHECK(!HandleCoopModDecisionKey(VK_F5));CHECK(!CoopModSupport::consent.LocalAccepted());}
    {Fixture f;f.Accept();ResetCoopModPeer();CHECK(!CoopModsReady(session));CHECK(!CoopModSupport::consent.Bound());}
    {Fixture f;coop_mods::Packet hello,ack;hello.session=session.sessionId;hello.token=29;hello.echo=13;
        hello.total=uint32_t(CoopModSupport::inventory.blob.size());hello.hash=CoopModSupport::inventory.hash;
        CoopModSupport::exchange.Receive(hello,f.now,ack);TickCoopModNetwork(f.now+300);
        CHECK(CoopModsReady(session));CHECK(!HandleCoopModDecisionKey(VK_F5));CHECK(std::string(CoopModStatus())=="MOD PACK MATCHED");}
    VerifiedInventory(false);VerifiedInventory(true);
    std::printf("Native mismatch gates: %u checks, 0 failures; no game, network or file writes.\n",checks);return 0;
}
