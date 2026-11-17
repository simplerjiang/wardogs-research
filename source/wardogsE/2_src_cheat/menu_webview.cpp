/* menu_webview.cpp — хост WebView2 для HTML-меню BLAQUE v2.
 *
 * Открывает menu.html (рядом с exe) в прозрачном WS_POPUP-окне поверх игры.
 * Мост: те же BR_Config-поля, что у GDI-меню, сериализуются в JSON и
 * гоняются через PostWebMessageAsString / WebMessageReceived.
 * Протокол (строки "cmd:json"):
 *   ->  set:<key>:<int>   тумблер/слайдер из HTML
 *   ->  col:<i>:r,g,b     цвет слота i
 *   ->  fac:<i>:0|1       флаг «цвет фракции»
 *   ->  act:save|load|def запрос конфига
 *   ->  drag:             начать перетаскивание окна (шапка)
 *   ->  lang:eng|rus      язык (сохраняем)
 *   ->  hello:/ping:      keepalive
 *   <-  state:{json}      полный конфиг при старте и по запросу
 *   <-  telem:{json}      телеметрия 2 раза/сек
 *   <-  toast:<text>      уведомление
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>

#include "menu_bridge.h"
#include "wv2_cb.h"

/* WebView2 SDK — MIDL-хедер, требует COM-хедеры до него */
#include <objbase.h>
#include <rpc.h>
#include <rpcndr.h>
#include <combaseapi.h>
#include "WebView2.h"
#include "wv2_cb_impl.h"
#pragma comment(lib, "WebView2Loader.dll.lib")

/* ---------------- окно ---------------- */
static HWND              g_hwnd   = NULL;
static ICoreWebView2Environment*         g_env  = nullptr;
static ICoreWebView2*                   g_wv   = nullptr;
static ICoreWebView2Controller*          g_ctl  = nullptr;
static ICoreWebView2WebMessageReceivedEventHandler* g_msg_h = nullptr;

static HANDLE  g_thread     = NULL;
static DWORD   g_thread_id  = 0;
static volatile LONG g_quit = 0;
static bool    g_visible    = true;
static bool    g_dragging   = false;
static POINT   g_drag_from  = {0,0};
static POINT   g_drag_origin= {0,0};   /* позиция окна на старте drag */

static const int WIN_W = 976;
static const int WIN_H = 640;

