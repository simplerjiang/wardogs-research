                                                       
  
                                               
  
                                                                            
                                                                               
                                                                                 
  
                                                                             
                                                                             
                                                                               
                                                                                
                                 
#pragma once

#include "common.h"

#include <string>
#include <vector>

struct CaseObservation {
    std::string name;
    std::string scope = "process";                                            
    std::string module;                                             
    u64 va = 0;
    u32 length = 0;
    std::string expect_hex;                                                      
    std::string note;

    bool has_expectation() const { return !expect_hex.empty(); }
};

struct CaseGate {
    std::string module = "ntoskrnl.exe";
    std::string routine = "PsSetLoadImageNotifyRoutine";
    u32 routine_rva = 0;
    u32 flag_rva = 0;
    std::string patch_hex = "4831c0c3";                      
    bool has_routine_rva = false;
    bool has_flag_rva = false;
};

struct CaseFile {
    std::string schema;
    std::string id;
    std::string name;
    std::string export_root;
    std::string reference_note;

    std::wstring image_path;                                                
    std::string image_rel;                                                       
    std::string image_sha256;
    u64 image_base = 0;
    u32 image_timestamp = 0;
    u32 image_size = 0;
    std::string process_name;

    std::vector<CaseObservation> observations;
    CaseGate gate;

    bool Load(const std::wstring& path, std::string* error);
};

namespace casefile {

                                                                           
bool ParseHexBytes(const std::string& text, std::vector<u8>* out, std::string* error);

                                                                                
std::string ToHex(const void* data, size_t n);

}                       
