#include "device_removed.h"

#include <atomic>
#include <mutex>
#include <string>

#include "common.h"
#include "transport.h"

namespace dxinsp
{

namespace
{

/** The runtime's names for the operations it records, indexed by D3D12_AUTO_BREADCRUMB_OP. */
const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op)
    {
        case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
        case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return "CopyTiles";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "ExecuteBundle";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
        case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
        case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAccelerationStructure";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DispatchMesh";
        default: return "an operation";
    }
}

/** What a resource near a page fault was: still live, recently freed, or never allocated. */
const char* AllocationTypeName(D3D12_DRED_ALLOCATION_TYPE type)
{
    switch (type)
    {
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "command queue";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "command allocator";
        case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "pipeline state";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "command list";
        case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "fence";
        case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "descriptor heap";
        case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "heap";
        case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "query heap";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "command signature";
        case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_LIBRARY: return "pipeline library";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER: return "video decoder";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_PROCESSOR: return "video processor";
        case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "resource";
        case D3D12_DRED_ALLOCATION_TYPE_PASS: return "pass";
        case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSION: return "crypto session";
        case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSIONPOLICY: return "crypto session policy";
        case D3D12_DRED_ALLOCATION_TYPE_PROTECTEDRESOURCESESSION: return "protected resource session";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER_HEAP: return "video decoder heap";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_POOL: return "command pool";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_RECORDER: return "command recorder";
        case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT: return "state object";
        case D3D12_DRED_ALLOCATION_TYPE_METACOMMAND: return "meta command";
        case D3D12_DRED_ALLOCATION_TYPE_SCHEDULINGGROUP: return "scheduling group";
        default: return "an object";
    }
}

/** A wide name the runtime kept, as UTF-8; empty when it has none. `Narrow` is from common.h. */
std::string NameOf(const wchar_t* text) { return text && *text ? Narrow(text) : std::string(); }

/**
 * What the removal code means. The distinction these draw is the first thing worth knowing — work
 * that ran too long is the application's to fix, a driver fault is not — and a bare hex code says
 * none of it.
 */
std::string RemovalReason(HRESULT hr)
{
    switch (hr)
    {
        case DXGI_ERROR_DEVICE_HUNG:
            return "the GPU stopped making progress on this application's work (DXGI_ERROR_DEVICE_HUNG), "
                   "usually a shader that does not terminate or a draw too large to finish in the time Windows allows";
        case DXGI_ERROR_DEVICE_REMOVED:
            return "the device was removed (DXGI_ERROR_DEVICE_REMOVED), which is a driver crash, a driver update, "
                   "or the adapter being physically detached";
        case DXGI_ERROR_DEVICE_RESET:
            return "the device was reset (DXGI_ERROR_DEVICE_RESET) because of an invalid command from this application";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
            return "the driver hit an internal error (DXGI_ERROR_DRIVER_INTERNAL_ERROR)";
        case DXGI_ERROR_INVALID_CALL:
            return "the runtime rejected a call as invalid (DXGI_ERROR_INVALID_CALL)";
        default:
            return "the device reported " + HrText(hr);
    }
}

std::atomic<bool> g_reported{false};

} // namespace

bool IsDeviceRemoved(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

void EnableDeviceRemovedData()
{
    if (ConfigFlag("DXINSP_NO_DRED"))
        return;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
        if (!d3d12)
            d3d12 = LoadLibraryW(L"d3d12.dll");
        auto getDebugInterface = d3d12 ? reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(d3d12, "D3D12GetDebugInterface")) : nullptr;
        if (!getDebugInterface)
            return;
        ScopedInternal internal;
        // DRED 1.1 carries the breadcrumbs and the page fault; the runtime only keeps them when
        // asked before the device exists, which is why this cannot be done later.
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
        if (FAILED(getDebugInterface(IID_PPV_ARGS(settings.put()))) || !settings)
        {
            Log("device-removed data: this runtime has no DRED settings interface");
            return;
        }
        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        LogAlways("device-removed data (DRED): breadcrumbs and page faults on");
    });
}

bool SimulateDeviceRemoved()
{
    static const int at = [] {
        const std::string v = ConfigValue("DXINSP_SIMULATE_DEVICE_REMOVED");
        if (v.empty() || v == "0")
            return 0;
        const int n = atoi(v.c_str());
        return n > 0 ? n : 1;
    }();
    if (!at)
        return false;
    static std::atomic<int> presents{0};
    return presents.fetch_add(1) + 1 == at;
}

