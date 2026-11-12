// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "CoopSession.h"

// This protocol exchanges file NAMES, lengths and SHA-256s, never file contents.
// It is a compatibility contract, not a sandbox or an anti-cheat system.
namespace coop_mods {
using Hash = std::array<uint8_t,32>;
constexpr size_t kMaxEntries=16384, kMaxPath=384, kMaxManifest=4*1024*1024;
constexpr uint32_t kMagic=0x4D32434E, kManifestMagic=0x4932434E; // NC2M / NC2I
constexpr size_t kBlock=1024, kWindow=32;
struct Entry { std::string path; uint64_t size=0; Hash hash{}; };
inline std::string Lower(std::string value) {
    for(char& c:value)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return value;
}
inline bool SafePath(const std::string& path) {
    if(path.empty()||path.size()>kMaxPath||path.front()=='/'||path.back()=='/')return false;
    size_t begin=0;
    for(size_t i=0;i<=path.size();++i) {
        if(i<path.size()) {
            const auto c=static_cast<unsigned char>(path[i]);
            if(c<32||c==127||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|'||
                (c>='A'&&c<='Z'))return false;
            if(c!='/')continue;
        }
        const auto part=path.substr(begin,i-begin);
        if(part.empty()||part=="."||part==".."||part.back()=='.'||part.back()==' ')return false;
        begin=i+1;
    }
    return true;
}
inline std::string Extension(const std::string& path) {
    const auto slash=path.find_last_of('/'),dot=path.find_last_of('.');
    return dot!=std::string::npos&&(slash==std::string::npos||dot>slash)?path.substr(dot):"";
}
inline bool IsPrivateDirectory(const std::string& name) {
#ifdef UNDERCOOP_MOD_MANAGER
    // Effective resources are exposed at game-relative names by the verified
    // overlay; inactive packages, import copies and UI state are not runtime.
    if(name=="mods")return true;
#endif
    static const char* excluded[]={".git",".vs","src","docs","scratch","research","backups",
        "sdata.backup","decompile_dump","logs","screenshots","cache",
        "saves","savegames","profiles","coop-saves","save_import","__pycache__"};
    for(const char* item:excluded)if(name==item)return true;return false;
}
inline bool IncludeFile(const std::string& path) {
    if(!SafePath(path))return false;
#ifdef UNDERCOOP_MOD_MANAGER
    // First-run loader metadata differs between a fresh setup and an existing
    // installation. Runtime game files and native modules are checked below.
    if(path=="global.ini")return false;
#endif
    const auto slash=path.find('/');const auto first=path.substr(0,slash);
    if(IsPrivateDirectory(first))return false;
    for(size_t start=0,end=path.find('/');end!=std::string::npos;start=end+1,end=path.find('/',start))
        if(IsPrivateDirectory(path.substr(start,end-start)))return false;
    const auto base=path.substr(path.find_last_of('/')==std::string::npos?0:path.find_last_of('/')+1);
    if(IsPrivateDirectory(base))return false;
    const auto ext=Extension(path);
    // PC MEMCARD contains native locale resources, not the AppData careers.
    // Keep possible card/profile payloads private; compare its .loc resources.
    if(first=="memcard"&&ext!=".loc")return false;
    if(ext==".log"||ext==".tmp"||ext==".bak"||ext==".bacc"||ext==".url"||ext==".pdb"||ext==".obj"||ext==".exp"||ext==".lib"||
        ext==".cpp"||ext==".h"||ext==".inl"||ext==".md"||ext==".ps1"||ext==".bat"||
        ext==".previous"||ext==".journal")return false;
    if(first=="scripts") {
        // Development helpers are not loaded by the game's ASI loader.
        if(ext==".py")return false;
        if(base=="imgui.ini"||base=="nfsu2cameratoolkit.ini")return false; // local windows, camera UI/hotkeys
        if(base=="nfsu2coop.ini"||base=="nfsu2coop-mods.txt")return false;
        if((base.rfind("player1car.",0)==0||base.rfind("player2car.",0)==0||
            base.rfind("player1param.",0)==0||base.rfind("player2param.",0)==0)&&ext==".bin")return false;
    }
    if(slash==std::string::npos) {
        if(base.rfind("unins",0)==0||base=="server.cfg"||base=="filelist.txt"||base=="cmakelists.txt"||
            base=="nfsu2_coop_handover_chatgpt.txt"||base[0]=='.'||ext==".py")return false;
        return true;
    }
    return true;
}
inline bool ValidEntries(const std::vector<Entry>& entries) {
    if(entries.empty()||entries.size()>kMaxEntries)return false;
    for(size_t i=0;i<entries.size();++i)
        if(!SafePath(entries[i].path)||(i&&entries[i-1].path>=entries[i].path))return false;
    return true;
}
inline void Put(std::vector<uint8_t>& out,uint64_t value,size_t bytes) {
    for(size_t i=0;i<bytes;++i)out.push_back(uint8_t(value>>(i*8)));
}
inline bool Get(const std::vector<uint8_t>& in,size_t& cursor,size_t bytes,uint64_t& value) {
    if(bytes>8||cursor>in.size()||bytes>in.size()-cursor)return false;
    value=0;for(size_t i=0;i<bytes;++i)value|=uint64_t(in[cursor++])<<(i*8);return true;
}
inline bool Encode(const std::vector<Entry>& entries,std::vector<uint8_t>& out) {
    out.clear();if(!ValidEntries(entries))return false;
    Put(out,kManifestMagic,4);Put(out,1,4);Put(out,entries.size(),4);
    for(const auto& entry:entries) {
        Put(out,entry.path.size(),2);Put(out,entry.size,8);
        out.insert(out.end(),entry.hash.begin(),entry.hash.end());
        out.insert(out.end(),entry.path.begin(),entry.path.end());
        if(out.size()>kMaxManifest){out.clear();return false;}
    }
    return true;
}
inline bool Decode(const std::vector<uint8_t>& in,std::vector<Entry>& out) {
    out.clear();if(in.size()<12||in.size()>kMaxManifest)return false;
    size_t cursor=0;uint64_t magic=0,version=0,count=0;
    if(!Get(in,cursor,4,magic)||!Get(in,cursor,4,version)||!Get(in,cursor,4,count)||
        magic!=kManifestMagic||version!=1||!count||count>kMaxEntries||count>(in.size()-12)/43)return false;
    std::vector<Entry> candidate;candidate.reserve(size_t(count));
    for(size_t i=0;i<count;++i) {
        uint64_t length=0,size=0;if(!Get(in,cursor,2,length)||!Get(in,cursor,8,size)||
            !length||length>kMaxPath||cursor>in.size()||32+length>in.size()-cursor)return false;
        Entry entry;entry.size=size;std::memcpy(entry.hash.data(),in.data()+cursor,32);cursor+=32;
        entry.path.assign(reinterpret_cast<const char*>(in.data()+cursor),size_t(length));cursor+=size_t(length);
        if(!SafePath(entry.path)||(!candidate.empty()&&candidate.back().path>=entry.path))return false;
        candidate.push_back(std::move(entry));
    }
    if(cursor!=in.size())return false;out=std::move(candidate);return true;
}
enum class Difference { MissingHere, ExtraHere, Changed };
struct Diff { Difference kind; std::string path; };
inline std::vector<Diff> Compare(const std::vector<Entry>& local,const std::vector<Entry>& peer) {
    std::vector<Diff> out;size_t a=0,b=0;
    while(a<local.size()||b<peer.size()) {
        if(b==peer.size()||(a<local.size()&&local[a].path<peer[b].path))out.push_back({Difference::ExtraHere,local[a++].path});
        else if(a==local.size()||peer[b].path<local[a].path)out.push_back({Difference::MissingHere,peer[b++].path});
        else {if(local[a].size!=peer[b].size||local[a].hash!=peer[b].hash)out.push_back({Difference::Changed,local[a].path});++a;++b;}
    }
    return out;
}
// Only explicit personal/display keys are omitted. Unknown settings are strict.
inline bool PersonalIniKey(const std::string& path,const std::string& section,const std::string& key) {
    if(path=="scripts/nfsu2extraoptionssettings.ini")
        return key=="allowmultipleinstances"||key=="headlightsmode";
    if(path=="scripts/nfsunderground2.widescreenfix.ini") {
        static const char* keys[]={"resx","resy","windowedmode","fovfactor","hudwidescreenmode",
            "femovieswidescreenmode","femoviefmvwidescreenmode","shadowsres","scaling","frontendfixedaspectratio",
            "fixhud","fixfov","fmvwidescreenmode","skipintro","fpslimit","hudscale","fescale","singlecoreaffinity"};
        for(const auto* item:keys)if(key==item)return true;
    }
    (void)section;return false;
}
inline std::string Trim(std::string text) {
    const auto a=text.find_first_not_of(" \t\r\n");if(a==std::string::npos)return {};
    return text.substr(a,text.find_last_not_of(" \t\r\n")-a+1);
}
inline bool CanonicalIni(const std::string& path,const std::string& text,std::string& out) {
    out.clear();std::string section;std::vector<std::pair<std::string,std::string>> values;
    for(size_t at=0;at<text.size();) {
        const auto end=text.find('\n',at);auto line=Trim(text.substr(at,end==std::string::npos?end:end-at));
        at=end==std::string::npos?text.size():end+1;
        if(line.rfind("\xEF\xBB\xBF",0)==0)line.erase(0,3);
        bool quoted=false;
        for(size_t i=0;i<line.size();++i) {
            if(line[i]=='"')quoted=!quoted;
            if(!quoted&&(line[i]==';'||(line[i]=='/'&&i+1<line.size()&&line[i+1]=='/'))&&
                (!i||line[i-1]==' '||line[i-1]=='\t'||line[0]=='[')) {line=Trim(line.substr(0,i));break;}
        }
        if(quoted)return false;
        if(line.empty()||line[0]==';'||line[0]=='#'||line.rfind("//",0)==0)continue;
        if(line[0]=='[') {if(line.back()!=']')return false;section=Lower(Trim(line.substr(1,line.size()-2)));continue;}
        const auto equal=line.find('=');if(equal==std::string::npos)return false;
        auto key=Lower(Trim(line.substr(0,equal))),value=Trim(line.substr(equal+1));if(key.empty())return false;
        if(PersonalIniKey(path,section,key))continue;
        auto name=section+"/"+key;
        if(values.size()>=4096)return false;
        for(const auto& previous:values)if(previous.first==name)return false; // ambiguous duplicate keys: hash raw instead
        values.push_back({std::move(name),std::move(value)});
    }
    std::sort(values.begin(),values.end());
    for(const auto& item:values)out+=item.first+"="+item.second+"\n";
    return true;
}
enum class Type:uint16_t { Hello=1, Block=2, Ack=3, Failure=4 };
#pragma pack(push,1)
struct Packet {
    uint32_t magic=kMagic;uint16_t version=1,size=sizeof(Packet);
    uint64_t session=0;uint32_t token=0,echo=0;
    Type type=Type::Hello;uint16_t index=0,bytes=0,reserved=0;
    uint16_t runtimeReady=1;
    uint32_t total=0;Hash hash{};std::array<uint8_t,kBlock> data{};
};
#pragma pack(pop)
static_assert(sizeof(Packet)<1200,"Mod datagrams must fit a normal VPN MTU");
inline bool Valid(const Packet& p) {
    if(p.magic!=kMagic||p.version!=1||p.size!=sizeof(Packet)||!p.session||!p.token||p.reserved||p.runtimeReady>1||
        p.type<Type::Hello||p.type>Type::Failure)return false;
    if(p.type==Type::Failure) {
        if(p.total||p.bytes||!p.index||p.index>4||p.runtimeReady)return false;
        for(auto byte:p.hash)if(byte)return false;for(auto byte:p.data)if(byte)return false;return true;
    }
    if(p.total<12||p.total>kMaxManifest)return false;
    const size_t count=(p.total+kBlock-1)/kBlock;
    if(p.type==Type::Hello) {if(p.index||p.bytes)return false;}
    else if(p.index>=count||!p.echo)return false;
    if(p.type==Type::Block) {
        if(p.bytes!=std::min(kBlock,size_t(p.total)-size_t(p.index)*kBlock))return false;
    }else if(p.bytes)return false;
    for(size_t i=p.bytes;i<kBlock;++i)if(p.data[i])return false;
    return true;
}
// Selective repeat: identical packs exchange only Hello; full inventories are
// transferred only on mismatch. All access is serialized by the native adapter.
class Exchange {
public:
    void Bind(uint64_t session,uint32_t token,const std::vector<uint8_t>& blob,const Hash& hash) {
        *this=Exchange{};session_=session;token_=token;local_=blob;localHash_=hash;
        sent_.assign((blob.size()+kBlock-1)/kBlock,0);acked_.assign(sent_.size(),false);
    }
    bool Bound()const{return session_&&token_&&!local_.empty();}
    void SetRuntimeReady(bool ready){localRuntimeReady_=ready;}
    bool Ready()const{return Bound()&&localRuntimeReady_&&peerRuntimeReady_&&peerToken_&&peerEcho_==token_&&peerHash_==localHash_&&peerTotal_==local_.size();}
    bool Mismatch()const{return Bound()&&peerToken_&&(peerHash_!=localHash_||peerTotal_!=local_.size());}
    bool PeerKnown()const{return peerToken_!=0;}
    uint32_t LocalToken()const{return token_;}
    uint32_t PeerToken()const{return peerToken_;}
    uint32_t PeerTotal()const{return peerTotal_;}
    const Hash& PeerHash()const{return peerHash_;}
    bool RuntimeReady()const{return Bound()&&localRuntimeReady_&&peerRuntimeReady_&&peerToken_&&peerEcho_==token_;}
    size_t Received()const{return receivedCount_;}
    bool PopRemote(std::vector<uint8_t>& blob,Hash& hash) {
        if(taken_||remote_.empty()||receivedCount_!=received_.size())return false;
        taken_=true;blob=remote_;hash=peerHash_;return true;
    }
    bool Receive(const Packet& p,uint32_t now,Packet& ack) {
        (void)now;if(!Bound()||!Valid(p)||p.session!=session_)return false;
        if(p.type==Type::Failure)return false;
        if(p.type==Type::Hello) {
            if(peerToken_!=p.token||peerHash_!=p.hash||peerTotal_!=p.total) {
                peerToken_=p.token;peerHash_=p.hash;peerTotal_=p.total;peerEcho_=0;
                remote_.clear();received_.clear();receivedCount_=0;taken_=false;
                std::fill(acked_.begin(),acked_.end(),false);std::fill(sent_.begin(),sent_.end(),0);
            }
            peerEcho_=p.echo;peerRuntimeReady_=p.runtimeReady!=0;return false;
        }
        if(p.token!=peerToken_||p.echo!=token_)return false;
        if(p.type==Type::Ack) {
            if(p.hash==localHash_&&p.total==local_.size()&&p.index<acked_.size()&&sent_[p.index])acked_[p.index]=true;
            return false;
        }
        if(!Mismatch()||p.hash!=peerHash_||p.total!=peerTotal_)return false;
        if(remote_.empty()){remote_.resize(peerTotal_);received_.assign((peerTotal_+kBlock-1)/kBlock,false);}
        if(!received_[p.index]) {
            std::memcpy(remote_.data()+size_t(p.index)*kBlock,p.data.data(),p.bytes);
            received_[p.index]=true;++receivedCount_;
        }
        ack=Base(Type::Ack);ack.hash=peerHash_;ack.total=peerTotal_;ack.index=p.index;return true;
    }
    size_t Tick(uint32_t now,std::array<Packet,4>& out) {
        if(!Bound())return 0;size_t count=0;
        if(!helloSent_||now-lastHello_>=500) {out[count++]=Base(Type::Hello);lastHello_=now;helloSent_=true;}
        if(!Mismatch()||peerEcho_!=token_)return count;
        size_t first=0;while(first<acked_.size()&&acked_[first])++first;
        for(size_t i=first;i<std::min(first+kWindow,acked_.size())&&count<out.size();++i) {
            if(acked_[i]||(sent_[i]&&now-sent_[i]<500))continue;
            auto p=Base(Type::Block);p.index=uint16_t(i);
            p.bytes=uint16_t(std::min(kBlock,local_.size()-i*kBlock));
            std::memcpy(p.data.data(),local_.data()+i*kBlock,p.bytes);out[count++]=p;sent_[i]=now?now:1;
        }
        return count;
    }
private:
    Packet Base(Type type)const {
        Packet p;p.session=session_;p.token=token_;p.echo=peerToken_;p.type=type;
        p.runtimeReady=localRuntimeReady_?1:0;
        p.total=uint32_t(local_.size());p.hash=localHash_;return p;
    }
    uint64_t session_=0;uint32_t token_=0,peerToken_=0,peerEcho_=0,peerTotal_=0,lastHello_=0;
    Hash localHash_{},peerHash_{};std::vector<uint8_t> local_,remote_;
    std::vector<uint32_t> sent_;std::vector<bool> acked_,received_;
    size_t receivedCount_=0;bool taken_=false,helloSent_=false,localRuntimeReady_=true,peerRuntimeReady_=false;
};
}
