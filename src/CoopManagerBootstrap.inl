// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.

// Included after the manager's confined path/file helpers. A clean installation
// has no package catalog or private workspace until the ASI prepares them.
inline bool BootstrapInstallation(const std::wstring& root){
    auto rootPin=Pin(root,true);
    if(!rootPin)return Fail(L"Cannot initialize Undercoop in the game folder: "+root);
    std::vector<std::unique_ptr<Handle>> directories;
    for(const auto* relative:{L"mods",L"mods\\_undercoop",L"mods\\_undercoop\\state"}){
        const auto path=root+L"\\"+relative;
        if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES){
            const DWORD error=GetLastError();
            if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)
                return Fail(L"Cannot inspect the Undercoop startup folder: "+path);
            if(!CreateDirectoryW(path.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)
                return Fail(L"Cannot create the Undercoop startup folder: "+path);
        }
        auto pin=Pin(path,true);
        if(!pin)return Fail(L"The Undercoop startup path must be a regular writable folder: "+path);
        directories.push_back(std::move(pin));
    }
    const auto target=root+L"\\global.ini";
    if(GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES){
        if(!Pin(target))return Fail(L"Cannot read the existing loader configuration: "+target);
        return true;
    }
    const DWORD missing=GetLastError();
    if(missing!=ERROR_FILE_NOT_FOUND&&missing!=ERROR_PATH_NOT_FOUND)
        return Fail(L"Cannot inspect the loader configuration: "+target);

    static std::atomic<unsigned long> nextFile{0};
    const auto staging=root+L"\\mods\\_undercoop\\state\\startup-"+
        std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+
        std::to_wstring(nextFile.fetch_add(1))+L".pending";
    Handle file(CreateFileW(staging.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)return Fail(L"Cannot prepare the Undercoop startup configuration.");
    constexpr char contents[]="; Created automatically by Undercoop: NFSU2.\r\n"
        "[GlobalSets]\r\nDontLoadFromDllMain=1\r\n";
    DWORD written=0;
    const bool complete=WriteFile(file.value,contents,DWORD(sizeof(contents)-1),&written,nullptr)&&
        written==sizeof(contents)-1&&FlushFileBuffers(file.value);
    CloseHandle(file.value);file.value=INVALID_HANDLE_VALUE;
    if(!complete){DeleteFileW(staging.c_str());return Fail(L"Cannot write the Undercoop startup configuration.");}
    if(MoveFileExW(staging.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))return true;
    // Another instance may have completed the same first-run setup. Publish
    // without replacement, so an existing user's configuration always wins.
    const DWORD published=GetLastError();
    DeleteFileW(staging.c_str());
    if((published==ERROR_ALREADY_EXISTS||published==ERROR_FILE_EXISTS)&&Pin(target))return true;
    return Fail(L"Cannot finish the Undercoop startup configuration: "+target);
}