/* ---------------- утилиты JSON ---------------- */
static void jcat_int(std::string& s, const char* k, int v, bool first=false){
    char b[96]; snprintf(b,sizeof(b),"%s\"%s\":%d", first?"":",",k,v); s+=b;
}
static void jcat_f(std::string& s, const char* k, float v){
    char b[96]; snprintf(b,sizeof(b),",\"%s\":%.1f",k,v); s+=b;
}
static std::string build_state_json(BR_Config* b){
    std::string s("{");
    jcat_int(s,"enabled",b->enabled,1);
    jcat_int(s,"show_box",b->show_box);
    jcat_int(s,"show_skeleton",b->show_skeleton);
    jcat_int(s,"show_name",b->show_name);
    jcat_int(s,"show_health",b->show_health);
    jcat_int(s,"show_armor",b->show_armor);
    jcat_int(s,"show_distance",b->show_distance);
    jcat_int(s,"show_weapon",b->show_weapon);
    jcat_int(s,"show_head",b->show_head);
    jcat_int(s,"show_team",b->show_team);
    jcat_int(s,"only_visible",b->only_visible);
    jcat_int(s,"show_vehicles",b->show_vehicles);
    jcat_int(s,"veh_armed_only",b->veh_armed_only);
    jcat_int(s,"veh_show_hp",b->veh_show_hp);
    jcat_int(s,"show_lead",b->show_lead);
    jcat_int(s,"lead_numbers",b->lead_numbers);
    jcat_int(s,"lead_zeroing",b->lead_zeroing);
    jcat_int(s,"lead_vehicles",b->lead_vehicles);
    jcat_int(s,"max_distance",b->max_distance);
    jcat_int(s,"skel_distance",b->skel_distance);
    jcat_int(s,"dim_pct",b->dim_pct);
    jcat_int(s,"line_w",b->line_w);
    jcat_int(s,"show_l81_users",b->show_l81_users);
    jcat_int(s,"show_proj_trail",b->show_proj_trail);
    jcat_int(s,"cham_on",b->cham_on);
    jcat_int(s,"cham_team",b->cham_team);
    /* Aim assist */
    jcat_int(s,"aa_on",b->aa_on);
    jcat_int(s,"aa_show_fov",b->aa_show_fov);
    jcat_int(s,"aa_fov_px",b->aa_fov_px);
    jcat_int(s,"aa_key",b->aa_key);
    jcat_int(s,"aa_smooth",b->aa_smooth);
    jcat_int(s,"aa_visible",b->aa_visible);
    jcat_int(s,"aa_dist",b->aa_dist);
    jcat_int(s,"aa_team",b->aa_team);
    jcat_int(s,"aa_head",b->aa_head);
    jcat_int(s,"aa_zoom_mode",b->aa_zoom_mode);
    jcat_int(s,"aa_gain_ref_x100",b->aa_gain_ref_x100);
    jcat_int(s,"telem_web_posted",b->telem_web_posted);
    jcat_int(s,"telem_web_code",b->telem_web_code);
    jcat_int(s,"telem_web_tgt",b->telem_web_tgt);
    s += ",\"colors\":{";
    for(int i=0;i<BR_COLOR_COUNT;i++){
        char kv[32]; snprintf(kv,sizeof(kv),"%s\"%d\":[%d,%d,%d,%d]",
            i?",":"",i,
            b->color[i].rgb[0],b->color[i].rgb[1],b->color[i].rgb[2],
            b->color[i].use_fac);
        s += kv;
    }
    s += "}}";
    return s;
}
static std::string build_telem_json(BR_Config* b){
    std::string s("{");
    jcat_int(s,"pid",b->telem_game_pid,1);
    jcat_int(s,"targets",b->telem_targets);
    jcat_f (s,"fps",b->telem_fps);
    jcat_int(s,"l81",b->telem_l81_found);
    jcat_int(s,"cham_mat",b->telem_cham_mat);
    jcat_int(s,"cham_n",b->telem_cham_n);
    jcat_int(s,"wposted",b->telem_web_posted);
    jcat_int(s,"wcode",b->telem_web_code);
    jcat_int(s,"wtgt",b->telem_web_tgt);
    jcat_int(s,"aa",b->telem_aa_tgt);
    jcat_int(s,"aafov",b->telem_aa_fov);
    jcat_int(s,"aazoom",b->telem_aa_zoom_x100);
    s += "}";
    return s;
}

/* ---------------- отправка в HTML ---------------- */
static void wv_send(const std::string& msg){
    if(!g_wv) return;
    std::wstring w(msg.begin(), msg.end());
    g_wv->PostWebMessageAsString(w.c_str());
}

