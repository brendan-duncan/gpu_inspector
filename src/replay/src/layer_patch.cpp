#include "layer_patch.h"

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
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpConstant = 43,
    OpConstantComposite = 44,
    OpFunction = 54,
    OpFunctionEnd = 56,
    OpVariable = 59,
    OpLoad = 61,
    OpStore = 62,
    OpAccessChain = 65,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpCompositeConstruct = 80,
    OpIEqual = 170,
    OpSelect = 169,
    OpEmitVertex = 218,
    OpEmitStreamVertex = 220,
    OpReturn = 253,
};
constexpr uint32_t DecorationBuiltIn = 11;
constexpr uint32_t BuiltInPosition = 0;
constexpr uint32_t BuiltInLayer = 9;
constexpr uint32_t StorageClassOutput = 3;
constexpr uint32_t ExecutionModelVertex = 0;
constexpr uint32_t ExecutionModelTessellationEvaluation = 2;
constexpr uint32_t ExecutionModelGeometry = 3;

/** A literal string at `w`, and how many words it takes. */
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

/** Where a built-in output lives: a variable of its own, or a member of an output block variable. */
struct BuiltinOutput
{
    uint32_t variable = 0;
    bool member = false;
    uint32_t index = 0;     // the member's index in the block
    uint32_t type = 0;      // the built-in's own type (int, vec4)
};

} // namespace

