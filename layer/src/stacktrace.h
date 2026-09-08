// Stack traces: where an object was created and where a captured command was recorded. The
// raw return addresses are captured in the hot path (cheap: no symbol lookup); the UI asks for
// the symbolized frames of the objects it shows (RequestStacktraces) and of the addresses a
// capture's commands carry (RequestSymbols). Symbols come from DbgHelp on Windows (PDBs next
// to the modules, or export tables) and dladdr elsewhere (exported symbols, or module+offset).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vkinsp {

class JsonWriter;

constexpr size_t kMaxStackFrames = 32;

// Return addresses, innermost first.
using StackTrace = std::vector<uint64_t>;

struct StackFrame {
    uint64_t address = 0;
    std::string module;     // file name of the module (no directory), empty when unknown
    std::string function;   // demangled / undecorated symbol, empty when unknown
    std::string file;       // source file, when line information exists
    uint32_t line = 0;
    uint64_t offset = 0;    // from the module base when no symbol, else from the symbol
    bool internal = false;  // inside the inspector layer, the loader or another layer
};

// VKINSP_STACKTRACES: capture a stack at every object creation (the launch dialog's
// "Stack traces" option). Commands get theirs per capture (CaptureOptions::stacktraces).
bool StackTracesEnabled();

// The current thread's stack, skipping `skip` frames above the caller.
StackTrace CaptureStack(unsigned skip = 0);

// Symbolizes addresses; a frame per address, resolved as far as the platform allows.
std::vector<StackFrame> Symbolize(const StackTrace& addresses);

// Writes frames as a JSON array of {address, module, function, file, line, offset, internal}.
void WriteStackFrames(JsonWriter& w, const std::vector<StackFrame>& frames);

// Writes addresses as a JSON array of "0x..." strings (64-bit values are not safe as numbers).
void WriteStackAddresses(JsonWriter& w, const StackTrace& stack);

// ",\"stack\":[...]" for a recorded command's extra JSON, empty for an empty stack.
std::string StackExtraJson(const StackTrace& stack);

}  // namespace vkinsp
