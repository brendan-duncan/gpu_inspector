// A JSON DOM for capture manifests: one pass over the text into an arena, strings pointing into
// the input where they need no unescaping, numbers kept as their text and converted on use, so
// 64-bit integers stay exact (the layer writes values above 2^53 as decimal strings, which Uint()
// also reads). The input must outlive the document.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "arena.h"

namespace vkreplay {

enum class JType : uint8_t { Null, Bool, Number, String, Array, Object };

struct JMember;

struct JValue {
    JType type = JType::Null;
    bool boolean = false;
    uint32_t count = 0;            // elements of an array, members of an object
    const char* text = nullptr;    // string contents, or a number's text
    size_t length = 0;
    JValue* items = nullptr;       // arrays
    JMember* members = nullptr;    // objects

    bool IsNull() const { return type == JType::Null; }
    bool IsBool() const { return type == JType::Bool; }
    bool IsNumber() const { return type == JType::Number; }
    bool IsString() const { return type == JType::String; }
    bool IsArray() const { return type == JType::Array; }
    bool IsObject() const { return type == JType::Object; }

    std::string_view Str() const { return type == JType::String ? std::string_view(text, length) : std::string_view(); }
    /** An object's member, or null (also for non-objects). */
    const JValue* Get(std::string_view key) const;
    /** A number (or a numeric string) as an integer; 0 otherwise. */
    int64_t Int() const;
    uint64_t Uint() const;
    double Double() const;
};

struct JMember {
    std::string_view key;
    JValue value;
};

class JsonDocument {
public:
    /** Parses `size` bytes of `data`; false with a message (and the byte offset) on malformed input. */
    bool Parse(const char* data, size_t size, std::string& error);
    const JValue& Root() const { return _root; }

private:
    bool ParseValue(JValue& out, int depth);
    bool ParseString(const char*& text, size_t& length);
    void SkipSpace();
    bool Fail(const char* message);

    Arena _arena{16 << 20};
    JValue _root;
    const char* _pos = nullptr;
    const char* _end = nullptr;
    const char* _begin = nullptr;
    std::string* _error = nullptr;
    std::vector<JValue> _items;     // pending array elements, shared by every nesting level
    std::vector<JMember> _members;  // pending object members
};

} // namespace vkreplay
