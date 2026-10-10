#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

const Json* Json::get(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : o)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

void Json::set(const std::string& key, Json value) {
    if (type != Type::Object) { *this = object(); }
    for (auto& kv : o)
        if (kv.first == key) { kv.second = std::move(value); return; }
    o.emplace_back(key, std::move(value));
}

std::string Json::str(const std::string& key, const std::string& fallback) const {
    const Json* v = get(key);
    if (!v) return fallback;
    if (v->type == Type::String) return v->s;
    if (v->type == Type::Number) {
        char buf[64];
        if (std::floor(v->n) == v->n && std::fabs(v->n) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", v->n);
        else std::snprintf(buf, sizeof buf, "%g", v->n);
        return buf;
    }
    return fallback;
}

double Json::num(const std::string& key, double fallback) const {
    const Json* v = get(key);
    if (!v) return fallback;
    if (v->type == Type::Number) return v->n;
    if (v->type == Type::String && !v->s.empty()) {
        char* end = nullptr;
        double d = std::strtod(v->s.c_str(), &end);
        if (end && *end == '\0') return d;
    }
    return fallback;
}

bool Json::flag(const std::string& key, bool fallback) const {
    const Json* v = get(key);
    if (!v) return fallback;
    if (v->type == Type::Bool) return v->b;
    if (v->type == Type::Number) return v->n != 0.0;
    return fallback;
}

static void dumpString(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

static void dumpValue(const Json& j, std::string& out, int indent, int depth) {
    auto newline = [&](int d) {
        if (indent <= 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent) * static_cast<size_t>(d), ' ');
    };
    switch (j.type) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += j.b ? "true" : "false"; break;
    case Json::Type::Number: {
        char buf[64];
        if (!std::isfinite(j.n)) { out += "null"; break; }
        if (std::floor(j.n) == j.n && std::fabs(j.n) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", j.n);
        else std::snprintf(buf, sizeof buf, "%.10g", j.n);
        out += buf;
        break;
    }
    case Json::Type::String: dumpString(j.s, out); break;
    case Json::Type::Array:
        out += '[';
        for (size_t i = 0; i < j.a.size(); ++i) {
            if (i) out += ',';
            newline(depth + 1);
            dumpValue(j.a[i], out, indent, depth + 1);
        }
        if (!j.a.empty()) newline(depth);
        out += ']';
        break;
    case Json::Type::Object:
        out += '{';
        for (size_t i = 0; i < j.o.size(); ++i) {
            if (i) out += ',';
            newline(depth + 1);
            dumpString(j.o[i].first, out);
            out += indent > 0 ? ": " : ":";
            dumpValue(j.o[i].second, out, indent, depth + 1);
        }
        if (!j.o.empty()) newline(depth);
        out += '}';
        break;
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpValue(*this, out, indent, 0);
    return out;
}

namespace {

struct Parser {
    const std::string& t;
    size_t i = 0;
    int depth = 0;

    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i; }

    static void utf8(unsigned cp, std::string& out) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& v) {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool str(std::string& out) {
        if (i >= t.size() || t[i] != '"') return false;
        ++i;
        while (i < t.size()) {
            char c = t[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= t.size()) return false;
            char e = t[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                    i += 2;
                    unsigned lo = 0;
                    if (!hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(cp, out);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool value(Json& out) {
        if (++depth > 64) return false;
        ws();
        if (i >= t.size()) return false;
        char c = t[i];
        bool ok = false;
        if (c == '{') {
            out = Json::object();
            ++i;
            ws();
            if (i < t.size() && t[i] == '}') { ++i; ok = true; }
            else {
                for (;;) {
                    ws();
                    std::string key;
                    if (!str(key)) break;
                    ws();
                    if (i >= t.size() || t[i] != ':') break;
                    ++i;
                    Json v;
                    if (!value(v)) break;
                    out.o.emplace_back(std::move(key), std::move(v));
                    ws();
                    if (i < t.size() && t[i] == ',') { ++i; continue; }
                    if (i < t.size() && t[i] == '}') { ++i; ok = true; }
                    break;
                }
            }
        } else if (c == '[') {
            out = Json::array();
            ++i;
            ws();
            if (i < t.size() && t[i] == ']') { ++i; ok = true; }
            else {
                for (;;) {
                    Json v;
                    if (!value(v)) break;
                    out.a.push_back(std::move(v));
                    ws();
                    if (i < t.size() && t[i] == ',') { ++i; continue; }
                    if (i < t.size() && t[i] == ']') { ++i; ok = true; }
                    break;
                }
            }
        } else if (c == '"') {
            out = Json::string("");
            ok = str(out.s);
        } else if (t.compare(i, 4, "true") == 0) { out = Json::boolean(true); i += 4; ok = true; }
        else if (t.compare(i, 5, "false") == 0) { out = Json::boolean(false); i += 5; ok = true; }
        else if (t.compare(i, 4, "null") == 0) { out = Json::null(); i += 4; ok = true; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = t.c_str() + i;
            char* end = nullptr;
            double d = std::strtod(start, &end);
            if (end && end != start) { i += static_cast<size_t>(end - start); out = Json::number(d); ok = true; }
        }
        --depth;
        return ok;
    }
};

}  // namespace

bool parseJson(const std::string& text, Json& out) {
    Parser p{text};
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        p.i = 3;
    Json v;
    if (!p.value(v)) return false;
    p.ws();
    if (p.i != text.size()) return false;
    out = std::move(v);
    return true;
}
