                                                                             
#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>
#include <memory>

#pragma comment(lib, "bcrypt.lib")

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

                                                   
struct PhysRange {
    u64 start = 0;
    u64 end = 0;
};

                                                                              
                  
                                                                              

inline std::string Hex(u64 value, int width = 16) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%0*llX", width, (unsigned long long)value);
    return buf;
}

inline std::string HexBytes(const void* data, size_t n) {
    static const char* digits = "0123456789abcdef";
    const u8* p = static_cast<const u8*>(data);
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(digits[p[i] >> 4]);
        out.push_back(digits[p[i] & 0xF]);
    }
    return out;
}

inline std::string UtcNow() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

inline std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

inline std::string Json(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }

inline std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

inline std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

                                                                              
                                           
                                                                              

class Logger {
public:
    bool Open(const std::wstring& path) {
        if (_wfopen_s(&file_, path.c_str(), L"wb") != 0) return false;
        return file_ != nullptr;
    }
    ~Logger() { if (file_) std::fclose(file_); }

    void Event(const std::string& type, const std::string& fields = std::string()) {
        std::string line = "{\"ts\":" + Json(UtcNow()) + ",\"type\":" + Json(type);
        if (!fields.empty()) line += "," + fields;
        if (!pending_.empty()) line += pending_;
        pending_.clear();
        line += "}";
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        if (file_) {
            std::fputs(line.c_str(), file_);
            std::fputc('\n', file_);
            std::fflush(file_);
        }
    }

    void Field(const char* key, const std::string& value) { pending_ += std::string(",\"") + key + "\":" + value; }
    static std::string Num(u64 v) { char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)v); return b; }
    static std::string Bool(bool v) { return v ? "true" : "false"; }
    static std::string Str(const std::string& v) { return Json(v); }

private:
    FILE* file_ = nullptr;
    std::string pending_;
};

                                                                              
                                                                         
                                                                              

inline bool Sha256File(const std::wstring& path, std::string* out_hex) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    DWORD obj_size = 0, cb = 0;
    std::vector<u8> obj, digest;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) goto done;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_size, sizeof(obj_size), &cb, 0) < 0) goto done;
    obj.resize(obj_size);
    digest.resize(32);
    if (BCryptCreateHash(alg, &hash, obj.data(), obj_size, nullptr, 0, 0) < 0) goto done;
    {
        std::vector<u8> buf(1 << 20);
        DWORD read = 0;
        while (ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr) && read > 0) {
            if (BCryptHashData(hash, buf.data(), read, 0) < 0) goto done;
        }
    }
    if (BCryptFinishHash(hash, digest.data(), (ULONG)digest.size(), 0) < 0) goto done;
    *out_hex = HexBytes(digest.data(), digest.size());
    ok = true;
done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return ok;
}

                                                                                
inline bool Sha256Bytes(const void* data, size_t n, std::string* out_hex) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    DWORD obj_size = 0, cb = 0;
    std::vector<u8> obj, digest;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) goto done;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_size, sizeof(obj_size), &cb, 0) < 0) goto done;
    obj.resize(obj_size);
    digest.resize(32);
    if (BCryptCreateHash(alg, &hash, obj.data(), obj_size, nullptr, 0, 0) < 0) goto done;
    if (n && BCryptHashData(hash, (PUCHAR)data, (ULONG)n, 0) < 0) goto done;
    if (BCryptFinishHash(hash, digest.data(), (ULONG)digest.size(), 0) < 0) goto done;
    *out_hex = HexBytes(digest.data(), digest.size());
    ok = true;
done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

inline bool ReadWholeFile(const std::wstring& path, std::vector<u8>* out) {    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (64ll << 20)) {
        CloseHandle(h);
        return false;
    }
    out->resize((size_t)size.QuadPart);
    DWORD read = 0;
    bool ok = ReadFile(h, out->data(), (DWORD)out->size(), &read, nullptr) && read == out->size();
    CloseHandle(h);
    return ok;
}

                                                                              
                                                             
                                                                              

struct PeSection {
    u32 va, vsize, raw, rawsize, characteristics;
    std::string name;
};

struct PeFile {
    std::vector<u8> bytes;
    u64 image_base = 0;
    u32 size_of_image = 0;
    u32 size_of_headers = 0;
    u32 timestamp = 0;
    std::vector<PeSection> sections;

    bool Load(const std::wstring& path) {
        if (!ReadWholeFile(path, &bytes) || bytes.size() < 0x200) return false;
        if (bytes[0] != 'M' || bytes[1] != 'Z') return false;
        u32 lfanew = *(u32*)(bytes.data() + 0x3C);
        if (lfanew + 0x108 > bytes.size()) return false;
        if (*(u32*)(bytes.data() + lfanew) != 0x00004550) return false;
        const u8* nt = bytes.data() + lfanew;
        u16 nsec = *(u16*)(nt + 6);
        u16 opt_size = *(u16*)(nt + 20);
        const u8* opt = nt + 24;
        u16 magic = *(u16*)(opt + 0);
                                                                            
        image_base = (magic == 0x20B) ? *(u64*)(opt + 24) : (u64)*(u32*)(opt + 28);
        size_of_image = *(u32*)(opt + 56);
        size_of_headers = *(u32*)(opt + 60);
        timestamp = *(u32*)(nt + 8);
        const u8* sec = opt + opt_size;
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

    long OffsetOf(u32 rva) const {
        for (const auto& s : sections) {
            u32 span = s.vsize ? s.vsize : s.rawsize;
            if (rva >= s.va && rva < s.va + span) {
                u32 d = rva - s.va;
                if (d < s.rawsize) return (long)(s.raw + d);
            }
        }
        return -1;
    }

    u32 ExportRva(const char* name) const {
        u32 lfanew = *(u32*)(bytes.data() + 0x3C);
        const u8* opt = bytes.data() + lfanew + 24;
        u32 dir_rva = *(u32*)(opt + 112 + 0 * 8);
        u32 dir_size = *(u32*)(opt + 112 + 0 * 8 + 4);
        if (!dir_rva || !dir_size) return 0;
        long off = OffsetOf(dir_rva);
        if (off < 0) return 0;
        const u8* d = bytes.data() + off;
        u32 num_names = *(u32*)(d + 24);
        u32 names_rva = *(u32*)(d + 32);
        u32 ords_rva = *(u32*)(d + 36);
        long names_off = OffsetOf(names_rva);
        long ords_off = OffsetOf(ords_rva);
        if (names_off < 0 || ords_off < 0) return 0;
        for (u32 i = 0; i < num_names; ++i) {
            u32 name_rva = *(u32*)(bytes.data() + names_off + i * 4);
            long name_off = OffsetOf(name_rva);
            if (name_off < 0) continue;
            if (strcmp((const char*)bytes.data() + name_off, name) == 0) {
                u16 ordinal = *(u16*)(bytes.data() + ords_off + i * 2);
                u32 func_rva = *(u32*)(bytes.data() + off + 40 + ordinal * 4);
                return func_rva;
            }
        }
        return 0;
    }
};
