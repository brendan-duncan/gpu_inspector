// dxinsp_replay: re-executes a Direct3D 12 GPU Inspector capture (.gpucap) on this machine's GPU,
// the counterpart of vkinsp_replay for Vulkan captures (docs/REPLAY.md, "Direct3D 12").
//
//   dxinsp_replay <capture.gpucap> [--debug-layer] [--trace]
//       Re-creates the capture's objects, replays its command lists, and compares every render
//       target the capture read back with the replay's own copy at the same point. Exit code 0
//       when every compared target matches exactly, 1 when some differ or could not be compared,
//       2 when the replay could not run.
//
//   dxinsp_replay <capture.gpucap> --export <directory> [--export-data <file>]
//       Export to C++: also writes the frame, as it is replayed, as a standalone C++ project that
//       re-creates its objects and re-issues its commands (dx_exporter.h).
//
//   dxinsp_replay <capture.gpucap> --counters [--counter <name>]... [--counter-data <file>]
//   dxinsp_replay <capture.gpucap> --list-counters [--counter-data <file>]
//       The GPU's own hardware counters around each render pass, through NVIDIA's Nsight Perf SDK
//       (dx_counters.h). The frame is replayed once per collection pass the counters need, and
//       nothing else runs; --list-counters names the ones this GPU offers instead.
//
//   dxinsp_replay <capture.gpucap> --draws [--draw-data <file>]
//       Every draw and dispatch of the frame with a timestamp pair, a pipeline statistics query and
//       an occlusion query around it (dx_measure.cpp), for GPU Inspector's Shader Flame Graph.
//
//   dxinsp_replay <capture.gpucap> --replace <request> [--target-data <file>] [--draws]
//       The frame replayed with other code for some pipelines' stages: a shader edited in GPU
//       Inspector, run in the capture. --target-data writes every compared render target with
//       how far it is from what the capture read back, and the pixels of the ones that differ.
//
//   dxinsp_replay <capture.gpucap> --ablate <request> [--ablate-data <file>]
//       Draws timed again with variants of one of their shader stages, which is how a function, a
//       source line or a texture of a shader is given a measured cost (dx_measure.cpp).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

#include "gpucap.h"

#include "dx_replayer.h"

using namespace dxreplay;

namespace {

// A capture is somebody else's frame run on this driver, and the frames worth exporting are the ones
// that break something. When one takes the process down, GPU Inspector is told why: without this it
// had a tool that wrote no summary, and could only say "the replay wrote no data".
std::string g_exportDataPath;
std::string g_exportDir;

LONG WINAPI OnCrash(EXCEPTION_POINTERS* info) {
    const unsigned code = info && info->ExceptionRecord ? (unsigned)info->ExceptionRecord->ExceptionCode : 0;
    char text[512];
    const char* step = CurrentStep();
    std::snprintf(text, sizeof(text), "the replay crashed (exception 0x%08X)%s%s: the frame could not be run on this GPU and driver, so no project was written",
                  code, *step ? " at " : "", step);
    std::fprintf(stderr, "dxinsp_replay: %s\n", text);
    if (!g_exportDataPath.empty()) {
        if (FILE* f = std::fopen(g_exportDataPath.c_str(), "wb")) {
            std::string error;
            for (const char* p = text; *p; ++p) {
                if (*p == '"' || *p == '\\') error += '\\';
                error += *p;
            }
            std::string dir;
            for (char ch : g_exportDir) dir += ch == '\\' ? '/' : ch;
            std::fprintf(f, "{\"format\":\"gpu-inspector-export-cpp\",\"version\":1,\"device\":\"\",\"directory\":\"%s\",\"ok\":false,\"error\":\"%s\","
                            "\"objects\":0,\"commands\":0,\"submissions\":0,\"targets\":0,\"leftOut\":0,\"dataBytes\":0,\"files\":[],\"notes\":[],\"problems\":[]}",
                         dir.c_str(), error.c_str());
            std::fclose(f);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

std::string JsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += (char)ch;
        } else if (ch < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
            out += escaped;
        } else {
            out += (char)ch;
        }
    }
    return out + "\"";
}

/** The summary GPU Inspector reads (parseExportSummary in src/app/src/renderer/export_cpp.ts): vkinsp_replay's, field for field. */
bool WriteExportData(const DxReplayReport& report, const std::string& path) {
    const DxExportReport& e = report.exported;
    std::string json = "{\"format\":\"gpu-inspector-export-cpp\",\"version\":1,\"device\":" + JsonString(report.device) +
                       ",\"directory\":" + JsonString(e.directory) + ",\"ok\":" + (e.error.empty() ? "true" : "false") +
                       ",\"error\":" + JsonString(e.error) + ",\"objects\":" + std::to_string(e.objects) +
                       ",\"commands\":" + std::to_string(e.commands) + ",\"submissions\":" + std::to_string(e.submissions) +
                       ",\"targets\":" + std::to_string(e.targets) + ",\"leftOut\":" + std::to_string(e.leftOut) +
                       ",\"dataBytes\":" + std::to_string(e.dataBytes) + ",\"files\":[";
    for (size_t i = 0; i < e.files.size(); ++i) json += (i ? "," : "") + JsonString(e.files[i]);
    json += "],\"notes\":[";
    for (size_t i = 0; i < e.notes.size() && i < 100; ++i) json += (i ? "," : "") + JsonString(e.notes[i]);
    json += "],\"problems\":[";
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i) json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(json.data(), (std::streamsize)json.size());
    return (bool)out;
}

