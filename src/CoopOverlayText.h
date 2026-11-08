// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <iterator>
#include <cwchar>
#pragma comment(lib,"d2d1.lib")
#pragma comment(lib,"dwrite.lib")

// A private CPU-backed surface: never changes the game's D3D device, COM
// apartment, installed fonts, or process DPI awareness.
namespace coop_overlay {
using Microsoft::WRL::ComPtr;
template<class Interface> class Object : public Interface {
    std::atomic<ULONG> references{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(id!=__uuidof(IUnknown)&&id!=__uuidof(Interface))return E_NOINTERFACE;
        *out=static_cast<Interface*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override {const ULONG n=--references;if(!n)delete this;return n;}
    virtual ~Object()=default;
};
class FontStream final : public Object<IDWriteFontFileStream> {
    const BYTE* bytes;UINT32 size;
public:
    FontStream(const BYTE* data,UINT32 length):bytes(data),size(length){}
    HRESULT STDMETHODCALLTYPE ReadFileFragment(const void** out,UINT64 offset,UINT64 length,void** context) override {
        if(!out||!context)return E_POINTER;*out=nullptr;*context=nullptr;
        if(offset>size||length>size-offset)return E_FAIL;*out=bytes+size_t(offset);return S_OK;
    }
    void STDMETHODCALLTYPE ReleaseFileFragment(void*) override{}
    HRESULT STDMETHODCALLTYPE GetFileSize(UINT64* out) override{if(!out)return E_POINTER;*out=size;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetLastWriteTime(UINT64* out) override{if(!out)return E_POINTER;*out=0;return S_OK;}
};
class FontLoader final : public Object<IDWriteFontFileLoader> {
    ComPtr<IDWriteFontFileStream> stream;
public:
    FontLoader(const BYTE* data,UINT32 size){stream.Attach(new FontStream(data,size));}
    HRESULT STDMETHODCALLTYPE CreateStreamFromKey(const void* key,UINT32 size,IDWriteFontFileStream** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!key||size!=sizeof(UINT32)||*static_cast<const UINT32*>(key)!=0)return E_INVALIDARG;
        return stream.CopyTo(out);
    }
};
class FontEnumerator final : public Object<IDWriteFontFileEnumerator> {
    ComPtr<IDWriteFactory> factory;ComPtr<IDWriteFontFileLoader> loader;ComPtr<IDWriteFontFile> file;
    bool visited=false;
public:
    FontEnumerator(IDWriteFactory* f,IDWriteFontFileLoader* l):factory(f),loader(l){}
    HRESULT STDMETHODCALLTYPE MoveNext(BOOL* out) override {
        if(!out)return E_POINTER;*out=FALSE;if(visited){file.Reset();return S_OK;}visited=true;
        const UINT32 key=0;const HRESULT hr=factory->CreateCustomFontFileReference(&key,sizeof(key),loader.Get(),&file);
        if(SUCCEEDED(hr))*out=TRUE;return hr;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentFontFile(IDWriteFontFile** out) override {
        if(!out)return E_POINTER;*out=nullptr;return file?file.CopyTo(out):E_FAIL;
    }
};
class CollectionLoader final : public Object<IDWriteFontCollectionLoader> {
    ComPtr<IDWriteFontFileLoader> loader;
public:
    explicit CollectionLoader(IDWriteFontFileLoader* value):loader(value){}
    HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(IDWriteFactory* factory,const void* key,UINT32 size,IDWriteFontFileEnumerator** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!factory||!key||size!=sizeof(UINT32)||*static_cast<const UINT32*>(key)!=0)return E_INVALIDARG;
        *out=new FontEnumerator(factory,loader.Get());return S_OK;
    }
};
class Renderer {
    ComPtr<IDWriteFactory> write;ComPtr<ID2D1Factory> draw;
    ComPtr<IDWriteFontFileLoader> fileLoader;ComPtr<IDWriteFontCollectionLoader> collectionLoader;
    ComPtr<IDWriteFontCollection> collection;
    ComPtr<IDWriteRenderingParams> rendering;
    ComPtr<ID2D1DCRenderTarget> target;ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteTextFormat> body,title,build;
    bool fileRegistered=false,collectionRegistered=false,attempted=false;
    HRESULT error=S_OK;wchar_t family[128]{};
public:
    HRESULT Error()const{return error;}
    const wchar_t* Family()const{return family;}
    ~Renderer(){Shutdown();}
    void Shutdown() {
        brush.Reset();target.Reset();body.Reset();title.Reset();build.Reset();collection.Reset();rendering.Reset();
        if(write&&collectionRegistered)write->UnregisterFontCollectionLoader(collectionLoader.Get());
        if(write&&fileRegistered)write->UnregisterFontFileLoader(fileLoader.Get());
        collectionRegistered=fileRegistered=false;collectionLoader.Reset();fileLoader.Reset();draw.Reset();write.Reset();
        attempted=false;family[0]=0;
    }
    bool Initialize(HMODULE module) {
        if(attempted)return write&&body&&title&&build&&rendering;attempted=true;
        const HRSRC r=module?FindResourceW(module,MAKEINTRESOURCEW(302),MAKEINTRESOURCEW(10)):nullptr;
        const HGLOBAL loaded=r?LoadResource(module,r):nullptr;const DWORD size=r?SizeofResource(module,r):0;
        const BYTE* bytes=loaded?static_cast<const BYTE*>(LockResource(loaded)):nullptr;
        error=E_FAIL;if(!bytes||!size)return false;
        if(FAILED(error=DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.GetAddressOf()))))return false;
        if(FAILED(error=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,draw.GetAddressOf())))return false;
        fileLoader.Attach(new FontLoader(bytes,size));collectionLoader.Attach(new CollectionLoader(fileLoader.Get()));
        if(FAILED(error=write->RegisterFontFileLoader(fileLoader.Get())))return false;fileRegistered=true;
        if(FAILED(error=write->RegisterFontCollectionLoader(collectionLoader.Get())))return false;collectionRegistered=true;
        const UINT32 key=0;
        if(FAILED(error=write->CreateCustomFontCollection(collectionLoader.Get(),&key,sizeof(key),&collection)))return false;
        ComPtr<IDWriteFontFamily> font;ComPtr<IDWriteLocalizedStrings> names;
        if(FAILED(error=collection->GetFontFamily(0,&font))||FAILED(error=font->GetFamilyNames(&names))||
           FAILED(error=names->GetString(0,family,UINT32(std::size(family)))))return false;
        auto format=[&](float size,ComPtr<IDWriteTextFormat>& out) {
            return write->CreateTextFormat(family,collection.Get(),DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,size,L"en-us",&out);
        };
        if(FAILED(error=format(16,body))||FAILED(error=format(17,title))||FAILED(error=format(16,build)))return false;
        // Grayscale alpha is required on transparent layered surfaces; ClearType
        // RGB fringes cannot be composited correctly over arbitrary game pixels.
        error=write->CreateCustomRenderingParams(2.2f,1.0f,0.0f,DWRITE_PIXEL_GEOMETRY_FLAT,
            DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,&rendering);
        return SUCCEEDED(error);
    }
    bool Begin(HDC dc,int width,int height,float dpi) {
        if(!write||!body||width<=0||height<=0||width>8192||height>8192||dpi<48||dpi>768)return false;
        if(!target) {
            const auto p=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpi,dpi);
            if(FAILED(error=draw->CreateDCRenderTarget(&p,&target)))return false;
            if(FAILED(error=target->CreateSolidColorBrush(D2D1::ColorF(1,1,1),&brush))){target.Reset();return false;}
        }
        const RECT rect{0,0,width,height};if(FAILED(error=target->BindDC(dc,&rect)))return false;
        target->SetDpi(dpi,dpi);target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);target->SetTextRenderingParams(rendering.Get());
        target->BeginDraw();target->Clear(D2D1::ColorF(0,0,0,0));return true;
    }
    bool End(){error=target->EndDraw();if(FAILED(error)){brush.Reset();target.Reset();}return SUCCEEDED(error);}
    void Rectangle(float x,float y,float width,float height,unsigned alpha,unsigned gray) {
        brush->SetColor(D2D1::ColorF(gray/255.0f,gray/255.0f,gray/255.0f,alpha/255.0f));
        target->FillRectangle(D2D1::RectF(x,y,x+width,y+height),brush.Get());
    }
    ComPtr<IDWriteTextLayout> Layout(const wchar_t* text,float width,float height,int style=0,bool wrap=true) {
        ComPtr<IDWriteTextLayout> layout;if(!text||width<=0||height<=0)return layout;
        auto* format=style==1?title.Get():style==2?build.Get():body.Get();
        error=write->CreateTextLayout(text,UINT32(std::wcslen(text)),format,width,height,&layout);
        if(SUCCEEDED(error))layout->SetWordWrapping(wrap?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP);
        return layout;
    }
    DWRITE_TEXT_METRICS Measure(const wchar_t* text,float width=10000,int style=0,bool wrap=true) {
        DWRITE_TEXT_METRICS m{};auto layout=Layout(text,width,10000,style,wrap);if(layout)layout->GetMetrics(&m);return m;
    }
    ComPtr<IDWriteTextLayout> LabelLayout(const wchar_t* text,float size) {
        auto layout=Layout(text,2048,256,0,false);
        if(layout&&(size<4||size>128||FAILED(layout->SetFontSize(size,{0,UINT32(std::wcslen(text))}))))layout.Reset();
        return layout;
    }
    void Label(IDWriteTextLayout* layout,float x,float y) {
        if(!layout)return;
        // One shared layout, grayscale antialiased alpha, a small dark outline
        // for legibility against headlights/sky. There is no panel/background.
        brush->SetColor(D2D1::ColorF(0.02f,0.02f,0.02f,0.9f));
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)if(dx||dy)
            target->DrawTextLayout(D2D1::Point2F(x+dx,y+dy),layout,brush.Get());
        brush->SetColor(D2D1::ColorF(0.94f,0.97f,0.94f,1));
        target->DrawTextLayout(D2D1::Point2F(x,y),layout,brush.Get());
    }
    void Text(const wchar_t* text,float x,float y,float width,float height,unsigned gray,unsigned alpha=255,int style=0,bool wrap=true) {
        auto layout=Layout(text,width,height,style,wrap);if(!layout)return;
        brush->SetColor(D2D1::ColorF(gray/255.0f,gray/255.0f,gray/255.0f,alpha/255.0f));
        target->PushAxisAlignedClip(D2D1::RectF(x,y,x+width,y+height),D2D1_ANTIALIAS_MODE_ALIASED);
        target->DrawTextLayout(D2D1::Point2F(x,y),layout.Get(),brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        target->PopAxisAlignedClip();
    }
    void ColoredText(const wchar_t* text,float x,float y,float width,float height,unsigned rgb,float fontSize=14,bool fit=false) {
        auto layout=Layout(text,width,height,0,false);if(!layout)return;
        const DWRITE_TEXT_RANGE range{0,UINT32(std::wcslen(text))};layout->SetFontSize(fontSize,range);
        if(fit){DWRITE_TEXT_METRICS metrics{};if(SUCCEEDED(layout->GetMetrics(&metrics))&&metrics.widthIncludingTrailingWhitespace>width)
            layout->SetFontSize((std::max)(9.0f,fontSize*width/metrics.widthIncludingTrailingWhitespace),range);}
        target->PushAxisAlignedClip(D2D1::RectF(x-1,y-1,x+width+1,y+height+1),D2D1_ANTIALIAS_MODE_ALIASED);
        brush->SetColor(D2D1::ColorF(0.015f,0.02f,0.025f,0.95f));
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)if(dx||dy)
            target->DrawTextLayout(D2D1::Point2F(x+dx,y+dy),layout.Get(),brush.Get());
        brush->SetColor(D2D1::ColorF(rgb,1.0f));
        target->DrawTextLayout(D2D1::Point2F(x,y),layout.Get(),brush.Get());
        target->PopAxisAlignedClip();
    }
};
// Only our two owned top-level overlays are per-monitor aware. The game's
// thread context is restored before returning, including every failure path.
class DpiScope {
    using Set=HANDLE(WINAPI*)(HANDLE);Set set=nullptr;HANDLE previous=nullptr;
public:
    explicit DpiScope(HANDLE context=reinterpret_cast<HANDLE>(INT_PTR(-4))) {
        set=reinterpret_cast<Set>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetThreadDpiAwarenessContext"));
        if(set){previous=set(context);if(!previous&&context==reinterpret_cast<HANDLE>(INT_PTR(-4)))previous=set(reinterpret_cast<HANDLE>(INT_PTR(-3)));}
    }
    ~DpiScope(){if(set&&previous)set(previous);}
};
inline float WindowDpi(HWND window) {
    using Get=UINT(WINAPI*)(HWND);static auto get=reinterpret_cast<Get>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
    const UINT dpi=get?get(window):96;return float(dpi?dpi:96);
}
inline bool PhysicalClient(HWND window,RECT& result) {
    using Context=HANDLE(WINAPI*)(HWND);using Convert=BOOL(WINAPI*)(HWND,LPPOINT);
    const auto user=GetModuleHandleW(L"user32.dll");
    const auto context=reinterpret_cast<Context>(GetProcAddress(user,"GetWindowDpiAwarenessContext"));
    const auto convert=reinterpret_cast<Convert>(GetProcAddress(user,"LogicalToPhysicalPointForPerMonitorDPI"));
    // Obtain logical screen coordinates in the OWNER's space, then convert
    // using that owner's transform. Never mix virtualized and physical points.
    DpiScope scope(context?context(window):reinterpret_cast<HANDLE>(INT_PTR(-1)));
    RECT client{};if(!GetClientRect(window,&client))return false;
    POINT first{client.left,client.top},last{client.right,client.bottom};
    if(!ClientToScreen(window,&first)||!ClientToScreen(window,&last))return false;
    if(convert&&(!convert(window,&first)||!convert(window,&last)))return false;
    result={first.x,first.y,last.x,last.y};return true;
}
}
