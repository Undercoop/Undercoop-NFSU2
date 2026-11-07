// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopPeerLabel.h"
#include "CoopOverlayText.h"
namespace CoopPeerLabelOverlay {
static HMODULE module=nullptr;
static HWND window=nullptr;
static coop_overlay::Renderer renderer;
static HDC dc=nullptr;
static HBITMAP bitmap=nullptr;
static HGDIOBJ previous=nullptr;
static int surfaceWidth=0,surfaceHeight=0;
static float paintedFont=0,paintedDpi=0;
static std::wstring paintedName;
static void Initialize(HMODULE value){module=value;}
static void Hide(){if(window&&IsWindowVisible(window))ShowWindow(window,SW_HIDE);}
static void ClearSurface(){if(dc&&previous)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);
    if(dc)DeleteDC(dc);dc=nullptr;bitmap=nullptr;previous=nullptr;surfaceWidth=surfaceHeight=0;paintedFont=paintedDpi=0;paintedName.clear();}
static LRESULT CALLBACK Procedure(HWND target,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_NCHITTEST)return HTTRANSPARENT;if(message==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(message==WM_ERASEBKGND)return 1;if(message==WM_DPICHANGED){paintedDpi=0;return 0;}
    return DefWindowProcW(target,message,wp,lp);
}
static bool Ensure(HWND game,const RECT& client){
    if(!module||!renderer.Initialize(module))return false;
    if(window&&IsWindow(window))return GetWindow(window,GW_OWNER)==game;
    coop_overlay::DpiScope scope;
    WNDCLASSW klass{};klass.hInstance=module;klass.lpfnWndProc=&Procedure;klass.lpszClassName=L"Undercoop.PeerName";
    if(!RegisterClassW(&klass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
    window=CreateWindowExW(WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT,
        klass.lpszClassName,L"Undercoop Peer Name",WS_POPUP,client.left,client.top,1,1,game,nullptr,module,nullptr);
    return window!=nullptr;
}
static bool Paint(const wchar_t* name,float fontPixels,float dpi){
    if(paintedName==name&&paintedFont==fontPixels&&paintedDpi==dpi&&dc)return true;
    const float scale=dpi/96.0f;
    auto layout=renderer.LabelLayout(name,fontPixels/scale);DWRITE_TEXT_METRICS metrics{};
    DWRITE_OVERHANG_METRICS overhang{};
    if(!layout||FAILED(layout->GetMetrics(&metrics))||FAILED(layout->GetOverhangMetrics(&overhang)))return false;
    const float pad=3.0f+std::max({overhang.left,overhang.right,overhang.top,overhang.bottom,0.0f});
    const int width=int(std::ceil((metrics.widthIncludingTrailingWhitespace+pad*2)*scale));
    const int height=int(std::ceil((metrics.height+pad*2)*scale));
    if(width<=0||height<=0||width>2048||height>512)return false;
    if(width!=surfaceWidth||height!=surfaceHeight||!dc){
        ClearSurface();dc=CreateCompatibleDC(nullptr);if(!dc)return false;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
        info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap||!pixels){ClearSurface();return false;}previous=SelectObject(dc,bitmap);
        if(!previous||previous==HGDI_ERROR){ClearSurface();return false;}surfaceWidth=width;surfaceHeight=height;
    }
    if(!renderer.Begin(dc,width,height,dpi))return false;
    renderer.Label(layout.Get(),pad,pad);if(!renderer.End())return false;
    paintedName=name;paintedFont=fontPixels;paintedDpi=dpi;return true;
}
static bool Show(HWND game,bool focused,const RECT& client,const coop_peer_label::Projection& point,const wchar_t* name){
    if(!game||!name||!*name||!IsWindow(game)||!IsWindowVisible(game)||IsIconic(game)||
        GetWindowThreadProcessId(game,nullptr)!=GetCurrentThreadId()){Hide();return false;}
    coop_overlay::DpiScope scope;
    if(!Ensure(game,client)){Hide();return false;}
    if(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST)!=MonitorFromRect(&client,MONITOR_DEFAULTTONEAREST)){
        SetWindowPos(window,nullptr,client.left,client.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);paintedDpi=0;}
    if(!Paint(name,point.fontPixels,coop_overlay::WindowDpi(window))){Hide();return false;}
    POINT at{client.left+int(std::lround(point.x))-surfaceWidth/2,client.top+int(std::lround(point.y))-surfaceHeight};
    // Whole-label clipping rather than allowing an owned popup outside its
    // game window (which would leak into the other local instance/desktop).
    if(at.x<client.left+2||at.y<client.top+2||at.x+surfaceWidth>client.right-2||at.y+surfaceHeight>client.bottom-2){Hide();return false;}
    POINT source{};SIZE size{surfaceWidth,surfaceHeight};BLENDFUNCTION blend{AC_SRC_OVER,0,point.alpha,AC_SRC_ALPHA};
    if(!UpdateLayeredWindow(window,nullptr,&at,&size,dc,&source,0,&blend,ULW_ALPHA)){Hide();return false;}
    if(!IsWindowVisible(window))ShowWindow(window,SW_SHOWNOACTIVATE);
    if(focused)SetWindowPos(window,HWND_TOP,0,0,0,0,SWP_NOSIZE|SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
    return true;
}
}
