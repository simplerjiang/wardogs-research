                                                                                   
  
                                                                               
                                                                               
                                                           
  
                                                                                
                                                                            
                                                                               
                                                                            
                                                                         
  
                                                                            
                                                                               
                                                  
  
                                                                              
                                                                         
#pragma once

#include "common.h"
#include "provider.h"

#include <functional>
#include <vector>

class Translator {
public:
    using PhysRead = std::function<bool(u64 pa, void* dst, size_t n, std::string* error)>;
    using PhysWrite = std::function<bool(u64 pa, const void* src, size_t n, std::string* error)>;

    bool Init(IMemoryProvider* provider, std::string* error);
    bool InitMemory(const std::vector<u8>* physical_image);

                                                                              
                                                                              
                                                                                
                                               
    void SetBands(std::vector<PhysRange> ranges);
    bool InBands(u64 pa) const;
    bool InBands(u64 pa, u64 n) const;

                                                                               
                                                   
    bool Discover(u64 nt_base, u32 nt_size, u32 nt_timestamp, const std::vector<PhysRange>& ranges,
                  std::string* error);

                                                                             
                                                                              
                                                                       
    bool DiscoverForImage(u64 image_base, u32 timestamp, u32 size_of_image, std::string* error);

    bool ready() const { return cr3_ != 0; }
    u64 cr3() const { return cr3_; }
    void SetCr3ForTest(u64 cr3) { cr3_ = cr3; }
                                                                             
                                                                               
                                                                                
                             
    void SetCollectAllRoots(bool on) { collect_all_ = on; }
                                                                                
                                                                              
    bool AdoptAddressSpace(u64 cr3);

    u64 scanned_bytes() const { return scanned_; }
    u64 candidate_pages() const { return candidates_; }
    const std::vector<u64>& candidate_table_roots() const { return candidate_roots_; }
                                                                                  
                                                                              
                                                                               
                                                                              
    const std::vector<u64>& candidate_table_pages() const { return candidate_pages_; }
    u64 scan_skipped_windows() const { return scan_skipped_windows_; }
    const std::vector<u64>& scan_skipped_examples() const { return scan_skipped_examples_; }
    u64 image_scan_roots() const { return image_scan_roots_; }
    u64 image_scan_pages() const { return image_scan_pages_; }
                                                                                
                                                                                    
                                                                                  
    const std::string& image_scan_note() const { return image_scan_note_; }

    u64 VaToPa(u64 va);
    u64 WalkFrom(u64 cr3, u64 va);
                                                                                   
                                                                                    
                                                                   
    const std::string& last_walk_error() const { return last_walk_error_; }
                                                                                    
    bool PeekPage(u64 pa, std::vector<u8>* out);
    bool ReadVa(u64 va, void* dst, size_t n, std::string* error);
    bool WriteVa(u64 va, const void* src, size_t n, std::string* error);
    bool ReadVaAs(u64 cr3, u64 va, void* dst, size_t n, std::string* error);

                                                                           
    static bool LooksLikePml4(const u8* page, u64 max_phys);

private:
    struct PageSlot {
        u64 pa = 0;
        u8 data[0x1000];
        bool used = false;
    };
    bool ReadPage(u64 pa, const u8** out, std::string* error);
    u64 Walk(u64 va);
    void FlushCache();
    bool CandidateMapsKernel(u64 pa, u64 nt_base);

    PhysRead read_;
    PhysWrite write_;
    u64 cr3_ = 0;
    u64 scanned_ = 0;
    u64 candidates_ = 0;
    bool collect_all_ = false;
    std::vector<u64> candidate_roots_;                                            
    std::vector<u64> candidate_pages_;                                          
    u64 image_scan_roots_ = 0;
    u64 image_scan_pages_ = 0;
    std::string image_scan_note_;
    std::string last_walk_error_;
    std::string scan_error_note_;
public:
    const std::string& scan_error_note() const { return scan_error_note_; }
private:
    u64 scan_skipped_windows_ = 0;
    std::vector<u64> scan_skipped_examples_;
    std::vector<PhysRange> bands_;
    PageSlot cache_[64];
    unsigned clock_ = 0;
};
