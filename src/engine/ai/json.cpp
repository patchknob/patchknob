#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace PatchKnob { namespace ai {

const Json& Json::null() { static const Json n; return n; }

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return null();
    std::map<std::string, Json>::const_iterator it = obj_.find(key);
    return it == obj_.end() ? null() : it->second;
}

const Json& Json::operator[](size_t i) const {
    if (type_ != Type::Array || i >= arr_.size()) return null();
    return arr_[i];
}

//  UTF-8 encode one code point (used by \uXXXX unescaping, surrogate pairs
//  included -- the API escapes non-ASCII that way and Csound sources can carry
//  it in comments).
static void utf8Append(std::string& out, unsigned int cp) {
    if (cp < 0x80) { out += (char)cp; }
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

struct JsonParser {
    const std::string& t;
    size_t p = 0;
    std::string err;

    explicit JsonParser(const std::string& text) : t(text) {}

    void ws() {
        while (p < t.size() && (t[p]==' '||t[p]=='\t'||t[p]=='\n'||t[p]=='\r')) ++p;
    }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }

    bool value(Json& out) {
        ws();
        if (p >= t.size()) return fail("unexpected end of input");
        switch (t[p]) {
            case '{': return object(out);
            case '[': return array(out);
            case '"': {
                std::string s;
                if (!string(s)) return false;
                out.type_ = Json::Type::String; out.s_ = s; return true;
            }
            case 't':
                if (t.compare(p, 4, "true") != 0) return fail("bad literal");
                p += 4; out.type_ = Json::Type::Bool; out.b_ = true; return true;
            case 'f':
                if (t.compare(p, 5, "false") != 0) return fail("bad literal");
                p += 5; out.type_ = Json::Type::Bool; out.b_ = false; return true;
            case 'n':
                if (t.compare(p, 4, "null") != 0) return fail("bad literal");
                p += 4; out.type_ = Json::Type::Null; return true;
            default:  return number(out);
        }
    }

    bool number(Json& out) {
        const size_t start = p;
        if (p < t.size() && (t[p]=='-'||t[p]=='+')) ++p;
        bool any = false;
        while (p < t.size() && ((t[p]>='0'&&t[p]<='9')||t[p]=='.'||t[p]=='e'||
                                t[p]=='E'||t[p]=='-'||t[p]=='+')) { ++p; any = true; }
        if (!any) return fail("expected a value");
        out.type_ = Json::Type::Number;
        out.n_ = std::strtod(t.substr(start, p - start).c_str(), nullptr);
        return true;
    }

    bool string(std::string& out) {
        if (p >= t.size() || t[p] != '"') return fail("expected a string");
        ++p;
        out.clear();
        while (p < t.size()) {
            const char c = t[p];
            if (c == '"') { ++p; return true; }
            if (c != '\\') { out += c; ++p; continue; }
            if (++p >= t.size()) return fail("truncated escape");
            const char e = t[p++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (p + 4 > t.size()) return fail("truncated \\u escape");
                    unsigned int cp = (unsigned int)std::strtoul(
                        t.substr(p, 4).c_str(), nullptr, 16);
                    p += 4;
                    //  A high surrogate must be joined with the low one that
                    //  follows, or the text comes out as two broken glyphs.
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= t.size() &&
                        t[p] == '\\' && t[p+1] == 'u') {
                        const unsigned int lo = (unsigned int)std::strtoul(
                            t.substr(p + 2, 4).c_str(), nullptr, 16);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    utf8Append(out, cp);
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    bool array(Json& out) {
        ++p;  // '['
        out.type_ = Json::Type::Array;
        ws();
        if (p < t.size() && t[p] == ']') { ++p; return true; }
        for (;;) {
            Json v;
            if (!value(v)) return false;
            out.arr_.push_back(v);
            ws();
            if (p < t.size() && t[p] == ',') { ++p; continue; }
            if (p < t.size() && t[p] == ']') { ++p; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool object(Json& out) {
        ++p;  // '{'
        out.type_ = Json::Type::Object;
        ws();
        if (p < t.size() && t[p] == '}') { ++p; return true; }
        for (;;) {
            ws();
            std::string key;
            if (!string(key)) return false;
            ws();
            if (p >= t.size() || t[p] != ':') return fail("expected ':'");
            ++p;
            Json v;
            if (!value(v)) return false;
            out.obj_[key] = v;
            ws();
            if (p < t.size() && t[p] == ',') { ++p; continue; }
            if (p < t.size() && t[p] == '}') { ++p; return true; }
            return fail("expected ',' or '}'");
        }
    }
};

Json Json::parse(const std::string& text, std::string* error) {
    JsonParser ps(text);
    Json out;
    if (!ps.value(out)) {
        if (error) *error = ps.err.empty() ? "malformed JSON" : ps.err;
        return Json();
    }
    if (error) error->clear();
    return out;
}

std::string Json::quote(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 2);
    out += '"';
    for (size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c = (unsigned char)raw[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    //  Control characters are illegal raw in a JSON string.
                    //  Csound sources really do contain them (a stray \v in a
                    //  comment), and an unescaped one makes the API reject the
                    //  whole request with a parse error.
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += (char)c;   // UTF-8 passes through untouched
                }
        }
    }
    out += '"';
    return out;
}

}} // namespace PatchKnob::ai
