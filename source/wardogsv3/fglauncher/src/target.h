                                                                      
  
                                                                             
                                                           
  
                                             
  
                                                                               
                                                                                
                                                                                 
                                                                            
                                      
  
                                                                             
                                                                   
#pragma once

#include "common.h"
#include "translate.h"

#include <string>

struct LiveTarget {
    u32 pid = 0;
    std::wstring image_path;
    std::string leaf;
    u64 image_base = 0;
    u32 size_of_image = 0;
    u32 timestamp = 0;
    u32 entry_rva = 0;
    HANDLE handle = nullptr;
    u64 directory_table_base = 0;                                               
    u64 kernel_root = 0;                                                       
    bool header_verified = false;                                                  
    std::string resolved_via;                                             

    void Close() {
        if (handle) {
            ::CloseHandle(handle);
            handle = nullptr;
        }
    }
    ~LiveTarget() { Close(); }
};

namespace target {

                                                                            
                                                                                
                                                                               
                       
struct DiskImage {
    HANDLE file = nullptr;
    u64 file_size = 0;
    u32 size_of_headers = 0;
    u64 image_base = 0;
    u32 size_of_image = 0;
    u32 timestamp = 0;
    u32 entry_rva = 0;
    std::vector<PeSection> sections;

    ~DiskImage() { Close(); }
    void Close() {
        if (file && file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
        file = nullptr;
    }
    bool Open(const std::wstring& path, std::string* error);
                                                                            
    bool ReadRva(u32 rva, void* dst, size_t n);
};

                                                                                 
                                                                         
                                    
bool Resolve(u32 pid, LiveTarget* out, std::string* error);

                                                                                
                                                               
bool ResolveWithBase(u32 pid, u64 image_base, LiveTarget* out, std::string* error);

                                                                              
                                                                               
                                                            
  
                                                                                 
                                                                           
                                                                           
                                                                             
  
                                                                                
                                                  
  
                                                                                  
                                                                              
                                                                      
bool FromKernel(Translator& translator, u64 nt_base, const PeFile& disk_nt, u32 pid,
                const std::string& image_leaf, u32 expect_timestamp, LiveTarget* out,
                std::string* error);

                                                                               
                                                                                
void TraceReset();
const std::vector<std::string>& Trace();

                                                                            
                                                                                
                                                                        
                                                                                 
                                                                                
                            
bool FindImageByUserScan(Translator& translator, u64 expect_timestamp, u32 expect_size, LiveTarget* out,
                         std::string* error);

bool PathOf(u32 pid, std::wstring* path, std::string* leaf);

}                     
