// What the generated source emitters (gen/vk_emit.gen.cpp) spell values with, for Export to C++:
// numbers, enum and flag names from the decoder's tables, handles as the variables the exporter
// named them, blobs as offsets into the exported project's data file, and locals declared ahead
// of the statement being built. The exporter (exporter.h) owns one per section of the project.
#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "decode.h"

namespace vkreplay {

class SourceWriter {
public:
    /** The variable spelling a handle of `type` ("VkImage", aliases resolved), or empty when it has none. */
    std::function<std::string(const char* type, uint64_t handle)> handle;
    /** The expression for a blob's bytes in the exported project: the exporter appends them to its data file. */
    std::function<std::string(const void* data, size_t size)> data;
    /** The expression for a device address of the replaying process, or empty to spell the number. */
    std::function<std::string(uint64_t address)> address;

    /** The expression a vkCmd* is recorded into (the current command buffer's variable). */
    std::string cb = "cb";
    /** The statement depth, in 4-space steps, that Line writes at and nested initializers indent from. */
    int indent = 1;
    /** The text so far. */
    std::string text;
    /** Vulkan functions the text calls, for the exported project's function table. */
    std::set<std::string> used;
    /** Handles without a name, structs without an emitter: what the source could not spell. */
    std::vector<std::string> notes;

    void Line(const std::string& statement);
    void Comment(const std::string& text);
    void Note(const std::string& text);
    void Use(const char* function) { used.insert(function); }
    /** A fresh local variable name from a stem (attachments -> attachments_3). */
    std::string Local(const char* hint);
    /** Lines of text so far, which is what a part of the project is cut by. */
    size_t Lines() const { return _lines; }
    /** Statements written with Line: one of them may be an initializer of many lines. */
    size_t Statements() const { return _statements; }
    /** Starts a new part: the text so far has been taken, and local names may start over. */
    void ResetPart();
    /** Local names start over: what follows is a block of its own. */
    void ResetLocals() { _locals = 0; }
    /** An empty line. */
    void Blank() { text += "\n"; }
    /** Takes a scratch writer's lines as they are, with the functions it used and its notes. */
    void Append(const SourceWriter& other);

    std::string Enum(const EnumEntry* table, size_t count, int64_t value, const char* type) const;
    std::string EnumNumber(int64_t value, const char* type) const;
    std::string Flags(const EnumEntry* table, size_t count, uint64_t value, const char* type) const;
    std::string Handle(const char* type, uint64_t value);
    std::string Address(uint64_t value);
    std::string Bytes(const void* bytes, size_t size);
    /** A quoted string literal, or nullptr. */
    static std::string String(const char* s);
    static std::string FixedString(const char* s, size_t size);
    /** An array of strings as a local, or nullptr. */
    std::string Strings(const char* hint, const char* const* strings, size_t count);
    /** A trailing comment reading `count` floats, for the bits of a clear colour. */
    static std::string FloatComment(const float* values, size_t count);
    /** A pNext expression, cast for a non-const pNext member. */
    static std::string PNextCast(const std::string& expr, bool constMember);
    static std::string Bool(VkBool32 v) { return v ? "VK_TRUE" : "VK_FALSE"; }
    static std::string Uint(uint64_t v);
    static std::string Int(int64_t v);
    static std::string Float(float v);
    static std::string Double(double v);

    /** Scalar arrays longer than this are put in the data file rather than spelled out. */
    static constexpr size_t kInlineArrayLimit = 256;

private:
    unsigned _locals = 0;
    size_t _lines = 0;
    size_t _statements = 0;
};

/** Declares `const Type name = expr;` before the current statement and returns the name. */
std::string EmitLocal(SourceWriter& w, const char* type, const char* hint, const std::string& expr);
/** Declares `const Type name[] = {items};` and returns the name. */
std::string EmitArrayLocal(SourceWriter& w, const char* type, const char* hint, const std::string& items);

/** An array of structs as a local array (elements spelled by `element`), or nullptr when empty. */
template <typename T, typename F>
std::string EmitStructArray(SourceWriter& w, const char* hint, const char* type, const T* items, size_t count, F element) {
    if (!items || !count) return "nullptr";
    std::string body;
    const std::string pad((size_t)(w.indent + 1) * 4, ' ');
    for (size_t i = 0; i < count; ++i) body += pad + element(items[i]) + ",\n";
    return EmitArrayLocal(w, type, hint, "\n" + body + std::string((size_t)w.indent * 4, ' '));
}

/** An array of scalars, enums or handles as a local array; a long one goes to the data file instead. */
template <typename T, typename F>
std::string EmitScalarArray(SourceWriter& w, const char* hint, const char* type, const T* items, size_t count, F element) {
    if (!items || !count) return "nullptr";
    if (count > SourceWriter::kInlineArrayLimit) return "(const " + std::string(type) + "*)" + w.Bytes(items, count * sizeof(T));
    std::string body;
    for (size_t i = 0; i < count; ++i) body += (i ? ", " : "") + element(items[i]);
    return EmitArrayLocal(w, type, hint, body);
}

/** An array of handles as a local array, however long: a handle is a run-time value, so it cannot come from the data file. */
template <typename T, typename F>
std::string EmitHandleArray(SourceWriter& w, const char* hint, const char* type, const T* items, size_t count, F element) {
    if (!items || !count) return "nullptr";
    std::string body;
    for (size_t i = 0; i < count; ++i) body += (i ? ", " : "") + element(items[i]);
    return EmitArrayLocal(w, type, hint, body);
}

/** An array of pointers to structs as a local array of pointers, each a local of its own. */
template <typename T, typename F>
std::string EmitPointerArray(SourceWriter& w, const char* hint, const char* type, const T* const* items, size_t count, F element) {
    if (!items || !count) return "nullptr";
    std::string body;
    for (size_t i = 0; i < count; ++i) body += (i ? ", " : "") + element(items[i]);
    return EmitArrayLocal(w, (std::string("const ") + type + "*").c_str(), hint, body);
}

} // namespace vkreplay
