#include "identity_patch.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace vkreplay
{

namespace
{

// Opcodes, decorations and enums the edit reads or writes (the SPIR-V specification, section 3).
enum : uint32_t
{
    OpEntryPoint = 15,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeArray = 28,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpConstant = 43,
    OpFunction = 54,
    OpFunctionEnd = 56,
    OpVariable = 59,
    OpLoad = 61,
    OpStore = 62,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpEmitVertex = 218,
    OpEmitStreamVertex = 220,
    OpReturn = 253,
};
constexpr uint32_t DecorationBuiltIn = 11;
constexpr uint32_t DecorationLocation = 30;
constexpr uint32_t BuiltInPrimitiveId = 7;
constexpr uint32_t BuiltInInvocationId = 8;
constexpr uint32_t BuiltInTessCoord = 13;
constexpr uint32_t StorageClassInput = 1;
constexpr uint32_t StorageClassOutput = 3;
constexpr uint32_t ExecutionModelTessellationEvaluation = 2;
constexpr uint32_t ExecutionModelGeometry = 3;

std::string LiteralString(const uint32_t* w, size_t words, size_t& used)
{
    std::string s;
    for (size_t i = 0; i < words; ++i)
    {
        for (int b = 0; b < 4; ++b)
        {
            const char c = (char)((w[i] >> (8 * b)) & 0xFF);
            if (!c)
            {
                used = i + 1;
                return s;
            }
            s += c;
        }
    }
    used = words;
    return s;
}

void Emit(std::vector<uint32_t>& out, uint32_t op, std::initializer_list<uint32_t> operands)
{
    out.push_back(((uint32_t)operands.size() + 1) << 16 | op);
    out.insert(out.end(), operands.begin(), operands.end());
}

struct Type
{
    uint32_t op = 0;
    std::vector<uint32_t> operands;   // the words after the result id
};

} // namespace

IdentityPatch AddIdentityOutputs(const uint32_t* words, size_t count, const std::string& entryPoint)
{
    IdentityPatch patch;
    if (!words || count < 5 || words[0] != 0x07230203)
    {
        patch.error = "not a SPIR-V module";
        return patch;
    }
    struct Entry
    {
        size_t at = 0;
        uint32_t model = 0, function = 0;
        std::string name;
    };
    std::vector<Entry> entries;
    std::map<uint32_t, Type> types;
    std::map<uint32_t, uint32_t> constants;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> variables;   // id -> (storage, pointer type)
    std::map<uint32_t, uint32_t> builtins;                         // variable -> BuiltIn
    std::map<uint32_t, uint32_t> locations;                        // variable -> Location
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> memberLocations;
    size_t firstType = 0, firstFunction = 0;
    bool inFunction = false;
    for (size_t at = 5; at < count;)
    {
        const uint32_t op = words[at] & 0xFFFF;
        const uint32_t n = words[at] >> 16;
        if (!n || at + n > count)
        {
            patch.error = "the module is malformed";
            return patch;
        }
        const uint32_t* w = words + at;
        switch (op)
        {
            case OpEntryPoint:
            {
                Entry e;
                e.at = at;
                e.model = w[1];
                e.function = w[2];
                size_t used = 0;
                e.name = LiteralString(w + 3, n - 3, used);
                entries.push_back(e);
                break;
            }
            case OpDecorate:
                if (n >= 4 && w[2] == DecorationBuiltIn)
                    builtins[w[1]] = w[3];
                if (n >= 4 && w[2] == DecorationLocation)
                    locations[w[1]] = w[3];
                break;
            case OpMemberDecorate:
                if (n >= 5 && w[3] == DecorationLocation)
                    memberLocations[{w[1], w[2]}] = w[4];
                break;
            case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeMatrix: case OpTypeArray: case OpTypeStruct: case OpTypePointer:
                types[w[1]] = Type{op, std::vector<uint32_t>(w + 2, w + n)};
                break;
            case OpConstant:
                if (n >= 4)
                    constants[w[2]] = w[3];
                break;
            case OpFunction:
                inFunction = true;
                if (!firstFunction)
                    firstFunction = at;
                break;
            case OpVariable:
                if (!inFunction && n >= 4)
                    variables[w[2]] = {w[3], w[1]};
                break;
            default:
                break;
        }
        if (!firstType && (op == OpTypeInt || op == OpTypeFloat || op == OpTypeVector || op == OpTypePointer || op == OpTypeStruct ||
                              op == OpTypeArray || op == OpTypeMatrix || op == 19 /* OpTypeVoid */ || op == 20 /* OpTypeBool */ ||
                              op == 33 /* OpTypeFunction */ || op == OpConstant || op == OpVariable))
            firstType = at;
        at += n;
    }
    const Entry* entry = nullptr;
    for (const Entry& e : entries)
        if ((e.model == ExecutionModelGeometry || e.model == ExecutionModelTessellationEvaluation) && (!entry || e.name == entryPoint))
            entry = &e;
    if (!entry)
        return patch;   // nothing to add
    if (!firstType || !firstFunction)
    {
        patch.error = "the module has no types or functions";
        return patch;
    }
    const bool geometry = entry->model == ExecutionModelGeometry;

    // The locations the shader's outputs take, so the new ones come after every one of them.
    std::function<uint32_t(uint32_t)> slots = [&](uint32_t type) -> uint32_t {
        auto t = types.find(type);
        if (t == types.end())
            return 1;
        const Type& y = t->second;
        switch (y.op)
        {
            case OpTypeVector:
            {
                auto e = types.find(y.operands[0]);
                const bool wide = e != types.end() && e->second.operands.size() && e->second.operands[0] == 64;
                return wide && y.operands[1] > 2 ? 2 : 1;
            }
            case OpTypeMatrix: return y.operands[1] * slots(y.operands[0]);
            case OpTypeArray: return (constants.count(y.operands[1]) ? constants[y.operands[1]] : 1) * slots(y.operands[0]);
            case OpTypeStruct:
            {
                uint32_t sum = 0;
                for (uint32_t m : y.operands)
                    sum += slots(m);
                return sum;
            }
            default: return 1;
        }
    };
    uint32_t next = 0;
    for (const auto& [id, v] : variables)
    {
        if (v.first != StorageClassOutput)
            continue;
        auto ptr = types.find(v.second);
        const uint32_t pointee = ptr != types.end() && ptr->second.operands.size() >= 2 ? ptr->second.operands[1] : 0;
        if (auto l = locations.find(id); l != locations.end())
            next = std::max(next, l->second + slots(pointee));
        if (auto st = types.find(pointee); st != types.end() && st->second.op == OpTypeStruct)
            for (uint32_t m = 0; m < st->second.operands.size(); ++m)
                if (auto ml = memberLocations.find({pointee, m}); ml != memberLocations.end())
                    next = std::max(next, ml->second + slots(st->second.operands[m]));
    }

    uint32_t bound = words[3];
    std::vector<uint32_t> declarations, decorations;
    // Types found or made: a duplicate of a non-aggregate type is not allowed.
    auto findType = [&](uint32_t op, std::vector<uint32_t> operands) -> uint32_t {
        for (const auto& [id, t] : types)
            if (t.op == op && t.operands == operands)
                return id;
        const uint32_t id = bound++;
        std::vector<uint32_t> inst{((uint32_t)operands.size() + 2) << 16 | op, id};
        inst.insert(inst.end(), operands.begin(), operands.end());
        declarations.insert(declarations.end(), inst.begin(), inst.end());
        types[id] = Type{op, operands};
        return id;
    };
    std::vector<uint32_t> newInterface;
    // The built-in input, as the shader declares it, or declared here.
    auto builtinInput = [&](uint32_t builtin, uint32_t fallbackType) -> std::pair<uint32_t, uint32_t> {
        for (const auto& [id, b] : builtins)
        {
            auto v = variables.find(id);
            if (b != builtin || v == variables.end() || v->second.first != StorageClassInput)
                continue;
            auto ptr = types.find(v->second.second);
            if (ptr != types.end() && ptr->second.operands.size() >= 2)
                return {id, ptr->second.operands[1]};
        }
        const uint32_t pointer = findType(OpTypePointer, {StorageClassInput, fallbackType});
        const uint32_t id = bound++;
        Emit(declarations, OpVariable, {pointer, id, StorageClassInput});
        Emit(decorations, OpDecorate, {id, DecorationBuiltIn, builtin});
        newInterface.push_back(id);
        return {id, fallbackType};
    };
    const uint32_t intType = findType(OpTypeInt, {32, 1});
    struct Copy
    {
        uint32_t input, type, output;
    };
    std::vector<Copy> copies;
    auto addCopy = [&](uint32_t builtin, uint32_t fallbackType, const char* name, const char* builtinName) {
        const auto [input, type] = builtinInput(builtin, fallbackType);
        const uint32_t pointer = findType(OpTypePointer, {StorageClassOutput, type});
        const uint32_t output = bound++;
        Emit(declarations, OpVariable, {pointer, output, StorageClassOutput});
        Emit(decorations, OpDecorate, {output, DecorationLocation, next});
        newInterface.push_back(output);
        patch.outputs.push_back(IdentityOutput{next, name, builtinName});
        next += slots(type);
        copies.push_back(Copy{input, type, output});
    };
    if (geometry)
    {
        addCopy(BuiltInPrimitiveId, intType, "gl_PrimitiveIDIn", "PrimitiveId");
        addCopy(BuiltInInvocationId, intType, "gl_InvocationID", "InvocationId");
    }
    else
    {
        addCopy(BuiltInPrimitiveId, intType, "gl_PrimitiveID", "PrimitiveId");
        const uint32_t floatType = findType(OpTypeFloat, {32});
        addCopy(BuiltInTessCoord, findType(OpTypeVector, {floatType, 3}), "gl_TessCoord", "TessCoord");
    }

    std::vector<uint32_t>& out = patch.words;
    out.assign(words, words + 5);
    out.reserve(count + declarations.size() + decorations.size() + 64);
    bool inEntry = false;
    size_t inserted = 0;
    for (size_t at = 5; at < count;)
    {
        const uint32_t op = words[at] & 0xFFFF;
        const uint32_t n = words[at] >> 16;
        const uint32_t* w = words + at;
        if (at == firstType)
            out.insert(out.end(), decorations.begin(), decorations.end());
        if (at == firstFunction)
            out.insert(out.end(), declarations.begin(), declarations.end());
        if (op == OpFunction)
            inEntry = n >= 3 && w[2] == entry->function;
        else if (op == OpFunctionEnd)
            inEntry = false;
        // Where a vertex is finished: a geometry shader's emits, anywhere; the end of the entry point otherwise.
        if (geometry ? op == OpEmitVertex || op == OpEmitStreamVertex : inEntry && op == OpReturn)
        {
            for (const Copy& c : copies)
            {
                const uint32_t value = bound++;
                Emit(out, OpLoad, {c.type, value, c.input});
                Emit(out, OpStore, {c.output, value});
            }
            ++inserted;
        }
        if (at == entry->at)
        {
            out.push_back(((uint32_t)(n + newInterface.size())) << 16 | OpEntryPoint);
            out.insert(out.end(), w + 1, w + n);
            out.insert(out.end(), newInterface.begin(), newInterface.end());
        }
        else
        {
            out.insert(out.end(), w, w + n);
        }
        at += n;
    }
    if (!inserted)
    {
        patch.words.clear();
        patch.outputs.clear();
        patch.error = "no place where the shader finishes a vertex was found";
        return patch;
    }
    out[3] = bound;
    return patch;
}

} // namespace vkreplay
