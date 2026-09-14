// Stack traces: where an object was created and where a captured command was recorded. The raw
// return addresses are captured in the hot path (cheap: no symbol lookup); the UI asks for the
// symbolized frames of the objects it shows (RequestStacktraces) and of the addresses a
// capture's commands carry (RequestSymbols). Symbols come from DbgHelp (PDBs next to the modules,
// or export tables), the Vulkan layer's Windows path.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vkinsp { class JsonWriter; }

namespace dxinsp {

constexpr size_t kMaxStackFrames = 32;

/** Return addresses, innermost first. */
using StackTrace = std::vector<uint64_t>;

struct StackFrame {
    uint64_t address = 0;
    std::string module;     // file name of the module (no directory), empty when unknown
    std::string function;   // undecorated symbol, empty when unknown
    std::string file;       // source file, when line information exists
    uint32_t line = 0;
    uint64_t offset = 0;    // from the module base
    bool internal = false;  // inside this library, the D3D12 runtime, DXGI or the driver
};

/** DXINSP_STACKTRACES: capture a stack at every object creation (the launch dialog's "Stack traces"). */
bool StackTracesEnabled();

/** The current thread's stack, skipping `skip` frames above the caller. */
StackTrace CaptureStack(unsigned skip = 0);

/** Symbolizes addresses; a frame per address, resolved as far as DbgHelp allows. */
std::vector<StackFrame> Symbolize(const StackTrace& addresses);

/** Writes frames as a JSON array of {address, module, function, file, line, offset, internal}. */
void WriteStackFrames(vkinsp::JsonWriter& w, const std::vector<StackFrame>& frames);

/** Writes addresses as a JSON array of "0x..." strings (64-bit values are not safe as numbers). */
void WriteStackAddresses(vkinsp::JsonWriter& w, const StackTrace& stack);

/** ",\"stack\":[...]" for a recorded command's extra JSON, empty for an empty stack. */
std::string StackExtraJson(const StackTrace& stack);

}  // namespace dxinsp
