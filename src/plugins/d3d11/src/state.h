// What the library knows about the application's Direct3D 11: every object with the description
// the inspector shows (AddObject's args), and the state bound on each device context, followed from
// the application's Set* calls so a draw's state is known without asking the runtime (a deferred
// context cannot be asked at all).
//
// Objects are keyed by their interface pointer: the runtime implements every version of an
// interface in one object (ID3D11Device5 is the same pointer as ID3D11Device), so the pointer is
// the object's identity whichever version it is seen as. Release is hooked so a count reaching zero
// drops the entry before the address is reused.
//
// Everything here is guarded by one lock, State().mutex. Device methods are free-threaded; an
// immediate context is used from one thread at a time, and each deferred context from its own.
#pragma once

#include "common.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace d3d11insp
{

// ------------------------------------------------------------------------------------------------
// Objects

enum class ObjKind : uint8_t
{
    Other,
    Device,
    Context,
    SwapChain,
    Buffer,
    Texture1D,
    Texture2D,
    Texture3D,
    ShaderResourceView,
    RenderTargetView,
    DepthStencilView,
    UnorderedAccessView,
    Shader,
    InputLayout,
    SamplerState,
    RasterizerState,
    BlendState,
    DepthStencilState,
    Query,
    CommandList,
    ClassLinkage,
};

/** One element of an input layout, with its offset resolved (D3D11_APPEND_ALIGNED_ELEMENT). */
struct InputElement
{
    std::string semanticName;
    UINT semanticIndex = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT slot = 0;
    UINT offset = 0;
    UINT size = 0;
    bool perInstance = false;
    UINT stepRate = 0;
};

struct Object
{
    uint64_t id = 0;
    IUnknown* ptr = nullptr;
    ObjKind kind = ObjKind::Other;
    std::string type;                // "ID3D11Buffer"
    uint64_t parent = 0;
    std::string cmd;                 // the call that made it
    std::string label;
    /** What describes it, key -> JSON value, in the order first set: AddObject's args. */
    std::vector<std::pair<std::string, std::string>> args;
    /** Shader bytecode: "<stage>:<entry>", as the inspector's shader views find it. */
    std::vector<std::pair<std::string, std::shared_ptr<std::vector<uint8_t>>>> blobs;

    // Resources
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0, depth = 0;   // depth: a 3D texture's depth, else the array size
    UINT mips = 1;
    UINT samples = 1;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    UINT bindFlags = 0;
    UINT miscFlags = 0;
    UINT cpuAccess = 0;
    /** Buffers: the size in bytes, and the structure stride (0 when not structured). */
    UINT size = 0;
    UINT stride = 0;
    /** Bumped by every write the library sees, so a read-back of unchanged contents is reused. */
    uint32_t generation = 0;
    /** Swap chain back buffers: the swap chain (also the parent). */
    uint64_t swapChain = 0;

    // Views: the resource and what of it the view covers
    IUnknown* resource = nullptr;
    uint64_t resourceId = 0;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    UINT mip = 0;              // the mip slice (RTV, DSV, UAV) or the most detailed mip (SRV)
    UINT firstSlice = 0;
    UINT sliceCount = 1;
    /** Buffer views: the range in bytes. */
    UINT64 viewOffset = 0;
    UINT64 viewSize = 0;
    bool readOnlyDepth = false;
    bool readOnlyStencil = false;

    // Input layouts
    std::vector<InputElement> elements;

    // Shaders
    std::string stage;   // the inspector's stage name: vertex, fragment, ...

    // Rasterizer, blend and depth-stencil states: what the draw state reports
    D3D11_CULL_MODE cullMode = D3D11_CULL_BACK;
    bool frontCounterClockwise = false;
    bool scissorEnable = false;
    bool depthEnable = true;

    // Swap chains
    UINT bufferCount = 0;
    HWND window = nullptr;
    ID3D11Device* device = nullptr;
};

// ------------------------------------------------------------------------------------------------
// Contexts

enum Stage
{
    VS = 0,
    HS,
    DS,
    GS,
    PS,
    CS,
    kStageCount
};

/** The inspector's stage names, in Stage order ("vertex", "tess_control", ...). */
const char* StageName(int stage);
/** "VS", "HS", ... as the method prefixes spell them. */
const char* StagePrefix(int stage);

struct ConstantBufferBinding
{
    ID3D11Buffer* buffer = nullptr;
    UINT first = 0;   // in 16-byte constants (VSSetConstantBuffers1)
    UINT count = 0;   // 0: the whole buffer
};

struct StageBindings
{
    IUnknown* shader = nullptr;
    std::array<ConstantBufferBinding, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> constantBuffers{};
    std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> resources{};
    std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers{};
};

struct VertexBufferBinding
{
    ID3D11Buffer* buffer = nullptr;
    UINT stride = 0;
    UINT offset = 0;
};

/**
 * The state bound on a context, as the application set it. Cleared by ClearState, by an executed
 * command list when the application did not ask for the state to be kept, and on a deferred
 * context by FinishCommandList.
 */
struct PipelineState
{
    std::array<StageBindings, kStageCount> stages{};
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> csUavs{};
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> psUavs{};
    ID3D11InputLayout* inputLayout = nullptr;
    std::array<VertexBufferBinding, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertexBuffers{};
    ID3D11Buffer* indexBuffer = nullptr;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT indexOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> renderTargets{};
    UINT renderTargetCount = 0;
    ID3D11DepthStencilView* depthStencil = nullptr;
    ID3D11BlendState* blendState = nullptr;
    float blendFactor[4] = {1, 1, 1, 1};
    UINT sampleMask = 0xffffffff;
    ID3D11DepthStencilState* depthStencilState = nullptr;
    UINT stencilRef = 0;
    ID3D11RasterizerState* rasterizerState = nullptr;
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    UINT viewportCount = 0;
    std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
    UINT scissorCount = 0;
    std::array<ID3D11Buffer*, D3D11_SO_STREAM_COUNT> streamOutput{};
    ID3D11Predicate* predicate = nullptr;
    BOOL predicateValue = FALSE;
};

/** A render pass the library has open on a context (capture.cpp). */
struct OpenPass
{
    bool open = false;
    bool compute = false;
    uint32_t index = 0;
    /** What the pass draws into, as bound when it began; the pass stays open while these do not change. */
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
    UINT targetCount = 0;
    ID3D11DepthStencilView* depth = nullptr;
    /** The index of the pass's BeginRenderPass in the recording, rewritten when the pass ends. */
    size_t beginCommand = 0;
    bool drawn = false;
    /** Attachments cleared before the first draw (a load op of CLEAR): bit i for target i, bit 8 for depth, 9 for stencil. */
    uint32_t cleared = 0;
    /** Attachments discarded (DiscardView) after drawing: same bits. */
    uint32_t discarded = 0;
    /** The timestamp query written where the pass began, or null. */
    ID3D11Query* beginQuery = nullptr;
    /** The pass's attachments were read back already (before a discard threw them away). */
    bool readBack = false;
    int eventDepth = 0;
};

struct CommandRecorder;
class ContextProxy;

struct Context
{
    /** The real context, which the library's own calls go to; the application has `proxy`. */
    ID3D11DeviceContext* ptr = nullptr;
    ID3D11DeviceContext* proxy = nullptr;
    ID3D11Device* device = nullptr;
    uint64_t id = 0;
    uint64_t deviceId = 0;
    bool deferred = false;
    PipelineState state;
    OpenPass pass;
    /** How many passes this context began since its last Present (or FinishCommandList): the next pass's index. */
    uint32_t nextPassIndex = 0;
    uint32_t nextComputePassIndex = 0;
    /** BeginEvent depth (ID3DUserDefinedAnnotation), and how deep the open pass began. */
    int eventDepth = 0;
    /** A deferred context's recording since its last FinishCommandList (capture.cpp). */
    std::shared_ptr<CommandRecorder> recorder;
    /** The disjoint query bracketing this context's timestamps for the frame being captured. */
    ID3D11Query* disjointQuery = nullptr;
};

struct LibraryState
{
    std::recursive_mutex mutex;
    std::unordered_map<uint64_t, std::unique_ptr<Object>> objects;
    std::unordered_map<const void*, uint64_t> byPointer;
    std::unordered_map<const void*, std::unique_ptr<Context>> contexts;
    uint64_t nextId = 1;
};

LibraryState& State();

/**
 * A new object for `ptr`, not yet announced: the caller describes it (its args), then calls
 * Announce. One already tracked is returned as is.
 */
Object& Track(IUnknown* ptr, ObjKind kind, const char* type, uint64_t parent, const char* cmd);
/** Announces a new object to the inspector (AddObject), if one is connected. */
void Announce(const Object& o);
/** The object behind an interface pointer; null when the library does not know it. */
Object* Find(const void* ptr);
Object* FindById(uint64_t id);
/** The id of a tracked object, 0 for null or one the library does not know. */
uint64_t IdOf(const void* ptr);
/** The object is gone (its count reached zero): forgotten, and the inspector told (DeleteObjects). */
void Untrack(const void* ptr);

/** Sets one field of an object's description and tells the inspector (ObjectUpdate). */
void Describe(Object& o, const std::string& key, const std::string& json);
void SetLabel(Object& o, const std::string& label);
/** Attaches a payload the inspector fetches on request (RequestBlob): shader bytecode. */
void AddBlob(Object& o, const std::string& name, std::shared_ptr<std::vector<uint8_t>> data);
/** Answers RequestBlob: the payload, or null. */
std::shared_ptr<std::vector<uint8_t>> BlobOf(uint64_t id, size_t index);

/** The context record of a real device context, made on first sight. */
Context& ContextOf(ID3D11DeviceContext* ctx);
Context* FindContext(const void* ctx);
/** The context record whose proxy this is, or null for anything else. */
Context* ContextOfProxy(const void* proxy);

/** Everything, for a newly connected inspector: Snapshot, then AddObject for every object. */
void SendSnapshot();

// ------------------------------------------------------------------------------------------------
// JSON helpers for descriptions

std::string JsonString(const std::string& s);
std::string JsonRef(uint64_t id, const char* className);
std::string JsonRef(const void* ptr);
inline std::string JsonInt(int64_t v) { return std::to_string(v); }
inline std::string JsonBool(bool v) { return v ? "true" : "false"; }
/** The type name of a tracked object, for a reference to it; "ID3D11DeviceChild" when unknown. */
const char* ClassOf(const void* ptr);

}  // namespace d3d11insp
