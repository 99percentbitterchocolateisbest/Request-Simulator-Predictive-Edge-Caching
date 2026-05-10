#pragma once
// Json.hpp ─────────────────────────────────────────────────────────────────
// Minimal recursive-descent JSON parser, header-only, no dependencies.
//
// We avoid a third-party JSON library (nlohmann, RapidJSON) because the
// rest of the simulator is intentionally self-contained — pthreads is
// already the only non-stdlib dependency, and we want to keep it that
// way for a course reference impl. Hand-rolled is ~250 lines and that's
// all we need for a flat config file.
//
// Supports: objects, arrays, strings (with \" \\ \n \t \uXXXX), numbers
// (integers and IEEE 754 doubles), true/false/null, comments are NOT
// supported (strict JSON, RFC 8259).
//
// Path lookup: cfg.find("hardware.queue_capacity") walks dotted paths.
// Typed accessors return std::optional so missing keys are graceful.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pec {

class JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
using JsonArray  = std::vector<JsonValue>;

class JsonValue {
public:
    using Variant = std::variant<std::nullptr_t, bool, double, std::string,
                                 JsonArray, JsonObject>;

    JsonValue() : v_(nullptr) {}
    JsonValue(std::nullptr_t)        : v_(nullptr) {}
    JsonValue(bool b)                : v_(b) {}
    JsonValue(double d)              : v_(d) {}
    JsonValue(std::string s)         : v_(std::move(s)) {}
    JsonValue(JsonArray a)           : v_(std::move(a)) {}
    JsonValue(JsonObject o)          : v_(std::move(o)) {}

    bool is_null()   const { return std::holds_alternative<std::nullptr_t>(v_); }
    bool is_bool()   const { return std::holds_alternative<bool>(v_); }
    bool is_number() const { return std::holds_alternative<double>(v_); }
    bool is_string() const { return std::holds_alternative<std::string>(v_); }
    bool is_array()  const { return std::holds_alternative<JsonArray>(v_); }
    bool is_object() const { return std::holds_alternative<JsonObject>(v_); }

    bool                as_bool()   const { return std::get<bool>(v_); }
    double              as_number() const { return std::get<double>(v_); }
    const std::string&  as_string() const { return std::get<std::string>(v_); }
    const JsonArray&    as_array()  const { return std::get<JsonArray>(v_); }
    const JsonObject&   as_object() const { return std::get<JsonObject>(v_); }

    // Walk a dotted path through nested objects. Returns nullptr if any
    // segment is missing or hits a non-object.
    const JsonValue* find(std::string_view path) const;

    // Typed lookup with default fallback. Returns default_v on missing
    // key, wrong type, or out-of-range integer.
    template <typename T>
    T get_or(std::string_view path, T default_v) const;

    // Throws std::runtime_error with a 1-indexed line/col on syntax errors.
    static JsonValue parse(std::string_view text);

private:
    Variant v_;
};

// ──────────────────────────────────────────────────────────────────────
// Parser
// ──────────────────────────────────────────────────────────────────────

namespace detail {

class JsonParser {
public:
    explicit JsonParser(std::string_view t) : t_(t) {}

    JsonValue parse_root() {
        skip_ws();
        auto v = parse_value();
        skip_ws();
        if (i_ != t_.size())
            fail("trailing content after top-level value");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) {
        // Compute 1-indexed line/col for the error.
        std::size_t line = 1, col = 1;
        for (std::size_t k = 0; k < i_ && k < t_.size(); ++k) {
            if (t_[k] == '\n') { ++line; col = 1; } else { ++col; }
        }
        throw std::runtime_error("JSON parse error at line " +
                                 std::to_string(line) + ", col " +
                                 std::to_string(col) + ": " + msg);
    }

    void skip_ws() {
        while (i_ < t_.size()) {
            char c = t_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
            else break;
        }
    }

    char peek() {
        if (i_ >= t_.size()) fail("unexpected end of input");
        return t_[i_];
    }

    bool match(std::string_view kw) {
        if (i_ + kw.size() > t_.size()) return false;
        if (t_.compare(i_, kw.size(), kw) != 0) return false;
        i_ += kw.size();
        return true;
    }

    JsonValue parse_value() {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return parse_string();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        fail("unexpected character");
    }

