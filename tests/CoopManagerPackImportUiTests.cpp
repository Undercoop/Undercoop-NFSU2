// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
// Reuse the isolated TEMP fixture and native-load traps. Its original test
// entry point is not called; no modpack commit, activation or game is run.
#define main CoopExistingFolderTests_NotInvoked
#include "CoopManagerFolderImportTests.cpp"
#undef main

static void PackLayoutAndConsent(){
    Reset();m::View view;view.tab=6;
    for(const auto size:{std::pair<float,float>{850.0f,620.0f},{934.0f,675.0f},{1280.0f,720.0f}}){
        const m::ModpackLayout pack(size.first,size.second);const auto import=pack.Import(),library=pack.Library();
        CHECK(import.x>=pack.base.content&&import.x+import.w<library.x);
        CHECK(library.x+library.w<=size.first-20);
        CHECK(m::HitTest(view,import.x+2,import.y+2,size.first,size.second).action==m::UiAction::PackImport);
        CHECK(m::HitTest(view,library.x+2,library.y+2,size.first,size.second).action==m::UiAction::PackLibrary);
    }
    view.tab=5;view.imports.plan=std::make_shared<m::ImportPlan>();view.imports.plan->pack=std::make_shared<m::PackArchivePlan>();
    view.imports.plan->pack->name=L"underground2.net 1.0.3";view.imports.plan->pack->files=900;view.imports.plan->pack->bytes=1000;
    view.imports.plan->pack->nativeModules={L"AnimationController.asi",L"ReShade.asi"};view.imports.plan->code=true;
    view.imports.plan->pack->notes={L"F5 is reserved for CO-OP votes.",L"Keep your current loader."};
    view.imports.plan->pack->hash=std::string(64,'a');
    const auto detailText=m::PackImportDetailsText(*view.imports.plan->pack);
    CHECK(detailText.find(L"AnimationController.asi")!=std::wstring::npos&&detailText.find(L"ReShade.asi")!=std::wstring::npos);
    CHECK(detailText.find(L"F5 is reserved")!=std::wstring::npos&&detailText.find(L"Keep your current loader.")!=std::wstring::npos);
    CHECK(detailText.find(std::wstring(64,L'a'))!=std::wstring::npos);
    view.imports.phase=m::ImportPhase::Review;
    const m::ImportLayout layout(934,675);const auto install=layout.Install(),consent=layout.Consent(),downloads=layout.Downloads(),details=layout.Details();
    CHECK(m::HitTest(view,install.x+1,install.y+1,934,675).action==m::UiAction::None);
    CHECK(m::HitTest(view,consent.x+1,consent.y+1,934,675).action==m::UiAction::ImportConsent);
    CHECK(m::HitTest(view,downloads.x+1,downloads.y+1,934,675).action!=m::UiAction::ImportDownloads);
    CHECK(m::HitTest(view,details.x+1,details.y+1,934,675).action==m::UiAction::ImportPackDetails);
    CHECK(m::DispatchUi(view,{m::UiAction::ImportConsent})==m::UiEffect::Redraw);
    CHECK(view.imports.codeAccepted);
    CHECK(m::HitTest(view,install.x+1,install.y+1,934,675).action==m::UiAction::ImportInstall);
    CHECK(!view.imports.worker.joinable());CHECK(nativeLoads==0);
    CHECK(m::ImportInspectWork(m::ImportWork::Inspect)&&m::ImportInspectWork(m::ImportWork::PackInspect));
    CHECK(!m::ImportInspectWork(m::ImportWork::Install)&&!m::ImportInspectWork(m::ImportWork::Repair));
    CHECK(std::wstring(m::kModpackArchiveFilter)==L"Modpack ZIP archives");
    CHECK(std::wstring(m::kModpackArchiveFilter+std::wcslen(m::kModpackArchiveFilter)+1)==L"*.zip");
}
static void PackCompletionDoesNotSwitch(Fixture& fixture,bool success){
    const auto root=fixture.Use(success?L"completed-pack-ui":L"failed-pack-ui");
    const auto relative=success?L"completed-pack-ui":L"failed-pack-ui";
    fixture.Dir(std::wstring(relative)+L"\\mods\\_undercoop\\modpacks");
    fixture.File(std::wstring(relative)+L"\\mods\\_undercoop\\state\\active-modpack.txt","keep-active");
    fixture.File(std::wstring(relative)+L"\\mods\\_undercoop\\state\\selection.txt","keep-selected\n");
    fixture.File(std::wstring(relative)+L"\\saves\\NAME\\NAME","keep-career");
    m::state.instanceId=L"keep-active";m::state.instanceName=L"Existing pack";m::state.packageRoot=root+L"\\existing-private-revision";
    m::state.selected={"keep-selected"};m::state.nativeTrusted=true;
    const auto selected=m::state.selected;const auto packageRoot=m::state.packageRoot;
    m::View view;view.tab=5;view.busy=true;auto& panel=view.imports;
    panel.plan=std::make_shared<m::ImportPlan>();panel.plan->pack=std::make_shared<m::PackArchivePlan>();
    panel.work=m::ImportWork::Install;panel.phase=m::ImportPhase::Installing;panel.selectedBefore={"must-not-overwrite-active-selection"};
    panel.okay=success;panel.message=success?L"Imported into Modpacks; not activated.":L"Commit rejected safely.";
    panel.worker=std::thread([]{});panel.done=true;
    CHECK(m::PollImport(view));CHECK(!view.busy&&!panel.worker.joinable());
    CHECK(m::state.instanceId==L"keep-active"&&m::state.packageRoot==packageRoot);
    CHECK(m::state.selected==selected&&m::state.nativeTrusted);
    CHECK(Read(root+L"\\mods\\_undercoop\\state\\active-modpack.txt")=="keep-active");
    CHECK(Read(root+L"\\mods\\_undercoop\\state\\selection.txt")=="keep-selected\n");
    CHECK(Read(root+L"\\saves\\NAME\\NAME")=="keep-career");
    CHECK(panel.phase==(success?m::ImportPhase::Complete:m::ImportPhase::Failed));
    CHECK(view.tab==(success?6:5));CHECK(nativeLoads==0);
}
int main(){
    PackLayoutAndConsent();Fixture fixture;PackCompletionDoesNotSwitch(fixture,true);PackCompletionDoesNotSwitch(fixture,false);
    CHECK(nativeLoads==0);std::printf("Modpack import UI: %zu checks, %zu failures; TEMP only, no import/activation/native execution.\n",checks,failures);
    return failures?1:0;
}
