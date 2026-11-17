          
#include "pgr.h"

namespace {
constexpr DWORD kIoctlMap = 0x80102040;
constexpr DWORD kIoctlUnmap = 0x80102044;
constexpr DWORD kIoctlPortIn = 0x80102050;
constexpr DWORD kIoctlPortOut = 0x80102054;
constexpr DWORD kIoctlHandshake = 0x0022218C;

#pragma pack(push, 1)
struct MapRequest {
    u64 size;
    u64 phys;
    u64 handle;
    u64 base;
    u64 object;
};
struct UnmapRequest {
    u64 reserved0;
    u64 reserved1;
    u64 handle;
    u64 base;
    u64 object;
};
struct PortRequest {
    u16 port;
    u32 value;
    u8 width;
};
struct HandshakeRequest {
    u32 version;               
    u32 declared;                   
    u32 reserved;
};
#pragma pack(pop)

static_assert(sizeof(MapRequest) == 0x28, "map request must match the driver's 0x28 byte copy");
static_assert(sizeof(UnmapRequest) == 0x28, "unmap request must match the driver's 0x28 byte copy");
static_assert(sizeof(PortRequest) == 7, "port request layout");
static_assert(sizeof(HandshakeRequest) == 0xC, "handshake input must satisfy the driver's 0xC size test");

bool Ioctl(HANDLE h, DWORD code, void* in, DWORD in_len, void* out, DWORD out_len, DWORD* returned,
           std::string* error) {
    DWORD bytes = 0;
    if (!DeviceIoControl(h, code, in, in_len, out, out_len, &bytes, nullptr)) {
        DWORD err = GetLastError();
        char buf[96];
        std::snprintf(buf, sizeof(buf), "DeviceIoControl 0x%08lX failed, win32=%lu", code, err);
        *error = buf;
        return false;
    }
    if (returned) *returned = bytes;
    return true;
}
}              

bool PgrDevice::Open(std::string* error) {
    if (handle_ != INVALID_HANDLE_VALUE) return true;
    handle_ = CreateFileW(L"\\\\.\\PGRHostControl", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "CreateFile(\\\\.\\PGRHostControl) failed, win32=%lu", GetLastError());
        *error = buf;
        return false;
    }
    return true;
}

void PgrDevice::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

bool PgrDevice::Handshake(u32* declared_size, std::string* error) {
                                                                        
                                                                               
                                                                          
                                     
    HandshakeRequest req{1, 0x40, 0};
    u8 out[16]{};
    DWORD returned = 0;
    if (!Ioctl(handle_, kIoctlHandshake, &req, sizeof(req), out, sizeof(out), &returned, error)) return false;
    if (declared_size) *declared_size = returned >= 8 ? *(u32*)(out + 4) : 0;
    if (returned < 8) return true;                                    
    u32 version = *(u32*)out;
    u32 size = *(u32*)(out + 4);
    return version == 1 && size >= 0xC;
}

bool PgrDevice::Map(u64 phys, u64 size, Mapping* out, std::string* error) {
    if (!size) {
        *error = "zero-length map";
        return false;
    }
    u64 start = phys & ~0xFFFull;
    u64 end = (phys + size + 0xFFF) & ~0xFFFull;
    MapRequest req{};
    req.size = end - start;
    req.phys = start;
    DWORD returned = 0;
    if (!Ioctl(handle_, kIoctlMap, &req, sizeof(req), &req, sizeof(req), &returned, error)) return false;
    if (req.base == 0) {
        *error = "map returned null base";
        return false;
    }
                                                                               
                                                                             
                                                                              
                                                                                 
                                                       
    if (req.base >= 0x00007FFFFFFFFFFFull) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "map base 0x%llX for phys 0x%llX is not a user address",
                      (unsigned long long)req.base, (unsigned long long)start);
        *error = buf;
        return false;
    }
    const u64 base = req.base + (phys - start);
    if (base < req.base) {
        *error = "map base arithmetic overflowed";
        return false;
    }
                                                                                
                                                                                
                                                                              
                                                                                
                                                                
    u64 cursor = base & ~0xFFFull;
    const u64 last = base + size - 1;
    for (int guard = 0; guard < 512; ++guard) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery((const void*)(uintptr_t)cursor, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "map 0x%llX..0x%llX is not committed readable memory at 0x%llX",
                          (unsigned long long)base, (unsigned long long)last, (unsigned long long)cursor);
            *error = buf;
            return false;
        }
        u64 region_end = (u64)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (region_end > last) break;
        if (region_end <= cursor) {
            *error = "map region walk made no progress";
            return false;
        }
        cursor = region_end;
        if (guard == 511) {
            *error = "map span crosses more than 512 regions";
            return false;
        }
    }
    out->phys = start;
    out->size = end - start;
    out->handle = req.handle;
    out->object = req.object;
    out->view_base = req.base;
    out->base = base;
    return true;
}

bool PgrDevice::Unmap(Mapping* mapping) {
    if (!mapping->valid()) return true;
    UnmapRequest req{};
    req.handle = mapping->handle;
    req.base = mapping->view_base;
    req.object = mapping->object;
    std::string ignored;
    bool ok = Ioctl(handle_, kIoctlUnmap, &req, sizeof(req), nullptr, 0, nullptr, &ignored);
    mapping->base = 0;
    mapping->view_base = 0;
    mapping->size = 0;
    return ok;
}

bool PgrDevice::ReadPhys(u64 phys, void* dst, size_t n, std::string* error) {
    u8* out = static_cast<u8*>(dst);
    while (n) {
        u64 window = n < kMaxWindow ? n : kMaxWindow;
        Mapping m;
        if (!Map(phys, window, &m, error)) return false;
        std::memcpy(out, m.ptr(), (size_t)window);
        Unmap(&m);
        phys += window;
        out += window;
        n -= (size_t)window;
    }
    return true;
}

bool PgrDevice::WritePhys(u64 phys, const void* src, size_t n, std::string* error) {
    const u8* in = static_cast<const u8*>(src);
    while (n) {
        u64 window = n < kMaxWindow ? n : kMaxWindow;
        Mapping m;
        if (!Map(phys, window, &m, error)) return false;
        std::memcpy(m.ptr(), in, (size_t)window);
        Unmap(&m);
        phys += window;
        in += window;
        n -= (size_t)window;
    }
    return true;
}

bool PgrDevice::PortWrite(u16 port, u32 value, int width, std::string* error) {
    PortRequest req{port, value, (u8)(width == 1 ? 1 : width == 2 ? 2 : 3)};
    DWORD returned = 0;
    return Ioctl(handle_, kIoctlPortOut, &req, sizeof(req), nullptr, 0, &returned, error);
}

bool PgrDevice::PortRead(u16 port, int width, u32* value, std::string* error) {
    PortRequest req{port, 0, (u8)(width == 1 ? 1 : width == 2 ? 2 : 3)};
    PortRequest out{};
    DWORD returned = 0;
    if (!Ioctl(handle_, kIoctlPortIn, &req, sizeof(req), &out, sizeof(out), &returned, error)) return false;
    *value = out.value;
    return true;
}
