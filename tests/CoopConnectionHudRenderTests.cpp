// Copyright (C) 2026 batuhan.
// GNU GPL version 3 only, with LICENSE-7ZIP-EXCEPTION.txt.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/CoopConnectionHudOverlay.inl"
#include <wincodec.h>
#include <cstdlib>
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"windowscodecs.lib")
using Microsoft::WRL::ComPtr;
static unsigned checks=0;
static void Check(bool value,const char* name){++checks;if(!value){std::fprintf(stderr,"FAIL: %s\n",name);std::exit(1);}}
static bool Save(const wchar_t* name,BYTE* pixels,UINT width,UINT height){
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))||
       FAILED(factory->CreateStream(&stream))||FAILED(stream->InitializeFromFilename(name,GENERIC_WRITE))||
       FAILED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))||FAILED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))||
       FAILED(encoder->CreateNewFrame(&frame,nullptr))||FAILED(frame->Initialize(nullptr))||FAILED(frame->SetSize(width,height)))return false;
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(frame->SetPixelFormat(&format))&&SUCCEEDED(frame->WritePixels(height,width*4,width*height*4,pixels))&&
        SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit());
}
int main(){
    Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialization");
    CoopConnectionHud::Initialize(GetModuleHandleW(nullptr));
    const char* labels[]={"Connected | 35 ms","Connected | 160 ms","Connected | 350 ms","Measuring ping...","Connection interrupted","Teammate disconnected"};
    const unsigned colors[]={coop_connection_hud::kGreen,coop_connection_hud::kOrange,coop_connection_hud::kRed,coop_connection_hud::kOrange,coop_connection_hud::kRed,coop_connection_hud::kRed};
    for(const int dpi:{96,144,192}){
        const int width=MulDiv(360,dpi,96),height=MulDiv(32,dpi,96);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        HDC dc=CreateCompatibleDC(nullptr);BYTE* pixels=nullptr;
        HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
        Check(dc&&bitmap&&pixels,"offscreen surface");auto old=SelectObject(dc,bitmap);
        for(unsigned row=0;row<6;++row){
            coop_connection_hud::View view;std::snprintf(view.text,sizeof(view.text),"%s",labels[row]);view.rgb=colors[row];view.visible=true;
            Check(CoopConnectionHud::Draw(dc,width,height,view,float(dpi)),"real embedded-font draw succeeds");GdiFlush();
            Check(pixels[3]==0,"transparent background remains empty");
            bool found=false;for(int i=0;i<width*height;++i){const auto* p=pixels+i*4;
                if(p[3]>240&&std::abs(int(p[2])-int((view.rgb>>16)&255))<12&&std::abs(int(p[1])-int((view.rgb>>8)&255))<12&&std::abs(int(p[0])-int(view.rgb&255))<12){found=true;break;}}
            Check(found,"visible text uses requested quality color");
            wchar_t text[80]{};MultiByteToWideChar(CP_UTF8,0,view.text,-1,text,80);
            auto layout=CoopConnectionHud::renderer.Layout(text,354,28,0,false);DWRITE_TEXT_METRICS metrics{};
            Check(layout&&SUCCEEDED(layout->SetFontSize(14,{0,UINT32(std::wcslen(text))}))&&SUCCEEDED(layout->GetMetrics(&metrics))&&metrics.widthIncludingTrailingWhitespace<=354,"status fits viewport");
            if(dpi==96){wchar_t path[80]{};std::swprintf(path,80,L"output/connection-hud-%u.png",row);Check(Save(path,pixels,width,height),"PNG preview saved");}
        }
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
    for(const int dpi:{96,144,192})for(const int logicalWidth:{216,360}){
        const int width=MulDiv(logicalWidth,dpi,96),height=MulDiv(48,dpi,96);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        HDC dc=CreateCompatibleDC(nullptr);BYTE* pixels=nullptr;
        HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
        Check(dc&&bitmap&&pixels,"activity offscreen surface");auto old=SelectObject(dc,bitmap);
        coop_connection_hud::Model hud;coop::SessionSnapshot session;session.sessionId=42;session.joined=session.fresh=true;
        coop_transport::Statistics wire;wire.ready=wire.connected=wire.rttMeasured=true;wire.rttMs=60;wire.rttSampleAtMs=100;
        for(unsigned row=0;row<unsigned(coop_activity::Scene::Count);++row){
            const auto scene=coop_activity::Scene(row);
            const auto view=hud.Update(session,wire,true,true,101,{scene,coop_activity::HasRace(scene)?std::uint8_t(9):std::uint8_t(0)});
            Check(CoopConnectionHud::Draw(dc,width,height,view,float(dpi)),"activity draw succeeds at narrow and normal widths");GdiFlush();
            bool secondLine=false;for(int y=MulDiv(24,dpi,96);y<height;++y)for(int x=0;x<width;++x)if(pixels[(y*width+x)*4+3])secondLine=true;
            Check(secondLine,"teammate activity is visible below ping");
            bool edge=false;for(int x=0;x<width;++x)if(pixels[((height-1)*width+x)*4+3])edge=true;
            Check(!edge,"activity stays inside transparent viewport");
            if(dpi==96&&logicalWidth==360&&(scene==coop_activity::Scene::CarLot||scene==coop_activity::Scene::Race||scene==coop_activity::Scene::AtDestination)){
                wchar_t path[96]{};std::swprintf(path,96,L"output/connection-activity-%u.png",row);Check(Save(path,pixels,width,height),"activity PNG preview saved");
            }
        }
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
    Check(CoopConnectionHud::Procedure(nullptr,WM_NCHITTEST,0,0)==HTTRANSPARENT,"HUD never captures mouse hit tests");
    Check(CoopConnectionHud::Procedure(nullptr,WM_MOUSEACTIVATE,0,0)==MA_NOACTIVATE,"HUD never steals focus");
    std::printf("Connection HUD render: %u checks passed; no visible windows.\n",checks);
    CoUninitialize();
}
