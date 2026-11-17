                
#include "translate.h"

namespace {
constexpr u64 kPage = 0x1000;

inline bool Present(u64 entry) { return (entry & 1) != 0; }
inline bool LargePage(u64 entry) { return (entry & 0x80) != 0; }
inline u64 PhysMask(u64 entry) { return entry & 0x000FFFFFFFFFF000ull; }

                                                                                
                                                                    
bool DescribeImage(const u8* page, u32* timestamp, u32* size_of_image) {
    if (page[0] != 'M' || page[1] != 'Z') return false;
    u32 lfanew = *(const u32*)(page + 0x3C);
    if (lfanew < 0x40 || lfanew > 0x800) return false;
    if (*(const u32*)(page + lfanew) != 0x00004550) return false;
    if (timestamp) *timestamp = *(const u32*)(page + lfanew + 8);
    if (size_of_image) *size_of_image = *(const u32*)(page + lfanew + 24 + 56);
    return true;
}
}              

bool Translator::PeekPage(u64 pa, std::vector<u8>* out) {
    out->assign(kPage, 0);
    std::string ignored;
    return read_(pa & ~0xFFFull, out->data(), out->size(), &ignored);
}

bool Translator::LooksLikePml4(const u8* page, u64 max_phys) {
    const u64* e = reinterpret_cast<const u64*>(page);
    int present = 0;
    for (int i = 256; i < 512; ++i) {
        u64 v = e[i];
        if (!Present(v)) continue;
        if (v & 0xE00ull) return false;                                          
        u64 pa = PhysMask(v);
        if (pa == 0 || pa >= max_phys || (pa & 0xFFF)) return false;
        ++present;
    }
    return present >= 1 && present <= 200;
}

void Translator::SetBands(std::vector<PhysRange> ranges) { bands_ = std::move(ranges); }

bool Translator::InBands(u64 pa) const {
    if (bands_.empty()) return true;                                            
    for (const auto& b : bands_) {
        if (pa >= b.start && pa < b.end) return true;
    }
    return false;
}

bool Translator::InBands(u64 pa, u64 n) const {
    if (bands_.empty()) return true;
    if (n == 0) return InBands(pa);
    u64 last = pa + n - 1;
    if (last < pa) return false;                
    for (const auto& b : bands_) {
        if (pa >= b.start && last < b.end) return true;
    }
    return false;
}

                                                                                
                                                                          
bool Translator::CandidateMapsKernel(u64 pa, u64 nt_base) {
    std::string ignored;
    const u8* page = nullptr;
    if (!ReadPage(pa, &page, &ignored)) return false;
    const u64 idx = (nt_base >> 39) & 0x1FF;
    u64 e = reinterpret_cast<const u64*>(page)[idx];
    if (!Present(e) || (e & 0xE00ull)) return false;
    u64 target = PhysMask(e);
    if (!target || (target & 0xFFF)) return false;
    return InBands(target);
}

void Translator::FlushCache() {
    for (auto& s : cache_) s.used = false;
    clock_ = 0;
}

bool Translator::Init(IMemoryProvider* provider, std::string* error) {
    if (!provider || !provider->ready()) {
        if (error) *error = "memory provider is not ready";
        return false;
    }
    read_ = [provider](u64 pa, void* dst, size_t n, std::string* err) {
        return provider->ReadPhys(pa, dst, n, err);
    };
    write_ = [provider](u64 pa, const void* src, size_t n, std::string* err) {
        return provider->WritePhys(pa, src, n, err);
    };
    FlushCache();
    cr3_ = 0;
    candidate_roots_.clear();
    return true;
}

bool Translator::InitMemory(const std::vector<u8>* image) {
    if (!image) return false;
    read_ = [image](u64 pa, void* dst, size_t n, std::string* err) {
        if (pa + n > image->size()) {
            *err = "physical read outside the test image";
            return false;
        }
        std::memcpy(dst, image->data() + pa, n);
        return true;
    };
    write_ = [image](u64 pa, const void* src, size_t n, std::string* err) {
        if (pa + n > image->size()) {
            *err = "physical write outside the test image";
            return false;
        }
        std::memcpy(const_cast<u8*>(image->data()) + pa, src, n);
        return true;
    };
    FlushCache();
    cr3_ = 0;
    candidate_roots_.clear();
    return true;
}