/**
 * --draw-data: every draw and dispatch of the frame with the time it took and the counters it ran
 * up, in the layout `vkinsp_replay --draw-data` writes (parseDrawStats in
 * src/app/src/renderer/draw_stats.ts).
 */
bool WriteDrawData(const DxReplayReport& report, const std::string& path) {
    std::string json = "{\"format\":\"gpu-inspector-draw-stats\",\"version\":1,\"device\":" + JsonString(report.device) +
                       ",\"note\":" + JsonString(report.drawStatsNote) + ",\"draws\":[";
    for (size_t i = 0; i < report.draws.size(); ++i) {
        const DxDrawResult& d = report.draws[i];
        char ms[32];
        std::snprintf(ms, sizeof(ms), "%.6f", d.durationMs);
        json += std::string(i ? "," : "") + "{\"command\":" + std::to_string(d.command) + ",\"frame\":" + std::to_string(d.frame) +
                ",\"commandBuffer\":" + std::to_string(d.commandList) + ",\"passIndex\":" + std::to_string(d.passIndex) +
                ",\"timed\":" + (d.timed ? "true" : "false") + ",\"ms\":" + ms +
                ",\"counted\":" + (d.counted ? "true" : "false") +
                ",\"vertexInvocations\":" + std::to_string(d.vertexInvocations) +
                ",\"primitives\":" + std::to_string(d.primitives) +
                ",\"fragmentInvocations\":" + std::to_string(d.fragmentInvocations) +
                ",\"computeInvocations\":" + std::to_string(d.computeInvocations) +
                ",\"sampled\":" + (d.sampled ? "true" : "false") +
                ",\"samplesPassed\":" + std::to_string(d.samplesPassed) + "}";
    }
    json += "],\"problems\":[";
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i) json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(json.data(), (std::streamsize)json.size());
    return (bool)out;
}

