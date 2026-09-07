//----------------------------------------------------------------------------
//  PatchKnob — minimal JSON reader/writer for the Claude Messages API.
//
//  Deliberately small: this exists to build one request body and to take apart
//  one streaming response, not to be a general JSON library.  It is a real
//  recursive-descent parser rather than a regex/substring hack, because the
//  text coming back is model-authored Csound code full of quotes, braces and
//  backslashes -- exactly the input that breaks a sloppy extractor.
//----------------------------------------------------------------------------
#pragma once
#include <map>
#include <string>
#include <vector>

namespace PatchKnob { namespace ai {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static Json parse(const std::string& text, std::string* error = nullptr);

    Type type() const { return type_; }
    bool isNull()   const { return type_ == Type::Null; }
    bool isString() const { return type_ == Type::String; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    //! Object member, or a Null Json if absent / not an object.  Never throws,
    //! so a malformed or unexpected response reads as "field missing" rather
    //! than taking the app down.
    const Json& operator[](const std::string& key) const;
    //! Array element, or Null if out of range / not an array.
    const Json& operator[](size_t i) const;

    size_t size() const { return type_ == Type::Array ? arr_.size() : 0; }

    std::string str(const std::string& fallback = std::string()) const {
        return type_ == Type::String ? s_ : fallback;
    }
    double num(double fallback = 0.0) const {
        return type_ == Type::Number ? n_ : fallback;
    }
    bool boolean(bool fallback = false) const {
        return type_ == Type::Bool ? b_ : fallback;
    }

    // --- writing -------------------------------------------------------------
    //! JSON string literal, quotes included, with control characters escaped.
    static std::string quote(const std::string& raw);

private:
    Type                          type_ = Type::Null;
    bool                          b_    = false;
    double                        n_    = 0.0;
    std::string                   s_;
    std::vector<Json>             arr_;
    std::map<std::string, Json>   obj_;

    friend struct JsonParser;
    static const Json& null();
};

}} // namespace PatchKnob::ai
