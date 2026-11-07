// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Imported native code is NOT a reviewed adapter. Explicit pre-launch approval
// is required. It runs with game permissions, not a security sandbox.
inline bool ImportedNativeKnown(const coop_packages::Manifest& p){const auto it=state.imported.find(p.id);return p.kind==coop_packages::Kind::Dependency&&it!=state.imported.end()&&it->second.version==p.version;}
inline bool SelectedImportedLibrariesSafe(){
    std::map<std::string,std::string> libraries;
    for(const auto& id:state.plan.Packages()){const auto found=state.imported.find(id);if(found==state.imported.end())continue;
        for(const auto& f:found->second.files)if(coop_packages::detail::Extension(f.source)==".dll"){
            const auto name=f.source.substr(f.source.find_last_of('/')+1);const auto previous=libraries.find(name);
            if(previous!=libraries.end()&&previous->second!=f.hash)return Fail(L"Selected native plugins contain conflicting DLL versions: "+Wide(name));libraries[name]=f.hash;
        }
    }return true;
}
inline bool ReadImportedNative(const coop_packages::Manifest& p){
    const auto path=PackageDirectory(p.id)+L"\\undercoop.native.txt";
    if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)return GetLastError()==ERROR_FILE_NOT_FOUND;
    std::string text;coop_import::Receipt receipt;if(!ReadText(path,text,1024*1024)||!coop_import::Decode(text,receipt)||receipt.id!=p.id||receipt.version!=p.version||p.kind!=coop_packages::Kind::Dependency)return Fail(L"The imported native receipt is invalid: "+Wide(p.id));
    state.imported[p.id]=std::move(receipt);return true;
}
inline bool ImportedTreeSafe(const std::wstring& root,const std::wstring& relative,const coop_import::Receipt& r,unsigned depth=0,size_t* visited=nullptr){
    size_t localVisited=0;if(!visited)visited=&localVisited;
    if(depth>12)return false;auto folder=Pin(root+(relative.empty()?L"":L"\\"+relative),true);if(!folder)return false;
    WIN32_FIND_DATAW item{};HANDLE find=FindFirstFileW((root+(relative.empty()?L"":L"\\"+relative)+L"\\*").c_str(),&item);if(find==INVALID_HANDLE_VALUE)return false;
    bool okay=true;size_t count=0;do{const std::wstring name=item.cFileName;if(name==L"."||name==L"..")continue;
        if(++count>4096||++*visited>8192||(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){okay=false;break;}const auto child=relative.empty()?name:relative+L"\\"+name;
        if(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){if(!ImportedTreeSafe(root,child,r,depth+1,visited)){okay=false;break;}}
        else{const auto lower=coop_mods::Lower(coop_mod_scan::Utf8(Disk(child)));const auto dot=lower.find_last_of('.');const auto ext=dot==std::string::npos?"":lower.substr(dot);
            if(ext==".asi"||ext==".dll"||ext==".exe"){std::string normalized;if(!coop_mod_scan::RelativeName(child,normalized)){okay=false;break;}bool declared=false;for(const auto& f:r.files)declared=declared||f.source==normalized;if(!declared){okay=false;break;}}
        }
    }while(FindNextFileW(find,&item));okay=okay&&GetLastError()==ERROR_NO_MORE_FILES;FindClose(find);return okay;
}
