#include "store_patch.h"

#include <functional>
#include <map>

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
    OpTypeMatrix = 24,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpConstant = 43,
    OpFunction = 54,
    OpFunctionEnd = 56,
    OpVariable = 59,
    OpLoad = 61,
    OpStore = 62,
    OpAccessChain = 65,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpCompositeExtract = 81,
    OpBitcast = 124,
    OpIAdd = 128,
    OpISub = 130,
    OpIMul = 132,
    OpULessThan = 176,
    OpSelectionMerge = 247,
    OpLabel = 248,
    OpBranch = 249,
    OpBranchConditional = 250,
    OpReturn = 253,
};
constexpr uint32_t DecorationBlock = 2;
constexpr uint32_t DecorationBufferBlock = 3;
constexpr uint32_t DecorationArrayStride = 6;
constexpr uint32_t DecorationBuiltIn = 11;
constexpr uint32_t DecorationBinding = 33;
constexpr uint32_t DecorationDescriptorSet = 34;
constexpr uint32_t DecorationOffset = 35;
constexpr uint32_t BuiltInVertexIndex = 42;
constexpr uint32_t BuiltInInstanceIndex = 43;
constexpr uint32_t StorageClassInput = 1;
constexpr uint32_t StorageClassUniform = 2;
constexpr uint32_t StorageClassOutput = 3;
constexpr uint32_t StorageClassStorageBuffer = 12;
constexpr uint32_t ExecutionModelVertex = 0;

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

void EmitList(std::vector<uint32_t>& out, uint32_t op, const std::vector<uint32_t>& operands)
{
    out.push_back(((uint32_t)operands.size() + 1) << 16 | op);
    out.insert(out.end(), operands.begin(), operands.end());
}

struct Type
{
    uint32_t op = 0;
    std::vector<uint32_t> operands;
};

} // namespace