void OnDeviceRemoved(ID3D12Device* device, const char* call)
{
    if (g_reported.exchange(true))
        return;
    ScopedInternal internal;

    // The reason is the device's own, and is more specific than the call's result. It is S_OK when
    // the device is in fact fine, which is the case under DXINSP_SIMULATE_DEVICE_REMOVED; then there
    // is nothing to report but the breadcrumbs.
    std::string reason;
    if (device)
    {
        const HRESULT why = device->GetDeviceRemovedReason();
        if (FAILED(why))
            reason = RemovalReason(why);
    }

    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (device)
        device->QueryInterface(IID_PPV_ARGS(dred.put()));

    JsonWriter w;
    w.BeginObject();
    w.Key("action");
    w.String("DeviceRemoved");
    w.Key("call");
    w.String(call);
    if (!reason.empty())
    {
        w.Key("reason");
        w.String(reason);
    }

    // Every message opens the same way, with the reason where the device gave one.
    const std::string lead = reason.empty() ? std::string("The device was lost. ")
                                            : "The device was lost: " + reason + ". ";

    std::string message;
    if (!dred)
    {
        w.Key("breadcrumbs");
        w.Boolean(false);
        message = lead + "The command it was running is not known: this runtime has no DRED, or it was turned off.";
        w.Key("message");
        w.String(message);
        w.EndObject();
        LogAlways("device removed from %s. %s", call, message.c_str());
        Transport::Get().SendJson(std::move(w.str()));
        return;
    }

    // Every command list the runtime was tracking, with how far into it the GPU had got. The runtime
    // fills this in only once the device has really been removed: while it is healthy the call fails
    // with DXGI_ERROR_NOT_CURRENTLY_AVAILABLE and there are no nodes at all, which is what
    // DXINSP_SIMULATE_DEVICE_REMOVED sees. No nodes is therefore "nothing is known", not "nothing was
    // running", and the two must not read the same.
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
    std::string firstIncomplete;
    size_t listsInFlight = 0;
    size_t nodes = 0;
    const HRESULT crumbsHr = dred->GetAutoBreadcrumbsOutput1(&crumbs);
    w.Key("commandLists");
    w.BeginArray();
    if (SUCCEEDED(crumbsHr))
    {
        for (const D3D12_AUTO_BREADCRUMB_NODE1* node = crumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
        {
            ++nodes;
            const uint32_t done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            const uint32_t total = node->BreadcrumbCount;
            // A list whose last completed operation is its last one finished cleanly; the rest were
            // still running when the GPU stopped, and the first of those is the suspect.
            const bool complete = done >= total;
            if (!complete)
                ++listsInFlight;
            Log("DRED node: %u/%u operations, history %s", done, total, node->pCommandHistory ? "kept" : "absent");
            const std::string listName = NameOf(node->pCommandListDebugNameW);
            const std::string queueName = NameOf(node->pCommandQueueDebugNameW);
            w.BeginObject();
            w.Key("commandList");
            w.String(listName.empty() ? "(unnamed)" : listName);
            w.Key("commandQueue");
            w.String(queueName.empty() ? "(unnamed)" : queueName);
            w.Key("operationsCompleted");
            w.Uint(done);
            w.Key("operationsTotal");
            w.Uint(total);
            w.Key("complete");
            w.Boolean(complete);
            if (!complete && node->pCommandHistory && done < total)
            {
                const char* op = BreadcrumbOpName(node->pCommandHistory[done]);
                w.Key("stoppedAt");
                w.String(op);
                if (firstIncomplete.empty())
                {
                    firstIncomplete = std::string(op) + " (operation " + std::to_string(done + 1) + " of " + std::to_string(total) + " in command list " + (listName.empty() ? "(unnamed)" : listName) + ")";
                }
            }
            w.EndObject();
        }
    }
    w.EndArray();

    // A page fault names an address the GPU read or wrote that it should not have; the runtime
    // lists what was allocated near it, which is usually the resource freed too early.
    D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
    std::string faultText;
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)) && pageFault.PageFaultVA)
    {
        char va[32];
        snprintf(va, sizeof(va), "0x%llx", (unsigned long long)pageFault.PageFaultVA);
        w.Key("pageFaultAddress");
        w.String(va);
        faultText = std::string(" The GPU faulted on address ") + va + ".";
        auto allocations = [&](const char* key, const D3D12_DRED_ALLOCATION_NODE* head, const char* what) {
            w.Key(key);
            w.BeginArray();
            size_t n = 0;
            for (const D3D12_DRED_ALLOCATION_NODE* node = head; node && n < 32; node = node->pNext, ++n)
            {
                const std::string name = NameOf(node->ObjectNameW);
                w.BeginObject();
                w.Key("name");
                w.String(name.empty() ? "(unnamed)" : name);
                w.Key("type");
                w.String(AllocationTypeName(node->AllocationType));
                w.EndObject();
                if (n == 0)
                {
                    faultText += std::string(" The nearest ") + what + " object is " + (name.empty() ? std::string("an unnamed ") + AllocationTypeName(node->AllocationType) : name + " (" + AllocationTypeName(node->AllocationType) + ")") + ".";
                }
            }
            w.EndArray();
        };
        allocations("existingAllocations", pageFault.pHeadExistingAllocationNode, "live");
        // A freed object next to the faulting address is the classic use-after-free.
        allocations("recentFreedAllocations", pageFault.pHeadRecentFreedAllocationNode, "recently freed");
    }

    w.Key("breadcrumbs");
    w.Boolean(nodes > 0);
    if (!firstIncomplete.empty())
    {
        message = lead + "The GPU was running " + firstIncomplete + (listsInFlight > 1 ? " (" + std::to_string(listsInFlight) + " command lists were in flight)" : "") + ".";
    }
    else if (listsInFlight)
    {
        message = lead + std::to_string(listsInFlight) + " command list(s) were unfinished, but the runtime kept no operation history for them.";
    }
    else if (nodes)
    {
        message = lead +
            "Every tracked command list had finished, so the cause is outside the work the runtime "
            "was tracking.";
    }
    else if (crumbsHr == DXGI_ERROR_UNSUPPORTED)
    {
        // The one case the user can do something about: the runtime was never asked to keep them.
        message = lead +
            "The runtime kept no breadcrumbs, because device-removed data was not enabled before the "
            "device was created (DXINSP_NO_DRED). What the GPU was running is not known.";
    }
    else
    {
        message = lead + "The runtime returned no breadcrumbs (" + HrText(crumbsHr) +
            "), so what the GPU was "
            "running is not known. It keeps them only once the device has really been removed.";
    }
    message += faultText;
    w.Key("message");
    w.String(message);
    w.EndObject();

    LogAlways("device removed from %s. %s", call, message.c_str());
    Transport::Get().SendJson(std::move(w.str()));
}

} // namespace dxinsp
