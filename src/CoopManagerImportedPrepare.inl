// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// The reviewed underground2.net SpeedReflect binary imports the dynamic
// Visual C++ runtime. Check the x86 system directory before loading any mod.
inline bool SpeedReflectRuntimePresent(){
    wchar_t systemDirectory[MAX_PATH]{};
    UINT count=GetSystemWow64DirectoryW(systemDirectory,MAX_PATH);
    if(!count)count=GetSystemDirectoryW(systemDirectory,MAX_PATH);
    if(!count||count>=MAX_PATH)return false;
    const std::wstring directory(systemDirectory,count);
    for(const auto* name:{L"MSVCP140.dll",L"VCRUNTIME140.dll"}){
        const DWORD attributes=GetFileAttributesW((directory+L"\\"+name).c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_DIRECTORY))return false;
    }
    return true;
}

inline bool PrepareImported(const coop_packages::Manifest& p,std::vector<std::unique_ptr<Handle>>& pins,
    std::vector<NativeRequest>& natives,std::vector<ConfigWatch>& watches,std::vector<Mapping>& maps,std::string& receipt){
    const auto found=state.imported.find(p.id);if(found==state.imported.end())return Fail(L"Imported component receipt is missing.");
    const auto& r=found->second;const auto root=PackageDirectory(p.id);
    const bool texWizardRootConfig=p.id=="texwizard"&&p.version=="1.0.3"&&
        std::any_of(r.files.begin(),r.files.end(),[](const auto& f){
            return f.module&&f.destination=="scripts/texwizard.asi"&&
                f.hash=="8ad971bb9330518ac5b1710fb20260e831ccebb172a3e8c2d546bf91dbcee0e2";
        });
    std::string text;auto proof=Pin(root+L"\\undercoop.native.txt");
    if(!proof||!ReadText(root+L"\\undercoop.native.txt",text,1024*1024)||text!=coop_import::Encode(r)||!ImportedTreeSafe(root,L"",r))return Fail(L"Imported native files changed or contain undeclared code: "+Wide(p.name));
    pins.push_back(std::move(proof));auto manifest=Pin(root+L"\\undercoop.mod.ini");if(!manifest)return Fail(L"Imported manifest changed.");pins.push_back(std::move(manifest));
    receipt+="ImportedNative="+p.id+"="+r.archive+"\n";
    for(const auto& f:r.files){const auto physical=root+L"\\"+Disk(Wide(f.source));auto pin=Pin(physical);std::string hash;uint64_t bytes=0;
        if(!pin||!Digest(pin->value,hash,bytes)||(!f.config&&(hash!=f.hash||bytes!=f.bytes)))return Fail(L"Imported native payload differs from its installation receipt: "+Wide(f.source));
        if(f.module){const auto basename=physical.substr(physical.find_last_of(L'\\')+1);if(GetModuleHandleW(basename.c_str()))return Fail(L"Duplicate native component is already active: "+basename);
            if(p.id=="speedreflect"&&hash=="3ef35d470c3b7e795157d65e78617c164eaafecf60493a797991629a70a54181"&&!SpeedReflectRuntimePresent())
                return Fail(L"SpeedReflect requires the Microsoft Visual C++ 2015-2022 Redistributable (x86). Install it and reopen the launcher.");
            // This known Extra Options build resolves ../CARS during startup.
            const bool scriptsCwd=Equal(basename,L"NFSU2ExtraOptions.asi")&&
                hash=="2f59d8f82c69ab2b8102e4041dc04a8c7ea9440e597eb1222a41889cfbe547ae";
            natives.push_back({physical,scriptsCwd?state.root+L"\\scripts":state.root});}
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
        // This build reads bare TexWizard.json during DllMain and uses the
        // same game-root directory for the relative TexturePacks data paths.
        if(texWizardRootConfig&&f.destination=="scripts/texwizard.json")
            maps.push_back({state.root+L"\\texwizard.json",physical,"texwizard.json",hash,bytes,false});
    }return true;
}
