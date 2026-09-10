// Stack traces: where an object was created and where a captured command was recorded, the
// counterpart of layer/src/stacktrace.h. The raw return addresses are taken in the hot path
// (cheap: no symbol lookup); the UI asks for the symbolized frames of the objects it shows
// (RequestStacktraces) and of the addresses a capture's commands carry (RequestSymbols).
// Symbols come from dladdr: exported symbols, else module and offset, which is what the
// application's own frames usually get in a stripped build.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vkinsp {
class JsonWriter;
}

namespace mtlinsp {

constexpr size_t kMaxStackFrames = 32;

/** Return addresses, innermost first. */
using StackTrace = std::vector<uint64_t>;

struct StackFrame {
    uint64_t address = 0;
    std::string module;     // file name of the module, empty when unknown
    std::string function;   // demangled symbol, empty when unknown
    uint64_t offset = 0;    // from the module base when no symbol, else from the symbol
    bool internal = false;  // inside this library, Metal, the driver or the runtime
};

/** MTLINSP_STACKTRACES: a stack at every object creation (the launch dialog's option). */
bool StackTracesEnabled();

/** The current thread's stack, skipping `skip` frames above the caller. */
StackTrace CaptureStack(unsigned skip = 0);

/** A frame per address, resolved as far as dladdr allows. */
std::vector<StackFrame> Symbolize(const StackTrace &addresses);

void WriteStackFrames(vkinsp::JsonWriter &w, const std::vector<StackFrame> &frames);

/** "0x1234", the spelling the UI uses for an address. */
std::string HexAddress(uint64_t address);

}  // namespace mtlinsp
