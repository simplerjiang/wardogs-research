                                                                                     
  
                                                                        
  
                                                                                  
                                                                                  
                                                                              
                                                                               
                                 
  
                                                                           
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "case.h"
#include "ci.h"
#include "common.h"
#include "esp_live.h"
#include "esp_scene.h"
#include "json.h"
#include "math3d.h"
#include "nt.h"
#include "overlay_d3d12.h"
#include "pgr.h"
#include "provider.h"
#include "service.h"
#include "target.h"
#include "translate.h"
#include "psapi.h"

#include <tlhelp32.h>

#pragma comment(lib, "psapi.lib")

int RunSelfTest(Logger& log);                 

namespace {

                                                                            
                                                                            
                                      
constexpr char kPgrSha256[] = "076cfa7d866dc0364894fc5b9b671e5109935f564b01fe15ba35c46f3ef720a4";

Logger g_log;
NativeApi g_api;

struct Options {
    std::wstring pgr_path;
    std::wstring driver_path;
    std::wstring log_path;
    std::wstring mode = L"run";
    std::wstring case_path;
    std::wstring fixture_path;
    std::wstring target_process;
    std::string provider_id = "pgr";
    u64 max_phys = 0;
    u64 scan_from = 0;
    u32 patch_rva = 0;
    u32 pid = 0;
    bool has_patch_rva = false;
    bool dry_run = false;
    bool allow_write = false;
    bool spawn_target = false;
    bool check_only = false;
    bool no_rpm = false;
    bool no_kernel_route = false;
    u64 force_cr3 = 0;
    u64 read_base = 0;
    u32 read_len = 256;
    std::vector<u64> read_vas;
    std::vector<u64> read_rvas;
    struct EspPoint {
        Vec3 feet;
        float height_cm = 180.0f;
        int kind = 0;
    };
    Camera esp_camera;
    bool esp_camera_set = false;
    std::vector<EspPoint> esp_points;
    std::vector<u64> esp_actors;
    std::vector<u64> esp_vehicles;
    bool esp_demo = false;
    bool esp_live = false;
    bool esp_probe = false;
    u64 esp_probe_va = 0;
    bool esp_vsync = false;
    bool esp_corners = false;
    double esp_seconds = 0.0;
    float esp_scale = 1.0f;
    float esp_thickness = 1.0f;
    float esp_max_distance = 0.0f;
    int esp_max_boxes = 512;
    int esp_interval_ms = 16;
    std::wstring esp_report_path;
    std::wstring esp_capture_path;
    std::wstring esp_offsets_path;
};

void Field(const char* key, const std::string& value) { g_log.Field(key, value); }
std::string StatusHex(NTSTATUS st) { return Hex((u32)st, 8); }

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = false;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
    return elevated;
}

DWORD OsBuild() {
    struct OsVersionInfo {
        ULONG size, major, minor, build, platform;
        wchar_t csd[128];
    } info{};
    info.size = sizeof(info);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtl = (LONG(WINAPI*)(OsVersionInfo*))GetProcAddress(ntdll, "RtlGetVersion");
    if (!rtl || rtl(&info) < 0) return 0;
    return info.build;
}

                                                                              
                                                                          
                                                                               
                                                     
                                             
                                                                            
                                            
                                               
                                                                              
std::vector<PhysRange> BuildScanRanges(const Options& opt) {
    std::vector<PhysRange> ranges = {
        {0x1000ull, 0x9F000ull},                                                
        {0x100000ull, 0x80000000ull},                      
        {0x100000000ull, 0x840000000ull},                             
    };
    if (opt.max_phys) {
                                                                              
        for (auto& r : ranges) {
            if (r.end > opt.max_phys) r.end = opt.max_phys > r.start ? opt.max_phys : r.start;
        }
    }
    if (opt.scan_from) {
        for (auto& r : ranges) {
            if (r.start < opt.scan_from) r.start = opt.scan_from < r.end ? opt.scan_from : r.end;
        }
    }
    return ranges;
}

u64 MaxPhysicalAddress() {
    ULONGLONG kb = 0;
    if (!GetPhysicallyInstalledSystemMemory(&kb)) kb = 8ull * 1024 * 1024;                  
    u64 bytes = kb * 1024ull + (2ull << 30);
    if (bytes > (64ull << 30)) bytes = 64ull << 30;
    return bytes;
}

                                                                              
                                
                                                                              
                                                                                
                                                                   
                                                                              
                                                                                
                                                                          
                                                                           
bool WorkingSetProbeReport() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    size_t page = si.dwPageSize ? si.dwPageSize : 0x1000;
    u8* buffer = (u8*)VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer) return false;
    buffer[0] = 0x5A;
    struct {
        u64 va;
        u64 flags;
    } ws{(u64)(uintptr_t)buffer, 0};
    BOOL ok = QueryWorkingSetEx(GetCurrentProcess(), &ws, sizeof(ws));
    Field("own_page_va", Json(Hex(ws.va)));
    Field("working_set_query", Logger::Bool(ok != 0));
    Field("working_set_flags", Json(Hex(ws.flags)));
    Field("working_set_pfn_field", Json(Hex((ws.flags >> 12) & 0xFFFFFFFFFull)));
    VirtualFree(buffer, 0, MEM_RELEASE);
    g_log.Event("working_set_probe");
    return ok != 0;
}

                                                                              
                
                                                                              
struct Session {
    DriverService helper_service;
    PgrDevice pgr;
                                                                              
                                                                                 
                             
    std::unique_ptr<IMemoryProvider> provider;
    std::vector<KernelModule> modules;
    const KernelModule* nt = nullptr;
    const KernelModule* ci = nullptr;
    u32 ci_options = 0;
    PeFile disk_nt;
    Translator translator;
    CiImage image;
    std::vector<CiCandidate> candidates;
    ci::NotifyTarget notify;
    u64 kernel_cr3 = 0;
    bool translation_ready = false;
};

                                                                            
                                                                     
