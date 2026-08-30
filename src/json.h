#pragma once
// A small JSON reader and writer.
//
// The settings file has to survive being looked at and edited by hand, which
// rules out a binary blob, and it is the only thing in this program that needs
// a parser at all - so it gets a few hundred lines here rather than a library
// and a build step.
//
// What it does not do: comments, exponents in awkward places, or duplicate
// keys. What it does do: read back exactly what it wrote, and refuse the rest
// without crashing, which is the whole requirement.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace vp::json {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

class Value {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(bool b) : kind_(Kind::Bool), number_(b ? 1 : 0) {}
    Value(double d) : kind_(Kind::Number), number_(d) {}
    Value(int i) : kind_(Kind::Number), number_(i) {}
    Value(const char* s) : kind_(Kind::String), string_(s) {}
    Value(std::string s) : kind_(Kind::String), string_(std::move(s)) {}
    Value(Array a) : kind_(Kind::Array), array_(std::move(a)) {}
    Value(Object o) : kind_(Kind::Object), object_(std::move(o)) {}

    Kind kind() const { return kind_; }
    bool isObject() const { return kind_ == Kind::Object; }
    bool isArray() const { return kind_ == Kind::Array; }

    // Readers that never throw: a missing or wrong-typed field yields the
    // fallback, so a half-written settings file costs a preference and not a
    // start-up.
    bool asBool(bool fallback = false) const {
        return kind_ == Kind::Bool || kind_ == Kind::Number ? number_ != 0.0 : fallback;
    }
    double asNumber(double fallback = 0.0) const {
        return kind_ == Kind::Number || kind_ == Kind::Bool ? number_ : fallback;
    }
    int asInt(int fallback = 0) const {
        return kind_ == Kind::Number || kind_ == Kind::Bool
                   ? static_cast<int>(number_ + (number_ < 0 ? -0.5 : 0.5))
                   : fallback;
    }
    std::string asString(const std::string& fallback = {}) const {
        return kind_ == Kind::String ? string_ : fallback;
    }
    const Array& asArray() const {
        static const Array empty;
        return kind_ == Kind::Array ? array_ : empty;
    }
    const Object& asObject() const {
        static const Object empty;
        return kind_ == Kind::Object ? object_ : empty;
    }

    const Value& operator[](const std::string& key) const {
        static const Value null;
        if (kind_ != Kind::Object) return null;
        auto it = object_.find(key);
        return it == object_.end() ? null : it->second;
    }

    Array& array() {
        kind_ = Kind::Array;
        return array_;
    }
    Object& object() {
        kind_ = Kind::Object;
        return object_;
    }

    void set(const std::string& key, Value v) {
        kind_ = Kind::Object;
        object_[key] = std::move(v);
    }

    std::string dump(int indent = 0) const {
        std::ostringstream out;
        write(out, indent);
        return out.str();
    }

private:
    void write(std::ostringstream& out, int indent, int depth = 0) const;