/* поле по имени (только int-поля конфига) */
static bool cfg_set(BR_Config* b, const char* k, int v){
    struct F{ const char* n; size_t off; };
    static const F fields[] = {
        {"enabled",offsetof(BR_Config,enabled)},{"show_box",offsetof(BR_Config,show_box)},
        {"show_skeleton",offsetof(BR_Config,show_skeleton)},{"show_name",offsetof(BR_Config,show_name)},
        {"show_health",offsetof(BR_Config,show_health)},
        {"show_armor",offsetof(BR_Config,show_armor)},{"show_distance",offsetof(BR_Config,show_distance)},
        {"show_weapon",offsetof(BR_Config,show_weapon)},{"show_head",offsetof(BR_Config,show_head)},
        {"show_team",offsetof(BR_Config,show_team)},{"only_visible",offsetof(BR_Config,only_visible)},
        {"show_l81_users",offsetof(BR_Config,show_l81_users)},
        {"show_proj_trail",offsetof(BR_Config,show_proj_trail)},
        {"show_vehicles",offsetof(BR_Config,show_vehicles)},{"veh_armed_only",offsetof(BR_Config,veh_armed_only)},
        {"veh_show_hp",offsetof(BR_Config,veh_show_hp)},{"show_lead",offsetof(BR_Config,show_lead)},
        {"lead_numbers",offsetof(BR_Config,lead_numbers)},{"lead_zeroing",offsetof(BR_Config,lead_zeroing)},
        {"lead_vehicles",offsetof(BR_Config,lead_vehicles)},{"max_distance",offsetof(BR_Config,max_distance)},
        {"skel_distance",offsetof(BR_Config,skel_distance)},{"dim_pct",offsetof(BR_Config,dim_pct)},
        {"line_w",offsetof(BR_Config,line_w)},
        {"cham_on",offsetof(BR_Config,cham_on)},{"cham_team",offsetof(BR_Config,cham_team)},
        {"aa_on",offsetof(BR_Config,aa_on)},{"aa_show_fov",offsetof(BR_Config,aa_show_fov)},
        {"aa_fov_px",offsetof(BR_Config,aa_fov_px)},{"aa_key",offsetof(BR_Config,aa_key)},
        {"aa_smooth",offsetof(BR_Config,aa_smooth)},{"aa_visible",offsetof(BR_Config,aa_visible)},
        {"aa_dist",offsetof(BR_Config,aa_dist)},{"aa_team",offsetof(BR_Config,aa_team)},
        {"aa_head",offsetof(BR_Config,aa_head)},{"aa_zoom_mode",offsetof(BR_Config,aa_zoom_mode)},
        {"aa_gain_ref_x100",offsetof(BR_Config,aa_gain_ref_x100)},
        {"aa_gain_x100",offsetof(BR_Config,aa_gain_x100)},{"aa_gain_y100",offsetof(BR_Config,aa_gain_y100)},
    };
    for(auto&f:fields) if(!strcmp(f.n,k)){ *(int*)((char*)b+f.off)=v; return true; }
    return false;
}