int RecoverLeftovers() {
    std::vector<std::wstring> names = DriverService::FindLeftovers();
    Field("leftover_count", Logger::Num(names.size()));
    int cleared = 0;
    for (const auto& n : names) {
        bool key_removed = false;
        NTSTATUS st = DriverService::UnloadByName(n, &key_removed);
        g_log.Event("leftover_recovered", "\"service\":" + Json(Narrow(n)) + ",\"unload_status\":" +
                                               Json(StatusHex(st)) +
                                               ",\"key_removed\":" + Logger::Bool(key_removed));
        if (st >= 0 || key_removed) ++cleared;
    }
    return cleared;
}

                                                              
bool StartHelper(const Options& opt, Session& s, std::string* error) {
    if (DriverService::DevicePresent(L"PGRHostControl")) {
        RecoverLeftovers();
        for (int i = 0; i < 40 && DriverService::DevicePresent(L"PGRHostControl"); ++i) Sleep(25);
        if (DriverService::DevicePresent(L"PGRHostControl")) {
            *error = "\\Device\\PGRHostControl is still present after cleanup";
            return false;
        }
    }
    std::string sha;
    if (!Sha256File(opt.pgr_path, &sha)) {
        *error = "cannot hash helper driver";
        return false;
    }
    Field("helper_path", Json(Narrow(opt.pgr_path)));
    Field("helper_sha256", Json(sha));
    if (sha != kPgrSha256) {
        *error = "helper driver identity does not match the pinned original";
        return false;
    }
    if (!s.helper_service.Create(opt.pgr_path, error)) return false;
    Field("helper_service", Json(Narrow(s.helper_service.service_name())));
    NTSTATUS st = s.helper_service.Load(error);
    Field("helper_load_status", Json(StatusHex(st)));
    bool present = DriverService::DevicePresent(L"PGRHostControl");
    Field("helper_device_present", Logger::Bool(present));
    g_log.Event(st >= 0 ? "helper_loaded" : "helper_load_failed");
    if (st < 0) return false;
    if (!s.pgr.Open(error)) return false;
    u32 declared = 0;
    std::string hs_error;
    bool hs = s.pgr.Handshake(&declared, &hs_error);
    Field("handshake_ok", Logger::Bool(hs));
    Field("handshake_size", Json(Hex(declared, 4)));
    if (!hs) Field("handshake_note", Json(hs_error));
    g_log.Event("helper_handshake");
    if (!hs) g_log.Event("helper_handshake_soft");
    g_log.Event("helper_ready");
    return true;
}

                                                                              
                                                                               
                                                                          
bool PhysicalReadProof(Session& s, std::string* error) {
    if (!s.translation_ready || !s.ci) {
        *error = "translation not ready";
        return false;
    }
    u8 live[0x400]{};
    if (!s.translator.ReadVa(s.ci->base, live, sizeof(live), error)) return false;
    u8 live_again[0x400]{};
    if (!s.translator.ReadVa(s.ci->base, live_again, sizeof(live_again), error)) return false;
    bool stable = std::memcmp(live, live_again, sizeof(live)) == 0;

    wchar_t win[MAX_PATH]{};
    GetWindowsDirectoryW(win, MAX_PATH);
    std::vector<u8> disk;
    bool disk_ok = ReadWholeFile(std::wstring(win) + L"\\System32\\ci.dll", &disk) && disk.size() >= sizeof(live);
    bool matches = disk_ok && std::memcmp(live, disk.data(), sizeof(live)) == 0;

    Field("ci_live_pa", Json(Hex(s.translator.VaToPa(s.ci->base))));
    Field("ci_live_header", Json(HexBytes(live, 16)));
    Field("ci_read_stable", Logger::Bool(stable));
    Field("ci_header_matches_disk", Logger::Bool(matches));
    g_log.Event(stable && matches ? "physical_read_verified" : "physical_read_mismatch");
    if (!stable) {
        *error = "two reads of the same ci.dll page disagreed";
        return false;
    }
    if (!matches) {
        *error = disk_ok ? "live ci.dll header differs from the on-disk image"
                         : "on-disk ci.dll could not be read for comparison";
        return false;
    }
    return true;
}

                                                                 
bool Prepare(const Options& opt, Session& s, bool need_candidates, bool collect_roots, std::string* error) {
                                                                         
                                                                                
                                                     
    bool debug_was = false;
    bool debug_ok = DriverService::EnablePrivilegeId(g_api, 20                     , true, &debug_was);
    Field("debug_privilege", Logger::Bool(debug_ok));
    if (!QueryModules(g_api, &s.modules) || s.modules.empty()) {
        *error = "NtQuerySystemInformation(SystemModuleInformation) failed";
        return false;
    }
    s.nt = FindModule(s.modules, "ntoskrnl.exe");
    s.ci = FindModule(s.modules, "ci.dll");
    Field("module_count", Logger::Num(s.modules.size()));
    Field("ntoskrnl_base", Json(Hex(s.nt ? s.nt->base : 0)));
    Field("ntoskrnl_size", Json(Hex(s.nt ? s.nt->size : 0)));
    Field("ci_base", Json(Hex(s.ci ? s.ci->base : 0)));
    Field("ci_size", Json(Hex(s.ci ? s.ci->size : 0)));
    g_log.Event("modules");
    if (!s.nt || !s.ci || !s.nt->base || !s.ci->base) {
        *error = "kernel module bases are hidden; elevation is required";
        return false;
    }
    if (QueryCiOptions(g_api, &s.ci_options)) {
        Field("ci_options", Json(Hex(s.ci_options, 8)));
        g_log.Event("ci_state");
    }

    wchar_t win[MAX_PATH]{};
    GetWindowsDirectoryW(win, MAX_PATH);
    if (!s.disk_nt.Load(std::wstring(win) + L"\\System32\\ntoskrnl.exe")) {
        *error = "cannot read the on-disk ntoskrnl.exe header";
        return false;
    }

    s.translator.Init(s.provider.get(), error);
    s.translator.SetCollectAllRoots(collect_roots);
    std::vector<PhysRange> ranges = BuildScanRanges(opt);
    for (size_t i = 0; i < ranges.size(); ++i) {
        g_log.Event("scan_range", "\"index\":" + Logger::Num(i) + ",\"start\":" + Json(Hex(ranges[i].start)) +
                                      ",\"end\":" + Json(Hex(ranges[i].end)));
    }
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    bool discovered = s.translator.Discover(s.nt->base, s.nt->size, s.disk_nt.timestamp, ranges, error);
    QueryPerformanceCounter(&t1);
    {
        double seconds = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        double mib = (double)s.translator.scanned_bytes() / (1024.0 * 1024.0);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f", seconds);
        Field("scan_seconds", Json(buf));
        std::snprintf(buf, sizeof(buf), "%.1f", seconds > 0.0 ? mib / seconds : 0.0);
        Field("scan_mib_per_s", Json(buf));
        Field("scan_candidates", Logger::Num(s.translator.candidate_pages()));
        Field("scan_skipped_windows", Logger::Num(s.translator.scan_skipped_windows()));
        std::string skipped;
        for (u64 b : s.translator.scan_skipped_examples()) {
            if (!skipped.empty()) skipped += ",";
            skipped += Hex(b);
        }
        if (!skipped.empty()) Field("scan_skipped_examples", Json(skipped));
        if (!s.translator.scan_error_note().empty()) Field("scan_read_error", Json(s.translator.scan_error_note()));
        g_log.Event("scan_timing");
    }
    if (!discovered) {
                                                                               
                                                          
        return false;
    }
    Field("cr3", Json(Hex(s.translator.cr3())));
    Field("candidate_roots", Logger::Num((u64)s.translator.candidate_table_roots().size()));
    Field("scan_position", Json(Hex(s.translator.scanned_bytes())));
    g_log.Event("translation_ready");
    s.kernel_cr3 = s.translator.cr3();

    u8 live[0x200]{};
    std::string live_error;
    bool live_ok = s.translator.ReadVa(s.nt->base, live, sizeof(live), &live_error);
    if (!live_ok) {
                                                                                  
                                                                              
                                                                            
                                          
        Field("kernel_root_retry", Json(live_error));
        const u64 first_root = s.translator.cr3();
        for (u64 root : s.translator.candidate_table_roots()) {
            if (!s.translator.AdoptAddressSpace(root)) continue;
            std::string ignored;
            if (!s.translator.ReadVa(s.nt->base, live, sizeof(live), &ignored)) continue;
            if (live[0] == 'M' && live[1] == 'Z') {
                live_ok = true;
                Field("kernel_root_used", Json(Hex(root)));
                break;
            }
        }
        if (!live_ok) {
            s.translator.AdoptAddressSpace(first_root);
            *error = live_error;
            return false;
        }
    }
    u32 lfanew = *(u32*)(live + 0x3C);
    bool header_ok = live[0] == 'M' && live[1] == 'Z' && lfanew < 0x200 && *(u32*)(live + lfanew) == 0x00004550;
    bool stamp_ok = header_ok && (*(u32*)(live + lfanew + 8) == s.disk_nt.timestamp);
    Field("live_header_ok", Logger::Bool(header_ok));
    Field("live_timestamp_matches_disk", Logger::Bool(stamp_ok));
    g_log.Event(header_ok ? "translation_verified" : "translation_mismatch");
    if (!header_ok) {
        *error = "translated read does not match the live kernel image";
        return false;
    }
    s.kernel_cr3 = s.translator.cr3();
    s.translation_ready = true;

    if (!ci::ReadImage(s.translator, s.ci->base, s.ci->size, &s.image, error)) return false;
    Field("ci_sections", Logger::Num(s.image.sections.size()));
    Field("ci_timestamp", Json(Hex(s.image.timestamp, 8)));
    g_log.Event("ci_image_parsed");

    if (need_candidates) {
                                                                           
                                                                             
                                                                              
                                                         
        std::string local_error;
        if (!ci::LocateNotifyTargets(s.disk_nt, &s.notify, &local_error)) {
            Field("notify_note", Json(local_error));
            *error = "reference target selection failed: " + local_error;
            return false;
        }
        Field("notify_set_rva", Json(Hex(s.notify.set_rva, 8)));
        Field("notify_remove_rva", Json(Hex(s.notify.remove_rva, 8)));
        Field("notify_mask_rva", Json(Hex(s.notify.mask_rva, 8)));
        Field("notify_mask_bit", Logger::Num(s.notify.mask_bit));
        Field("notify_mask_site_rva", Json(Hex(s.notify.mask_site_rva, 8)));
        Field("notify_note", Json(s.notify.note));
        g_log.Event("notify_target_located");

                                                                      
        u8 code[16]{};
        u64 set_va = s.nt->base + s.notify.set_rva;
        g_log.Event("notify_read_begin", "\"target\":\"routine\",\"va\":" + Json(Hex(set_va)) +
                                             ",\"pa\":" + Json(Hex(s.translator.VaToPa(set_va))));
        if (s.translator.ReadVa(set_va, code, sizeof(code), &local_error)) {
            Field("notify_set_va", Json(Hex(set_va)));
            Field("notify_set_pa", Json(Hex(s.translator.VaToPa(set_va))));
            Field("notify_set_bytes", Json(HexBytes(code, 8)));
        } else {
            Field("notify_set_error", Json(local_error));
        }
        u64 mask_va = s.nt->base + s.notify.mask_rva;
        u32 mask = 0;
        if (s.translator.ReadVa(mask_va, &mask, sizeof(mask), &local_error)) {
            Field("notify_mask_va", Json(Hex(mask_va)));
            Field("notify_mask_pa", Json(Hex(s.translator.VaToPa(mask_va))));
            Field("notify_mask_value", Json(Hex(mask, 8)));
        } else {
            Field("notify_mask_error", Json(local_error));
        }
        g_log.Event("notify_target_live");
    }
    return true;
}

                                                                              
                                                                           
                                                                              
struct TrialResult {
    NTSTATUS load = STATUS_UNSUCCESSFUL;
    NTSTATUS unload = 0;
    bool device = false;
    bool device_final = false;
    bool service_removed = false;
    bool create_failed = false;
    std::string error;
};

TrialResult TryLoadDriver(const std::wstring& image, const wchar_t* dos_name) {
    TrialResult r;
    DriverService service;
    if (!service.Create(image, &r.error)) {
        r.create_failed = true;
        return r;
    }
    r.load = service.Load(&r.error);
    r.device = dos_name ? DriverService::DevicePresent(dos_name) : false;
    if (r.load >= 0) {
        for (int i = 0; i < 20 && dos_name && !r.device; ++i) {
            Sleep(10);
            r.device = DriverService::DevicePresent(dos_name);
        }
        r.unload = service.Unload();
    }
    r.device_final = dos_name ? DriverService::DevicePresent(dos_name) : false;
    r.service_removed = service.Delete();
    return r;
}