    JsonValue parse_object() {
        ++i_;  // consume {
        JsonObject obj;
        skip_ws();
        if (i_ < t_.size() && t_[i_] == '}') { ++i_; return JsonValue(std::move(obj)); }
        while (true) {
            skip_ws();
            if (peek() != '"') fail("expected string key");
            JsonValue keyv = parse_string();
            skip_ws();
            if (peek() != ':') fail("expected ':' after key");
            ++i_;
            JsonValue val = parse_value();
            obj.emplace(keyv.as_string(), std::move(val));
            skip_ws();
            if (i_ < t_.size() && t_[i_] == ',') { ++i_; continue; }
            if (i_ < t_.size() && t_[i_] == '}') { ++i_; break; }
            fail("expected ',' or '}' in object");
        }
        return JsonValue(std::move(obj));
    }

    JsonValue parse_array() {
        ++i_;  // consume [
        JsonArray arr;
        skip_ws();
        if (i_ < t_.size() && t_[i_] == ']') { ++i_; return JsonValue(std::move(arr)); }
        while (true) {
            arr.push_back(parse_value());
            skip_ws();
            if (i_ < t_.size() && t_[i_] == ',') { ++i_; continue; }
            if (i_ < t_.size() && t_[i_] == ']') { ++i_; break; }
            fail("expected ',' or ']' in array");
        }
        return JsonValue(std::move(arr));
    }

    JsonValue parse_string() {
        ++i_;  // consume opening quote
        std::string out;
        while (i_ < t_.size()) {
            char c = t_[i_++];
            if (c == '"') return JsonValue(std::move(out));
            if (c == '\\') {
                if (i_ >= t_.size()) fail("bad escape");
                char e = t_[i_++];
                switch (e) {
                    case '"':  out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (i_ + 4 > t_.size()) fail("bad \\u escape");
                        // Decode 4 hex chars to a code point; emit UTF-8.
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = t_[i_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9')      cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else fail("bad hex in \\u escape");
                        }
                        if (cp < 0x80) {
                            out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: fail("unknown escape");
                }
            } else {
                out += c;
            }
        }
        fail("unterminated string");
    }

    JsonValue parse_bool() {
        if (match("true"))  return JsonValue(true);
        if (match("false")) return JsonValue(false);
        fail("invalid literal");
    }

    JsonValue parse_null() {
        if (match("null")) return JsonValue(nullptr);
        fail("invalid literal");
    }

    JsonValue parse_number() {
        std::size_t start = i_;
        if (i_ < t_.size() && t_[i_] == '-') ++i_;
        while (i_ < t_.size() && ((t_[i_] >= '0' && t_[i_] <= '9') ||
               t_[i_] == '.' || t_[i_] == 'e' || t_[i_] == 'E' ||
               t_[i_] == '+' || t_[i_] == '-'))
            ++i_;
        if (i_ == start) fail("expected number");
        try {
            return JsonValue(std::stod(std::string(t_.substr(start, i_ - start))));
        } catch (...) {
            fail("malformed number");
        }
    }

    std::string_view t_;
    std::size_t      i_ = 0;
};

}  // namespace detail

inline JsonValue JsonValue::parse(std::string_view text) {
    return detail::JsonParser(text).parse_root();
}

inline const JsonValue* JsonValue::find(std::string_view path) const {
    const JsonValue* cur = this;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        std::size_t dot = path.find('.', pos);
        std::string_view seg = path.substr(pos, dot - pos);
        if (!cur->is_object()) return nullptr;
        const auto& obj = cur->as_object();
        auto it = obj.find(std::string(seg));
        if (it == obj.end()) return nullptr;
        cur = &it->second;
        if (dot == std::string_view::npos) break;
        pos = dot + 1;
    }
    return cur;
}

// Single primary template with if-constexpr branches. Avoids the
// std::size_t / std::uint64_t alias collision on 64-bit Linux, where
// they're the same type and explicit specializations would conflict.
template <typename T>
inline T JsonValue::get_or(std::string_view path, T default_v) const {
    const JsonValue* p = find(path);
    if (!p) return default_v;
    if constexpr (std::is_same_v<T, bool>) {
        return p->is_bool() ? p->as_bool() : default_v;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return p->is_string() ? p->as_string() : default_v;
    } else if constexpr (std::is_arithmetic_v<T>) {
        if (!p->is_number()) return default_v;
        double x = p->as_number();
        if constexpr (std::is_unsigned_v<T>) {
            if (x < 0) return default_v;
        }
        return static_cast<T>(x);
    } else {
        static_assert(!sizeof(T*), "JsonValue::get_or: unsupported type");
    }
}

}  // namespace pec
