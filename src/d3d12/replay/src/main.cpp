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
#include <windows.h>

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
        else if (argv[i][0] != '-' && path.empty()) path = argv[i];
        else path.clear(), i = argc;
    }
    if (path.empty() || (!exportData.empty() && options.exportDir.empty()) || (!counterData.empty() && !options.counters.enabled)) {
        std::fprintf(stderr, "usage: dxinsp_replay <capture.gpucap> [--debug-layer] [--trace] [--export <directory> [--export-data <file>]]\n"
                             "       dxinsp_replay <capture.gpucap> --counters [--counter <name>]... [--counter-data <file>]\n"
                             "       dxinsp_replay <capture.gpucap> --list-counters [--counter-data <file>]\n");
        return 2;
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
    std::printf("problems: %zu\n", report.problems.size());
    PrintGrouped(report.problems, 80);
    if (options.debugLayer) {
        std::printf("debug layer messages: %zu\n", report.messages.size());
        PrintGrouped(report.messages, 40);
    }
    if (!ran) return 2;
    return differing == 0 && skipped == 0 ? 0 : 1;
}
