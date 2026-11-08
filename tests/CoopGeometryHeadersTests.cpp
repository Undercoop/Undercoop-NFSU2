// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#include "../src/CoopGeometryHeaders.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static unsigned checks=0;
static void Check(bool value,const char* name) {
    ++checks;
    if(!value){std::fprintf(stderr,"FAIL: %s\n",name);std::exit(1);}
}
struct FakeEngine {
    std::vector<coop_geometry::Header> headers;
    unsigned opens=0,closes=0,reads=0;
    bool rejectOpen=false,failReadAfterOpen=false;
    void Add(const char* path,uintptr_t address,int refs=1) {
        coop_geometry::Header h{};h.address=address;h.references=refs;
        std::snprintf(h.path,sizeof(h.path),"%s",path);headers.push_back(h);
    }
    coop_geometry::Header* Find(const char* path) {
        for(auto& h:headers)if(h.address&&coop_geometry::SamePath(path,h.path))return &h;
        return nullptr;
    }
    bool Read(const char* path,coop_geometry::Header& h) {
        ++reads;
        if(failReadAfterOpen&&opens)return false;
        auto* found=Find(path);if(!found)return false;h=*found;return true;
    }
    bool Open(const char* path) {
        if(rejectOpen)return false;
        auto* h=Find(path);if(!h)return false;++opens;++h->references;return true;
    }
    void Close(const char* path) {
        auto* h=Find(path);Check(h&&h->references>0,"close references a live owned header");
        ++closes;if(!--h->references)h->address=0;
    }
    auto Reader(){return [this](const char* p,coop_geometry::Header& h){return Read(p,h);};}
    auto Opener(){return [this](const char* p){return Open(p);};}
    auto Closer(){return [this](const char* p){Close(p);};}
};
int main() {
    using namespace coop_geometry;
    for(const char* p:{"CARS\\SKYLINE\\GEOMETRY.BIN","cars/wheels/geometry_BBS.bin","CARS\\X\\GEOMETRY.BIN"})
        Check(GeometryPath(p),"native car/shared/wheel geometry path accepted");
    for(const char* p:{"", "CARS\\GEOMETRY.BIN","GLOBAL\\GEOMETRY.BIN","CARS\\X\\TEXTURES.BIN",
        "C:\\CARS\\X\\GEOMETRY.BIN","\\CARS\\X\\GEOMETRY.BIN","CARS\\..\\GEOMETRY.BIN",
        "CARS\\X\\GEOMETRY.BIN\\","CARS\\\\X\\GEOMETRY.BIN","CARS\\X\\GEOMETRY.BIN.exe"})
        Check(!GeometryPath(p),"unrelated or escaping path rejected");
    Check(!GeometryPath(nullptr),"null path rejected");
    const std::string longPath="CARS\\"+std::string(115,'X')+"\\GEOMETRY.BIN";
    Check(!GeometryPath(longPath.c_str()),"overlong path rejected");
    Check(SamePath("cars/wheels/geometry_BBS.bin","CARS\\WHEELS\\GEOMETRY_BBS.BIN"),"path comparison matches native spelling variations");

    {
        FakeEngine e;e.Add("CARS\\SKYLINE\\GEOMETRY.BIN",0x1000);
        HeaderLeases<2> leases;
        Check(leases.Retain(e.headers[0].path,e.Reader(),e.Opener()),"first retain acquires one reference");
        Check(leases.Retain("cars/skyline/geometry.bin",e.Reader(),e.Opener()),"equivalent retain is idempotent");
        Check(leases.Size()==1&&e.opens==1&&e.headers[0].references==2,"duplicate retain does not multiply references");
        e.Close(e.headers[0].path); // native Stage5 drops its own reference
        Check(e.headers[0].address==0x1000&&e.headers[0].references==1,"Stage5 retirement keeps index alive under our lease");
        Check(leases.Release(e.Reader(),e.Closer())==1,"frontend releases the owned reference");
        Check(!leases.Size()&&!e.headers[0].address&&e.closes==2,"native and owned references both retire exactly once");
        Check(!leases.Release(e.Reader(),e.Closer())&&e.closes==2,"second cleanup cannot double release");
    }
    {
        FakeEngine e;e.Add("CARS\\A\\GEOMETRY.BIN",0x1000);e.Add("CARS\\B\\GEOMETRY.BIN",0x2000);
        HeaderLeases<1> leases;
        Check(leases.Retain(e.headers[0].path,e.Reader(),e.Opener()),"capacity fixture retains first header");
        Check(!leases.Retain(e.headers[1].path,e.Reader(),e.Opener())&&e.opens==1,"full lease table never acquires untracked reference");
        leases.Release(e.Reader(),e.Closer());
        Check(e.headers[0].references==1&&e.headers[1].references==1,"budget pressure preserves native ownership");
    }
    {
        FakeEngine e;e.Add("CARS\\A\\GEOMETRY.BIN",0x1000);
        HeaderLeases<2> leases;
        Check(leases.Retain(e.headers[0].path,e.Reader(),e.Opener()),"replacement fixture retained");
        e.headers[0].address=0x2000;e.headers[0].references=1; // native teardown replaced the node
        Check(!leases.Release(e.Reader(),e.Closer())&&e.closes==0&&e.headers[0].references==1,"cleanup never decrements a replacement header");
        Check(leases.Retain(e.headers[0].path,e.Reader(),e.Opener())&&e.opens==2,"replacement can acquire a fresh owned lease");
        leases.Release(e.Reader(),e.Closer());
        Check(e.headers[0].references==1,"fresh replacement lease releases only its own reference");
    }
    {
        FakeEngine e;e.Add("CARS\\A\\GEOMETRY.BIN",0x1000);e.rejectOpen=true;
        HeaderLeases<1> leases;
        Check(!leases.Retain(e.headers[0].path,e.Reader(),e.Opener())&&!leases.Size()&&e.headers[0].references==1,"failed acquisition retains no ownership");
        e.rejectOpen=false;e.headers[0].references=0;
        Check(!leases.Retain(e.headers[0].path,e.Reader(),e.Opener())&&e.opens==0,"already retired header is never resurrected");
    }
    {
        FakeEngine e;e.Add("CARS\\A\\GEOMETRY.BIN",0x1000);e.failReadAfterOpen=true;
        HeaderLeases<1> leases;
        leases.Retain(e.headers[0].path,e.Reader(),e.Opener());
        Check(leases.Size()==1&&e.opens==1,"successful acquire stays tracked if verification read fails");
        e.failReadAfterOpen=false;
        Check(leases.Retain(e.headers[0].path,e.Reader(),e.Opener())&&e.opens==1,"retry cannot duplicate an acquired but unreadable lease");
        leases.Release(e.Reader(),e.Closer());
        Check(e.headers[0].references==1,"temporarily unreadable lease remains releasable");
    }
    {
        FakeEngine e;e.Add("CARS\\A\\GEOMETRY.BIN",0x1000);e.Add("CARS\\B\\GEOMETRY.BIN",0x2000);
        HeaderLeases<2> leases;
        leases.Retain(e.headers[0].path,e.Reader(),e.Opener());leases.Retain(e.headers[1].path,e.Reader(),e.Opener());
        Check(leases.Drop("cars/a/geometry.bin",e.Reader(),e.Closer()),"callback close first drops only matching owned lease");
        Check(leases.Size()==1&&e.headers[0].references==1&&e.headers[1].references==2,"callback close leaves unrelated leases untouched");
        e.Close(e.headers[0].path); // original callback-bearing native close now reaches zero
        Check(!e.headers[0].address,"original callback close reaches its unmodified final-reference condition");
        Check(!leases.Drop("CARS\\A\\GEOMETRY.BIN",e.Reader(),e.Closer()),"second drop does not touch already retired header");
        leases.Release(e.Reader(),e.Closer());
    }
    {
        const uint32_t keys[]={0,11,12,13,14};
        const auto r=Inspect(keys,5,[](uint32_t k){return k==11?0u:(k==12?2u:1u);});
        Check(r.indexed==3&&r.resident==1&&r.missing==2&&r.firstMissing==13,"residency distinguishes optional hashes, drawable meshes and indexed pending data");
        const auto complete=Inspect(keys,5,[](uint32_t k){return k==11?0u:2u;});
        Check(complete.indexed==3&&complete.resident==3&&!complete.missing&&!complete.firstMissing,"absent optional LODs never block completed native streaming");
    }
    std::printf("Geometry header leases: %u checks passed.\n",checks);
}