std::string TrialJson(const TrialResult& r) {
    std::string s = "\"load_status\":" + Json(StatusHex(r.load)) +
                    ",\"unload_status\":" + Json(StatusHex(r.unload)) + ",\"device\":" +
                    Logger::Bool(r.device) + ",\"device_final\":" + Logger::Bool(r.device_final) +
                    ",\"service_removed\":" + Logger::Bool(r.service_removed);
    if (!r.error.empty()) s += ",\"error\":" + Json(r.error);
    return s;
}

                                                                              
std::vector<float> SplitFloats(const std::wstring& text) {
    std::vector<float> values;
    std::wstring current;
    for (wchar_t ch : text) {
        if (ch == L',' || ch == L';' || ch == L' ' || ch == L'\t') {
            if (!current.empty()) {
                values.push_back(static_cast<float>(wcstod(current.c_str(), nullptr)));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) values.push_back(static_cast<float>(wcstod(current.c_str(), nullptr)));
    return values;
}

bool ParseArgs(int argc, wchar_t** argv, Options* opt) {
    std::wstring dir = ExeDir();
    opt->pgr_path = dir + L"\\drivers\\PGRHostControl.sys";
    opt->driver_path = dir + L"\\drivers\\WardogProbe.sys";
    opt->fixture_path = dir + L"\\WardogFixture.exe";
    opt->case_path = dir + L"\\..\\cases\\case-000-reference.json";
    opt->log_path = dir + L"\\wardog-launcher.jsonl";
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&](std::wstring* out) {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        if (a == L"--mode") {
            if (!next(&opt->mode)) return false;
        } else if (a == L"--pgr") {
            if (!next(&opt->pgr_path)) return false;
        } else if (a == L"--driver") {
            if (!next(&opt->driver_path)) return false;
        } else if (a == L"--log") {
            if (!next(&opt->log_path)) return false;
        } else if (a == L"--case") {
            if (!next(&opt->case_path)) return false;
        } else if (a == L"--fixture") {
            if (!next(&opt->fixture_path)) return false;
        } else if (a == L"--provider") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->provider_id = Narrow(v);
        } else if (a == L"--pid") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->pid = (u32)wcstoul(v.c_str(), nullptr, 0);
        } else if (a == L"--process") {
            if (!next(&opt->target_process)) return false;
        } else if (a == L"--spawn-target") {
            opt->spawn_target = true;
        } else if (a == L"--check") {
            opt->check_only = true;
        } else if (a == L"--va") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->read_vas.push_back(wcstoull(v.c_str(), nullptr, 0));
        } else if (a == L"--rva") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->read_rvas.push_back(wcstoull(v.c_str(), nullptr, 0));
        } else if (a == L"--len") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->read_len = (u32)wcstoul(v.c_str(), nullptr, 0);
            if (!opt->read_len || opt->read_len > 0x100000) return false;
        } else if (a == L"--base") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->read_base = wcstoull(v.c_str(), nullptr, 0);
        } else if (a == L"--no-rpm") {
            opt->no_rpm = true;
        } else if (a == L"--cr3") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->force_cr3 = wcstoull(v.c_str(), nullptr, 0);
        } else if (a == L"--no-kernel-route") {
            opt->no_kernel_route = true;
        } else if (a == L"--max-phys") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->max_phys = wcstoull(v.c_str(), nullptr, 0);
        } else if (a == L"--scan-from") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->scan_from = wcstoull(v.c_str(), nullptr, 0);
        } else if (a == L"--allow-write") {
            opt->allow_write = true;
        } else if (a == L"--patch-rva") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->patch_rva = (u32)wcstoul(v.c_str(), nullptr, 0);
            opt->has_patch_rva = true;
        } else if (a == L"--dry-run") {
            opt->dry_run = true;
        } else if (a == L"--esp-demo") {
            opt->esp_demo = true;
        } else if (a == L"--esp-live") {
            opt->esp_live = true;
        } else if (a == L"--esp-probe") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->esp_probe = true;
            opt->esp_probe_va = wcstoull(v.c_str(), nullptr, 0);
        } else if (a == L"--esp-actor") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->esp_actors.push_back(wcstoull(v.c_str(), nullptr, 0));
        } else if (a == L"--esp-vehicle") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->esp_vehicles.push_back(wcstoull(v.c_str(), nullptr, 0));
        } else if (a == L"--esp-world") {
            std::wstring v;
            if (!next(&v)) return false;
            const std::vector<float> values = SplitFloats(v);
            if (values.size() < 3) return false;
            Options::EspPoint point;
            point.feet = Vec3{values[0], values[1], values[2]};
            if (values.size() > 3 && values[3] > 0.0f) point.height_cm = values[3];
            if (values.size() > 4) point.kind = values[4] != 0.0f ? 1 : 0;
            opt->esp_points.push_back(point);
        } else if (a == L"--esp-camera") {
            std::wstring v;
            if (!next(&v)) return false;
            const std::vector<float> values = SplitFloats(v);
            if (values.size() < 5) return false;
            opt->esp_camera.location = Vec3{values[0], values[1], values[2]};
            opt->esp_camera.pitch = values[3];
            opt->esp_camera.yaw = values[4];
            if (values.size() > 5 && values[5] > 1.0f && values[5] < 179.0f) opt->esp_camera.fov = values[5];
            opt->esp_camera_set = true;
        } else if (a == L"--esp-scale") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->esp_scale = static_cast<float>(wcstod(v.c_str(), nullptr));
            if (!(opt->esp_scale > 0.2f) || !(opt->esp_scale < 4.0f)) return false;
        } else if (a == L"--esp-thickness") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->esp_thickness = static_cast<float>(wcstod(v.c_str(), nullptr));
            if (!(opt->esp_thickness > 0.0f) || !(opt->esp_thickness < 16.0f)) return false;
        } else if (a == L"--esp-max-distance") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->esp_max_distance = static_cast<float>(wcstod(v.c_str(), nullptr));
        } else if (a == L"--esp-max-boxes") {
            std::wstring v;
            if (!next(&v)) return false;
            const int value = (int)wcstol(v.c_str(), nullptr, 0);
            if (value <= 0 || value > 8192) return false;
            opt->esp_max_boxes = value;
        } else if (a == L"--esp-interval") {
            std::wstring v;
            if (!next(&v)) return false;
            const int value = (int)wcstol(v.c_str(), nullptr, 0);
            if (value < 0 || value > 1000) return false;
            opt->esp_interval_ms = value;
        } else if (a == L"--esp-seconds") {
            std::wstring v;
            if (!next(&v)) return false;
            opt->esp_seconds = wcstod(v.c_str(), nullptr);
            if (!(opt->esp_seconds >= 0.0)) return false;
        } else if (a == L"--esp-capture") {
            if (!next(&opt->esp_capture_path)) return false;
        } else if (a == L"--esp-report") {
            if (!next(&opt->esp_report_path)) return false;
        } else if (a == L"--esp-offsets") {
            if (!next(&opt->esp_offsets_path)) return false;
        } else if (a == L"--esp-vsync") {
            opt->esp_vsync = true;
        } else if (a == L"--esp-corners") {
            opt->esp_corners = true;
        } else {
            return false;
        }
    }
    return opt->mode == L"probe" || opt->mode == L"scan" || opt->mode == L"run" ||
           opt->mode == L"selftest" || opt->mode == L"cleanup" || opt->mode == L"case" ||
           opt->mode == L"read" || opt->mode == L"esp";
}

void Usage() {
    std::printf(
        "WardogLauncher -- PGR-first loader with physical-memory assisted driver load\n\n"
        "Usage: WardogLauncher.exe [--mode probe|scan|read|run|case|selftest|cleanup|esp] [options]\n"
        "  --mode selftest  host-free check of the paging walk and ci.dll finder\n"
        "  --mode probe   load PGR, open the device, prove real physical read/write\n"
        "  --mode scan    probe + locate the code-integrity target (read only)\n"
        "  --mode read    read another process at given addresses through the physical map\n"
        "  --mode case    run a case file: named process, named addresses, byte check\n"
        "  --mode run     full flow: control run, patch, load, restore, verify (needs --allow-write)\n"
        "  --mode cleanup unload/delete leftover WardogLaunch_* services from an aborted run\n"
        "  --mode read takes: --pid N [--va ADDR]... [--len N] [--base ADDR] [--no-rpm]\n"
        "      --va    address to read (repeatable; default: image base and entry point)\n"
        "      --len   bytes per address (default 256)\n"
        "      --base  initial image base to try when the module list is hidden\n"
        "      --no-rpm skip the ReadProcessMemory cross-check\n"
        "  --case PATH    case file to run (default: ..\\cases\\case-000-reference.json)\n"
        "  --spawn-target start WardogFixture.exe to materialise the case target image\n"
        "  --pid N        target process for --mode read, or override the case target\n"
        "  --fixture PATH fixture executable (default: WardogFixture.exe next to this one)\n"
        "  --provider ID  memory provider: pgr (default)\n"
        "  --pgr PATH     helper driver image (default: drivers\\PGRHostControl.sys)\n"
        "  --driver PATH  unsigned driver to load (default: drivers\\WardogProbe.sys)\n"
        "  --patch-rva X  instead of the CI options word, patch 48 31 C0 C3 at ci.dll+X\n"
        "  --max-phys N   upper bound for the RAM search (diagnostics only)\n"
        "  --scan-from N  start the RAM search at this physical address\n"
        "  --allow-write  permit the kernel-memory write step of --mode run (off by default)\n"
        "  --dry-run      stop before the first write to kernel memory\n"
        "  --log PATH     JSONL log destination\n"
        "\n"
        "  --mode esp     D3D12 target boxes over the game window\n"
        "      --esp-demo          fixed synthetic scene (no driver, no game needed)\n"
        "      --esp-camera X,Y,Z,PITCH,YAW[,FOV]  view to project with\n"
        "      --esp-world X,Y,Z[,HEIGHT_CM][,1]   add a world point (1 = vehicle colour)\n"
        "      --esp-actor VA      read an actor through the physical map and box it\n"
        "      --esp-vehicle VA    same, drawn with the vehicle height and colour\n"
        "      --esp-live          sweep the actor list from the offset profile\n"
        "      --esp-probe VA      report candidate root components of an actor\n"
        "      --esp-offsets PATH  offset profile (default: offsets\\wardog-esp.json)\n"
        "      --esp-scale F       box scale, 1.0 = reference ratio (0.2 .. 4.0)\n"
        "      --esp-thickness F   border thickness in pixels (default 1)\n"
        "      --esp-max-distance  drop boxes farther than this\n"
        "      --esp-max-boxes N   box budget per frame (default 512)\n"
        "      --esp-interval MS   frame interval (default 16)\n"
        "      --esp-seconds F     stop after this many seconds (default: F9)\n"
        "      --esp-report PATH   write the last frame as JSON\n"
        "      --esp-capture PATH  save the rendered frame as a BMP\n"
        "      --esp-vsync         wait for the compositor each frame\n"
        "      --esp-corners       draw corner brackets instead of full boxes\n"
        "  keys while --mode esp runs: F5 scale down, F6 scale up, F8 hide, F9 quit\n");
}

                                                                              
int RunFull(const Options& opt, Session& s) {
    std::string error;

                                                                                    
    g_log.Event("control_begin", "\"case\":\"baseline_unsigned\"");
    TrialResult baseline = TryLoadDriver(opt.driver_path, L"WardogProbe");
    g_log.Event("control_result", "\"case\":\"baseline_unsigned\"," + TrialJson(baseline));
    if (baseline.load >= 0) {
        g_log.Event("abort", "\"reason\":\"baseline load admitted; this host is not enforcing CI\"");
        return 11;
    }
    if (!opt.allow_write) {
        g_log.Event("write_refused",
                    "\"reason\":\"kernel writes are disabled; pass --allow-write to run the patch step\"");
        return 13;
    }
    if (opt.dry_run) {
        g_log.Event("dry_run_end");
        return 0;
    }

                                                                               
                                                                    
    struct PatchEntry {
        u64 va = 0;
        std::string label;
        bool code = false;
        u8 original[4]{};
        u8 temporary[4]{};
        bool restored = false;
    };
    std::vector<PatchEntry> entries;
    {
        const u8 ret_zero[4] = {0x48, 0x31, 0xC0, 0xC3};                      
        if (opt.has_patch_rva) {
            PatchEntry e;
            e.va = s.ci->base + opt.patch_rva;
            e.label = "custom_code";
            e.code = true;
            std::memcpy(e.temporary, ret_zero, 4);
            entries.push_back(e);
        } else {
            if (!s.notify.set_rva || !s.notify.mask_rva) {
                g_log.Event("abort", "\"reason\":\"notify targets were not located\"");
                return 10;
            }
            PatchEntry body;
            body.va = s.nt->base + s.notify.set_rva;
            body.label = "notify_routine";
            body.code = true;
            std::memcpy(body.temporary, ret_zero, 4);
            PatchEntry mask;
            mask.va = s.nt->base + s.notify.mask_rva;
            mask.label = "notify_mask";
            mask.code = false;
            entries.push_back(body);
            entries.push_back(mask);
        }
    }

                                                                    
    for (auto& e : entries) {
        if (!ci::ReadBytes(s.translator, e.va, e.original, 4, &error)) {
            g_log.Event("abort", "\"reason\":\"save original content\"");
            return 13;
        }
        g_log.Event("original_content_saved", "\"target\":" + Json(e.label) + ",\"va\":" + Json(Hex(e.va)) +
                                                  ",\"bytes\":" + Json(HexBytes(e.original, 4)));
        if (!ci::WriteBytes(s.translator, e.va, e.temporary, 4, &error)) {
            g_log.Event("abort", "\"reason\":\"write temporary content\"");
            return 13;
        }
        u8 check[4]{};
        ci::ReadBytes(s.translator, e.va, check, 4, &error);
        g_log.Event("temporary_content_written", "\"target\":" + Json(e.label) + ",\"va\":" + Json(Hex(e.va)) +
                                                     ",\"bytes\":" + Json(HexBytes(check, 4)) +
                                                     ",\"match\":" +
                                                     Logger::Bool(std::memcmp(check, e.temporary, 4) == 0));
    }

                                                                          
                                                  
    g_log.Event("followup_load_begin", "\"driver\":" + Json(Narrow(opt.driver_path)));
    DriverService followup;
    std::string load_error;
    TrialResult load;
    if (!followup.Create(opt.driver_path, &load_error)) {
        load.error = load_error;
        load.create_failed = true;
    } else {
        load.load = followup.Load(&load_error);
        load.error = load_error;
        if (load.load >= 0) {
            load.device = DriverService::DevicePresent(L"WardogProbe");
            for (int i = 0; i < 20 && !load.device; ++i) {
                Sleep(10);
                load.device = DriverService::DevicePresent(L"WardogProbe");
            }
        }
    }
    g_log.Event("followup_load_result", TrialJson(load));

                               
    bool all_restored = true;
    for (auto& e : entries) {
        ci::WriteBytes(s.translator, e.va, e.original, 4, &error);
        u8 check[4]{};
        ci::ReadBytes(s.translator, e.va, check, 4, &error);
        e.restored = std::memcmp(check, e.original, 4) == 0;
        all_restored = all_restored && e.restored;
        g_log.Event("original_content_restored", "\"target\":" + Json(e.label) + ",\"va\":" + Json(Hex(e.va)) +
                                                     ",\"bytes\":" + Json(HexBytes(check, 4)) +
                                                     ",\"match\":" + Logger::Bool(e.restored));
    }

                                                                   
    if (load.load >= 0) {
        load.unload = followup.Unload();
    }
    load.device_final = DriverService::DevicePresent(L"WardogProbe");
    load.service_removed = followup.Delete();
    g_log.Event("followup_unload",
                "\"unload_status\":" + Json(StatusHex(load.unload)) + ",\"device_final\":" +
                    Logger::Bool(load.device_final) + ",\"service_removed\":" + Logger::Bool(load.service_removed));

                                                                                
    g_log.Event("control_begin", "\"case\":\"after_restore\"");
    TrialResult after = TryLoadDriver(opt.driver_path, L"WardogProbe");
    g_log.Event("control_result", "\"case\":\"after_restore\"," + TrialJson(after));

    int result = 0;
    if (load.load < 0) result = 14;
    else if (!load.device) result = 15;
    else if (!all_restored) result = 16;
    else if (after.load >= 0) result = 17;

    g_log.Event("summary", "\"gate\":" + Json(entries.empty() ? std::string() : entries.front().label) +
                               ",\"gate_va\":" + Json(Hex(entries.empty() ? 0 : entries.front().va)) +
                               ",\"targets\":" + Logger::Num((u64)entries.size()) +
                               ",\"followup_load\":" + Json(StatusHex(load.load)) +
                               ",\"driver_ran\":" + Logger::Bool(load.device) +
                               ",\"restored\":" + Logger::Bool(all_restored) +
                               ",\"control_after_restore\":" + Json(StatusHex(after.load)) +
                               ",\"result\":" + Json(result == 0 ? "success" : "incomplete"));
    return result;
}

                                                                              
                        
                                                                              
                                                                               
                                                                                
                                                                                
                                                                           
                                                  