// --replace: the request GPU Inspector writes (encodeReplaceRequest in
// src/app/src/renderer/shader_replay.ts), in the layout of --ablate's: "REPLACE 1\n", a
// little-endian u32 manifest length, the JSON manifest
//   {"replacements": [{"pipeline": 27, "stage": "fragment", "payload": [0, 11364]}]}
// and the code, which the manifest names as [offset, length] after it.
bool ReadReplaceRequest(const std::string& path, std::vector<DxShaderReplacement>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string magic = "REPLACE 1\n";
    if (bytes.size() < magic.size() + 4 || std::memcmp(bytes.data(), magic.data(), magic.size()) != 0) {
        error = "not a shader replacement request: " + path;
        return false;
    }
    uint32_t length = 0;
    std::memcpy(&length, bytes.data() + magic.size(), 4);
    const size_t start = magic.size() + 4;
    if (start + length > bytes.size()) {
        error = "the replacement request is truncated";
        return false;
    }
    vkreplay::JsonDocument doc;
    if (!doc.Parse(bytes.data() + start, length, error)) {
        error = "the replacement request's manifest is not valid JSON: " + error;
        return false;
    }
    const size_t base = start + length;
    const vkreplay::JValue* list = doc.Root().Get("replacements");
    for (uint32_t i = 0; list && list->IsArray() && i < list->count; ++i) {
        const vkreplay::JValue& item = list->items[i];
        DxShaderReplacement r;
        r.pipeline = item.Get("pipeline") ? item.Get("pipeline")->Uint() : 0;
        const vkreplay::JValue* stage = item.Get("stage");
        r.stage = stage && stage->IsString() ? std::string(stage->Str()) : std::string();
        const vkreplay::JValue* payload = item.Get("payload");
        if (payload && payload->IsArray() && payload->count == 2) {
            const uint64_t offset = payload->items[0].Uint();
            const uint64_t size = payload->items[1].Uint();
            if (base + offset + size <= bytes.size()) r.code.assign(bytes.begin() + (ptrdiff_t)(base + offset), bytes.begin() + (ptrdiff_t)(base + offset + size));
        }
        if (!r.pipeline || r.stage.empty() || r.code.empty()) {
            error = "replacement " + std::to_string(i) + " names no pipeline, no stage or no code";
            return false;
        }
        out.push_back(std::move(r));
    }
    if (out.empty()) {
        error = "the replacement request replaces nothing";
        return false;
    }
    return true;
}

/**
 * --target-data: what the replayed frame's render targets hold, against what the capture read back
 * (parseReplayedTargets in src/app/src/renderer/shader_replay.ts). "TARGETS 1\n", a little-endian
 * u32 manifest length, the JSON manifest, and after it the replayed pixels of each target that
 * differs, in the layout of the capture's own read-back of it, which is how they are decoded.
 */
bool WriteTargetData(const DxReplayReport& report, const std::string& path) {
    std::string payloads;
    std::string json = "{\"format\":\"gpu-inspector-replayed-targets\",\"version\":1,\"device\":" + JsonString(report.device) + ",\"targets\":[";
    for (size_t i = 0; i < report.targets.size(); ++i) {
        const DxTargetComparison& t = report.targets[i];
        json += std::string(i ? "," : "") + "{\"image\":" + std::to_string(t.resource) + ",\"commandBuffer\":" + std::to_string(t.commandList) +
                ",\"frame\":" + std::to_string(t.frame) + ",\"passIndex\":" + std::to_string(t.passIndex) + ",\"attachment\":" + std::to_string(t.attachment) +
                ",\"aspect\":" + JsonString(t.aspect) + ",\"format\":" + JsonString(t.format) + ",\"width\":" + std::to_string(t.width) +
                ",\"height\":" + std::to_string(t.height) + ",\"compared\":" + (t.compared ? "true" : "false") +
                ",\"texels\":" + std::to_string(t.texels) + ",\"differingTexels\":" + std::to_string(t.differingTexels) +
                ",\"maxByteDelta\":" + std::to_string(t.maxByteDelta);
        if (!t.note.empty()) json += ",\"note\":" + JsonString(t.note);
        if (t.differingTexels && !t.replayed.empty()) {
            json += ",\"payload\":[" + std::to_string(payloads.size()) + "," + std::to_string(t.replayed.size()) + "]";
            payloads.append(reinterpret_cast<const char*>(t.replayed.data()), t.replayed.size());
        }
        json += "}";
    }
    json += "],\"problems\":[";
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i) json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const uint32_t length = (uint32_t)json.size();
    out.write("TARGETS 1\n", 10);
    out.write(reinterpret_cast<const char*>(&length), 4);
    out.write(json.data(), (std::streamsize)json.size());
    out.write(payloads.data(), (std::streamsize)payloads.size());
    return (bool)out;
}

