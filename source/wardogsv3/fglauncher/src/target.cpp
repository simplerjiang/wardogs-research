             
#include "target.h"

#include <psapi.h>

#include <algorithm>

namespace {

typedef LONG(NTAPI* NtQueryInformationProcessFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct ProcessBasicInformation {
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
};

constexpr ULONG kProcessBasicInformation = 0;

std::vector<std::string> g_trace;

void Step(const std::string& line) {
    if (g_trace.size() < 64) g_trace.push_back(line);
}

std::wstring LeafOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool ReadHeaderBytes(HANDLE process, u64 base, u8* buffer, size_t size) {
    SIZE_T got = 0;
    return ::ReadProcessMemory(process, (const void*)(uintptr_t)base, buffer, size, &got) != 0 && got == size;
}

bool DescribeHeader(const u8* header, u32* timestamp, u32* size_of_image, u32* entry_rva) {
    if (header[0] != 'M' || header[1] != 'Z') return false;
    u32 lfanew = *(const u32*)(header + 0x3C);
    if (lfanew < 0x40 || lfanew > 0x400) return false;
    if (*(const u32*)(header + lfanew) != 0x00004550) return false;
    u16 magic = *(const u16*)(header + lfanew + 24);
    if (magic != 0x20B && magic != 0x10B) return false;
    *timestamp = *(const u32*)(header + lfanew + 8);
    *size_of_image = *(const u32*)(header + lfanew + 24 + 56);
    *entry_rva = *(const u32*)(header + lfanew + 24 + 16);
    return *size_of_image != 0;
}

                                         
bool BaseFromModuleList(HANDLE process, u64* base, u32* size) {
    HMODULE modules[8]{};
    DWORD needed = 0;
    if (!K32EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_ALL) || !needed) {
        return false;
    }
    MODULEINFO info{};
    if (!K32GetModuleInformation(process, modules[0], &info, sizeof(info)) || !info.lpBaseOfDll) return false;
    *base = (u64)(uintptr_t)info.lpBaseOfDll;
    *size = info.SizeOfImage;
    return true;
}

                                                            
bool BaseFromPeb(HANDLE process, u64* base) {
    static auto query = (NtQueryInformationProcessFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                                   "NtQueryInformationProcess");
    if (!query) return false;
    ProcessBasicInformation pbi{};
    ULONG returned = 0;
    if (query(process, kProcessBasicInformation, &pbi, sizeof(pbi), &returned) < 0) return false;
    if (!pbi.PebBaseAddress) return false;
    u64 image_base = 0;
    SIZE_T got = 0;
                                                 
    if (!::ReadProcessMemory(process, (const u8*)pbi.PebBaseAddress + 0x10, &image_base, sizeof(image_base),
                             &got) ||
        got != sizeof(image_base)) {
        return false;
    }
    *base = image_base;
    return image_base != 0;
}

                                                                              
                                                                                
                                                                          
bool BaseFromMappedName(HANDLE process, const std::wstring& wanted_leaf, u64* base, u32* size) {
    if (wanted_leaf.empty()) return false;
    u64 address = 0x10000;
    constexpr u64 kUserTop = 0x7FFFFFFF0000ull;
    MEMORY_BASIC_INFORMATION mbi{};
    for (int guard = 0; guard < 400000 && address < kUserTop; ++guard) {
        if (!VirtualQueryEx(process, (const void*)(uintptr_t)address, &mbi, sizeof(mbi))) break;
        u64 region_base = (u64)(uintptr_t)mbi.BaseAddress;
        u64 region_end = region_base + mbi.RegionSize;
        if (mbi.Type == MEM_IMAGE && (u64)(uintptr_t)mbi.AllocationBase == region_base) {
            wchar_t name[MAX_PATH]{};
            DWORD chars = K32GetMappedFileNameW(process, (LPVOID)(uintptr_t)region_base, name, MAX_PATH);
            if (chars) {
                std::wstring mapped(name, chars);
                if (_wcsicmp(LeafOf(mapped).c_str(), wanted_leaf.c_str()) == 0) {
                    u64 total = 0;
                    u64 cursor = region_base;
                    MEMORY_BASIC_INFORMATION inner{};
                    for (int step = 0; step < 8192; ++step) {
                        if (!VirtualQueryEx(process, (const void*)(uintptr_t)cursor, &inner, sizeof(inner))) break;
                        if ((u64)(uintptr_t)inner.AllocationBase != region_base) break;
                        if (!inner.RegionSize) break;
                        total += inner.RegionSize;
                        cursor += inner.RegionSize;
                    }
                    *base = region_base;
                    if (size) *size = (u32)total;
                    return true;
                }
            }
        }
        address = region_end > address ? region_end : address + 0x1000;
    }
    return false;
}

                                                                                  
                                                                                 
                                                                               
                                                            
bool BasesFromMappedName(HANDLE process, const std::wstring& wanted_leaf, std::vector<u64>* bases,
                         std::vector<u32>* sizes) {
    if (wanted_leaf.empty()) return false;
    u64 address = 0x10000;
    constexpr u64 kUserTop = 0x7FFFFFFF0000ull;
    MEMORY_BASIC_INFORMATION mbi{};
    for (int guard = 0; guard < 400000 && address < kUserTop && bases->size() < 16; ++guard) {
        if (!VirtualQueryEx(process, (const void*)(uintptr_t)address, &mbi, sizeof(mbi))) break;
        u64 region_base = (u64)(uintptr_t)mbi.BaseAddress;
        u64 region_end = region_base + mbi.RegionSize;
        if (mbi.Type == MEM_IMAGE && (u64)(uintptr_t)mbi.AllocationBase == region_base) {
            wchar_t name[MAX_PATH]{};
            DWORD chars = K32GetMappedFileNameW(process, (LPVOID)(uintptr_t)region_base, name, MAX_PATH);
            if (chars) {
                std::wstring mapped(name, chars);
                if (_wcsicmp(LeafOf(mapped).c_str(), wanted_leaf.c_str()) == 0) {
                    u64 total = 0;
                    u64 cursor = region_base;
                    MEMORY_BASIC_INFORMATION inner{};
                    for (int step = 0; step < 8192; ++step) {
                        if (!VirtualQueryEx(process, (const void*)(uintptr_t)cursor, &inner, sizeof(inner))) break;
                        if ((u64)(uintptr_t)inner.AllocationBase != region_base) break;
                        if (!inner.RegionSize) break;
                        total += inner.RegionSize;
                        cursor += inner.RegionSize;
                    }
                    bases->push_back(region_base);
                    sizes->push_back((u32)total);
                }
            }
        }
        address = region_end > address ? region_end : address + 0x1000;
    }
    return !bases->empty();
}

                                                                              
                                                                                 
                                                    
bool Finish(LiveTarget* out, u64 base, u32 size_from_module, std::string* error) {
    u8 header[0x400]{};
    u32 timestamp = 0, size_of_image = 0, entry_rva = 0;

    if (ReadHeaderBytes(out->handle, base, header, sizeof(header)) &&
        DescribeHeader(header, &timestamp, &size_of_image, &entry_rva)) {
        out->image_base = base;
        out->timestamp = timestamp;
        out->size_of_image = size_of_image;
        out->entry_rva = entry_rva;
        out->header_verified = true;
        if (size_from_module && size_from_module != size_of_image) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "module size 0x%X disagrees with SizeOfImage 0x%X", size_from_module,
                          size_of_image);
            *error = buf;
            return false;
        }
        return true;
    }

    if (out->image_path.empty()) {
        *error = "cannot read the live header and have no image path for a disk fallback";
        return false;
    }
                                                                              
                                                                        
    {
        HANDLE file = CreateFileW(out->image_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            *error = "cannot read the live header nor open the image on disk";
            return false;
        }
        u8 disk_header[0x400]{};
        DWORD got = 0;
        bool ok = ReadFile(file, disk_header, sizeof(disk_header), &got, nullptr) && got == sizeof(disk_header);
        CloseHandle(file);
        if (!ok || !DescribeHeader(disk_header, &timestamp, &size_of_image, &entry_rva)) {
            *error = "cannot read the live header nor the header of the image on disk";
            return false;
        }
    }
    out->image_base = base;
    out->timestamp = timestamp;
    out->size_of_image = size_of_image;
    out->entry_rva = entry_rva;
    out->header_verified = false;
    return true;
}

}              