u32 FindProcessByName(const std::wstring& name) {
    u32 pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name.c_str()) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

                                                                          
                                                                               
                        
struct FixtureProcess {
    PROCESS_INFORMATION pi{};
    std::wstring status_path;
    bool started = false;

    ~FixtureProcess() { Stop(); }

    bool Start(const std::wstring& exe, const CaseFile& c, u32* pid, std::string* error) {
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            *error = "fixture executable not found: " + Narrow(exe);
            return false;
        }
        size_t slash = exe.find_last_of(L"\\/");
        std::wstring dir = slash == std::wstring::npos ? L"." : exe.substr(0, slash);
        status_path = dir + L"\\fixture-status.json";
        DeleteFileW(status_path.c_str());

        std::wstring cmd = L"\"" + exe + L"\" --image \"" + c.image_path + L"\" --base " +
                           Widen(Hex(c.image_base)) + L" --status \"" + status_path +
                           L"\" --seconds 300";
        std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
        mutable_cmd.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            nullptr, &si, &pi)) {
            *error = "CreateProcess(fixture) failed, win32=" + std::to_string(GetLastError());
            return false;
        }
        started = true;

                                                                               
                                                       
        for (int i = 0; i < 200; ++i) {
            if (GetFileAttributesW(status_path.c_str()) != INVALID_FILE_ATTRIBUTES) break;
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
            Sleep(25);
        }
        std::vector<u8> raw;
        if (!ReadWholeFile(status_path, &raw) || raw.empty()) {
            *error = "fixture did not report a usable status file";
            return false;
        }
        mj::Value status;
        std::string parse_error;
        if (!mj::Parse(std::string((const char*)raw.data(), raw.size()), &status, &parse_error)) {
            *error = "fixture status is not JSON: " + parse_error;
            return false;
        }
        if (!status.Find("ok") || !status.Find("ok")->AsBool(false)) {
            const mj::Value* reason = status.Find("error");
            *error = "fixture refused the mapping: " + (reason ? reason->AsString() : std::string("unknown"));
            return false;
        }
        u64 child_pid = 0;
        if (!status.Find("pid") || !status.Find("pid")->AsU64(&child_pid, 0) || !child_pid) {
            *error = "fixture status has no pid";
            return false;
        }
        *pid = (u32)child_pid;
        return true;
    }

    void Stop() {
        if (!started) return;
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        started = false;
        if (!status_path.empty()) DeleteFileW(status_path.c_str());
    }
};

                                                                                
                                                                            
                                 
int ResolveCaseTarget(const Options& opt, CaseFile* c, FixtureProcess* fixture, u32* pid,
                      std::string* pid_source) {
    std::string error;
    if (!c->Load(opt.case_path, &error)) {
        Field("case_path", Json(Narrow(opt.case_path)));
        Field("error", Json(error));
        g_log.Event("case_load_failed");
        return 18;
    }
    Field("case_id", Json(c->id));
    Field("case_name", Json(c->name));
    Field("case_schema", Json(c->schema));
    Field("case_export_root", Json(c->export_root));
    Field("case_reference_note", Json(c->reference_note));
    Field("case_observation_count", Logger::Num((u64)c->observations.size()));
    Field("case_gate_module", Json(c->gate.module));
    Field("case_gate_routine", Json(c->gate.routine));
    Field("case_gate_patch", Json(c->gate.patch_hex));
    g_log.Event("case_loaded");

                                                                                 
                                     
    std::string sha;
    bool hashed = Sha256File(c->image_path, &sha);
    bool identity_ok = hashed && (c->image_sha256.empty() || sha == c->image_sha256);
    Field("case_image_path", Json(Narrow(c->image_path)));
    Field("case_image_sha256", Json(sha));
    Field("case_image_sha256_pinned", Json(c->image_sha256));
    g_log.Event(identity_ok ? "case_image_verified" : "case_image_mismatch");
    if (!identity_ok) return 19;

    PeFile pe;
    if (!pe.Load(c->image_path)) {
        Field("error", Json("case image is not a readable PE"));
        g_log.Event("case_image_unparsable");
        return 19;
    }
    Field("case_image_base", Json(Hex(pe.image_base, 8)));
    Field("case_image_timestamp", Json(Hex(pe.timestamp, 8)));
    Field("case_image_size", Json(Hex(pe.size_of_image, 8)));
    Field("case_image_base_matches", Logger::Bool(pe.image_base == c->image_base));
    g_log.Event("case_image_pe");

    u32 found = opt.pid;
    std::string source = "argument";
    if (!found) {
        found = FindProcessByName(Widen(c->process_name));
        source = "running";
    }
    if (!found && opt.spawn_target && !opt.check_only) {
        std::string spawn_error;
        if (!fixture->Start(opt.fixture_path, *c, &found, &spawn_error)) {
            Field("error", Json(spawn_error));
            g_log.Event("target_fixture_failed");
            return 20;
        }
        source = "fixture";
    }
    Field("target_process", Json(c->process_name));
    Field("target_pid", Logger::Num(found));
    Field("target_pid_source", Json(source));
    g_log.Event(found ? "target_process_ready" : "target_process_absent");
    if (!found && !opt.check_only) return 20;

    *pid = found;
    *pid_source = source;
    return 0;
}

                                                                               
                                                                                
                                                                           
int ValidateCaseOffline(const CaseFile& c) {
    PeFile pe;
    if (!pe.Load(c.image_path)) {
        Field("error", Json("case image is not a readable PE"));
        g_log.Event("case_offline_failed");
        return 19;
    }
    int verified = 0, virtual_only = 0, mismatched = 0, out_of_image = 0;
    for (const auto& obs : c.observations) {
        Field("observation", Json(obs.name));
        Field("scope", Json(obs.scope));
        Field("va", Json(Hex(obs.va)));
        Field("length", Logger::Num(obs.length));
        if (obs.scope != "process") {
            g_log.Event("case_offline_scope_skipped");
            continue;
        }
        if (obs.va < c.image_base || obs.va - c.image_base >= pe.size_of_image) {
            Field("error", Json("address is outside the image"));
            g_log.Event("case_offline_out_of_image");
            ++out_of_image;
            continue;
        }
        u32 rva = (u32)(obs.va - c.image_base);
        long offset = -1;
        if (rva < pe.size_of_headers && (u64)rva + obs.length <= pe.size_of_headers) {
                                                                                 
            offset = (long)rva;
        }
        for (const auto& s : pe.sections) {
            if (offset >= 0) break;
            u32 span = s.vsize ? s.vsize : s.rawsize;
            if (rva < s.va || rva >= s.va + span) continue;
            u32 delta = rva - s.va;
            if ((u64)delta + obs.length <= s.rawsize) offset = (long)(s.raw + delta);
            break;
        }
        if (offset < 0) {
                                                                             
                                            
            Field("note", Json(obs.note));
            if (!obs.has_expectation()) {
                g_log.Event("case_offline_runtime_only");
                ++virtual_only;
            } else {
                Field("error", Json("expectation points at a virtual-only section"));
                g_log.Event("case_offline_expectation_unbacked");
                ++mismatched;
            }
            continue;
        }
        Field("file_offset", Json(Hex((u64)offset)));
        Field("observed", Json(casefile::ToHex(pe.bytes.data() + offset, obs.length)));
        if (!obs.has_expectation()) {
            g_log.Event("case_offline_runtime_only");
            ++virtual_only;
            continue;
        }
        std::vector<u8> expected;
        std::string decode_error;
        bool expected_ok = casefile::ParseHexBytes(obs.expect_hex, &expected, &decode_error);
        bool match = expected_ok && expected.size() == obs.length &&
                     std::memcmp(pe.bytes.data() + offset, expected.data(), obs.length) == 0;
        Field("expected", Json(obs.expect_hex));
        Field("match", Logger::Bool(match));
        if (match) {
            g_log.Event("case_offline_verified");
            ++verified;
        } else {
            g_log.Event("case_offline_mismatch");
            ++mismatched;
        }
    }

                                                                           
    wchar_t win[MAX_PATH]{};
    GetWindowsDirectoryW(win, MAX_PATH);
    PeFile nt;
    bool nt_ok = nt.Load(std::wstring(win) + L"\\System32\\ntoskrnl.exe");
    Field("kernel_image_readable", Logger::Bool(nt_ok));
    if (nt_ok && c.gate.has_routine_rva) {
        u32 export_rva = nt.ExportRva(c.gate.routine.c_str());
        Field("gate_export_rva", Json(Hex(export_rva, 8)));
        Field("gate_routine_rva_pinned", Json(Hex(c.gate.routine_rva, 8)));
        bool same = export_rva == c.gate.routine_rva;
        Field("gate_export_matches", Logger::Bool(same));
        g_log.Event(same ? "case_offline_gate_export_verified" : "case_offline_gate_export_mismatch");
        if (!same) ++mismatched;
    }
    if (nt_ok && c.gate.has_flag_rva) {
        long offset = nt.OffsetOf(c.gate.flag_rva);
        Field("gate_flag_rva_pinned", Json(Hex(c.gate.flag_rva, 8)));
        Field("gate_flag_backed", Logger::Bool(offset >= 0));
        g_log.Event(offset >= 0 ? "case_offline_gate_flag_verified" : "case_offline_gate_flag_unbacked");
        if (offset < 0) ++mismatched;
    }

    int rc = (mismatched || out_of_image) ? 22 : 0;
    Field("case_id", Json(c.id));
    Field("verified", Logger::Num((u64)verified));
    Field("runtime_only", Logger::Num((u64)virtual_only));
    Field("mismatched", Logger::Num((u64)mismatched));
    Field("out_of_image", Logger::Num((u64)out_of_image));
    Field("result", Json(rc == 0 ? "success" : "incomplete"));
    g_log.Event("case_offline_summary");
    return rc;
}

