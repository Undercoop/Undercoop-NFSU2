// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Dedicated launcher settings for this ASI only. Included inside CoopModManager
// after CoopManagerIni.inl. Mod package options, selections and save domains do
// not participate. Untouched settings (including legacy/unknown keys) retain
// their original bytes; an Apply commits one validated draft with a backup.
#pragma once
namespace undercoop_settings {
enum class Field { Instance,PeerAddress,Port,PeerPort,AutoStartCoop,EnableSync,RaceAI,WorldFleet,Profile,AutoShow,BackgroundOpacity,AutoBackup,Count };
constexpr size_t kCount=size_t(Field::Count);
struct Definition {const char* section;const char* key;const wchar_t* fallback;};
inline constexpr Definition definitions[kCount]={
    {"Network","Instance",L"0"},{"Network","PeerAddress",L"127.0.0.1"},
    {"Network","Port",L"48930"},{"Network","PeerPort",L"48931"},
    {"General","AutoStartCoop",L"0"},{"GhostCar","EnableSync",L"1"},
    {"Experimental","RaceAI",L"0"},{"Experimental","WorldFleet",L"0"},
    {"Career","Profile",L""},{"Announcements","AutoShow",L"1"},
    {"Announcements","BackgroundOpacity",L"70"},{"Career","AutoBackup",L"1"}
};
struct Draft {
    std::array<std::wstring,kCount> values{},baseline{};
    coop_ini::Document document;
    std::wstring path,error;
    UINT codepage=CP_ACP;
    bool loaded=false;
};
inline const std::wstring& Get(const Draft& draft,Field field){return draft.values.at(size_t(field));}
inline void Set(Draft& draft,Field field,const std::wstring& value){draft.values.at(size_t(field))=value;}
inline bool Dirty(const Draft& draft){return draft.loaded&&draft.values!=draft.baseline;}
inline void Discard(Draft& draft){draft.values=draft.baseline;draft.error.clear();}
inline std::wstring Path(){return state.root+L"\\scripts\\NFSU2Coop.ini";}
inline bool SameKey(const coop_ini::Row& row,size_t index){
    return coop_ini::detail::Fold(row.section)==coop_ini::detail::Fold(definitions[index].section)&&
        coop_ini::detail::Fold(row.key)==coop_ini::detail::Fold(definitions[index].key);
}
inline bool Populate(Draft& draft,const std::string& bytes){
    coop_ini::Error error;if(!coop_ini::Parse(bytes,draft.document,error,IniLimits())){draft.error=Wide(error.detail);return false;}
    draft.codepage=draft.document.EncodingKind()==coop_ini::Encoding::Utf8Bom?CP_UTF8:CP_ACP;
    for(size_t i=0;i<kCount;++i){draft.values[i]=definitions[i].fallback;
        for(const auto& row:draft.document.Rows())if(SameKey(row,i)){draft.values[i]=IniWide(row.value,draft.codepage);break;}}
    draft.baseline=draft.values;draft.loaded=true;draft.error.clear();return true;
}
inline bool Load(Draft& draft){
    draft=Draft{};draft.path=Path();std::string bytes;
    if(!RealPath(state.root,true)||!RealPath(state.root+L"\\scripts",true)||!ReadText(draft.path,bytes)){
        draft.error=L"Cannot read this game's scripts/NFSU2Coop.ini. Close another editor, then reload.";return false;}
    return Populate(draft,bytes);
}
inline bool Number(const std::wstring& text,unsigned minimum,unsigned maximum){
    if(text.empty()||text.size()>10)return false;unsigned value=0;
    for(wchar_t ch:text){if(ch<L'0'||ch>L'9')return false;const unsigned digit=unsigned(ch-L'0');
        if(value>maximum/10||value*10+digit>maximum)return false;value=value*10+digit;}
    return value>=minimum&&value<=maximum;
}
inline bool Address(const std::wstring& text){
    // The native runtime uses inet_pton(AF_INET); DNS/IPv6 are not supported.
    if(text.empty()||text.size()>15)return false;size_t at=0;
    for(unsigned part=0;part<4;++part){const size_t next=text.find(L'.',at),end=next==std::wstring::npos?text.size():next;
        const auto digits=text.substr(at,end-at);
        if(digits.empty()||digits.size()>3||(digits.size()>1&&digits[0]==L'0')||!Number(digits,0,255))return false;
        if(part==3)return next==std::wstring::npos;if(next==std::wstring::npos)return false;at=next+1;}
    return false;
}
inline bool Validate(const Draft& draft,std::wstring& message){
    message.clear();if(!draft.loaded){message=draft.error.empty()?L"Reload Undercoop settings before applying changes.":draft.error;return false;}
    if(state.ready||state.fatal||!Equal(draft.path,Path())){message=L"These settings no longer belong to the current pre-game window.";return false;}
    for(size_t i=0;i<kCount;++i){
        if(draft.values[i]==draft.baseline[i])continue;const auto field=Field(i);const auto& value=draft.values[i];bool okay=false;
        switch(field){
        case Field::Instance:okay=Number(value,0,2);message=L"Choose Automatic, Host or Player 2.";break;
        case Field::PeerAddress:okay=Address(value);message=L"Enter a valid IPv4 address, such as 127.0.0.1 or your teammate's Radmin IPv4 address.";break;
        case Field::Port:case Field::PeerPort:okay=Number(value,1,64535);message=L"Ports must be between 1 and 64535; the gameplay connection also uses port + 1000.";break;
        case Field::AutoStartCoop:case Field::EnableSync:case Field::RaceAI:case Field::WorldFleet:case Field::AutoBackup:
            okay=value==L"0"||value==L"1";message=L"Choose On or Off for this option.";break;
        case Field::AutoShow:okay=Number(value,0,2);message=L"Choose a supported notification mode.";break;
        case Field::BackgroundOpacity:okay=Number(value,10,100);message=L"Console background opacity must be between 10% and 100%.";break;
        case Field::Profile:{std::string native;okay=value.empty()||(coop_save::IsSafeProfileName(value)&&IniBytes(value,CP_ACP,native)&&native.size()<32);
            message=L"The starting career name must be empty or a valid game profile name of up to 31 bytes.";break;}
        default:break;
        }
        if(!okay)return false;std::string encoded;
        if(!IniBytes(value,draft.codepage,encoded)){message=L"This value cannot be represented in the INI's original encoding.";return false;}
        for(const auto& row:draft.document.Rows())if(SameKey(row,i)&&!row.editable){
            message=L"This option has duplicate or ambiguous INI entries. Resolve them in the file, then reload.";return false;}
    }
    message.clear();return true;
}
inline bool AppendMissing(std::string& bytes,const Definition& definition,const std::string& value,std::wstring& message){
    const std::string newline=bytes.find("\r\n")!=std::string::npos?"\r\n":"\n";
    size_t at=bytes.compare(0,3,"\xef\xbb\xbf")==0?3:0,insert=bytes.size(),matches=0;bool inSection=false;
    while(at<bytes.size()){
        const size_t first=at,end=bytes.find_first_of("\r\n",at),last=end==std::string::npos?bytes.size():end;
        at=last;if(at<bytes.size()&&bytes[at]=='\r')++at;if(at<bytes.size()&&bytes[at]=='\n')++at;
        const size_t left=coop_ini::detail::Left(bytes,first,last);
        if(left==last||bytes[left]!='[')continue;const size_t close=bytes.find(']',left+1);
        if(close==std::string::npos||close>=last)continue;
        if(inSection){insert=first;inSection=false;}
        const size_t begin=coop_ini::detail::Left(bytes,left+1,close),finish=coop_ini::detail::Right(bytes,begin,close);
        if(coop_ini::detail::Fold(bytes.substr(begin,finish-begin))==coop_ini::detail::Fold(definition.section)){++matches;inSection=true;insert=bytes.size();}
    }
    if(matches>1){message=L"This option's INI section is duplicated. Resolve it in the file, then reload.";return false;}
    const std::string line=std::string(definition.key)+" = "+value+newline;
    if(!matches){if(!bytes.empty()&&bytes.back()!='\r'&&bytes.back()!='\n')bytes+=newline;
        bytes+=newline+"["+definition.section+"]"+newline+line;
    }else{std::string addition;if(insert&&bytes[insert-1]!='\r'&&bytes[insert-1]!='\n')addition+=newline;addition+=line;bytes.insert(insert,addition);}
    return true;
}
inline bool Render(const Draft& draft,const std::string& current,std::string& output,std::wstring& message){
    if(!Validate(draft,message))return false;std::vector<coop_ini::Edit> edits;std::vector<std::pair<size_t,std::string>> missing;
    for(size_t i=0;i<kCount;++i)if(draft.values[i]!=draft.baseline[i]){
        std::string value;if(!IniBytes(draft.values[i],draft.codepage,value))return false;size_t found=0;
        for(const auto& row:draft.document.Rows())if(SameKey(row,i)){edits.push_back({row.id,value});++found;}
        if(!found)missing.push_back({i,value});
    }
    coop_ini::Error error;if(!coop_ini::Render(draft.document,current,edits,output,error,IniLimits())){message=Wide(error.detail);return false;}
    for(const auto& item:missing)if(!AppendMissing(output,definitions[item.first],item.second,message))return false;
    coop_ini::Document verify;if(!coop_ini::Parse(output,verify,error,IniLimits())){message=Wide(error.detail);return false;}
    for(size_t i=0;i<kCount;++i)if(draft.values[i]!=draft.baseline[i]){size_t matches=0;
        for(const auto& row:verify.Rows())if(SameKey(row,i)){++matches;if(!row.editable||IniWide(row.value,draft.codepage)!=draft.values[i]){
            message=L"This value would change the INI's quoting or comments. Use a plain value and try again.";return false;}}
        if(matches!=1){message=L"The edited option could not be represented without ambiguity.";return false;}}
    return true;
}
inline bool Save(Draft& draft,std::wstring& message){
    if(!Validate(draft,message))return false;if(!Dirty(draft)){message=L"No changes to apply.";return true;}
    auto root=Pin(state.root,true),scripts=Pin(state.root+L"\\scripts",true);if(!root||!scripts){message=L"The Undercoop settings folder changed. Reload before trying again.";return false;}
    auto original=IniHandle(draft.path,GENERIC_READ|DELETE,OPEN_EXISTING);std::string current,output;
    if(!original||!IniRead(original->value,current)){message=L"Undercoop settings are busy. Close the other editor, then try again.";return false;}
    if(!Render(draft,current,output,message))return false;
    const auto work=state.root+L"\\mods\\_undercoop\\state";auto workPin=Pin(work,true);
    if(!workPin){message=L"The private settings workspace is unavailable.";return false;}
    for(const auto* name:{L"ini-backups",L"ini-staging"}){const auto folder=work+L"\\"+name;
        if(GetFileAttributesW(folder.c_str())==INVALID_FILE_ATTRIBUTES&&!CreateDirectoryW(folder.c_str(),nullptr)){
            message=L"Cannot create the settings backup folder. The INI was not changed.";return false;}}
    auto backups=Pin(work+L"\\ini-backups",true),staging=Pin(work+L"\\ini-staging",true);
    if(!backups||!staging){message=L"The settings backup folders are linked or unavailable.";return false;}
    static LONG serial=0;const auto token=L"undercoop-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(InterlockedIncrement(&serial));
    const auto pending=work+L"\\ini-staging\\"+token+L".pending",backup=work+L"\\ini-backups\\"+token+L".ini";
    if(pending.size()>=MAX_PATH||backup.size()>=MAX_PATH){message=L"The settings backup path is too long.";return false;}
    auto next=IniHandle(pending,GENERIC_READ|GENERIC_WRITE|DELETE,CREATE_NEW);
    struct PendingCleanup{HANDLE file;bool published=false;~PendingCleanup(){if(file!=INVALID_HANDLE_VALUE&&!published){FILE_DISPOSITION_INFO info{TRUE};SetFileInformationByHandle(file,FileDispositionInfo,&info,sizeof(info));}}}cleanup{next?next->value:INVALID_HANDLE_VALUE};
    DWORD written=0;std::string verified;
    if(!next||!WriteFile(next->value,output.data(),DWORD(output.size()),&written,nullptr)||written!=output.size()||!FlushFileBuffers(next->value)||!IniRead(next->value,verified)||verified!=output){
        message=L"Cannot verify the new settings. The original INI was not changed.";return false;}
    if(!IniRename(original->value,backup)){message=L"Cannot back up Undercoop settings. The original INI was not changed.";return false;}
    if(!IniRename(next->value,draft.path)){
        const bool restored=IniRename(original->value,draft.path);message=restored?L"Apply failed; the original settings were restored.":L"Apply failed. Your original settings are safe at: "+backup;return false;}
    cleanup.published=true;
    // Render was parsed and verified before publication. Update the draft
    // without an unlocked path reread that could race another editor.
    if(!Populate(draft,output)){message=L"Settings were saved with a backup. Reload this page.";return true;}
    message=L"Applied for the next launch. The previous INI was backed up.";return true;
}
} // namespace undercoop_settings
