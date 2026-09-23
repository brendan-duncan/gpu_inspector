#include "shader_edit.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#import <Metal/Metal.h>
#import <objc/message.h>

#include "function_constants.h"
#include "hooks_common.h"
#include "overdraw.h"
#include "swizzle.h"
#include "tracker.h"

namespace mtlinsp
{
namespace
{

/** The stage names the UI sends, and the descriptor property each one sets. */
struct StageSlot
{
    const char* name;
    const char* getter;   // "vertexFunction"
    const char* setter;   // "setVertexFunction:"
};

/**
 * Every stage a Metal pipeline descriptor can hold a function in, render and compute alike. The
 * object and mesh stages are here because a mesh pipeline's descriptor names its functions the
 * same way — a pipeline built from one is not rebuilt today (`MTLMeshRenderPipelineDescriptor` is
 * a different class this does not copy), but the names are what the UI would send for it.
 */
constexpr StageSlot kStages[] = {
    {"vertex", "vertexFunction", "setVertexFunction:"},
    {"fragment", "fragmentFunction", "setFragmentFunction:"},
    {"compute", "computeFunction", "setComputeFunction:"},
    {"object", "objectFunction", "setObjectFunction:"},
    {"mesh", "meshFunction", "setMeshFunction:"},
};

const StageSlot* SlotFor(const std::string& stage)
{
    for (const StageSlot& s : kStages)
    {
        if (stage == s.name)
            return &s;
    }
    return nullptr;
}

/** One pipeline the UI has edited: the stages replaced, and the state bound in its place. */
struct Edit
{
    /** Stage name -> the source it was replaced with, kept so another stage's edit can re-apply it. */
    std::unordered_map<std::string, std::string> stages;
    /** The state being bound instead of the original. Retained; never released (see shader_edit.h). */
    id replacement = nil;
};

std::mutex g_mutex;
/** Original state -> its edit. Keyed by pointer, the shape everything in this library uses. */
std::unordered_map<const void*, Edit> g_edits;
/** A compute pipeline built from a function rather than a descriptor: the function, retained. */
std::unordered_map<const void*, id> g_computeFunctions;
/** Replacements, held for the library's life so a command buffer with unretained references is safe. */
std::vector<id> g_kept;
/** How many substitutions are live, so the binding hooks can skip the lock when none are. */
std::atomic<size_t> g_editCount{0};

id Send(id target, const char* selector)
{
    return ((id(*)(id, SEL))objc_msgSend)(target, sel_registerName(selector));
}

void SendSet(id target, const char* selector, id value)
{
    ((void (*)(id, SEL, id))objc_msgSend)(target, sel_registerName(selector), value);
}

std::string Utf8(NSString* s) { return s != nil ? std::string(s.UTF8String) : std::string(); }

/** The compiler's diagnostics, or the error's description when it gave none. */
std::string CompileError(NSError* error)
{
    if (error == nil)
        return "the compiler reported no error and produced no library";
    NSString* log = error.userInfo[@"MTLCompilerErrorLog"];
    if (log == nil)
        log = error.localizedDescription;
    return Utf8(log);
}

/**
 * The library compiled from `source`, or nil with `error`.
 *
 * The compile options are the defaults rather than the application's: a descriptor does not carry
 * what its functions were compiled with, and the application's `MTLCompileOptions` object is long
 * gone by the time a person is editing. Fast math and the language version are the two that could
 * differ, and both are reported in `note` rather than guessed at.
 */
id CompileLibrary(id<MTLDevice> device, const std::string& source, std::string& error)
{
    NSString* text = [NSString stringWithUTF8String:source.c_str()];
    if (text == nil)
    {
        error = "the source is not valid UTF-8";
        return nil;
    }
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    NSError* compileError = nil;
    id library = [device newLibraryWithSource:text options:options error:&compileError];
    [options release];
    if (library == nil)
        error = CompileError(compileError);
    return library;
}

/**
 * The function named like `original` taken out of `library`, specialized the way `original` was.
 *
 * The name is the original's: an edit replaces a stage's body, not which entry point the pipeline
 * uses, and a person who renames the function in the text has changed the pipeline rather than
 * edited it — which reads as "the edited source has no function named X", the honest answer.
 */
id FunctionLike(id library, id<MTLFunction> original, std::string& error, std::string& note)
{
    NSString* name = original.name;
    if (name == nil)
    {
        error = "the stage's function has no name";
        return nil;
    }
    id values = FunctionConstantsOf(original);
    if (values == nil)
    {
        id function = [(id<MTLLibrary>)library newFunctionWithName:name];
        if (function == nil)
        {
            error = "the edited source has no function named '" + Utf8(name) + "'";
        }
        return function;
    }
    // Specialized the same way. A variant-heavy library compiled with the constants at their
    // defaults would compile and draw, which is what makes getting this wrong hard to notice.
    NSError* specializeError = nil;
    id function = [(id<MTLLibrary>)library newFunctionWithName:name
                                                constantValues:(MTLFunctionConstantValues*)values
                                                         error:&specializeError];
    if (function == nil)
    {
        error = "specializing '" + Utf8(name) + "' with the function constants the application used failed: " + CompileError(specializeError);
        return nil;
    }
    if (note.empty())
        note = "specialized with the function constants the original was built with";
    return function;
}

/** Whether the state is a render pipeline (as opposed to a compute one). */
bool IsRenderPipeline(id state)
{
    return [state conformsToProtocol:@protocol(MTLRenderPipelineState)];
}

/**
 * Builds the pipeline again with every stage of `edit` replaced, and returns it retained.
 *
 * Every edited stage is re-applied from its source each time, not only the one that just changed:
 * a second edit rebuilds from the *application's* descriptor, so a stage edited earlier would
 * otherwise be silently restored by the next edit of another stage.
 */
id BuildReplacement(id original, const Edit& edit, std::string& error, std::string& note)
{
    id<MTLDevice> device = IsRenderPipeline(original) ? ((id<MTLRenderPipelineState>)original).device
                                                      : ((id<MTLComputePipelineState>)original).device;
    if (device == nil)
    {
        error = "the pipeline does not say which device made it";
        return nil;
    }

    id descriptor = CopyRememberedPipelineDescriptor(original);
    id computeFunction = nil;
    if (descriptor == nil)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_computeFunctions.find((__bridge const void*)original);
        if (it != g_computeFunctions.end())
            computeFunction = [it->second retain];
    }
    if (descriptor == nil && computeFunction == nil)
    {
        error = "the pipeline's descriptor was not recorded (created before the inspector was loaded, "
                "or through a form it does not hook): it cannot be rebuilt";
        return nil;
    }
    // A compute pipeline built from a bare function: a descriptor of one, so the swap below is the
    // same code for both kinds.
    if (descriptor == nil)
    {
        MTLComputePipelineDescriptor* made = [[MTLComputePipelineDescriptor alloc] init];
        made.computeFunction = (id<MTLFunction>)computeFunction;
        descriptor = made;
        [computeFunction release];
    }