int RunCase(const Options& opt, Session& s, const CaseFile& c, u32 pid, const std::string& pid_source) {
    std::string error;

                                        
    if (!s.translator.DiscoverForImage(c.image_base, c.image_timestamp, c.image_size, &error)) {
        Field("error", Json(error));
        Field("candidate_roots", Logger::Num((u64)s.translator.candidate_table_roots().size()));
        Field("candidate_pages", Logger::Num((u64)s.translator.candidate_table_pages().size()));
        Field("image_scan_roots", Logger::Num(s.translator.image_scan_roots()));
        Field("image_scan_pages", Logger::Num(s.translator.image_scan_pages()));
        g_log.Event("target_address_space_missing");
        return 21;
    }
    const u64 target_cr3 = s.translator.cr3();
    Field("target_cr3", Json(Hex(target_cr3)));
    Field("kernel_cr3", Json(Hex(s.kernel_cr3)));
    Field("candidate_roots", Logger::Num((u64)s.translator.candidate_table_roots().size()));
    Field("candidate_pages", Logger::Num((u64)s.translator.candidate_table_pages().size()));
    Field("image_scan_roots", Logger::Num(s.translator.image_scan_roots()));
    Field("image_scan_pages", Logger::Num(s.translator.image_scan_pages()));
    g_log.Event("target_address_space_ready");

                                                                           
    int verified = 0, runtime_only = 0, mismatched = 0, unreadable = 0;
    for (const auto& obs : c.observations) {
        const bool kernel_scope = obs.scope == "kernel";
        const u64 cr3 = kernel_scope ? s.kernel_cr3 : target_cr3;
        u64 va = obs.va;
        if (kernel_scope) {
            u64 module_base = s.nt ? s.nt->base : 0;
            if (!obs.module.empty() && _stricmp(obs.module.c_str(), "ntoskrnl.exe") != 0) module_base = 0;
            if (!module_base) {
                Field("observation", Json(obs.name));
                Field("scope", Json(obs.scope));
                Field("error", Json("kernel-scope observation names an unknown module"));
                g_log.Event("case_observation_unreadable");
                ++unreadable;
                continue;
            }
            va = module_base + obs.va;
        }

        std::vector<u8> buffer(obs.length);
        std::string read_error;
        const bool read_ok = s.translator.ReadVaAs(cr3, va, buffer.data(), buffer.size(), &read_error);
        const u64 pa = read_ok ? s.translator.WalkFrom(cr3, va) : 0;

        Field("observation", Json(obs.name));
        Field("scope", Json(obs.scope));
        Field("va", Json(Hex(va)));
        Field("cr3", Json(Hex(cr3)));
        Field("pa", Json(Hex(pa)));
        Field("length", Logger::Num(obs.length));
        if (!obs.note.empty()) Field("note", Json(obs.note));
        if (!read_ok) {
            Field("error", Json(read_error));
            g_log.Event("case_observation_unreadable");
            ++unreadable;
            continue;
        }
        Field("observed", Json(casefile::ToHex(buffer.data(), buffer.size())));
        if (!obs.has_expectation()) {
                                                                          
                                                                             
                                                                
            g_log.Event("case_observation_runtime");
            ++runtime_only;
            continue;
        }
        std::vector<u8> expected;
        std::string decode_error;
        bool expected_ok = casefile::ParseHexBytes(obs.expect_hex, &expected, &decode_error);
        const bool match = expected_ok && expected == buffer;
        Field("expected", Json(obs.expect_hex));
        Field("match", Logger::Bool(match));
        if (match) {
            g_log.Event("case_observation_verified");
            ++verified;
        } else {
            g_log.Event("case_observation_mismatch");
            ++mismatched;
        }
    }

                                                                       
    const bool routine_pin_ok = !c.gate.has_routine_rva || s.notify.set_rva == c.gate.routine_rva;
    const bool flag_pin_ok = !c.gate.has_flag_rva || s.notify.mask_rva == c.gate.flag_rva;
    Field("gate_routine_rva_pinned", Json(Hex(c.gate.routine_rva, 8)));
    Field("gate_routine_rva_located", Json(Hex(s.notify.set_rva, 8)));
    Field("gate_flag_rva_pinned", Json(Hex(c.gate.flag_rva, 8)));
    Field("gate_flag_rva_located", Json(Hex(s.notify.mask_rva, 8)));
    Field("gate_pin_matches", Logger::Bool(routine_pin_ok && flag_pin_ok));
    g_log.Event(routine_pin_ok && flag_pin_ok ? "case_gate_verified" : "case_gate_pin_mismatch");

                                                                              
                                                                              
                                                                       
    int rc = 0;
    if (mismatched || unreadable) {
        rc = 22;
    } else if (opt.allow_write) {
        s.translator.AdoptAddressSpace(s.kernel_cr3);
        rc = RunFull(opt, s);
    }

    Field("case_id", Json(c.id));
    Field("target_pid", Logger::Num(pid));
    Field("target_pid_source", Json(pid_source));
    Field("verified", Logger::Num((u64)verified));
    Field("runtime_only", Logger::Num((u64)runtime_only));
    Field("mismatched", Logger::Num((u64)mismatched));
    Field("unreadable", Logger::Num((u64)unreadable));
    Field("write_phase", Json(opt.allow_write ? "ran" : "disabled"));
    Field("result", Json(rc == 0 ? "success" : "incomplete"));
    g_log.Event("case_summary");
    return rc;
}

                                                                              
                                                                              
                                                      
                                                                              
                                                                                
                                                                              
                                                                          
int RunRead(const Options& opt, Session& s) {
    u32 pid = opt.pid;
    if (!pid && !opt.target_process.empty()) {
        pid = FindProcessByName(opt.target_process);
        Field("target_process_name", Json(Narrow(opt.target_process)));
    }
    if (!pid) {
        g_log.Event("fatal", "\"reason\":\"--mode read needs --pid or --process\"");
        return 2;
    }
    LiveTarget t;
    std::string error;
    std::wstring image_path;
    std::string leaf;
    if (target::PathOf(pid, &image_path, &leaf)) {
        t.image_path = image_path;
        t.leaf = leaf;
    }
    if (leaf.empty() && !opt.target_process.empty()) leaf = Narrow(opt.target_process);

                                                                                
                                                                               
                                                                             
                                                                                
                                                                            
    target::DiskImage disk;
    std::string disk_error;
    const bool disk_ok = !image_path.empty() && disk.Open(image_path, &disk_error);
    Field("disk_ground_truth", Logger::Bool(disk_ok));
    if (!disk_ok && !disk_error.empty()) Field("disk_ground_truth_note", Json(disk_error));

                                                                               
                                                                               
    bool resolved = false;
    if (opt.read_base) {
        resolved = target::ResolveWithBase(pid, opt.read_base, &t, &error);
    } else if (!opt.no_kernel_route &&
               target::FromKernel(s.translator, s.nt ? s.nt->base : 0, s.disk_nt, pid, leaf,
                                  disk_ok ? disk.timestamp : 0, &t, &error)) {
        resolved = true;
    } else {
        for (const auto& line : target::Trace()) g_log.Event("kernel_route_trace", "\"step\":" + Json(line));
        if (!error.empty()) Field("kernel_route_note", Json(error));
        error.clear();
                                                                                  
                                                                                 
                                                                                
        if (disk_ok && target::FindImageByUserScan(s.translator, disk.timestamp, disk.size_of_image, &t, &error)) {
            resolved = true;
            if (t.image_path.empty() && !image_path.empty()) t.image_path = image_path;
            Field("user_scan_note", Json(error));
            error.clear();
        } else {
            if (!error.empty()) Field("user_scan_note", Json(error));
            error.clear();
            resolved = target::Resolve(pid, &t, &error);
            if (resolved && t.image_path.empty() && !image_path.empty()) t.image_path = image_path;
        }
    }
    Field("target_pid", Logger::Num(pid));
    Field("target_leaf", Json(t.leaf));
    Field("target_image", Json(Narrow(t.image_path)));
    Field("target_image_base", Json(Hex(t.image_base)));
    Field("target_size_of_image", Json(Hex(t.size_of_image, 8)));
    Field("target_timestamp", Json(Hex(t.timestamp, 8)));
    Field("target_entry_va", Json(Hex(t.image_base + t.entry_rva)));
    Field("target_resolved_via", Json(t.resolved_via));
    Field("target_header_live", Logger::Bool(t.header_verified));
    if (!resolved) {
        Field("error", Json(error));
        g_log.Event("target_resolve_failed");
        return 23;
    }
    g_log.Event("target_resolved");

    if (opt.force_cr3) {
        s.translator.AdoptAddressSpace(opt.force_cr3);
        Field("cr3_source", Json("forced"));
    } else if (t.directory_table_base) {
                                                                            
                                     
        s.translator.AdoptAddressSpace(t.directory_table_base);
        Field("cr3_source", Json(t.resolved_via.empty() ? "directory_table_base" : t.resolved_via));
    } else if (!s.translator.DiscoverForImage(t.image_base, t.timestamp, t.size_of_image, &error)) {
        Field("error", Json(error));
        if (!s.translator.image_scan_note().empty()) Field("image_scan_note", Json(s.translator.image_scan_note()));
        Field("candidate_roots", Logger::Num((u64)s.translator.candidate_table_roots().size()));
        Field("candidate_pages", Logger::Num((u64)s.translator.candidate_table_pages().size()));
        Field("image_scan_roots", Logger::Num(s.translator.image_scan_roots()));
        Field("image_scan_pages", Logger::Num(s.translator.image_scan_pages()));
        g_log.Event("target_address_space_missing");
        return 21;
    } else {
        Field("cr3_source", Json("page_table_scan"));
    }
    if (opt.force_cr3) {
        std::vector<u8> page;
        if (s.translator.PeekPage(opt.force_cr3, &page)) {
            const u64* e = (const u64*)page.data();
            Field("probe_e0", Json(Hex(e[0])));
            Field("probe_e255", Json(Hex(e[0xFF])));
            Field("probe_e511", Json(Hex(e[511])));
            int present = 0;
            for (int i = 0; i < 512; ++i) {
                if ((e[i] & 1) && (e[i] & 0xFFF)) ++present;
            }
            Field("probe_present_entries", Logger::Num((u64)present));
            bool listed = false;
            for (u64 c : s.translator.candidate_table_pages()) {
                if (c == opt.force_cr3) { listed = true; break; }
            }
            Field("probe_in_candidates", Logger::Bool(listed));
        } else {
            Field("probe_error", Json(s.translator.last_walk_error()));
        }
    }
    const u64 cr3 = s.translator.cr3();
    Field("target_cr3", Json(Hex(cr3)));
    Field("kernel_cr3", Json(Hex(s.kernel_cr3)));
    Field("candidate_roots", Logger::Num((u64)s.translator.candidate_table_roots().size()));
    Field("candidate_pages", Logger::Num((u64)s.translator.candidate_table_pages().size()));
    Field("image_scan_roots", Logger::Num(s.translator.image_scan_roots()));
    Field("image_scan_pages", Logger::Num(s.translator.image_scan_pages()));
    g_log.Event("target_address_space_ready");

    std::vector<u64> vas = opt.read_vas;
    for (u64 rva : opt.read_rvas) vas.push_back(t.image_base + rva);
    if (vas.empty()) {
                                                                                
                                                                                
                                                                                  
                                                                                
        vas.push_back(t.image_base);
        vas.push_back(t.image_base + t.entry_rva);
        vas.push_back(t.image_base + t.entry_rva + 8);
        vas.push_back(t.image_base + t.entry_rva + 0x1000);
    }

    int verified = 0, verified_disk = 0, differs_disk = 0, agreed_kernel_root = 0, contradicted = 0, unreadable = 0;
    for (u64 va : vas) {
        const size_t length = opt.read_len;
        std::vector<u8> buffer(length);
        std::string read_error;
        const bool read_ok = s.translator.ReadVaAs(cr3, va, buffer.data(), buffer.size(), &read_error);
        const u64 pa = read_ok ? s.translator.WalkFrom(cr3, va) : 0;

        Field("va", Json(Hex(va)));
        Field("pa", Json(Hex(pa)));
        Field("length", Logger::Num(length));
        if (!read_ok) {
            Field("error", Json(read_error));
            g_log.Event("read_unmapped");
            ++unreadable;
            continue;
        }
        std::string digest;
        Sha256Bytes(buffer.data(), buffer.size(), &digest);
        Field("sha256", Json(digest));
        Field("head", Json(casefile::ToHex(buffer.data(), std::min<size_t>(32, buffer.size()))));

                                                                                    
                                                                                  
                                                                                
        bool kernel_says_same = false;
        {
            std::vector<u8> other(length);
            std::string ignored;
            const u64 other_pa = s.translator.WalkFrom(s.kernel_cr3, va);
            if (other_pa && s.translator.ReadVaAs(s.kernel_cr3, va, other.data(), other.size(), &ignored)) {
                kernel_says_same = std::memcmp(buffer.data(), other.data(), length) == 0;
                Field("kernel_root_pa", Json(Hex(other_pa)));
                Field("kernel_root_agrees", Logger::Bool(kernel_says_same));
                if (std::memcmp(buffer.data(), other.data(), length) != 0) {
                    size_t first_diff = 0;
                    while (first_diff < length && buffer[first_diff] == other[first_diff]) ++first_diff;
                    Field("kernel_root_first_difference", Json(Hex((u64)first_diff, 4)));
                }
            }
        }

        if (!opt.no_rpm) {
            std::vector<u8> rpm(length);
            SIZE_T got = 0;
            const bool rpm_ok = ReadProcessMemory(t.handle, (const void*)(uintptr_t)va, rpm.data(), length,
                                                  &got) != 0 && got == length;
            Field("rpm_ok", Logger::Bool(rpm_ok));
            if (rpm_ok) {
                size_t first_diff = length;
                for (size_t i = 0; i < length; ++i) {
                    if (buffer[i] != rpm[i]) {
                        first_diff = i;
                        break;
                    }
                }
                const bool same = first_diff == length;
                Field("matches_rpm", Logger::Bool(same));
                if (!same) {
                    Field("first_difference", Json(Hex((u64)first_diff, 4)));
                    Field("ours", Json(casefile::ToHex(buffer.data() + first_diff, 8)));
                    Field("rpm", Json(casefile::ToHex(rpm.data() + first_diff, 8)));
                }
                if (same) {
                    g_log.Event("read_verified");
                    ++verified;
                } else {
                    g_log.Event("read_mismatch");
                    ++contradicted;
                }
                continue;
            }
        }

        if (disk_ok && va >= t.image_base) {
            const u64 rva = va - t.image_base;
            std::vector<u8> expected(length);
            if (rva <= 0xFFFFFFFFull && disk.ReadRva((u32)rva, expected.data(), expected.size())) {
                const bool same = std::memcmp(buffer.data(), expected.data(), length) == 0;
                Field("matches_disk", Logger::Bool(same));
                Field("disk_expected", Json(casefile::ToHex(expected.data(), std::min<size_t>(32, length))));
                if (same) {
                    g_log.Event("read_verified_disk");
                    ++verified_disk;
                } else {
                                                                                     
                                                                            
                    g_log.Event("read_differs_from_disk");
                    ++differs_disk;
                }
                continue;
            }
            Field("disk_backing", Logger::Bool(false));
        }
        if (kernel_says_same) ++agreed_kernel_root;
                                                                                  
                                           
        g_log.Event("read_reported");
    }

    int rc = 0;
    if (unreadable || contradicted) rc = 22;
    if (!verified && !verified_disk) rc = 22;
    Field("target_pid", Logger::Num(pid));
    Field("verified", Logger::Num((u64)verified));
    Field("verified_disk", Logger::Num((u64)verified_disk));
    Field("differs_from_disk", Logger::Num((u64)differs_disk));
    Field("agreed_kernel_root", Logger::Num((u64)agreed_kernel_root));
    Field("contradicted", Logger::Num((u64)contradicted));
    Field("unreadable", Logger::Num((u64)unreadable));
    Field("result", Json(rc == 0 ? "success" : "incomplete"));
    g_log.Event("read_summary");
    return rc;
}