namespace {

u64 PhysMaskOf(u64 e) { return e & 0x000FFFFFFFFFF000ull; }

bool IsKernelPointer(u64 v) { return v >= 0xFFFF000000000000ull && v < 0xFFFFFFFFFFFF0000ull; }

                                                                                 
                                                                    
                                                                      
bool BlockHasImageName(const u8* block, size_t size, const std::string& leaf) {
    if (leaf.size() < 5) return false;
    const size_t probe = leaf.size() < 12 ? leaf.size() : 12;
    for (size_t i = 0; i + probe <= size; ++i) {
        size_t k = 0;
        for (; k < probe; ++k) {
            u8 c = block[i + k];
            if (c >= 'a' && c <= 'z') c = (u8)(c - 'a' + 'A');
            u8 w = (u8)leaf[k];
            if (w >= 'a' && w <= 'z') w = (u8)(w - 'a' + 'A');
            if (c != w) break;
        }
        if (k == probe) return true;
    }
    return false;
}

bool BlockHasPid(const u8* block, size_t size, u32 pid) {
    if (!pid) return false;
    for (size_t i = 0; i + 4 <= size; i += 8) {
        if (*(const u32*)(block + i) == pid) return true;
    }
    return false;
}

                                                                               
                                                                          
std::string GuessImageName(const u8* block, size_t size) {
    std::string best;
    size_t i = 0;
    const size_t limit = size < 0x800 ? size : 0x800;
    while (i < limit) {
        size_t j = i, n = 0;
        while (j < limit) {
            u8 c = block[j];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                      c == '_' || c == '-';
            if (!ok) break;
            ++j;
            ++n;
        }
        if (n >= 4 && (j == limit || block[j] == 0)) {
            if (n > best.size()) best.assign((const char*)block + i, n);
        }
        i = (j > i) ? j + 1 : i + 1;
    }
    return best.empty() ? std::string("-") : best;
}

                                                                                
std::string NameFields(const u8* block, size_t size) {
    std::string out;
    const size_t limit = size < 0x800 ? size : 0x800;
    for (size_t i = 0; i + 4 <= limit; ++i) {
        size_t n = 0;
        while (i + n < limit && n < 16) {
            u8 c = block[i + n];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                      c == '_' || c == '-';
            if (!ok) break;
            ++n;
        }
        if (n >= 4 && i + n < limit && block[i + n] == 0) {
            if (!out.empty()) out += "|";
            out += Hex((u64)i) + ":";
            out.append((const char*)block + i, n);
            if (out.size() > 160) break;
        }
    }
    return out.empty() ? std::string("-") : out;
}

}              

