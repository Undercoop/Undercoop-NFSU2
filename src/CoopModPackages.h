// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Declarative package metadata and planning only: no filesystem, native-code
// loading, scripts, network, save access, or writes. The host resolves a source
// as mods/<owner>/<source> and generates its overlay under mods/. A destination
// is a virtual game-relative asset name, never permission to overwrite stock.
//
// [Package]                 Required, keys case-insensitive and unique.
// Id=bmw_m3_gtr              Canonical lowercase ASCII identifier.
// Name=BMW M3 GTR            Printable UTF-8 display text.
// Version=1.0               Opaque, exact ASCII version (no ranges).
// Kind=assets               assets | dependency | data
// [Files]                   Required for assets; absent for dependency/data.
// CARS/BMWM3GTR/GEOMETRY.BIN=assets/GEOMETRY.BIN
// [Requires]                Optional. Dependencies may be declared in any order.
// nfsu2-unlimiter=3.1.1.1337
//
// ';' and '#' introduce whole-line comments only. No interpolation, includes,
// quoting, command fields, or inline comments. Path separators and ASCII case
// normalize to '/' and lowercase; every destination is unique after folding.
// dependency packages carry no Files; the host must independently approve any
// native dependency. Files in either kind cannot designate executable content.
namespace coop_packages {

struct Limits {
    size_t manifestBytes=256*1024, lineBytes=1024, pathBytes=240;
    size_t idBytes=64, nameBytes=256, versionBytes=64;
    size_t filesPerPackage=4096, requiresPerPackage=128;
    size_t packages=256, selected=256, catalogFiles=65536, planFiles=16384;
};
enum class ErrorCode {
    None, Budget, Syntax, Encoding, DuplicateSection, DuplicateKey,
    UnknownSection, UnknownKey, MissingField, InvalidId, InvalidName,
    InvalidVersion, InvalidKind, UnsafePath, UnsafeFile, ReservedTarget,
    DuplicateTarget, InvalidPackage, DuplicateId, MissingPackage,
    VersionMismatch, Cycle, TargetConflict
};
struct Error {
    ErrorCode code=ErrorCode::None;
    size_t line=0;                 // 1-based manifest line, 0 for planner errors.
    std::string package, detail;
};
enum class Kind { Assets, Dependency, Data };
struct File { std::string destination, source; };
struct Requirement { std::string id, version; };
struct Manifest {
    std::string id, name, version;
    Kind kind=Kind::Assets;
    std::vector<File> files;
    std::vector<Requirement> requires;
};
struct IndexEntry { std::string owner, source, destination; };
class Plan;
inline bool BuildPlan(const std::vector<Manifest>& catalog,
    const std::vector<std::string>& selected, Plan& out, Error& error,
    const Limits& limits=Limits{});
class Plan {
public:
    // Generated once by BuildPlan. Consumers cannot mutate the index in place.
    const std::vector<std::string>& Packages() const { return packages_; }
    const std::vector<IndexEntry>& Index() const { return index_; }
private:
    std::vector<std::string> packages_;
    std::vector<IndexEntry> index_;
    friend bool BuildPlan(const std::vector<Manifest>&,
        const std::vector<std::string>&, Plan&, Error&, const Limits&);
};

namespace detail {
inline bool Fail(Error& error, ErrorCode code, const std::string& message,
    size_t line=0, const std::string& package={}) {
    error={code,line,package,message}; return false;
}
inline std::string Lower(std::string value) {
    for(char& c:value) if(c>='A'&&c<='Z') c=char(c-'A'+'a');
    return value;
}
inline std::string Trim(const std::string& value) {
    const auto first=value.find_first_not_of(" \t\r");
    if(first==std::string::npos) return {};
    return value.substr(first,value.find_last_not_of(" \t\r")-first+1);
}
inline bool Device(const std::string& component) {
    const auto name=component.substr(0,component.find('.'));
    if(name=="con"||name=="prn"||name=="aux"||name=="nul"||
        name=="clock$"||name=="conin$"||name=="conout$") return true;
    return name.size()==4&&(name.compare(0,3,"com")==0||name.compare(0,3,"lpt")==0)&&
        name[3]>='1'&&name[3]<='9';
}
inline bool TokenChar(char c) {
    return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';
}
inline bool Id(const std::string& value, const Limits& limits) {
    if(value.empty()||value.size()>limits.idBytes||
        !((value[0]>='a'&&value[0]<='z')||(value[0]>='0'&&value[0]<='9'))||
        value.back()=='.'||Device(value)) return false;
    for(char c:value) if(!TokenChar(c)) return false;
    return value!="mods"&&value!="generated"&&value!="cache"&&value!="state";
}
inline bool Version(const std::string& value, const Limits& limits) {
    if(value.empty()||value.size()>limits.versionBytes) return false;
    for(char c:value) if(!TokenChar(c)&&!(c>='A'&&c<='Z')&&c!='+') return false;
    return true;
}
inline bool Utf8(const std::string& value, bool display=false) {
    for(size_t at=0;at<value.size();) {
        const unsigned char lead=static_cast<unsigned char>(value[at++]);
        unsigned long point=lead; size_t count=0;
        if(lead>=0xc2&&lead<=0xdf) { point=lead&31; count=1; }
        else if(lead>=0xe0&&lead<=0xef) { point=lead&15; count=2; }
        else if(lead>=0xf0&&lead<=0xf4) { point=lead&7; count=3; }
        else if(lead>=0x80) return false;
        if(count>value.size()-at) return false;
        for(size_t i=0;i<count;++i) {
            const unsigned char next=static_cast<unsigned char>(value[at++]);
            if((next&0xc0)!=0x80) return false;
            point=(point<<6)|(next&63);
        }
        if((count==1&&point<0x80)||(count==2&&point<0x800)||
            (count==3&&point<0x10000)||point>0x10ffff||
            (point>=0xd800&&point<=0xdfff)) return false;
        if(display&&(point<32||(point>=127&&point<=159)||
            (point>=0x202a&&point<=0x202e)||(point>=0x2066&&point<=0x2069))) return false;
    }
    return true;
}
inline std::string Extension(const std::string& path) {
    const auto slash=path.find_last_of('/'), dot=path.find_last_of('.');
    return dot!=std::string::npos&&(slash==std::string::npos||dot>slash)?path.substr(dot):"";
}
inline bool Asset(const std::string& path) {
    const auto ext=Extension(path);
    // A conservative data allowlist also excludes unknown executable/script
    // suffixes and extensionless commands. Content checks remain the host's job.
    static const char* allowed[]={".bin",".tpk",".dds",".ini",".txt",".loc",
        ".bmp",".png",".jpg",".jpeg",".fng",".dat",".viv",".geo",".wav",
        ".mp3",".ogg",".big",".lzc",".bng",".xml",".json",".cfg",".msh",
        ".vp6",".fnt",".ttf",".otf",".ico",".pal",".bik",".abk"};
    for(const char* item:allowed) if(ext==item) return true;
    return false;
}
inline bool ReservedDestination(const std::string& path) {
    const auto first=path.substr(0,path.find('/'));
    static const char* reserved[]={"mods","saves","profiles","coop-saves","src",
        "docs","scratch","research","backups","logs","screenshots","cache"};
    for(const char* item:reserved) if(first==item) return true;
    return path=="scripts/nfsu2coop.ini"||path=="scripts/nfsu2coop-mods.txt";
}
inline bool Prefix(const std::string& file, const std::string& other) {
    return other.size()>file.size()&&other.compare(0,file.size(),file)==0&&other[file.size()]=='/';
}
} // detail

// Canonicalization is lexical only. The host must additionally reject actual
// filesystem junctions/symlinks and verify final containment before reading.
inline bool CanonicalPath(const std::string& input, std::string& out,
    const Limits& limits=Limits{}) {
    out.clear();
    if(input.empty()||input.size()>limits.pathBytes) return false;
    std::string candidate=detail::Lower(input);
    for(char& c:candidate) { if(c=='\\') c='/'; if(c!='/'&&!detail::TokenChar(c)) return false; }
    if(candidate.front()=='/'||candidate.back()=='/') return false;
    size_t begin=0;
    for(size_t at=0;at<=candidate.size();++at) {
        if(at<candidate.size()&&candidate[at]!='/') continue;
        const auto component=candidate.substr(begin,at-begin);
        if(component.empty()||component=="."||component==".."||component.front()=='.'||
            component.back()=='.'||detail::Device(component)) return false;
        begin=at+1;
    }
    out=std::move(candidate); return true;
}

inline bool ValidateManifest(const Manifest& manifest, Error& error,
    const Limits& limits=Limits{}) {
    error={};
    const auto bad=[&](ErrorCode code,const std::string& why) {
        return detail::Fail(error,code,why,0,manifest.id);
    };
    if(!detail::Id(manifest.id,limits)) return bad(ErrorCode::InvalidId,"Invalid canonical package Id");
    if(manifest.name.empty()||manifest.name.size()>limits.nameBytes||
        !detail::Utf8(manifest.name,true)) return bad(ErrorCode::InvalidName,"Name must be printable UTF-8");
    if(!detail::Version(manifest.version,limits)) return bad(ErrorCode::InvalidVersion,"Invalid exact Version");
    if(manifest.kind!=Kind::Assets&&manifest.kind!=Kind::Dependency&&manifest.kind!=Kind::Data) return bad(ErrorCode::InvalidKind,"Unknown Kind");
    if(manifest.files.size()>limits.filesPerPackage||manifest.requires.size()>limits.requiresPerPackage)
        return bad(ErrorCode::Budget,"Package entry budget exceeded");
    if((manifest.kind==Kind::Assets&&manifest.files.empty())||
        (manifest.kind!=Kind::Assets&&!manifest.files.empty()))
        return bad(ErrorCode::InvalidPackage,"assets needs Files; dependency/data must have no Files");
    std::set<std::string> destinations, requirements;
    for(const auto& file:manifest.files) {
        std::string destination,source;
        if(!CanonicalPath(file.destination,destination,limits)||destination!=file.destination||
            !CanonicalPath(file.source,source,limits)||source!=file.source)
            return bad(ErrorCode::UnsafePath,"Unsafe or noncanonical file path");
        if(!detail::Asset(destination)||!detail::Asset(source))
            return bad(ErrorCode::UnsafeFile,"Files accepts data assets only: "+destination);
        if(detail::ReservedDestination(destination)) return bad(ErrorCode::ReservedTarget,"Reserved target: "+destination);
        if(!destinations.insert(destination).second) return bad(ErrorCode::DuplicateTarget,"Duplicate target: "+destination);
    }
    for(const auto& destination:destinations)
        for(size_t slash=destination.find('/');slash!=std::string::npos;slash=destination.find('/',slash+1))
            if(destinations.count(destination.substr(0,slash)))
                return bad(ErrorCode::TargetConflict,"File/directory target conflict: "+destination);
    for(const auto& requirement:manifest.requires) {
        if(!detail::Id(requirement.id,limits)) return bad(ErrorCode::InvalidId,"Invalid required Id: "+requirement.id);
        if(!detail::Version(requirement.version,limits)) return bad(ErrorCode::InvalidVersion,"Invalid required Version: "+requirement.id);
        if(!requirements.insert(requirement.id).second) return bad(ErrorCode::DuplicateKey,"Duplicate requirement: "+requirement.id);
    }
    return true;
}

inline bool ParseManifest(const std::string& text, Manifest& out, Error& error,
    const Limits& limits=Limits{}) {
    out={}; error={};
    if(text.size()>limits.manifestBytes) return detail::Fail(error,ErrorCode::Budget,"Manifest byte budget exceeded");
    if(!detail::Utf8(text)) return detail::Fail(error,ErrorCode::Encoding,"Manifest must be valid UTF-8");
    for(size_t at=0;at<text.size();++at) {
        const unsigned char c=static_cast<unsigned char>(text[at]);
        if((c<32&&c!='\t'&&c!='\n'&&c!='\r')||c==127||
            (c=='\r'&&(at+1==text.size()||text[at+1]!='\n')))
            return detail::Fail(error,ErrorCode::Encoding,"Invalid control character");
    }
    Manifest candidate; std::string section;
    std::set<std::string> sections,keys;
    size_t lineNumber=0;
    for(size_t at=0;at<text.size();) {
        ++lineNumber;
        const auto end=text.find('\n',at), count=(end==std::string::npos?text.size():end)-at;
        if(count>limits.lineBytes) return detail::Fail(error,ErrorCode::Budget,"Line byte budget exceeded",lineNumber);
        auto line=detail::Trim(text.substr(at,count)); at=end==std::string::npos?text.size():end+1;
        if(lineNumber==1&&line.compare(0,3,"\xef\xbb\xbf")==0) line=detail::Trim(line.substr(3));
        if(line.empty()||line.front()==';'||line.front()=='#') continue;
        if(line.front()=='[') {
            if(line.size()<3||line.back()!=']') return detail::Fail(error,ErrorCode::Syntax,"Malformed section",lineNumber);
            section=detail::Lower(detail::Trim(line.substr(1,line.size()-2)));
            if(section!="package"&&section!="files"&&section!="requires")
                return detail::Fail(error,ErrorCode::UnknownSection,"Unknown section: "+section,lineNumber);
            if(!sections.insert(section).second) return detail::Fail(error,ErrorCode::DuplicateSection,"Repeated section: "+section,lineNumber);
            continue;
        }
        const auto equal=line.find('=');
        if(section.empty()||equal==std::string::npos)
            return detail::Fail(error,ErrorCode::Syntax,"Expected key=value inside a section",lineNumber);
        const auto rawKey=detail::Trim(line.substr(0,equal));
        const auto value=detail::Trim(line.substr(equal+1));
        auto key=detail::Lower(rawKey);
        if(key.empty()||value.empty()) return detail::Fail(error,ErrorCode::Syntax,"Empty key or value",lineNumber);
        if(section=="files") {
            std::string destination,source;
            if(!CanonicalPath(rawKey,destination,limits)||!CanonicalPath(value,source,limits))
                return detail::Fail(error,ErrorCode::UnsafePath,"Unsafe source or destination path",lineNumber);
            key=destination;
            if(candidate.files.size()>=limits.filesPerPackage)
                return detail::Fail(error,ErrorCode::Budget,"Files budget exceeded",lineNumber);
            candidate.files.push_back({destination,source});
        } else if(section=="requires") {
            if(!detail::Id(rawKey,limits)) return detail::Fail(error,ErrorCode::InvalidId,"Required Id must be canonical ASCII",lineNumber);
            if(candidate.requires.size()>=limits.requiresPerPackage)
                return detail::Fail(error,ErrorCode::Budget,"Requires budget exceeded",lineNumber);
            candidate.requires.push_back({rawKey,value});
        } else if(key=="id") candidate.id=value;
        else if(key=="name") candidate.name=value;
        else if(key=="version") candidate.version=value;
        else if(key=="kind") {
            if(value=="assets") candidate.kind=Kind::Assets;
            else if(value=="dependency") candidate.kind=Kind::Dependency;
            else if(value=="data") candidate.kind=Kind::Data;
            else return detail::Fail(error,ErrorCode::InvalidKind,"Kind must be assets, dependency or data",lineNumber);
        } else return detail::Fail(error,ErrorCode::UnknownKey,"Unknown Package key: "+key,lineNumber);
        if(!keys.insert(section+"/"+key).second)
            return detail::Fail(error,section=="files"?ErrorCode::DuplicateTarget:ErrorCode::DuplicateKey,"Duplicate key or target: "+key,lineNumber);
    }
    for(const char* key:{"id","name","version","kind"})
        if(!keys.count(std::string("package/")+key)) return detail::Fail(error,ErrorCode::MissingField,std::string("Missing Package key: ")+key);
    if(candidate.kind==Kind::Assets&&!sections.count("files")) return detail::Fail(error,ErrorCode::MissingField,"Missing Files section");
    if(!ValidateManifest(candidate,error,limits)) return false;
    std::sort(candidate.files.begin(),candidate.files.end(),[](const File& a,const File& b){return a.destination<b.destination;});
    std::sort(candidate.requires.begin(),candidate.requires.end(),[](const Requirement& a,const Requirement& b){return a.id<b.id;});
    out=std::move(candidate); return true;
}

inline bool BuildPlan(const std::vector<Manifest>& catalog,
    const std::vector<std::string>& selected, Plan& out, Error& error,
    const Limits& limits) {
    out=Plan{}; error={};
    if(catalog.size()>limits.packages||selected.size()>limits.selected)
        return detail::Fail(error,ErrorCode::Budget,"Package or selection budget exceeded");
    std::map<std::string,const Manifest*> available;
    size_t catalogFiles=0;
    for(const auto& manifest:catalog) {
        if(!ValidateManifest(manifest,error,limits)) return false;
        if(manifest.files.size()>limits.catalogFiles-catalogFiles)
            return detail::Fail(error,ErrorCode::Budget,"Catalog file budget exceeded");
        catalogFiles+=manifest.files.size();
        if(!available.emplace(manifest.id,&manifest).second)
            return detail::Fail(error,ErrorCode::DuplicateId,"Duplicate catalog Id: "+manifest.id,0,manifest.id);
    }
    std::set<std::string> roots;
    for(const auto& id:selected) {
        if(!detail::Id(id,limits)) return detail::Fail(error,ErrorCode::InvalidId,"Selected Id must be canonical ASCII: "+id);
        roots.insert(id); // repeated UI selections are harmless.
    }
    Plan candidate;
    std::map<std::string,int> state;
    std::vector<std::string> stack;
    std::function<bool(const std::string&)> visit=[&](const std::string& id) {
        const auto found=available.find(id);
        if(found==available.end()) return detail::Fail(error,ErrorCode::MissingPackage,"Missing package: "+id,0,id);
        if(state[id]==2) return true;
        if(state[id]==1) {
            std::string chain;
            for(const auto& part:stack) { if(!chain.empty()) chain+=" -> "; chain+=part; }
            return detail::Fail(error,ErrorCode::Cycle,"Dependency cycle: "+chain+" -> "+id,0,id);
        }
        state[id]=1; stack.push_back(id);
        auto requires=found->second->requires;
        std::sort(requires.begin(),requires.end(),[](const Requirement& a,const Requirement& b){return a.id<b.id;});
        for(const auto& dependency:requires) {
            const auto dependencyFound=available.find(dependency.id);
            if(dependencyFound==available.end()) return detail::Fail(error,ErrorCode::MissingPackage,"Package "+id+" requires missing "+dependency.id,0,id);
            if(dependencyFound->second->version!=dependency.version)
                return detail::Fail(error,ErrorCode::VersionMismatch,"Package "+id+" requires "+dependency.id+"="+dependency.version+", found "+dependencyFound->second->version,0,id);
            if(!visit(dependency.id)) return false;
        }
        stack.pop_back(); state[id]=2; candidate.packages_.push_back(id); return true;
    };
    for(const auto& id:roots) if(!visit(id)) return false;
    std::map<std::string,IndexEntry> index;
    for(const auto& id:candidate.packages_) for(const auto& file:available.at(id)->files) {
        if(index.size()>=limits.planFiles) return detail::Fail(error,ErrorCode::Budget,"Generated index file budget exceeded");
        if(!index.emplace(file.destination,IndexEntry{id,file.source,file.destination}).second)
            return detail::Fail(error,ErrorCode::TargetConflict,"Multiple packages own target: "+file.destination,0,id);
    }
    for(const auto& item:index) {
        for(size_t slash=item.first.find('/');slash!=std::string::npos;slash=item.first.find('/',slash+1))
            if(index.count(item.first.substr(0,slash)))
                return detail::Fail(error,ErrorCode::TargetConflict,"File/directory target conflict: "+item.first,0,item.second.owner);
        candidate.index_.push_back(item.second);
    }
    out=std::move(candidate); return true;
}
} // namespace coop_packages
