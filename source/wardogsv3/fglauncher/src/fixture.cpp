                                                                     
  
                                                                                
                                                                              
                                                                              
                                                                               
                     
  
                                                                             
                                                                            
                                                                        
  
                                                                               
                                               
#include <cstdio>
#include <string>
#include <vector>

#include "common.h"

namespace {

void WriteStatus(const std::wstring& path, const std::string& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    std::fputs(text.c_str(), f);
    std::fputs("\n", f);
    std::fclose(f);
}

int Usage() {
    std::printf(
        "WardogFixture -- materialise a case target image at its declared base\n\n"
        "Usage: WardogFixture.exe --image PATH --base 0x140000000 --status FILE [--seconds N]\n"
        "  --image PATH   PE image to map (the case target)\n"
        "  --base ADDR    required preferred image base (must match the PE header)\n"
        "  --status FILE  JSON status file written once the mapping is live\n"
        "  --seconds N    exit after N seconds (default 120, 0 = wait for the stop file)\n"
        "  --stop FILE    stop file; the process exits when it appears\n");
    return 2;
}

}              

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::wstring image, status, stop;
    u64 base = 0;
    int seconds = 120;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&](std::wstring* out) {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        std::wstring v;
        if (a == L"--image") {
            if (!next(&image)) return Usage();
        } else if (a == L"--status") {
            if (!next(&status)) return Usage();
        } else if (a == L"--stop") {
            if (!next(&stop)) return Usage();
        } else if (a == L"--base") {
            if (!next(&v)) return Usage();
            base = wcstoull(v.c_str(), nullptr, 0);
        } else if (a == L"--seconds") {
            if (!next(&v)) return Usage();
            seconds = (int)wcstol(v.c_str(), nullptr, 0);
        } else {
            return Usage();
        }
    }
    if (image.empty() || base == 0 || status.empty()) return Usage();

    PeFile pe;
    std::string error;
    if (!pe.Load(image)) {
        error = "cannot read the target image";
    } else if (pe.image_base != base) {
        error = "image preferred base is " + Hex(pe.image_base, 8) + ", case requires " + Hex(base);
    } else if (pe.size_of_image == 0) {
        error = "image declares SizeOfImage = 0";
    }
    if (!error.empty()) {
        WriteStatus(status, "{\"ok\":false,\"error\":" + Json(error) + "}");
        std::printf("fixture-error: %s\n", error.c_str());
        return 3;
    }

    void* view = VirtualAlloc((void*)(uintptr_t)base, pe.size_of_image, MEM_COMMIT | MEM_RESERVE,
                              PAGE_READWRITE);
    if (!view) {
        error = "VirtualAlloc at " + Hex(base) + " failed, win32=" + std::to_string(GetLastError());
        WriteStatus(status, "{\"ok\":false,\"error\":" + Json(error) + "}");
        std::printf("fixture-error: %s\n", error.c_str());
        return 4;
    }

    u8* dst = static_cast<u8*>(view);
    std::memset(dst, 0, pe.size_of_image);
                                                                           
                                                                             
                                                                               
                                                
    u32 headers = *(u32*)(pe.bytes.data() + *(u32*)(pe.bytes.data() + 0x3C) + 24 + 60);
    if (headers > pe.bytes.size()) headers = (u32)pe.bytes.size();
    if (headers > pe.size_of_image) headers = pe.size_of_image;
    std::memcpy(dst, pe.bytes.data(), headers);

    std::string copied;
    for (const auto& s : pe.sections) {
        if (!s.raw || !s.rawsize) continue;
        if ((u64)s.raw + s.rawsize > pe.bytes.size()) continue;
        u32 span = s.rawsize < s.vsize || s.vsize == 0 ? s.rawsize : s.vsize;
        if (s.va + span > pe.size_of_image) span = pe.size_of_image > s.va ? pe.size_of_image - s.va : 0;
        if (!span) continue;
        std::memcpy(dst + s.va, pe.bytes.data() + s.raw, span);
        copied += (copied.empty() ? "" : ",") + Json(s.name.empty() ? std::string("?") : s.name) + ":" +
                  Hex(span, 1);
    }

    std::string ready = std::string("{\"ok\":true,\"pid\":") + std::to_string(GetCurrentProcessId()) +
                        ",\"base\":" + Json(Hex(base)) + ",\"size\":" + Json(Hex(pe.size_of_image)) +
                        ",\"timestamp\":" + Json(Hex(pe.timestamp, 8)) + ",\"image\":" +
                        Json(Narrow(image)) + ",\"sections\":" + Json(copied) + "}";
    WriteStatus(status, ready);
    std::printf("FIXTURE_READY %s\n", ready.c_str());

    LARGE_INTEGER freq{}, start{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (;;) {
        if (!stop.empty() && GetFileAttributesW(stop.c_str()) != INVALID_FILE_ATTRIBUTES) break;
        if (seconds > 0) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            double elapsed = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
            if (elapsed >= (double)seconds) break;
        }
        Sleep(50);
    }

    WriteStatus(status, "{\"ok\":true,\"phase\":\"exit\",\"pid\":" + std::to_string(GetCurrentProcessId()) + "}");
    return 0;
}
