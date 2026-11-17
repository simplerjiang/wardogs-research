                                                              
  
                                                                                
                                                                            
                                                                          
                                                                          
  
                                                                          
                                                                                
                                                                       
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace mj {

struct Value;
using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
using Object = std::vector<Member>;

struct Value {
    enum Kind { kNull, kBool, kNumber, kString, kArray, kObject };

    Kind kind = kNull;
    bool boolean = false;
    std::string text;                                                 
    Array items;
    Object members;

    bool is_object() const { return kind == kObject; }
    bool is_array() const { return kind == kArray; }
    bool is_string() const { return kind == kString; }

    const Value* Find(const char* key) const {
        if (kind != kObject) return nullptr;
        for (const auto& m : members) {
            if (m.first == key) return &m.second;
        }
        return nullptr;
    }

    std::string AsString(const std::string& fallback = std::string()) const {
        if (kind == kString) return text;
        if (kind == kNumber) return text;
        return fallback;
    }

    bool AsBool(bool fallback = false) const { return kind == kBool ? boolean : fallback; }

                                                                                 
    bool AsU64(std::uint64_t* out, std::uint64_t fallback = 0) const {
        std::uint64_t v = fallback;
        if (kind == kNumber || kind == kString) {
            const char* s = text.c_str();
            if (text.empty()) return false;
            char* end = nullptr;
            unsigned long long parsed = std::strtoull(s, &end, 0);
            if (!end || *end != '\0') return false;
            v = (std::uint64_t)parsed;
        } else {
            return false;
        }
        if (out) *out = v;
        return true;
    }

    const Value* At(std::size_t i) const {
        if (kind != kArray || i >= items.size()) return nullptr;
        return &items[i];
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool Parse(Value* out, std::string* error) {
        SkipSpace();
        if (!ParseValue(out)) {
            if (error) *error = error_.empty() ? "malformed JSON" : error_;
            return false;
        }
        SkipSpace();
        if (i_ != s_.size()) {
            if (error) *error = "trailing content at offset " + std::to_string(i_);
            return false;
        }
        return true;
    }

private:
    void SkipSpace() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++i_;
            } else {
                break;
            }
        }
    }

    bool Fail(const char* what) {
        error_ = std::string(what) + " at offset " + std::to_string(i_);
        return false;
    }

    bool Literal(const char* word) {
        std::size_t n = std::strlen(word);
        if (s_.compare(i_, n, word) != 0) return Fail("unexpected token");
        i_ += n;
        return true;
    }

    bool ParseValue(Value* out) {
        if (i_ >= s_.size()) return Fail("unexpected end of input");
        char c = s_[i_];
        if (c == '{') return ParseObject(out);
        if (c == '[') return ParseArray(out);
        if (c == '"') {
            out->kind = Value::kString;
            return ParseString(&out->text);
        }
        if (c == 't') {
            out->kind = Value::kBool;
            out->boolean = true;
            return Literal("true");
        }
        if (c == 'f') {
            out->kind = Value::kBool;
            out->boolean = false;
            return Literal("false");
        }
        if (c == 'n') {
            out->kind = Value::kNull;
            return Literal("null");
        }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
        return Fail("unexpected character");
    }

    bool ParseNumber(Value* out) {
        std::size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        while (i_ < s_.size()) {
            char c = s_[i_];
            bool digit = c >= '0' && c <= '9';
            bool hex = (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            bool mark = c == 'x' || c == 'X';
            if (digit || hex || mark || c == '.') {
                ++i_;
                continue;
            }
            break;
        }
        if (i_ == start) return Fail("bad number");
        out->kind = Value::kNumber;
        out->text = s_.substr(start, i_ - start);
        return true;
    }

    bool ParseString(std::string* out) {
        if (s_[i_] != '"') return Fail("expected a string");
        ++i_;
        out->clear();
        while (i_ < s_.size()) {
            unsigned char c = (unsigned char)s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out->push_back((char)c);
                continue;
            }
            if (i_ >= s_.size()) return Fail("truncated escape");
            char e = s_[i_++];
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    if (i_ + 4 > s_.size()) return Fail("truncated \\u escape");
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s_[i_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                        else return Fail("bad \\u escape");
                    }
                                                                                
                                                                               
                                                              
                    if (code < 0x80) {
                        out->push_back((char)code);
                    } else {
                        out->push_back('?');
                    }
                    break;
                }
                default: return Fail("unknown escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseArray(Value* out) {
        out->kind = Value::kArray;
        ++i_;        
        SkipSpace();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            SkipSpace();
            Value item;
            if (!ParseValue(&item)) return false;
            out->items.push_back(std::move(item));
            SkipSpace();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseObject(Value* out) {
        out->kind = Value::kObject;
        ++i_;        
        SkipSpace();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            SkipSpace();
            std::string key;
            if (i_ >= s_.size() || s_[i_] != '"') return Fail("expected a member name");
            if (!ParseString(&key)) return false;
            SkipSpace();
            if (i_ >= s_.size() || s_[i_] != ':') return Fail("expected ':'");
            ++i_;
            SkipSpace();
            Value item;
            if (!ParseValue(&item)) return false;
            out->members.emplace_back(std::move(key), std::move(item));
            SkipSpace();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    const std::string& s_;
    std::size_t i_ = 0;
    std::string error_;
};

inline bool Parse(const std::string& text, Value* out, std::string* error) {
    Parser parser(text);
    return parser.Parse(out, error);
}

}                 
