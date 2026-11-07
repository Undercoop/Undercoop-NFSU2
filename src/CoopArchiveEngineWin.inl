// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

// Materialize only the exact embedded open-source decoder and its corresponding
// source/licenses. Never search PATH/Program Files or load a DLL from a mod ZIP.
inline std::mutex archiveEngineInstallMutex;
inline std::unique_ptr<Handle> archiveEngineFilePin;
inline HMODULE(WINAPI* archiveEngineLoad)(LPCWSTR,HANDLE,DWORD)=LoadLibraryExW;
inline bool EnsureArchiveEngine(const std::wstring& root,std::wstring& error){
    std::lock_guard<std::mutex> lock(archiveEngineInstallMutex);if(coop_archive::EngineReady())return true;
    HMODULE owner=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&EnsureArchiveEngine),&owner)){error=L"Cannot locate the bundled archive decoder.";return false;}
    const auto base=root+L"\\mods\\_undercoop";auto rootPin=Pin(root,true),modsPin=Pin(root+L"\\mods",true),ownerPin=Pin(base,true);
    if(!rootPin||!modsPin||!ownerPin){error=L"The private archive workspace is missing, linked or unavailable.";return false;}
    if(!ImportDirectory(base,L"archive-engine\\26.05",error))return false;
    const auto folder=base+L"\\archive-engine\\26.05";std::wstring library;std::unique_ptr<Handle> libraryPin;
    auto engineParentPin=Pin(base+L"\\archive-engine",true),engineFolderPin=Pin(folder,true);
    if(!engineParentPin||!engineFolderPin){error=L"The archive decoder directory changed or is linked.";return false;}
    for(const auto& item:std::initializer_list<std::pair<int,const char*>>{{317,"7z.dll"},{318,"license.txt"},{319,"copying.txt"},{320,"undercoop-7zip2605-src.tar.xz"},{321,"undercoop-source.txt"}}){
        const auto resource=FindResourceW(owner,MAKEINTRESOURCEW(item.first),MAKEINTRESOURCEW(10));const auto loaded=resource?LoadResource(owner,resource):nullptr;
        const auto count=resource?SizeofResource(owner,resource):0;const auto* memory=loaded?static_cast<const char*>(LockResource(loaded)):nullptr;
        if(!memory||!count||count>8*1024*1024){error=L"Bundled archive support is incomplete. Reinstall the current Undercoop build.";return false;}
        const std::string data(memory,count),relative=std::string("archive-engine/26.05/")+item.second;const auto path=folder+L"\\"+Wide(item.second);
        const auto expected=ImportHash(data);if(expected.empty()){error=L"Cannot verify the bundled archive decoder.";return false;}
        if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES&&!ImportWriteNew(base,relative,data,error))return false;
        auto pin=Pin(path);std::string hash;uint64_t length=0;
        if(!pin||!Digest(pin->value,hash,length)||length!=count||hash!=expected){
            error=L"An archive-engine file changed or is linked. It was not loaded or overwritten. Restore the bundled decoder before importing archives.";return false;}
        if(item.first==317){library=path;libraryPin=std::move(pin);}
    }
    const auto module=archiveEngineLoad(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module){error=L"The verified archive decoder could not initialize. No mod code or installer was run.";return false;}
    if(!coop_archive::BindEngine(module)){FreeLibrary(module);error=L"The archive decoder interfaces are unavailable.";return false;}
    archiveEngineFilePin=std::move(libraryPin);return true;
}