bool EspNeedsMemory(const Options& opt) {
    if (opt.esp_live || opt.esp_probe) return true;
    return !opt.esp_actors.empty() || !opt.esp_vehicles.empty();
}

struct WindowProbe {
    u32 pid = 0;
    HWND best = nullptr;
    long area = 0;
};

BOOL CALLBACK WindowProbeProc(HWND hwnd, LPARAM param) {
    auto* probe = reinterpret_cast<WindowProbe*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != probe->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return TRUE;
    const long width = client.right - client.left;
    const long height = client.bottom - client.top;
    if (width < 64 || height < 64) return TRUE;
    const long area = width * height;
    if (area > probe->area) {
        probe->area = area;
        probe->best = hwnd;
    }
    return TRUE;
}

HWND FindLargestWindow(u32 pid) {
    WindowProbe probe;
    probe.pid = pid;
    EnumWindows(WindowProbeProc, reinterpret_cast<LPARAM>(&probe));
    return probe.best;
}

bool ResolveEspTarget(const Options& opt, Session& s, u32* pid_out, LiveTarget* target, std::string* error) {
    u32 pid = opt.pid;
    if (!pid && !opt.target_process.empty()) pid = FindProcessByName(opt.target_process);
    if (!pid) {
        *error = "no target process (pass --pid or --process)";
        return false;
    }
    *pid_out = pid;

    std::wstring image_path;
    std::string leaf;
    if (target::PathOf(pid, &image_path, &leaf)) {
        target->image_path = image_path;
        target->leaf = leaf;
    }
    target::DiskImage disk;
    const bool disk_ok = !image_path.empty() && disk.Open(image_path, error);
    if (!disk_ok) error->clear();

    bool resolved = false;
    if (opt.read_base) {
        resolved = target::ResolveWithBase(pid, opt.read_base, target, error);
    } else {
        resolved = target::FromKernel(s.translator, s.nt ? s.nt->base : 0, s.disk_nt, pid, leaf,
                                      disk_ok ? disk.timestamp : 0, target, error);
    }
    if (!resolved) {
        error->clear();
        if (disk_ok && target::FindImageByUserScan(s.translator, disk.timestamp, disk.size_of_image, target,
                                                   error)) {
            resolved = true;
        } else {
            error->clear();
            resolved = target::Resolve(pid, target, error);
        }
    }
    if (!resolved) {
        if (error->empty()) *error = "target image base was not resolved";
        return false;
    }
    if (target->directory_table_base) {
        s.translator.AdoptAddressSpace(target->directory_table_base);
    } else if (!s.translator.DiscoverForImage(target->image_base, target->timestamp, target->size_of_image,
                                             error)) {
        return false;
    }
    return true;
}