bool Translator::ReadPage(u64 pa, const u8** out, std::string* error) {
    pa &= ~0xFFFull;
    if (!InBands(pa)) {
        if (error) *error = "physical page outside the RAM bands: " + Hex(pa);
        last_walk_error_ = "bands:" + Hex(pa);
        return false;
    }
    for (auto& s : cache_) {
        if (s.used && s.pa == pa) {
            *out = s.data;
            return true;
        }
    }
    PageSlot& slot = cache_[clock_++ % 64];
    std::string local;
    if (!read_(pa, slot.data, kPage, &local)) {
        if (error) *error = local;
        last_walk_error_ = "read_failed:" + Hex(pa) + ":" + local;
        return false;
    }
    slot.pa = pa;
    slot.used = true;
    *out = slot.data;
    return true;
}

u64 Translator::WalkFrom(u64 cr3, u64 va) {
    if (!cr3) {
        last_walk_error_ = "no_cr3";
        return 0;
    }
    std::string ignored;
    u64 table = cr3 & ~0xFFFull;
    const u8* page = nullptr;

    u64 idx = (va >> 39) & 0x1FF;
    if (!ReadPage(table, &page, &ignored)) { last_walk_error_ = "L4:" + last_walk_error_; return 0; }
    u64 e = reinterpret_cast<const u64*>(page)[idx];
    if (!Present(e)) {
        last_walk_error_ = "L4:pml4e_absent:" + Hex(idx);
        return 0;
    }
    table = PhysMask(e);

    idx = (va >> 30) & 0x1FF;
    if (!ReadPage(table, &page, &ignored)) { last_walk_error_ = "L3:" + last_walk_error_; return 0; }
    e = reinterpret_cast<const u64*>(page)[idx];
    if (!Present(e)) {
        last_walk_error_ = "L3:pdpte_absent:" + Hex(idx);
        return 0;
    }
    if (LargePage(e)) {
        u64 pa = (e & 0x000FFFFFC0000000ull) | (va & 0x3FFFFFFFull);
        if (!InBands(pa)) last_walk_error_ = "bands:" + Hex(pa);
        else last_walk_error_.clear();
        return InBands(pa) ? pa : 0;
    }
    table = PhysMask(e);

    idx = (va >> 21) & 0x1FF;
    if (!ReadPage(table, &page, &ignored)) { last_walk_error_ = "L2:" + last_walk_error_; return 0; }
    e = reinterpret_cast<const u64*>(page)[idx];
    if (!Present(e)) {
        last_walk_error_ = "L2:pde_absent:" + Hex(idx);
        return 0;
    }
    if (LargePage(e)) {
        u64 pa = (e & 0x000FFFFFFFE00000ull) | (va & 0x1FFFFFull);
        if (!InBands(pa)) last_walk_error_ = "bands:" + Hex(pa);
        else last_walk_error_.clear();
        return InBands(pa) ? pa : 0;
    }
    table = PhysMask(e);

    idx = (va >> 12) & 0x1FF;
    if (!ReadPage(table, &page, &ignored)) { last_walk_error_ = "L1:" + last_walk_error_; return 0; }
    e = reinterpret_cast<const u64*>(page)[idx];
    if (!Present(e) || (e & 0x400ull)) {
        last_walk_error_ = (e & 0x400ull) ? "L1:prototype_pte:" + Hex(idx) : "L1:pte_absent:" + Hex(idx);
        return 0;
    }
    u64 pa = PhysMask(e) | (va & 0xFFFull);
    if (!InBands(pa & ~0xFFFull)) {
        last_walk_error_ = "bands:" + Hex(pa);
        return 0;
    }
    last_walk_error_.clear();
    return pa;
}