// --ablate: the request GPU Inspector writes (encodeAblationRequest in
// src/app/src/renderer/shader_ablation.ts), the file `vkinsp_replay --ablate` reads with DXIL
// containers where that one has SPIR-V: "ABLATE 1\n", a little-endian u32 manifest length, the JSON
//   {"rounds": 5, "targets": [{"command": 17, "stage": "fragment", "variants": [{"name": "fbm", "payload": [0, 7288]}]}]}
// and the variants' code, which the manifest names as [offset, length] after it.
bool ReadAblationRequest(const std::string& path, DxAblationOptions& options, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string magic = "ABLATE 1\n";
    if (bytes.size() < magic.size() + 4 || std::memcmp(bytes.data(), magic.data(), magic.size()) != 0) {
        error = "not an ablation request: " + path;
        return false;
    }
    uint32_t length = 0;
    std::memcpy(&length, bytes.data() + magic.size(), 4);
    const size_t start = magic.size() + 4;
    if (start + length > bytes.size()) {
        error = "the ablation request is truncated";
        return false;
    }
    vkreplay::JsonDocument doc;
    if (!doc.Parse(bytes.data() + start, length, error)) {
        error = "the ablation request's manifest is not valid JSON: " + error;
        return false;
    }
    const size_t base = start + length;
    const vkreplay::JValue& root = doc.Root();
    options.enabled = true;
    if (const vkreplay::JValue* rounds = root.Get("rounds")) options.rounds = std::clamp<uint32_t>((uint32_t)rounds->Uint(), 1, 64);
    const vkreplay::JValue* targets = root.Get("targets");
    for (uint32_t t = 0; targets && targets->IsArray() && t < targets->count; ++t) {
        const vkreplay::JValue& target = targets->items[t];
        DxAblationOptions::Target out;
        out.command = target.Get("command") ? (uint32_t)target.Get("command")->Uint() : 0;
        const vkreplay::JValue* stage = target.Get("stage");
        out.stage = stage && stage->IsString() ? std::string(stage->Str()) : std::string();
        if (const vkreplay::JValue* repeat = target.Get("repeat")) out.repeat = std::clamp<uint32_t>((uint32_t)repeat->Uint(), 1, 256);
        const vkreplay::JValue* variants = target.Get("variants");
        for (uint32_t v = 0; variants && variants->IsArray() && v < variants->count; ++v) {
            const vkreplay::JValue& variant = variants->items[v];
            DxAblationOptions::Variant vo;
            const vkreplay::JValue* name = variant.Get("name");
            vo.name = name && name->IsString() ? std::string(name->Str()) : std::string();
            const vkreplay::JValue* payload = variant.Get("payload");
            if (payload && payload->IsArray() && payload->count == 2) {
                const uint64_t offset = payload->items[0].Uint();
                const uint64_t size = payload->items[1].Uint();
                if (base + offset + size <= bytes.size()) vo.code.assign(bytes.begin() + (ptrdiff_t)(base + offset), bytes.begin() + (ptrdiff_t)(base + offset + size));
            }
            out.variants.push_back(std::move(vo));
        }
        options.targets.push_back(std::move(out));
    }
    return true;
}

