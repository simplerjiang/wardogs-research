                                             
  
                                                                                       
                                                                            
  
                                                                                   
                                              
  
                                                                       
                                                                             
                                                                                 
                                                                                 
                                                                                      
  
                                     
                                                              
                                                
                                                                             
                                                                                        
                                                                                            
                                                        
                                               
                                                                                
                                                                         
#pragma once

#include "common.h"

class PgrDevice {
public:
    struct Mapping {
        u64 phys = 0;                                                   
        u64 size = 0;                           
        u64 handle = 0;                                  
        u64 object = 0;                                             
        u64 view_base = 0;                                                   
        u64 base = 0;                                               
        bool valid() const { return base != 0 && size != 0; }
        u8* ptr() const { return reinterpret_cast<u8*>(base); }
    };

    ~PgrDevice() { Close(); }

    bool Open(std::string* error);
    void Close();
    bool ready() const { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE handle() const { return handle_; }

    bool Handshake(u32* declared_size, std::string* error);
    bool Map(u64 phys, u64 size, Mapping* out, std::string* error);
    bool Unmap(Mapping* mapping);

    bool ReadPhys(u64 phys, void* dst, size_t n, std::string* error);
    bool WritePhys(u64 phys, const void* src, size_t n, std::string* error);

    bool PortWrite(u16 port, u32 value, int width, std::string* error);
    bool PortRead(u16 port, int width, u32* value, std::string* error);

    static constexpr u64 kMaxWindow = 64ull << 20;

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
