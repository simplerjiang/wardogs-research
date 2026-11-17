                                                                           
#pragma once

#include "common.h"

                                                                              
                                                                         
#ifndef WARDOG_UNICODE_STRING_DEFINED
#define WARDOG_UNICODE_STRING_DEFINED
typedef struct _WARDOG_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;
#endif

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* pfnNtLoadDriver)(PUNICODE_STRING);
typedef NTSTATUS(NTAPI* pfnNtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS(NTAPI* pfnRtlAdjustPrivilege)(ULONG, BOOLEAN, BOOLEAN, PBOOLEAN);

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#endif
#ifndef STATUS_INVALID_IMAGE_HASH
#define STATUS_INVALID_IMAGE_HASH ((NTSTATUS)0xC0000428L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_IMAGE_ALREADY_LOADED
#define STATUS_IMAGE_ALREADY_LOADED ((NTSTATUS)0xC000010EL)
#endif

struct NativeApi {
    pfnNtLoadDriver LoadDriver = nullptr;
    pfnNtLoadDriver UnloadDriver = nullptr;
    pfnNtQuerySystemInformation QuerySystemInformation = nullptr;
    pfnRtlAdjustPrivilege AdjustPrivilege = nullptr;

    bool Init() {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return false;
        LoadDriver = (pfnNtLoadDriver)GetProcAddress(ntdll, "NtLoadDriver");
        UnloadDriver = (pfnNtLoadDriver)GetProcAddress(ntdll, "NtUnloadDriver");
        QuerySystemInformation = (pfnNtQuerySystemInformation)GetProcAddress(ntdll, "NtQuerySystemInformation");
        AdjustPrivilege = (pfnRtlAdjustPrivilege)GetProcAddress(ntdll, "RtlAdjustPrivilege");
        return LoadDriver && UnloadDriver && QuerySystemInformation && AdjustPrivilege;
    }
};

                                                                              
                                                                
                                                                         
                                                                                
                                                               
                                                                              

struct KernelModule {
    u64 base = 0;
    u32 size = 0;
    std::string name;

    std::string leaf() const {
        size_t p = name.find_last_of("\\/");
        return p == std::string::npos ? name : name.substr(p + 1);
    }
};

#pragma pack(push, 1)
struct RtlProcessModuleInformation {
    u64 section;
    u64 mapped_base;
    u64 image_base;
    u32 image_size;
    u32 flags;
    u16 load_order_index;
    u16 init_order_index;
    u16 load_count;
    u16 module_name_offset;
    char image_name[256];
};
#pragma pack(pop)

inline bool QueryModules(NativeApi& api, std::vector<KernelModule>* out) {
    ULONG need = 0;
    api.QuerySystemInformation(11, nullptr, 0, &need);
    if (need == 0) need = 0x10000;
    std::vector<u8> buf(need + 0x10000);
    ULONG got = 0;
    NTSTATUS st = api.QuerySystemInformation(11, buf.data(), (ULONG)buf.size(), &got);
    if (st < 0) return false;
    u32 count = *(u32*)buf.data();
    out->clear();
    const u8* p = buf.data() + 8;
    for (u32 i = 0; i < count; ++i, p += sizeof(RtlProcessModuleInformation)) {
        const auto* m = (const RtlProcessModuleInformation*)p;
        KernelModule km;
        km.base = m->image_base;
        km.size = m->image_size;
        km.name = std::string(m->image_name, strnlen(m->image_name, sizeof(m->image_name)));
        out->push_back(km);
    }
    return true;
}

inline const KernelModule* FindModule(const std::vector<KernelModule>& mods, const char* leaf_lower) {
    for (const auto& m : mods) {
        std::string l = m.leaf();
        for (auto& c : l) c = (char)tolower((unsigned char)c);
        if (l == leaf_lower) return &m;
    }
    return nullptr;
}

                                                                              
                                                                         
inline bool QueryCiOptions(NativeApi& api, u32* options) {
    struct {
        ULONG length;
        ULONG options;
    } ci{ sizeof(ci), 0 };
    ULONG got = 0;
    NTSTATUS st = api.QuerySystemInformation(103, &ci, sizeof(ci), &got);
    if (st < 0) return false;
    *options = ci.options;
    return true;
}
