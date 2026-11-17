/* Appended to the generated copy of wd_esp_full.c. No game or driver is opened.
 * Pipe: command + address/size/hash only. Scene data travels via real RPM.
 * The child holds each snapshot stable until the next parent command.
 */
#include <stddef.h>

static BR_Config controlled_config;
BR_Config* br_get(void) { return &controlled_config; }
int br_start_menu(void) { abort(); return 1; }
void br_stop_menu(void) { abort(); }
void br_request_toggle(void) { abort(); }
int controlled_gpu_pixels(unsigned char* pixels, int width, int height);

typedef struct {
    unsigned sequence;
    V3 camera;
    ROT rotation;
    double fov;
    int count;
    ENT entities[3];
} ControlledFrame;
typedef struct { unsigned sequence, scenario; } ControlledCommand;
typedef struct { ULONG_PTR address; SIZE_T size; uint64_t hash; } ControlledReply;

static HANDLE command_pipe, reply_pipe, child_handle;
static DWORD child_pid;
static int source_exited, automatic, transparent_mode, step, failed, frames;
static ULONGLONG preview_started;
static unsigned current_case, sequence;
static ControlledFrame frame;
static FILE* report;
static HWND test_window;
static char evidence_path[MAX_PATH];
static unsigned char* image_pixels;
static int pixel_width, pixel_height;
static double baseline_x, moved_x;
static unsigned expected_failures;

#define VERIFY(x) do { if (!(x)) { \
    if (report) { fprintf(report,"FAIL line %d: %s (win32=%lu)\n",__LINE__,#x,GetLastError()); fflush(report); } \
    failed=1; return 0; } } while(0)

