// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#include "CoopAnnouncements.h"
#include "CoopConsole.h"
#include "CoopOverlayText.h"
// Owned click-through windows. Native action-map slots are never guessed.
namespace CoopAnnouncements {
static SRWLOCK lock=SRWLOCK_INIT;
static coop_notices::Model model;
static coop_console::ChatHistory chat;
static coop_console::Editor editor;
static coop_console::Channel channel=coop_console::Channel::Chat;
static uint32_t inputRevision=1;
static HMODULE module=nullptr;
static HWND window=nullptr,buildWindow=nullptr;
static coop_overlay::Renderer textRenderer;
static float paintedDpi=0,buildDpi=0;
static uint32_t paintedRevision=0;
static uint64_t boundSession=0;
static RECT paintedRect{},buildRect{};
static bool enabled=false,failed=false,keyContext=true,toggleHeld=false,escapeBlocked=false;
static WPARAM releaseCapture=0;
static bool swallowChar=false;
static bool (*chatSender)(const char*)=nullptr;
static void SetChatSender(bool (*sender)(const char*)){AcquireSRWLockExclusive(&lock);chatSender=sender;ReleaseSRWLockExclusive(&lock);}
static constexpr WPARAM kToggleKey='Z';
static constexpr wchar_t kClass[]=L"NFSU2Coop.Announcements";
static void Initialize(HMODULE value){module=value;}
static void Configure(int autoShow,int opacity){AcquireSRWLockExclusive(&lock);model.Configure(autoShow,opacity);ReleaseSRWLockExclusive(&lock);}
static void Post(const char* text){AcquireSRWLockExclusive(&lock);model.Add(text,GetTickCount());ReleaseSRWLockExclusive(&lock);}
static void SetLive(const char* text) {
    AcquireSRWLockExclusive(&lock);
    if(text&&!std::strncmp(text,"COOP OBSERVER",13))model.SetDebug(text);
    else model.SetLive(text,GetTickCount());
    ReleaseSRWLockExclusive(&lock);
}
static void SetDebug(const char* text){AcquireSRWLockExclusive(&lock);model.SetDebug(text);ReleaseSRWLockExclusive(&lock);}
static void SetSponsorPrompt(const char* text){
    AcquireSRWLockExclusive(&lock);model.SetSponsorPrompt(text);ReleaseSRWLockExclusive(&lock);
}
static void AddChat(uint16_t player,const char* name,const char* text) {
    AcquireSRWLockExclusive(&lock);chat.Add(player,name,text,GetTickCount(),model.expanded&&channel==coop_console::Channel::Chat);
    ReleaseSRWLockExclusive(&lock);
}
static void FromLog(const char* text) {
    if(!text)return;
    struct Notice{const char* match;const char* message;};
    static const Notice notices[]{
        {"[SESSION] Explicit Join/JoinAck confirmed","Both players connected. Checking installed mods."},
        {"[CAREER_OFFERS] Native selector/New Career callback mismatch","Career menu hooks could not be verified. Restart both games with the same updated Undercoop version."},
        {"[SESSION] Peer left/timed out","Your teammate disconnected."},
        {"[SESSION] Both profiles ready","Career data synchronized. Waiting for the opening scene or loading."},
        {"[SAVE] Host checkpoint validated/committed","Shared career progress synchronized."},
        {"[SAVE] Checkpoint rejected","Save transfer could not be verified. Existing saves are safe."},
        {"[RACE] Session-scoped peer invitation received","Race invitation received. F5 accept / F6 decline."},
        {"[RACE] Peer invitation accepted","Race accepted. Waiting for synchronized loading."},
        {"[RACE] Invitation cancelled","Race invitation cancelled."},
        {"[RACE_RESTART] Native restart held","Race restart requested. Waiting for your teammate."},
        {"[RACE_GRID] Synchronized native starter/countdown released","Both players ready. Countdown begins."},
        {"[RACE_FINISH] Local finish locked","Finish recorded. Waiting for your teammate."},
        {"[RACE_FINISH] Both humans finished","Both players finished. Final results are being synchronized."},
        {"[COOP_PAUSE] Both players paused","Shared pause active. Use the game's Resume action to continue."},
        {"[COOP_PAUSE] Both players resumed","Shared pause released."},
        {"[INTRO_VOTE] Local skip vote sent","Cutscene skip vote submitted."}
    };
    for(const auto& notice:notices)if(std::strstr(text,notice.match)){Post(notice.message);return;}
    if(!std::strncmp(text,"[RACE_LOAD_FAILED]",18))Post("Race loading could not be verified. Restart both games.");
}
static void Bind(bool active,uint64_t session,bool allowToggle=true) {
    AcquireSRWLockExclusive(&lock);enabled=active;keyContext=allowToggle;
    if(!active||!allowToggle){if(editor.active||releaseCapture)++inputRevision;editor.Clear();releaseCapture=0;swallowChar=false;toggleHeld=false;escapeBlocked=false;}
    if(active&&session&&boundSession!=session) {
        if(boundSession){model.Reset();chat={};editor.Clear();releaseCapture=0;swallowChar=false;toggleHeld=false;escapeBlocked=false;channel=coop_console::Channel::Chat;++inputRevision;}
        boundSession=session;
    }
    ReleaseSRWLockExclusive(&lock);
}
static bool CapturesKeyboard() {
    AcquireSRWLockShared(&lock);const bool value=enabled&&keyContext&&(editor.active||releaseCapture);
    ReleaseSRWLockShared(&lock);return value;
}
static bool Key(WPARAM key,bool repeated) {
    char message[coop_console::kMessageBytes+1]{};bool submit=false,handled=false;
    AcquireSRWLockExclusive(&lock);
    if(!enabled){ReleaseSRWLockExclusive(&lock);return false;}
    if(key==releaseCapture||(key==VK_ESCAPE&&escapeBlocked)){ReleaseSRWLockExclusive(&lock);return true;}
    if(editor.active) {
        if(key==VK_RETURN) {
            if(!repeated){
                if(editor.length)submit=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,editor.text.data(),-1,message,sizeof(message),nullptr,nullptr)>1;
                editor.Clear();releaseCapture=key;swallowChar=true;++inputRevision;
            }handled=true;
        }else if(key==VK_ESCAPE){editor.Clear();releaseCapture=key;escapeBlocked=true;++inputRevision;handled=true;}
        else if(key==VK_BACK){if(editor.Backspace())++inputRevision;handled=true;}
        else if(key==coop_console::kAcceptKey||key==coop_console::kDeclineKey)handled=false;
        else if(key==VK_F4&&(GetKeyState(VK_MENU)&0x8000))handled=false;
        else handled=true;
    }else if(key==kToggleKey&&keyContext){if(!repeated&&!toggleHeld){model.Toggle();if(model.expanded&&channel==coop_console::Channel::Chat)chat.unread=0;swallowChar=true;}toggleHeld=true;handled=true;}
    else if(model.expanded&&keyContext&&key==VK_RETURN){if(!repeated){editor.Begin();channel=coop_console::Channel::Chat;chat.unread=0;chat.scroll=0;swallowChar=true;++inputRevision;}handled=true;}
    else if(key==VK_ESCAPE&&model.expanded){model.Close();releaseCapture=key;escapeBlocked=true;handled=true;}
    else if(model.expanded&&key==VK_TAB){
        if(!repeated){channel=channel==coop_console::Channel::Chat?coop_console::Channel::System:coop_console::Channel::Chat;
            if(channel==coop_console::Channel::Chat)chat.unread=0;++inputRevision;}handled=true;
    }else if(model.expanded&&(key==VK_PRIOR||key==VK_NEXT)) {
        const int delta=key==VK_PRIOR?3:-3;
        if(channel==coop_console::Channel::Chat)chat.Scroll(delta);else model.Scroll(delta);handled=true;
    }
    const auto sender=chatSender;ReleaseSRWLockExclusive(&lock);
    if(submit&&(!sender||!sender(message)))Post("Chat unavailable or sending too fast. Please try again.");
    return handled;
}
static bool Character(WPARAM code,bool unicode=false) {
    AcquireSRWLockExclusive(&lock);const bool handled=enabled&&(editor.active||swallowChar);
    if(handled&&editor.active&&code>=32&&code!=127) {
        if(unicode){if(editor.Append(uint32_t(code)))++inputRevision;}
        else {
            // The game subclasses an ANSI WndProc, so WM_CHAR is ACP, not UTF-8.
            char bytes[2]={char(code&255),char((code>>8)&255)};wchar_t chars[2]{};
            const int count=MultiByteToWideChar(CP_ACP,0,bytes,bytes[1]?2:1,chars,2);
            for(int i=0;i<count;++i)if(editor.Append(uint16_t(chars[i])))++inputRevision;
        }
    }
    ReleaseSRWLockExclusive(&lock);return handled;
}
static void Release(WPARAM key) {
    AcquireSRWLockExclusive(&lock);if(key==kToggleKey)toggleHeld=false;if(key==VK_ESCAPE)escapeBlocked=false;
    if(key==releaseCapture)releaseCapture=0;if(key==kToggleKey||key==VK_RETURN)swallowChar=false;ReleaseSRWLockExclusive(&lock);
}
static void FocusLost(){AcquireSRWLockExclusive(&lock);editor.Clear();releaseCapture=0;swallowChar=false;toggleHeld=false;++inputRevision;ReleaseSRWLockExclusive(&lock);}
static void PollToggle(bool down) {
    AcquireSRWLockShared(&lock);const bool can=enabled&&keyContext&&!editor.active;ReleaseSRWLockShared(&lock);
    if(!down)toggleHeld=false;else if(can&&!toggleHeld)Key(kToggleKey,false);
}
static bool Expanded(){AcquireSRWLockShared(&lock);const bool result=model.expanded;ReleaseSRWLockShared(&lock);return result;}
static std::wstring Wide(const char* text) {
    if(!text||!*text)return {};
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,nullptr,0);if(size<=1)return {};
    std::wstring result(size,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,&result[0],size);result.resize(size-1);return result;
}
static bool EnsureFont() {
    return textRenderer.Initialize(module);
}
struct View{coop_notices::Model system;coop_console::ChatHistory messages;coop_console::Editor input;coop_console::Channel selected;};
static void DrawView(float width,float height,const View& state) {
    auto& g=textRenderer;constexpr unsigned white=230,muted=155;
    const auto& sys=state.system;const bool expanded=sys.expanded;
    g.Rectangle(0,0,width,height,sys.backgroundAlpha,12);
    g.Rectangle(0,0,width,35,std::min(255,int(sys.backgroundAlpha)+25),25);g.Rectangle(0,34,width,1,150,95);
    g.Text(L"UNDERCOOP / CONSOLE",14,8,width-100,24,white,255,1);
    g.Text(state.input.active?L"WRITE":L"[ Z ]",width-85,10,75,22,muted);
    float top=47,bottom=float(height-33);
    if(expanded) {
        g.Rectangle(state.selected==coop_console::Channel::Chat?10.0f:145.0f,42,128,27,175,38);
        wchar_t label[48]{};std::swprintf(label,48,L"CHAT (%u)",state.messages.unread);
        g.Text(label,20,47,120,23,state.selected==coop_console::Channel::Chat?white:muted);
        g.Text(L"SYSTEM",155,47,120,23,state.selected==coop_console::Channel::System?white:muted);top=80;bottom=height-88;
    }
    const auto& live=sys.sponsorPrompt[0]?sys.sponsorPrompt:sys.live;
    const float liveHeight=live[0]?(expanded?88.0f:bottom-top):0;
    const float historyBottom=bottom-liveHeight-(live[0]&&expanded?8:0);
    if(expanded&&state.selected==coop_console::Channel::System&&sys.debug[0]) {
        const auto text=Wide(sys.debug.data());g.Text(text.c_str(),14,top,width-28,std::max(0.0f,historyBottom-top),muted);
    }else if(expanded) {
        const bool messages=state.selected==coop_console::Channel::Chat;
        const auto count=messages?state.messages.count:sys.count,scroll=messages?state.messages.scroll:sys.scroll;
        std::array<std::wstring,8> texts{},authors{};std::array<float,8> heights{};
        std::size_t visible=0;float used=0;
        while(visible<texts.size()&&scroll+visible<count) {
            auto& text=texts[visible];auto& author=authors[visible];
            if(messages){const auto& entry=state.messages.Recent(scroll+visible);author=Wide(entry.name.data());text=Wide(entry.text.data());}
            else{author=L"SERVER";text=Wide(sys.Recent(scroll+visible).text.data());std::replace(text.begin(),text.end(),L'\n',L' ');}
            const auto bounds=g.Measure(text.c_str(),width-28);
            const float row=std::max(21.0f,bounds.height)+29.0f;
            if(used+row>historyBottom-top)break;heights[visible]=row;used+=row;++visible;
        }
        if(!count)g.Text(messages?L"No messages yet. Press Enter to chat.":L"Server announcements appear here.",14,top,width-28,44,muted);
        float y=top;
        for(std::size_t i=visible;i>0;--i) {
            g.Text(authors[i-1].c_str(),14,y,width-28,22,muted);
            g.Text(texts[i-1].c_str(),14,y+21,width-28,heights[i-1]-26,white);y+=heights[i-1];
        }
    }else if(!live[0]) {
        const bool message=state.messages.Transient(GetTickCount())&&state.messages.count;std::wstring text;
        if(message){text=Wide(state.messages.Recent(0).name.data());text+=L": ";text+=Wide(state.messages.Recent(0).text.data());}
        else if(sys.count){text=L"SERVER: ";text+=Wide(sys.Recent(0).text.data());}
        g.Text(text.c_str(),14,top,width-28,bottom-top,white);
    }
    if(live[0]) {
        const float y=expanded?bottom-liveHeight:top;g.Rectangle(10,y-3,width-20,bottom-y+3,175,38);
        const auto text=Wide(live.data());g.Text(text.c_str(),16,y,width-32,bottom-y,white);
    }
    if(expanded) {
        const float y=height-74;g.Rectangle(10,y,width-20,37,175,38);
        std::wstring draft=state.input.active?L"> ":L"ENTER TO WRITE A MESSAGE";
        if(state.input.active){std::size_t start=0;
            do {draft=start?L"> ...":L"> ";draft+=state.input.text.data()+start;draft+=L"_";
                const auto bounds=g.Measure(draft.c_str(),10000,0,false);
                if(bounds.widthIncludingTrailingWhitespace<=width-42||start>=state.input.length)break;
                ++start;if(start<state.input.length&&uint16_t(state.input.text[start])>=0xDC00&&uint16_t(state.input.text[start])<=0xDFFF)++start;
            }while(true);}
        g.Text(draft.c_str(),18,y+8,width-36,28,state.input.active?white:muted,255,0,false);
    }
    g.Text(expanded?(state.input.active?L"ENTER SEND / ESC CANCEL":L"ENTER CHAT / TAB CHANNEL / Z CLOSE"):L"Z: CHAT / SYSTEM HISTORY",14,height-24,width-28,23,muted);
}
static void DrawBuild(float width,float height) {
    char info[240]{};coop_console::BuildInfo(info,sizeof(info),__DATE__,__TIME__);const auto text=Wide(info);
    textRenderer.Text(text.c_str(),0,0,width,height,211,205,2); // Text only, transparent background.
}
static LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_NCHITTEST)return HTTRANSPARENT;if(message==WM_MOUSEACTIVATE)return MA_NOACTIVATE;if(message==WM_ERASEBKGND)return 1;
    if(message==WM_DPICHANGED){paintedDpi=buildDpi=0;return 0;}
    return DefWindowProcW(hwnd,message,wp,lp);
}
static bool Ensure(HWND owner) {
    if(window&&IsWindow(window)&&buildWindow&&IsWindow(buildWindow))return true;if(failed||!module)return false;
    if(!EnsureFont()){OutputDebugStringW(L"Undercoop: DirectWrite embedded font initialization failed.\n");failed=true;return false;}
    coop_overlay::DpiScope dpiScope;RECT at{};coop_overlay::PhysicalClient(owner,at);
    WNDCLASSW klass{};klass.hInstance=module;klass.lpfnWndProc=WindowProc;klass.lpszClassName=kClass;
    if(!RegisterClassW(&klass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){failed=true;return false;}
    constexpr DWORD style=WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT;
    window=CreateWindowExW(style,kClass,L"Undercoop Console",WS_POPUP,at.left,at.top,1,1,owner,nullptr,module,nullptr);
    buildWindow=CreateWindowExW(style,kClass,L"Undercoop Build Info",WS_POPUP,at.left,at.top,1,1,owner,nullptr,module,nullptr);
    if(!window||!buildWindow)failed=true;paintedRevision=0;return !failed;
}
static bool Surface(HWND target,const RECT& rect,bool build,const View* state,float dpi,BYTE* preview=nullptr) {
    const int width=rect.right-rect.left,height=rect.bottom-rect.top;
    if(width<=0||height<=0||width>8192||height>8192||!EnsureFont())return false;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    HDC dc=CreateCompatibleDC(nullptr);if(!dc)return false;void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);bool success=false;
    if(bitmap&&pixels){const auto old=SelectObject(dc,bitmap);
        if(textRenderer.Begin(dc,width,height,dpi)) {
            const float scale=dpi/96.0f;
            if(build)DrawBuild(width/scale,height/scale);else DrawView(width/scale,height/scale,*state);
            if(textRenderer.End()) {
                if(preview){std::memcpy(preview,pixels,size_t(width)*height*4);success=true;}
                else{POINT at{rect.left,rect.top},source{};SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
                    success=UpdateLayeredWindow(target,nullptr,&at,&size,dc,&source,0,&blend,ULW_ALPHA)!=FALSE;}
            }
        }
        SelectObject(dc,old);
    }
    if(bitmap)DeleteObject(bitmap);DeleteDC(dc);return success;
}
static bool RenderView(BYTE* pixels,int width,int height,const View& state,float dpi=96) {
    return pixels&&Surface(nullptr,RECT{0,0,width,height},false,&state,dpi,pixels);
}
static bool Render(BYTE* pixels,int width,int height,const coop_notices::Model& sys,float dpi=96) {
    View state{};AcquireSRWLockShared(&lock);state={sys,chat,editor,channel};ReleaseSRWLockShared(&lock);return RenderView(pixels,width,height,state,dpi);
}
static bool RenderBuild(BYTE* pixels,int width,int height,float dpi=96) {
    return pixels&&Surface(nullptr,RECT{0,0,width,height},true,nullptr,dpi,pixels);
}
static bool Paint(HWND target,const RECT& rect,bool build,const View* state,float dpi) {
    return Surface(target,rect,build,state,dpi);
}
static void Tick(HWND game,bool focused) {
    if(!game||!IsWindow(game)||!IsWindowVisible(game)||IsIconic(game)||GetWindowThreadProcessId(game,nullptr)!=GetCurrentThreadId()) {
        if(window&&IsWindowVisible(window))ShowWindow(window,SW_HIDE);if(buildWindow&&IsWindowVisible(buildWindow))ShowWindow(buildWindow,SW_HIDE);return;
    }
    coop_overlay::DpiScope dpiScope;RECT client{};
    if(!coop_overlay::PhysicalClient(game,client)||client.right-client.left<240||client.bottom-client.top<180||!Ensure(game))return;
    // Move before querying DPI so a monitor change is rendered at the new
    // monitor's density on this frame, not upscaled from an old 96-DPI bitmap.
    const auto monitor=MonitorFromRect(&client,MONITOR_DEFAULTTONEAREST);
    for(const auto overlay:{window,buildWindow})if(MonitorFromWindow(overlay,MONITOR_DEFAULTTONEAREST)!=monitor) {
        SetWindowPos(overlay,nullptr,client.left,client.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);paintedDpi=buildDpi=0;
    }
    const float dpi=coop_overlay::WindowDpi(window),scale=dpi/96.0f;
    const float clientWidth=(client.right-client.left)/scale,clientHeight=(client.bottom-client.top)/scale;
    const auto px=[&](float value){return int(value*scale+0.5f);};
    View state{};uint32_t revision=0;bool show=false;
    AcquireSRWLockShared(&lock);state={model,chat,editor,channel};revision=model.revision+chat.revision+inputRevision;
    show=enabled&&(model.Visible(GetTickCount())||(model.autoShow!=coop_notices::AutoShow::Manual&&chat.Transient(GetTickCount())));ReleaseSRWLockShared(&lock);
    const int width=px(std::min(clientWidth-24,std::clamp(clientWidth*0.64f,460.0f,760.0f)));
    const int height=px(std::min(clientHeight-24,state.system.expanded?std::clamp(clientHeight*0.55f,380.0f,500.0f):170.0f));
    const RECT target{client.left+px(12),client.top+px(12),client.left+px(12)+width,client.top+px(12)+height};
    if(show){if(revision!=paintedRevision||paintedDpi!=dpi||std::memcmp(&target,&paintedRect,sizeof(target)))if(Paint(window,target,false,&state,dpi)){paintedRect=target;paintedRevision=revision;paintedDpi=dpi;}
        if(!IsWindowVisible(window))ShowWindow(window,SW_SHOWNOACTIVATE);
    }else if(IsWindowVisible(window))ShowWindow(window,SW_HIDE);
    const RECT build{client.left+px(12),client.bottom-px(52),client.left+px(12)+px(std::min(clientWidth-24,760.0f)),client.bottom-px(8)};
    if(!IsWindowVisible(buildWindow)||buildDpi!=dpi||std::memcmp(&build,&buildRect,sizeof(build)))if(Paint(buildWindow,build,true,nullptr,dpi)){buildRect=build;buildDpi=dpi;}
    if(!IsWindowVisible(buildWindow))ShowWindow(buildWindow,SW_SHOWNOACTIVATE);
    if(focused){if(show)SetWindowPos(window,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        SetWindowPos(buildWindow,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);}
}
}