namespace target {

void TraceReset() { g_trace.clear(); }
const std::vector<std::string>& Trace() { return g_trace; }

namespace {
bool KernelRouteOnce(Translator& translator, u64 nt_base, const PeFile& disk_nt, u32 pid,
                     const std::string& image_leaf, u32 expect_timestamp, LiveTarget* out,
                     std::string* error);
}

bool FromKernel(Translator& translator, u64 nt_base, const PeFile& disk_nt, u32 pid,
                const std::string& image_leaf, u32 expect_timestamp, LiveTarget* out,
                std::string* error) {
    TraceReset();
    Step("start nt_base=" + Hex(nt_base) + " pid=" + std::to_string(pid) + " leaf=" + image_leaf);
    if (!translator.ready()) {
        *error = "translator is not ready";
        return false;
    }
    if (!nt_base) {
        *error = "the kernel base is unknown";
        return false;
    }
                                                                               
                                                                                 
                                                                                  
                                                                               
                                                                     
    std::vector<u64> roots;
    if (translator.cr3()) roots.push_back(translator.cr3());
    for (u64 root : translator.candidate_table_roots()) {
        if (root != translator.cr3()) roots.push_back(root);
    }
    if (roots.empty()) {
        *error = "no kernel page table root is available";
        return false;
    }
    std::string last_error = "no kernel root completed the route";
    int tried = 0;
    for (u64 root : roots) {
        if (tried >= 1024) break;
        ++tried;
        if (!translator.AdoptAddressSpace(root)) continue;
        std::string local;
        Step("attempt=" + std::to_string(tried) + " root=" + Hex(root));
        if (KernelRouteOnce(translator, nt_base, disk_nt, pid, image_leaf, expect_timestamp, out, &local)) {
            out->kernel_root = root;
            Step("kernel_root=" + Hex(root) + " attempt=" + std::to_string(tried));
            return true;
        }
        last_error = local;
    }
    *error = last_error;
    return false;
}

namespace {

                                                                         
bool KernelRouteOnce(Translator& translator, u64 nt_base, const PeFile& disk_nt, u32 pid,
                     const std::string& image_leaf, u32 expect_timestamp, LiveTarget* out,
                     std::string* error) {
    const u32 system_rva = disk_nt.ExportRva("PsInitialSystemProcess");
    if (!system_rva) {
        *error = "PsInitialSystemProcess is not an export of the on-disk ntoskrnl";
        return false;
    }
    Step("export PsInitialSystemProcess rva=" + Hex(system_rva));
    u8 pointer[8]{};
    if (!translator.ReadVa(nt_base + system_rva, pointer, sizeof(pointer), error)) {
        *error = "cannot read PsInitialSystemProcess: " + *error;
        return false;
    }
    const u64 system_eprocess = *(u64*)pointer;
    Step("system_eprocess=" + Hex(system_eprocess));
    if (!IsKernelPointer(system_eprocess)) {
        *error = "PsInitialSystemProcess did not hold a kernel pointer";
        return false;
    }

                                                                               
                                                                               
                                                                                 
                                                                               
    constexpr size_t kBlock = 0x800;
    std::vector<u8> block(kBlock);
    if (!translator.ReadVa(system_eprocess, block.data(), block.size(), error)) {
        *error = "cannot read the System EPROCESS: " + *error;
        return false;
    }
    Step("sysfields=" + NameFields(block.data(), block.size()));
    std::vector<size_t> offsets;
    for (size_t off = 0; off + 16 <= kBlock && offsets.size() < 64; off += 8) {
        u64 flink = *(u64*)(block.data() + off);
        if (!IsKernelPointer(flink)) continue;
        u64 blink = *(u64*)(block.data() + off + 8);
        if (!IsKernelPointer(blink)) continue;
        u8 back[8]{};
        std::string ignored;
        if (!translator.ReadVa(flink + 8, back, sizeof(back), &ignored)) continue;
        if (*(u64*)back != system_eprocess + off) continue;
        offsets.push_back(off);
    }
    if (offsets.empty()) {
        *error = "ActiveProcessLinks could not be identified in the System EPROCESS";
        return false;
    }

    u64 found = 0;
    size_t links_offset = 0;
    int steps = 0;
    int budget = 40000;                                                             
    int reported = 0;
    for (size_t off : offsets) {
        const u64 head = system_eprocess + off;
        u64 entry = *(u64*)(block.data() + off);
        int walked = 0;
        bool first_read_ok = false;
        std::string stop = "head";
        std::string names;
        for (int step = 0; step < 2048 && IsKernelPointer(entry) && entry != head && budget > 0;
             ++step, ++walked, --budget) {
            const u64 eprocess = entry - off;
            std::vector<u8> candidate(kBlock);
            std::string ignored;
            if (translator.ReadVa(eprocess, candidate.data(), candidate.size(), &ignored)) {
                first_read_ok = first_read_ok || walked == 0;
                if (walked < 3) {
                    if (!names.empty()) names += ",";
                    names += GuessImageName(candidate.data(), candidate.size());
                    if (walked == 0) {
                        names += " fields{" + NameFields(candidate.data(), candidate.size()) + "}";
                        names += " walk_block=" + Hex(translator.WalkFrom(translator.cr3(), eprocess));
                        names += " walk_next=" + Hex(translator.WalkFrom(translator.cr3(), eprocess + off));
                    }
                }
                if (BlockHasImageName(candidate.data(), candidate.size(), image_leaf) &&
                    BlockHasPid(candidate.data(), candidate.size(), pid)) {
                    block = candidate;
                    found = eprocess;
                    break;
                }
            } else {
                stop = "read_eprocess";
            }
            u8 next[8]{};
            std::string next_error;
            if (!translator.ReadVa(entry, next, sizeof(next), &next_error)) {
                stop = "read_next@" + Hex(entry) + ":" + next_error + "/" + translator.last_walk_error();
                break;
            }
            entry = *(u64*)next;
        }
        if (reported < 8) {
            Step("list off=" + Hex((u64)off) + " walked=" + std::to_string(walked) + " first_read=" +
                 (first_read_ok ? "ok" : "fail") + " stop=" + stop + " names=[" + names + "]");
            ++reported;
        }
        if (found) {
            links_offset = off;
            steps = walked;
            break;
        }
        if (budget <= 0) break;
    }
    Step("candidate_list_offsets=" + std::to_string(offsets.size()) + " used=" + Hex((u64)links_offset) +
         " list_walked=" + std::to_string(steps) + " match=" + Hex(found));
    if (!found) {
        *error = "no EPROCESS matched the requested image name and pid";
        return false;
    }

                                                                                  
                                                           
    u64 cr3 = 0;
    for (size_t off = 0; off + 8 <= kBlock; off += 8) {
        u64 value = *(u64*)(block.data() + off);
        if (!value || (value & 0xFFF) || value < 0x1000 || value > 0x840000000ull) continue;
        u8 header[2]{};
        std::string ignored;
        if (!translator.ReadVaAs(value, nt_base, header, sizeof(header), &ignored)) continue;
        if (header[0] == 'M' && header[1] == 'Z') {
            cr3 = value;
            Step("DirectoryTableBase=" + Hex(value) + " at EPROCESS+" + Hex(off));
            break;
        }
    }
    if (!cr3) {
        *error = "no field in the EPROCESS reproduced ntoskrnl as a page table root";
        return false;
    }

                                                                                   
                                                                                   
                                                            
    u64 image_base = 0;
    u64 fallback_base = 0;
    u32 fallback_timestamp = 0, fallback_size = 0, fallback_entry = 0;
    for (size_t off = 0; off + 8 <= kBlock; off += 8) {
        u64 value = *(u64*)(block.data() + off);
        if (!value || (value & 0xFFFF) || value < 0x10000 || value > 0x7FFFFFFF0000ull) continue;
        u8 page[0x400]{};
        std::string ignored;
        if (!translator.ReadVaAs(cr3, value, page, sizeof(page), &ignored)) continue;
        u32 timestamp = 0, size_of_image = 0, entry_rva = 0;
        if (!DescribeHeader(page, &timestamp, &size_of_image, &entry_rva)) continue;
        if (!fallback_base) {
            fallback_base = value;
            fallback_timestamp = timestamp;
            fallback_size = size_of_image;
            fallback_entry = entry_rva;
        }
        if (expect_timestamp && timestamp != expect_timestamp) continue;
        Step("image_base=" + Hex(value) + " at EPROCESS+" + Hex(off) + " ts=" + Hex(timestamp, 8) +
             " size=" + Hex(size_of_image, 8));
        image_base = value;
        out->timestamp = timestamp;
        out->size_of_image = size_of_image;
        out->entry_rva = entry_rva;
        break;
    }
    if (!image_base && fallback_base) {
        Step("image_base fell back to first PE header " + Hex(fallback_base) + " ts=" + Hex(fallback_timestamp, 8) +
             " (expected " + Hex(expect_timestamp, 8) + ")");
        image_base = fallback_base;
        out->timestamp = fallback_timestamp;
        out->size_of_image = fallback_size;
        out->entry_rva = fallback_entry;
    }
    if (!image_base) {
        *error = "no field in the EPROCESS resolved to a page table mapping of the image";
        return false;
    }

    out->pid = pid;
    out->image_base = image_base;
    out->directory_table_base = cr3;
    out->header_verified = true;                                              
    out->resolved_via = "kernel_eprocess";
    if (image_leaf.size()) out->leaf = image_leaf;
    return true;
}

}              

                                                                                  
                                                                                  
                                                                              
                                                                                   
                                                                         
bool FindImageByUserScan(Translator& translator, u64 expect_timestamp, u32 expect_size, LiveTarget* out,
                         std::string* error) {
    const std::vector<u64>& roots = translator.candidate_table_roots();
    if (roots.empty()) {
        *error = "no kernel page table root is available for a user-space scan";
        return false;
    }
    struct Ranked {
        u64 root;
        int regions;
    };
    std::vector<Ranked> ranked;
    std::vector<u8> table;
    for (u64 root : roots) {
        if (ranked.size() >= 256) break;
        if (!translator.PeekPage(root, &table)) continue;
        std::vector<u8> pdpt;
        const u64 e = reinterpret_cast<const u64*>(table.data())[0xFF];
        if (!(e & 1)) continue;
        const u64 pdpt_pa = PhysMaskOf(e);
        if (!pdpt_pa || !translator.PeekPage(pdpt_pa, &pdpt)) continue;
        int regions = 0;
        for (int i = 0; i < 512 && regions < 4096; ++i) {
            const u64 pe = reinterpret_cast<const u64*>(pdpt.data())[i];
            if (!(pe & 1)) continue;
            if ((pe & 0x80) || 0) {                                           
                regions += 512;
                continue;
            }
            std::vector<u8> pd;
            const u64 pd_pa = PhysMaskOf(pe);
            if (!pd_pa || !translator.PeekPage(pd_pa, &pd)) continue;
            for (int j = 0; j < 512; ++j) {
                const u64 de = reinterpret_cast<const u64*>(pd.data())[j];
                if (de & 0x80) {
                    ++regions;
                    continue;
                }
                if (de & 1) ++regions;
            }
        }
        if (regions) ranked.push_back({root, regions});
    }
    if (ranked.empty()) {
        *error = "no page table mapped any user space";
        return false;
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.regions > b.regions; });

