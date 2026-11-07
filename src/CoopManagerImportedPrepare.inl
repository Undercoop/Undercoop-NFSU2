// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

inline bool PrepareImported(const coop_packages::Manifest& p,std::vector<std::unique_ptr<Handle>>& pins,
    std::vector<NativeRequest>& natives,std::vector<ConfigWatch>& watches,std::vector<Mapping>& maps,std::string& receipt){
    const auto found=state.imported.find(p.id);if(found==state.imported.end())return Fail(L"Imported component receipt is missing.");
    const auto& r=found->second;const auto root=PackageDirectory(p.id);
    std::string text;auto proof=Pin(root+L"\\undercoop.native.txt");
    if(!proof||!ReadText(root+L"\\undercoop.native.txt",text,1024*1024)||text!=coop_import::Encode(r)||!ImportedTreeSafe(root,L"",r))return Fail(L"Imported native files changed or contain undeclared code: "+Wide(p.name));
    pins.push_back(std::move(proof));auto manifest=Pin(root+L"\\undercoop.mod.ini");if(!manifest)return Fail(L"Imported manifest changed.");pins.push_back(std::move(manifest));
    receipt+="ImportedNative="+p.id+"="+r.archive+"\n";
    for(const auto& f:r.files){const auto physical=root+L"\\"+Disk(Wide(f.source));auto pin=Pin(physical);std::string hash;uint64_t bytes=0;
        if(!pin||!Digest(pin->value,hash,bytes)||(!f.config&&(hash!=f.hash||bytes!=f.bytes)))return Fail(L"Imported native payload differs from its installation receipt: "+Wide(f.source));
        if(f.module){const auto basename=physical.substr(physical.find_last_of(L'\\')+1);if(GetModuleHandleW(basename.c_str()))return Fail(L"Duplicate native component is already active: "+basename);
            natives.push_back({physical,state.root});}
        else if(coop_packages::detail::Extension(f.source)==".dll"){
            const auto basename=physical.substr(physical.find_last_of(L'\\')+1);
            if(const auto active=GetModuleHandleW(basename.c_str())){wchar_t actual[MAX_PATH]{};
                if(!GetModuleFileNameW(active,actual,MAX_PATH)||!Equal(actual,physical))return Fail(L"A different DLL with the same name is already loaded: "+basename);}
        }
        if(f.config){ConfigWatch watch;watch.physical=physical;watch.relative=coop_mods::Lower(coop_mod_scan::Utf8(physical.substr(state.root.size()+1)));for(auto& c:watch.relative)if(c=='\\')c='/';coop_mod_scan::Sha256 sha;std::string error;uint64_t total=0;
            if(!coop_mod_scan::HashFile({physical,f.destination},sha,watch.expected,total,error))return Fail(L"Cannot fingerprint imported options.");
            receipt+="ImportedOptions="+f.destination+"="+coop_mod_scan::Hex(watch.expected.hash)+"\n";watches.push_back(std::move(watch));
        }else{receipt+=f.destination+"="+hash+"\n";pins.push_back(std::move(pin));}
        const auto ext=coop_packages::detail::Extension(f.destination);
        if(ext!=".asi"&&ext!=".dll")maps.push_back({state.root+L"\\"+Disk(Wide(f.destination)),physical,f.destination,hash,bytes});
    }return true;
}
