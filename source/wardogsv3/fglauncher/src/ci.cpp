         
#include "ci.h"
#include <algorithm>

namespace ci {

namespace {
constexpr u32 kMemWrite = 0x80000000;
constexpr u32 kMemExecute = 0x20000000;
constexpr u32 kMemRead = 0x40000000;

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    return s;
}
}              

bool ReadDword(Translator& t, u64 va, u32* value, std::string* error) {
    return t.ReadVa(va, value, sizeof(*value), error);
}

bool WriteDword(Translator& t, u64 va, u32 value, std::string* error) {
    return t.WriteVa(va, &value, sizeof(value), error);
}

bool ReadBytes(Translator& t, u64 va, void* dst, size_t n, std::string* error) {
    return t.ReadVa(va, dst, n, error);
}

bool WriteBytes(Translator& t, u64 va, const void* src, size_t n, std::string* error) {
    return t.WriteVa(va, src, n, error);
}

                                                                              
                                                                        
                                                                              
bool LocateNotifyTargets(const PeFile& disk_nt, NotifyTarget* out, std::string* error) {
    if (disk_nt.bytes.empty()) {
        *error = "disk image not loaded";
        return false;
    }
    out->ex_rva = disk_nt.ExportRva("PsSetLoadImageNotifyRoutineEx");
    out->set_rva = disk_nt.ExportRva("PsSetLoadImageNotifyRoutine");
    out->remove_rva = disk_nt.ExportRva("PsRemoveLoadImageNotifyRoutine");
    if (!out->set_rva && !out->ex_rva) {
        *error = "the load-image notify exports are missing from this ntoskrnl.exe";
        return false;
    }
                                                                            
                                                                                  
                                                                               
    struct Spot {
        u32 rva;
        const char* what;
    };
    const Spot spots[] = {{out->ex_rva, "PsSetLoadImageNotifyRoutineEx"},
                          {out->set_rva, "PsSetLoadImageNotifyRoutine"},
                          {out->remove_rva, "PsRemoveLoadImageNotifyRoutine"}};
    const u32 needle = 0x2DBA0FF0u;                
    for (const auto& spot : spots) {
        if (!spot.rva) continue;
        long off = disk_nt.OffsetOf(spot.rva);
        if (off < 0) continue;
        const u8* p = disk_nt.bytes.data() + off;
        size_t avail = disk_nt.bytes.size() - (size_t)off;
        size_t window = avail < 0x300 ? avail : 0x300;
        for (size_t i = 0; i + 9 <= window; ++i) {
            if (*(const u32*)(p + i) != needle) continue;
            int disp = *(const int*)(p + i + 4);
            u64 site_rva = (u64)spot.rva + i;
            long long target_rva = (long long)site_rva + 9 + disp;
            if (target_rva <= 0 || target_rva >= (long long)disk_nt.size_of_image) continue;
            out->mask_site_rva = (u32)site_rva;
            out->mask_rva = (u32)target_rva;
            out->mask_bit = p[i + 8] & 0x3F;
            out->note = std::string("lock bts [rip+disp32], imm8 inside ") + spot.what + "+0x" +
                        Hex((u64)i, 1);
            return true;
        }
    }
    *error = "no lock bts [rip+disp32] site found inside the notify routine bodies";
    return false;
}

bool ReadImage(Translator& t, u64 base, u32 size, CiImage* out, std::string* error) {    u8 header[0x1000];
    if (!t.ReadVa(base, header, sizeof(header), error)) return false;
    if (header[0] != 'M' || header[1] != 'Z') {
        *error = "ci.dll image does not start with MZ";
        return false;
    }
    u32 lfanew = *(u32*)(header + 0x3C);
    if (lfanew < 0x40 || lfanew + 0x100 > sizeof(header)) {
        *error = "unexpected e_lfanew in ci.dll";
        return false;
    }
    if (*(u32*)(header + lfanew) != 0x00004550) {
        *error = "ci.dll image has no PE signature";
        return false;
    }
    const u8* nt = header + lfanew;
    u16 nsec = *(u16*)(nt + 6);
    u16 opt_size = *(u16*)(nt + 20);
    out->timestamp = *(u32*)(nt + 8);
    const u8* sec = nt + 24 + opt_size;
    if (lfanew + 24 + opt_size + (size_t)nsec * 40 > sizeof(header)) {
        *error = "ci.dll section table not contained in the first page";
        return false;
    }
    out->base = base;
    out->size = size;
    out->sections.clear();
    for (u16 i = 0; i < nsec; ++i, sec += 40) {
        PeSection s{};
        s.name = Trim(std::string((const char*)sec, 8));
        s.vsize = *(u32*)(sec + 8);
        s.va = *(u32*)(sec + 12);
        s.rawsize = *(u32*)(sec + 16);
        s.characteristics = *(u32*)(sec + 36);
        out->sections.push_back(s);
    }
    return true;
}

bool FindValueCandidates(Translator& t, const CiImage& image, u32 value, std::vector<CiCandidate>* out,
                         std::string* error) {
    out->clear();
                                                                         
    for (const auto& s : image.sections) {
        if (!(s.characteristics & kMemWrite)) continue;
        u32 span = (s.vsize ? s.vsize : s.rawsize);
        if (!span) continue;
        span = std::min<u32>(span, 0x40000);
        std::vector<u8> data(span);
        if (!t.ReadVa(image.base + s.va, data.data(), data.size(), error)) return false;
        for (u32 i = 0; i + 4 <= span; i += 1) {
            u32 v = *(u32*)(data.data() + i);
            if (v != value) continue;
            CiCandidate c;
            c.rva = s.va + i;
            c.va = image.base + c.rva;
            c.value = v;
            c.xrefs = 0;
            c.section = s.name;
            out->push_back(c);
        }
    }
    if (out->empty()) {
        *error = "no writable DWORD in ci.dll equals the reported CI options";
        return false;
    }
                                                                  
    for (const auto& s : image.sections) {
        if (!(s.characteristics & kMemExecute)) continue;
        u32 span = (s.vsize ? s.vsize : s.rawsize);
        if (!span) continue;
        span = std::min<u32>(span, 0x800000);
        std::vector<u8> code(span);
        if (!t.ReadVa(image.base + s.va, code.data(), code.size(), error)) return false;
        for (u32 i = 0; i + 6 < span; ++i) {
            u8 op = code[i];
            u8 modrm = code[i + 1];
                                                                         
            if (op != 0x8B && op != 0x89 && op != 0x85 && op != 0x39 && op != 0x3B && op != 0xF7 &&
                op != 0x0B && op != 0x23 && op != 0x8D)
                continue;
            if ((modrm & 0xC7) != 0x05) continue;
            std::int32_t disp = *(std::int32_t*)(code.data() + i + 2);
            u64 target = image.base + s.va + i + 6 + (std::int64_t)disp;
            for (auto& c : *out) {
                if (c.va == target) c.xrefs++;
            }
        }
    }
    std::sort(out->begin(), out->end(), [](const CiCandidate& a, const CiCandidate& b) {
        if (a.xrefs != b.xrefs) return a.xrefs > b.xrefs;
        return a.rva < b.rva;
    });
    return true;
}

}                 