    int budget = 400000;                                               
    for (const Ranked& r : ranked) {
        if (budget <= 0) break;
        if (!translator.PeekPage(r.root, &table)) continue;
        const u64 e = reinterpret_cast<const u64*>(table.data())[0xFF];
        if (!(e & 1)) continue;
        std::vector<u8> pdpt;
        if (!translator.PeekPage(PhysMaskOf(e), &pdpt)) continue;
        for (int i = 0; i < 512 && budget > 0; ++i) {
            const u64 pe = reinterpret_cast<const u64*>(pdpt.data())[i];
            if (!(pe & 1) || (pe & 0x80)) continue;
            std::vector<u8> pd;
            if (!translator.PeekPage(PhysMaskOf(pe), &pd)) continue;
            for (int j = 0; j < 512 && budget > 0; ++j) {
                const u64 de = reinterpret_cast<const u64*>(pd.data())[j];
                if (!(de & 1)) continue;
                const u64 region = (0xFFull << 39) | ((u64)i << 30) | ((u64)j << 21);
                for (int k = 0; k < 32; ++k) {
                    if (budget <= 0) break;
                    const u64 va = region + (u64)k * 0x10000;
                    u8 header[0x400];
                    std::string ignored;
                    --budget;
                    if (!translator.ReadVaAs(r.root, va, header, sizeof(header), &ignored)) continue;
                    u32 timestamp = 0, size_of_image = 0, entry_rva = 0;
                    if (!DescribeHeader(header, &timestamp, &size_of_image, &entry_rva)) continue;
                    if (expect_timestamp && timestamp != expect_timestamp) continue;
                    if (expect_size && size_of_image != expect_size) continue;
                    Step("user_scan base=" + Hex(va) + " root=" + Hex(r.root) + " regions=" + std::to_string(r.regions));
                    out->image_base = va;
                    out->timestamp = timestamp;
                    out->size_of_image = size_of_image;
                    out->entry_rva = entry_rva;
                    out->directory_table_base = r.root;
                    out->kernel_root = r.root;
                    out->header_verified = true;
                    out->resolved_via = "user_scan";
                    return true;
                }
            }
        }
    }
    *error = "no user-space region carried the header of the image on disk";
    return false;
}