/** --ablate-data: each target's timings (parseAblationResult in src/app/src/renderer/shader_ablation.ts). */
bool WriteAblationData(const DxReplayReport& report, const std::string& path) {
    auto timing = [](const DxAblationTiming& t) {
        char ms[32];
        std::snprintf(ms, sizeof(ms), "%.6f", t.ms);
        std::string s = "{\"name\":" + JsonString(t.name) + ",\"measured\":" + (t.measured ? "true" : "false") + ",\"ms\":" + ms + ",\"samples\":[";
        for (size_t i = 0; i < t.samples.size(); ++i) {
            std::snprintf(ms, sizeof(ms), "%.6f", t.samples[i]);
            s += std::string(i ? "," : "") + ms;
        }
        return s + "]" + (t.note.empty() ? "" : ",\"note\":" + JsonString(t.note)) + "}";
    };
    std::string json = "{\"format\":\"gpu-inspector-ablation\",\"version\":1,\"device\":" + JsonString(report.device) + ",\"targets\":[";
    for (size_t i = 0; i < report.ablations.size(); ++i) {
        const DxAblationResult& a = report.ablations[i];
        json += std::string(i ? "," : "") + "{\"command\":" + std::to_string(a.command) + ",\"stage\":" + JsonString(a.stage) +
                ",\"pipeline\":" + std::to_string(a.pipeline) + ",\"frame\":" + std::to_string(a.frame) + ",\"commandBuffer\":" +
                std::to_string(a.commandList) + ",\"passIndex\":" + std::to_string(a.passIndex) + ",\"rounds\":" + std::to_string(a.rounds) +
                ",\"repeat\":" + std::to_string(a.repeat) + ",\"baseline\":" + timing(a.baseline) + ",\"variants\":[";
        for (size_t v = 0; v < a.variants.size(); ++v) json += (v ? "," : "") + timing(a.variants[v]);
        json += "]" + (a.note.empty() ? std::string() : ",\"note\":" + JsonString(a.note)) + "}";
    }
    json += "],\"problems\":[";
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i) json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(json.data(), (std::streamsize)json.size());
    return (bool)out;
}

// --counter-data: the GPU's own hardware counters per pass (dx_counters.cpp), for GPU Inspector's
// GPU Bottlenecks report — the file `vkinsp_replay --counter-data` writes for a Vulkan capture
// (parseHwCounters in src/app/src/renderer/hw_counters.ts).

/** A double as JSON: a finite number, or null for NaN (a counter that did not evaluate) or infinity. */
std::string JsonNumber(double v) {
    if (std::isnan(v) || std::isinf(v)) return "null";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string CounterInfoJson(const DxCounterInfo& c) {
    return "{\"name\":" + JsonString(c.name) + ",\"description\":" + JsonString(c.description) +
           ",\"category\":" + JsonString(c.category) + ",\"unit\":" + JsonString(c.unit) +
           ",\"perDraw\":false}";
}

std::string CounterRangeJson(const DxCounterRange& r) {
    std::string values;
    for (size_t i = 0; i < r.values.size(); ++i) values += (i ? "," : "") + JsonNumber(r.values[i]);
    return "{\"command\":" + std::to_string(r.command) + ",\"frame\":" + std::to_string(r.frame) +
           ",\"commandBuffer\":" + std::to_string(r.commandBuffer) + ",\"passIndex\":" + std::to_string(r.passIndex) +
           ",\"values\":[" + values + "]}";
}

bool WriteCounterData(const DxReplayReport& report, const std::string& path) {
    const DxCounterReport& h = report.counters;
    std::string json = "{\"format\":\"gpu-inspector-hw-counters\",\"version\":1,\"device\":" + JsonString(report.device) +
                       ",\"backend\":" + JsonString(h.backend) + ",\"chip\":" + JsonString(h.chip) +
                       ",\"rounds\":" + std::to_string(h.rounds) + ",\"counters\":[";
    for (size_t i = 0; i < h.counters.size(); ++i) json += (i ? "," : "") + CounterInfoJson(h.counters[i]);
    json += "],\"passes\":[";
    for (size_t i = 0; i < h.passes.size(); ++i) json += (i ? "," : "") + CounterRangeJson(h.passes[i]);
    // A D3D12 capture measures no per-draw ranges: the draws of a pass are its own measurement
    // (Measure draws, src/d3d12/src/capture.cpp), and nesting a range per draw here would roughly
    // double the collection passes for numbers the app does not ask this tool for.
    json += "],\"draws\":[],\"available\":[";
    for (size_t i = 0; i < h.available.size(); ++i) json += (i ? "," : "") + CounterInfoJson(h.available[i]);
    json += "],\"notes\":[";
    for (size_t i = 0; i < h.notes.size() && i < 100; ++i) json += (i ? "," : "") + JsonString(h.notes[i]);
    json += "],\"problems\":[";
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i) json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(json.data(), (std::streamsize)json.size());
    return (bool)out;
}

