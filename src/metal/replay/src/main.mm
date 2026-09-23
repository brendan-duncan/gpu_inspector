// mtlinsp_replay: re-executes a Metal capture (.gpucap) on this machine's GPU without the
// application, compares every render target the capture read back, and writes the frame out as a
// standalone Objective-C++ project. The Metal counterpart of vkinsp_replay and dxinsp_replay.
// docs/REPLAY.md, "Metal".
//
//   mtlinsp_replay <capture.gpucap> [--validate] [--dump <dir>] [--trace]
//                  [--export <directory> [--export-data <file>]]
//
// Exit code 0 when every compared target is identical to the capture's, 1 when some differ or could
// not be compared, and 2 when the replay could not run.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#import <Foundation/Foundation.h>

#include "gpucap.h"

#include "mtl_replayer.h"

namespace
{

using mtlreplay::MtlExportReport;
using mtlreplay::MtlReplayOptions;
using mtlreplay::MtlReplayReport;
using mtlreplay::MtlTargetComparison;

std::string JsonString(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += (char)c;
        }
        else if (c == '\n')
            out += "\\n";
        else if (c < 0x20)
        {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\u%04x", c);
            out += esc;
        }
        else
            out += (char)c;
    }
    return out + "\"";
}

/** The summary Export to C++ reads back (parseExportSummary in src/app/src/renderer/export_cpp.ts). */
bool WriteExportData(const MtlReplayReport& report, const std::string& path)
{
    const MtlExportReport& e = report.exported;
    std::string json = "{\"format\":\"gpu-inspector-export-cpp\",\"version\":1,\"device\":" + JsonString(report.device) +
        ",\"directory\":" + JsonString(e.directory) + ",\"ok\":" + (e.error.empty() ? "true" : "false") +
        ",\"error\":" + JsonString(e.error) + ",\"objects\":" + std::to_string(e.objects) +
        ",\"commands\":" + std::to_string(e.commands) + ",\"submissions\":" + std::to_string(e.submissions) +
        ",\"targets\":" + std::to_string(e.targets) + ",\"leftOut\":" + std::to_string(e.leftOut) +
        ",\"dataBytes\":" + std::to_string(e.dataBytes) + ",\"files\":[";
    for (size_t i = 0; i < e.files.size(); ++i)
        json += (i ? "," : "") + JsonString(e.files[i]);
    json += "],\"notes\":[";
    for (size_t i = 0; i < e.notes.size() && i < 100; ++i)
        json += (i ? "," : "") + JsonString(e.notes[i]);
    json += "],\"problems\":[";
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i)
        json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write(json.data(), (std::streamsize)json.size());
    return (bool)out;
}

void PrintGrouped(const std::vector<std::string>& lines, size_t limit)
{
    std::map<std::string, int> grouped;
    for (const std::string& p : lines)
        ++grouped[p];
    size_t shown = 0;
    for (const auto& [p, n] : grouped)
    {
        if (++shown > limit)
        {
            std::printf("  ... %zu more\n", grouped.size() - limit);
            break;
        }
        std::printf("  %s%s\n", p.c_str(), n > 1 ? (" (x" + std::to_string(n) + ")").c_str() : "");
    }
}

/** A comparison's two copies as .raw files, for --dump. */
void DumpTarget(const std::string& directory, const MtlTargetComparison& t)
{
    const std::string base = directory + "/cb" + std::to_string(t.commandBuffer) + "_pass" +
        std::to_string(t.passIndex) + "_texture" + std::to_string(t.texture) + "_" + t.aspect;
    for (const auto& [suffix, bytes] : {std::pair{"_captured.raw", &t.captured}, std::pair{"_replayed.raw", &t.replayed}})
    {
        if (bytes->empty())
            continue;
        std::ofstream out(base + suffix, std::ios::binary);
        out.write((const char*)bytes->data(), (std::streamsize)bytes->size());
    }
}

void Usage()
{
    std::fprintf(stderr,
        "usage: mtlinsp_replay <capture.gpucap> [--validate] [--dump <dir>] [--trace]\n"
        "                      [--export <directory> [--export-data <file>]]\n");
}

} // namespace