u64 Translator::Walk(u64 va) { return WalkFrom(cr3_, va); }

u64 Translator::VaToPa(u64 va) { return Walk(va); }

bool Translator::AdoptAddressSpace(u64 cr3) {
    if (!cr3 || (cr3 & 0xFFF)) return false;
    cr3_ = cr3;
    FlushCache();
    return true;
}

bool Translator::Discover(u64 nt_base, u32 nt_size, u32 nt_timestamp, const std::vector<PhysRange>& ranges,
                          std::string* error) {
    if (!read_) {
        *error = "translator not initialized";
        return false;
    }
    SetBands(ranges);
    constexpr u64 kChunk = 16ull << 20;                  
    constexpr size_t kMaxRoots = 1024;                                             
    constexpr size_t kMaxPages = 1u << 20;                                           
    scanned_ = 0;
    candidates_ = 0;
    candidate_roots_.clear();
    candidate_pages_.clear();
    scan_skipped_windows_ = 0;
    scan_skipped_examples_.clear();
    std::vector<u8> window((size_t)kChunk);
    std::string read_error;
    bool found = false;

    for (const auto& range : ranges) {
        if (range.end <= range.start) continue;
        for (u64 base = range.start & ~0xFFFull; base < range.end; base += kChunk) {
            u64 size = range.end - base < kChunk ? range.end - base : kChunk;
            scanned_ = base + size;
            if (!read_(base, window.data(), (size_t)size, &read_error)) {
                ++scan_skipped_windows_;
                if (scan_skipped_examples_.size() < 8) scan_skipped_examples_.push_back(base);
                if (scan_error_note_.empty()) scan_error_note_ = Hex(base) + " " + read_error;
                continue;
            }
            for (u64 off = 0; off + kPage <= size; off += kPage) {
                                                                                 
                                                                            
                const u8* page = window.data() + off;
                                                                                 
                                                                                   
                                                                                
                                                                               
                                                                                    
                                                                             
                auto plausible = [&](u64 e) {
                    if (!Present(e)) return false;
                    u64 pdpt = PhysMask(e);
                    return pdpt != 0 && (pdpt & 0xFFF) == 0 && InBands(pdpt);
                };
                const u64* entries = reinterpret_cast<const u64*>(page);
                const bool kernel_root = plausible(entries[(nt_base >> 39) & 0x1FF]);
                                                                                    
                                                                                   
                                                                                   
                                                                                      
                                                    
                bool user_root = plausible(entries[0]) || plausible(entries[0xFF]);
                if (!kernel_root && !user_root) continue;
                u64 candidate = base + off;
                ++candidates_;
                if (candidate_pages_.size() < kMaxPages) candidate_pages_.push_back(candidate);
                if (!kernel_root) continue;
                u64 pa = WalkFrom(candidate, nt_base);
                if (!pa) continue;
                u8 header[0x200];
                if (!read_(pa, header, sizeof(header), &read_error)) continue;
                u32 timestamp = 0;
                u32 size_of_image = 0;
                if (!DescribeImage(header, &timestamp, &size_of_image)) continue;
                if (nt_size && size_of_image != nt_size) continue;
                if (nt_timestamp && timestamp != nt_timestamp) continue;
                if (!found) {
                                                                               
                                                                      
                    cr3_ = candidate;
                    FlushCache();
                    found = true;
                    if (!collect_all_) return true;
                }
                if (candidate_roots_.size() < kMaxRoots) candidate_roots_.push_back(candidate);
                if (candidate_roots_.size() >= kMaxRoots) break;
            }
        }
    }
    if (!found) {
        *error = "no PML4 candidate mapped the live ntoskrnl image";
        return false;
    }
    return true;
}

                                                                              
                                                                             
                                                                                
                                                                                
                          
bool Translator::DiscoverForImage(u64 image_base, u32 timestamp, u32 size_of_image, std::string* error) {
    if (!read_) {
        *error = "translator not initialized";
        return false;
    }
    if (candidate_roots_.empty() && candidate_pages_.empty()) {
        *error = "no recorded candidates; the kernel discovery pass must run first";
        return false;
    }
    std::string read_error;
    image_scan_roots_ = 0;
    image_scan_pages_ = 0;
    image_scan_note_.clear();
    int notes = 0;

    auto try_candidate = [&](u64 candidate) -> bool {
        u64 pa = WalkFrom(candidate, image_base);
        if (!pa) return false;
        u8 page[kPage];
        if (!read_(pa & ~0xFFFull, page, sizeof(page), &read_error)) return false;
        u32 found_timestamp = 0;
        u32 found_size = 0;
        if (!DescribeImage(page, &found_timestamp, &found_size)) {
            if (page[0] == 'M' && page[1] == 'Z' && notes < 6) {
                image_scan_note_ += " cand=" + Hex(candidate) + " mz_unparsable";
                ++notes;
            }
            return false;
        }
        if ((timestamp && found_timestamp != timestamp) || (size_of_image && found_size != size_of_image)) {
            if (notes < 6) {
                image_scan_note_ += " cand=" + Hex(candidate) + " ts=" + Hex(found_timestamp, 8) +
                                    " size=" + Hex(found_size, 8);
                ++notes;
            }
            return false;
        }
        cr3_ = candidate;
        FlushCache();
        return true;
    };

                                                                                 
                                                                 
    for (u64 candidate : candidate_roots_) {
        ++image_scan_roots_;
        if (try_candidate(candidate)) return true;
    }
                                                                             
                                                                               
                                                                    
    for (u64 candidate : candidate_pages_) {
        ++image_scan_pages_;
        if (try_candidate(candidate)) return true;
    }
    *error = "no candidate page table mapped an image at " + Hex(image_base) + " (roots=" +
             std::to_string(image_scan_roots_) + ", pages=" + std::to_string(image_scan_pages_) + ")";
    return false;
}

