                                                                                  
#pragma once

#include "nt.h"

                                                                              
                                                                   
                                                                               
class DriverService {
public:
    ~DriverService() { Unload(); Delete(); }

    bool Create(const std::wstring& image_path, std::string* error);
    NTSTATUS Load(std::string* error);
    NTSTATUS Unload();
    bool Delete();

    bool loaded() const { return loaded_; }
    const std::wstring& service_name() const { return service_name_; }
    const std::wstring& native_path() const { return native_path_; }

    static bool EnablePrivilege(NativeApi& api, bool enable, bool* was_enabled);
    static bool EnablePrivilegeId(NativeApi& api, int id, bool enable, bool* was_enabled);
    static bool DevicePresent(const wchar_t* dos_name);

                                                                                 
                                                                            
    static std::vector<std::wstring> FindLeftovers();
    static NTSTATUS UnloadByName(const std::wstring& service_name, bool* key_removed);

private:
    NativeApi api_{};
    std::wstring service_name_;
    std::wstring key_path_;
    std::wstring native_path_;
    std::wstring image_path_;
    bool created_ = false;
    bool loaded_ = false;
};
