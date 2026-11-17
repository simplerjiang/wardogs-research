                                                                            
  
                                                    
  
                                                                                  
                                                                                 
                                                                                    
  
                                                                            
                                                                             
                                                                           
  
                                                                                   
                                                                                     
  
                                                                                
                                                                                 
                                                               
#pragma once

#include "common.h"
#include "pgr.h"

#include <memory>
#include <string>

class IMemoryProvider {
public:
    virtual ~IMemoryProvider() = default;

                                                                          
    virtual const char* id() const = 0;

                                                       
    virtual bool ready() const = 0;

                                                                                
                                                                           
    virtual bool ReadPhys(u64 pa, void* dst, size_t n, std::string* error) = 0;
    virtual bool WritePhys(u64 pa, const void* src, size_t n, std::string* error) = 0;

                                                                               
                                                                            
    virtual bool writable() const { return true; }

    virtual void Close() {}
};

                                                                            
                                                                           
                                                                               
                                                                              
class PgrProvider final : public IMemoryProvider {
public:
    explicit PgrProvider(PgrDevice* device) : device_(device) {}

    const char* id() const override { return "pgr"; }
    bool ready() const override { return device_ && device_->ready(); }

    bool ReadPhys(u64 pa, void* dst, size_t n, std::string* error) override {
        return device_->ReadPhys(pa, dst, n, error);
    }
    bool WritePhys(u64 pa, const void* src, size_t n, std::string* error) override {
        return device_->WritePhys(pa, src, n, error);
    }
    void Close() override {
        if (device_) device_->Close();
    }

private:
    PgrDevice* device_ = nullptr;
};

                                                                          
                                                                      
inline std::unique_ptr<IMemoryProvider> MakeProvider(const std::string& id, PgrDevice* pgr) {
    if (id == "pgr") return std::make_unique<PgrProvider>(pgr);
    return nullptr;
}