bool PathOf(u32 pid, std::wstring* path, std::string* leaf) {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle) return false;
    wchar_t buffer[MAX_PATH]{};
    DWORD chars = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(handle, 0, buffer, &chars) != 0;
    CloseHandle(handle);
    if (!ok || !chars) return false;
    path->assign(buffer, chars);
    if (leaf) *leaf = Narrow(LeafOf(*path));
    return true;
}

bool DiskImage::Open(const std::wstring& path, std::string* error) {
    Close();
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        file = nullptr;
        *error = "cannot open the image on disk";
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        *error = "cannot size the image on disk";
        return false;
    }
    file_size = (u64)size.QuadPart;

    u8 header[0x1000]{};
    DWORD got = 0;
    if (!ReadFile(file, header, sizeof(header), &got, nullptr) || got < 0x200) {
        *error = "cannot read the image header on disk";
        return false;
    }
    if (header[0] != 'M' || header[1] != 'Z') {
        *error = "image on disk is not an MZ file";
        return false;
    }
    u32 lfanew = *(u32*)(header + 0x3C);
    if (lfanew + 0x108 > got || *(u32*)(header + lfanew) != 0x00004550) {
        *error = "image on disk has no PE header";
        return false;
    }
    const u8* nt = header + lfanew;
    u16 nsec = *(u16*)(nt + 6);
    u16 opt_size = *(u16*)(nt + 20);
    const u8* opt = nt + 24;
    size_of_headers = *(u32*)(opt + 60);
    timestamp = *(u32*)(nt + 8);
    size_of_image = *(u32*)(opt + 56);
    entry_rva = *(u32*)(opt + 16);
    image_base = (*(u16*)(opt) == 0x20B) ? *(u64*)(opt + 24) : (u64) * (u32*)(opt + 28);
    const u8* sec = opt + opt_size;
    if ((const u8*)sec + (size_t)nsec * 40 > header + got) {
        *error = "section table does not fit in the first page";
        return false;
    }
    for (u16 i = 0; i < nsec; ++i, sec += 40) {
        PeSection s{};
        s.name.assign((const char*)sec, strnlen((const char*)sec, 8));
        s.vsize = *(u32*)(sec + 8);
        s.va = *(u32*)(sec + 12);
        s.rawsize = *(u32*)(sec + 16);
        s.raw = *(u32*)(sec + 20);
        s.characteristics = *(u32*)(sec + 36);
        sections.push_back(s);
    }
    return true;
}

