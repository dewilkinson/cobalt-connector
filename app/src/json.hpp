// A small JSON value: enough for limits.json, contributor.json and status.json. Keeps key order.
#pragma once
#include <string>
#include <utility>
#include <vector>

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double n = 0.0;
    std::string s;                                       // UTF-8
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    static Json null() { return {}; }
    static Json boolean(bool v) { Json j; j.type = Type::Bool; j.b = v; return j; }
    static Json number(double v) { Json j; j.type = Type::Number; j.n = v; return j; }
    static Json string(std::string v) { Json j; j.type = Type::String; j.s = std::move(v); return j; }
    static Json object() { Json j; j.type = Type::Object; return j; }
    static Json array() { Json j; j.type = Type::Array; return j; }

    bool isObject() const { return type == Type::Object; }
    const Json* get(const std::string& key) const;       // nullptr when absent or not an object
    void set(const std::string& key, Json value);        // makes this an object if it is not one

    // conveniences for reading a field with a fallback
    std::string str(const std::string& key, const std::string& fallback = "") const;
    double num(const std::string& key, double fallback = 0.0) const;
    bool flag(const std::string& key, bool fallback = false) const;

    std::string dump(int indent = 2) const;
};

// Parses UTF-8 text (a leading byte order mark is skipped). Returns false on malformed input.
bool parseJson(const std::string& text, Json& out);