/* ---------------- WebMessageReceived ---------------- */
static HRESULT STDMETHODCALLTYPE msg_cb(ICoreWebView2* sender,
    ICoreWebView2WebMessageReceivedEventArgs* args, void* ctx)
{
    (void)sender;(void)ctx;
    if(!args) return S_OK;
    LPWSTR w = nullptr;
    if(FAILED(args->TryGetWebMessageAsString(&w)) || !w) return S_OK;
    std::string msg;
    for(const wchar_t* p=w; *p; ++p){
        if(*p<128) msg+=(char)*p;
    }
    CoTaskMemFree(w);
    BR_Config* b = br_get();

    /* parse "cmd:body" */
    size_t c1 = msg.find(':');
    if(c1==std::string::npos) return S_OK;
    std::string cmd = msg.substr(0,c1), body = msg.substr(c1+1);

    if(cmd=="set"){
        size_t c2=body.find(':');
        if(c2!=std::string::npos){
            cfg_set(b, body.substr(0,c2).c_str(), atoi(body.substr(c2+1).c_str()));
            b->cfg_dirty = 1;
        }
    }
    else if(cmd=="col" || cmd=="fac"){
        size_t c2=body.find(':');
        if(c2!=std::string::npos){
            int idx=atoi(body.substr(0,c2).c_str());
            std::string val=body.substr(c2+1);
            if(idx>=0 && idx<BR_COLOR_COUNT){
                if(cmd=="col"){
                    int r,g,bl; sscanf(val.c_str(),"%d,%d,%d",&r,&g,&bl);
                    b->color[idx].rgb[0]=(unsigned char)r;
                    b->color[idx].rgb[1]=(unsigned char)g;
                    b->color[idx].rgb[2]=(unsigned char)bl;
                    b->color[idx].use_fac=0;
                    b->cfg_dirty=1;
                } else {
                    b->color[idx].use_fac=(unsigned char)atoi(val.c_str());
                    b->cfg_dirty=1;
                }
            }
        }
    }
    else if(cmd=="act"){
        if(body=="save") b->req_save=1;
        else if(body=="load") b->req_load=1;
        else if(body=="def") b->req_defaults=1;
    }
    else if(cmd=="drag"){
        /* HTML шлёт экранные координаты курсора и дельту от старта drag.
           Дельта ПОЛНАЯ (от точки старта), поэтому окно ставится в
           START+delta, а не текущее+delta — иначе окно «разгоняется»
           с каждым move и улетает за экран. */
        int sx=0, sy=0, dx=0, dy=0;
        if(sscanf(body.c_str(),"%d,%d,%d,%d",&sx,&sy,&dx,&dy)==4){
            if(!g_dragging){
                g_dragging=true;
                /* запомнить окно на старте и курсор (экранные) */
                RECT wr; GetWindowRect(g_hwnd,&wr);
                g_drag_origin.x=wr.left; g_drag_origin.y=wr.top;
            }
            SetWindowPos(g_hwnd,NULL,
                         g_drag_origin.x+dx, g_drag_origin.y+dy,
                         0,0, SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            (void)sx; (void)sy;
        }
    }
    else if(cmd=="dragend"){
        g_dragging=false;
        g_drag_origin.x=g_drag_origin.y=0;
    }
    else if(cmd=="close"){
        br_get()->menu_open=0;
        g_visible=false;
    }
    else if(cmd=="col" || cmd=="fac" || cmd=="hello" || cmd=="ping"){
        if(cmd!="hello" && cmd!="ping"){
            FILE* df = fopen("E:\Project\wardogs\coldbg.txt","a");
            if(df){ SYSTEMTIME st3; GetLocalTime(&st3);
                     fprintf(df, "%02d:%02d:%02d.%03d IN=[%s]\n", st3.wHour,st3.wMinute,st3.wSecond,st3.wMilliseconds, msg.c_str());
                     fclose(df); }
        }
        std::string st = build_state_json(b);
        /* ДЕБАГ: следим за слотами 10 (head) и 11 (HP hi) — кто их обнуляет */
        {
            FILE* df = fopen("E:\Project\wardogs\coldbg.txt","a");
            if(df){
                SYSTEMTIME st2; GetLocalTime(&st2);
                size_t p10 = st.find("\"10\":");
                size_t p11 = st.find("\"11\":");
                fprintf(df, "%02d:%02d:%02d.%03d msg=[%s] slot10=%s slot11=%s\n",
                    st2.wHour,st2.wMinute,st2.wSecond,st2.wMilliseconds,
                    msg.c_str(),
                    p10!=std::string::npos? st.substr(p10,24).c_str():"?",
                    p11!=std::string::npos? st.substr(p11,24).c_str():"?");
                fclose(df);
            }
        }
        wv_send("state:"+st);
    }
    return S_OK;
}

/* handler через лёгкий колбэк-компоновщик */

/* ---------------- перетаскивание окна ---------------- */
static LRESULT WINAPI wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l){
    switch(m){
    case WM_DESTROY: PostQuitMessage(0); break;
    }
    return DefWindowProcW(h,m,w,l);
}