StorePatch PatchForVertexStores(const uint32_t* words, size_t count, const std::string& entryPoint, const std::vector<XfbOutput>& outputs,
    uint32_t stride, const StoreParams& params)
{
    StorePatch patch;
    if (!words || count < 5 || words[0] != 0x07230203)
    {
        patch.error = "not a SPIR-V module";
        return patch;
    }
    if (!stride || stride % 4 || outputs.empty())
    {
        patch.error = "the shader has no outputs to store";
        return patch;
    }
    struct Entry
    {
        size_t at = 0;
        uint32_t function = 0;
        std::string name;
    };
    std::vector<Entry> entries;
    std::map<uint32_t, Type> types;
    std::map<uint32_t, uint32_t> constants;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> variables;   // id -> (storage, pointer type)
    std::map<uint32_t, uint32_t> builtins;                         // variable -> BuiltIn
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
                if (n >= 4 && w[1] == ExecutionModelVertex)
                {
                    size_t used = 0;
                    entries.push_back(Entry{at, w[2], LiteralString(w + 3, n - 3, used)});
                }
                break;
            case OpDecorate:
                if (n >= 4 && w[2] == DecorationBuiltIn)
                    builtins[w[1]] = w[3];
                break;
            case OpTypeBool: case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeMatrix: case OpTypeArray: case OpTypeStruct:
            case OpTypePointer: case OpTypeRuntimeArray:
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
        // The first instruction of the types, constants and globals section.
        if (!firstType && (op == 19 || (op >= OpTypeBool && op <= 39) || op == OpConstant || op == OpVariable))
            firstType = at;
        at += n;
    }
    const Entry* entry = nullptr;
    for (const Entry& e : entries)
        if (!entry || e.name == entryPoint)
            entry = &e;
    if (!entry)
    {
        patch.error = "the module has no vertex entry point";
        return patch;
    }
    if (!firstType || !firstFunction)
    {
        patch.error = "the module has no types or functions";
        return patch;
    }
    // SPIR-V 1.3 has the StorageBuffer class; before it a storage buffer is a Uniform BufferBlock.
    const uint32_t version = words[1];
    const bool storageClass13 = version >= 0x00010300;
    const uint32_t bufferClass = storageClass13 ? StorageClassStorageBuffer : StorageClassUniform;
    const bool listsEveryGlobal = version >= 0x00010400;

    uint32_t bound = words[3];
    std::vector<uint32_t> declarations, decorations;
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
    auto constant = [&](uint32_t type, uint32_t value) -> uint32_t {
        const uint32_t id = bound++;
        Emit(declarations, OpConstant, {type, id, value});
        return id;
    };
    const uint32_t uintT = findType(OpTypeInt, {32, 0});
    const uint32_t intT = findType(OpTypeInt, {32, 1});
    const uint32_t boolT = findType(OpTypeBool, {});
    std::vector<uint32_t> newInterface;

    // The built-in inputs the slot is made from, as the shader declares them or declared here.
    auto builtinInput = [&](uint32_t builtin) -> std::pair<uint32_t, uint32_t> {
        for (const auto& [id, b] : builtins)
        {
            auto v = variables.find(id);
            if (b != builtin || v == variables.end() || v->second.first != StorageClassInput)
                continue;
            auto ptr = types.find(v->second.second);
            if (ptr != types.end() && ptr->second.operands.size() >= 2)
                return {id, ptr->second.operands[1]};
        }
        const uint32_t pointer = findType(OpTypePointer, {StorageClassInput, intT});
        const uint32_t id = bound++;
        Emit(declarations, OpVariable, {pointer, id, StorageClassInput});
        Emit(decorations, OpDecorate, {id, DecorationBuiltIn, builtin});
        newInterface.push_back(id);
        return {id, intT};
    };
    const auto [vertexIndex, vertexIndexType] = builtinInput(BuiltInVertexIndex);
    const auto [instanceIndex, instanceIndexType] = builtinInput(BuiltInInstanceIndex);

    // The buffer: a block of one runtime array of words.
    const uint32_t words32 = bound++;
    Emit(declarations, OpTypeRuntimeArray, {words32, uintT});
    Emit(decorations, OpDecorate, {words32, DecorationArrayStride, 4});
    const uint32_t block = bound++;
    Emit(declarations, OpTypeStruct, {block, words32});
    Emit(decorations, OpDecorate, {block, storageClass13 ? DecorationBlock : DecorationBufferBlock});
    Emit(decorations, OpMemberDecorate, {block, 0, DecorationOffset, 0});
    const uint32_t blockPointer = findType(OpTypePointer, {bufferClass, block});
    const uint32_t wordPointer = findType(OpTypePointer, {bufferClass, uintT});
    const uint32_t buffer = bound++;
    Emit(declarations, OpVariable, {blockPointer, buffer, bufferClass});
    Emit(decorations, OpDecorate, {buffer, DecorationDescriptorSet, params.set});
    Emit(decorations, OpDecorate, {buffer, DecorationBinding, params.binding});
    if (listsEveryGlobal)
        newInterface.push_back(buffer);

    const uint32_t zero = constant(uintT, 0);
    const uint32_t firstInstance = constant(intT, (uint32_t)params.firstInstance);
    const uint32_t perInstance = constant(intT, params.perInstance);
    const uint32_t base = constant(intT, (uint32_t)params.base);
    const uint32_t capacity = constant(uintT, params.capacity);
    const uint32_t strideWords = constant(uintT, stride / 4);

    // Each output: how it is reached (a variable, or a member of a block), and its scalars in the
    // order transform feedback lays them out, each with the path to it and its type.
    struct Leaf
    {
        std::vector<uint32_t> path;
        uint32_t type;
    };
    struct Source
    {
        uint32_t variable;
        int32_t member;
        uint32_t memberPointer;   // for a member: a pointer to its type
        uint32_t type;            // the value loaded
        uint32_t wordOffset;
        std::vector<Leaf> leaves;
        std::vector<uint32_t> wordConstants;
    };
    std::function<bool(uint32_t, std::vector<uint32_t>&, std::vector<Leaf>&)> flatten = [&](uint32_t type, std::vector<uint32_t>& path,
                                                                                         std::vector<Leaf>& out) -> bool {
        auto t = types.find(type);
        if (t == types.end())
            return false;
        const Type& y = t->second;
        if (y.op == OpTypeFloat || y.op == OpTypeInt)
        {
            if (y.operands.empty() || y.operands[0] != 32)
                return false;
            out.push_back(Leaf{path, type});
            return true;
        }
        uint32_t n = 0, element = 0;
        if (y.op == OpTypeVector || y.op == OpTypeMatrix)
        {
            element = y.operands[0];
            n = y.operands[1];
        }
        else if (y.op == OpTypeArray)
        {
            element = y.operands[0];
            n = constants.count(y.operands[1]) ? constants[y.operands[1]] : 0;
        }
        else
        {
            return false;
        }
        for (uint32_t i = 0; i < n; ++i)
        {
            path.push_back(i);
            const bool ok = flatten(element, path, out);
            path.pop_back();
            if (!ok)
                return false;
        }
        return true;
    };
    std::vector<Source> sources;
    for (const XfbOutput& o : outputs)
    {
        auto v = variables.find(o.variable);
        auto ptr = v != variables.end() ? types.find(v->second.second) : types.end();
        if (ptr == types.end() || ptr->second.operands.size() < 2 || ptr->second.operands[0] != StorageClassOutput)
        {
            patch.error = "an output to store is not an output variable of the module";
            return patch;
        }
        Source s{o.variable, o.member, 0, ptr->second.operands[1], o.offset / 4, {}, {}};
        if (o.member >= 0)
        {
            auto st = types.find(s.type);
            if (st == types.end() || st->second.op != OpTypeStruct || (size_t)o.member >= st->second.operands.size())
            {
                patch.error = "an output member to store is not in its block";
                return patch;
            }
            s.type = st->second.operands[(size_t)o.member];
            s.memberPointer = findType(OpTypePointer, {StorageClassOutput, s.type});
        }
        std::vector<uint32_t> path;
        if (!flatten(s.type, path, s.leaves) || s.leaves.size() != o.components)
        {
            patch.error = "an output's scalars could not be read one by one";
            return patch;
        }
        for (size_t k = 0; k < s.leaves.size(); ++k)
            s.wordConstants.push_back(constant(uintT, s.wordOffset + (uint32_t)k));
        sources.push_back(std::move(s));
    }
    std::map<int32_t, uint32_t> memberConstants;
    for (const Source& s : sources)
        if (s.member >= 0 && !memberConstants.count(s.member))
            memberConstants[s.member] = constant(uintT, (uint32_t)s.member);

    // The code before each return of the entry point.
    auto asInt = [&](std::vector<uint32_t>& out, uint32_t type, uint32_t value) -> uint32_t {
        if (type == intT)
            return value;
        const uint32_t id = bound++;
        Emit(out, OpBitcast, {intT, id, value});
        return id;
    };
    auto store = [&](std::vector<uint32_t>& out) {
        const uint32_t vi = bound++, ii = bound++;
        Emit(out, OpLoad, {vertexIndexType, vi, vertexIndex});
        Emit(out, OpLoad, {instanceIndexType, ii, instanceIndex});
        const uint32_t v = asInt(out, vertexIndexType, vi), i = asInt(out, instanceIndexType, ii);
        const uint32_t d1 = bound++, m = bound++, d2 = bound++, slotInt = bound++, slot = bound++, inside = bound++;
        Emit(out, OpISub, {intT, d1, i, firstInstance});
        Emit(out, OpIMul, {intT, m, d1, perInstance});
        Emit(out, OpISub, {intT, d2, v, base});
        Emit(out, OpIAdd, {intT, slotInt, m, d2});
        Emit(out, OpBitcast, {uintT, slot, slotInt});
        // A negative slot is a large unsigned one: outside too.
        Emit(out, OpULessThan, {boolT, inside, slot, capacity});
        const uint32_t write = bound++, merge = bound++;
        Emit(out, OpSelectionMerge, {merge, 0});
        Emit(out, OpBranchConditional, {inside, write, merge});
        Emit(out, OpLabel, {write});
        const uint32_t first = bound++;
        Emit(out, OpIMul, {uintT, first, slot, strideWords});
        for (const Source& s : sources)
        {
            uint32_t pointer = s.variable;
            if (s.member >= 0)
            {
                pointer = bound++;
                Emit(out, OpAccessChain, {s.memberPointer, pointer, s.variable, memberConstants[s.member]});
            }
            const uint32_t value = bound++;
            Emit(out, OpLoad, {s.type, value, pointer});
            for (size_t k = 0; k < s.leaves.size(); ++k)
            {
                const Leaf& leaf = s.leaves[k];
                uint32_t scalar = value;
                if (!leaf.path.empty())
                {
                    scalar = bound++;
                    std::vector<uint32_t> operands{leaf.type, scalar, value};
                    operands.insert(operands.end(), leaf.path.begin(), leaf.path.end());
                    EmitList(out, OpCompositeExtract, operands);
                }
                uint32_t bits = scalar;
                if (leaf.type != uintT)
                {
                    bits = bound++;
                    Emit(out, OpBitcast, {uintT, bits, scalar});
                }
                const uint32_t index = bound++, at = bound++;
                Emit(out, OpIAdd, {uintT, index, first, s.wordConstants[k]});
                Emit(out, OpAccessChain, {wordPointer, at, buffer, zero, index});
                Emit(out, OpStore, {at, bits});
            }
        }
        Emit(out, OpBranch, {merge});
        Emit(out, OpLabel, {merge});
    };

    std::vector<uint32_t>& out = patch.words;
    out.assign(words, words + 5);
    out.reserve(count + declarations.size() + decorations.size() + 256);
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
        if (inEntry && op == OpReturn)
        {
            store(out);
            ++inserted;
        }
        if (at == entry->at && !newInterface.empty())
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
        patch.error = "the vertex shader's entry point has no return to store its outputs before";
        return patch;
    }
    out[3] = bound;
    return patch;
}

} // namespace vkreplay
