#include "count_patch.h"

#include <map>
#include <set>

namespace vkreplay
{

namespace
{

// Opcodes, decorations and enums the edit reads or writes (the SPIR-V specification, section 3).
enum : uint32_t
{
    OpNop = 0,
    OpUndef = 1,
    OpSourceContinued = 2,
    OpSource = 3,
    OpSourceExtension = 4,
    OpName = 5,
    OpMemberName = 6,
    OpString = 7,
    OpLine = 8,
    OpExtension = 10,
    OpExtInstImport = 11,
    OpExtInst = 12,
    OpMemoryModel = 14,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpCapability = 17,
    OpTypeFloat = 22,
    OpTypePointer = 32,
    OpConstant = 43,
    OpFunction = 54,
    OpFunctionParameter = 55,
    OpFunctionCall = 57,
    OpVariable = 59,
    OpImageTexelPointer = 60,
    OpLoad = 61,
    OpStore = 62,
    OpCopyMemory = 63,
    OpCopyMemorySized = 64,
    OpAccessChain = 65,
    OpInBoundsAccessChain = 66,
    OpPtrAccessChain = 67,
    OpInBoundsPtrAccessChain = 70,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpDecorationGroup = 73,
    OpGroupDecorate = 74,
    OpGroupMemberDecorate = 75,
    OpCopyObject = 83,
    OpImageWrite = 99,
    OpConvertUToPtr = 120,
    OpBitcast = 124,
    OpSelect = 169,
    OpAtomicStore = 228,
    OpAtomicXor = 242,
    OpPhi = 245,
    OpLabel = 248,
    OpKill = 252,
    OpAtomicFlagTestAndSet = 318,
    OpAtomicFlagClear = 319,
    OpNoLine = 317,
    OpModuleProcessed = 330,
    OpExecutionModeId = 331,
    OpDecorateId = 332,
    OpTerminateInvocation = 4416,
    OpDemoteToHelperInvocation = 5380,
    OpAtomicFMinEXT = 5614,
    OpAtomicFMaxEXT = 5615,
    OpDecorateString = 5632,
    OpMemberDecorateString = 5633,
    OpAtomicFAddEXT = 6035,
};
enum : uint32_t
{
    DecorationBuiltIn = 11,
    DecorationLocation = 30,
};
enum : uint32_t
{
    BuiltInSampleMask = 20,
    BuiltInFragDepth = 22,
    BuiltInFragStencilRefEXT = 5014,
};
enum : uint32_t
{
    StorageClassUniform = 2,
    StorageClassOutput = 3,
    StorageClassPrivate = 6,
    StorageClassImage = 11,
    StorageClassStorageBuffer = 12,
    StorageClassPhysicalStorageBuffer = 5349,
};
constexpr uint32_t ExecutionModelFragment = 4;
constexpr uint32_t kVersion14 = 0x00010400;

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

/** The instructions of a module's first sections, before its types: what a decoration must come ahead of. */
bool IsPreamble(uint32_t op)
{
    switch (op)
    {
    case OpNop: case OpSourceContinued: case OpSource: case OpSourceExtension: case OpName: case OpMemberName:
    case OpString: case OpLine: case OpNoLine: case OpExtension: case OpExtInstImport: case OpMemoryModel:
    case OpEntryPoint: case OpExecutionMode: case OpExecutionModeId: case OpCapability: case OpModuleProcessed:
    case OpDecorate: case OpMemberDecorate: case OpDecorationGroup: case OpGroupDecorate: case OpGroupMemberDecorate:
    case OpDecorateId: case OpDecorateString: case OpMemberDecorateString:
        return true;
    default:
        return false;
    }
}

/** Instructions whose result can be a pointer, [result type, result id, ...]: what a store's target is found from. */
bool MayMakePointer(uint32_t op)
{
    switch (op)
    {
    case OpUndef: case OpFunctionParameter: case OpVariable: case OpImageTexelPointer: case OpLoad:
    case OpAccessChain: case OpInBoundsAccessChain: case OpPtrAccessChain: case OpInBoundsPtrAccessChain:
    case OpCopyObject: case OpConvertUToPtr: case OpBitcast: case OpSelect: case OpPhi:
        return true;
    default:
        return false;
    }
}

bool IsAtomicWrite(uint32_t op)
{
    return (op >= OpAtomicStore && op <= OpAtomicXor) || op == OpAtomicFlagTestAndSet || op == OpAtomicFlagClear ||
        op == OpAtomicFMinEXT || op == OpAtomicFMaxEXT || op == OpAtomicFAddEXT;
}

void Emit(std::vector<uint32_t>& out, uint32_t op, std::initializer_list<uint32_t> operands)
{
    out.push_back(((uint32_t)operands.size() + 1) << 16 | op);
    out.insert(out.end(), operands.begin(), operands.end());
}

} // namespace

CountPatch PatchForCounting(const uint32_t* words, size_t count, const std::string& entryPoint)
{
    CountPatch patch;
    if (!words || count < 5 || words[0] != 0x07230203)
    {
        patch.error = "not a SPIR-V module";
        return patch;
    }

    // What the module holds: pointer types, the global variables and what is decorated on them, and
    // the instructions that decide whether a fragment is kept or that write memory.
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> pointers;   // pointer type -> (storage class, pointee)
    std::map<uint32_t, uint32_t> resultTypes;                     // pointer-making result -> its type
    std::map<uint32_t, uint32_t> globals;                         // global variable -> storage class
    std::map<uint32_t, uint32_t> builtins;                        // variable -> BuiltIn
    uint32_t float32 = 0;
    uint32_t entryFunction = 0;
    bool entryFound = false;
    bool discards = false;
    bool writesMemory = false;
    std::vector<std::pair<uint32_t, uint32_t>> stores;            // (target pointer, opcode)
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
        if (op == OpEntryPoint && wc >= 4 && o[0] == ExecutionModelFragment)
        {
            size_t used = 0;
            const std::string name = LiteralString(o + 2, wc - 3, used);
            // The one named, else the first fragment entry point.
            if (!entryFound && name == entryPoint)
            {
                entryFunction = o[1];
                entryFound = true;
            }
            else if (!entryFunction)
            {
                entryFunction = o[1];
            }
        }
        else if (op == OpTypePointer && wc == 4)
            pointers[o[0]] = {o[1], o[2]};
        else if (op == OpTypeFloat && wc >= 3 && o[1] == 32)
            float32 = o[0];
        else if (op == OpDecorate && wc >= 4 && o[1] == DecorationBuiltIn)
            builtins[o[0]] = o[2];
        else if (op == OpFunction)
            inFunction = true;
        else if (op == OpKill || op == OpTerminateInvocation || op == OpDemoteToHelperInvocation)
            discards = true;
        else if (op == OpImageWrite || IsAtomicWrite(op))
            writesMemory = true;
        else if ((op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized) && wc >= 2)
            stores.push_back({o[0], op});
        if (op == OpVariable && !inFunction && wc >= 4)
            globals[o[1]] = o[2];
        if (MayMakePointer(op) && wc >= 3)
            resultTypes[o[1]] = o[0];
        i += wc;
    }
    if (!entryFunction)
    {
        patch.error = "the module has no fragment entry point";
        return patch;
    }
    for (const auto& [target, op] : stores)
    {
        auto type = resultTypes.find(target);
        auto pointer = type != resultTypes.end() ? pointers.find(type->second) : pointers.end();
        const uint32_t storage = pointer != pointers.end() ? pointer->second.first : 0;
        if (storage == StorageClassUniform || storage == StorageClassImage || storage == StorageClassStorageBuffer ||
            storage == StorageClassPhysicalStorageBuffer)
            writesMemory = true;
    }
    // The outputs that decide a fragment's fate stay; the color outputs become private.
    std::set<uint32_t> converted;
    bool decidingOutputs = false;
    for (const auto& [id, storage] : globals)
    {
        if (storage != StorageClassOutput)
            continue;
        auto builtin = builtins.find(id);
        if (builtin == builtins.end())
            converted.insert(id);
        else if (builtin->second == BuiltInFragDepth || builtin->second == BuiltInSampleMask || builtin->second == BuiltInFragStencilRefEXT)
            decidingOutputs = true;
    }
    patch.needed = discards || decidingOutputs;
    if (!patch.needed)
        return patch;
    if (writesMemory)
    {
        patch.error = "the fragment shader writes memory, which drawing it again would repeat";
        return patch;
    }

