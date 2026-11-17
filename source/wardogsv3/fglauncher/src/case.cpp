           
#include "case.h"

#include "json.h"

namespace casefile {

bool ParseHexBytes(const std::string& text, std::vector<u8>* out, std::string* error) {
    out->clear();
    int hi = -1;
    for (char c : text) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-' || c == '_') continue;
        else {
            if (error) *error = "not a hex digit in \"" + text + "\"";
            return false;
        }
        if (hi < 0) {
            hi = v;
        } else {
            out->push_back((u8)((hi << 4) | v));
            hi = -1;
        }
    }
    if (hi >= 0) {
        if (error) *error = "odd number of hex digits in \"" + text + "\"";
        return false;
    }
    return true;
}

std::string ToHex(const void* data, size_t n) { return HexBytes(data, n); }

}                       

namespace {

bool GetU64(const mj::Value* v, u64* out) {
    if (!v) return false;
    return v->AsU64(out, 0);
}

void ReadString(const mj::Value& obj, const char* key, std::string* out) {
    if (const mj::Value* v = obj.Find(key)) {
        if (v->is_string() || v->kind == mj::Value::kNumber) *out = v->AsString();
    }
}

}              

bool CaseFile::Load(const std::wstring& path, std::string* error) {
    std::vector<u8> raw;
    if (!ReadWholeFile(path, &raw)) {
        *error = "cannot read case file";
        return false;
    }
    std::string text((const char*)raw.data(), raw.size());
                                                                             
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text.erase(0, 3);
    }
    mj::Value root;
    if (!mj::Parse(text, &root, error)) return false;
    if (!root.is_object()) {
        *error = "case file must be a JSON object";
        return false;
    }

    ReadString(root, "schema", &schema);
    ReadString(root, "id", &id);
    ReadString(root, "name", &name);
    if (id.empty()) {
        *error = "case file has no \"id\"";
        return false;
    }

    if (const mj::Value* source = root.Find("source")) {
        ReadString(*source, "export_root", &export_root);
        ReadString(*source, "note", &reference_note);
    }

    const mj::Value* target = root.Find("target");
    if (!target || !target->is_object()) {
        *error = "case file has no \"target\" object";
        return false;
    }
    ReadString(*target, "process_name", &process_name);
    const mj::Value* image = target->Find("image");
    if (!image || !image->is_object()) {
        *error = "case target has no \"image\" object";
        return false;
    }
    ReadString(*image, "path", &image_rel);
    ReadString(*image, "sha256", &image_sha256);
    GetU64(image->Find("base"), &image_base);
    u64 stamp = 0;
    if (GetU64(image->Find("timestamp"), &stamp)) image_timestamp = (u32)stamp;
    u64 size = 0;
    if (GetU64(image->Find("size_of_image"), &size)) image_size = (u32)size;
    if (image_rel.empty()) {
        *error = "case target image has no \"path\"";
        return false;
    }
    {
                                                                              
                                                                               
                                                                
        std::wstring wide = Widen(image_rel);
        bool absolute = wide.size() > 1 && wide[1] == L':';
        if (absolute) {
            image_path = wide;
        } else {
            std::wstring candidate = Widen(export_root) + L"\\" + wide;
            if (!export_root.empty() && GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                image_path = candidate;
            } else {
                size_t slash = path.find_last_of(L"\\/");
                std::wstring dir = slash == std::wstring::npos ? L"." : path.substr(0, slash);
                image_path = dir + L"\\" + wide;
            }
        }
    }

    if (const mj::Value* list = root.Find("observations")) {
        if (!list->is_array()) {
            *error = "\"observations\" must be an array";
            return false;
        }
        for (size_t i = 0; i < list->items.size(); ++i) {
            const mj::Value& item = list->items[i];
            if (!item.is_object()) {
                *error = "observation entries must be objects";
                return false;
            }
            CaseObservation obs;
            ReadString(item, "name", &obs.name);
            ReadString(item, "scope", &obs.scope);
            ReadString(item, "module", &obs.module);
            ReadString(item, "expect", &obs.expect_hex);
            ReadString(item, "note", &obs.note);
            u64 va = 0;
            if (!GetU64(item.Find("va"), &va)) {
                *error = "observation \"" + obs.name + "\" has no numeric \"va\"";
                return false;
            }
            obs.va = va;
            u64 length = 0;
            if (!GetU64(item.Find("length"), &length) || length == 0 || length > 0x10000) {
                *error = "observation \"" + obs.name + "\" needs a length in 1..65536";
                return false;
            }
            obs.length = (u32)length;
            if (obs.name.empty()) obs.name = "obs" + std::to_string(i);
            if (obs.has_expectation()) {
                std::vector<u8> bytes;
                if (!casefile::ParseHexBytes(obs.expect_hex, &bytes, error)) return false;
                if (bytes.size() != obs.length) {
                    *error = "observation \"" + obs.name + "\": expect has " + std::to_string(bytes.size()) +
                             " bytes but length is " + std::to_string(obs.length);
                    return false;
                }
            }
            observations.push_back(std::move(obs));
        }
    }

    if (const mj::Value* gate_obj = root.Find("gate")) {
        if (!gate_obj->is_object()) {
            *error = "\"gate\" must be an object";
            return false;
        }
        ReadString(*gate_obj, "module", &gate.module);
        ReadString(*gate_obj, "routine", &gate.routine);
        ReadString(*gate_obj, "patch", &gate.patch_hex);
        u64 rva = 0;
        if (GetU64(gate_obj->Find("routine_rva"), &rva)) {
            gate.routine_rva = (u32)rva;
            gate.has_routine_rva = rva != 0;
        }
        u64 flag = 0;
        if (GetU64(gate_obj->Find("flag_rva"), &flag)) {
            gate.flag_rva = (u32)flag;
            gate.has_flag_rva = flag != 0;
        }
        if (!gate.patch_hex.empty()) {
            std::vector<u8> bytes;
            if (!casefile::ParseHexBytes(gate.patch_hex, &bytes, error)) return false;
            if (bytes.empty()) {
                *error = "gate patch decodes to zero bytes";
                return false;
            }
        }
    }
    if (gate.module.empty()) gate.module = "ntoskrnl.exe";
    return true;
}
