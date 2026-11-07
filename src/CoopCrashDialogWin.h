// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <initializer_list>
#include "CoopBuildVersion.h"

#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"user32.lib")

namespace coop_crash {
struct Report {
    DWORD code=0;
    uintptr_t address=0;
    wchar_t module[MAX_PATH]{};
    wchar_t logPath[MAX_PATH]{};
    unsigned player=0;
};

namespace dialog {
constexpr wchar_t kClass[]=L"UndercoopCrashReport";
constexpr wchar_t kTitle[]=L"Undercoop | Crash report";
constexpr int kWidth=610,kHeight=424,kCopy=1001,kClose=1002;
constexpr COLORREF kBackdrop=RGB(19,22,29),kPanel=RGB(27,32,41),kBorder=RGB(57,66,82);
constexpr COLORREF kWhite=RGB(238,242,248),kMuted=RGB(166,177,194),kAccent=RGB(251,116,108);
constexpr COLORREF kButton=RGB(43,51,64),kPrimary=RGB(238,242,248),kInk=RGB(24,29,38);
#define UNDERCOOP_CRASH_WIDEN_INNER(value) L##value
#define UNDERCOOP_CRASH_WIDEN(value) UNDERCOOP_CRASH_WIDEN_INNER(value)
constexpr wchar_t kBuild[]=UNDERCOOP_CRASH_WIDEN(UNDERCOOP_BUILD_VERSION);
#undef UNDERCOOP_CRASH_WIDEN
#undef UNDERCOOP_CRASH_WIDEN_INNER

struct View {
    const Report* report=nullptr;
    UINT dpi=96;
    HWND copy=nullptr,close=nullptr;
    HFONT heading=nullptr,body=nullptr,compact=nullptr,label=nullptr,code=nullptr;
};

inline const wchar_t* Category(DWORD code) noexcept {
    switch(code) {
    case EXCEPTION_ACCESS_VIOLATION: return L"Access violation";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return L"Array bounds exceeded";
    case EXCEPTION_BREAKPOINT: return L"Breakpoint or assertion";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return L"Misaligned memory access";
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW: return L"Floating-point exception";
    case EXCEPTION_GUARD_PAGE: return L"Guard page violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return L"Illegal instruction";
    case EXCEPTION_IN_PAGE_ERROR: return L"Memory paging error";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return L"Integer division by zero";
    case EXCEPTION_INT_OVERFLOW: return L"Integer overflow";
    case EXCEPTION_INVALID_DISPOSITION: return L"Invalid exception disposition";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return L"Noncontinuable exception";
    case EXCEPTION_PRIV_INSTRUCTION: return L"Privileged instruction";
    case EXCEPTION_SINGLE_STEP: return L"Single-step exception";
    case EXCEPTION_STACK_OVERFLOW: return L"Stack overflow";
    case 0xE06D7363u: return L"Unhandled C++ exception";
    case 0x40000015u: return L"Fatal application exit";
    default: return L"Unhandled exception";
    }
}

inline const wchar_t* Available(const wchar_t* text) noexcept {
    return *text?text:L"Unavailable";
}

inline void Details(const Report& report,wchar_t* output,size_t capacity) noexcept {
    wchar_t player[24]=L"Unknown";
    if(report.player)swprintf_s(player,L"Player %u",report.player);
    swprintf_s(output,capacity,
        L"Undercoop %ls Crash Report\r\nInstance: %ls\r\nException: 0x%08lX (%ls)\r\n"
        L"Address: 0x%p\r\nModule: %ls\r\nLog: %ls",
        kBuild,player,static_cast<unsigned long>(report.code),Category(report.code),
        reinterpret_cast<void*>(report.address),Available(report.module),Available(report.logPath));
}

inline int Px(const View& view,int value) noexcept {return MulDiv(value,view.dpi,96);}

inline RECT Box(const View& view,int x,int y,int w,int h) noexcept {
    return {Px(view,x),Px(view,y),Px(view,x+w),Px(view,y+h)};
}

inline void Fill(HDC dc,RECT box,COLORREF color) noexcept {
    if(HBRUSH brush=CreateSolidBrush(color)){FillRect(dc,&box,brush);DeleteObject(brush);}
}

inline void Rounded(HDC dc,RECT box,int radius,COLORREF color,COLORREF edge) noexcept {
    HBRUSH brush=CreateSolidBrush(color);
    HPEN pen=CreatePen(PS_SOLID,1,edge);
    if(brush && pen) {
        const HGDIOBJ oldBrush=SelectObject(dc,brush),oldPen=SelectObject(dc,pen);
        RoundRect(dc,box.left,box.top,box.right,box.bottom,radius,radius);
        SelectObject(dc,oldPen);SelectObject(dc,oldBrush);
    } else Fill(dc,box,color);
    if(pen)DeleteObject(pen);
    if(brush)DeleteObject(brush);
}

inline void Text(HDC dc,HFONT font,COLORREF color,const wchar_t* text,RECT box,UINT format) noexcept {
    const HGDIOBJ previous=SelectObject(dc,font?static_cast<HGDIOBJ>(font):GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc,color);
    SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,text,-1,&box,format|DT_NOPREFIX);
    if(previous)SelectObject(dc,previous);
}

inline HFONT Font(const View& view,int size,int weight,const wchar_t* family=L"Segoe UI") noexcept {
    return CreateFontW(-Px(view,size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,family);
}

inline void FreeFonts(View& view) noexcept {
    for(HFONT font:{view.heading,view.body,view.compact,view.label,view.code})if(font)DeleteObject(font);
    view.heading=view.body=view.compact=view.label=view.code=nullptr;
}

inline void Layout(View& view) noexcept {
    FreeFonts(view);
    view.heading=Font(view,34,FW_SEMIBOLD);
    view.body=Font(view,15,FW_NORMAL);
    view.compact=Font(view,13,FW_NORMAL);
    view.label=Font(view,13,FW_SEMIBOLD);
    view.code=Font(view,13,FW_NORMAL,L"Consolas");
    MoveWindow(view.copy,Px(view,326),Px(view,359),Px(view,129),Px(view,37),FALSE);
    MoveWindow(view.close,Px(view,465),Px(view,359),Px(view,109),Px(view,37),FALSE);
}

inline void DrawReport(HDC dc,const View& view,RECT client) noexcept {
    Fill(dc,client,kBackdrop);
    FrameRect(dc,&client,GetSysColorBrush(COLOR_3DSHADOW));
    Fill(dc,Box(view,1,1,kWidth-2,3),kAccent);
    Rounded(dc,Box(view,35,23,22,22),Px(view,6),kAccent,kAccent);
    Text(dc,view.label,kInk,L"!",Box(view,35,23,22,21),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    Text(dc,view.label,kWhite,L"UNDERCOOP / CRASH REPORT",Box(view,67,23,330,21),DT_VCENTER|DT_SINGLELINE);
    wchar_t player[32]=L"GAME SESSION";
    if(view.report->player)swprintf_s(player,L"PLAYER %u",view.report->player);
    Rounded(dc,Box(view,454,22,120,25),Px(view,11),kPanel,kBorder);
    Text(dc,view.label,kMuted,player,Box(view,454,22,120,25),DT_CENTER|DT_VCENTER|DT_SINGLELINE);

    Text(dc,view.heading,kWhite,L"The game crashed",Box(view,35,66,540,46),DT_SINGLELINE|DT_END_ELLIPSIS);
    Text(dc,view.body,kMuted,L"An unexpected error stopped the game. Copy the details to report it.",
        Box(view,35,120,540,28),DT_SINGLELINE|DT_END_ELLIPSIS);

    Rounded(dc,Box(view,35,162,540,165),Px(view,14),kPanel,kBorder);
    Text(dc,view.label,kWhite,L"TECHNICAL DETAILS",Box(view,53,175,480,20),DT_SINGLELINE);
    Fill(dc,Box(view,53,199,504,1),kBorder);
    wchar_t code[100]{},address[32]{};
    swprintf_s(code,L"0x%08lX  %ls",static_cast<unsigned long>(view.report->code),Category(view.report->code));
    swprintf_s(address,L"0x%p",reinterpret_cast<void*>(view.report->address));
    struct Line{const wchar_t* label;const wchar_t* value;bool path;};
    const Line rows[]={
        {L"Exception",code,false},{L"Address",address,false},
        {L"Module",Available(view.report->module),true},{L"Log file",Available(view.report->logPath),true}
    };
    for(int index=0;index<4;++index) {
        const auto& row=rows[index];
        const int y=207+index*26;
        Text(dc,view.compact,kMuted,row.label,Box(view,53,y,91,20),DT_SINGLELINE);
        Text(dc,row.path?view.compact:view.code,kWhite,row.value,Box(view,147,y,406,20),
            DT_SINGLELINE|(row.path?DT_PATH_ELLIPSIS:DT_END_ELLIPSIS));
    }
    wchar_t footer[96]{};swprintf_s(footer,L"UNDERGROUND 2  /  %ls",kBuild);
    Text(dc,view.compact,kMuted,footer,Box(view,36,362,275,29),DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
}

inline void PaintButton(const View& view,const DRAWITEMSTRUCT& item) noexcept {
    const bool primary=item.CtlID==kClose;
    const bool pressed=(item.itemState&ODS_SELECTED)!=0;
    Rounded(item.hDC,item.rcItem,Px(view,11),primary?(pressed?RGB(191,202,220):kPrimary):
        (pressed?RGB(59,69,87):kButton),primary?kPrimary:kBorder);
    wchar_t title[32]{};GetWindowTextW(item.hwndItem,title,32);
    Text(item.hDC,view.label,primary?kInk:kWhite,title,item.rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    if(item.itemState&ODS_FOCUS){RECT focus=item.rcItem;InflateRect(&focus,-Px(view,4),-Px(view,4));DrawFocusRect(item.hDC,&focus);}
}

inline bool Copy(HWND window,const Report& report) noexcept {
    wchar_t text[2*MAX_PATH+220]{};Details(report,text,sizeof(text)/sizeof(text[0]));
    const size_t size=(std::wcslen(text)+1)*sizeof(wchar_t);
    HGLOBAL buffer=GlobalAlloc(GMEM_MOVEABLE,size);
    if(!buffer)return false;
    void* storage=GlobalLock(buffer);
    if(!storage){GlobalFree(buffer);return false;}
    CopyMemory(storage,text,size);GlobalUnlock(buffer);
    bool okay=false;
    if(OpenClipboard(window)) {
        okay=EmptyClipboard() && SetClipboardData(CF_UNICODETEXT,buffer);
        CloseClipboard();
    }
    if(!okay)GlobalFree(buffer);
    return okay;
}

inline LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) noexcept {
    if(message==WM_NCCREATE) {
        const auto* create=reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* view=reinterpret_cast<View*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(view)switch(message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc=BeginPaint(window,&paint);
        RECT client{};GetClientRect(window,&client);
        if(dc)DrawReport(dc,*view,client);
        EndPaint(window,&paint);return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DRAWITEM: PaintButton(*view,*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));return TRUE;
    case WM_DPICHANGED: {
        view->dpi=HIWORD(wparam);Layout(*view);
        const auto* suggested=reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window,nullptr,suggested->left,suggested->top,Px(*view,kWidth),Px(*view,kHeight),SWP_NOZORDER|SWP_NOACTIVATE);
        InvalidateRect(window,nullptr,FALSE);return 0;
    }
    case WM_NCHITTEST: {
        const LRESULT hit=DefWindowProcW(window,message,wparam,lparam);
        POINT point{static_cast<short>(LOWORD(lparam)),static_cast<short>(HIWORD(lparam))};
        ScreenToClient(window,&point);
        return hit==HTCLIENT && point.y<Px(*view,55)?HTCAPTION:hit;
    }
    case WM_COMMAND:
        if(LOWORD(wparam)==kClose || LOWORD(wparam)==IDCANCEL){DestroyWindow(window);return 0;}
        if(LOWORD(wparam)==kCopy){SetWindowTextW(view->copy,Copy(window,*view->report)?L"Copied":L"Try copying again");return 0;}
        break;
    case WM_CLOSE: DestroyWindow(window);return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

inline bool Register(HINSTANCE instance) noexcept {
    WNDCLASSEXW type{};
    type.cbSize=sizeof(type);type.lpfnWndProc=WindowProc;type.hInstance=instance;
    type.lpszClassName=kClass;type.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
    type.hIcon=LoadIconW(nullptr,MAKEINTRESOURCEW(32513));type.hIconSm=type.hIcon;
    return RegisterClassExW(&type)!=0 || GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
}

inline HWND Button(HWND window,HINSTANCE instance,int id,const wchar_t* title) noexcept {
    return CreateWindowExW(0,L"BUTTON",title,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
        0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
}

inline UINT WindowDpi(HWND window) noexcept {
    using GetDpi=UINT(WINAPI*)(HWND);
    const auto getDpi=reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
    if(getDpi)if(const UINT dpi=getDpi(window))return dpi;
    HDC dc=GetDC(window);
    if(!dc)return 96;
    const int dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(window,dc);
    return dpi>0?UINT(dpi):96;
}

inline bool Run(const Report& report,HANDLE presented) noexcept {
    HINSTANCE instance=GetModuleHandleW(nullptr);
    if(!Register(instance))return false;
    POINT cursor{};GetCursorPos(&cursor);
    MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
    if(!GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&monitor)) {
        monitor.rcWork={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
    }
    View view{};view.report=&report;
    HWND window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_APPWINDOW|WS_EX_CONTROLPARENT,kClass,kTitle,
        WS_POPUP|WS_SYSMENU,monitor.rcWork.left+16,monitor.rcWork.top+16,1,1,nullptr,nullptr,instance,&view);
    if(!window)return false;
    view.copy=Button(window,instance,kCopy,L"Copy details");
    view.close=Button(window,instance,kClose,L"Close game");
    if(!view.copy || !view.close){DestroyWindow(window);return false;}
    const auto& work=monitor.rcWork;
    const int fitX=MulDiv(work.right-work.left-16,96,kWidth),fitY=MulDiv(work.bottom-work.top-16,96,kHeight);
    view.dpi=WindowDpi(window);
    if(fitX>0 && fitY>0) {
        if(view.dpi>UINT(fitX))view.dpi=UINT(fitX);
        if(view.dpi>UINT(fitY))view.dpi=UINT(fitY);
    }
    Layout(view);
    const int width=Px(view,kWidth),height=Px(view,kHeight);
    if(!SetWindowPos(window,HWND_TOPMOST,work.left+(work.right-work.left-width)/2,
        work.top+(work.bottom-work.top-height)/2,width,height,SWP_SHOWWINDOW)) {
        DestroyWindow(window);FreeFonts(view);return false;
    }
    SetForegroundWindow(window);SetFocus(view.close);UpdateWindow(window);
    if(presented)SetEvent(presented);
    MSG message{};
    while(IsWindow(window)) {
        const BOOL result=GetMessageW(&message,nullptr,0,0);
        if(result<=0){if(result==0)PostQuitMessage(static_cast<int>(message.wParam));break;}
        if(message.message==WM_KEYDOWN && message.wParam==VK_ESCAPE){DestroyWindow(window);break;}
        if(message.message==WM_KEYDOWN && message.wParam==VK_RETURN) {
            SendMessageW(window,WM_COMMAND,GetFocus()==view.copy?kCopy:kClose,0);continue;
        }
        if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    }
    if(IsWindow(window))DestroyWindow(window);
    FreeFonts(view);
    return true;
}
} // namespace dialog

inline void Prepare() noexcept {dialog::Register(GetModuleHandleW(nullptr));}

inline void Show(const Report& report,HANDLE presented) noexcept {
    using SetDpiContext=HANDLE(WINAPI*)(HANDLE);
    const auto setDpi=reinterpret_cast<SetDpiContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetThreadDpiAwarenessContext"));
    const HANDLE oldDpi=setDpi?setDpi(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))):nullptr;
    Report safe=report;
    safe.module[MAX_PATH-1]=safe.logPath[MAX_PATH-1]=L'\0';
    if(!dialog::Run(safe,presented)) {
        wchar_t details[2*MAX_PATH+220]{};
        dialog::Details(safe,details,sizeof(details)/sizeof(details[0]));
        if(presented)SetEvent(presented);
        MessageBoxW(nullptr,details,dialog::kTitle,MB_OK|MB_ICONERROR|MB_TOPMOST|MB_SETFOREGROUND|MB_TASKMODAL);
    }
    if(oldDpi)setDpi(oldDpi);
}
inline void Show(const Report& report) noexcept {Show(report,nullptr);}
} // namespace coop_crash
