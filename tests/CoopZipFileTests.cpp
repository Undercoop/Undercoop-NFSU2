// Copyright (C) 2026 batuhan. GNU GPL version 3 only.
// Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
#include "../src/CoopZipFileWin.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>

static unsigned checks=0;
static void Check(bool value,const char* expression,int line){++checks;if(!value){std::printf("FAIL %d %s (Win32 %lu)\n",line,expression,GetLastError());std::exit(1);}}
#define CHECK(...) Check(bool((__VA_ARGS__)),#__VA_ARGS__,__LINE__)
static void U16(std::string& b,uint16_t n){b+=char(n);b+=char(n>>8);}
static void U32(std::string& b,uint32_t n){U16(b,uint16_t(n));U16(b,uint16_t(n>>16));}
static void Set16(std::string& b,size_t at,uint16_t n){b[at]=char(n);b[at+1]=char(n>>8);}
static void Set32(std::string& b,size_t at,uint32_t n){Set16(b,at,uint16_t(n));Set16(b,at+2,uint16_t(n>>16));}
struct Item{std::string name,data,encoded;bool descriptor=false;uint32_t attributes=0;};
static std::string Zip(const std::vector<Item>& items){
    std::string b,central;
    for(const auto& i:items){
        const uint32_t local=uint32_t(b.size()),crc=coop_zip::detail::Crc(i.data.data(),i.data.size());
        const auto& payload=i.encoded.empty()?i.data:i.encoded;const uint16_t method=i.encoded.empty()?0:8,flags=i.descriptor?8:0;
        U32(b,0x04034B50);U16(b,20);U16(b,flags);U16(b,method);U16(b,0);U16(b,0);
        U32(b,i.descriptor?0:crc);U32(b,i.descriptor?0:uint32_t(payload.size()));U32(b,i.descriptor?0:uint32_t(i.data.size()));
        U16(b,uint16_t(i.name.size()));U16(b,0);b+=i.name;b+=payload;
        if(i.descriptor){U32(b,0x08074B50);U32(b,crc);U32(b,uint32_t(payload.size()));U32(b,uint32_t(i.data.size()));}
        U32(central,0x02014B50);U16(central,20);U16(central,20);U16(central,flags);U16(central,method);U16(central,0);U16(central,0);
        U32(central,crc);U32(central,uint32_t(payload.size()));U32(central,uint32_t(i.data.size()));
        U16(central,uint16_t(i.name.size()));U16(central,0);U16(central,0);U16(central,0);U16(central,0);U32(central,i.attributes);U32(central,local);central+=i.name;
    }
    const uint32_t at=uint32_t(b.size());b+=central;
    U32(b,0x06054B50);U16(b,0);U16(b,0);U16(b,uint16_t(items.size()));U16(b,uint16_t(items.size()));U32(b,uint32_t(central.size()));U32(b,at);U16(b,0);
    return b;
}
class Fixture{
    std::wstring root_,path_;
public:
    Fixture(){wchar_t temp[MAX_PATH]{};CHECK(GetTempPathW(MAX_PATH,temp));root_=std::wstring(temp)+L"UndercoopZipFile-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());CHECK(CreateDirectoryW(root_.c_str(),nullptr));path_=root_+L"\\fixture.zip";}
    ~Fixture(){CHECK(path_==root_+L"\\fixture.zip");if(GetFileAttributesW(path_.c_str())!=INVALID_FILE_ATTRIBUTES)CHECK(DeleteFileW(path_.c_str()));CHECK(RemoveDirectoryW(root_.c_str()));}
    void Write(const std::string& bytes){HANDLE file=CreateFileW(path_.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);DWORD n=0;CHECK(WriteFile(file,bytes.data(),DWORD(bytes.size()),&n,nullptr)&&n==bytes.size());CHECK(CloseHandle(file));}
    HANDLE Read(){const auto f=CreateFileW(path_.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(f!=INVALID_HANDLE_VALUE);return f;}
};
static void Reject(const std::string& bytes){Fixture f;f.Write(bytes);HANDLE file=f.Read();coop_zip_file::Archive a;std::string error;CHECK(!a.Open(file,error));CHECK(!error.empty());CHECK(a.Entries().empty());CHECK(CloseHandle(file));}
static void Synthetic(){
    const auto base=Zip({{"global/a.bin","first",{}},{"scripts/b.ini","[x]\ny=1\n",{}}});
    {Fixture f;f.Write(base);const auto file=f.Read();coop_zip_file::Archive a;std::string e,out="old";CHECK(a.Open(file,e));CHECK(a.Entries().size()==2);CHECK(a.Extract(0,out,e)&&out=="first");CHECK(a.Extract(1,out,e)&&out=="[x]\ny=1\n");CHECK(!a.Extract(2,out,e));CHECK(out=="[x]\ny=1\n");CHECK(CloseHandle(file));}
    const std::string rawDeflate("\xCB\x48\xCD\xC9\xC9\x07\x00",7);
    for(bool descriptor:{false,true}){Fixture f;f.Write(Zip({{"file.bin","hello",rawDeflate,descriptor}}));const auto file=f.Read();coop_zip_file::Archive a;std::string e,out;CHECK(a.Open(file,e));CHECK(a.Extract(0,out,e)&&out=="hello");CHECK(CloseHandle(file));}
    {Fixture f;auto unsignedDescriptor=Zip({{"file.bin","hello",rawDeflate,true}});const size_t descriptor=30+8+rawDeflate.size();unsignedDescriptor.erase(descriptor,4);Set32(unsignedDescriptor,unsignedDescriptor.size()-6,uint32_t(descriptor+12));f.Write(unsignedDescriptor);const auto file=f.Read();coop_zip_file::Archive a;std::string e,out;CHECK(a.Open(file,e));CHECK(a.Extract(0,out,e)&&out=="hello");CHECK(CloseHandle(file));}
    Reject("not a zip");Reject(base.substr(0,base.size()-1));Reject(base+"trailer");
    for(const auto* name:{"../escape.bin","/root.bin","C:/root.bin","a/CON.ini","a./x.bin","a//x.bin"})Reject(Zip({{name,"x",{}}}));
    Reject(Zip({{"A.bin","x",{}},{"a.bin","x",{}}}));
    Reject(Zip({{"same","x",{}},{"same/child.bin","x",{}}}));
    Reject(Zip({{"link","x",{},false,uint32_t(0120777)<<16}}));
    auto bad=Zip({{"a.bin","hello",{}}});const size_t center=bad.find("PK\1\2",0,4);
    Set16(bad,6,1);Set16(bad,center+8,1);Reject(bad);
    bad=Zip({{"a.bin","hello",{}}});Set16(bad,8,99);Set16(bad,center+10,99);Reject(bad);
    bad=Zip({{"a.bin","hello",{}}});bad[30]='b';Reject(bad);
    bad=Zip({{"a.bin","hello",{}}});Set32(bad,center+24,uint32_t(coop_zip_file::kMaxFile+1));Reject(bad);
    bad=Zip({{"a.bin","hello",{}}});Set16(bad,bad.size()-22+8,0xFFFF);Set16(bad,bad.size()-22+10,0xFFFF);Reject(bad);
    bad=Zip({{"a.bin","hello",{}}});Set32(bad,center+42,1);Reject(bad);
    // Both local headers are individually valid, but the inner file overlaps
    // the first file's payload. Bounds/CRC metadata alone cannot permit it.
    {const auto inner=Zip({{"inner.bin","nested",{}}});const size_t innerCenter=coop_zip::detail::U32(inner,inner.size()-6);const auto outer=Zip({{"outer.bin",inner.substr(0,innerCenter),{}}});const size_t outerCenter=coop_zip::detail::U32(outer,outer.size()-6);std::string central=outer.substr(outerCenter,outer.size()-22-outerCenter);auto innerDirectory=inner.substr(innerCenter,inner.size()-22-innerCenter);Set32(innerDirectory,42,30+9);central+=innerDirectory;std::string overlap=outer.substr(0,outerCenter)+central;U32(overlap,0x06054B50);U16(overlap,0);U16(overlap,0);U16(overlap,2);U16(overlap,2);U32(overlap,uint32_t(central.size()));U32(overlap,uint32_t(outerCenter));U16(overlap,0);Reject(overlap);}
    bad=Zip({{"a.bin","hello",{}}});bad.insert(center,1,'x');Set32(bad,bad.size()-6,uint32_t(center+1));Reject(bad);
    {std::vector<Item> items;for(unsigned i=0;i<17;++i)items.push_back({"part"+std::to_string(i)+".bin","x",std::string(1,0)});bad=Zip(items);size_t at=coop_zip::detail::U32(bad,bad.size()-6);for(unsigned i=0;i<17;++i){const auto local=coop_zip::detail::U32(bad,at+42);Set32(bad,local+22,uint32_t(coop_zip_file::kMaxFile));Set32(bad,at+24,uint32_t(coop_zip_file::kMaxFile));at+=46+coop_zip::detail::U16(bad,at+28);}Reject(bad);}
    {Fixture f;bad=Zip({{"a.bin","hello",{}}});bad[35]='x';f.Write(bad);const auto file=f.Read();coop_zip_file::Archive a;std::string e,out="unchanged";CHECK(a.Open(file,e));CHECK(!a.Extract(0,out,e));CHECK(out=="unchanged");CHECK(CloseHandle(file));}
    {Fixture f;f.Write(base);const auto file=f.Read();std::atomic<bool> cancel{true};coop_zip_file::Archive a;std::string e;CHECK(!a.Open(file,e,&cancel));cancel=false;CHECK(a.Open(file,e,&cancel));cancel=true;std::string out="unchanged";CHECK(!a.Extract(0,out,e));CHECK(out=="unchanged");CHECK(CloseHandle(file));}
}
static void RealArchive(const wchar_t* path){
    if(!path)return;
    const auto file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
    coop_zip_file::Archive a;std::string e;CHECK(a.Open(file,e));CHECK(a.Entries().size()==1004);
    std::array<size_t,3> chosen{{SIZE_MAX,SIZE_MAX,SIZE_MAX}};
    for(const auto& v:a.Entries()){
        if(v.path.find("TexturePacks/underground2.net/TEXTURES.BIN")!=std::string::npos)chosen[0]=v.index;
        if(v.path.find("SCRIPTS/AnimationController.asi")!=std::string::npos)chosen[1]=v.index;
        if(v.path.find("/Readme.txt")!=std::string::npos)chosen[2]=v.index;
    }
    uint64_t total=0;for(const auto index:chosen){CHECK(index!=SIZE_MAX);std::string out;CHECK(a.Extract(index,out,e));CHECK(out.size()==a.Entries()[index].bytes);total+=out.size();}
    std::printf("Real archive: %zu entries, extracted %llu bytes one entry at a time; no installation.\n",a.Entries().size(),static_cast<unsigned long long>(total));CHECK(CloseHandle(file));
}
int wmain(int argc,wchar_t** argv){Synthetic();RealArchive(argc>1?argv[1]:nullptr);std::printf("Streaming ZIP checks: %u passed.\n",checks);return 0;}