    Kind kind_ = Kind::Null;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

inline void escape(std::ostringstream& out, const std::string& s) {
    out << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x", c);
                    out << buf;
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    out << '"';
}

inline void Value::write(std::ostringstream& out, int indent, int depth) const {
    const std::string pad(static_cast<size_t>(indent * (depth + 1)), ' ');
    const std::string padEnd(static_cast<size_t>(indent * depth), ' ');
    const char* nl = indent ? "\n" : "";

    switch (kind_) {
        case Kind::Null: out << "null"; break;
        case Kind::Bool: out << (number_ != 0.0 ? "true" : "false"); break;
        case Kind::Number: {
            // %.10g rather than the default: it round-trips a double that came
            // from a slider without printing seventeen digits of noise.
            char buf[40];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.10g", number_);
            out << buf;
            break;
        }
        case Kind::String: escape(out, string_); break;
        case Kind::Array: {
            if (array_.empty()) { out << "[]"; break; }
            out << '[' << nl;
            for (size_t i = 0; i < array_.size(); ++i) {
                out << pad;
                array_[i].write(out, indent, depth + 1);
                if (i + 1 < array_.size()) out << ',';
                out << nl;
            }
            out << padEnd << ']';
            break;
        }
        case Kind::Object: {
            if (object_.empty()) { out << "{}"; break; }
            out << '{' << nl;
            size_t i = 0;
            for (const auto& [key, value] : object_) {
                out << pad;
                escape(out, key);
                out << ':' << (indent ? " " : "");
                value.write(out, indent, depth + 1);
                if (++i < object_.size()) out << ',';
                out << nl;
            }
            out << padEnd << '}';
            break;
        }
    }
}

// ---------------------------------------------------------------- parsing

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Value parse() {
        skip();
        Value v = value();
        return failed_ ? Value{} : v;
    }

private:
    void skip() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' ||
                                      text_[at_] == '\n' || text_[at_] == '\r')) {
            ++at_;
        }
    }
    bool eat(char c) {
        skip();
        if (at_ < text_.size() && text_[at_] == c) { ++at_; return true; }
        return false;
    }
    bool literal(const char* word) {
        const size_t n = strlen(word);
        if (text_.compare(at_, n, word) != 0) return false;
        at_ += n;
        return true;
    }

    Value value() {
        skip();
        if (at_ >= text_.size()) { failed_ = true; return {}; }
        switch (text_[at_]) {
            case '{': return objectValue();
            case '[': return arrayValue();
            case '"': return Value{stringValue()};
            case 't': return literal("true") ? Value{true} : (failed_ = true, Value{});
            case 'f': return literal("false") ? Value{false} : (failed_ = true, Value{});
            case 'n': return literal("null") ? Value{} : (failed_ = true, Value{});
            default: return numberValue();
        }
    }

    Value objectValue() {
        Value v;
        v.object();
        eat('{');
        if (eat('}')) return v;
        while (!failed_) {
            skip();
            if (at_ >= text_.size() || text_[at_] != '"') { failed_ = true; break; }
            const std::string key = stringValue();
            if (!eat(':')) { failed_ = true; break; }
            v.set(key, value());
            if (eat(',')) continue;
            if (eat('}')) break;
            failed_ = true;
        }
        return v;
    }

    Value arrayValue() {
        Value v;
        Array& items = v.array();
        eat('[');
        if (eat(']')) return v;
        while (!failed_) {
            items.push_back(value());
            if (eat(',')) continue;
            if (eat(']')) break;
            failed_ = true;
        }
        return v;
    }

    std::string stringValue() {
        std::string out;
        ++at_;  // opening quote
        while (at_ < text_.size() && text_[at_] != '"') {
            char c = text_[at_++];
            if (c != '\\') { out += c; continue; }
            if (at_ >= text_.size()) break;
            switch (text_[at_++]) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    // Only the plane this program writes: escapes above 0x7f
                    // come out as UTF-8, and a surrogate pair is left alone.
                    if (at_ + 4 > text_.size()) { failed_ = true; break; }
                    const unsigned code =
                        static_cast<unsigned>(strtoul(text_.substr(at_, 4).c_str(), nullptr, 16));
                    at_ += 4;
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: out += text_[at_ - 1];
            }
        }
        if (at_ < text_.size()) ++at_;  // closing quote
        return out;
    }

    Value numberValue() {
        const size_t start = at_;
        while (at_ < text_.size() && (isdigit(static_cast<unsigned char>(text_[at_])) ||
                                      text_[at_] == '-' || text_[at_] == '+' ||
                                      text_[at_] == '.' || text_[at_] == 'e' ||
                                      text_[at_] == 'E')) {
            ++at_;
        }
        if (at_ == start) { failed_ = true; return {}; }
        return Value{atof(text_.substr(start, at_ - start).c_str())};
    }

    const std::string& text_;
    size_t at_ = 0;
    bool failed_ = false;
};

inline Value parse(const std::string& text) { return Parser(text).parse(); }

}  // namespace vp::json
