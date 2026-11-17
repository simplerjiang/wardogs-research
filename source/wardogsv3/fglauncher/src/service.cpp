              
#include "service.h"

bool DriverService::EnablePrivilegeId(NativeApi& api, int id, bool enable, bool* was_enabled) {
    BOOLEAN previous = FALSE;
    NTSTATUS st = api.AdjustPrivilege((ULONG)id, enable ? TRUE : FALSE, FALSE, &previous);
    if (st < 0) return false;
    if (was_enabled) *was_enabled = previous != FALSE;
    return true;
}

bool DriverService::EnablePrivilege(NativeApi& api, bool enable, bool* was_enabled) {
    return EnablePrivilegeId(api, 10                          , enable, was_enabled);
}

bool DriverService::DevicePresent(const wchar_t* dos_name) {
    wchar_t buf[1024]{};
    if (QueryDosDeviceW(dos_name, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])))) return true;
    return GetLastError() != ERROR_FILE_NOT_FOUND;
}

bool DriverService::Create(const std::wstring& image_path, std::string* error) {
    if (!api_.Init()) {
        *error = "native api resolution failed";
        return false;
    }
    image_path_ = image_path;
    service_name_ = L"WardogLaunch_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                    std::to_wstring(GetTickCount64());
    key_path_ = L"SYSTEM\\CurrentControlSet\\Services\\" + service_name_;
    native_path_ = L"\\Registry\\Machine\\" + key_path_;

    HKEY key = nullptr;
    DWORD disposition = 0;
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_path_.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                              &disposition);
    if (rc != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
        if (key) RegCloseKey(key);
        *error = "service key creation failed rc=" + std::to_string(rc);
        return false;
    }
    created_ = true;
    struct Value {
        const wchar_t* name;
        DWORD data;
    } values[] = {{L"Type", 1}, {L"Start", 3}, {L"ErrorControl", 1}};
    for (const auto& v : values) {
        rc = RegSetValueExW(key, v.name, 0, REG_DWORD, (const BYTE*)&v.data, sizeof(DWORD));
        if (rc != ERROR_SUCCESS) {
            RegCloseKey(key);
            *error = "service value write failed";
            return false;
        }
    }
    std::wstring image = L"\\??\\" + image_path;
    rc = RegSetValueExW(key, L"ImagePath", 0, REG_EXPAND_SZ, (const BYTE*)image.c_str(),
                        (DWORD)((image.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        *error = "ImagePath write failed";
        return false;
    }
    return true;
}

NTSTATUS DriverService::Load(std::string* error) {
    UNICODE_STRING name{};
    name.Length = (USHORT)(native_path_.size() * sizeof(wchar_t));
    name.MaximumLength = (USHORT)((native_path_.size() + 1) * sizeof(wchar_t));
    name.Buffer = const_cast<wchar_t*>(native_path_.c_str());

    bool was_enabled = false;
    if (!EnablePrivilege(api_, true, &was_enabled)) {
        *error = "SeLoadDriverPrivilege could not be enabled (run elevated)";
        return STATUS_UNSUCCESSFUL;
    }
    NTSTATUS st = api_.LoadDriver(&name);
    if (!was_enabled) {
        bool unused = false;
        EnablePrivilege(api_, false, &unused);
    }
    loaded_ = st >= 0;
    if (st < 0) *error = "NtLoadDriver failed";
    return st;
}

NTSTATUS DriverService::Unload() {
    if (!loaded_) return STATUS_SUCCESS;
    UNICODE_STRING name{};
    name.Length = (USHORT)(native_path_.size() * sizeof(wchar_t));
    name.MaximumLength = (USHORT)((native_path_.size() + 1) * sizeof(wchar_t));
    name.Buffer = const_cast<wchar_t*>(native_path_.c_str());
                                                                                 
                                                               
    bool was_enabled = false;
    bool armed = EnablePrivilege(api_, true, &was_enabled);
    NTSTATUS st = api_.UnloadDriver(&name);
    if (armed && !was_enabled) {
        bool unused = false;
        EnablePrivilege(api_, false, &unused);
    }
    if (st >= 0) loaded_ = false;
    return st;
}

bool DriverService::Delete() {
    if (!created_) return true;
    if (loaded_) return false;
    LONG rc = RegDeleteKeyW(HKEY_LOCAL_MACHINE, key_path_.c_str());
    if (rc == ERROR_SUCCESS) created_ = false;
    return rc == ERROR_SUCCESS;
}

                                                                              
                    
                                                                              
std::vector<std::wstring> DriverService::FindLeftovers() {
    std::vector<std::wstring> names;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services", 0,
                      KEY_READ | KEY_ENUMERATE_SUB_KEYS, &root) != ERROR_SUCCESS) {
        return names;
    }
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]{};
        DWORD len = (DWORD)(sizeof(name) / sizeof(name[0]));
        LONG rc = RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) break;
        std::wstring n(name);
        if (n.rfind(L"WardogLaunch_", 0) == 0 || n == L"WardogProbe") names.push_back(n);
    }
    RegCloseKey(root);
    return names;
}

NTSTATUS DriverService::UnloadByName(const std::wstring& service_name, bool* key_removed) {
    NativeApi api;
    if (key_removed) *key_removed = false;
    if (!api.Init()) return STATUS_UNSUCCESSFUL;
    std::wstring native_path = L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\" + service_name;
    UNICODE_STRING name{};
    name.Length = (USHORT)(native_path.size() * sizeof(wchar_t));
    name.MaximumLength = (USHORT)((native_path.size() + 1) * sizeof(wchar_t));
    name.Buffer = const_cast<wchar_t*>(native_path.c_str());
    bool was_enabled = false;
    EnablePrivilege(api, true, &was_enabled);
    NTSTATUS st = api.UnloadDriver(&name);
    if (!was_enabled) {
        bool unused = false;
        EnablePrivilege(api, false, &unused);
    }
                                                                                    
                                                    
    if (st >= 0 || st == (NTSTATUS)0xC0000034L) {
        std::wstring key_path = L"SYSTEM\\CurrentControlSet\\Services\\" + service_name;
        if (RegDeleteKeyW(HKEY_LOCAL_MACHINE, key_path.c_str()) == ERROR_SUCCESS && key_removed) {
            *key_removed = true;
        }
    }
    return st;
}