    for (const auto& [stageName, source] : edit.stages)
    {
        const StageSlot* slot = SlotFor(stageName);
        if (slot == nullptr)
        {
            error = "'" + stageName + "' is not a stage a Metal pipeline has";
            break;
        }
        if (![descriptor respondsToSelector:sel_registerName(slot->setter)])
        {
            error = "this pipeline has no " + stageName + " stage";
            break;
        }
        id<MTLFunction> current = (id<MTLFunction>)Send(descriptor, slot->getter);
        if (current == nil)
        {
            error = "this pipeline's " + stageName + " stage is not set";
            break;
        }
        id library = CompileLibrary(device, source, error);
        if (library == nil)
            break;
        id function = FunctionLike(library, current, error, note);
        [library release];
        if (function == nil)
            break;
        SendSet(descriptor, slot->setter, function);
        [function release];
    }
    if (!error.empty())
    {
        [descriptor release];
        return nil;
    }

    // The label the UI will show it under. A pipeline state's own label is read-only and comes
    // from the descriptor, so it has to be set before the state is made.
    if ([descriptor respondsToSelector:@selector(setLabel:)])
    {
        NSString* was = (NSString*)Send(descriptor, "label");
        NSString* label = was.length > 0 ? [NSString stringWithFormat:@"%@ (edited)", was] : @"(edited)";
        SendSet(descriptor, "setLabel:", label);
    }

    NSError* buildError = nil;
    id state = nil;
    std::string args;
    if (IsRenderPipeline(original))
    {
        MTLRenderPipelineReflection* reflection = nil;
        state = [device newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor*)descriptor
                                                     options:kReflectionOptions
                                                  reflection:&reflection
                                                       error:&buildError];
        if (state != nil)
            args = RenderPipelineArgs((MTLRenderPipelineDescriptor*)descriptor, reflection);
    }
    else
    {
        MTLComputePipelineReflection* reflection = nil;
        state = [device newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor*)descriptor
                                                      options:kReflectionOptions
                                                   reflection:&reflection
                                                        error:&buildError];
        if (state != nil)
        {
            args = ComputePipelineFunctionArgs(((MTLComputePipelineDescriptor*)descriptor).computeFunction,
                (id<MTLComputePipelineState>)state, reflection);
        }
    }
    [descriptor release];
    if (state == nil)
    {
        error = "the pipeline did not build with the edited stage: " + CompileError(buildError);
        return nil;
    }

    // Registered as an object of its own, the way the D3D12 library registers its replacement: the
    // same class and the same creating call, with its label marked, so the UI can show what is
    // bound. Deliberately not under Internal(): this object is meant to be announced.
    const char* cmd = IsRenderPipeline(original) ? "newRenderPipelineStateWithDescriptor:error:"
                                                 : "newComputePipelineStateWithDescriptor:options:reflection:error:";
    Track(state, IsRenderPipeline(original) ? "MTLRenderPipelineState" : "MTLComputePipelineState",
        cmd, device, args);
    TrackLabel(state);
    return state;
}