    uint32_t bound = words[3];
    const uint32_t counter = bound++;                 // the new output
    const uint32_t counterPointer = bound++;
    const uint32_t one = bound++;
    const uint32_t floatType = float32 ? float32 : bound++;
    std::map<uint32_t, uint32_t> twins;               // Output pointer type -> its Private twin
    for (const auto& [id, p] : pointers)
        if (p.first == StorageClassOutput)
            twins[id] = bound++;

    std::vector<uint32_t> out(words, words + 5);
    out.reserve(count + 64);
    std::set<uint32_t> derived = converted;           // the converted variables and pointers into them
    bool decorated = false;
    bool declared = false;
    bool inEntry = false;
    bool storePending = false;
    for (size_t i = 5; i < count;)
    {
        const uint32_t wc = words[i] >> 16;
        const uint32_t op = words[i] & 0xFFFF;
        const uint32_t* o = words + i + 1;
        i += wc;
        if (!decorated && !IsPreamble(op))
        {
            Emit(out, OpDecorate, {counter, DecorationLocation, 0});
            decorated = true;
        }
        if (!declared && op == OpFunction)
        {
            if (!float32)
                Emit(out, OpTypeFloat, {floatType, 32});
            Emit(out, OpTypePointer, {counterPointer, StorageClassOutput, floatType});
            Emit(out, OpConstant, {floatType, one, 0x3F800000u});
            Emit(out, OpVariable, {counterPointer, counter, StorageClassOutput});
            declared = true;
        }
        // The count is written first thing: a discarded fragment writes nothing whatever it stored.
        if (storePending && op != OpVariable && op != OpLine && op != OpNoLine && op != OpExtInst)
        {
            Emit(out, OpStore, {counter, one});
            storePending = false;
        }
        const size_t start = out.size();   // where this instruction is copied to
        switch (op)
        {
        case OpEntryPoint:
            if (wc >= 4 && o[0] == ExecutionModelFragment && o[1] == entryFunction)
            {
                size_t used = 0;
                LiteralString(o + 2, wc - 3, used);
                std::vector<uint32_t> operands(o, o + 2 + used);
                // Before SPIR-V 1.4 an entry point lists only its Input and Output variables.
                for (size_t k = 2 + used; k < wc - 1; ++k)
                    if (words[1] >= kVersion14 || !converted.count(o[k]))
                        operands.push_back(o[k]);
                operands.push_back(counter);
                out.push_back(((uint32_t)operands.size() + 1) << 16 | op);
                out.insert(out.end(), operands.begin(), operands.end());
                continue;
            }
            break;
        case OpDecorate:
        case OpDecorateId:
        case OpDecorateString:
            if (wc >= 2 && converted.count(o[0]))
                continue;   // a private variable has no location, component or index
            break;
        case OpGroupDecorate:
        {
            std::vector<uint32_t> operands;
            for (size_t k = 0; k + 1 < wc; ++k)
                if (k == 0 || !converted.count(o[k]))
                    operands.push_back(o[k]);
            out.push_back(((uint32_t)operands.size() + 1) << 16 | op);
            out.insert(out.end(), operands.begin(), operands.end());
            continue;
        }
        case OpTypePointer:
            if (wc == 4 && twins.count(o[0]))
            {
                out.insert(out.end(), words + i - wc, words + i);
                Emit(out, OpTypePointer, {twins[o[0]], StorageClassPrivate, o[2]});
                continue;
            }
            break;
        case OpVariable:
            if (wc >= 4 && converted.count(o[1]) && twins.count(o[0]))
            {
                out.insert(out.end(), words + i - wc, words + i);
                out[start + 1] = twins[o[0]];
                out[start + 3] = StorageClassPrivate;
                continue;
            }
            break;
        case OpAccessChain:
        case OpInBoundsAccessChain:
        case OpPtrAccessChain:
        case OpInBoundsPtrAccessChain:
        case OpCopyObject:
            if (wc >= 4 && derived.count(o[2]))
            {
                if (!twins.count(o[0]))
                {
                    patch.error = "a pointer into a color output is not an Output pointer";
                    return patch;
                }
                out.insert(out.end(), words + i - wc, words + i);
                out[start + 1] = twins[o[0]];
                derived.insert(o[1]);
                continue;
            }
            break;
        case OpFunctionCall:
            for (size_t k = 3; k + 1 < wc; ++k)
            {
                if (derived.count(o[k]))
                {
                    patch.error = "the fragment shader passes a color output to a function";
                    return patch;
                }
            }
            break;
        case OpPhi:
        case OpSelect:
            for (size_t k = 2; k + 1 < wc; ++k)
            {
                if (derived.count(o[k]))
                {
                    patch.error = "the fragment shader chooses between pointers to its color outputs";
                    return patch;
                }
            }
            break;
        case OpFunction:
            inEntry = wc >= 3 && o[1] == entryFunction;
            break;
        case OpLabel:
            if (inEntry)
            {
                storePending = true;   // the first block, after its variables
                inEntry = false;
            }
            break;
        default:
            break;
        }
        out.insert(out.end(), words + i - wc, words + i);
    }
    if (!declared || storePending)
    {
        patch.error = "the module has no functions";
        return patch;
    }
    out[3] = bound;
    patch.words = std::move(out);
    return patch;
}

} // namespace vkreplay
