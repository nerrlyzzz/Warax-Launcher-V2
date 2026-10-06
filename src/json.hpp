// =====================================================================
//  json.hpp — компактная JSON-библиотека (nlohmann-совместимое подмножество).
//  Без внешних зависимостей. Покрывает только то, что использует лаунчер.
// =====================================================================
#pragma once
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nlohmann {

class json {
public:
    enum class value_t { null, object, array, string, boolean, number_integer, number_unsigned, number_float, discarded };
    enum class error_handler_t { strict, replace, ignore };

    using array_t = std::vector<json>;
    using object_t = std::vector<std::pair<std::string, json>>;

    value_t t = value_t::null;
    bool b = false;
    int64_t i = 0;
    uint64_t u = 0;
    double d = 0;
    std::string s;
    array_t arr;
    object_t obj;

    // ---- constructors
    json() = default;
    json(std::nullptr_t) : t(value_t::null) {}
    json(bool v) : t(value_t::boolean), b(v) {}
    json(int v) : t(value_t::number_integer), i(v) {}
    json(long v) : t(value_t::number_integer), i(v) {}
    json(long long v) : t(value_t::number_integer), i(v) {}
    json(unsigned v) : t(value_t::number_unsigned), u(v) {}
    json(unsigned long v) : t(value_t::number_unsigned), u(v) {}
    json(unsigned long long v) : t(value_t::number_unsigned), u(v) {}
    json(double v) : t(value_t::number_float), d(v) {}
    json(const char* v) : t(value_t::string), s(v ? v : "") {}
    json(const std::string& v) : t(value_t::string), s(v) {}
    json(std::string&& v) : t(value_t::string), s(std::move(v)) {}

    json(std::initializer_list<json> init) {
        bool isObj = init.size() > 0;
        for (const json& e : init)
            if (!(e.t == value_t::array && e.arr.size() == 2 && e.arr[0].t == value_t::string)) { isObj = false; break; }
        if (isObj) {
            t = value_t::object;
            for (const json& e : init) obj.emplace_back(e.arr[0].s, e.arr[1]);
        } else {
            t = value_t::array;
            arr.assign(init.begin(), init.end());
        }
    }

    static json array() { json j; j.t = value_t::array; return j; }
    static json object() { json j; j.t = value_t::object; return j; }

    // ---- type checks
    bool is_null() const { return t == value_t::null; }
    bool is_boolean() const { return t == value_t::boolean; }
    bool is_object() const { return t == value_t::object; }
    bool is_array() const { return t == value_t::array; }
    bool is_string() const { return t == value_t::string; }
    bool is_discarded() const { return t == value_t::discarded; }
    bool is_number() const { return t == value_t::number_integer || t == value_t::number_unsigned || t == value_t::number_float; }

    // ---- object access
    json* find(const std::string& key) {
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    const json* find(const std::string& key) const {
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool contains(const std::string& key) const { return find(key) != nullptr; }

    json& operator[](const std::string& key) {
        if (t != value_t::object) { t = value_t::object; obj.clear(); }
        if (json* p = find(key)) return *p;
        obj.emplace_back(key, json());
        return obj.back().second;
    }
    json& operator[](const char* key) { return (*this)[std::string(key)]; }

    json& at(const std::string& key) {
        json* p = (t == value_t::object) ? find(key) : nullptr;
        if (!p) throw std::runtime_error("json: key '" + key + "' not found");
        return *p;
    }
    const json& at(const std::string& key) const {
        const json* p = (t == value_t::object) ? find(key) : nullptr;
        if (!p) throw std::runtime_error("json: key '" + key + "' not found");
        return *p;
    }

    // ---- value() with defaults
    std::string value(const std::string& key, const char* def) const {
        const json* p = find(key);
        return (p && p->t == value_t::string) ? p->s : std::string(def);
    }
    std::string value(const std::string& key, const std::string& def) const {
        const json* p = find(key);
        return (p && p->t == value_t::string) ? p->s : def;
    }
    bool value(const std::string& key, bool def) const {
        const json* p = find(key);
        return (p && p->t == value_t::boolean) ? p->b : def;
    }
    int value(const std::string& key, int def) const {
        const json* p = find(key);
        return p ? (int)p->as_int() : def;
    }
    uint64_t value(const std::string& key, uint64_t def) const {
        const json* p = find(key);
        return p ? p->as_uint() : def;
    }
    json value(const std::string& key, const json& def) const {
        const json* p = find(key);
        return p ? *p : def;
    }

    int64_t as_int() const {
        if (t == value_t::number_integer) return i;
        if (t == value_t::number_unsigned) return (int64_t)u;
        if (t == value_t::number_float) return (int64_t)d;
        if (t == value_t::boolean) return b ? 1 : 0;
        return 0;
    }
    uint64_t as_uint() const {
        if (t == value_t::number_unsigned) return u;
        if (t == value_t::number_integer) return (uint64_t)i;
        if (t == value_t::number_float) return (uint64_t)d;
        return 0;
    }

    // ---- get<T>
    template <typename T> T get() const;

    // ---- array
    void push_back(const json& v) {
        if (t != value_t::array) { t = value_t::array; arr.clear(); }
        arr.push_back(v);
    }
    size_t size() const { return t == value_t::array ? arr.size() : t == value_t::object ? obj.size() : 0; }

    array_t::iterator begin() { return arr.begin(); }
    array_t::iterator end() { return arr.end(); }
    array_t::const_iterator begin() const { return arr.begin(); }
    array_t::const_iterator end() const { return arr.end(); }

    // ---- dump
    std::string dump(int indent = -1, char = ' ', bool = false, error_handler_t = error_handler_t::strict) const {
        std::string out;
        dumpTo(out, indent, 0);
        return out;
    }

    // ---- parse
    static json parse(const std::string& text, std::nullptr_t = nullptr, bool allow_exceptions = true) {
        Parser p{text};
        json result;
        bool ok = p.parseValue(result) && p.atEnd();
        if (!ok) {
            if (allow_exceptions) throw std::runtime_error("json parse error");
            json dj; dj.t = value_t::discarded; return dj;
        }
        return result;
    }

private:
    static void escape(std::string& out, const std::string& v) {
        out.push_back('"');
        for (unsigned char c : v) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* h = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(h[c >> 4]);
                    out.push_back(h[c & 15]);
                } else {
                    out.push_back((char)c);
                }
            }
        }
        out.push_back('"');
    }

    void dumpTo(std::string& out, int indent, int depth) const {
        auto nl = [&](int dd) {
            if (indent >= 0) { out.push_back('\n'); out.append((size_t)indent * dd, ' '); }
        };
        switch (t) {
        case value_t::null: out += "null"; break;
        case value_t::discarded: out += "null"; break;
        case value_t::boolean: out += b ? "true" : "false"; break;
        case value_t::number_integer: out += std::to_string(i); break;
        case value_t::number_unsigned: out += std::to_string(u); break;
        case value_t::number_float: {
            if (d != d || d == 1e400 || d == -1e400) { out += "null"; break; }
            char buf[32];
            snprintf(buf, sizeof(buf), "%.17g", d);
            out += buf;
            break;
        }
        case value_t::string: escape(out, s); break;
        case value_t::array:
            if (arr.empty()) { out += "[]"; break; }
            out.push_back('[');
            for (size_t k = 0; k < arr.size(); k++) {
                if (k) out.push_back(',');
                nl(depth + 1);
                arr[k].dumpTo(out, indent, depth + 1);
            }
            nl(depth);
            out.push_back(']');
            break;
        case value_t::object:
            if (obj.empty()) { out += "{}"; break; }
            out.push_back('{');
            for (size_t k = 0; k < obj.size(); k++) {
                if (k) out.push_back(',');
                nl(depth + 1);
                escape(out, obj[k].first);
                out.push_back(':');
                if (indent >= 0) out.push_back(' ');
                obj[k].second.dumpTo(out, indent, depth + 1);
            }
            nl(depth);
            out.push_back('}');
            break;
        }
    }

    struct Parser {
        const std::string& x;
        size_t p = 0;
        explicit Parser(const std::string& t) : x(t) {}
        void ws() {
            while (p < x.size() && (x[p] == ' ' || x[p] == '\t' || x[p] == '\n' || x[p] == '\r')) p++;
        }
        bool atEnd() { ws(); return p >= x.size(); }
        bool lit(const char* w, json& out, json v) {
            size_t n = 0;
            while (w[n]) n++;
            if (x.compare(p, n, w) == 0) { p += n; out = v; return true; }
            return false;
        }
        bool parseValue(json& out) {
            ws();
            if (p >= x.size()) return false;
            char c = x[p];
            if (c == '{') return parseObject(out);
            if (c == '[') return parseArray(out);
            if (c == '"') { std::string str; if (!parseString(str)) return false; out = std::move(str); return true; }
            if (c == 't') return lit("true", out, json(true));
            if (c == 'f') return lit("false", out, json(false));
            if (c == 'n') return lit("null", out, json(nullptr));
            if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
            return false;
        }
        bool parseObject(json& out) {
            out = json::object();
            p++;  // {
            ws();
            if (p < x.size() && x[p] == '}') { p++; return true; }
            for (;;) {
                ws();
                if (p >= x.size() || x[p] != '"') return false;
                std::string key;
                if (!parseString(key)) return false;
                ws();
                if (p >= x.size() || x[p] != ':') return false;
                p++;
                json v;
                if (!parseValue(v)) return false;
                out.obj.emplace_back(std::move(key), std::move(v));
                ws();
                if (p >= x.size()) return false;
                if (x[p] == ',') { p++; continue; }
                if (x[p] == '}') { p++; return true; }
                return false;
            }
        }
        bool parseArray(json& out) {
            out = json::array();
            p++;  // [
            ws();
            if (p < x.size() && x[p] == ']') { p++; return true; }
            for (;;) {
                json v;
                if (!parseValue(v)) return false;
                out.arr.push_back(std::move(v));
                ws();
                if (p >= x.size()) return false;
                if (x[p] == ',') { p++; continue; }
                if (x[p] == ']') { p++; return true; }
                return false;
            }
        }
        static void appendUtf8(std::string& o, unsigned cp) {
            if (cp <= 0x7F) o.push_back((char)cp);
            else if (cp <= 0x7FF) { o.push_back((char)(0xC0 | (cp >> 6))); o.push_back((char)(0x80 | (cp & 0x3F))); }
            else if (cp <= 0xFFFF) { o.push_back((char)(0xE0 | (cp >> 12))); o.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); o.push_back((char)(0x80 | (cp & 0x3F))); }
            else { o.push_back((char)(0xF0 | (cp >> 18))); o.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); o.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); o.push_back((char)(0x80 | (cp & 0x3F))); }
        }
        int hex4(size_t at) {
            if (at + 4 > x.size()) return -1;
            int v = 0;
            for (int k = 0; k < 4; k++) {
                char c = x[at + k];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= c - '0';
                else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
                else return -1;
            }
            return v;
        }
        bool parseString(std::string& out) {
            p++;  // opening quote
            while (p < x.size()) {
                char c = x[p++];
                if (c == '"') return true;
                if (c == '\\') {
                    if (p >= x.size()) return false;
                    char e = x[p++];
                    switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        int cp = hex4(p);
                        if (cp < 0) return false;
                        p += 4;
                        if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < x.size() && x[p] == '\\' && x[p + 1] == 'u') {
                            int lo = hex4(p + 2);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                p += 6;
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                        }
                        appendUtf8(out, (unsigned)cp);
                        break;
                    }
                    default: return false;
                    }
                } else {
                    out.push_back(c);
                }
            }
            return false;
        }
        bool parseNumber(json& out) {
            size_t start = p;
            bool isFloat = false;
            if (p < x.size() && x[p] == '-') p++;
            while (p < x.size() && x[p] >= '0' && x[p] <= '9') p++;
            if (p < x.size() && x[p] == '.') { isFloat = true; p++; while (p < x.size() && x[p] >= '0' && x[p] <= '9') p++; }
            if (p < x.size() && (x[p] == 'e' || x[p] == 'E')) {
                isFloat = true; p++;
                if (p < x.size() && (x[p] == '+' || x[p] == '-')) p++;
                while (p < x.size() && x[p] >= '0' && x[p] <= '9') p++;
            }
            std::string num = x.substr(start, p - start);
            if (num.empty() || num == "-") return false;
            try {
                if (isFloat) { out = json(std::stod(num)); }
                else if (num[0] == '-') { out = json((long long)std::stoll(num)); }
                else { out = json((unsigned long long)std::stoull(num)); }
            } catch (...) { return false; }
            return true;
        }
    };
};

template <> inline std::string json::get<std::string>() const {
    if (t != value_t::string) throw std::runtime_error("json: not a string");
    return s;
}
template <> inline bool json::get<bool>() const { return t == value_t::boolean ? b : false; }
template <> inline int json::get<int>() const { return (int)as_int(); }
template <> inline int64_t json::get<int64_t>() const { return as_int(); }
template <> inline uint64_t json::get<uint64_t>() const { return as_uint(); }
template <> inline double json::get<double>() const { return t == value_t::number_float ? d : (double)as_int(); }

}  // namespace nlohmann
