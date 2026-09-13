#include "xfb_patch.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <utility>

namespace vkreplay {

namespace {

// Opcodes, decorations and enums the edit reads or writes (the SPIR-V specification, section 3).
enum : uint32_t {
    OpName = 5, OpMemberName = 6, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17,
    OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24, OpTypeArray = 28,
    OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpVariable = 59,
    OpDecorate = 71, OpMemberDecorate = 72, OpDecorationGroup = 73, OpGroupDecorate = 74, OpGroupMemberDecorate = 75,
    OpExecutionModeId = 331, OpDecorateId = 332, OpDecorateString = 5632, OpMemberDecorateString = 5633,
};
enum : uint32_t { DecorationBlock = 2, DecorationBuiltIn = 11, DecorationLocation = 30, DecorationOffset = 35, DecorationXfbBuffer = 36, DecorationXfbStride = 37 };
constexpr uint32_t BuiltInPosition = 0;
constexpr uint32_t CapabilityTransformFeedback = 53;
constexpr uint32_t ExecutionModeXfb = 11;
constexpr uint32_t StorageClassOutput = 3;
constexpr uint32_t ExecutionModelVertex = 0;

struct Type {
    uint32_t op = 0;
    uint32_t width = 0;      // int, float
    bool isSigned = false;   // int
    uint32_t element = 0;    // vector, matrix, array: the element type; pointer: the pointee
    uint32_t count = 0;      // vector, matrix: the count; array: the length constant's id
    uint32_t storage = 0;    // pointer
    std::vector<uint32_t> members;
};

/** A literal string at `at`, and how many words it takes. */
std::string LiteralString(const uint32_t* w, size_t words, size_t& used) {
    std::string s;
    for (size_t i = 0; i < words; ++i) {
        for (int b = 0; b < 4; ++b) {
            const char c = (char)((w[i] >> (8 * b)) & 0xFF);
            if (!c) {
                used = i + 1;
                return s;
            }
            s += c;
        }
    }
    used = words;
    return s;
}

struct Flat {
    bool ok = false;
    uint32_t bytes = 0;
    uint32_t components = 0;
    std::string base;
};

bool IsAnnotation(uint32_t op) {
    return op == OpDecorate || op == OpMemberDecorate || op == OpDecorationGroup || op == OpGroupDecorate ||
           op == OpGroupMemberDecorate || op == OpDecorateId || op == OpDecorateString || op == OpMemberDecorateString;
}

void Emit(std::vector<uint32_t>& out, uint32_t op, std::initializer_list<uint32_t> operands) {
    out.push_back(((uint32_t)operands.size() + 1) << 16 | op);
    out.insert(out.end(), operands.begin(), operands.end());
}

} // namespace

XfbPatch PatchForTransformFeedback(const uint32_t* words, size_t count, const std::string& entryPoint) {
    XfbPatch patch;
    if (!words || count < 5 || words[0] != 0x07230203) {
        patch.error = "not a SPIR-V module";
        return patch;
    }

    std::map<uint32_t, Type> types;
    std::map<uint32_t, uint32_t> constants;
    std::map<uint32_t, uint32_t> variables;                            // id -> pointer type
    std::map<uint32_t, std::string> names;
    std::map<std::pair<uint32_t, uint32_t>, std::string> memberNames;
    std::map<uint32_t, uint32_t> builtins;                             // variable -> BuiltIn
    std::map<uint32_t, int32_t> locations;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> memberBuiltins;  // (struct, member) -> BuiltIn
    std::set<uint32_t> blocks;
    std::set<uint32_t> hasOffsets;                                     // structs with member Offsets already
    std::set<uint32_t> hasXfb;                                         // variables already decorated for transform feedback
    bool hasCapability = false;

    struct Entry { uint32_t model, function; std::string name; std::vector<uint32_t> interface; };
    std::vector<Entry> entries;
    std::set<uint32_t> xfbModes;
    size_t afterCapabilities = 5, afterEntries = 0, afterAnnotations = 0, firstType = 0;

    for (size_t at = 5; at < count;) {
        const uint32_t op = words[at] & 0xFFFF;
        const uint32_t n = words[at] >> 16;
        if (!n || at + n > count) {
            patch.error = "the module is malformed";
            return patch;
        }
        const uint32_t* w = words + at;
        switch (op) {
        case OpCapability:
            if (w[1] == CapabilityTransformFeedback) hasCapability = true;
            afterCapabilities = at + n;
            break;
        case OpEntryPoint: {
            Entry e;
            e.model = w[1];
            e.function = w[2];
            size_t used = 0;
            e.name = LiteralString(w + 3, n - 3, used);
            for (size_t i = 3 + used; i < n; ++i) e.interface.push_back(w[i]);
            entries.push_back(std::move(e));
            afterEntries = at + n;
            break;
        }
        case OpExecutionMode:
        case OpExecutionModeId:
            if (op == OpExecutionMode && n >= 3 && w[2] == ExecutionModeXfb) xfbModes.insert(w[1]);
            afterEntries = at + n;
            break;
        case OpName: {
            size_t used = 0;
            names[w[1]] = LiteralString(w + 2, n - 2, used);
            break;
        }
        case OpMemberName: {
            size_t used = 0;
            memberNames[{w[1], w[2]}] = LiteralString(w + 3, n - 3, used);
            break;
        }
        case OpDecorate:
            if (n >= 3 && w[2] == DecorationBuiltIn && n >= 4) builtins[w[1]] = w[3];
            if (n >= 3 && w[2] == DecorationLocation && n >= 4) locations[w[1]] = (int32_t)w[3];
            if (n >= 3 && w[2] == DecorationBlock) blocks.insert(w[1]);
            if (n >= 3 && (w[2] == DecorationXfbBuffer || w[2] == DecorationOffset)) hasXfb.insert(w[1]);
            break;
        case OpMemberDecorate:
            if (n >= 5 && w[3] == DecorationBuiltIn) memberBuiltins[{w[1], w[2]}] = w[4];
            if (n >= 4 && w[3] == DecorationOffset) hasOffsets.insert(w[1]);
            break;
        case OpTypeBool: case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeMatrix:
        case OpTypeArray: case OpTypeStruct: case OpTypePointer: {
            Type t;
            t.op = op;
            if (op == OpTypeInt) { t.width = w[2]; t.isSigned = w[3] != 0; }
            if (op == OpTypeFloat) t.width = w[2];
            if (op == OpTypeVector || op == OpTypeMatrix || op == OpTypeArray) { t.element = w[2]; t.count = w[3]; }
            if (op == OpTypeStruct) t.members.assign(w + 2, w + n);
            if (op == OpTypePointer) { t.storage = w[2]; t.element = w[3]; }
            types[w[1]] = t;
            if (!firstType) firstType = at;
            break;
        }
        case OpConstant:
            if (n >= 4) constants[w[2]] = w[3];
            break;
        case OpVariable:
            if (n >= 4) variables[w[2]] = w[1];
            break;
        default:
            break;
        }
        if (IsAnnotation(op)) afterAnnotations = at + n;
        at += n;
    }

    const Entry* entry = nullptr;
    for (const Entry& e : entries)
        if (e.model == ExecutionModelVertex && e.name == entryPoint) entry = &e;
    for (const Entry& e : entries)
        if (!entry && e.model == ExecutionModelVertex) entry = &e;
    if (!entry) {
        patch.error = "the module has no vertex entry point";
        return patch;
    }
    if (xfbModes.count(entry->function)) {
        patch.error = "the shader already writes transform feedback";
        return patch;
    }

    // Four bytes per scalar: 64-bit floats and 8- or 16-bit integers are not captured.
    std::function<Flat(uint32_t)> flatten = [&](uint32_t id) -> Flat {
        Flat f;
        auto it = types.find(id);
        if (it == types.end()) return f;
        const Type& t = it->second;
        if (t.op == OpTypeFloat && t.width == 32) return Flat{true, 4, 1, "float"};
        if (t.op == OpTypeInt && t.width == 32) return Flat{true, 4, 1, t.isSigned ? "int" : "uint"};
        if (t.op == OpTypeVector || t.op == OpTypeMatrix || t.op == OpTypeArray) {
            Flat e = flatten(t.element);
            uint32_t n = t.count;
            if (t.op == OpTypeArray) {
                auto c = constants.find(t.count);
                if (c == constants.end()) return f;
                n = c->second;
            }
            if (!e.ok || !n) return f;
            return Flat{true, e.bytes * n, e.components * n, e.base};
        }
        return f;
    };

    // What each captured output is, and where its decorations go.
    struct Plan { uint32_t variable; uint32_t structType; uint32_t member; bool isMember; XfbOutput out; uint32_t bytes; };
    std::vector<Plan> plans;
    uint32_t offset = 0;
    auto add = [&](uint32_t variable, uint32_t structType, uint32_t member, bool isMember, const Flat& f, std::string name, std::string builtin, int32_t location) {
        Plan p{variable, structType, member, isMember, XfbOutput{}, f.bytes};
        p.out.name = std::move(name);
        p.out.offset = offset;
        p.out.components = f.components;
        p.out.base = f.base;
        p.out.builtin = std::move(builtin);
        p.out.location = location;
        offset += f.bytes;
        plans.push_back(std::move(p));
    };

    for (uint32_t id : entry->interface) {
        auto v = variables.find(id);
        if (v == variables.end()) continue;
        auto ptr = types.find(v->second);
        if (ptr == types.end() || ptr->second.op != OpTypePointer || ptr->second.storage != StorageClassOutput) continue;
        if (hasXfb.count(id)) continue;
        const uint32_t pointee = ptr->second.element;
        auto st = types.find(pointee);
        if (st != types.end() && st->second.op == OpTypeStruct) {
            if (hasOffsets.count(pointee)) continue;
            const std::string instance = names.count(id) ? names[id] : "";
            bool builtinBlock = false;
            for (uint32_t m = 0; m < st->second.members.size(); ++m)
                if (memberBuiltins.count({pointee, m})) builtinBlock = true;
            for (uint32_t m = 0; m < st->second.members.size(); ++m) {
                auto b = memberBuiltins.find({pointee, m});
                // gl_PerVertex: only the position; point size and clip distances are left out.
                if (builtinBlock && (b == memberBuiltins.end() || b->second != BuiltInPosition)) continue;
                if (!builtinBlock && !blocks.count(pointee)) continue;
                Flat f = flatten(st->second.members[m]);
                if (!f.ok) continue;
                std::string member = memberNames.count({pointee, m}) ? memberNames[{pointee, m}] : "member" + std::to_string(m);
                if (builtinBlock) add(id, pointee, m, true, f, "gl_Position", "Position", -1);
                else add(id, pointee, m, true, f, instance.empty() ? member : instance + "." + member, "", locations.count(id) ? locations[id] : -1);
            }
            continue;
        }
        auto b = builtins.find(id);
        if (b != builtins.end() && b->second != BuiltInPosition) continue;
        if (b == builtins.end() && !locations.count(id)) continue;
        Flat f = flatten(pointee);
        if (!f.ok) continue;
        const std::string name = names.count(id) && !names[id].empty() ? names[id]
                               : b != builtins.end() ? "gl_Position" : "location" + std::to_string(locations[id]);
        add(id, 0, 0, false, f, name, b != builtins.end() ? "Position" : "", b != builtins.end() ? -1 : locations[id]);
    }
    if (plans.empty()) {
        patch.error = "the vertex shader has no outputs transform feedback can capture";
        return patch;
    }
    patch.stride = offset;

    // The new instructions, each after the section it belongs to.
    std::vector<uint32_t> capability, modes, decorations;
    if (!hasCapability) Emit(capability, OpCapability, {CapabilityTransformFeedback});
    Emit(modes, OpExecutionMode, {entry->function, ExecutionModeXfb});
    std::set<uint32_t> decoratedVariables;
    for (const Plan& p : plans) {
        if (decoratedVariables.insert(p.variable).second) {
            Emit(decorations, OpDecorate, {p.variable, DecorationXfbBuffer, 0});
            Emit(decorations, OpDecorate, {p.variable, DecorationXfbStride, patch.stride});
        }
        if (p.isMember) Emit(decorations, OpMemberDecorate, {p.structType, p.member, DecorationOffset, p.out.offset});
        else Emit(decorations, OpDecorate, {p.variable, DecorationOffset, p.out.offset});
        patch.outputs.push_back(p.out);
    }
    if (!afterAnnotations) afterAnnotations = firstType ? firstType : count;
    if (!afterEntries) afterEntries = afterCapabilities;

    patch.words.reserve(count + capability.size() + modes.size() + decorations.size());
    patch.words.insert(patch.words.end(), words, words + 5);
    for (size_t at = 5; at <= count;) {
        if (at == afterCapabilities) patch.words.insert(patch.words.end(), capability.begin(), capability.end());
        if (at == afterEntries) patch.words.insert(patch.words.end(), modes.begin(), modes.end());
        if (at == afterAnnotations) patch.words.insert(patch.words.end(), decorations.begin(), decorations.end());
        if (at == count) break;
        const uint32_t n = words[at] >> 16;
        patch.words.insert(patch.words.end(), words + at, words + at + n);
        at += n;
    }
    return patch;
}

} // namespace vkreplay