void PrintGrouped(const std::vector<std::string>& lines, size_t limit) {
    std::map<std::string, int> grouped;
    for (const std::string& p : lines) ++grouped[p];
    size_t shown = 0;
    for (auto& [p, n] : grouped) {
        if (++shown > limit) {
            std::printf("  ... %zu more\n", grouped.size() - limit);
            break;
        }
        std::printf("  %s%s\n", p.c_str(), n > 1 ? (" (x" + std::to_string(n) + ")").c_str() : "");
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string exportData;
    std::string counterData;
    std::string drawData;
    std::string ablateRequest;
    std::string ablateData;
    std::string replaceRequest;
    std::string targetData;
    DxReplayOptions options;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--debug-layer")) options.debugLayer = true;
        else if (!std::strcmp(argv[i], "--trace")) options.trace = true;
        else if (!std::strcmp(argv[i], "--export") && i + 1 < argc) options.exportDir = argv[++i];
        else if (!std::strcmp(argv[i], "--export-data") && i + 1 < argc) exportData = argv[++i];
        else if (!std::strcmp(argv[i], "--counters")) options.counters.enabled = true;
        else if (!std::strcmp(argv[i], "--list-counters")) options.counters.enabled = options.counters.list = true;
        else if (!std::strcmp(argv[i], "--counter") && i + 1 < argc) options.counters.names.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--counter-data") && i + 1 < argc) counterData = argv[++i];
        else if (!std::strcmp(argv[i], "--draws")) options.drawStats = true;
        else if (!std::strcmp(argv[i], "--draw-data") && i + 1 < argc) { options.drawStats = true; drawData = argv[++i]; }
        else if (!std::strcmp(argv[i], "--ablate") && i + 1 < argc) ablateRequest = argv[++i];
        else if (!std::strcmp(argv[i], "--ablate-data") && i + 1 < argc) ablateData = argv[++i];
        else if (!std::strcmp(argv[i], "--replace") && i + 1 < argc) replaceRequest = argv[++i];
        else if (!std::strcmp(argv[i], "--target-data") && i + 1 < argc) targetData = argv[++i];
        else if (argv[i][0] != '-' && path.empty()) path = argv[i];
        else path.clear(), i = argc;
    }
    // Measuring and exporting are different replays: a measured frame issues work the capture never had.
    const bool measuring = options.drawStats || !ablateRequest.empty();
    if (path.empty() || (!exportData.empty() && options.exportDir.empty()) || (!counterData.empty() && !options.counters.enabled) ||
        (!ablateData.empty() && ablateRequest.empty()) || (measuring && (!options.exportDir.empty() || options.counters.enabled))) {
        std::fprintf(stderr, "usage: dxinsp_replay <capture.gpucap> [--debug-layer] [--trace] [--export <directory> [--export-data <file>]]\n"
                             "       dxinsp_replay <capture.gpucap> --counters [--counter <name>]... [--counter-data <file>]\n"
                             "       dxinsp_replay <capture.gpucap> --list-counters [--counter-data <file>]\n"
                             "       dxinsp_replay <capture.gpucap> [--draws [--draw-data <file>]] [--ablate <request> [--ablate-data <file>]]\n"
                             "       dxinsp_replay <capture.gpucap> --replace <request> [--target-data <file>] [--draws [--draw-data <file>]]\n");
        return 2;
    }
    if (!replaceRequest.empty()) {
        std::string error;
        if (!ReadReplaceRequest(replaceRequest, options.replacements, error)) {
            std::fprintf(stderr, "dxinsp_replay: %s\n", error.c_str());
            return 2;
        }
    }
    // The pixels of what differs are what --target-data is for.
    if (!targetData.empty()) options.keepPixels = true;
    if (!ablateRequest.empty()) {
        std::string error;
        if (!ReadAblationRequest(ablateRequest, options.ablation, error)) {
            std::fprintf(stderr, "dxinsp_replay: %s\n", error.c_str());
            return 2;
        }
    }
    g_exportDataPath = exportData;
    g_exportDir = options.exportDir;
    SetUnhandledExceptionFilter(OnCrash);
    vkreplay::CaptureFile capture;
    std::string error;
    if (!capture.Load(path, error)) {
        std::fprintf(stderr, "dxinsp_replay: %s\n", error.c_str());
        return 2;
    }
    DxReplayReport report;
    bool ran = false;
    {
        DxReplayer replayer;
        ran = replayer.Run(capture, options, report);
    }
    std::printf("device: %s\n", report.device.c_str());
    std::printf("objects: %zu created, %zu left out\n", report.objectsCreated, report.objectsSkipped);
    std::printf("uploads: %zu sampled textures, %zu buffer ranges; %zu descriptors written\n", report.texturesUploaded, report.bufferUploads, report.descriptorsWritten);
    std::printf("commands: %zu recorded in %zu submissions\n", report.commandsRecorded, report.submissions);
    size_t differing = 0, skipped = 0;
    std::printf("render targets: %zu\n", report.targets.size());
    for (const DxTargetComparison& t : report.targets) {
        std::printf("  resource %llu (command list %llu, pass %u, attachment %u, %s %ux%u %s): ", (unsigned long long)t.resource,
                    (unsigned long long)t.commandList, t.passIndex, t.attachment, t.format.c_str(), t.width, t.height, t.aspect.c_str());
        if (!t.compared) {
            if (!t.undefined) ++skipped;
            std::printf("not compared: %s\n", t.note.c_str());
        } else if (t.differingTexels == 0 && t.note.empty()) {
            std::printf("identical (%llu texels)\n", (unsigned long long)t.texels);
        } else {
            ++differing;
            std::printf("%llu of %llu texels differ, largest byte difference %u%s%s\n", (unsigned long long)t.differingTexels,
                        (unsigned long long)t.texels, t.maxByteDelta, t.note.empty() ? "" : "; ", t.note.c_str());
        }
    }
    if (report.exported.requested) {
        const DxExportReport& e = report.exported;
        if (!e.error.empty()) {
            std::printf("export to C++: failed: %s\n", e.error.c_str());
        } else {
            std::printf("export to C++: %s\n", e.directory.c_str());
            std::printf("  %zu objects, %zu commands in %zu submission%s, %zu render target%s compared, %.1f MB of data, %zu files\n", e.objects, e.commands,
                        e.submissions, e.submissions == 1 ? "" : "s", e.targets, e.targets == 1 ? "" : "s", e.dataBytes / (1024.0 * 1024.0), e.files.size());
            if (e.leftOut) std::printf("  %zu command%s left out, each with a comment where it would be\n", e.leftOut, e.leftOut == 1 ? "" : "s");
            for (size_t i = 0; i < e.notes.size() && i < 20; ++i) std::printf("  note: %s\n", e.notes[i].c_str());
        }
        if (!exportData.empty()) std::printf(WriteExportData(report, exportData) ? "  wrote %s\n" : "  could not write %s\n", exportData.c_str());
    }
    if (report.counters.requested) {
        const DxCounterReport& h = report.counters;
        if (options.counters.list) {
            std::printf("counters this GPU offers: %zu\n", h.available.size());
            for (size_t i = 0; i < h.available.size() && i < 20; ++i)
                std::printf("  %s (%s, %s)\n", h.available[i].name.c_str(), h.available[i].category.c_str(), h.available[i].unit.c_str());
            if (h.available.size() > 20) std::printf("  ... %zu more (--counter-data writes them all)\n", h.available.size() - 20);
        } else if (h.counters.empty()) {
            std::printf("hardware counters: none collected\n");
        } else {
            std::printf("hardware counters (%s%s%s): %zu counters over %u collection pass%s, %zu passes measured\n",
                        h.backend.c_str(), h.chip.empty() ? "" : ", ", h.chip.c_str(), h.counters.size(), h.rounds,
                        h.rounds == 1 ? "" : "es", h.passes.size());
            for (size_t i = 0; i < h.passes.size() && i < 10; ++i) {
                const DxCounterRange& r = h.passes[i];
                std::printf("  pass %u:", r.passIndex);
                for (size_t c = 0; c < h.counters.size() && c < r.values.size(); ++c)
                    std::printf(" %s=%s%s", h.counters[c].name.c_str(), JsonNumber(r.values[c]).c_str(),
                                h.counters[c].unit == "percent" ? "%" : "");
                std::printf("\n");
            }
        }
        for (size_t i = 0; i < h.notes.size() && i < 20; ++i) std::printf("  note: %s\n", h.notes[i].c_str());
        if (!counterData.empty())
            std::printf(WriteCounterData(report, counterData) ? "  wrote %s\n" : "  could not write %s\n", counterData.c_str());
    }
    if (!targetData.empty()) std::printf(WriteTargetData(report, targetData) ? "wrote %s\n" : "could not write %s\n", targetData.c_str());
    if (options.drawStats) {
        double total = 0;
        uint64_t fragments = 0;
        for (const DxDrawResult& d : report.draws) {
            total += d.durationMs;
            fragments += d.fragmentInvocations;
        }
        std::printf("draws measured: %zu, %.3f ms of draw time, %llu pixel shader invocations%s\n", report.draws.size(), total,
                    (unsigned long long)fragments, report.drawStatsNote.empty() ? "" : (" (" + report.drawStatsNote + ")").c_str());
        for (size_t i = 0; i < report.draws.size() && i < 20; ++i) {
            const DxDrawResult& d = report.draws[i];
            std::printf("  [%u] %.4f ms, %llu vertices, %llu primitives, %llu pixels, %llu compute, %llu samples passed\n", d.command, d.durationMs,
                        (unsigned long long)d.vertexInvocations, (unsigned long long)d.primitives, (unsigned long long)d.fragmentInvocations,
                        (unsigned long long)d.computeInvocations, (unsigned long long)d.samplesPassed);
        }
        if (report.draws.size() > 20) std::printf("  ... %zu more\n", report.draws.size() - 20);
        if (!drawData.empty()) std::printf(WriteDrawData(report, drawData) ? "  wrote %s\n" : "  could not write %s\n", drawData.c_str());
    }
    if (options.ablation.enabled) {
        std::printf("ablations: %zu\n", report.ablations.size());
        for (const DxAblationResult& a : report.ablations) {
            std::printf("  [%u] %s stage, pipeline %llu: ", a.command, a.stage.c_str(), (unsigned long long)a.pipeline);
            if (!a.baseline.measured) {
                std::printf("not measured: %s\n", a.note.c_str());
                continue;
            }
            std::printf("%.4f ms as captured (median of %u rounds)\n", a.baseline.ms, a.rounds);
            for (const DxAblationTiming& v : a.variants) {
                if (!v.measured) std::printf("    %-40s not measured%s%s\n", v.name.c_str(), v.note.empty() ? "" : ": ", v.note.c_str());
                else std::printf("    %-40s %.4f ms, saves %.4f ms\n", v.name.c_str(), v.ms, a.baseline.ms - v.ms);
            }
        }
        if (!ablateData.empty()) std::printf(WriteAblationData(report, ablateData) ? "  wrote %s\n" : "  could not write %s\n", ablateData.c_str());
    }
    std::printf("problems: %zu\n", report.problems.size());
    PrintGrouped(report.problems, 80);
    if (options.debugLayer) {
        std::printf("debug layer messages: %zu\n", report.messages.size());
        PrintGrouped(report.messages, 40);
    }
    if (!ran) return 2;
    // A measuring replay compares nothing, so it has nothing to differ: it ran, or it did not. And
    // a frame replayed with an edited shader is meant to differ.
    if (measuring || !replaceRequest.empty()) return 0;
    return differing == 0 && skipped == 0 ? 0 : 1;
}