/* ---------------- поток окна ---------------- */
static DWORD WINAPI menu_thread(LPVOID){
    /* WebView2 requires COM on this thread. Without it
       CreateCoreWebView2EnvironmentWithOptions returns S_OK and the callback
       then fails with CO_E_NOTINITIALIZED (0x800401F0) - the window appears and
       stays blank, which reads as "the menu does not open". */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc={0};
    wc.cbSize=sizeof(wc); wc.style=CS_CLASSDC;
    wc.lpfnWndProc=wnd_proc; wc.hInstance=GetModuleHandleW(NULL);
    wc.lpszClassName=L"mrzvCoreWebMenu";
    wc.hCursor=LoadCursorW(NULL,(LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);

    int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
    g_hwnd=CreateWindowExW(
        WS_EX_TOPMOST|WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"mrzvCore",
        WS_POPUP,
        sw/2-WIN_W/2, 80, WIN_W, WIN_H,
        NULL,NULL,wc.hInstance,NULL);
    if(!g_hwnd) return 1;

    /* Showing a topmost window takes the foreground with it, and the game then
       looks dead until it is clicked again. Remember who owned the foreground
       before the window existed and hand it back right after the first show.
       (SW_SHOWNOACTIVATE was tried here and stops WebView2 from painting, so the
       focus is returned instead of suppressed.) */
    /* SHOWNOACTIVATE keeps the game in the foreground. This was blamed for the
       blank menu earlier, but the real cause was the missing CoInitializeEx on
       this thread - WebView2 never painted at all. */
    ShowWindow(g_hwnd,SW_SHOWNOACTIVATE);

    /* WebView2: создаём среду -> контроллер -> WebView */
    auto on_env = [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
        { FILE* ef=fopen("E:/Project/wardogs/wv2_error.txt","a");
          if(ef){ fprintf(ef,"on_env hr=0x%08X env=%p\n",(unsigned)hr,(void*)env); fclose(ef); } }
        if(FAILED(hr)||!env) return hr;
        g_env=env; env->AddRef();
        auto on_cti = [](HRESULT hr2, ICoreWebView2Controller* ctl) -> HRESULT {
            if(FAILED(hr2)||!ctl) return hr2;
            g_ctl=ctl; ctl->AddRef();
            ICoreWebView2* wv=nullptr;
            if(SUCCEEDED(ctl->get_CoreWebView2(&wv)) && wv){
                g_wv=wv;
                /* прозрачный фон */
                ICoreWebView2Controller2* ctl2=nullptr;
                if(SUCCEEDED(ctl->QueryInterface(&ctl2))){
                    COREWEBVIEW2_COLOR bg={0,0,0,0};
                    ctl2->put_DefaultBackgroundColor(bg);
                    ctl2->Release();
                }
                RECT rc; GetClientRect(g_hwnd,&rc);
                ctl->put_Bounds(rc);
                /* навигация на локальный файл */
                wchar_t path[MAX_PATH]; GetModuleFileNameW(NULL,path,MAX_PATH);
                wchar_t* slash=wcsrchr(path,L'\\'); if(slash) *slash=0;
                wcscat_s(path,L"\\menu.html");
                std::wstring url=L"file:///"+std::wstring(path);
                for(auto&c:url) if(c==L'\\') c=L'/';
                wv->Navigate(url.c_str());

                auto h = MakeMsgCb([](ICoreWebView2* s,
                        ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                    return msg_cb(s,a,nullptr);
                });
                EventRegistrationToken tok{0};
                wv->add_WebMessageReceived(h, &tok);
                h->Release();
                wv_send("state:"+build_state_json(br_get()));
            }
            return S_OK;
        };
        return env->CreateCoreWebView2Controller(g_hwnd, MakeCtlCb(on_cti));
    };

    /* Explicit user-data folder. With nullptr WebView2 derives the folder from
       the executable path, and a single orphaned msedgewebview2.exe from an
       earlier run keeps it locked - the environment then fails to create and the
       menu never appears, with nothing on screen to say why. A private folder
       plus a per-process fallback makes the menu survive that. */
    wchar_t dataDir[MAX_PATH];
    DWORD dn = GetEnvironmentVariableW(L"LOCALAPPDATA", dataDir, MAX_PATH);
    if (dn == 0 || dn >= MAX_PATH) wcscpy_s(dataDir, MAX_PATH, L"C:\ProgramData\vanta");
    wcscat_s(dataDir, MAX_PATH, L"/mrzvCore_webview2");

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir, nullptr, MakeEnvCb(on_env));
    if (FAILED(hr)) {
        /* Locked by a leftover process: retry with a folder nobody else owns. */
        wchar_t alt[MAX_PATH];
        swprintf_s(alt, MAX_PATH, L"%s_p%lu", dataDir, (unsigned long)GetCurrentProcessId());
        hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, alt, nullptr, MakeEnvCb(on_env));
    }
    { FILE* ef=fopen("E:/Project/wardogs/wv2_error.txt","a");
      if(ef){ fprintf(ef,"create hr=0x%08X dir=%ls\n",(unsigned)hr,dataDir); fclose(ef); } }
    if (FAILED(hr)){ DestroyWindow(g_hwnd); return 2; }

    BR_Config* b = br_get();
    DWORD last_telem=0, last_vis=0;

    MSG msg; ZeroMemory(&msg,sizeof(msg));
    while(msg.message!=WM_QUIT && !g_quit){
        if(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){
            TranslateMessage(&msg); DispatchMessageW(&msg); continue;
        }
        if(!g_visible){ ShowWindow(g_hwnd,SW_HIDE); Sleep(80); continue; }
        if(!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd,SW_SHOWNOACTIVATE);

        if(b->menu_close_req){ b->menu_close_req=0; b->menu_open=0; }
        DWORD now=GetTickCount();
        if(g_wv && now-last_telem>500){
            last_telem=now;
            wv_send("telem:"+build_telem_json(b));
        }
        Sleep(15);
    }

    if(g_msg_h){ g_msg_h->Release(); g_msg_h=nullptr; }
    if(g_ctl){ g_ctl->Close(); g_ctl->Release(); g_ctl=nullptr; }
    if(g_wv){ g_wv->Release(); g_wv=nullptr; }
    if(g_env){ g_env->Release(); g_env=nullptr; }
    DestroyWindow(g_hwnd);
    UnregisterClassW(L"mrzvCoreWebMenu",GetModuleHandleW(NULL));
    return 0;
}