/** The tracked pipeline state with this id, or nil with `error`. */
id LookupPipeline(uint64_t pipelineId, std::string& error)
{
    id state = LiveObject(pipelineId);
    if (state == nil)
    {
        error = "object " + std::to_string(pipelineId) + " is no longer alive";
        return nil;
    }
    if (!IsRenderPipeline(state) && ![state conformsToProtocol:@protocol(MTLComputePipelineState)])
    {
        error = "object " + std::to_string(pipelineId) + " is not a render or compute pipeline state";
        return nil;
    }
    return state;
}

}  // namespace

bool ReplaceShader(uint64_t pipelineId, const std::string& stage, const std::string& source,
    std::string& error, uint64_t& replacementId, std::string& note)
{
    id original = LookupPipeline(pipelineId, error);
    if (original == nil)
        return false;
    if (SlotFor(stage) == nullptr)
    {
        error = "'" + stage + "' is not a stage a Metal pipeline has";
        return false;
    }
    if (source.empty())
    {
        error = "no source to compile";
        return false;
    }

    // The edit is assembled outside the lock: compiling is slow, and a rebuild announces an
    // object, which reaches the tracker's own lock.
    Edit next;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_edits.find((__bridge const void*)original);
        if (it != g_edits.end())
            next.stages = it->second.stages;
    }
    next.stages[stage] = source;

    id state = BuildReplacement(original, next, error, note);
    if (state == nil)
        return false;
    next.replacement = state;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_kept.push_back(state);
        g_edits[(__bridge const void*)original] = next;
        g_editCount.store(g_edits.size(), std::memory_order_release);
    }
    replacementId = IdOf(state);
    Log("shader edit: pipeline %llu rebuilt as %llu (%zu edited stage(s))",
        (unsigned long long)pipelineId, (unsigned long long)replacementId, next.stages.size());
    return true;
}

bool RestoreShader(uint64_t pipelineId, const std::string& stage, std::string& error)
{
    id original = LookupPipeline(pipelineId, error);
    if (original == nil)
        return false;

    Edit remaining;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_edits.find((__bridge const void*)original);
        if (it == g_edits.end())
        {
            error = "pipeline " + std::to_string(pipelineId) + " is not edited";
            return false;
        }
        remaining.stages = it->second.stages;
    }
    if (!stage.empty())
    {
        if (remaining.stages.erase(stage) == 0)
        {
            error = "the " + stage + " stage of pipeline " + std::to_string(pipelineId) + " is not edited";
            return false;
        }
    }
    else
    {
        remaining.stages.clear();
    }

    // Other stages stay edited: the pipeline is made again without this one, which is also what
    // makes restoring the last stage the same code path as dropping the edit entirely.
    if (!remaining.stages.empty())
    {
        std::string note;
        id state = BuildReplacement(original, remaining, error, note);
        if (state == nil)
            return false;
        remaining.replacement = state;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_kept.push_back(state);
        g_edits[(__bridge const void*)original] = remaining;
        g_editCount.store(g_edits.size(), std::memory_order_release);
        return true;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_edits.erase((__bridge const void*)original);
    g_editCount.store(g_edits.size(), std::memory_order_release);
    Log("shader edit: pipeline %llu restored", (unsigned long long)pipelineId);
    return true;
}

id SubstitutePipelineState(id state)
{
    if (state == nil || g_editCount.load(std::memory_order_acquire) == 0)
        return state;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_edits.find((__bridge const void*)state);
    return it != g_edits.end() && it->second.replacement != nil ? it->second.replacement : state;
}

void RememberComputePipeline(id state, id function)
{
    if (state == nil || function == nil || IsInternal())
        return;
    id kept = [function retain];
    std::lock_guard<std::mutex> lock(g_mutex);
    id& slot = g_computeFunctions[(__bridge const void*)state];
    [slot release];
    slot = kept;
}

void ForgetEditedPipeline(id object)
{
    if (object == nil)
        return;
    id released = nil;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_computeFunctions.find((__bridge const void*)object);
        if (it != g_computeFunctions.end())
        {
            released = it->second;
            g_computeFunctions.erase(it);
        }
        // An edited pipeline being deallocated cannot be bound again, so its entry goes; the
        // replacement itself stays in g_kept, which nothing frees while the library is attached.
        if (g_edits.erase((__bridge const void*)object) != 0)
        {
            g_editCount.store(g_edits.size(), std::memory_order_release);
        }
    }
    [released release];
}

}  // namespace mtlinsp