int main(int argc, const char** argv)
{
    @autoreleasepool
    {
        std::string capturePath, dumpDir, exportData;
        MtlReplayOptions options;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--validate")
                options.validate = true;
            else if (arg == "--trace")
                options.trace = true;
            else if (arg == "--dump" && i + 1 < argc)
            {
                dumpDir = argv[++i];
                options.keepPixels = true;
            }
            else if (arg == "--export" && i + 1 < argc)
                options.exportDir = argv[++i];
            else if (arg == "--export-data" && i + 1 < argc)
                exportData = argv[++i];
            else if (!arg.empty() && arg[0] == '-')
            {
                Usage();
                return 2;
            }
            else if (capturePath.empty())
                capturePath = arg;
            else
            {
                Usage();
                return 2;
            }
        }
        if (capturePath.empty())
        {
            Usage();
            return 2;
        }

        // Metal's API validation is a process-wide environment variable, read when the first device
        // is made — so it has to be set before MTLCreateSystemDefaultDevice, which is why this is
        // here rather than in the replayer.
        if (options.validate)
            setenv("METAL_DEVICE_WRAPPER_TYPE", "1", 1);

        vkreplay::CaptureFile capture;
        std::string error;
        if (!capture.Load(capturePath, error))
        {
            std::fprintf(stderr, "%s: %s\n", capturePath.c_str(), error.c_str());
            return 2;
        }
        const vkreplay::JValue* api = capture.Manifest().Get("api");
        if (api && api->IsString() && api->Str() != "metal")
        {
            std::fprintf(stderr, "%s is a %.*s capture; mtlinsp_replay replays Metal captures\n",
                capturePath.c_str(), (int)api->Str().size(), api->Str().data());
            return 2;
        }

        MtlReplayReport report;
        mtlreplay::MtlReplayer replayer(capture, options);
        if (!replayer.Run(report))
        {
            for (const std::string& p : report.problems)
                std::fprintf(stderr, "%s\n", p.c_str());
            return 2;
        }

        std::printf("device: %s\n", report.device.c_str());
        if (!report.capturedDevice.empty() && report.capturedDevice != report.device)
        {
            std::printf("captured on: %s\n", report.capturedDevice.c_str());
        }
        std::printf("objects: %zu, commands: %zu, command buffers: %zu\n", report.objects, report.commands,
            report.submissions);

        size_t identical = 0, differing = 0, skipped = 0;
        if (!dumpDir.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(dumpDir, ec);
        }
        for (const MtlTargetComparison& t : report.comparisons)
        {
            std::printf("texture %llu (%s %ux%u %s, pass %u of command buffer %llu): ",
                (unsigned long long)t.texture, t.format.c_str(), t.width, t.height, t.aspect.c_str(),
                t.passIndex, (unsigned long long)t.commandBuffer);
            if (!t.compared)
            {
                std::printf("not compared: %s\n", t.note.c_str());
                if (!t.undefined)
                    ++skipped;
            }
            else if (!t.differingTexels)
            {
                std::printf("identical\n");
                ++identical;
            }
            else
            {
                std::printf("%llu of %llu texels differ, largest byte delta %u\n",
                    (unsigned long long)t.differingTexels, (unsigned long long)t.texels, t.maxByteDelta);
                ++differing;
            }
            if (!dumpDir.empty())
                DumpTarget(dumpDir, t);
        }
        if (!dumpDir.empty())
            std::printf("wrote the compared targets to %s/\n", dumpDir.c_str());

        if (!options.exportDir.empty())
        {
            const MtlExportReport& e = report.exported;
            if (e.error.empty())
            {
                std::printf("exported to %s: %zu objects, %zu commands, %llu bytes of data%s\n", e.directory.c_str(),
                    e.objects, e.commands, (unsigned long long)e.dataBytes,
                    e.leftOut ? (", " + std::to_string(e.leftOut) + " commands left out").c_str() : "");
            }
            else
            {
                std::printf("export failed: %s\n", e.error.c_str());
            }
            if (!exportData.empty())
            {
                std::printf(WriteExportData(report, exportData) ? "  wrote %s\n" : "  could not write %s\n",
                    exportData.c_str());
            }
        }

        std::printf("problems: %zu\n", report.problems.size());
        PrintGrouped(report.problems, 80);
        // Metal's API validation has no message list to read, unlike D3D12's info queue: it logs
        // to stderr and aborts on a hard error, so there is nothing to count here.
        if (options.validate)
            std::printf("Metal's API validation is on; its messages are on stderr above\n");
        std::printf("%zu identical, %zu differing, %zu not compared\n", identical, differing, skipped);
        return differing == 0 && skipped == 0 ? 0 : 1;
    }
}