LayerPatch PatchForLayer(const uint32_t* words, size_t count, const std::string& entryPoint, uint32_t layer)
{
    LayerPatch patch;
    if (!words || count < 5 || words[0] != 0x07230203)
    {
        patch.error = "not a SPIR-V module";
        return patch;
    }

    uint32_t model = UINT32_MAX;
    uint32_t entryFunction = 0;
    bool entryFound = false;
    std::map<uint32_t, uint32_t> builtins;                               // variable -> BuiltIn
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> memberBuiltins;    // (struct, member) -> BuiltIn
    std::map<uint32_t, std::vector<uint32_t>> structs;                   // struct -> member types
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> pointers;          // pointer -> (storage, pointee)
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> vectors;           // vector -> (component, count)
    std::map<uint32_t, uint32_t> outputs;                                // Output variable -> pointer type
    uint32_t boolType = 0;
    uint32_t int32Type = 0;
    bool inFunction = false;
    for (size_t i = 5; i < count;)
    {
        const uint32_t wc = words[i] >> 16;
        const uint32_t op = words[i] & 0xFFFF;
        if (!wc || i + wc > count)
        {
            patch.error = "the module is truncated";
            return patch;
        }
        const uint32_t* o = words + i + 1;
        switch (op)
        {
        case OpEntryPoint:
            if (wc >= 4 && (o[0] == ExecutionModelVertex || o[0] == ExecutionModelTessellationEvaluation || o[0] == ExecutionModelGeometry))
            {
                size_t used = 0;
                const std::string name = LiteralString(o + 2, wc - 3, used);
                if (!entryFound && name == entryPoint)
                {
                    model = o[0];
                    entryFunction = o[1];
                    entryFound = true;
                }
                else if (!entryFunction)
                {
                    model = o[0];
                    entryFunction = o[1];
                }
            }
            break;
        case OpDecorate:
            if (wc >= 4 && o[1] == DecorationBuiltIn)
                builtins[o[0]] = o[2];
            break;
        case OpMemberDecorate:
            if (wc >= 5 && o[2] == DecorationBuiltIn)
                memberBuiltins[{o[0], o[1]}] = o[3];
            break;
        case OpTypeBool:
            boolType = o[0];
            break;
        case OpTypeInt:
            if (wc >= 4 && o[1] == 32 && !int32Type)
                int32Type = o[0];
            break;
        case OpTypeVector:
            if (wc >= 4)
                vectors[o[0]] = {o[1], o[2]};
            break;
        case OpTypeStruct:
            structs[o[0]] = std::vector<uint32_t>(o + 1, o + wc - 1);
            break;
        case OpTypePointer:
            if (wc == 4)
                pointers[o[0]] = {o[1], o[2]};
            break;
        case OpFunction:
            inFunction = true;
            break;
        case OpVariable:
            if (!inFunction && wc >= 4 && o[2] == StorageClassOutput)
                outputs[o[1]] = o[0];
            break;
        default:
            break;
        }
        i += wc;
    }
    if (!entryFunction)
    {
        patch.error = "the module has no vertex, tessellation evaluation or geometry entry point";
        return patch;
    }
    // The outputs gl_Position and gl_Layer are, as variables of their own or members of a block.
    auto find = [&](uint32_t builtin) {
        BuiltinOutput out;
        for (const auto& [variable, pointer] : outputs)
        {
            auto pt = pointers.find(pointer);
            if (pt == pointers.end())
                continue;
            if (auto b = builtins.find(variable); b != builtins.end() && b->second == builtin)
            {
                out.variable = variable;
                out.type = pt->second.second;
                return out;
            }
            auto st = structs.find(pt->second.second);
            for (size_t m = 0; st != structs.end() && m < st->second.size(); ++m)
            {
                if (auto b = memberBuiltins.find({st->first, (uint32_t)m}); b != memberBuiltins.end() && b->second == builtin)
                {
                    out.variable = variable;
                    out.member = true;
                    out.index = (uint32_t)m;
                    out.type = st->second[m];
                    return out;
                }
            }
        }
        return out;
    };
    const BuiltinOutput position = find(BuiltInPosition);
    const BuiltinOutput layerOut = find(BuiltInLayer);
    patch.needed = layerOut.variable || layer != 0;
    if (!patch.needed)
        return patch;
    auto vec = vectors.find(position.type);
    if (!position.variable || vec == vectors.end() || vec->second.second != 4)
    {
        patch.error = "the shader writes no gl_Position to move";
        return patch;
    }
    if (layerOut.variable && layerOut.member == false && !pointers.count(outputs[layerOut.variable]))
    {
        patch.error = "the shader's gl_Layer has no type";
        return patch;
    }
    const uint32_t floatType = vec->second.first;

    // What the inserted code needs that the module may not declare: the types, the constants, and
    // pointers to the members when the built-ins are in a block.
    uint32_t bound = words[3];
    const bool newBool = layerOut.variable && !boolType;
    const uint32_t boolT = boolType ? boolType : bound++;
    uint32_t bool4 = 0;
    for (const auto& [id, v] : vectors)
        if (v.first == boolT && v.second == 4)
            bool4 = id;
    const bool newBool4 = layerOut.variable && !bool4;
    if (newBool4)
        bool4 = bound++;
    const bool needIndex = position.member || layerOut.member;
    const bool newInt = needIndex && !int32Type;
    const uint32_t intT = int32Type ? int32Type : bound++;
    const uint32_t two = bound++;
    const uint32_t one = bound++;
    const uint32_t cull = bound++;                        // (2, 2, 2, 1): outside every clip plane x <= w
    const uint32_t layerConst = layerOut.variable ? bound++ : 0;
    const uint32_t positionIndex = position.member ? bound++ : 0;
    const uint32_t layerIndex = layerOut.member ? bound++ : 0;
    const uint32_t positionPointer = position.member ? bound++ : 0;
    const uint32_t layerPointer = layerOut.member ? bound++ : 0;

    std::vector<uint32_t> out(words, words + 5);
    out.reserve(count + 128);
    bool declared = false;
    bool inEntry = false;
    size_t inserted = 0;
    auto insert = [&]() {
        uint32_t posPtr = position.variable;
        if (position.member)
        {
            posPtr = bound++;
            Emit(out, OpAccessChain, {positionPointer, posPtr, position.variable, positionIndex});
        }
        if (!layerOut.variable)
        {
            Emit(out, OpStore, {posPtr, cull});   // no layer written: layer 0, which is not the one followed
            ++inserted;
            return;
        }
        uint32_t layerPtr = layerOut.variable;
        if (layerOut.member)
        {
            layerPtr = bound++;
            Emit(out, OpAccessChain, {layerPointer, layerPtr, layerOut.variable, layerIndex});
        }
        const uint32_t value = bound++, same = bound++, p = bound++, same4 = bound++, moved = bound++;
        Emit(out, OpLoad, {layerOut.type, value, layerPtr});
        Emit(out, OpIEqual, {boolT, same, value, layerConst});
        Emit(out, OpLoad, {position.type, p, posPtr});
        Emit(out, OpCompositeConstruct, {bool4, same4, same, same, same, same});
        Emit(out, OpSelect, {position.type, moved, same4, p, cull});
        Emit(out, OpStore, {posPtr, moved});
        ++inserted;
    };
    for (size_t i = 5; i < count;)
    {
        const uint32_t wc = words[i] >> 16;
        const uint32_t op = words[i] & 0xFFFF;
        const uint32_t* o = words + i + 1;
        if (!declared && op == OpFunction)
        {
            if (newBool)
                Emit(out, OpTypeBool, {boolT});
            if (newBool4)
                Emit(out, OpTypeVector, {bool4, boolT, 4});
            if (newInt)
                Emit(out, OpTypeInt, {intT, 32, 1});
            Emit(out, OpConstant, {floatType, two, 0x40000000u});
            Emit(out, OpConstant, {floatType, one, 0x3F800000u});
            Emit(out, OpConstantComposite, {position.type, cull, two, two, two, one});
            if (layerConst)
                Emit(out, OpConstant, {layerOut.type, layerConst, layer});
            if (positionIndex)
                Emit(out, OpConstant, {intT, positionIndex, position.index});
            if (layerIndex)
                Emit(out, OpConstant, {intT, layerIndex, layerOut.index});
            if (positionPointer)
                Emit(out, OpTypePointer, {positionPointer, StorageClassOutput, position.type});
            if (layerPointer)
                Emit(out, OpTypePointer, {layerPointer, StorageClassOutput, layerOut.type});
            declared = true;
        }
        if (op == OpFunction)
            inEntry = wc >= 3 && o[1] == entryFunction;
        else if (op == OpFunctionEnd)
            inEntry = false;
        // A vertex is finished at the entry point's return, or a geometry shader's emit.
        if (model == ExecutionModelGeometry ? op == OpEmitVertex || op == OpEmitStreamVertex : inEntry && op == OpReturn)
            insert();
        out.insert(out.end(), words + i, words + i + wc);
        i += wc;
    }
    if (!inserted)
    {
        patch.error = "no place where the shader finishes a vertex was found";
        return patch;
    }
    out[3] = bound;
    patch.words = std::move(out);
    return patch;
}

} // namespace vkreplay
