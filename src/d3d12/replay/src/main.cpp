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
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

#include "gpucap.h"

#include "dx_replayer.h"

using namespace dxreplay;

namespace {

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
    DxReplayOptions options;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--debug-layer")) options.debugLayer = true;
        else if (!std::strcmp(argv[i], "--trace")) options.trace = true;
        else if (!std::strcmp(argv[i], "--export") && i + 1 < argc) options.exportDir = argv[++i];
        else if (!std::strcmp(argv[i], "--export-data") && i + 1 < argc) exportData = argv[++i];
        else if (argv[i][0] != '-' && path.empty()) path = argv[i];
        else path.clear(), i = argc;
    }
    if (path.empty() || (!exportData.empty() && options.exportDir.empty())) {
        std::fprintf(stderr, "usage: dxinsp_replay <capture.gpucap> [--debug-layer] [--trace] [--export <directory> [--export-data <file>]]\n");
        return 2;
    }
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
    std::printf("problems: %zu\n", report.problems.size());
    PrintGrouped(report.problems, 80);
    if (options.debugLayer) {
        std::printf("debug layer messages: %zu\n", report.messages.size());
        PrintGrouped(report.messages, 40);
    }
    if (!ran) return 2;
    return differing == 0 && skipped == 0 ? 0 : 1;
}
