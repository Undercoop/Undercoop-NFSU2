// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#define main CoopExistingFolderTests_NotInvoked
#include "CoopManagerFolderImportTests.cpp"
#undef main

static bool MockCareerSnapshot(const std::wstring&,const std::wstring& pending,const std::atomic<bool>*) {
    return m::Below(pending,m::ModpackHome())&&m::ModpackWriteNew(pending+L"\\snapshot-proof.txt","fixture only");
}

static void ArchiveRoundTrip(const std::wstring& archive) {
    Fixture fixture;
    const auto root=fixture.Use(L"pack");
    fixture.File(L"pack\\SPEED2.EXE","test executable bytes");
    fixture.File(L"pack\\dinput8.dll","test loader bytes");
    fixture.File(L"pack\\scripts\\NFSU2Coop.ini","test settings");
    const auto marker=fixture.File(L"pack\\mods\\_undercoop\\state\\active-modpack.txt","");
    const auto selection=fixture.File(L"pack\\mods\\_undercoop\\state\\selection.txt","");

    m::PackArchivePlan plan;
    std::wstring error,id;
    const auto probe=m::InspectModpackArchive(archive,plan,error);
    if(probe!=m::PackProbe::Ready)std::fprintf(stderr,"Inspect: %s\n",coop_mod_scan::Utf8(error).c_str());
    Need(probe==m::PackProbe::Ready,"inspect the supplied 1.0.3 archive");
    CHECK(plan.files>800&&plan.nativeModules.size()==10);
    CHECK(plan.bytes>800ull*1024*1024);
    CHECK(!Exists(m::ModpackHome()));
    CHECK(m::CommitModpackArchive(root,plan,id,error,nullptr,{},MockCareerSnapshot)||
        (std::fprintf(stderr,"Commit: %s\n",coop_mod_scan::Utf8(error).c_str()),false));
    Need(!id.empty(),"publish a scratch modpack");
    CHECK(Read(marker).empty()&&Read(selection).empty());
    CHECK(m::state.selected.empty()&&m::state.instanceId.empty());
    CHECK(Read(root+L"\\dinput8.dll")=="test loader bytes");
    CHECK(!Exists(root+L"\\CARS")&&!Exists(root+L"\\SAVEGAMES"));
    m::ModpackLoaded pack;
    CHECK(m::ReadModpack(id,pack));
    CHECK(pack.info.name==L"underground2.net 1.0.3"&&pack.imported.size()==10);
    CHECK(pack.catalog.size()==17&&pack.selected.size()==17);
    const auto native=pack.root+L"\\nfsu2-extra-options\\files\\scripts\\nfsu2extraoptionssettings.ini";
    const auto config=Read(native);
    coop_ini::Document parsed;coop_ini::Error parseError;
    CHECK(coop_ini::Parse(config,parsed,parseError));
    unsigned corrected=0;
    for(const auto& row:parsed.Rows()) {
        const auto section=coop_mods::Lower(row.section),key=coop_mods::Lower(row.key);
        if((section=="hotkeys"&&(key=="unlockallthings"||key=="autodrive"))||
            (section=="gameplay"&&key=="unlockallthings")){CHECK(row.value=="0");++corrected;}
    }
    CHECK(corrected==3);
    CHECK(Exists(pack.root+L"\\ug2net-103-texturepacks\\files\\texturepacks\\underground2.net\\textures.bin"));
    CHECK(Exists(pack.root+L"\\nfsu2-car-sound-tuner\\files\\scripts\\carsounddata\\cartypemapping\\240sx.ini"));
    CHECK(Exists(pack.root+L"\\nfsu2-unlimiter\\files\\scripts\\unlimiterdata\\_general.ini"));

    // Prepare the real selected packages and collect effective inventory.
    // There is no loader invocation or game/third-party code execution.
    m::ApplyModpack(std::move(pack));
    m::PreparedActivation staged;
    const bool prepared=m::PrepareActivation(staged);
    if(!prepared)std::fprintf(stderr,"Prepare: %s\n",coop_mod_scan::Utf8(m::state.error).c_str());
    Need(prepared,"prepare imported modules, resources and sidecars");
    CHECK(staged.natives.size()==10);
    CHECK(staged.mappings.size()==plan.files-staged.natives.size()+1);
    CHECK(std::count_if(staged.mappings.begin(),staged.mappings.end(),[](const auto& mapping){
        return mapping.relative=="texwizard.json"&&!mapping.inventory;
    })==1);
    m::state.mapping=std::move(staged.mappings);
    for(const auto& module:staged.natives)m::state.inventoryNativePaths.push_back(module.path);
    m::state.ready=true;
    coop_mod_scan::additionalFiles=&m::ManagedInventoryFiles;
    // Model a directory listing that includes the synthetic loader alias.
    fixture.File(L"pack\\texwizard.json","the mapped alias masks this loose path");
    const auto imported=coop_mod_scan::Scan(root,"same runtime settings");
    if(!imported.Ready())std::fprintf(stderr,"Inventory: %s\n",imported.error.c_str());
    Need(imported.Ready(),"scan imported runtime inventory");
    CHECK(imported.entries.size()==plan.files+3);

    // Compare identical effective files in physical and managed layouts.
    const auto loose=fixture.Dir(L"loose");
    fixture.File(L"loose\\SPEED2.EXE","test executable bytes");
    fixture.File(L"loose\\dinput8.dll","test loader bytes");
    fixture.File(L"loose\\SAVEGAMES\\NFS Underground 2\\Settings.ini","old bundled settings");
    fixture.File(L"loose\\GLOBAL\\GLOBALB.LZC.bacc","Binary backup");
    std::vector<coop_mod_scan::File> effective;std::string scanError;
    Need(m::ManagedInventoryFiles(root,effective,scanError),"enumerate managed files");
    std::map<std::string,std::wstring> unique;
    for(const auto& file:effective)if(file.inventory)unique[file.relative]=file.absolute;
    for(const auto& [relative,source]:unique){
        const auto dest=m::Disk(m::Wide(relative));
        fixture.Dir(L"loose\\"+dest.substr(0,dest.find_last_of(L'\\')));
        Need(CopyFileW(source.c_str(),(loose+L"\\"+dest).c_str(),TRUE)!=FALSE,"copy matching physical fixture resource");
    }
    coop_mod_scan::additionalFiles=nullptr;
    const auto physical=coop_mod_scan::Scan(loose,"same runtime settings");
    CHECK(physical.Ready()&&physical.hash==imported.hash);
    CHECK(nativeLoads==0);
    staged.pins.clear();
    Reset();
}

int main(int argc,char** argv) {
    if(argc!=2){std::puts("Usage: CoopManagerPackImportTests <original underground2.net 1.0.3 ZIP>");return 2;}
    const int length=MultiByteToWideChar(CP_ACP,0,argv[1],-1,nullptr,0);
    if(length<2)return 2;
    std::wstring archive(size_t(length),L'\0');
    MultiByteToWideChar(CP_ACP,0,argv[1],-1,archive.data(),length);archive.pop_back();
    ArchiveRoundTrip(archive);
    std::printf("Modpack archive integration: %u checks, %u failures, %zu native loads.\n",checks,failures,nativeLoads);
    return failures?1:0;
}