bool Translator::ReadVa(u64 va, void* dst, size_t n, std::string* error) {
    u8* out = static_cast<u8*>(dst);
    while (n) {
        u64 pa = Walk(va);
        if (!pa) {
                                                                                 
                                                                                
                                                  
            const std::string first = last_walk_error_;
            FlushCache();
            pa = Walk(va);
            if (!pa) {
                *error = "unmapped VA " + Hex(va) + " (" + first + " / retry " + last_walk_error_ + ")";
                return false;
            }
        }
        size_t chunk = (size_t)std::min<u64>(n, kPage - (va & 0xFFF));
        if (!read_(pa, out, chunk, error)) return false;
        va += chunk;
        out += chunk;
        n -= chunk;
    }
    return true;
}

bool Translator::ReadVaAs(u64 cr3, u64 va, void* dst, size_t n, std::string* error) {
    u8* out = static_cast<u8*>(dst);
    while (n) {
        u64 pa = WalkFrom(cr3, va);
        if (!pa) {
            *error = "unmapped VA " + Hex(va) + " in address space " + Hex(cr3);
            return false;
        }
        size_t chunk = (size_t)std::min<u64>(n, kPage - (va & 0xFFF));
        if (!read_(pa, out, chunk, error)) return false;
        va += chunk;
        out += chunk;
        n -= chunk;
    }
    return true;
}

bool Translator::WriteVa(u64 va, const void* src, size_t n, std::string* error) {
    const u8* in = static_cast<const u8*>(src);
    while (n) {
        u64 pa = Walk(va);
        if (!pa) {
            *error = "unmapped VA " + Hex(va);
            return false;
        }
        size_t chunk = (size_t)std::min<u64>(n, kPage - (va & 0xFFF));
        if (!write_(pa, in, chunk, error)) return false;
        for (auto& s : cache_) {
            if (s.used && s.pa == (pa & ~0xFFFull)) s.used = false;
        }
        va += chunk;
        in += chunk;
        n -= chunk;
    }
    return true;
}
