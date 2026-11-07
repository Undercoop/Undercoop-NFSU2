// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopOverlayText.h"
#include <wincodec.h>
#include <shlwapi.h>
#include <array>
#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"shlwapi.lib")

// Launcher-only rendering. Console/labels keep their existing renderer and
// resources. All fonts are private embedded streams, never system-installed.
namespace coop_manager_graphics {
using Microsoft::WRL::ComPtr;
class Files final:public coop_overlay::Object<IDWriteFontFileLoader>{
    std::array<ComPtr<IDWriteFontFileStream>,4> streams;
public:
    bool Load(HMODULE module){
        for(UINT32 i=0;i<4;++i){auto resource=FindResourceW(module,MAKEINTRESOURCEW(311+i),MAKEINTRESOURCEW(10));
            auto data=resource?LoadResource(module,resource):nullptr;const DWORD size=resource?SizeofResource(module,resource):0;
            const auto* bytes=data?static_cast<const BYTE*>(LockResource(data)):nullptr;
            if(!bytes||!size)return false;streams[i].Attach(new coop_overlay::FontStream(bytes,size));}
        return true;
    }
    HRESULT STDMETHODCALLTYPE CreateStreamFromKey(const void* key,UINT32 size,IDWriteFontFileStream** out)override{
        if(!out)return E_POINTER;*out=nullptr;
        if(!key||size!=sizeof(UINT32)||*static_cast<const UINT32*>(key)>=4)return E_INVALIDARG;
        return streams[*static_cast<const UINT32*>(key)].CopyTo(out);
    }
};
class Enumerator final:public coop_overlay::Object<IDWriteFontFileEnumerator>{
    ComPtr<IDWriteFactory> factory;ComPtr<IDWriteFontFileLoader> loader;ComPtr<IDWriteFontFile> current;UINT32 index=0;
public:
    Enumerator(IDWriteFactory* f,IDWriteFontFileLoader* l):factory(f),loader(l){}
    HRESULT STDMETHODCALLTYPE MoveNext(BOOL* out)override{
        if(!out)return E_POINTER;*out=FALSE;current.Reset();if(index>=4)return S_OK;
        const UINT32 key=index++;const auto hr=factory->CreateCustomFontFileReference(&key,sizeof(key),loader.Get(),&current);
        if(SUCCEEDED(hr))*out=TRUE;return hr;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentFontFile(IDWriteFontFile** out)override{if(!out)return E_POINTER;*out=nullptr;return current?current.CopyTo(out):E_FAIL;}
};
class Collection final:public coop_overlay::Object<IDWriteFontCollectionLoader>{
    ComPtr<IDWriteFontFileLoader> loader;
public:
    explicit Collection(IDWriteFontFileLoader* value):loader(value){}
    HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(IDWriteFactory* f,const void* key,UINT32 size,IDWriteFontFileEnumerator** out)override{
        if(!out)return E_POINTER;*out=nullptr;if(!key||size!=sizeof(UINT32))return E_INVALIDARG;
        *out=new Enumerator(f,loader.Get());return S_OK;
    }
};
struct ComScope{
    HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ~ComScope(){if(SUCCEEDED(result))CoUninitialize();}
    bool Ready()const{return SUCCEEDED(result)||result==RPC_E_CHANGED_MODE;}
};
enum class Face{Regular,Bold,Italic,BoldItalic,Heading};
class Renderer{
    ComPtr<IDWriteFactory> write;ComPtr<ID2D1Factory> draw;
    ComPtr<IDWriteFontFileLoader> files,headingFile;ComPtr<IDWriteFontCollectionLoader> collectionLoader,headingLoader;
    ComPtr<IDWriteFontCollection> collection,headingCollection;ComPtr<IDWriteRenderingParams> rendering;
    ComPtr<ID2D1DCRenderTarget> target;ComPtr<ID2D1SolidColorBrush> brush;ComPtr<ID2D1Bitmap> logo;
    ComPtr<IWICFormatConverter> logoPixels;
    std::array<ComPtr<IDWriteTextFormat>,5> formats;
    bool filesRegistered=false,collectionRegistered=false,headingFileRegistered=false,headingRegistered=false,attempted=false;
    HRESULT error=E_FAIL;wchar_t family[128]{},heading[128]{};UINT logoWidth=0,logoHeight=0;
    static bool Family(IDWriteFontCollection* source,wchar_t* name,UINT size){
        ComPtr<IDWriteFontFamily> value;ComPtr<IDWriteLocalizedStrings> names;
        return source&&SUCCEEDED(source->GetFontFamily(0,&value))&&SUCCEEDED(value->GetFamilyNames(&names))&&SUCCEEDED(names->GetString(0,name,size));
    }
    void Color(unsigned rgb,float alpha=1){brush->SetColor(D2D1::ColorF(rgb,alpha));}
public:
    ~Renderer(){Shutdown();}
    HRESULT Error()const{return error;}
    const wchar_t* Family()const{return family;}
    const wchar_t* HeadingFamily()const{return heading;}
    UINT BodyFaces()const{
        ComPtr<IDWriteFontFamily> f;UINT real=0;if(!collection||FAILED(collection->GetFontFamily(0,&f)))return 0;
        // DirectWrite can expose synthetic oblique faces alongside the four
        // actual embedded files. Count only real faces for resource checks.
        for(UINT i=0;i<f->GetFontCount();++i){ComPtr<IDWriteFont> font;
            if(SUCCEEDED(f->GetFont(i,&font))&&font->GetSimulations()==DWRITE_FONT_SIMULATIONS_NONE)++real;}
        return real;
    }
    bool HasFace(DWRITE_FONT_WEIGHT weight,DWRITE_FONT_STYLE style)const{
        ComPtr<IDWriteFontFamily> f;ComPtr<IDWriteFont> font;
        return collection&&SUCCEEDED(collection->GetFontFamily(0,&f))&&SUCCEEDED(f->GetFirstMatchingFont(weight,DWRITE_FONT_STRETCH_NORMAL,style,&font))&&
            font->GetWeight()==weight&&font->GetStyle()==style&&font->GetSimulations()==DWRITE_FONT_SIMULATIONS_NONE;
    }
    void Shutdown(){
        logo.Reset();logoPixels.Reset();brush.Reset();target.Reset();for(auto& f:formats)f.Reset();collection.Reset();headingCollection.Reset();rendering.Reset();
        if(write&&collectionRegistered)write->UnregisterFontCollectionLoader(collectionLoader.Get());
        if(write&&headingRegistered)write->UnregisterFontCollectionLoader(headingLoader.Get());
        if(write&&filesRegistered)write->UnregisterFontFileLoader(files.Get());
        if(write&&headingFileRegistered)write->UnregisterFontFileLoader(headingFile.Get());
        filesRegistered=collectionRegistered=headingFileRegistered=headingRegistered=attempted=false;
        collectionLoader.Reset();headingLoader.Reset();files.Reset();headingFile.Reset();draw.Reset();write.Reset();family[0]=heading[0]=0;
    }
    bool Initialize(HMODULE module){
        if(attempted)return write&&draw&&logoPixels&&formats[0];attempted=true;
        if(FAILED(error=DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.GetAddressOf())))||
           FAILED(error=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,draw.GetAddressOf())))return false;
        auto* embedded=new Files();files.Attach(embedded);if(!embedded->Load(module)){error=HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND);return false;}
        collectionLoader.Attach(new Collection(files.Get()));
        if(FAILED(error=write->RegisterFontFileLoader(files.Get())))return false;filesRegistered=true;
        if(FAILED(error=write->RegisterFontCollectionLoader(collectionLoader.Get())))return false;collectionRegistered=true;
        const UINT32 key=0;if(FAILED(error=write->CreateCustomFontCollection(collectionLoader.Get(),&key,sizeof(key),&collection))||!Family(collection.Get(),family,128))return false;
        const auto resource=FindResourceW(module,MAKEINTRESOURCEW(302),MAKEINTRESOURCEW(10));
        const auto data=resource?LoadResource(module,resource):nullptr;const DWORD size=resource?SizeofResource(module,resource):0;
        if(!data||!size){error=HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND);return false;}
        headingFile.Attach(new coop_overlay::FontLoader(static_cast<const BYTE*>(LockResource(data)),size));
        headingLoader.Attach(new coop_overlay::CollectionLoader(headingFile.Get()));
        if(FAILED(error=write->RegisterFontFileLoader(headingFile.Get())))return false;headingFileRegistered=true;
        if(FAILED(error=write->RegisterFontCollectionLoader(headingLoader.Get())))return false;headingRegistered=true;
        if(FAILED(error=write->CreateCustomFontCollection(headingLoader.Get(),&key,sizeof(key),&headingCollection))||!Family(headingCollection.Get(),heading,128))return false;
        for(size_t i=0;i<formats.size();++i){const bool sport=i==4,bold=i==1||i==3,italic=i==2||i==3;
            if(FAILED(error=write->CreateTextFormat(sport?heading:family,sport?headingCollection.Get():collection.Get(),bold?DWRITE_FONT_WEIGHT_BOLD:DWRITE_FONT_WEIGHT_NORMAL,
                italic?DWRITE_FONT_STYLE_ITALIC:DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,14,L"en-us",&formats[i])))return false;}
        if(FAILED(error=write->CreateCustomRenderingParams(2.2f,1,0,DWRITE_PIXEL_GEOMETRY_FLAT,DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,&rendering)))return false;
        const auto image=FindResourceW(module,MAKEINTRESOURCEW(315),MAKEINTRESOURCEW(10));auto imageData=image?LoadResource(module,image):nullptr;
        if(!imageData){error=HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND);return false;}
        ComPtr<IStream> stream;stream.Attach(SHCreateMemStream(static_cast<const BYTE*>(LockResource(imageData)),SizeofResource(module,image)));
        ComPtr<IWICImagingFactory> wic;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
        if(!stream||FAILED(error=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)))||
           FAILED(error=wic->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder))||
           FAILED(error=decoder->GetFrame(0,&frame))||FAILED(error=frame->GetSize(&logoWidth,&logoHeight))||!logoWidth||!logoHeight||
           FAILED(error=wic->CreateFormatConverter(&logoPixels))||
           FAILED(error=logoPixels->Initialize(frame.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return false;
        return true;
    }
    bool Begin(HDC dc,int width,int height,float dpi){
        if(!write||!formats[0]||width<=0||height<=0||width>8192||height>8192||dpi<48||dpi>768)return false;
        if(!target){auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),dpi,dpi);
            // Publish target-dependent resources together. A transient failure
            // must not leave a target with a missing brush on the next paint.
            ComPtr<ID2D1DCRenderTarget> nextTarget;ComPtr<ID2D1SolidColorBrush> nextBrush;ComPtr<ID2D1Bitmap> nextLogo;
            if(FAILED(error=draw->CreateDCRenderTarget(&props,&nextTarget))||
               FAILED(error=nextTarget->CreateSolidColorBrush(D2D1::ColorF(1,1,1),&nextBrush))||
               FAILED(error=nextTarget->CreateBitmapFromWicBitmap(logoPixels.Get(),nullptr,&nextLogo)))return false;
            target=nextTarget;brush=nextBrush;logo=nextLogo;}
        RECT bounds{0,0,width,height};if(FAILED(error=target->BindDC(dc,&bounds)))return false;
        target->SetDpi(dpi,dpi);target->SetTransform(D2D1::Matrix3x2F::Identity());target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target->SetTextRenderingParams(rendering.Get());target->BeginDraw();target->Clear(D2D1::ColorF(0x111214));return true;
    }
    bool End(){error=target->EndDraw();if(FAILED(error)){logo.Reset();target.Reset();brush.Reset();}return SUCCEEDED(error);}
    void Box(float x,float y,float w,float h,unsigned color,float radius=0,unsigned border=0){
        Color(color);const auto shape=D2D1::RoundedRect(D2D1::RectF(x,y,x+w,y+h),radius,radius);target->FillRoundedRectangle(shape,brush.Get());
        if(border){Color(border);target->DrawRoundedRectangle(shape,brush.Get(),1);}
    }
    void Line(float x,float y,float xx,float yy,unsigned color,float thickness=1){Color(color);target->DrawLine(D2D1::Point2F(x,y),D2D1::Point2F(xx,yy),brush.Get(),thickness);}
    void Circle(float x,float y,float radius,unsigned color,bool filled=true,float thickness=1){Color(color);auto ellipse=D2D1::Ellipse(D2D1::Point2F(x,y),radius,radius);
        if(filled)target->FillEllipse(ellipse,brush.Get());else target->DrawEllipse(ellipse,brush.Get(),thickness);}
    void Logo(float x,float y,float width){if(logo){const float h=width*logoHeight/logoWidth;
        target->DrawBitmap(logo.Get(),D2D1::RectF(x,y,x+width,y+h),1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);}}
    ComPtr<IDWriteTextLayout> Layout(const wchar_t* text,float size,float width,float height,Face face=Face::Regular,bool wrap=false){
        ComPtr<IDWriteTextLayout> layout;if(!text||size<6||size>100||width<=0||height<=0)return layout;
        error=write->CreateTextLayout(text,UINT32(std::wcslen(text)),formats[size_t(face)].Get(),width,height,&layout);
        if(SUCCEEDED(error)){layout->SetFontSize(size,{0,UINT32(std::wcslen(text))});layout->SetWordWrapping(wrap?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP);
            if(!wrap){ComPtr<IDWriteInlineObject> ellipsis;
                if(SUCCEEDED(write->CreateEllipsisTrimmingSign(layout.Get(),&ellipsis))){DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};layout->SetTrimming(&trimming,ellipsis.Get());}}}
        return layout;
    }
    DWRITE_TEXT_METRICS Measure(const wchar_t* text,float size,float width,float height,Face face=Face::Regular,bool wrap=false){
        DWRITE_TEXT_METRICS metrics{};auto layout=Layout(text,size,width,height,face,wrap);if(layout)layout->GetMetrics(&metrics);return metrics;
    }
    void Text(const wchar_t* text,float x,float y,float width,float height,float size=14,unsigned color=0xD7D9DD,Face face=Face::Regular,bool wrap=false,bool centered=false,float scroll=0){
        auto layout=Layout(text,size,width,height+scroll,face,wrap);if(!layout)return;
        if(centered){layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);}
        Color(color);target->PushAxisAlignedClip(D2D1::RectF(x,y,x+width,y+height),D2D1_ANTIALIAS_MODE_ALIASED);
        target->DrawTextLayout(D2D1::Point2F(x,y-scroll),layout.Get(),brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);target->PopAxisAlignedClip();
    }
};
}
