// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Bounded, byte-preserving INI editing only. No Windows APIs, filesystem access,
// execution, interpolation or includes. A caller must separately confine paths
// and commit the rendered bytes safely. Unknown lines/keys remain untouched.
namespace coop_ini {

struct Limits {
    size_t documentBytes=1024*1024, lineBytes=16384, lines=16384;
    size_t rows=4096, sectionBytes=256, keyBytes=256, valueBytes=8192, edits=4096;
};
enum class Encoding { Bytes, Utf8Bom };
enum class ErrorCode {
    None, Budget, Encoding, Syntax, Stale, UnknownRow, DuplicateEdit,
    ReadOnly, InvalidValue
};
struct Error {
    ErrorCode code=ErrorCode::None;
    size_t line=0, rowId=0;
    std::string detail;
};
struct Row {
    size_t id=0, line=0;
    std::string section, key, value;
    size_t valueOffset=0, valueBytes=0;
    char quote=0; // Original outer quotes are outside the editable byte span.
    std::string leadingComments, inlineComment;
    bool editable=true;
    std::string readOnlyReason;
};
struct Edit { size_t rowId=0; std::string value; };
class Document;
inline bool Parse(const std::string&,Document&,Error&,const Limits& = Limits{});
inline bool Render(const Document&,const std::string&,const std::vector<Edit>&,
    std::string&,Error&,const Limits& = Limits{});

class Document {
public:
    const std::string& Source() const { return source_; }
    const std::vector<Row>& Rows() const { return rows_; }
    Encoding EncodingKind() const { return encoding_; }
    bool Valid() const { return valid_; }
private:
    std::string source_;
    std::vector<Row> rows_;
    Encoding encoding_=Encoding::Bytes;
    bool valid_=false;
    friend bool Parse(const std::string&,Document&,Error&,const Limits&);
    friend bool Render(const Document&,const std::string&,const std::vector<Edit>&,
        std::string&,Error&,const Limits&);
};

namespace detail {
inline bool Fail(Error& error,ErrorCode code,const std::string& detail,
    size_t line=0,size_t rowId=0) {
    error={code,line,rowId,detail};return false;
}
inline bool Space(char ch) { return ch==' '||ch=='\t'; }
inline bool Comment(char ch) { return ch==';'||ch=='#'; }
inline bool CommentAt(const std::string& bytes,size_t at,size_t last) {
    return at<last&&(Comment(bytes[at])||(bytes[at]=='/'&&at+1<last&&bytes[at+1]=='/'));
}
inline size_t Left(const std::string& source,size_t first,size_t last) {
    while(first<last&&Space(source[first]))++first;return first;
}
inline size_t Right(const std::string& source,size_t first,size_t last) {
    while(last>first&&Space(source[last-1]))--last;return last;
}
inline std::string Fold(std::string text) {
    for(char& ch:text)if(ch>='A'&&ch<='Z')ch=static_cast<char>(ch-'A'+'a');
    return text;
}
inline bool Escaped(const std::string& source,size_t index,size_t begin) {
    size_t slashes=0;while(index>begin&&source[index-1]=='\\'){--index;++slashes;}
    return (slashes&1)!=0;
}
inline void ReadOnly(Row& row,const std::string& reason) {
    row.editable=false;
    if(row.readOnlyReason.empty())row.readOnlyReason=reason;
    else if(row.readOnlyReason.find(reason)==std::string::npos)
        row.readOnlyReason+="; "+reason;
}
inline bool PlainBytes(const std::string& bytes) {
    for(unsigned char ch:bytes)
        if(ch==0||(ch<32&&ch!='\t'&&ch!='\r'&&ch!='\n')||ch==127)return false;
    return true; // Deliberately do not reinterpret ANSI/high bytes as UTF-8.
}
inline bool SafeValue(const std::string& bytes) {
    for(unsigned char ch:bytes)
        if(ch==0||ch=='\r'||ch=='\n'||(ch<32&&ch!='\t')||ch==127)return false;
    return true;
}
inline void ValueSpan(const std::string& source,size_t first,size_t last,Row& row) {
    const size_t afterEquals=first;
    first=Left(source,first,last);last=Right(source,first,last);
    row.valueOffset=first;row.valueBytes=last-first;
    if(first==last)return;
    if(CommentAt(source,first,last)&&first>afterEquals) {
        // Empty values with inline comments keep their existing separator. Put
        // a later insertion before that separator, not against the ';'/'#'.
        row.valueOffset=afterEquals;row.valueBytes=0;
        row.inlineComment=source.substr(first,last-first);return;
    }
    if(source[first]=='\"'||source[first]=='\'') {
        const char quote=source[first];size_t close=first+1;
        while(close<last&&(source[close]!=quote||Escaped(source,close,first+1)))++close;
        if(close==last) { ReadOnly(row,"Unclosed quoted value");return; }
        const size_t suffix=Left(source,close+1,last);
        if(suffix<last&&(!CommentAt(source,suffix,last)||suffix==close+1)) {
            ReadOnly(row,"Unexpected text after quoted value");return;
        }
        row.quote=quote;row.valueOffset=first+1;row.valueBytes=close-first-1;
        if(suffix<last)row.inlineComment=source.substr(suffix,last-suffix);
        return;
    }
    char quoted=0;
    for(size_t at=first;at<last;++at) {
        const char ch=source[at];
        if(quoted) {
            if(ch==quoted&&!Escaped(source,at,first))quoted=0;
        } else if((ch=='\"'||ch=='\'')&&(at==first||Space(source[at-1]))&&
            !Escaped(source,at,first))quoted=ch;
        else if(CommentAt(source,at,last)&&at>first&&Space(source[at-1])) {
            row.valueBytes=Right(source,first,at)-first;
            row.inlineComment=source.substr(at,last-at);break;
        }
    }
    if(quoted)ReadOnly(row,"Unclosed embedded quote");
}
} // namespace detail

inline bool Parse(const std::string& bytes,Document& out,Error& error,const Limits& limits) {
    using namespace detail;
    if(bytes.size()>limits.documentBytes)return Fail(error,ErrorCode::Budget,"INI exceeds document byte limit");
    if(bytes.size()>=2&&((static_cast<unsigned char>(bytes[0])==0xff&&
        static_cast<unsigned char>(bytes[1])==0xfe)||(static_cast<unsigned char>(bytes[0])==0xfe&&
        static_cast<unsigned char>(bytes[1])==0xff)))
        return Fail(error,ErrorCode::Encoding,"UTF-16 INIs are read-only; no byte conversion was attempted");
    if(!PlainBytes(bytes))return Fail(error,ErrorCode::Encoding,"INI contains NUL or unsupported control bytes");
    Document parsed;parsed.source_=bytes;
    size_t at=0,line=0;
    if(bytes.size()>=3&&bytes.compare(0,3,"\xef\xbb\xbf")==0) {
        parsed.encoding_=Encoding::Utf8Bom;at=3;
    }
    std::string section,pendingComments;
    std::map<std::string,size_t> sectionCounts;
    std::map<std::pair<std::string,std::string>,std::vector<size_t>> keys;
    while(at<bytes.size()) {
        if(++line>limits.lines)return Fail(error,ErrorCode::Budget,"INI exceeds line count limit",line);
        const size_t first=at;
        size_t end=bytes.find_first_of("\r\n",first);
        if(end==std::string::npos)end=bytes.size();
        if(end-first>limits.lineBytes)return Fail(error,ErrorCode::Budget,"INI line exceeds byte limit",line);
        at=end;
        if(at<bytes.size()&&bytes[at]=='\r')++at;
        if(at<bytes.size()&&bytes[at]=='\n')++at;
        const size_t left=Left(bytes,first,end),right=Right(bytes,left,end);
        if(left==right) { pendingComments.clear();continue; }
        if(CommentAt(bytes,left,right)) { pendingComments.append(bytes,first,at-first);continue; }
        if(bytes[left]=='[') {
            const size_t close=bytes.find(']',left+1);
            if(close==std::string::npos||close>=end)
                return Fail(error,ErrorCode::Syntax,"Unclosed INI section",line);
            const size_t nameFirst=Left(bytes,left+1,close),nameLast=Right(bytes,nameFirst,close);
            const size_t suffix=Left(bytes,close+1,end);
            if(nameFirst==nameLast||(suffix<end&&!CommentAt(bytes,suffix,end)))
                return Fail(error,ErrorCode::Syntax,"Malformed INI section",line);
            if(nameLast-nameFirst>limits.sectionBytes)
                return Fail(error,ErrorCode::Budget,"INI section name exceeds byte limit",line);
            section=bytes.substr(nameFirst,nameLast-nameFirst);++sectionCounts[Fold(section)];
            continue;
        }
        const size_t equal=bytes.find('=',left);
        if(equal==std::string::npos||equal>=end) { pendingComments.clear();continue; }
        const size_t keyLast=Right(bytes,left,equal);
        if(keyLast==left)return Fail(error,ErrorCode::Syntax,"Empty INI key",line);
        if(keyLast-left>limits.keyBytes)
            return Fail(error,ErrorCode::Budget,"INI key exceeds byte limit",line);
        if(parsed.rows_.size()>=limits.rows)
            return Fail(error,ErrorCode::Budget,"INI exceeds editable row count limit",line);
        Row row;row.id=parsed.rows_.size()+1;row.line=line;row.section=section;
        row.key=bytes.substr(left,keyLast-left);row.leadingComments=std::move(pendingComments);
        pendingComments.clear();ValueSpan(bytes,equal+1,end,row);
        if(row.valueBytes>limits.valueBytes)
            return Fail(error,ErrorCode::Budget,"INI value exceeds byte limit",line,row.id);
        row.value=bytes.substr(row.valueOffset,row.valueBytes);
        keys[{Fold(section),Fold(row.key)}].push_back(parsed.rows_.size());
        parsed.rows_.push_back(std::move(row));
    }
    for(Row& row:parsed.rows_)
        if(sectionCounts[Fold(row.section)]>1)ReadOnly(row,"Duplicate section name");
    for(const auto& key:keys)if(key.second.size()>1)
        for(size_t index:key.second)ReadOnly(parsed.rows_[index],"Duplicate key in section");
    parsed.valid_=true;out=std::move(parsed);error={};return true;
}

// currentBytes must be read again immediately before saving. Returning false
// leaves output exactly as supplied, including on stale/ambiguous documents.
// An edited quoted value excludes its original outer quotes; surrounding layout,
// line endings, BOM, comments and all unedited bytes are copied verbatim.
inline bool Render(const Document& document,const std::string& currentBytes,
    const std::vector<Edit>& edits,std::string& output,Error& error,const Limits& limits) {
    using namespace detail;
    if(!document.valid_)return Fail(error,ErrorCode::Syntax,"No parsed INI document");
    if(currentBytes!=document.source_)return Fail(error,ErrorCode::Stale,"INI changed after it was opened; reload it first");
    if(edits.size()>limits.edits)return Fail(error,ErrorCode::Budget,"Too many INI edits");
    if(currentBytes.size()>limits.documentBytes)return Fail(error,ErrorCode::Budget,"INI exceeds document byte limit");
    std::map<size_t,std::string> replacements;
    size_t removedBytes=0,insertedBytes=0;
    for(const Edit& edit:edits) {
        if(edit.rowId==0||edit.rowId>document.rows_.size())
            return Fail(error,ErrorCode::UnknownRow,"Unknown INI row",0,edit.rowId);
        const Row& row=document.rows_[edit.rowId-1];
        if(!replacements.emplace(edit.rowId,edit.value).second)
            return Fail(error,ErrorCode::DuplicateEdit,"INI row was edited twice",row.line,row.id);
        if(!row.editable)return Fail(error,ErrorCode::ReadOnly,row.readOnlyReason,row.line,row.id);
        if(edit.value.size()>limits.valueBytes)
            return Fail(error,ErrorCode::Budget,"Edited INI value exceeds byte limit",row.line,row.id);
        if(!SafeValue(edit.value))
            return Fail(error,ErrorCode::InvalidValue,"INI value contains newline, NUL or control bytes",row.line,row.id);
        if(edit.value.size()>limits.documentBytes-insertedBytes)
            return Fail(error,ErrorCode::Budget,"Edited INI exceeds document byte limit",row.line,row.id);
        insertedBytes+=edit.value.size();removedBytes+=row.valueBytes;
    }
    const size_t retainedBytes=currentBytes.size()-removedBytes;
    if(insertedBytes>limits.documentBytes-retainedBytes)
        return Fail(error,ErrorCode::Budget,"Edited INI exceeds document byte limit");
    const size_t resultBytes=retainedBytes+insertedBytes;
    std::string rendered;rendered.reserve(resultBytes);size_t copied=0;
    for(const auto& replacement:replacements) {
        const Row& row=document.rows_[replacement.first-1];
        rendered.append(currentBytes,copied,row.valueOffset-copied);
        rendered+=replacement.second;copied=row.valueOffset+row.valueBytes;
    }
    rendered.append(currentBytes,copied,currentBytes.size()-copied);
    Document verified;Error parseError;
    if(!Parse(rendered,verified,parseError,limits)) { error=std::move(parseError);return false; }
    if(verified.rows_.size()!=document.rows_.size()||verified.encoding_!=document.encoding_)
        return Fail(error,ErrorCode::InvalidValue,"INI edit would change document structure");
    for(size_t i=0;i<document.rows_.size();++i) {
        const Row& before=document.rows_[i];const Row& after=verified.rows_[i];
        const auto edited=replacements.find(before.id);
        const std::string& expected=edited==replacements.end()?before.value:edited->second;
        if(before.key!=after.key||before.section!=after.section||before.quote!=after.quote||
            before.editable!=after.editable||after.value!=expected||before.line!=after.line||
            before.inlineComment!=after.inlineComment)
            return Fail(error,ErrorCode::InvalidValue,
                "INI value would alter quoting, comments or structure; keep the original value format",before.line,before.id);
    }
    output.swap(rendered);error={};return true;
}

} // namespace coop_ini