std::vector<EspTarget> DemoTargets(const EspOffsets& offsets) {
    struct Spot {
        float x;
        float y;
        float height;
        int kind;
    };
    const Spot spots[] = {
        {600.0f, -300.0f, 0.0f, 0},
        {900.0f, 0.0f, 0.0f, 0},
        {1400.0f, 350.0f, 0.0f, 0},
        {2000.0f, -500.0f, 0.0f, 0},
        {2600.0f, 120.0f, 0.0f, 0},
        {1100.0f, 700.0f, 0.0f, 1},
    };
    std::vector<EspTarget> targets;
    for (const auto& spot : spots) {
        EspTarget entry;
        entry.feet = Vec3{spot.x, spot.y, 0.0f};
        entry.kind = spot.kind ? TargetKind::kVehicle : TargetKind::kCharacter;
        entry.height_cm = entry.kind == TargetKind::kVehicle ? offsets.vehicle_height_cm
                                                            : offsets.character_height_cm;
        targets.push_back(entry);
    }
    return targets;
}

std::string FormatFloats(const float* values, size_t count) {
    std::string text = "[";
    char buffer[48];
    for (size_t i = 0; i < count; ++i) {
        if (i) text += ",";
        std::snprintf(buffer, sizeof(buffer), "%.4f", values[i]);
        text += buffer;
    }
    text += "]";
    return text;
}

bool WriteEspReport(const std::wstring& path, const Camera& camera, const std::vector<EspTarget>& targets,
                    const EspSettings& settings, const SceneStats& stats, int frames, int frames_with_quads,
                    int quads, int viewport_width, int viewport_height, const std::string& source,
                    std::string* error) {
    std::string text;
    text.reserve(4096);
    char buffer[256];
    text += "{\"source\":" + Json(source);
    std::snprintf(buffer, sizeof(buffer), ",\"frames\":%d,\"frames_with_quads\":%d,\"quads\":%d", frames,
                  frames_with_quads, quads);
    text += buffer;
    std::snprintf(buffer, sizeof(buffer), ",\"viewport\":[%d,%d]", viewport_width, viewport_height);
    text += buffer;
    const float location[3] = {camera.location.x, camera.location.y, camera.location.z};
    text += ",\"camera\":{\"location\":" + FormatFloats(location, 3);
    std::snprintf(buffer, sizeof(buffer), ",\"pitch\":%.4f,\"yaw\":%.4f,\"roll\":%.4f,\"fov\":%.4f", camera.pitch,
                  camera.yaw, camera.roll, camera.fov);
    text += buffer + std::string("}");
    std::snprintf(buffer, sizeof(buffer), ",\"settings\":{\"scale\":%.4f,\"width_ratio\":%.4f,\"thickness\":%.4f}",
                  settings.scale, settings.width_ratio, settings.thickness);
    text += buffer;
    std::snprintf(buffer, sizeof(buffer),
                  ",\"stats\":{\"targets\":%zu,\"drawn\":%zu,\"skipped\":%zu,\"outside\":%zu,\"too_far\":%zu,"
                  "\"limited\":%zu,\"nearest\":%.4f,\"farthest\":%.4f}",
                  stats.targets, stats.drawn, stats.skipped, stats.outside, stats.too_far, stats.limited,
                  stats.nearest, stats.farthest);
    text += buffer;

    text += ",\"boxes\":[";
    bool first = true;
    for (const auto& target : targets) {
        const float height_cm = target.height_cm > 1.0f ? target.height_cm : 180.0f;
        const Vec3 head{target.feet.x, target.feet.y, target.feet.z + height_cm};
        ProjectionStyle style;
        style.width_ratio = settings.width_ratio;
        style.min_height = settings.min_height;
        style.min_width = settings.min_width;
        style.scale = settings.scale;
        style.nominal_height_cm = height_cm;
        const ProjectionResult projection = ProjectBox(camera, head, target.feet, style);

        ProjectionStyle scaled = style;
        scaled.scale = settings.scale * 1.3f;
        const ProjectionResult projection_130 = ProjectBox(camera, head, target.feet, scaled);

        if (!first) text += ",";
        first = false;
        const float feet[3] = {target.feet.x, target.feet.y, target.feet.z};
        text += "{\"feet\":" + FormatFloats(feet, 3);
        std::snprintf(buffer, sizeof(buffer), ",\"height_cm\":%.4f,\"kind\":%d,\"status\":%d", height_cm,
                      target.kind == TargetKind::kVehicle ? 1 : 0, (int)projection.status);
        text += buffer;
        text += ",\"box\":" + FormatFloats(&projection.box.left, 4);
        std::snprintf(buffer, sizeof(buffer), ",\"box_width\":%.4f,\"box_height\":%.4f,\"distance\":%.4f",
                      projection.box.Width(), projection.box.Height(), projection.distance);
        text += buffer;
        std::snprintf(buffer, sizeof(buffer), ",\"box_width_130\":%.4f,\"box_height_130\":%.4f",
                      projection_130.box.Width(), projection_130.box.Height());
        text += buffer + std::string("}");
    }
    text += "]}";

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = "cannot write report: " + Narrow(path);
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr) != 0 &&
                    written == text.size();
    CloseHandle(file);
    if (!ok && error) *error = "short write on report: " + Narrow(path);
    return ok;
}