static int transfer(HANDLE pipe, void* buffer, DWORD size, int writing) {
    DWORD total=0;
    while(total<size) {
        DWORD amount=0;
        BOOL ok=writing ? WriteFile(pipe,(char*)buffer+total,size-total,&amount,NULL)
                        : ReadFile(pipe,(char*)buffer+total,size-total,&amount,NULL);
        if(!ok || !amount) return 0;
        total+=amount;
    }
    return 1;
}
static uint64_t frame_hash(const void* data, size_t size) {
    const unsigned char* bytes=(const unsigned char*)data;
    uint64_t hash=14695981039346656037ULL;
    for(size_t i=0;i<size;i++) { hash^=bytes[i]; hash*=1099511628211ULL; }
    return hash;
}
static void populate(ControlledFrame* f, ControlledCommand command, unsigned char raw[3][16384]) {
    static const V3 joints[B__COUNT]={
        {0,0,85},{0,0,105},{0,0,130},{0,0,150},{0,0,175},
        {0,-15,145},{0,-30,135},{0,-45,115},{0,-55,100},
        {0,15,145},{0,30,135},{0,45,115},{0,55,100},
        {0,-12,80},{0,-14,45},{0,-18,0},{0,12,80},{0,14,45},{0,18,0}};
    memset(f,0,sizeof(*f));
    f->sequence=command.sequence;
    f->camera=(V3){0,command.scenario==2 ? 70.0 : 0.0,90};
    f->fov=90; f->count=3;
    for(int i=0;i<3;i++) {
        ENT* e=&f->entities[i];
        e->pawn=(uint64_t)(ULONG_PTR)raw[i];
        e->root=(V3){i==2 ? -500 : 500+i*100,
            i==0 ? (command.scenario==1 ? -15 : -170) : 185,0};
        if(command.scenario==6 && i==0) e->root.y+=50*sin(command.sequence*0.08);
        e->fac_slot=1; e->fac_cls=i; e->visible=1;
        e->hp_valid=1; e->hp=(i==0 && command.scenario==1)?35:90;
        e->maxhp=100; e->bone_cnt=B__COUNT;
        snprintf(e->name,sizeof(e->name),"CONTROL %c",'A'+i);
        double dx=e->root.x-f->camera.x, dy=e->root.y-f->camera.y, dz=e->root.z-f->camera.z;
        e->dist=sqrt(dx*dx+dy*dy+dz*dz);
        for(int b=0;b<B__COUNT;b++) {
            e->bones[b]=(V3){e->root.x+joints[b].x,e->root.y+joints[b].y,e->root.z+joints[b].z};
            e->bone_ok[b]=1;
        }
    }
}
static int child_main(void) {
    HANDLE input=GetStdHandle(STD_INPUT_HANDLE), output=GetStdHandle(STD_OUTPUT_HANDLE);
    ControlledFrame* own=(ControlledFrame*)VirtualAlloc(NULL,sizeof(*own),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    static unsigned char raw[3][16384];
    if(!own) return 10;
    ControlledCommand command;
    while(transfer(input,&command,sizeof(command),0)) {
        if(command.scenario==99) break;
        DWORD old;
        if(!VirtualProtect(own,sizeof(*own),PAGE_READWRITE,&old)) return 11;
        populate(own,command,raw);
        ControlledReply reply={(ULONG_PTR)own,sizeof(*own),frame_hash(own,sizeof(*own))};
        if(command.scenario==3 && !VirtualProtect(own,sizeof(*own),PAGE_NOACCESS,&old)) return 12;
        if(!transfer(output,&reply,sizeof(reply),1)) return 13;
    }
    VirtualFree(own,0,MEM_RELEASE);
    return 0;
}
static int start_child(void) {
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    HANDLE child_input=NULL,child_output=NULL;
    VERIFY(CreatePipe(&child_input,&command_pipe,&sa,0));
    VERIFY(CreatePipe(&reply_pipe,&child_output,&sa,0));
    VERIFY(SetHandleInformation(command_pipe,HANDLE_FLAG_INHERIT,0));
    VERIFY(SetHandleInformation(reply_pipe,HANDLE_FLAG_INHERIT,0));
    char exe[MAX_PATH], command[MAX_PATH+32];
    VERIFY(GetModuleFileNameA(NULL,exe,sizeof(exe))>0);
    snprintf(command,sizeof(command),"\"%s\" --child",exe);
    STARTUPINFOA si={sizeof(si)};
    si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdInput=child_input; si.hStdOutput=child_output; si.hStdError=child_output;
    PROCESS_INFORMATION pi={0};
    BOOL created=CreateProcessA(exe,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi);
    CloseHandle(child_input); CloseHandle(child_output);
    VERIFY(created);
    child_handle=pi.hProcess; child_pid=pi.dwProcessId; CloseHandle(pi.hThread);
    controlled_process=OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,child_pid);
    VERIFY(controlled_process && child_pid!=GetCurrentProcessId());
    fprintf(report,"REAL parent_pid=%lu child_pid=%lu transport=ReadProcessMemory\n",GetCurrentProcessId(),child_pid);
    return 1;
}
static int stop_child(void) {
    if(source_exited) return 1;
    ControlledCommand command={0,99};
    VERIFY(transfer(command_pipe,&command,sizeof(command),1));
    VERIFY(WaitForSingleObject(child_handle,5000)==WAIT_OBJECT_0);
    DWORD status=1;
    VERIFY(GetExitCodeProcess(child_handle,&status) && status==0);
    source_exited=1;
    return 1;
}
static int acquire_frame(unsigned scenario) {
    static ControlledReply reply;
    if(!source_exited) {
        ControlledCommand command={++sequence,scenario};
        VERIFY(transfer(command_pipe,&command,sizeof(command),1));
        VERIFY(transfer(reply_pipe,&reply,sizeof(reply),0));
        VERIFY(reply.size==sizeof(frame));
    }
    ControlledFrame received;
    int ok=rd(reply.address,&received,sizeof(received));
    if(scenario==3 || source_exited) {
        VERIFY(!ok);
        // Test adapter policy: discard the old rendered snapshot on a read failure.
        // This is not a claim about the game's original reader recovery path.
        memset(&frame,0,sizeof(frame)); g_nrender=0; expected_failures++;
        fprintf(report,"EXPECTED_READ_FAILURE case=%u error=%lu old_frame_cleared=1\n",scenario,controlled_read_error);
        return 1;
    }
    VERIFY(ok && received.sequence==sequence);
    VERIFY(frame_hash(&received,sizeof(received))==reply.hash);
    frame=received;
    memcpy(g_render,frame.entities,sizeof(frame.entities));
    g_nrender=frame.count; g_rcam=frame.camera; g_rcamrot=frame.rotation; g_rfov=frame.fov;
    return 1;
}
static int save_bitmap(const char* name) {
    char path[MAX_PATH]; snprintf(path,sizeof(path),"%s\\%s.bmp",evidence_path,name);
    FILE* output=fopen(path,"wb"); VERIFY(output);
    BITMAPFILEHEADER file={0}; BITMAPINFOHEADER info={0};
    file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(info);
    file.bfSize=file.bfOffBits+(DWORD)(g_sw*g_sh*4);
    info.biSize=sizeof(info); info.biWidth=g_sw; info.biHeight=-g_sh;
    info.biPlanes=1; info.biBitCount=32; info.biCompression=BI_RGB;
    int ok=fwrite(&file,sizeof(file),1,output)==1 && fwrite(&info,sizeof(info),1,output)==1 &&
        fwrite(image_pixels,(size_t)g_sw*g_sh*4,1,output)==1;
    int closed=fclose(output); VERIFY(ok && closed==0);
    return 1;
}
static int colored_pixels(int x0,int y0,int x1,int y1) {
    int count=0;
    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++) {
        if(x<0||y<0||x>=g_sw||y>=g_sh) continue;
        const unsigned char* p=image_pixels+((size_t)y*g_sw+x)*4;
        if(p[0]>55||p[1]>55||p[2]>55) count++;
    }
    return count;
}
static int check_pixels(unsigned scenario) {
    // A blank pixel must carry real alpha, not an opaque background/color key.
    VERIFY(image_pixels[((size_t)160*g_sw+10)*4+3]==(transparent_mode?0:255));
    if(transparent_mode) {
        size_t clear=0, total=(size_t)g_sw*g_sh;
        for(size_t i=0;i<total;i++) if(!image_pixels[i*4+3]) clear++;
        VERIFY(clear>total*9/10);
    }
    if(scenario==3 || source_exited) {
        VERIFY(colored_pixels(0,160,g_sw,g_sh-65)==0);
        return 1;
    }
    int visible=0;
    for(int i=0;i<3;i++) {
        const ENT* e=&frame.entities[i];
        if(e->root.x<0) {
            for(int b=0;b<B__COUNT;b++) VERIFY(!g_scr[i].bok[b]);
            continue;
        }
        visible++;
        // Independent oracle for this zero-rotation, 90-degree synthetic camera.
        double x=g_sw*0.5+(e->bones[B_HEAD].y-frame.camera.y)/e->bones[B_HEAD].x*g_sw*0.5;
        double y=g_sh*0.5-(e->bones[B_HEAD].z-frame.camera.z)/e->bones[B_HEAD].x*g_sw*0.5;
        VERIFY(fabs(g_scr[i].sx[B_HEAD]-x)<0.01 && fabs(g_scr[i].sy[B_HEAD]-y)<0.01);
        VERIFY(colored_pixels((int)x-8,(int)y-8,(int)x+9,(int)y+9)>12);
        // Check neck-to-spine pixels, not just a test-drawn marker or text label.
        int spine_y=(int)((g_scr[i].sy[B_NECK]+g_scr[i].sy[B_SPINE3])*0.5);
        VERIFY(colored_pixels((int)x-3,spine_y-3,(int)x+4,spine_y+4)>2);
        double minx=1e9,miny=1e9,maxy=-1e9;
        for(int b=0;b<B__COUNT;b++) {
            if(g_scr[i].sx[b]<minx) minx=g_scr[i].sx[b];
            if(g_scr[i].sy[b]<miny) miny=g_scr[i].sy[b];
            if(g_scr[i].sy[b]>maxy) maxy=g_scr[i].sy[b];
        }
        int hp_x=(int)minx-6, hp_middle=(int)((miny+maxy)*0.5), hp_bottom=(int)maxy-3;
        VERIFY(colored_pixels(hp_x,hp_bottom,hp_x+2,hp_bottom+1)>0);
        int middle_filled=colored_pixels(hp_x,hp_middle,hp_x+2,hp_middle+1)>0;
        VERIFY(middle_filled==(e->hp>50));
        if(i==0) {
            if(scenario==0) baseline_x=x;
            if(scenario==1) { moved_x=x; VERIFY(moved_x-baseline_x>100); }
            if(scenario==2) VERIFY(x<baseline_x-50);
        }
    }
    VERIFY(visible==2 && g_ndirty==2);
    if(scenario==1) VERIFY(colored_pixels((int)baseline_x-8,g_sh/2-25,(int)baseline_x+8,g_sh/2+25)==0);
    return 1;
}
static int paint_frame(const char* capture) {
    RECT rect; GetClientRect(test_window,&rect);
    g_sw=rect.right; g_sh=rect.bottom;
    if(!g_sw||!g_sh) return 1;
    g_raspect=(float)g_sw/g_sh; gpu_size(g_sw,g_sh);
    gpu_begin();
    if(!transparent_mode) r_fill(0,0,(float)g_sw,(float)g_sh,RRGB(16,22,32));
    DrawOverlay(NULL,g_sw,g_sh);
    gpu_font(23);
    r_text(26,20,"CONTROLLED PROCESS -> MEMORY READ -> GPU",RRGB(225,235,250),RT_LEFT);
    gpu_font(14);
    char text[240];
    snprintf(text,sizeof(text),"REAL ReadProcessMemory | child PID %lu | sequence %u | case %u",child_pid,sequence,current_case);
    r_text(26,56,text,RRGB(100,200,245),RT_LEFT);
    r_text(26,80,"SYNTHETIC entities/camera | original project_all + DrawOverlay + gpu.cpp | NO DRIVER",RRGB(160,174,195),RT_LEFT);
    r_text(26,104,source_exited?"SOURCE EXITED - old frame cleared":current_case==3?"PAGE_NOACCESS - read failed, old frame cleared":"Snapshot hash matched child memory; camera-behind entity is excluded",RRGB(220,195,100),RT_LEFT);
    r_text(26,(float)g_sh-42,transparent_mode?
        (automatic?"TRANSPARENT / BORDERLESS / TOPMOST | automatic six-scenario test":
                   "TRANSPARENT / BORDERLESS / TOPMOST | animated preview, closes after 15 seconds"):
        "DIAGNOSTIC ONLY | 1 Normal  2 Move+HP  3 Camera  4 Unreadable  5 Exit source  6 Animate  Esc Close",
        RRGB(160,174,195),RT_LEFT);
    if(capture) {
        if(pixel_width!=g_sw || pixel_height!=g_sh) {
            free(image_pixels); image_pixels=(unsigned char*)malloc((size_t)g_sw*g_sh*4);
            pixel_width=g_sw; pixel_height=g_sh;
        }
        VERIFY(image_pixels && controlled_gpu_pixels(image_pixels,g_sw,g_sh));
        VERIFY(save_bitmap(capture));
        VERIFY(check_pixels(current_case));
        fprintf(report,"PASS frame=%s seq=%u case=%u entities=%d gpu_pixels=verified client=%dx%d\n",capture,sequence,current_case,g_nrender,g_sw,g_sh);
        fflush(report);
    }
    gpu_present(); frames++;
    return 1;
}
static LRESULT CALLBACK ControlledWindow(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    switch(message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd,&ps); EndPaint(hwnd,&ps); return 0; }
    case WM_KEYDOWN:
        if(wparam==VK_ESCAPE) DestroyWindow(hwnd);
        else if(wparam>='1' && wparam<='6') {
            if(wparam=='5') { if(!stop_child()) DestroyWindow(hwnd); }
            else if(!source_exited) current_case=wparam=='6'?6:(unsigned)(wparam-'1');
        }
        return 0;
    case WM_TIMER: {
        if(transparent_mode && !automatic && GetTickCount64()-preview_started>=15000) {
            DestroyWindow(hwnd); return 0;
        }
        if(automatic) {
            static const unsigned scenarios[]={0,1,2,3,0,4};
            static const char* names[]={"01-normal","02-moved","03-camera","04-unreadable","05-recovered","06-exited"};
            current_case=scenarios[step];
            if(step==5 && !stop_child()) { DestroyWindow(hwnd); return 0; }
            if(!acquire_frame(current_case) || !paint_frame(names[step])) { DestroyWindow(hwnd); return 0; }
            if(++step==6) DestroyWindow(hwnd);
        } else {
            if(!acquire_frame(current_case) || !paint_frame(NULL)) DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_DESTROY: KillTimer(hwnd,1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd,message,wparam,lparam);
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR arguments,int show) {
    (void)previous; (void)arguments; (void)show;
    if(__argc==2 && !strcmp(__argv[1],"--child")) return child_main();
    if(__argc!=3 || (strcmp(__argv[1],"--self-test") && strcmp(__argv[1],"--preview") &&
        strcmp(__argv[1],"--overlay-test") && strcmp(__argv[1],"--overlay-preview") &&
        strcmp(__argv[1],"--diagnostic-test") && strcmp(__argv[1],"--diagnostic-preview"))) return 2;
    transparent_mode=strncmp(__argv[1],"--diagnostic-",13)!=0;
    automatic=!strcmp(__argv[1],"--self-test") || !strcmp(__argv[1],"--overlay-test") ||
              !strcmp(__argv[1],"--diagnostic-test");
    snprintf(evidence_path,sizeof(evidence_path),"%s",__argv[2]);
    char path[MAX_PATH]; snprintf(path,sizeof(path),"%s\\%s.log",evidence_path,automatic?"self-test":"preview");
    report=fopen(path,"w"); if(!report) return 3;
    if(!start_child()) goto cleanup;
    SetProcessDPIAware();
    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    pal_bind(); cfg_defaults(); InitGdiCache();
    g_cfg_enabled=1; g_cfg_show_team=1; g_cfg_show_box=1; g_cfg_show_skeleton=1;
    g_cfg_show_name=1; g_cfg_show_distance=1; g_cfg_show_health=1; g_cfg_show_head=1;
    g_cfg_show_weapon=0; g_cfg_show_armor=0; g_cfg_show_l81_users=0;
    g_cfg_show_vehicles=0; g_cfg_show_lead=0; g_cfg_cham_on=0; g_cfg_aa_on=0; g_cfg_aa_fov=0;
    g_cfg_skel_distance=100000; g_cfg_max_distance=100000;
    WNDCLASSA wc={0}; wc.lpfnWndProc=ControlledWindow; wc.hInstance=instance;
    wc.lpszClassName="ControlledMemoryRender"; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClassA(&wc)) { failed=1; goto cleanup; }
    DWORD style=transparent_mode?WS_POPUP:WS_OVERLAPPEDWINDOW;
    DWORD extended=WS_EX_NOREDIRECTIONBITMAP;
    if(transparent_mode) extended|=WS_EX_TOPMOST|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE;
    RECT rect={0,0,1000,650}; AdjustWindowRect(&rect,style,FALSE);
    test_window=CreateWindowExA(extended,wc.lpszClassName,
        "Controlled memory render - user-mode, no driver",style,
        70,70,rect.right-rect.left,rect.bottom-rect.top,NULL,NULL,instance,NULL);
    if(!test_window || !gpu_init(test_window,1000,650)) {
        fprintf(report,"FAIL window/GPU initialization\n"); failed=1; goto cleanup;
    }
    ShowWindow(test_window,transparent_mode?SW_SHOWNOACTIVATE:SW_SHOWNORMAL);
    if(transparent_mode) {
        LONG_PTR actual_style=GetWindowLongPtr(test_window,GWL_STYLE);
        LONG_PTR actual_extended=GetWindowLongPtr(test_window,GWL_EXSTYLE);
        if(!(actual_style&WS_POPUP) || (actual_style&(WS_CAPTION|WS_THICKFRAME)) ||
            !(actual_extended&WS_EX_TOPMOST) || !IsWindowVisible(test_window)) {
            fprintf(report,"FAIL transparent window styles\n"); failed=1; goto cleanup;
        }
        fprintf(report,"PASS visible_window borderless=1 topmost=1 style=0x%llx exstyle=0x%llx\n",
            (unsigned long long)actual_style,(unsigned long long)actual_extended);
    }
    preview_started=GetTickCount64();
    if(transparent_mode && !automatic) current_case=6;
    if(!SetTimer(test_window,1,automatic?500:40,NULL)) { failed=1; goto cleanup; }
    { MSG msg; while(GetMessage(&msg,NULL,0,0)>0) { TranslateMessage(&msg); DispatchMessage(&msg); } }
cleanup:
    if(child_handle && !source_exited) stop_child();
    if(command_pipe) CloseHandle(command_pipe);
    if(reply_pipe) CloseHandle(reply_pipe);
    if(controlled_process) CloseHandle(controlled_process);
    if(child_handle) CloseHandle(child_handle);
    gpu_shutdown(); free(image_pixels);
    if(test_window && IsWindow(test_window)) DestroyWindow(test_window);
    CoUninitialize();
    if(automatic && (step!=6 || expected_failures!=2)) failed=1;
    fprintf(report,"%s frames=%d real_read_calls=%llu bytes=%llu expected_failures=%u child_exited=%d transparent=%d kernel_tested=0\n",
        failed?"FAIL":"PASS",frames,controlled_reads,controlled_bytes,expected_failures,source_exited,transparent_mode);
    fclose(report);
    return failed?1:0;
}
