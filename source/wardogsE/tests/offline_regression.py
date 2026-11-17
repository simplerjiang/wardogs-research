"""Exercise source excerpts and a controlled child process; never load the driver.

Run from an x64 MSVC developer shell:
  python tests/offline_regression.py --out <isolated-output-directory>
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
args = parser.parse_args()
root = args.source_root.resolve()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
source = (root / "2_src_cheat/wd_esp_full.c").read_text(encoding="utf-8-sig")
hook = (root / "2_src_cheat/chams_hook.h").read_text(encoding="utf-8-sig")
driver = (root / "3_src_driver/driver.c").read_text(encoding="utf-8-sig")


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def function(text, signature):
    """Balance C braces, excluding strings and comments, without changing source."""
    begin = text.index(signature)
    token = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*[\s\S]*?\*/|//[^\n]*|[{}]')
    depth = 0
    for match in token.finditer(text, begin):
        if match[0] == "{":
            depth += 1
        elif match[0] == "}":
            depth -= 1
            if depth == 0:
                return text[begin:match.end()]
    raise ValueError(f"Unclosed function: {signature}")


menu = (root / "2_src_cheat/menu_webview.cpp").read_text(encoding="utf-8-sig")
fields = re.findall(r'offsetof\(BR_Config,(\w+)\)', function(menu, "static bool cfg_set("))
special = dict(max_distance=31234, skel_distance=12345, dim_pct=63, line_w=4,
               aa_fov_px=321, aa_key=65, aa_smooth=7, aa_dist=123,
               aa_zoom_mode=2, aa_gain_ref_x100=2345, aa_gain_x100=4567, aa_gain_y100=6789)
set_fields = "\n".join(f"cfg.{f} = {special.get(f, '(pass & 1)')};" for f in fields)
check_fields = "\n".join(f'CHECK(cfg.{f} == expected.{f});' for f in fields)
macros = "\n".join(re.findall(r"^#define g_cfg_.*", source, re.M))
compact = between(source, "    /* Drop entities not seen this tick", "    /* fallback: sweep ULevel::Actors")
paint = between(source, "                g_nrender = g_nents[g_front];", "                /* ---- frame ----")
paint = "EnterCriticalSection(&g_cs);\n" + paint
world_rva = re.search(r"^#define RVA_UWORLD\s+(\S+)", source, re.M)[1]
user_test = r'''
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu_bridge.h"
#include "chams/chams.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static BR_Config cfg;
BR_Config* br_get(void) { return &cfg; }
static int g_menu_x, g_menu_y, g_menu_page;
static int* g_dim_pct = &cfg.dim_pct;
static unsigned char *g_name_fac=&cfg.color[6].use_fac, *g_dist_fac=&cfg.color[8].use_fac, *g_head_fac=&cfg.color[10].use_fac;
#define AA_FOV_PX_MIN 30
#define AA_FOV_PX_MAX 600
@MACROS@
@CONFIG@
static void test_config(void) {
    for (int pass=0; pass<2; ++pass) {
        cfg_defaults();
        @SET@
        for (int i=0;i<BR_COLOR_COUNT;++i) {
            cfg.color[i].rgb[0]=(unsigned char)(i*13);
            cfg.color[i].rgb[1]=(unsigned char)(255-i*11);
            cfg.color[i].rgb[2]=(unsigned char)(i*7);
            cfg.color[i].use_fac=(unsigned char)(i&1);
        }
        BR_Config expected=cfg;
        g_menu_x=123; g_menu_y=456; g_menu_page=3;
        cfg_save();
        memset(&cfg,0xA5,sizeof(cfg));
        g_menu_x=g_menu_y=g_menu_page=-1;
        cfg_load();
        @CHECK@
        CHECK(g_menu_x==123 && g_menu_y==456 && g_menu_page==3);
        for (int i=0;i<BR_COLOR_COUNT;++i) {
            CHECK(!memcmp(cfg.color[i].rgb,expected.color[i].rgb,3));
            CHECK(cfg.color[i].use_fac==expected.color[i].use_fac);
        }
    }
}
#define MAX_PLAYERS 128
#define CHAMS_MAX_BONES 512
typedef struct { int id, last_tick; } ENT;
static ENT g_ents[2][MAX_PLAYERS],g_render[MAX_PLAYERS];
static int g_nents[2],g_write=1,g_front,g_nrender,g_render_gen,g_scan_tick=7;
static CH_XF g_chb[2][MAX_PLAYERS][CHAMS_MAX_BONES],g_chr[MAX_PLAYERS][CHAMS_MAX_BONES];
static int g_chbn[2][MAX_PLAYERS],g_chrn[MAX_PLAYERS],g_chmap[2][MAX_PLAYERS][CHAMS_MAX_BONES],g_chrmap[MAX_PLAYERS][CHAMS_MAX_BONES];
static int g_cam,g_camrot,g_fov,g_aspect,g_rcam,g_rcamrot,g_rfov,g_raspect;
static int g_cs,locked,unlocks;
static void enter(int* lock) { CHECK(!locked); locked=1; }
static void leave(int* lock) { CHECK(locked); locked=0; ++unlocks; g_front^=1; }
#define EnterCriticalSection enter
#define LeaveCriticalSection leave
int ch_bone_count(void) { return CHAMS_MAX_BONES; }
static void compact(void) { ENT* ents=g_ents[g_write]; int* nent=&g_nents[g_write]; @COMPACT@ }
static void snapshot(void) { @PAINT@ }
static void test_compact(void) {
    for (int mask=0;mask<64;++mask) {
        int ids[MAX_PLAYERS], count=0;
        g_nents[g_write]=MAX_PLAYERS;
        for (int i=0;i<MAX_PLAYERS;++i) {
            int live=(mask>>(i%6))&1;
            g_ents[g_write][i].id=i+1;
            g_ents[g_write][i].last_tick=live?g_scan_tick:0;
            g_chbn[g_write][i]=(i*7)%513;
            for (int b=0;b<CHAMS_MAX_BONES;++b) {
                g_chb[g_write][i][b].pos.x=i*1000+b;
                g_chmap[g_write][i][b]=i*1000+b;
            }
            if(live) ids[count++]=i+1;
        }
        compact();
        CHECK(g_nents[g_write]==count);
        for (int i=0;i<count;++i) {
            int original=ids[i]-1;
            CHECK(g_ents[g_write][i].id==ids[i]);
            CHECK(g_chbn[g_write][i]==(original*7)%513);
            for (int b=0;b<CHAMS_MAX_BONES;++b) {
                CHECK(g_chb[g_write][i][b].pos.x==original*1000+b);
                CHECK(g_chmap[g_write][i][b]==original*1000+b);
            }
        }
    }
}
static void test_snapshot(void) {
    for (int f=0;f<2;++f) {
        g_nents[f]=1; g_ents[f][0].id=f+1; g_chbn[f][0]=1;
        g_chb[f][0][0].pos.x=f+1; g_chmap[f][0][0]=f+1;
    }
    for (int f=0;f<2;++f) {
        g_front=f; unlocks=0;
        snapshot();
        CHECK(unlocks==1 && !locked);
        CHECK(g_nrender==1 && g_render[0].id==f+1 && g_render_gen==f);
        CHECK(g_chrn[0]==1 && g_chr[0][0].pos.x==g_render[0].id);
        CHECK(g_chrmap[0][0]==g_render[0].id);
    }
}
#define RVA_UWORLD @WORLD_RVA@
static uint64_t g_base=0x100000000ULL;
static int g_c_scan,reads;
static uint64_t rd64(uint64_t address) {
    static const uint32_t expected[]={RVA_UWORLD,0xD095198u,0xD0CC818u,0xCE54098u,0xCDFCA58u,0xCFB1B00u};
    uint64_t rva=reads<6?expected[reads]:0xC000000u+(reads-6)*4;
    CHECK(address==g_base+rva); ++reads; return 0;
}
static uint64_t probe_world(uint64_t world) { return 0; }
@WORLD@
static void test_world(void) { CHECK(find_world()==0); CHECK(reads==6+0x10000/4); }
static double g_chams_cluster_cm=3;
static int load_result,undo_calls;
int ch_load_ex(const char* path,double cluster) { CHECK(!strcmp(path,@MESH_PATH@)); return load_result; }
int ch_undo_log(int count) { ++undo_calls; return 1; }
@MESH@
static void test_mesh(void) {
    CHECK(SetCurrentDirectoryA("C:\\"));
    load_result=0; CHECK(!chams_init()); CHECK(undo_calls==0);
    load_result=1; CHECK(chams_init()); CHECK(undo_calls==1);
}
int main(int argc,char** argv) {
    CHECK(argc==2);
    if(!strcmp(argv[1],"config")) test_config();
    else if(!strcmp(argv[1],"compact")) test_compact();
    else if(!strcmp(argv[1],"snapshot")) test_snapshot();
    else if(!strcmp(argv[1],"world")) test_world();
    else if(!strcmp(argv[1],"mesh")) test_mesh();
    else return 2;
    printf("PASS %s\n",argv[1]); return 0;
}
'''
replacements = {
    "MACROS": macros,
    "CONFIG": between(source, "static void cfg_path(", "/* Cached GDI objects:"),
    "SET": set_fields, "CHECK": check_fields, "COMPACT": compact, "PAINT": paint,
    "WORLD_RVA": world_rva, "WORLD": function(source, "static uint64_t find_world("),
    "MESH": function(hook, "static int chams_init("),
    "MESH_PATH": json.dumps(str(out / "mesh.bin")),
}
for key, value in replacements.items():
    user_test = user_test.replace(f"@{key}@", value)
c_file = out / "offline_user.c"
c_file.write_text(user_test, encoding="utf-8")
exe = out / "offline_user.exe"
subprocess.run(["cl", "/nologo", "/utf-8", "/W3", "/Od", "/D_CRT_SECURE_NO_WARNINGS",
                f"/I{root / '2_src_cheat'}", str(c_file), f"/Fo{out / 'offline_user.obj'}",
                f"/Fe{exe}"], cwd=out, check=True)
for case in ("config", "compact", "snapshot", "world", "mesh"):
    subprocess.run([str(exe), case], cwd=out, check=True)
print(f"Verified {len(fields)} menu fields, palette, 64 compaction patterns, snapshot, world bounds and mesh path.")

kernel_test = r'''
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "abi_protocol.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef void *PEPROCESS;
typedef struct _DEVICE_OBJECT DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _DRIVER_OBJECT DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct {
    SRWLOCK gate;
    CONDITION_VARIABLE idle;
    LONG active;
    BOOLEAN draining;
} EX_RUNDOWN_REF;
typedef enum { KernelMode, UserMode } KPROCESSOR_MODE;
typedef struct {
    UCHAR MajorFunction;
    struct { struct { ULONG IoControlCode,InputBufferLength,OutputBufferLength; } DeviceIoControl; } Parameters;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct {
    struct { NTSTATUS Status; ULONG_PTR Information; } IoStatus;
    struct { PVOID SystemBuffer; } AssociatedIrp;
    IO_STACK_LOCATION stack;
} IRP, *PIRP;
struct _DEVICE_OBJECT { ULONG Flags; PDRIVER_OBJECT DriverObject; };
struct _DRIVER_OBJECT {
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS (*MajorFunction[28])(PDEVICE_OBJECT,PIRP);
    VOID (*DriverUnload)(PDRIVER_OBJECT);
};
#define IRP_MJ_CREATE 0
#define IRP_MJ_CLOSE 2
#define IRP_MJ_DEVICE_CONTROL 14
#define IRP_MJ_CLEANUP 18
#define IO_NO_INCREMENT 0
#define DO_BUFFERED_IO 4
#define DO_DEVICE_INITIALIZING 128
#define FILE_DEVICE_SECURE_OPEN 0x100
@ABI@
@STATE@
static PVOID MmHighestUserAddress=(PVOID)0x7FFFFFFEFFFFULL;
static void *process_type=(PVOID)11, **PsProcessType=&process_type;
static int process,lookup_calls,open_calls,close_calls,deref_calls,copy_calls;
static NTSTATUS lookup_status,open_status,query_status;
static int short_copy;
static ACCESS_MASK granted=PROCESS_ALL_ACCESS,requested;
static volatile LONG completions;
static int link_exists,link_deleted,devices,device_deleted,key_exists,key_handles;
static ULONG key_value;
static int fail_stage;
static DEVICE_OBJECT test_device;
static HANDLE copy_entered,copy_release,rundown_waiting;
static int block_copy;
static int live_mode;
static DWORD live_pid;
static HANDLE live_handle;
static ULONG_PTR live_peb;
static HANDLE device_handle=INVALID_HANDLE_VALUE;
static ULONG device_key;
static NTSTATUS PsLookupProcessByProcessId(HANDLE pid,PEPROCESS* p) {
    if(live_mode && (ULONG_PTR)pid!=live_pid) { *p=NULL; return STATUS_INVALID_CID; }
    ++lookup_calls; *p=NT_SUCCESS(lookup_status)?&process:NULL; return lookup_status;
}
static NTSTATUS ObOpenObjectByPointer(PVOID obj,ULONG flags,PVOID state,ACCESS_MASK access,PVOID type,KPROCESSOR_MODE mode,HANDLE* handle) {
    CHECK(obj==&process && type==*PsProcessType);
    CHECK((flags&(OBJ_KERNEL_HANDLE|OBJ_FORCE_ACCESS_CHECK))==(OBJ_KERNEL_HANDLE|OBJ_FORCE_ACCESS_CHECK));
    CHECK(mode==UserMode); ++open_calls; requested=access;
    if(live_mode) {
        *handle=OpenProcess(access,FALSE,live_pid);
        return *handle?STATUS_SUCCESS:STATUS_ACCESS_DENIED;
    }
    *handle=(HANDLE)123; return open_status;
}
static NTSTATUS ZwQueryObject(HANDLE h,OBJECT_INFORMATION_CLASS type,PVOID data,ULONG size,PULONG length) {
    if(live_mode) return NtQueryObject(h,type,data,size,length);
    CHECK(h==(HANDLE)123 && type==ObjectBasicInformation);
    ((PUBLIC_OBJECT_BASIC_INFORMATION*)data)->GrantedAccess=granted; return query_status;
}
static NTSTATUS ZwClose(HANDLE h) {
    if(live_mode) { ++close_calls; CHECK(CloseHandle(h)); return STATUS_SUCCESS; }
    if(h==(HANDLE)789) { CHECK(key_handles==1); --key_handles; }
    else { CHECK(h==(HANDLE)123); ++close_calls; }
    return STATUS_SUCCESS;
}
static void ObDereferenceObject(PVOID obj) { CHECK(obj==&process); ++deref_calls; }
static PEPROCESS PsGetCurrentProcess(void) { return live_mode?(PEPROCESS)GetCurrentProcess():&process; }
static PPEB PsGetProcessPeb(PEPROCESS obj) { return (PPEB)(live_mode?live_peb:0x100000); }
static NTSTATUS MmCopyVirtualMemory(PEPROCESS src,PVOID sa,PEPROCESS dst,PVOID da,SIZE_T n,KPROCESSOR_MODE mode,PSIZE_T copied) {
    ++copy_calls;
    if(live_mode) {
        CHECK(src==&process && dst==(PEPROCESS)GetCurrentProcess() && mode==KernelMode);
        if(ReadProcessMemory(live_handle,sa,da,n,copied)) return STATUS_SUCCESS;
        return GetLastError()==ERROR_PARTIAL_COPY?STATUS_PARTIAL_COPY:STATUS_ACCESS_VIOLATION;
    }
    if(block_copy) {
        CHECK(SetEvent(copy_entered));
        CHECK(WaitForSingleObject(copy_release,10000)==WAIT_OBJECT_0);
    }
    *copied=short_copy?n-1:n; return STATUS_SUCCESS;
}
static void ExInitializeRundownProtection(EX_RUNDOWN_REF* ref) {
    InitializeSRWLock(&ref->gate); InitializeConditionVariable(&ref->idle);
    ref->active=0; ref->draining=FALSE;
}
static BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF* ref) {
    AcquireSRWLockExclusive(&ref->gate);
    BOOLEAN acquired=!ref->draining;
    if(acquired) ++ref->active;
    ReleaseSRWLockExclusive(&ref->gate); return acquired;
}
static void ExReleaseRundownProtection(EX_RUNDOWN_REF* ref) {
    AcquireSRWLockExclusive(&ref->gate); CHECK(ref->active>0);
    if(!--ref->active) WakeAllConditionVariable(&ref->idle);
    ReleaseSRWLockExclusive(&ref->gate);
}
static void ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF* ref) {
    AcquireSRWLockExclusive(&ref->gate); ref->draining=TRUE;
    if(rundown_waiting) CHECK(SetEvent(rundown_waiting));
    while(ref->active) CHECK(SleepConditionVariableSRW(&ref->idle,&ref->gate,INFINITE,0));
    ReleaseSRWLockExclusive(&ref->gate);
}
static void RtlInitUnicodeString(PUNICODE_STRING s,PCWSTR value) { s->Buffer=(PWSTR)value; }
static NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING name) {
    CHECK(link_exists && !wcscmp(name->Buffer,L"\\DosDevices\\Global\\AbiComm"));
    link_exists=0; ++link_deleted; return STATUS_SUCCESS;
}
static NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING name,PUNICODE_STRING target) {
    CHECK(!wcscmp(name->Buffer,L"\\DosDevices\\Global\\AbiComm"));
    CHECK(!wcscmp(target->Buffer,L"\\Device\\AbiComm0"));
    if(fail_stage==2) return STATUS_OBJECT_NAME_COLLISION;
    CHECK(!link_exists); link_exists=1; return STATUS_SUCCESS;
}
static NTSTATUS IoCreateDeviceSecure(PDRIVER_OBJECT driver,ULONG extension,PUNICODE_STRING name,
    ULONG type,ULONG characteristics,BOOLEAN exclusive,PUNICODE_STRING sddl,const GUID* guid,PDEVICE_OBJECT* device) {
    CHECK(!extension && type==FILE_DEVICE_UNKNOWN && characteristics==FILE_DEVICE_SECURE_OPEN && !exclusive);
    CHECK(!wcscmp(name->Buffer,L"\\Device\\AbiComm0") && sddl->Buffer && guid);
    if(fail_stage==1) return STATUS_OBJECT_NAME_COLLISION;
    CHECK(!devices); devices=1; test_device.Flags=DO_DEVICE_INITIALIZING;
    test_device.DriverObject=driver; driver->DeviceObject=*device=&test_device; return STATUS_SUCCESS;
}
static void IoDeleteDevice(PDEVICE_OBJECT device) {
    CHECK(device==&test_device && devices==1 && !g_DeviceCreated && !g_RequestRundown.active);
    CHECK(!g_StateKey && !key_handles);
    devices=0; ++device_deleted; device->DriverObject->DeviceObject=NULL;
}
static NTSTATUS ZwCreateKey(PHANDLE key,ACCESS_MASK access,POBJECT_ATTRIBUTES attributes,
    ULONG title,PUNICODE_STRING class_name,ULONG options,PULONG disposition) {
    CHECK(!wcscmp(attributes->ObjectName->Buffer,L"\\Registry\\Machine\\SYSTEM\\AbiComm"));
    CHECK((attributes->Attributes&OBJ_KERNEL_HANDLE) && access==(KEY_SET_VALUE|DELETE) && options==REG_OPTION_VOLATILE);
    if(fail_stage==3) return STATUS_ACCESS_DENIED;
    CHECK(!key_handles); ++key_handles; *key=(HANDLE)789;
    *disposition=key_exists?REG_OPENED_EXISTING_KEY:REG_CREATED_NEW_KEY;
    key_exists=1; return STATUS_SUCCESS;
}
static NTSTATUS ZwSetValueKey(HANDLE key,PUNICODE_STRING name,ULONG title,ULONG type,PVOID data,ULONG size) {
    CHECK(key==(HANDLE)789 && key_handles && !wcscmp(name->Buffer,L"SessionKey") && type==REG_DWORD && size==4);
    if(fail_stage==4) return STATUS_ACCESS_DENIED;
    key_value=*(ULONG*)data; return STATUS_SUCCESS;
}
static NTSTATUS ZwDeleteValueKey(HANDLE key,PUNICODE_STRING name) {
    CHECK(key==(HANDLE)789 && key_handles && !wcscmp(name->Buffer,L"SessionKey"));
    key_value=0; return STATUS_SUCCESS;
}
static NTSTATUS ZwDeleteKey(HANDLE key) {
    CHECK(key==(HANDLE)789 && key_handles && key_exists && !key_value);
    key_exists=0; return STATUS_SUCCESS;
}
static ULONG ComputeSessionKey(void) { return 123; }
static PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP irp) { return &irp->stack; }
static void IoCompleteRequest(PIRP irp,int increment) { InterlockedIncrement(&completions); }
@FUNCTIONS@
static void reset_access(void) {
    lookup_calls=open_calls=close_calls=deref_calls=copy_calls=0;
    short_copy=0;
    lookup_status=open_status=query_status=STATUS_SUCCESS; granted=PROCESS_ALL_ACCESS;
}
static void test_access(void) {
    ULONG_PTR limit=(ULONG_PTR)MmHighestUserAddress;
    CHECK(IsUserRange(0x10000,0x10000)); CHECK(IsUserRange(limit,1));
    CHECK(!IsUserRange(limit,2)); CHECK(!IsUserRange(limit+1,1));
    CHECK(!IsUserRange(0x10000,0)); CHECK(!IsUserRange(0x10000,(SIZE_T)-1));
    CHECK(!IsUserRange((ULONG_PTR)-1,2));
    char buffer[16]; ULONG_PTR base;
    reset_access();
    CHECK(ReadTargetMemory(1,limit,buffer,2)==STATUS_ACCESS_VIOLATION);
    CHECK(WriteTargetMemory(1,(ULONG_PTR)-1,buffer,2)==STATUS_ACCESS_VIOLATION);
    CHECK(!lookup_calls && !copy_calls);
    reset_access(); lookup_status=STATUS_INVALID_CID;
    CHECK(ReadTargetMemory(1,0x10000,buffer,1)==STATUS_INVALID_CID);
    CHECK(!open_calls && !deref_calls && !copy_calls);
    reset_access(); open_status=STATUS_ACCESS_DENIED;
    CHECK(ReadTargetMemory(1,0x10000,buffer,1)==STATUS_ACCESS_DENIED);
    CHECK(deref_calls==1 && !close_calls && !copy_calls);
    reset_access(); granted=0;
    CHECK(ReadTargetMemory(1,0x10000,buffer,1)==STATUS_ACCESS_DENIED);
    CHECK(close_calls==1 && deref_calls==1 && !copy_calls);
    reset_access(); query_status=STATUS_UNSUCCESSFUL;
    CHECK(WriteTargetMemory(1,0x10000,buffer,1)==STATUS_UNSUCCESSFUL);
    CHECK(close_calls==1 && deref_calls==1 && !copy_calls);
    reset_access();
    CHECK(NT_SUCCESS(ReadTargetMemory(1,0x10000,buffer,1)));
    CHECK(requested==PROCESS_VM_READ && copy_calls==1 && deref_calls==1 && close_calls==1);
    reset_access();
    CHECK(NT_SUCCESS(WriteTargetMemory(1,0x10000,buffer,1)));
    CHECK(requested==(PROCESS_VM_WRITE|PROCESS_VM_OPERATION) && copy_calls==1 && deref_calls==1);
    reset_access(); granted=PROCESS_VM_WRITE;
    CHECK(WriteTargetMemory(1,0x10000,buffer,1)==STATUS_ACCESS_DENIED && !copy_calls);
    reset_access();
    CHECK(NT_SUCCESS(GetTargetImageBase(1,&base)) && requested==PROCESS_VM_READ);
}
static void test_disable(void) {
    ABI_COMMAND command={123,1,0x10000,1};
    IRP irp={0}; irp.AssociatedIrp.SystemBuffer=&command;
    irp.stack.Parameters.DeviceIoControl.InputBufferLength=sizeof(command);
    irp.stack.Parameters.DeviceIoControl.OutputBufferLength=sizeof(command);
    irp.stack.Parameters.DeviceIoControl.IoControlCode=ABI_IOCTL_UNLOAD;
    LONG before=completions;
    CHECK(g_DeviceCreated && g_LinkCreated && link_exists);
    CHECK(AbiDeviceControl(NULL,&irp)==STATUS_SUCCESS);
    CHECK(!g_DeviceCreated && !g_LinkCreated && !link_exists && devices==1 && key_handles==1);
    CHECK(AbiDeviceControl(NULL,&irp)==STATUS_DEVICE_NOT_READY);
    irp.stack.Parameters.DeviceIoControl.IoControlCode=ABI_IOCTL_READ_MEMORY;
    reset_access();
    CHECK(AbiDeviceControl(NULL,&irp)==STATUS_DEVICE_NOT_READY && !copy_calls);
    irp.stack.MajorFunction=IRP_MJ_CREATE;
    CHECK(AbiCreateClose(NULL,&irp)==STATUS_DEVICE_NOT_READY);
    irp.stack.MajorFunction=IRP_MJ_CLOSE;
    CHECK(AbiCreateClose(NULL,&irp)==STATUS_SUCCESS);
    irp.stack.MajorFunction=IRP_MJ_CLEANUP;
    CHECK(AbiCreateClose(NULL,&irp)==STATUS_SUCCESS);
    CHECK(completions==before+6 && !g_RequestRundown.active);
}
static NTSTATUS request(ULONG code,const void* input,ULONG in_size,void* output,ULONG out_size,ULONG* returned) {
    ULONG capacity=in_size>out_size?in_size:out_size;
    void* storage=calloc(1,capacity?capacity:1);
    CHECK(storage!=NULL);
    if(in_size) memcpy(storage,input,in_size);
    if(device_handle!=INVALID_HANDLE_VALUE) {
        ((ABI_COMMAND*)storage)->magic_key=device_key;
        *returned=0;
        BOOL ok=DeviceIoControl(device_handle,code,storage,in_size,output,out_size,returned,NULL);
        free(storage); return ok?STATUS_SUCCESS:STATUS_UNSUCCESSFUL;
    }
    IRP irp={0}; irp.AssociatedIrp.SystemBuffer=storage;
    irp.stack.Parameters.DeviceIoControl.IoControlCode=code;
    irp.stack.Parameters.DeviceIoControl.InputBufferLength=in_size;
    irp.stack.Parameters.DeviceIoControl.OutputBufferLength=out_size;
    NTSTATUS status=AbiDeviceControl(NULL,&irp);
    CHECK(status==irp.IoStatus.Status && irp.IoStatus.Information<=out_size);
    *returned=(ULONG)irp.IoStatus.Information;
    if(*returned) memcpy(output,storage,*returned);
    free(storage); return status;
}
typedef struct { ABI_BATCH_COMMAND header; ABI_READ_SEGMENT segments[ABI_MAX_BATCH_COUNT]; } BATCH;
static BATCH batch(ULONG pid,ULONG count,ULONG size) {
    BATCH b={0}; b.header.command.magic_key=123; b.header.command.process_id=pid;
    b.header.command.size=size; b.header.count=count; return b;
}
static ULONG batch_size(const BATCH* b) { return sizeof(b->header)+b->header.count*sizeof(b->segments[0]); }
typedef struct { NTSTATUS status; ULONG returned; } READ_THREAD;
static DWORD WINAPI read_thread(void* context) {
    READ_THREAD* result=context; ABI_COMMAND command={123,1,0x10000,1}; unsigned char byte;
    result->status=request(ABI_IOCTL_READ_MEMORY,&command,sizeof(command),&byte,1,&result->returned);
    return 0;
}
static DWORD WINAPI unload_thread(void* context) {
    PDRIVER_OBJECT driver=context; driver->DriverUnload(driver); return 0;
}
static void test_lifecycle(void) {
    for(int existing=0;existing<2;++existing) for(int failure=1;failure<=4;++failure) {
        DRIVER_OBJECT driver={0};
        key_exists=existing; key_value=existing?777:0; link_exists=failure==2;
        fail_stage=failure; device_deleted=link_deleted=0;
        NTSTATUS expected=failure<=2?STATUS_OBJECT_NAME_COLLISION:STATUS_ACCESS_DENIED;
        CHECK(DriverEntry(&driver,NULL)==expected);
        CHECK(!g_DeviceCreated && !g_LinkCreated && !g_StateKey && !g_KeyPublished && !g_StateKeyCreated && !g_SessionKey);
        CHECK(!devices && !key_handles && !driver.DeviceObject);
        CHECK(key_exists==existing && key_value==(existing?777:0));
        CHECK(link_exists==(failure==2) && link_deleted==(failure>=3));
        CHECK(device_deleted==(failure!=1) && !g_RequestRundown.active);
        link_exists=0; /* Drop the simulated foreign link between fixtures. */
    }
    fail_stage=0;
    for(int existing=0;existing<2;++existing) {
        DRIVER_OBJECT driver={0}; key_exists=existing; key_value=existing?777:0;
        CHECK(DriverEntry(&driver,NULL)==STATUS_SUCCESS);
        CHECK(g_DeviceCreated && g_LinkCreated && devices==1 && link_exists && key_handles==1 && key_value==123);
        CHECK(driver.DeviceObject==&test_device && !(test_device.Flags&DO_DEVICE_INITIALIZING) && (test_device.Flags&DO_BUFFERED_IO));
        CHECK(driver.DriverUnload==AbiDriverUnload && driver.MajorFunction[IRP_MJ_CLEANUP]==AbiCreateClose);
        IRP create={0}; create.stack.MajorFunction=IRP_MJ_CREATE;
        CHECK(driver.MajorFunction[IRP_MJ_CREATE](&test_device,&create)==STATUS_SUCCESS);
        driver.DriverUnload(&driver);
        CHECK(!devices && !driver.DeviceObject && !link_exists && !key_handles && key_exists==existing && !key_value);
        CHECK(!g_StateKey && !g_KeyPublished && !g_StateKeyCreated && !g_SessionKey && g_RequestRundown.draining);
    }
    DRIVER_OBJECT driver={0}; key_exists=0;
    CHECK(DriverEntry(&driver,NULL)==STATUS_SUCCESS); reset_access();
    copy_entered=CreateEventW(NULL,TRUE,FALSE,NULL);
    copy_release=CreateEventW(NULL,TRUE,FALSE,NULL);
    rundown_waiting=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(copy_entered && copy_release && rundown_waiting);
    block_copy=1; READ_THREAD read_result={0};
    HANDLE reader=CreateThread(NULL,0,read_thread,&read_result,0,NULL); CHECK(reader);
    CHECK(WaitForSingleObject(copy_entered,10000)==WAIT_OBJECT_0);
    HANDLE unloader=CreateThread(NULL,0,unload_thread,&driver,0,NULL); CHECK(unloader);
    CHECK(WaitForSingleObject(rundown_waiting,10000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(unloader,0)==WAIT_TIMEOUT);
    CHECK(!g_DeviceCreated && devices==1 && link_exists && key_handles==1 && key_value==123);
    ABI_COMMAND command={123,1,0,0}; ABI_STATUS state; ULONG returned;
    CHECK(request(ABI_IOCTL_QUERY_STATUS,&command,sizeof(command),&state,sizeof(state),&returned)==STATUS_DELETE_PENDING && !returned);
    IRP close={0}; close.stack.MajorFunction=IRP_MJ_CREATE;
    CHECK(AbiCreateClose(NULL,&close)==STATUS_DEVICE_NOT_READY);
    close.stack.MajorFunction=IRP_MJ_CLEANUP; CHECK(AbiCreateClose(NULL,&close)==STATUS_SUCCESS);
    close.stack.MajorFunction=IRP_MJ_CLOSE; CHECK(AbiCreateClose(NULL,&close)==STATUS_SUCCESS);
    CHECK(SetEvent(copy_release));
    CHECK(WaitForSingleObject(reader,10000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(unloader,10000)==WAIT_OBJECT_0);
    CHECK(read_result.status==STATUS_SUCCESS && read_result.returned==1);
    CHECK(!devices && !driver.DeviceObject && !link_exists && !key_handles && !key_exists && !key_value && !g_SessionKey);
    CHECK(!g_RequestRundown.active && g_RequestRundown.draining);
    CloseHandle(reader); CloseHandle(unloader); CloseHandle(copy_entered);
    CloseHandle(copy_release); CloseHandle(rundown_waiting);
    copy_entered=copy_release=rundown_waiting=NULL; block_copy=0;
    puts("PASS standard load/rollback/unload fixtures: eight init failures, existing state, blocked read and rundown rejection");
}
static void test_interfaces(void) {
    g_DeviceCreated=TRUE; reset_access();
    ABI_COMMAND c={123,1,0,0}; ABI_STATUS state; ABI_PROCESS_INFO info; ULONG returned;
    CHECK(request(ABI_IOCTL_QUERY_STATUS,&c,sizeof(c),&state,sizeof(state),&returned)==STATUS_SUCCESS);
    CHECK(returned==sizeof(state) && state.protocol_version==ABI_PROTOCOL_VERSION && state.device_ready==TRUE);
    CHECK(state.max_transfer==65536 && state.max_batch_count==96 && state.capabilities==7 && !state.reserved);
    CHECK(!lookup_calls);
    CHECK(request(ABI_IOCTL_QUERY_STATUS,&c,sizeof(c),&state,sizeof(state)-1,&returned)==STATUS_BUFFER_TOO_SMALL && !returned);
    CHECK(request(ABI_IOCTL_QUERY_STATUS,&c,sizeof(c)-1,&state,sizeof(state),&returned)==STATUS_INVALID_PARAMETER && !returned);
    c.magic_key=0;
    CHECK(request(ABI_IOCTL_QUERY_STATUS,&c,sizeof(c),&state,sizeof(state),&returned)==STATUS_ACCESS_DENIED && !returned);
    c.magic_key=123;
    CHECK(request(ABI_IOCTL_GET_PROCESS_INFO,&c,sizeof(c),&info,sizeof(info)-1,&returned)==STATUS_BUFFER_TOO_SMALL && !returned);
    reset_access(); granted=0;
    CHECK(request(ABI_IOCTL_GET_PROCESS_INFO,&c,sizeof(c),&info,sizeof(info),&returned)==STATUS_ACCESS_DENIED && !returned && !copy_calls);
    reset_access();
    CHECK(request(ABI_IOCTL_GET_PROCESS_INFO,&c,sizeof(c),&info,sizeof(info),&returned)==STATUS_SUCCESS);
    CHECK(returned==sizeof(info) && info.peb==0x100000 && requested==PROCESS_VM_READ && deref_calls==1);
    reset_access(); short_copy=1;
    CHECK(request(ABI_IOCTL_GET_PROCESS_INFO,&c,sizeof(c),&info,sizeof(info),&returned)==STATUS_PARTIAL_COPY && !returned);
    BATCH b=batch(1,1,16); unsigned char output[32];
    b.segments[0]=(ABI_READ_SEGMENT){0x10000,16,0};
    reset_access(); granted=0;
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,16,&returned)==STATUS_ACCESS_DENIED && !returned && !copy_calls);
    reset_access(); short_copy=1;
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,16,&returned)==STATUS_PARTIAL_COPY && !returned && deref_calls==1);
    reset_access();
    b.header.count=0;
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,sizeof(b.header),output,16,&returned)==STATUS_INVALID_PARAMETER && !returned);
    b.header.count=97;
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,sizeof(b.header),output,16,&returned)==STATUS_INVALID_PARAMETER && !returned);
    b.header.count=1;
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,sizeof(b.header)-1,output,16,&returned)==STATUS_INVALID_PARAMETER && !returned);
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b)-1,output,16,&returned)==STATUS_INVALID_PARAMETER && !returned);
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,15,&returned)==STATUS_INVALID_PARAMETER && !returned);
    b.header.command.size=65537;
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,16,&returned)==STATUS_INVALID_PARAMETER && !returned);
    b.header.command.size=0;
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,16,&returned)==STATUS_INVALID_PARAMETER && !returned);
    b.header.command.size=16; b.segments[0].size=0;
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,16,&returned)==STATUS_ACCESS_VIOLATION && !returned);
    b.segments[0]=(ABI_READ_SEGMENT){(ULONG_PTR)MmHighestUserAddress,2,0};
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,16,&returned)==STATUS_ACCESS_VIOLATION && !returned);
    b.segments[0]=(ABI_READ_SEGMENT){0x10000,1,0xffffffff};
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,16,&returned)==STATUS_INVALID_PARAMETER && !returned);
    b.segments[0]=(ABI_READ_SEGMENT){0x10000,17,0};
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,16,&returned)==STATUS_INVALID_PARAMETER && !returned);
    CHECK(!copy_calls && !lookup_calls);
    puts("PASS new interface authentication, lengths, limits and access fixtures");
}
typedef struct { ULONG_PTR pid,peb,image_base,data,unreadable; } CHILD_INFO;
static unsigned char pattern(SIZE_T i) { return (unsigned char)(i*37+11); }
static int child(void) {
    unsigned char* data=VirtualAlloc(NULL,65536+4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    CHECK(data!=NULL);
    for(SIZE_T i=0;i<65536;++i) data[i]=pattern(i);
    DWORD previous,written,read;
    CHECK(VirtualProtect(data+65536,4096,PAGE_NOACCESS,&previous));
    CHILD_INFO info={GetCurrentProcessId(),__readgsqword(0x60),(ULONG_PTR)GetModuleHandleW(NULL),(ULONG_PTR)data,(ULONG_PTR)(data+65536)};
    CHECK(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),&info,sizeof(info),&written,NULL) && written==sizeof(info));
    char done;
    ReadFile(GetStdHandle(STD_INPUT_HANDLE),&done,1,&read,NULL);
    CHECK(VirtualFree(data,0,MEM_RELEASE)); return 0;
}
static void test_live_process(const char* executable) {
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE}; HANDLE parent_read,child_write,child_read,parent_write;
    CHECK(CreatePipe(&parent_read,&child_write,&sa,0));
    CHECK(CreatePipe(&child_read,&parent_write,&sa,0));
    CHECK(SetHandleInformation(parent_read,HANDLE_FLAG_INHERIT,0));
    CHECK(SetHandleInformation(parent_write,HANDLE_FLAG_INHERIT,0));
    STARTUPINFOA si={sizeof(si)}; PROCESS_INFORMATION pi;
    si.dwFlags=STARTF_USESTDHANDLES; si.hStdInput=child_read; si.hStdOutput=child_write; si.hStdError=child_write;
    char command[32768]; CHECK(sprintf_s(command,sizeof(command),"\"%s\" --child",executable)>0);
    CHECK(CreateProcessA(executable,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi));
    CloseHandle(child_read); CloseHandle(child_write); CloseHandle(pi.hThread);
    CHILD_INFO child_info; DWORD read;
    CHECK(ReadFile(parent_read,&child_info,sizeof(child_info),&read,NULL) && read==sizeof(child_info));
    CHECK(child_info.pid==pi.dwProcessId);
    PROCESS_BASIC_INFORMATION basic;
    CHECK(NT_SUCCESS(NtQueryInformationProcess(pi.hProcess,ProcessBasicInformation,&basic,sizeof(basic),NULL)));
    live_pid=pi.dwProcessId; live_handle=pi.hProcess; live_peb=(ULONG_PTR)basic.PebBaseAddress;
    live_mode=1; reset_access();
    ABI_COMMAND c={123,live_pid,0,0}; ABI_PROCESS_INFO info; ULONG returned;
    ABI_STATUS state;
    CHECK(request(ABI_IOCTL_QUERY_STATUS,&c,sizeof(c),&state,sizeof(state),&returned)==STATUS_SUCCESS);
    CHECK(returned==sizeof(state) && state.protocol_version==ABI_PROTOCOL_VERSION && state.device_ready && state.capabilities==7);
    CHECK(request(ABI_IOCTL_GET_PROCESS_INFO,&c,sizeof(c),&info,sizeof(info),&returned)==STATUS_SUCCESS);
    CHECK(returned==sizeof(info) && info.peb==child_info.peb && info.image_base==child_info.image_base);
    if(device_handle==INVALID_HANDLE_VALUE) CHECK(requested==PROCESS_VM_READ && close_calls==1 && deref_calls==1);
    unsigned char output[65536],expected[256]={0};
    ULONG_PTR legacy_base=0;
    CHECK(request(ABI_IOCTL_GET_BASE,&c,sizeof(c),&legacy_base,sizeof(legacy_base),&returned)==STATUS_SUCCESS);
    CHECK(returned==sizeof(legacy_base) && legacy_base==child_info.image_base);
    c.address=child_info.data; c.size=16;
    CHECK(request(ABI_IOCTL_READ_MEMORY,&c,sizeof(c),output,16,&returned)==STATUS_SUCCESS && returned==16);
    for(SIZE_T i=0;i<16;++i) CHECK(output[i]==pattern(i));
    BATCH b=batch(live_pid,3,256);
    b.segments[0]=(ABI_READ_SEGMENT){child_info.data+123,17,35};
    b.segments[1]=(ABI_READ_SEGMENT){child_info.data+4105,31,0};
    b.segments[2]=(ABI_READ_SEGMENT){child_info.data+33000,64,80};
    for(ULONG i=0;i<b.header.count;++i) for(ULONG j=0;j<b.segments[i].size;++j)
        expected[b.segments[i].output_offset+j]=pattern(b.segments[i].address-child_info.data+j);
    reset_access();
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,256,&returned)==STATUS_SUCCESS);
    CHECK(returned==256 && !memcmp(output,expected,256));
    if(device_handle==INVALID_HANDLE_VALUE) CHECK(lookup_calls==1 && deref_calls==1 && copy_calls==3);
    b=batch(live_pid,96,65536);
    for(ULONG i=0;i<96;++i) {
        ULONG offset=i*682, size=i==95?65536-offset:682;
        b.segments[i]=(ABI_READ_SEGMENT){child_info.data+offset,size,offset};
    }
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,sizeof(output),&returned)==STATUS_SUCCESS && returned==sizeof(output));
    for(SIZE_T i=0;i<sizeof(output);++i) CHECK(output[i]==pattern(i));
    b=batch(live_pid,2,8);
    b.segments[0]=(ABI_READ_SEGMENT){child_info.data+8,4,4};
    b.segments[1]=(ABI_READ_SEGMENT){child_info.data+4,4,0};
    CHECK(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,8,&returned)==STATUS_SUCCESS && returned==8);
    for(SIZE_T i=0;i<8;++i) CHECK(output[i]==pattern(i+4));
    // Valid address syntax followed by a PAGE_NOACCESS page: no partial reply.
    b.segments[1].address=child_info.unreadable; memset(output,0xa5,8);
    CHECK(!NT_SUCCESS(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,8,&returned)) && !returned);
    for(SIZE_T i=0;i<8;++i) CHECK(output[i]==0xa5);
    // A malformed later descriptor must be rejected before the first copy.
    b.segments[1].address=(ULONG_PTR)-1; reset_access();
    CHECK(!NT_SUCCESS(request(ABI_IOCTL_READ_BATCH,&b,batch_size(&b),output,8,&returned)) && !returned && !copy_calls && !lookup_calls);
    CloseHandle(parent_write);
    CHECK(WaitForSingleObject(pi.hProcess,10000)==WAIT_OBJECT_0);
    DWORD exit_code; CHECK(GetExitCodeProcess(pi.hProcess,&exit_code) && exit_code==0);
    CHECK(request(ABI_IOCTL_GET_PROCESS_INFO,&c,sizeof(c),&info,sizeof(info),&returned)!=STATUS_SUCCESS && !returned);
    live_mode=0; CloseHandle(parent_read); CloseHandle(pi.hProcess);
    puts("PASS real child: PEB/base, sparse/96-segment/64KiB/overlapping-buffer reads, inaccessible page and exited process");
    if(device_handle==INVALID_HANDLE_VALUE)
        puts("NOTE Windows-backed user-mode shims executed extracted driver code; kernel IOCTL/loading is NOT tested.");
    else
        puts("PASS kernel device IOCTLs against controlled child; driver loading is outside this test.");
}
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"--child")) return child();
    if(argc==2 && !strcmp(argv[1],"--device")) {
        device_handle=CreateFileW(L"\\\\.\\AbiComm",GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
        if(device_handle==INVALID_HANDLE_VALUE) {
            printf("NOT_TESTED kernel device: AbiComm open failed, Win32 error %lu\n",GetLastError()); return 2;
        }
        DWORD size=sizeof(device_key);
        LSTATUS error=RegGetValueW(HKEY_LOCAL_MACHINE,L"SYSTEM\\AbiComm",L"SessionKey",RRF_RT_REG_DWORD,NULL,&device_key,&size);
        if(error!=ERROR_SUCCESS) {
            printf("NOT_TESTED kernel device: session-key read failed, Win32 error %ld\n",error);
            CloseHandle(device_handle); return 2;
        }
        test_live_process(argv[0]); CloseHandle(device_handle); return 0;
    }
    CHECK(argc==1);
    test_access(); puts("PASS driver access/range fixtures");
    test_lifecycle();
    DRIVER_OBJECT driver={0};
    CHECK(DriverEntry(&driver,NULL)==STATUS_SUCCESS);
    test_disable(); puts("PASS logical disable fixtures");
    driver.DriverUnload(&driver);
    CHECK(DriverEntry(&driver,NULL)==STATUS_SUCCESS);
    test_interfaces(); test_live_process(argv[0]);
    driver.DriverUnload(&driver);
    CHECK(!devices && !link_exists && !key_handles && !g_RequestRundown.active); return 0;
}
'''
functions = ["AbiPublishSessionKey", "AbiDeleteSessionKey",
             "IsUserRange", "ReferenceTargetProcess", "CopyCrossProcess", "ReadTargetMemory",
             "WriteTargetMemory", "GetTargetProcessInfo", "GetTargetImageBase", "ReadTargetMemoryBatch",
             "AbiCompleteIrp", "AbiCreateClose", "AbiDeleteLink", "AbiDisableDevice", "AbiDeviceControl",
             "AbiDriverUnload"]
snippets = []
for name in functions:
    signature = re.search(rf"static \w+ {name}\(", driver)[0]
    snippets.append(function(driver, signature))
snippets.append(function(driver, "NTSTATUS DriverEntry("))
kernel_test = kernel_test.replace("@FUNCTIONS@", "\n".join(snippets))
kernel_test = kernel_test.replace("@ABI@", between(driver, "#define KEY_SALT", "static ULONG  g_SessionKey"))
kernel_test = kernel_test.replace("@STATE@", between(driver, "static ULONG  g_SessionKey", "// ---------------------------------------------------------------- session key"))
kernel_file = out / "offline_driver.c"
kernel_file.write_text(kernel_test, encoding="utf-8")
kernel_exe = out / "offline_driver.exe"
subprocess.run(["cl", "/nologo", "/utf-8", "/W3", "/Od", "/std:c11", str(kernel_file),
                f"/I{root / '3_src_driver'}", f"/Fo{out / 'offline_driver.obj'}", f"/Fe{kernel_exe}",
                "ntdll.lib", "advapi32.lib"], cwd=out, check=True)
subprocess.run([str(kernel_exe)], cwd=out, check=True)