/* ---------------- мост ---------------- */
static BR_Config g_br_inst;
static volatile LONG g_br_inited = 0;

BR_Config* br_get(void){
    if(InterlockedCompareExchange(&g_br_inited,1,0)==0){
        ZeroMemory(&g_br_inst,sizeof(g_br_inst));
        g_br_inst.magic=BR_MAGIC; g_br_inst.version=BR_VERSION;
        g_br_inst.enabled=1; g_br_inst.show_box=1; g_br_inst.show_skeleton=1;
        g_br_inst.show_name=1; g_br_inst.show_health=1; g_br_inst.show_distance=1;
        g_br_inst.show_weapon=1; g_br_inst.show_armor=1; g_br_inst.show_head=1;
        g_br_inst.max_distance=30000; g_br_inst.skel_distance=15000;
        g_br_inst.menu_open=1; g_br_inst.dim_pct=50;
        /* Aim assist defaults: off, ALT to hold, 150 px circle. */
        g_br_inst.aa_on=0; g_br_inst.aa_show_fov=1; g_br_inst.aa_fov_px=150;
        g_br_inst.aa_key=0x12;      /* VK_MENU = ALT */
        g_br_inst.aa_smooth=5; g_br_inst.aa_visible=1; g_br_inst.aa_dist=0;
        g_br_inst.aa_team=0; g_br_inst.aa_head=1;
        g_br_inst.aa_zoom_mode=2;       /* AUTO: zone from the measured scale */
        g_br_inst.aa_gain_x100=2000; g_br_inst.aa_gain_y100=2000;
        static const unsigned char pal[BR_COLOR_COUNT][3] = {
            { 86,168,255},{255, 86, 86},{120,255,140},{ 86,255,246},
            {158,158,158},{255,255,255},{255,255,255},{255,255,255},
            {255,255,255},{255,255,255},{255,255,255},{120,255,140},
            {255,176, 66},{139, 92,246},{220,220,220},
        };
        for(int i=0;i<BR_COLOR_COUNT;i++){
            g_br_inst.color[i].rgb[0]=pal[i][0];
            g_br_inst.color[i].rgb[1]=pal[i][1];
            g_br_inst.color[i].rgb[2]=pal[i][2];
            g_br_inst.color[i].use_fac=0;
        }
    }
    return &g_br_inst;
}

int br_start_menu(void){
    if(g_thread) return 0;
    g_quit=0;
    g_thread=CreateThread(NULL,0,menu_thread,NULL,0,&g_thread_id);
    return g_thread?0:1;
}
void br_stop_menu(void){
    if(!g_thread) return;
    g_quit=1;
    if(g_hwnd) PostMessageW(g_hwnd,WM_CLOSE,0,0);
    WaitForSingleObject(g_thread,3000);
    CloseHandle(g_thread); g_thread=NULL;
}
static HWND g_prev_fg = NULL;

void br_request_toggle(void){
    g_visible=!g_visible;
    br_get()->menu_open = g_visible?1:0;
    if(g_visible){
        /* Remember who owned the foreground so closing the menu returns it.
           Without this the game stays unfocused after the menu is hidden -
           keystrokes land in a hidden window and the game looks dead. */
        g_prev_fg = GetForegroundWindow();
        if(g_hwnd) SetForegroundWindow(g_hwnd);
    } else if(g_prev_fg && IsWindow(g_prev_fg) && g_prev_fg!=g_hwnd){
        SetForegroundWindow(g_prev_fg);
        g_prev_fg = NULL;
    }
}