int RunEsp(const Options& opt, Session* session) {
    std::string error;

    EspOffsets offsets;
    if (!opt.esp_offsets_path.empty()) {
        Field("esp_profile", Json(Narrow(opt.esp_offsets_path)));
        if (LoadOffsets(opt.esp_offsets_path, &offsets, &error)) {
            Field("esp_profile_loaded", Logger::Bool(true));
            Field("esp_profile_camera", Logger::Bool(offsets.camera_ready()));
            Field("esp_profile_actors", Logger::Bool(offsets.actors_ready()));
        } else {
            Field("esp_profile_loaded", Logger::Bool(false));
            Field("esp_profile_error", Json(error));
            error.clear();
        }
        g_log.Event("esp_profile");
    }

    std::unique_ptr<LiveTarget> target;
    EspReadFn read;
    u32 target_pid = opt.pid;
    if (session) {
        target.reset(new LiveTarget());
        if (!ResolveEspTarget(opt, *session, &target_pid, target.get(), &error)) {
            Field("error", Json(error));
            g_log.Event("esp_target_failed");
            return 32;
        }
        Field("esp_target_pid", Logger::Num(target_pid));
        Field("esp_target_base", Json(Hex(target->image_base)));
        Field("esp_target_cr3", Json(Hex(session->translator.cr3())));
        Field("esp_target_via", Json(target->resolved_via));
        g_log.Event("esp_target_ready");
        Translator* translator = &session->translator;
        read = [translator](u64 va, void* dst, size_t n, std::string* read_error) {
            return translator->ReadVa(va, dst, n, read_error);
        };
    }

    if (opt.esp_probe) {
        const std::string report = ProbeActor(read, opt.esp_probe_va, offsets);
        std::string flat;
        for (char ch : report) {
            if (ch == '\n') {
                flat += " | ";
            } else if (ch != '\r') {
                flat.push_back(ch);
            }
        }
        Field("esp_probe", Json(flat));
        g_log.Event("esp_probe_report");
        return read ? 0 : 33;
    }

    D3D12Overlay overlay;
    if (!overlay.Create(opt.esp_vsync, &error)) {
        Field("esp_overlay_error", Json(error));
        g_log.Event("esp_overlay_failed");
        return 30;
    }
    Field("esp_overlay_device", Json(overlay.adapter_name()));
    Field("esp_overlay_path", Json(overlay.present_path()));
    Field("esp_overlay_shader", Json(overlay.shader_note()));
    g_log.Event("esp_overlay_ready");

    HWND game = target_pid ? FindLargestWindow(target_pid) : nullptr;
    if (!overlay.Attach(game ? game : GetDesktopWindow(), &error)) {
        Field("esp_attach_error", Json(error));
        g_log.Event("esp_attach_failed");
        return 30;
    }
    if (!opt.esp_capture_path.empty()) overlay.SetCapturePath(opt.esp_capture_path);
    Field("esp_anchor", Json(game ? "game_window" : "desktop"));
    Field("esp_viewport_width", Logger::Num((u64)overlay.width()));
    Field("esp_viewport_height", Logger::Num((u64)overlay.height()));
    g_log.Event("esp_attached");

    EspSettings settings;
    settings.scale = opt.esp_scale;
    settings.thickness = opt.esp_thickness;
    settings.corners = opt.esp_corners;
    settings.max_distance = opt.esp_max_distance;
    settings.max_boxes = (size_t)opt.esp_max_boxes;

    Camera camera = opt.esp_camera;
    if (!opt.esp_camera_set) {
        camera.location = Vec3{0.0f, 0.0f, 100.0f};
        camera.pitch = 0.0f;
        camera.yaw = 0.0f;
        camera.roll = 0.0f;
        camera.fov = 90.0f;
    }

    EspPov pov;
    u64 pov_tick = 0;
    std::vector<u64> sweep;
    u64 sweep_tick = 0;
    std::string live_note;
    if (session && !offsets.camera_ready()) {
        live_note = "camera offsets are not calibrated in the profile";
    } else if (session && !offsets.actors_ready()) {
        live_note = "actor offsets are not calibrated in the profile";
    }

    LARGE_INTEGER frequency{}, start{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    DrawList list;
    SceneStats stats;
    std::vector<EspTarget> targets;
    int frames = 0;
    int frames_with_quads = 0;
    int rc = 0;
    bool f5_down = false;
    bool f6_down = false;
    bool f8_down = false;

    for (;;) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed = (double)(now.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
        if (opt.esp_seconds > 0.0 && elapsed >= opt.esp_seconds) break;
        if ((GetAsyncKeyState(VK_F9) & 0x8000) != 0) break;

        const bool f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f5 && !f5_down) {
            settings.scale -= 0.05f;
            if (settings.scale < 0.2f) settings.scale = 0.2f;
        }
        if (f6 && !f6_down) {
            settings.scale += 0.05f;
            if (settings.scale > 4.0f) settings.scale = 4.0f;
        }
        if (f8 && !f8_down) overlay.SetVisible(!overlay.visible());
        f5_down = f5;
        f6_down = f6;
        f8_down = f8;

        targets.clear();
        for (const auto& point : opt.esp_points) {
            EspTarget entry;
            entry.feet = point.feet;
            entry.height_cm = point.height_cm;
            entry.kind = point.kind ? TargetKind::kVehicle : TargetKind::kCharacter;
            targets.push_back(entry);
        }
        if (opt.esp_demo) {
            const std::vector<EspTarget> demo = DemoTargets(offsets);
            targets.insert(targets.end(), demo.begin(), demo.end());
        }

        if (session && target) {
            const u64 now_ms = (u64)(elapsed * 1000.0);
            if (!opt.esp_camera_set && offsets.camera_ready() && (pov_tick == 0 || now_ms - pov_tick > 100)) {
                EspPov fresh;
                std::string pov_error;
                if (ReadPov(read, target->image_base, offsets, &fresh, &pov_error)) {
                    pov = fresh;
                    pov_tick = now_ms;
                    live_note.clear();
                } else if (live_note.empty()) {
                    live_note = pov_error;
                }
            }
            if (pov.ok) {
                camera.location = pov.location;
                camera.pitch = pov.pitch;
                camera.yaw = pov.yaw;
                camera.roll = pov.roll;
                camera.fov = pov.fov;
            }
            for (u64 va : opt.esp_actors) {
                Vec3 origin{};
                std::string actor_error;
                if (ReadActorOrigin(read, offsets, va, &origin, &actor_error)) {
                    EspTarget entry;
                    entry.feet = origin;
                    entry.height_cm = offsets.character_height_cm;
                    entry.kind = TargetKind::kCharacter;
                    targets.push_back(entry);
                } else if (live_note.empty()) {
                    live_note = actor_error;
                }
            }
            for (u64 va : opt.esp_vehicles) {
                Vec3 origin{};
                std::string actor_error;
                if (ReadActorOrigin(read, offsets, va, &origin, &actor_error)) {
                    EspTarget entry;
                    entry.feet = origin;
                    entry.height_cm = offsets.vehicle_height_cm;
                    entry.kind = TargetKind::kVehicle;
                    targets.push_back(entry);
                } else if (live_note.empty()) {
                    live_note = actor_error;
                }
            }
            if (opt.esp_live && offsets.actors_ready() && offsets.world_slot) {
                if (sweep_tick == 0 || now_ms - sweep_tick > 500) {
                    std::string sweep_error;
                    sweep = ReadActorList(read, target->image_base, offsets, (size_t)opt.esp_max_boxes,
                                          &sweep_error);
                    sweep_tick = now_ms == 0 ? 1 : now_ms;
                    if (sweep.empty() && live_note.empty()) live_note = sweep_error;
                }
                for (u64 va : sweep) {
                    Vec3 origin{};
                    std::string actor_error;
                    if (ReadActorOrigin(read, offsets, va, &origin, &actor_error)) {
                        EspTarget entry;
                        entry.feet = origin;
                        entry.height_cm = offsets.character_height_cm;
                        entry.kind = TargetKind::kCharacter;
                        targets.push_back(entry);
                    } else if (live_note.empty()) {
                        live_note = actor_error;
                    }
                }
            }
        }

        camera.width = (float)overlay.width();
        camera.height = (float)overlay.height();
        list.Clear();
        BuildScene(camera, targets, settings, &list, &stats);
        if (!overlay.Render(list, &error)) {
            Field("esp_render_error", Json(error));
            g_log.Event("esp_render_failed");
            rc = 31;
            break;
        }
        ++frames;
        if (!list.quads.empty()) ++frames_with_quads;
        if (opt.esp_interval_ms > 0) Sleep((DWORD)opt.esp_interval_ms);
    }

    const int quads = (int)list.quads.size();
    Field("esp_frames", Logger::Num((u64)frames));
    Field("esp_frames_with_quads", Logger::Num((u64)frames_with_quads));
    Field("esp_quads", Logger::Num((u64)quads));
    Field("esp_drawn", Logger::Num((u64)stats.drawn));
    Field("esp_skipped", Logger::Num((u64)stats.skipped));
    Field("esp_outside", Logger::Num((u64)stats.outside));
    Field("esp_targets", Logger::Num((u64)stats.targets));
    Field("esp_scale_final", Json([](float v) {
              char buffer[32];
              std::snprintf(buffer, sizeof(buffer), "%.3f", v);
              return std::string(buffer);
          }(settings.scale)));
    Field("esp_capture_saved", Logger::Bool(overlay.capture_saved()));
    if (!overlay.capture_note().empty()) Field("esp_capture_note", Json(overlay.capture_note()));
    if (!live_note.empty()) Field("esp_note", Json(live_note));
    g_log.Event("esp_summary");

    if (rc == 0 && (frames == 0 || frames_with_quads == 0)) rc = 31;

    if (!opt.esp_report_path.empty()) {
        std::string report_error;
        const std::string source = opt.esp_demo ? "demo" : (session ? "live" : "world");
        if (WriteEspReport(opt.esp_report_path, camera, targets, settings, stats, frames, frames_with_quads,
                           quads, overlay.width(), overlay.height(), source, &report_error)) {
            Field("esp_report", Json(Narrow(opt.esp_report_path)));
        } else {
            Field("esp_report_error", Json(report_error));
            if (rc == 0) rc = 31;
        }
        g_log.Event("esp_report_written");
    }
    overlay.Shutdown();
    return rc;
}

}              

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Options opt;
    if (!ParseArgs(argc, argv, &opt)) {
        Usage();
        return 2;
    }
    g_log.Open(opt.log_path);
    if (!g_api.Init()) {
        g_log.Event("fatal", "\"reason\":\"ntdll exports\"");
        return 2;
    }
    bool elevated = IsElevated();
    g_log.Event("start", "\"mode\":" + Json(Narrow(opt.mode)) + ",\"elevated\":" + Logger::Bool(elevated) +
                             ",\"os_build\":" + Json(std::to_string(OsBuild())));
    if (opt.mode == L"selftest") {
        int rc = RunSelfTest(g_log);
        g_log.Event("end", "\"exit\":" + Logger::Num((u64)rc));
        return rc;
    }
    if (opt.mode == L"esp" && !EspNeedsMemory(opt)) {
        int rc = RunEsp(opt, nullptr);
        g_log.Event("end", "\"exit\":" + Logger::Num((u64)rc));
        return rc;
    }

                                                                                 
                                                                                
                                                                               
    CaseFile case_data;
    FixtureProcess fixture;
    u32 case_pid = 0;
    std::string case_pid_source;
    if (opt.mode == L"case") {
        int case_rc = ResolveCaseTarget(opt, &case_data, &fixture, &case_pid, &case_pid_source);
        if (case_rc != 0) {
            g_log.Event("end", "\"exit\":" + Logger::Num((u64)case_rc));
            return case_rc;
        }
        if (opt.check_only) {
            int check_rc = ValidateCaseOffline(case_data);
            g_log.Event("end", "\"exit\":" + Logger::Num((u64)check_rc));
            return check_rc;
        }
    }

    if (!elevated) {
        g_log.Event("fatal", "\"reason\":\"administrator token required (SeLoadDriverPrivilege + physical map)\"");
        return 2;
    }
    if (opt.mode == L"cleanup") {
        RecoverLeftovers();
        bool present = DriverService::DevicePresent(L"PGRHostControl");
        g_log.Event("cleanup_complete", "\"pgr_device_present\":" + Logger::Bool(present));
        g_log.Event("end", "\"exit\":" + Logger::Num(present ? 5u : 0u));
        return present ? 5 : 0;
    }
    if (GetFileAttributesW(opt.driver_path.c_str()) == INVALID_FILE_ATTRIBUTES) {        Field("driver_path", Json(Narrow(opt.driver_path)));
        g_log.Event("fatal", "\"reason\":\"follow-up driver image missing\"");
        return 2;
    }

    Session s;
    std::string error;
    if (!StartHelper(opt, s, &error)) {
        Field("error", Json(error));
        g_log.Event("helper_unavailable");
        s.helper_service.Unload();
        s.helper_service.Delete();
        return 3;
    }
    WorkingSetProbeReport();

                                                                            
                                                                            
    s.provider = MakeProvider(opt.provider_id, &s.pgr);
    if (!s.provider) {
        Field("provider", Json(opt.provider_id));
        g_log.Event("fatal", "\"reason\":\"unknown memory provider\"");
        s.helper_service.Unload();
        s.helper_service.Delete();
        return 2;
    }
    Field("provider", Json(s.provider->id()));
    g_log.Event("provider_ready");

    int rc = 0;
    if (opt.mode == L"probe") {
        if (!Prepare(opt, s, false, false, &error)) {
            Field("error", Json(error));
            g_log.Event("probe_incomplete");
            rc = 10;
        } else if (!PhysicalReadProof(s, &error)) {
            Field("error", Json(error));
            g_log.Event("probe_incomplete");
            rc = 4;
        } else {
            g_log.Event("probe_complete");
        }
    } else if (opt.mode == L"scan") {
        if (!Prepare(opt, s, true, false, &error)) {
            Field("error", Json(error));
            g_log.Event("scan_incomplete");
            rc = 10;
        } else if (!PhysicalReadProof(s, &error)) {
            Field("error", Json(error));
            g_log.Event("scan_incomplete");
            rc = 4;
        } else {
            g_log.Event("scan_complete");
        }
    } else if (opt.mode == L"read") {
        if (!Prepare(opt, s, false, true, &error)) {
            Field("error", Json(error));
            g_log.Event("read_prepare_failed");
            rc = 10;
        } else {
            rc = RunRead(opt, s);
        }
    } else if (opt.mode == L"esp") {
        if (!Prepare(opt, s, false, true, &error)) {
            Field("error", Json(error));
            g_log.Event("esp_prepare_failed");
            rc = 10;
        } else {
            rc = RunEsp(opt, &s);
        }
    } else if (opt.mode == L"case") {
        if (!Prepare(opt, s, true, true, &error)) {
            Field("error", Json(error));
            g_log.Event("case_prepare_failed");
            rc = 10;
        } else {
            rc = RunCase(opt, s, case_data, case_pid, case_pid_source);
        }
    } else {
        if (!Prepare(opt, s, !opt.has_patch_rva, false, &error)) {
            Field("error", Json(error));
            g_log.Event("abort", "\"phase\":\"prepare\"");
            rc = 10;
        } else if (!PhysicalReadProof(s, &error)) {
            Field("error", Json(error));
            g_log.Event("abort", "\"phase\":\"physical_selftest\"");
            rc = 4;
        } else {
            rc = RunFull(opt, s);
        }
    }

    s.pgr.Close();
    NTSTATUS unload = s.helper_service.Unload();
    bool removed = s.helper_service.Delete();
    bool device_gone = !DriverService::DevicePresent(L"PGRHostControl");
    g_log.Event("cleanup", "\"pgr_unload\":" + Json(StatusHex(unload)) + ",\"service_removed\":" +
                               Logger::Bool(removed) + ",\"device_present\":" +
                               Logger::Bool(!device_gone));
    g_log.Event("end", "\"exit\":" + Logger::Num((u64)rc));
    return rc;
}