bool DiskImage::ReadRva(u32 rva, void* dst, size_t n) {
    if (!file) return false;
    u64 offset = ~0ull;
    if (rva < size_of_headers && (u64)rva + n <= size_of_headers) offset = rva;
    if (offset == ~0ull) {
        for (const auto& s : sections) {
            u32 span = s.vsize ? s.vsize : s.rawsize;
            if (rva < s.va || rva >= s.va + span) continue;
            u32 delta = rva - s.va;
            if ((u64)delta + n <= s.rawsize) offset = (u64)s.raw + delta;
            break;
        }
    }
    if (offset == ~0ull || offset + n > file_size) return false;
    LARGE_INTEGER position{};
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(file, dst, (DWORD)n, &got, nullptr) != 0 && got == n;
}

bool ResolveWithBase(u32 pid, u64 image_base, LiveTarget* out, std::string* error) {
    out->pid = pid;
    out->handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!out->handle) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "OpenProcess(%u) failed, win32=%lu", pid, GetLastError());
        *error = buf;
        return false;
    }
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(out->handle, 0, path, &size)) {
        out->image_path.assign(path, size);
        out->leaf = Narrow(LeafOf(out->image_path));
    }
    return Finish(out, image_base, 0, error);
}

bool Resolve(u32 pid, LiveTarget* out, std::string* error) {
    out->pid = pid;
    out->handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!out->handle) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "OpenProcess(%u) failed, win32=%lu", pid, GetLastError());
        *error = buf;
        return false;
    }
    wchar_t path[MAX_PATH]{};
    DWORD path_chars = MAX_PATH;
    if (QueryFullProcessImageNameW(out->handle, 0, path, &path_chars)) {
        out->image_path.assign(path, path_chars);
        out->leaf = Narrow(LeafOf(out->image_path));
    }

                                                                                 
                                                                           
                                                                            
                     
    struct Attempt {
        const char* name;
        bool ok;
        u64 base;
        u32 size;
    } attempts[3] = {{"module_list", false, 0, 0}, {"peb", false, 0, 0}, {"mapped_name", false, 0, 0}};

    u64 base = 0;
    u32 size = 0;
    attempts[0].ok = BaseFromModuleList(out->handle, &base, &size);
    attempts[0].base = base;
    attempts[0].size = size;
    if (!attempts[0].ok) {
        base = 0;
        size = 0;
        attempts[1].ok = BaseFromPeb(out->handle, &base);
        attempts[1].base = base;
    }
    if (!attempts[0].ok && !attempts[1].ok) {
        attempts[2].ok = BaseFromMappedName(out->handle, LeafOf(out->image_path), &base, &size);
        attempts[2].base = base;
        attempts[2].size = size;
    }
    std::string chosen;
    std::string last_error;
    for (const auto& a : attempts) {
        if (!a.ok) continue;
        std::string finish_error;
        if (Finish(out, a.base, a.size, &finish_error)) {
            chosen = a.name;
            break;
        }
        last_error = finish_error;
    }
    if (chosen.empty()) {
                                                                                   
                                                                                   
                                                             
        std::vector<u64> bases;
        std::vector<u32> sizes;
        if (BasesFromMappedName(out->handle, LeafOf(out->image_path), &bases, &sizes)) {
            u32 disk_size = 0;
            {
                HANDLE file = CreateFileW(out->image_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    u8 disk_header[0x400]{};
                    DWORD got = 0;
                    u32 ts = 0, entry = 0;
                    if (ReadFile(file, disk_header, sizeof(disk_header), &got, nullptr) &&
                        got == sizeof(disk_header)) {
                        DescribeHeader(disk_header, &ts, &disk_size, &entry);
                    }
                    CloseHandle(file);
                }
            }
            for (size_t i = 0; i < bases.size(); ++i) {
                if (disk_size && sizes[i] != disk_size) continue;
                std::string finish_error;
                if (Finish(out, bases[i], sizes[i], &finish_error)) {
                    chosen = "mapped_name";
                    break;
                }
                last_error = finish_error;
            }
            if (chosen.empty()) {
                for (size_t i = 0; i < bases.size(); ++i) {
                    std::string finish_error;
                    if (Finish(out, bases[i], 0, &finish_error)) {
                        chosen = "mapped_name_any";
                        break;
                    }
                    last_error = finish_error;
                }
            }
        }
    }
    if (chosen.empty()) {
        *error = "no base-resolution route produced a valid image header";
        if (!last_error.empty()) *error += ": " + last_error;
        return false;
    }
    out->resolved_via = chosen;
    return true;
}

}                     
