// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.
#pragma once
#include "CoopConnectionHud.h"
#include "CoopOverlayText.h"
namespace CoopConnectionHud {
static HMODULE module=nullptr;
static HWND window=nullptr;
static coop_overlay::Renderer renderer;
static coop_connection_hud::Model model;
static HDC dc=nullptr;static HBITMAP bitmap=nullptr;static HGDIOBJ previous=nullptr;
static int surfaceWidth=0,surfaceHeight=0;
static float paintedDpi=0;static unsigned paintedRgb=0;
static char paintedText[80]{};
static char paintedActivity[96]{};
static DWORD updatedAt=0;static bool updated=false;
static POINT placedAt{};static bool placed=false;
static coop_connection_hud::View view;
static void Initialize(HMODULE value){module=value;}
static void Hide(){if(window&&IsWindowVisible(window))ShowWindow(window,SW_HIDE);}
static void ClearSurface(){
    if(dc&&previous)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);
    dc=nullptr;bitmap=nullptr;previous=nullptr;surfaceWidth=surfaceHeight=0;paintedDpi=0;paintedText[0]=paintedActivity[0]=0;
}
static LRESULT CALLBACK Procedure(HWND target,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_NCHITTEST)return HTTRANSPARENT;if(message==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(message==WM_ERASEBKGND)return 1;if(message==WM_DPICHANGED){paintedDpi=0;return 0;}
    return DefWindowProcW(target,message,wp,lp);
}
static bool Draw(HDC target,int width,int height,const coop_connection_hud::View& state,float dpi){
    if(!module||!renderer.Initialize(module)||!renderer.Begin(target,width,height,dpi))return false;
    wchar_t text[80]{};MultiByteToWideChar(CP_UTF8,0,state.text,-1,text,80);
    const float scale=dpi/96.0f;
    renderer.ColoredText(text,3,2,width/scale-6,height/scale-4,state.rgb);
    if(state.activity[0]){
        wchar_t activity[96]{};MultiByteToWideChar(CP_UTF8,0,state.activity,-1,activity,96);
        renderer.ColoredText(activity,3,24,width/scale-6,height/scale-26,0xCED3D9,12,true);
    }
    return renderer.End();
}
static bool Paint(int width,int height,float dpi){
    if(width!=surfaceWidth||height!=surfaceHeight||!dc){
        ClearSurface();dc=CreateCompatibleDC(nullptr);if(!dc)return false;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
        info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap||!pixels){ClearSurface();return false;}
        previous=SelectObject(dc,bitmap);if(!previous||previous==HGDI_ERROR){ClearSurface();return false;}
        surfaceWidth=width;surfaceHeight=height;
    }
    if(!Draw(dc,width,height,view,dpi))return false;
    std::memcpy(paintedText,view.text,sizeof(paintedText));std::memcpy(paintedActivity,view.activity,sizeof(paintedActivity));paintedRgb=view.rgb;paintedDpi=dpi;return true;
}
static bool NeedsUpdate(DWORD now){return !updated||coop_time::Elapsed(now,updatedAt)>=250;}
static void Update(const coop::SessionSnapshot& session,const coop_transport::Statistics& wire,
                   bool admitted,bool gameplay,DWORD now,coop_activity::Activity activity={}){
    view=model.Update(session,wire,admitted,gameplay,now,activity);updatedAt=now;updated=true;
}
static void Tick(HWND game,bool focused){
    // State snapshots and raster work are capped at four updates per second.
    // Position still follows a dragged game window. No socket/native-car work.
    if(!view.visible||!game||!IsWindow(game)||!IsWindowVisible(game)||IsIconic(game)||
       GetWindowThreadProcessId(game,nullptr)!=GetCurrentThreadId()){Hide();return;}
    coop_overlay::DpiScope dpiScope;RECT client{};
    if(!coop_overlay::PhysicalClient(game,client)||client.right-client.left<240||client.bottom-client.top<180||!module)return;
    if(!window||!IsWindow(window)){
        WNDCLASSW klass{};klass.hInstance=module;klass.lpfnWndProc=Procedure;klass.lpszClassName=L"Undercoop.ConnectionHUD";
        if(!RegisterClassW(&klass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        window=CreateWindowExW(WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT,
            klass.lpszClassName,L"Undercoop Connection",WS_POPUP,client.left,client.top,1,1,game,nullptr,module,nullptr);
        if(!window)return;
    }
    if(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST)!=MonitorFromRect(&client,MONITOR_DEFAULTTONEAREST)){
        SetWindowPos(window,nullptr,client.left,client.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);paintedDpi=0;}
    const float dpi=coop_overlay::WindowDpi(window),scale=dpi/96.0f;
    const int width=std::min(int(client.right-client.left)-24,int(360*scale)),height=int((view.activity[0]?48:32)*scale);
    if(width<1||height<1)return;
    const bool repaint=width!=surfaceWidth||height!=surfaceHeight||paintedDpi!=dpi||paintedRgb!=view.rgb||std::strcmp(paintedText,view.text)||std::strcmp(paintedActivity,view.activity);
    if(repaint)
        if(!Paint(width,height,dpi)){Hide();return;}
    POINT at{client.right-width-int(12*scale),client.top+int(10*scale)},source{};
    SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    if(repaint||!placed||placedAt.x!=at.x||placedAt.y!=at.y){
        if(!UpdateLayeredWindow(window,nullptr,&at,&size,dc,&source,0,&blend,ULW_ALPHA)){Hide();return;}
        placedAt=at;placed=true;
    }
    if(!IsWindowVisible(window))ShowWindow(window,SW_SHOWNOACTIVATE);
    if(focused)SetWindowPos(window,HWND_TOP,0,0,0,0,SWP_NOSIZE|SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
}
}
