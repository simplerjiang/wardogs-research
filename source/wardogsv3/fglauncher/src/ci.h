                                                                            
  
                                                                               
                                                                               
                                                                                
                                                                              
  
                                                                   
                                                                             
                                                                                
                                                                         
  
                                                                    
                                                                           
                                    
#pragma once

#include "common.h"
#include "translate.h"

struct CiCandidate {
    u32 rva = 0;
    u64 va = 0;
    u32 value = 0;
    int xrefs = 0;
    std::string section;
    std::string source;
};

struct CiImage {
    u64 base = 0;
    u32 size = 0;
    std::vector<PeSection> sections;
    u32 timestamp = 0;
};

namespace ci {

                                                                            
                                                                              
                                                                             
                                                                            
struct NotifyTarget {
    u32 ex_rva = 0;                                                          
    u32 set_rva = 0;                                                        
    u32 remove_rva = 0;                                      
    u32 mask_rva = 0;                                                        
    u32 mask_bit = 0;                                 
    u32 mask_site_rva = 0;                                              
    std::string note;
};

bool LocateNotifyTargets(const PeFile& disk_nt, NotifyTarget* out, std::string* error);

bool ReadImage(Translator& t, u64 base, u32 size, CiImage* out, std::string* error);

                                                                            
                                                                       
bool FindValueCandidates(Translator& t, const CiImage& image, u32 value, std::vector<CiCandidate>* out,
                         std::string* error);

bool ReadDword(Translator& t, u64 va, u32* value, std::string* error);
bool WriteDword(Translator& t, u64 va, u32 value, std::string* error);

                                                                                  
                                                                           
bool ReadBytes(Translator& t, u64 va, void* dst, size_t n, std::string* error);
bool WriteBytes(Translator& t, u64 va, const void* src, size_t n, std::string* error);

}                 

