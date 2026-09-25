// vkinsp_replay: re-executes a GPU Inspector capture (.gpucap) on this machine's GPU.
//
//   vkinsp_replay <capture.gpucap> [--validate]
//       Re-creates the capture's objects, replays its command buffers, and compares every render
//       target the capture read back with the replay's own copy at the same point. Exit code 0
//       when every compared target matches exactly, 1 when some differ or could not be compared,
//       2 when the replay could not run.
//
//   vkinsp_replay <capture.gpucap> --pixel <image id> <x> <y> [--mip <n>] [--layer <n>]
//       Also follows one pixel of an image through the frame: every pass that renders to it, and
//       every draw and clear in those passes, with what each draw's fragments at the pixel met
//       and the pixel's value after it.
//
//   vkinsp_replay <capture.gpucap> --check
//       Decodes every object's creation arguments and every command's arguments with the
//       generated decoders, and reports what the capture lacks for a replay.
//
//   vkinsp_replay <capture.gpucap> --export <directory>
//       Export to C++: also writes the frame, as it is replayed, as a standalone C++ project that
//       re-creates its objects and re-issues its commands (exporter.h), mainly for driver bug reports.
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "decode.h"
#include "gpucap.h"
#include "json.h"
#include "replayer.h"
#include "util.h"
#include "vk_decode.gen.h"

using namespace vkreplay;

namespace
{

void PrintUsage()
{
    std::fprintf(stderr,
        "usage: vkinsp_replay <capture.gpucap> [--validate [--validate-data <file>]] [--dump <dir>] [--overdraw <dir>] [--overdraw-data <file>]\n"
        "                     [--pixel <image> <x> <y> [--mip <n>] [--layer <n>] [--pixel-data <file>]]\n"
        "                     [--draws [--draw-data <file>]] [--overlay <command> ... [--overlay-data <file>]]\n"
        "                     [--mesh <command> ... [--mesh-data <file>]] [--ablate <request> [--ablate-data <file>]]\n"
        "                     [--counters [--counter <name>]... [--counter-draws] [--counter-backend nvperf|khr] [--counter-data <file>]]\n"
        "                     [--list-counters [--counter-data <file>]]\n"
        "                     [--export <directory> [--export-data <file>]]\n"
        "                     [--replace <request> [--target-data <file>]]\n"
        "                     [--trace] | --check | --serve [--validate]\n");
}

// ---------------------------------------------------------------------------------------------
// --overdraw-data: every overdraw measurement with its per-pixel counts, for GPU Inspector to show
// (parseOverdrawFile in src/app/src/renderer/overdraw.ts). The layout is the capture file's: a magic
// line, a little-endian u32 with the length of a JSON manifest, the manifest, then the counts of
// each measurement (u16 little endian, row by row), which the manifest names as [offset, length].

std::string JsonString(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char ch : s)
    {
        if (ch == '"' || ch == '\\')
        {
            out += '\\';
            out += (char)ch;
        }
        else if (ch < 0x20)
        {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
            out += escaped;
        }
        else
        {
            out += (char)ch;
        }
    }
    return out + "\"";
}

/**
 * --validate-data: the validation layer's messages with where the replay was when each fired,
 * for the Validate report (renderer/replay_validation.ts) and the MCP's get_validation. Repeats of
 * one message at one command are counted rather than listed.
 */
bool WriteValidationData(const ReplayReport& report, bool layerFound, const std::string& path)
{
    std::string json = "{\"format\":\"gpu-inspector-validation\",\"version\":1,\"device\":" + JsonString(report.device) +
        ",\"layer\":" + (layerFound ? "true" : "false") + ",\"messages\":[";
    struct Counted
    {
        const ValidationRecord* r;
        size_t count;
    };
    std::vector<Counted> counted;
    for (const ValidationRecord& r : report.validationRecords)
    {
        bool seen = false;
        for (Counted& c : counted)
        {
            if (c.r->error == r.error && c.r->id == r.id && c.r->command == r.command && c.r->phase == r.phase && c.r->message == r.message)
            {
                ++c.count;
                seen = true;
                break;
            }
        }
        if (!seen)
            counted.push_back({&r, 1});
    }
    for (size_t i = 0; i < counted.size(); ++i)
    {
        const ValidationRecord& r = *counted[i].r;
        json += std::string(i ? "," : "") + "{\"severity\":" + (r.error ? "\"error\"" : "\"warning\"") + ",\"id\":" + JsonString(r.id) +
            ",\"message\":" + JsonString(r.message) + ",\"command\":" + std::to_string(r.command) + ",\"phase\":" + JsonString(r.phase) +
            ",\"count\":" + std::to_string(counted[i].count) + "}";
    }
    json += "],\"problems\":[";
    for (size_t i = 0; i < report.problems.size(); ++i)
        json += std::string(i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write(json.data(), (std::streamsize)json.size());
    return out.good();
}

bool WriteOverdrawData(const ReplayReport& report, const std::string& path)
{
    std::string json = "{\"format\":\"gpu-inspector-overdraw\",\"version\":1,\"device\":" + JsonString(report.device) + ",\"passes\":[";
    uint64_t offset = 0;
    for (size_t i = 0; i < report.overdraw.size(); ++i)
    {
        const OverdrawResult& o = report.overdraw[i];
        const uint64_t size = (uint64_t)o.counts.size() * 2;
        std::string histogram;
        for (size_t b = 0; b < o.histogram.size(); ++b)
            histogram += (b ? "," : "") + std::to_string(o.histogram[b]);
        json += std::string(i ? "," : "") + "{\"frame\":" + std::to_string(o.frame) + ",\"commandBuffer\":" + std::to_string(o.commandBuffer) +
            ",\"passIndex\":" + std::to_string(o.passIndex) + ",\"depthTested\":" + (o.depthTested ? "true" : "false") +
            ",\"measured\":" + (o.counts.empty() ? "false" : "true") + ",\"width\":" + std::to_string(o.width) +
            ",\"height\":" + std::to_string(o.height) + ",\"fragments\":" + std::to_string(o.fragments) +
            ",\"coveredPixels\":" + std::to_string(o.coveredPixels) + ",\"maxCount\":" + std::to_string(o.maxCount) +
            ",\"draws\":" + std::to_string(o.draws) + ",\"skippedDraws\":" + std::to_string(o.skippedDraws) +
            ",\"histogram\":[" + histogram + "],\"size\":" + std::to_string(size);
        if (o.capturedFragments >= 0)
            json += ",\"capturedFragments\":" + std::to_string(o.capturedFragments);
        if (!o.note.empty())
            json += ",\"note\":" + JsonString(o.note);
        if (size)
            json += ",\"payload\":[" + std::to_string(offset) + "," + std::to_string(size) + "]";
        json += "}";
        offset += size;
    }
    json += "],\"problems\":[";
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i)
        json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";

    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    const char magic[] = "OVERDRAW 1\n";
    out.write(magic, sizeof(magic) - 1);
    const uint32_t length = (uint32_t)json.size();
    const uint8_t le[4] = {(uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16), (uint8_t)(length >> 24)};
    out.write((const char*)le, 4);
    out.write(json.data(), (std::streamsize)json.size());
    std::vector<uint8_t> bytes;
    for (const OverdrawResult& o : report.overdraw)
    {
        bytes.resize(o.counts.size() * 2);
        for (size_t p = 0; p < o.counts.size(); ++p)
        {
            bytes[p * 2] = (uint8_t)(o.counts[p] & 0xFF);
            bytes[p * 2 + 1] = (uint8_t)(o.counts[p] >> 8);
        }
        out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    }
    return (bool)out;
}

// ---------------------------------------------------------------------------------------------
// --pixel: the pixel's history.

/** A texel's bytes as values, for the formats render targets commonly have; hex otherwise. */
std::string FormatTexel(const std::string& format, const std::vector<uint8_t>& b, bool depthAspect)
{
    if (b.empty())
        return "?";
    auto has = [&](const char* s) { return format.find(s) != std::string::npos; };
    auto f32 = [&](size_t o) { float v = 0; if (o + 4 <= b.size()) std::memcpy(&v, &b[o], 4); return v; };
    auto u16 = [&](size_t o) { return o + 2 <= b.size() ? (uint16_t)(b[o] | (b[o + 1] << 8)) : (uint16_t)0; };
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= b.size()) std::memcpy(&v, &b[o], 4); return v; };
    char text[200];
    if (depthAspect)
    {
        if (has("D32_SFLOAT"))
            std::snprintf(text, sizeof(text), "%.6f", f32(0));
        else if (has("D24_UNORM") || has("X8_D24"))
            std::snprintf(text, sizeof(text), "%.6f", (u32(0) & 0xFFFFFF) / 16777215.0);
        else if (has("D16_UNORM"))
            std::snprintf(text, sizeof(text), "%.6f", u16(0) / 65535.0);
        else
            goto hex;
        return text;
    }
    if ((has("R8G8B8A8_") || has("B8G8R8A8_")) && b.size() >= 4)
    {
        const bool bgr = has("B8G8R8A8_");
        const uint8_t r = b[bgr ? 2 : 0], g = b[1], bl = b[bgr ? 0 : 2], a = b[3];
        std::snprintf(text, sizeof(text), "rgba(%u, %u, %u, %u) = (%.3f, %.3f, %.3f, %.3f)", r, g, bl, a, r / 255.0, g / 255.0, bl / 255.0, a / 255.0);
        return text;
    }
    if (has("R16G16B16A16_SFLOAT") && b.size() >= 8)
    {
        std::snprintf(text, sizeof(text), "(%.4f, %.4f, %.4f, %.4f)", HalfToFloat(u16(0)), HalfToFloat(u16(2)), HalfToFloat(u16(4)), HalfToFloat(u16(6)));
        return text;
    }
    if (has("R32G32B32A32_SFLOAT") && b.size() >= 16)
    {
        std::snprintf(text, sizeof(text), "(%.4f, %.4f, %.4f, %.4f)", f32(0), f32(4), f32(8), f32(12));
        return text;
    }
    if (format == "VK_FORMAT_R32_SFLOAT")
    {
        std::snprintf(text, sizeof(text), "%.6f", f32(0));
        return text;
    }
    if (format == "VK_FORMAT_R16_SFLOAT")
    {
        std::snprintf(text, sizeof(text), "%.4f", HalfToFloat(u16(0)));
        return text;
    }
    if (has("B10G11R11_UFLOAT_PACK32"))
    {
        // Unsigned small floats: 6-bit (red, green) or 5-bit (blue) mantissa over a 5-bit exponent, biased by 15.
        auto small = [](uint32_t bits, int mantissaBits) {
            const uint32_t mantissa = bits & ((1u << mantissaBits) - 1);
            const int exponent = (int)(bits >> mantissaBits) & 0x1F;
            if (exponent == 31)
                return mantissa ? (double)NAN : (double)INFINITY;
            if (exponent == 0)
                return std::ldexp((double)mantissa / (1u << mantissaBits), -14);
            return std::ldexp(1.0 + (double)mantissa / (1u << mantissaBits), exponent - 15);
        };
        const uint32_t v = u32(0);
        std::snprintf(text, sizeof(text), "(%.4f, %.4f, %.4f)", small(v & 0x7FF, 6), small((v >> 11) & 0x7FF, 6), small(v >> 22, 5));
        return text;
    }
    if (has("A2B10G10R10_UNORM") || has("A2R10G10B10_UNORM"))
    {
        const uint32_t v = u32(0);
        const bool bgr = has("A2R10G10B10");
        const uint32_t lo = v & 0x3FF, mid = (v >> 10) & 0x3FF, hi = (v >> 20) & 0x3FF;
        std::snprintf(text, sizeof(text), "(%.3f, %.3f, %.3f, %.3f)", (bgr ? hi : lo) / 1023.0, mid / 1023.0, (bgr ? lo : hi) / 1023.0, (v >> 30) / 3.0);
        return text;
    }
hex:
    std::string out = "bytes";
    for (uint8_t byte : b)
    {
        std::snprintf(text, sizeof(text), " %02x", byte);
        out += text;
    }
    return out;
}

/** What a draw's fragments at the pixel met, from its occlusion queries. */
std::string DrawOutcome(const PixelEvent& e)
{
    auto measured = [&](int bit) { return (e.testsMeasured >> bit) & 1; };
    if (e.scissored)
        return "outside the scissor";
    if (!e.testsMeasured)
        return "not measured (the draw's pipeline could not be copied)";
    if (measured(0) && !e.covered)
        return "does not cover the pixel";
    if (measured(1) && !e.facing)
        return "culled";
    if (measured(2) && !e.shaded)
        return "discarded by the fragment shader";
    const bool depthFailed = measured(3) && !e.depthPassed;
    const bool stencilFailed = measured(4) && !e.stencilPassed;
    if (depthFailed && stencilFailed)
        return "failed the depth and stencil tests";
    if (depthFailed)
        return "failed the depth test";
    if (stencilFailed)
        return "failed the stencil test";
    if (measured(5) && !e.passed)
        return "failed the depth and stencil tests together";
    // Samples of every fragment the draw put there, tested against the depth and stencil from before the draw.
    if (measured(5))
        return "wrote the pixel (" + std::to_string(e.passed) + (e.passed == 1 ? " sample passed)" : " samples passed)") + (e.primitive >= 0 ? ", primitive " + std::to_string(e.primitive) : "") + (e.fragments.empty() ? "" : ", " + std::to_string(e.fragments.size()) + " fragments");
    return "covers the pixel";
}

/**
 * --pixel-data: the history as JSON, for GPU Inspector to show (parsePixelHistory in
 * src/app/src/renderer/pixel_history.ts). Every event is kept, the draws that do not reach the pixel
 * too; texels are hex strings of the bytes the replay read, in the formats it names.
 */
bool WritePixelHistoryData(const ReplayReport& report, const std::string& path)
{
    const PixelHistoryResult& h = report.history;
    auto hex = [](const std::vector<uint8_t>& bytes) {
        static const char digits[] = "0123456789abcdef";
        std::string out;
        for (uint8_t b : bytes)
        {
            out += digits[b >> 4];
            out += digits[b & 15];
        }
        return "\"" + out + "\"";
    };
    auto strings = [](const std::vector<std::string>& list, size_t limit) {
        std::string out = "[";
        for (size_t i = 0; i < list.size() && i < limit; ++i)
            out += (i ? "," : "") + JsonString(list[i]);
        return out + "]";
    };
    std::string json = "{\"format\":\"gpu-inspector-pixel-history\",\"version\":1,\"device\":" + JsonString(report.device) +
        ",\"image\":" + std::to_string(h.image) + ",\"x\":" + std::to_string(h.x) + ",\"y\":" + std::to_string(h.y) +
        ",\"mip\":" + std::to_string(h.mip) + ",\"layer\":" + std::to_string(h.layer) +
        ",\"pixelFormat\":" + JsonString(h.format) + ",\"depthFormat\":" + JsonString(h.depthFormat) + ",\"events\":[";
    for (size_t i = 0; i < h.events.size(); ++i)
    {
        const PixelEvent& e = h.events[i];
        json += std::string(i ? "," : "") + "{\"kind\":" + JsonString(e.kind) + ",\"command\":" + std::to_string(e.command) +
            ",\"method\":" + JsonString(e.method) + ",\"detail\":" + JsonString(e.detail) +
            ",\"commandBuffer\":" + std::to_string(e.commandBuffer) + ",\"frame\":" + std::to_string(e.frame) +
            ",\"passIndex\":" + std::to_string(e.passIndex) + ",\"pipeline\":" + std::to_string(e.pipeline) +
            ",\"scissored\":" + (e.scissored ? "true" : "false") +
            ",\"earlyTests\":" + (e.earlyTests ? "true" : "false") + ",\"primitive\":" + std::to_string(e.primitive) +
            ",\"testsMeasured\":" + std::to_string(e.testsMeasured) +
            ",\"covered\":" + std::to_string(e.covered) + ",\"facing\":" + std::to_string(e.facing) +
            ",\"shaded\":" + std::to_string(e.shaded) + ",\"depthPassed\":" + std::to_string(e.depthPassed) +
            ",\"stencilPassed\":" + std::to_string(e.stencilPassed) + ",\"passed\":" + std::to_string(e.passed) +
            ",\"value\":" + hex(e.value) + ",\"depth\":" + hex(e.depth) + ",\"fragments\":[";
        for (size_t f = 0; f < e.fragments.size(); ++f)
        {
            const PixelFragment& fragment = e.fragments[f];
            json += std::string(f ? "," : "") + "{\"primitive\":" + std::to_string(fragment.primitive) +
                ",\"value\":" + hex(fragment.value) + "}";
        }
        json += "]}";
    }
    json += "],\"notes\":" + strings(h.notes, 100) + ",\"problems\":" + strings(report.problems, 100) + "}";
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write(json.data(), (std::streamsize)json.size());
    return (bool)out;
}

/**
 * --draw-data: every draw and dispatch of the frame with the time it took and the counters it ran
 * up, for GPU Inspector's Shader Flame Graph (parseDrawStats in src/app/src/renderer/draw_stats.ts).
 */
bool WriteDrawData(const ReplayReport& report, const std::string& path)
{
    std::string json = "{\"format\":\"gpu-inspector-draw-stats\",\"version\":1,\"device\":" + JsonString(report.device) +
        ",\"note\":" + JsonString(report.drawStatsNote) + ",\"draws\":[";
    for (size_t i = 0; i < report.draws.size(); ++i)
    {
        const DrawResult& d = report.draws[i];
        char ms[32];
        std::snprintf(ms, sizeof(ms), "%.6f", d.durationMs);
        json += std::string(i ? "," : "") + "{\"command\":" + std::to_string(d.command) + ",\"frame\":" + std::to_string(d.frame) +
            ",\"commandBuffer\":" + std::to_string(d.commandBuffer) + ",\"passIndex\":" + std::to_string(d.passIndex) +
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
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i)
        json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write(json.data(), (std::streamsize)json.size());
    return (bool)out;
}

// ---------------------------------------------------------------------------------------------
// --counter-data: the GPU's own hardware counters per pass and per draw (hw_counters.cpp), for GPU
// Inspector's GPU Bottlenecks report (parseHwCounters in src/app/src/renderer/hw_counters.ts).

/** A double as JSON: a finite number, or null for NaN (a counter that did not evaluate) or infinity. */
std::string JsonNumber(double v)
{
    if (std::isnan(v) || std::isinf(v))
        return "null";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string CounterInfoJson(const HwCounterInfo& c)
{
    return "{\"name\":" + JsonString(c.name) + ",\"description\":" + JsonString(c.description) +
        ",\"category\":" + JsonString(c.category) + ",\"unit\":" + JsonString(c.unit) +
        ",\"perDraw\":" + (c.perDraw ? "true" : "false") + "}";
}

std::string CounterRangeJson(const HwCounterRange& r)
{
    std::string values;
    for (size_t i = 0; i < r.values.size(); ++i)
        values += (i ? "," : "") + JsonNumber(r.values[i]);
    std::string out = "{\"command\":" + std::to_string(r.command) + ",\"frame\":" + std::to_string(r.frame) +
        ",\"commandBuffer\":" + std::to_string(r.commandBuffer) + ",\"passIndex\":" + std::to_string(r.passIndex) +
        ",\"values\":[" + values + "]}";
    return out;
}

bool WriteCounterData(const ReplayReport& report, const std::string& path)
{
    const HwCounterReport& h = report.counters;
    std::string json = "{\"format\":\"gpu-inspector-hw-counters\",\"version\":1,\"device\":" + JsonString(report.device) +
        ",\"backend\":" + JsonString(h.backend) + ",\"chip\":" + JsonString(h.chip) +
        ",\"rounds\":" + std::to_string(h.rounds) + ",\"counters\":[";
    for (size_t i = 0; i < h.counters.size(); ++i)
        json += (i ? "," : "") + CounterInfoJson(h.counters[i]);
    json += "],\"passes\":[";
    for (size_t i = 0; i < h.passes.size(); ++i)
        json += (i ? "," : "") + CounterRangeJson(h.passes[i]);
    json += "],\"draws\":[";
    for (size_t i = 0; i < h.draws.size(); ++i)
        json += (i ? "," : "") + CounterRangeJson(h.draws[i]);
    json += "],\"available\":[";
    for (size_t i = 0; i < h.available.size(); ++i)
        json += (i ? "," : "") + CounterInfoJson(h.available[i]);
    json += "],\"notes\":[";
    for (size_t i = 0; i < h.notes.size() && i < 100; ++i)
        json += (i ? "," : "") + JsonString(h.notes[i]);
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

// ---------------------------------------------------------------------------------------------
// --export: the frame as a C++ project (exporter.h). --export-data is the summary GPU Inspector reads
// (parseExportSummary in src/app/src/renderer/export_cpp.ts).

bool WriteExportData(const ReplayReport& report, const std::string& path)
{
    const ExportReport& e = report.exported;
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

void PrintExport(const ReplayReport& report)
{
    const ExportReport& e = report.exported;
    if (!e.error.empty())
    {
        std::printf("export to C++: failed: %s\n", e.error.c_str());
        return;
    }
    std::printf("export to C++: %s\n", e.directory.c_str());
    std::printf("  %zu objects, %zu commands in %zu submission%s, %zu render target%s compared, %.1f MB of data, %zu files\n", e.objects,
        e.commands, e.submissions, e.submissions == 1 ? "" : "s", e.targets, e.targets == 1 ? "" : "s", e.dataBytes / (1024.0 * 1024.0),
        e.files.size());
    if (e.leftOut)
        std::printf("  %zu command%s left out, each with a comment where it would be\n", e.leftOut, e.leftOut == 1 ? "" : "s");
    for (size_t i = 0; i < e.notes.size() && i < 20; ++i)
        std::printf("  note: %s\n", e.notes[i].c_str());
    std::printf("  build it: cmake -S \"%s\" -B \"%s/build\" && cmake --build \"%s/build\" --config Release\n", e.directory.c_str(),
        e.directory.c_str(), e.directory.c_str());
}

void PrintCounters(const ReplayReport& report)
{
    const HwCounterReport& h = report.counters;
    if (h.backend.empty())
    {
        std::printf("hardware counters: none available\n");
    }
    else
    {
        std::printf("hardware counters (%s%s%s): %zu counters over %u collection pass%s, %zu passes and %zu draws measured\n",
            h.backend.c_str(), h.chip.empty() ? "" : ", ", h.chip.c_str(), h.counters.size(), h.rounds,
            h.rounds == 1 ? "" : "es", h.passes.size(), h.draws.size());
    }
    if (!h.available.empty())
    {
        std::printf("counters this GPU offers: %zu\n", h.available.size());
        for (size_t i = 0; i < h.available.size(); ++i)
            std::printf("  %-56s %-10s %s\n", h.available[i].name.c_str(), h.available[i].unit.c_str(), h.available[i].category.c_str());
        return;
    }
    // The slowest few passes' counters, as the tool's own quick look.
    for (size_t i = 0; i < h.passes.size() && i < 8; ++i)
    {
        const HwCounterRange& r = h.passes[i];
        std::printf("  pass %u:", r.passIndex);
        for (size_t c = 0; c < h.counters.size() && c < r.values.size(); ++c)
            std::printf(" %s=%s%s", h.counters[c].name.c_str(), JsonNumber(r.values[c]).c_str(), h.counters[c].unit == "percent" ? "%" : "");
        std::printf("\n");
    }
    for (const std::string& note : h.notes)
        std::printf("  note: %s\n", note.c_str());
}

// ---------------------------------------------------------------------------------------------
// --replace: the frame replayed with other code for some pipelines' stages, which is a shader
// edited in GPU Inspector run in the capture. The request (encodeReplaceRequest in
// src/app/src/renderer/shader_replay.ts) is in --ablate's layout: "REPLACE 1\n", a little-endian
// u32 manifest length, the JSON manifest
//   {"replacements": [{"pipeline": 27, "stage": "fragment", "payload": [0, 7288]}]}
// and the SPIR-V, which the manifest names as [offset, length] after it.

bool ReadReplaceRequest(const std::string& path, ReplayOptions& options, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string magic = "REPLACE 1\n";
    if (bytes.size() < magic.size() + 4 || std::memcmp(bytes.data(), magic.data(), magic.size()) != 0)
    {
        error = "not a shader replacement request: " + path;
        return false;
    }
    uint32_t length = 0;
    std::memcpy(&length, bytes.data() + magic.size(), 4);
    const size_t start = magic.size() + 4;
    if (start + length > bytes.size())
    {
        error = "the replacement request is truncated";
        return false;
    }
    JsonDocument doc;
    if (!doc.Parse(bytes.data() + start, length, error))
    {
        error = "the replacement request's manifest is not valid JSON: " + error;
        return false;
    }
    const size_t base = start + length;
    const JValue* list = doc.Root().Get("replacements");
    for (uint32_t i = 0; list && list->IsArray() && i < list->count; ++i)
    {
        const JValue& item = list->items[i];
        ShaderReplacement r;
        r.pipeline = item.Get("pipeline") ? item.Get("pipeline")->Uint() : 0;
        r.stage = Str(item.Get("stage"));
        const JValue* payload = item.Get("payload");
        if (payload && payload->IsArray() && payload->count == 2)
        {
            const uint64_t offset = payload->items[0].Uint();
            const uint64_t size = payload->items[1].Uint();
            if (base + offset + size <= bytes.size() && size % 4 == 0)
            {
                r.words.resize((size_t)size / 4);
                std::memcpy(r.words.data(), bytes.data() + base + offset, (size_t)size);
            }
        }
        if (!r.pipeline || r.stage.empty() || r.words.empty())
        {
            error = "replacement " + std::to_string(i) + " names no pipeline, no stage or no code";
            return false;
        }
        options.replacements.push_back(std::move(r));
    }
    if (options.replacements.empty())
    {
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
bool WriteTargetData(const ReplayReport& report, const std::string& path)
{
    std::string payloads;
    std::string json = "{\"format\":\"gpu-inspector-replayed-targets\",\"version\":1,\"device\":" + JsonString(report.device) + ",\"targets\":[";
    for (size_t i = 0; i < report.targets.size(); ++i)
    {
        const TargetComparison& t = report.targets[i];
        json += std::string(i ? "," : "") + "{\"image\":" + std::to_string(t.image) + ",\"commandBuffer\":" + std::to_string(t.commandBuffer) +
            ",\"frame\":" + std::to_string(t.frame) + ",\"passIndex\":" + std::to_string(t.passIndex) + ",\"attachment\":" + std::to_string(t.attachment) +
            ",\"aspect\":" + JsonString(t.aspect) + ",\"format\":" + JsonString(t.format) + ",\"width\":" + std::to_string(t.width) +
            ",\"height\":" + std::to_string(t.height) + ",\"compared\":" + (t.compared ? "true" : "false") +
            ",\"texels\":" + std::to_string(t.texels) + ",\"differingTexels\":" + std::to_string(t.differingTexels) +
            ",\"maxByteDelta\":" + std::to_string(t.maxByteDelta);
        if (!t.note.empty())
            json += ",\"note\":" + JsonString(t.note);
        if (t.differingTexels && !t.replayed.empty())
        {
            json += ",\"payload\":[" + std::to_string(payloads.size()) + "," + std::to_string(t.replayed.size()) + "]";
            payloads.append(reinterpret_cast<const char*>(t.replayed.data()), t.replayed.size());
        }
        json += "}";
    }
    json += "],\"problems\":[";
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i)
        json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    const uint32_t length = (uint32_t)json.size();
    out.write("TARGETS 1\n", 10);
    out.write(reinterpret_cast<const char*>(&length), 4);
    out.write(json.data(), (std::streamsize)json.size());
    out.write(payloads.data(), (std::streamsize)payloads.size());
    return (bool)out;
}

// ---------------------------------------------------------------------------------------------
// --ablate: draws timed again with variants of a shader stage (ablation.cpp). The request is a file
// GPU Inspector writes (encodeAblationRequest in src/app/src/renderer/shader_ablation.ts), in the capture
// file's layout: "ABLATE 1\n", a little-endian u32 manifest length, the JSON manifest
//   {"rounds": 5, "targets": [{"command": 17, "stage": "fragment", "variants": [{"name": "fbm", "payload": [0, 7288]}]}]}
// and the variants' SPIR-V, which the manifest names as [offset, length] after it.

bool ReadAblationRequest(const std::string& path, ReplayOptions& options, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string magic = "ABLATE 1\n";
    if (bytes.size() < magic.size() + 4 || std::memcmp(bytes.data(), magic.data(), magic.size()) != 0)
    {
        error = "not an ablation request: " + path;
        return false;
    }
    uint32_t length = 0;
    std::memcpy(&length, bytes.data() + magic.size(), 4);
    const size_t start = magic.size() + 4;
    if (start + length > bytes.size())
    {
        error = "the ablation request is truncated";
        return false;
    }
    JsonDocument doc;
    if (!doc.Parse(bytes.data() + start, length, error))
    {
        error = "the ablation request's manifest is not valid JSON: " + error;
        return false;
    }
    const size_t base = start + length;
    const JValue& root = doc.Root();
    options.ablation.enabled = true;
    if (const JValue* rounds = root.Get("rounds"))
        options.ablation.rounds = std::clamp<uint32_t>((uint32_t)rounds->Uint(), 1, 64);
    const JValue* targets = root.Get("targets");
    for (uint32_t t = 0; targets && targets->IsArray() && t < targets->count; ++t)
    {
        const JValue& target = targets->items[t];
        ReplayOptions::AblationTarget out;
        out.command = target.Get("command") ? (uint32_t)target.Get("command")->Uint() : 0;
        out.stage = Str(target.Get("stage"));
        if (const JValue* repeat = target.Get("repeat"))
            out.repeat = std::clamp<uint32_t>((uint32_t)repeat->Uint(), 1, 256);
        const JValue* variants = target.Get("variants");
        for (uint32_t v = 0; variants && variants->IsArray() && v < variants->count; ++v)
        {
            const JValue& variant = variants->items[v];
            ReplayOptions::AblationVariant vo;
            vo.name = Str(variant.Get("name"));
            const JValue* payload = variant.Get("payload");
            if (payload && payload->IsArray() && payload->count == 2)
            {
                const uint64_t offset = payload->items[0].Uint();
                const uint64_t size = payload->items[1].Uint();
                if (base + offset + size <= bytes.size() && size % 4 == 0)
                {
                    vo.words.resize((size_t)size / 4);
                    std::memcpy(vo.words.data(), bytes.data() + base + offset, (size_t)size);
                }
            }
            out.variants.push_back(std::move(vo));
        }
        options.ablation.targets.push_back(std::move(out));
    }
    return true;
}

/** --ablate-data: each target's timings (parseAblationResult in src/app/src/renderer/shader_ablation.ts). */
bool WriteAblationData(const ReplayReport& report, const std::string& path)
{
    auto timing = [](const AblationTiming& t) {
        char ms[32];
        std::snprintf(ms, sizeof(ms), "%.6f", t.ms);
        std::string s = "{\"name\":" + JsonString(t.name) + ",\"measured\":" + (t.measured ? "true" : "false") + ",\"ms\":" + ms + ",\"samples\":[";
        for (size_t i = 0; i < t.samples.size(); ++i)
        {
            std::snprintf(ms, sizeof(ms), "%.6f", t.samples[i]);
            s += std::string(i ? "," : "") + ms;
        }
        return s + "]" + (t.note.empty() ? "" : ",\"note\":" + JsonString(t.note)) + "}";
    };
    std::string json = "{\"format\":\"gpu-inspector-ablation\",\"version\":1,\"device\":" + JsonString(report.device) + ",\"targets\":[";
    for (size_t i = 0; i < report.ablations.size(); ++i)
    {
        const AblationResult& a = report.ablations[i];
        json += std::string(i ? "," : "") + "{\"command\":" + std::to_string(a.command) + ",\"stage\":" + JsonString(a.stage) +
            ",\"pipeline\":" + std::to_string(a.pipeline) + (a.shaderObject ? ",\"shaderObject\":true" : "") +
            ",\"frame\":" + std::to_string(a.frame) + ",\"commandBuffer\":" +
            std::to_string(a.commandBuffer) + ",\"passIndex\":" + std::to_string(a.passIndex) + ",\"rounds\":" + std::to_string(a.rounds) + ",\"repeat\":" + std::to_string(a.repeat) +
            ",\"baseline\":" + timing(a.baseline) + ",\"variants\":[";
        for (size_t v = 0; v < a.variants.size(); ++v)
            json += (v ? "," : "") + timing(a.variants[v]);
        json += "]" + (a.note.empty() ? std::string() : ",\"note\":" + JsonString(a.note)) + "}";
    }
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

void PrintAblations(const ReplayReport& report)
{
    std::printf("ablations: %zu\n", report.ablations.size());
    for (const AblationResult& a : report.ablations)
    {
        std::printf("  [%u] %s stage, %s %llu: ", a.command, a.stage.c_str(), a.shaderObject ? "shader object" : "pipeline",
            (unsigned long long)a.pipeline);
        if (!a.baseline.measured)
        {
            std::printf("not measured: %s\n", a.note.c_str());
            continue;
        }
        std::printf("%.4f ms as captured (median of %u rounds)\n", a.baseline.ms, a.rounds);
        for (const AblationTiming& v : a.variants)
        {
            if (!v.measured)
                std::printf("    %-40s not measured%s%s\n", v.name.c_str(), v.note.empty() ? "" : ": ", v.note.c_str());
            else
                std::printf("    %-40s %.4f ms, saves %.4f ms\n", v.name.c_str(), v.ms, a.baseline.ms - v.ms);
        }
    }
}

void PrintDraws(const ReplayReport& report)
{
    double total = 0;
    uint64_t fragments = 0;
    for (const DrawResult& d : report.draws)
    {
        total += d.durationMs;
        fragments += d.fragmentInvocations;
    }
    std::printf("draws measured: %zu, %.3f ms of draw time, %llu fragment shader invocations%s\n", report.draws.size(), total,
        (unsigned long long)fragments, report.drawStatsNote.empty() ? "" : (" (" + report.drawStatsNote + ")").c_str());
    for (size_t i = 0; i < report.draws.size() && i < 20; ++i)
    {
        const DrawResult& d = report.draws[i];
        const std::string where = d.passIndex == UINT32_MAX ? "outside a render pass" : "pass " + std::to_string(d.passIndex);
        const std::string samples = d.sampled ? ", " + std::to_string(d.samplesPassed) + " samples passed" : std::string();
        std::printf("  [%u] %s: %.4f ms%s, %llu vertex, %llu primitives, %llu fragment, %llu compute invocations%s\n", d.command, where.c_str(),
            d.durationMs, d.timed ? "" : " (not timed)", (unsigned long long)d.vertexInvocations, (unsigned long long)d.primitives,
            (unsigned long long)d.fragmentInvocations, (unsigned long long)d.computeInvocations, samples.c_str());
    }
    if (report.draws.size() > 20)
        std::printf("  ... %zu more\n", report.draws.size() - 20);
}

/**
 * --overlay-data: where each draw asked for landed, for the overlays of GPU Inspector's render target
 * tab (parseDrawOverlayFile in src/app/src/renderer/draw_overlay.ts). The layout is --overdraw-data's,
 * with one byte per pixel of mask (OverlayResult).
 */
bool WriteOverlayData(const ReplayReport& report, const std::string& path)
{
    std::string json = "{\"format\":\"gpu-inspector-draw-overlay\",\"version\":1,\"device\":" + JsonString(report.device) + ",\"draws\":[";
    uint64_t offset = 0;
    for (size_t i = 0; i < report.overlays.size(); ++i)
    {
        const OverlayResult& o = report.overlays[i];
        const uint64_t size = o.mask.size();
        json += std::string(i ? "," : "") + "{\"command\":" + std::to_string(o.command) + ",\"method\":" + JsonString(o.method) +
            ",\"frame\":" + std::to_string(o.frame) + ",\"commandBuffer\":" + std::to_string(o.commandBuffer) +
            ",\"passIndex\":" + std::to_string(o.passIndex) + ",\"measured\":" + (size ? "true" : "false") +
            ",\"width\":" + std::to_string(o.width) + ",\"height\":" + std::to_string(o.height) +
            ",\"fragments\":" + std::to_string(o.fragments) + ",\"pixelsCovered\":" + std::to_string(o.pixelsCovered) +
            ",\"pixelsPassed\":" + std::to_string(o.pixelsPassed) + ",\"pixelsRejected\":" + std::to_string(o.pixelsRejected) +
            ",\"pixelsStencilRejected\":" + std::to_string(o.pixelsStencilRejected) +
            ",\"pixelsBackFacing\":" + std::to_string(o.pixelsBackFacing) +
            ",\"depthTested\":" + (o.depthTested ? "true" : "false") + ",\"wireframe\":" + (o.wireframe ? "true" : "false") +
            ",\"stencilTested\":" + (o.stencilTested ? "true" : "false") +
            ",\"backFaceTested\":" + (o.backFaceTested ? "true" : "false");
        if (!o.note.empty())
            json += ",\"note\":" + JsonString(o.note);
        if (size)
            json += ",\"payload\":[" + std::to_string(offset) + "," + std::to_string(size) + "]";
        json += "}";
        offset += size;
    }
    json += "],\"problems\":[";
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i)
        json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";

    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    const char magic[] = "OVERLAY 1\n";
    out.write(magic, sizeof(magic) - 1);
    const uint32_t length = (uint32_t)json.size();
    const uint8_t le[4] = {(uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16), (uint8_t)(length >> 24)};
    out.write((const char*)le, 4);
    out.write(json.data(), (std::streamsize)json.size());
    for (const OverlayResult& o : report.overlays)
        out.write((const char*)o.mask.data(), (std::streamsize)o.mask.size());
    return (bool)out;
}

void PrintOverlays(const ReplayReport& report)
{
    std::printf("draw overlays: %zu\n", report.overlays.size());
    for (const OverlayResult& o : report.overlays)
    {
        std::printf("  [%u] %s", o.command, o.method.empty() ? "?" : o.method.c_str());
        if (o.mask.empty())
        {
            std::printf(": not drawn: %s\n", o.note.c_str());
            continue;
        }
        const double pixels = (double)o.width * o.height;
        std::printf(" (command buffer %llu, pass %u): %llu fragments on %llu of %.0f pixels (%.2f%%); %llu passed depth and stencil, %llu rejected%s%s%s\n",
            (unsigned long long)o.commandBuffer, o.passIndex, (unsigned long long)o.fragments, (unsigned long long)o.pixelsCovered, pixels,
            pixels ? 100.0 * o.pixelsCovered / pixels : 0.0, (unsigned long long)o.pixelsPassed, (unsigned long long)o.pixelsRejected,
            o.wireframe ? ", wireframe drawn" : "", o.note.empty() ? "" : "; ", o.note.c_str());
        if (o.stencilTested || o.backFaceTested)
            std::printf("      %llu rejected by the stencil test alone, %llu covered by faces its culling removed\n",
                (unsigned long long)o.pixelsStencilRejected, (unsigned long long)o.pixelsBackFacing);
    }
}

/**
 * --mesh-data: what each draw asked for had its vertex shader write, for GPU Inspector's mesh view
 * (parseMeshFile in src/app/src/renderer/mesh_output.ts). The layout is --overdraw-data's, with each
 * draw's vertex records as the payload.
 */
bool WriteMeshData(const ReplayReport& report, const std::string& path)
{
    std::string json = "{\"format\":\"gpu-inspector-mesh\",\"version\":1,\"device\":" + JsonString(report.device) + ",\"draws\":[";
    uint64_t offset = 0;
    for (size_t i = 0; i < report.meshes.size(); ++i)
    {
        const MeshResult& m = report.meshes[i];
        const uint64_t size = m.data.size();
        std::string outputs;
        for (size_t k = 0; k < m.outputs.size(); ++k)
        {
            const XfbOutput& o = m.outputs[k];
            outputs += std::string(k ? "," : "") + "{\"name\":" + JsonString(o.name) + ",\"offset\":" + std::to_string(o.offset) +
                ",\"components\":" + std::to_string(o.components) + ",\"base\":" + JsonString(o.base) +
                (o.builtin.empty() ? "" : ",\"builtin\":" + JsonString(o.builtin)) +
                (o.location >= 0 ? ",\"location\":" + std::to_string(o.location) : "") + "}";
        }
        json += std::string(i ? "," : "") + "{\"command\":" + std::to_string(m.command) + ",\"method\":" + JsonString(m.method) +
            ",\"frame\":" + std::to_string(m.frame) + ",\"commandBuffer\":" + std::to_string(m.commandBuffer) +
            ",\"passIndex\":" + std::to_string(m.passIndex) + ",\"measured\":" + (m.stride ? "true" : "false") +
            ",\"topology\":" + JsonString(m.topology) + ",\"stride\":" + std::to_string(m.stride) +
            ",\"vertices\":" + std::to_string(m.vertices) + ",\"truncated\":" + (m.truncated ? "true" : "false") +
            ",\"outputs\":[" + outputs + "]";
        if (!m.note.empty())
            json += ",\"note\":" + JsonString(m.note);
        if (size)
            json += ",\"payload\":[" + std::to_string(offset) + "," + std::to_string(size) + "]";
        json += "}";
        offset += size;
    }
    json += "],\"problems\":[";
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i)
        json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";

    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    const char magic[] = "MESH 1\n";
    out.write(magic, sizeof(magic) - 1);
    const uint32_t length = (uint32_t)json.size();
    const uint8_t le[4] = {(uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16), (uint8_t)(length >> 24)};
    out.write((const char*)le, 4);
    out.write(json.data(), (std::streamsize)json.size());
    for (const MeshResult& m : report.meshes)
        out.write((const char*)m.data.data(), (std::streamsize)m.data.size());
    return (bool)out;
}

void PrintMeshes(const ReplayReport& report)
{
    std::printf("mesh outputs: %zu\n", report.meshes.size());
    for (const MeshResult& m : report.meshes)
    {
        std::printf("  [%u] %s", m.command, m.method.empty() ? "?" : m.method.c_str());
        if (!m.stride)
        {
            std::printf(": not captured: %s\n", m.note.c_str());
            continue;
        }
        std::printf(" (command buffer %llu, pass %u, %s): %u vertices, %u bytes each%s%s%s\n", (unsigned long long)m.commandBuffer, m.passIndex,
            m.topology.empty() ? "topology unknown" : m.topology.c_str(), m.vertices, m.stride, m.truncated ? ", truncated" : "",
            m.note.empty() ? "" : "; ", m.note.c_str());
        for (const XfbOutput& o : m.outputs)
        {
            std::printf("    %s: offset %u, %u %s%s", o.name.c_str(), o.offset, o.components, o.base.c_str(), o.components == 1 ? "" : "s");
            // The position's range in clip space, and the vertices behind the eye (w <= 0).
            if (o.builtin == "Position" && o.components == 4 && o.base == "float" && m.vertices)
            {
                float lo[4] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX}, hi[4] = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
                uint32_t behind = 0;
                for (uint32_t v = 0; v < m.vertices; ++v)
                {
                    float p[4];
                    std::memcpy(p, m.data.data() + (size_t)v * m.stride + o.offset, sizeof(p));
                    for (int k = 0; k < 4; ++k)
                    {
                        lo[k] = std::min(lo[k], p[k]);
                        hi[k] = std::max(hi[k], p[k]);
                    }
                    if (p[3] <= 0)
                        ++behind;
                }
                std::printf("; x [%g, %g], y [%g, %g], z [%g, %g], w [%g, %g], %u with w <= 0", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3], behind);
            }
            std::printf("\n");
        }
    }
}

void PrintHistory(const PixelHistoryResult& h)
{
    std::printf("pixel history: image %llu, pixel (%u, %u), mip %u, layer %u%s%s\n", (unsigned long long)h.image, h.x, h.y, h.mip, h.layer,
        h.format.empty() ? "" : (", " + h.format).c_str(), h.depthFormat.empty() ? "" : (", depth " + h.depthFormat).c_str());
    size_t untouched = 0;
    for (const PixelEvent& e : h.events)
    {
        const bool touched = e.kind != "draw" || (!e.scissored && (e.covered || !e.testsMeasured));
        if (!touched)
        {
            ++untouched;
            continue;
        }
        std::string line = "  [" + std::to_string(e.command) + "] ";
        if (e.kind == "load")
        {
            line += "command buffer " + std::to_string(e.commandBuffer) + ", pass " + std::to_string(e.passIndex) + " begins (" + e.detail + ")";
        }
        else if (e.kind == "draw")
        {
            line += e.method + " (pipeline " + std::to_string(e.pipeline) + "): " + DrawOutcome(e);
        }
        else
        {
            // A clear of the pass, or a write outside one: the value is the whole answer.
            line += e.method;
            if (!e.detail.empty() && e.detail != e.method)
                line += " (" + e.detail + ")";
        }
        std::printf("%s\n", line.c_str());
        std::printf("      value %s", FormatTexel(h.format, e.value, h.format.find("VK_FORMAT_D") == 0).c_str());
        if (!e.depth.empty())
            std::printf(", depth %s", FormatTexel(h.depthFormat, e.depth, true).c_str());
        std::printf("\n");
    }
    if (untouched)
        std::printf("  %zu other draws in these passes do not reach the pixel\n", untouched);
    for (const std::string& n : h.notes)
        std::printf("  note: %s\n", n.c_str());
}

// ---------------------------------------------------------------------------------------------
// --dump: the captured, replayed and difference images of each compared target as PNG files.

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready)
    {
        for (uint32_t n = 0; n < 256; ++n)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/** An 8-bit RGBA PNG with uncompressed (stored) deflate blocks: large, but needs no zlib. */
bool WritePng(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba)
{
    std::vector<uint8_t> raw;
    raw.reserve(((size_t)width * 4 + 1) * height);
    for (uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + (size_t)y * width * 4, rgba.begin() + ((size_t)y + 1) * width * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size();)
    {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF));
        z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8)
        z.push_back((uint8_t)(adler >> s));

    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.write((const char*)signature, 8);
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        uint32_t len = (uint32_t)data.size();
        uint8_t be[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
        out.write((const char*)be, 4);
        out.write((const char*)body.data(), (std::streamsize)body.size());
        uint32_t crc = Crc32(body.data(), body.size(), 0);
        uint8_t cb[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
        out.write((const char*)cb, 4);
    };
    std::vector<uint8_t> ihdr = {(uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8), (uint8_t)width,
        (uint8_t)(height >> 24), (uint8_t)(height >> 16), (uint8_t)(height >> 8), (uint8_t)height,
        8, 6, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return true;
}

/** A read-back's bytes as displayable RGBA: 8-bit color formats as they are, depth stretched to its range. */
bool ToRgba(const TargetComparison& t, const std::vector<uint8_t>& bytes, std::vector<uint8_t>& rgba)
{
    const size_t texels = (size_t)t.width * t.height;
    rgba.assign(texels * 4, 255);
    if (t.format.find("B8G8R8A8") != std::string::npos || t.format.find("R8G8B8A8") != std::string::npos)
    {
        const bool bgr = t.format.find("B8G8R8A8") != std::string::npos;
        if (bytes.size() < texels * 4)
            return false;
        for (size_t i = 0; i < texels; ++i)
        {
            rgba[i * 4] = bytes[i * 4 + (bgr ? 2 : 0)];
            rgba[i * 4 + 1] = bytes[i * 4 + 1];
            rgba[i * 4 + 2] = bytes[i * 4 + (bgr ? 0 : 2)];
        }
        return true;
    }
    if (t.format == "VK_FORMAT_D32_SFLOAT" || t.format == "VK_FORMAT_D32_SFLOAT_S8_UINT")
    {
        if (bytes.size() < texels * 4)
            return false;
        float lo = INFINITY, hi = -INFINITY;
        for (size_t i = 0; i < texels; ++i)
        {
            float v;
            std::memcpy(&v, &bytes[i * 4], 4);
            if (std::isfinite(v))
            {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        for (size_t i = 0; i < texels; ++i)
        {
            float v;
            std::memcpy(&v, &bytes[i * 4], 4);
            uint8_t g = hi > lo ? (uint8_t)std::lround(255.0 * (v - lo) / (hi - lo)) : 0;
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g;
        }
        return true;
    }
    return false;
}

/** The heat of a count: black for none, then blue, cyan, green, yellow, orange, red, magenta and white from 33. */
void Heat(uint16_t n, uint8_t* rgb)
{
    static const struct
    {
        uint16_t upTo;
        uint8_t r, g, b;
    } kRamp[] = {
        {0, 0, 0, 0},
        {1, 20, 40, 150},
        {2, 0, 120, 230},
        {3, 0, 190, 170},
        {4, 110, 210, 40},
        {6, 240, 210, 0},
        {10, 250, 120, 0},
        {16, 220, 20, 20},
        {32, 240, 0, 200},
        {65535, 255, 255, 255},
    };
    for (const auto& step : kRamp)
    {
        if (n <= step.upTo)
        {
            rgb[0] = step.r;
            rgb[1] = step.g;
            rgb[2] = step.b;
            return;
        }
    }
}

void WriteOverdraw(const ReplayReport& report, const std::string& dir)
{
    std::filesystem::create_directories(dir);
    for (const OverdrawResult& o : report.overdraw)
    {
        if (o.counts.empty())
            continue;
        const std::string path = dir + "/overdraw_cb" + std::to_string(o.commandBuffer) + "_pass" + std::to_string(o.passIndex) +
            (report.overdraw.size() && o.frame ? "_frame" + std::to_string(o.frame) : "") +
            (o.depthTested ? "_tested.png" : "_all.png");
        std::vector<uint8_t> rgba((size_t)o.width * o.height * 4, 255);
        for (size_t i = 0; i < o.counts.size(); ++i)
            Heat(o.counts[i], &rgba[i * 4]);
        WritePng(path, o.width, o.height, rgba);
        std::printf("  wrote %s\n", path.c_str());
    }
}

void DumpTargets(const ReplayReport& report, const std::string& dir)
{
    std::filesystem::create_directories(dir);
    for (const TargetComparison& t : report.targets)
    {
        if (!t.compared || t.captured.empty())
            continue;
        const std::string base = dir + "/image" + std::to_string(t.image) + "_cb" + std::to_string(t.commandBuffer) + "_pass" +
            std::to_string(t.passIndex) + "_att" + std::to_string(t.attachment);
        std::vector<uint8_t> captured, replayed;
        if (!ToRgba(t, t.captured, captured) || !ToRgba(t, t.replayed, replayed))
        {
            std::printf("  (%s: no PNG for this format)\n", base.c_str());
            continue;
        }
        std::vector<uint8_t> diff(captured.size(), 255);
        const size_t texel = t.texels ? t.captured.size() / t.texels : 4;
        for (size_t i = 0; i < t.texels && i * 4 < diff.size(); ++i)
        {
            uint32_t delta = 0;
            for (size_t k = 0; k < texel; ++k)
                delta = std::max<uint32_t>(delta, (uint32_t)std::abs((int)t.captured[i * texel + k] - (int)t.replayed[i * texel + k]));
            uint8_t v = (uint8_t)std::min<uint32_t>(255, delta ? 64 + delta * 4 : 0);
            diff[i * 4] = v;
            diff[i * 4 + 1] = diff[i * 4 + 2] = 0;
        }
        WritePng(base + "_captured.png", t.width, t.height, captured);
        WritePng(base + "_replayed.png", t.width, t.height, replayed);
        WritePng(base + "_diff.png", t.width, t.height, diff);
        std::printf("  wrote %s_{captured,replayed,diff}.png\n", base.c_str());
    }
}

void PrintGrouped(const std::vector<std::string>& lines, size_t limit)
{
    std::map<std::string, int> grouped;
    for (const std::string& p : lines)
        ++grouped[p];
    size_t shown = 0;
    for (auto& [p, n] : grouped)
    {
        if (++shown > limit)
        {
            std::printf("  ... %zu more\n", grouped.size() - limit);
            break;
        }
        std::printf("  %s%s\n", p.c_str(), n > 1 ? (" (x" + std::to_string(n) + ")").c_str() : "");
    }
}

/** Decodes every object and command of the capture; returns the process exit code. */
int Check(const CaptureFile& capture)
{
    Arena arena;
    DecodeContext ctx(arena);
    // Every object the manifest holds resolves (to a stand-in handle: its id); others do not.
    ctx.resolve = [&](uint64_t id, std::string_view) -> uint64_t { return capture.Object(id) ? id : 0; };

    std::map<std::string, int> objectTypes;
    std::map<std::string, int> methods;
    std::map<std::string, int> undecodable;
    size_t objects = 0, commands = 0;

    if (const JValue* list = capture.Objects(); list && list->IsArray())
    {
        for (uint32_t i = 0; i < list->count; ++i)
        {
            const JValue& o = list->items[i];
            const JValue* type = o.Get("type");
            const JValue* cmd = o.Get("cmd");
            const JValue* args = o.Get("args");
            std::string typeName(type ? type->Str() : "?");
            ++objectTypes[typeName];
            ++objects;
            if (!cmd || !args || args->IsNull())
                continue;
            DecodeCheckFn fn = FindArgsDecoder(cmd->Str());
            if (!fn)
            {
                ++undecodable[std::string(cmd->Str())];
                continue;
            }
            ctx.where = typeName + " " + std::to_string(o.Get("id") ? o.Get("id")->Uint() : 0) + " (" + std::string(cmd->Str()) + ")";
            fn(ctx, *args);
            arena.Reset();
        }
    }
    if (const JValue* list = capture.Commands(); list && list->IsArray())
    {
        for (uint32_t i = 0; i < list->count; ++i)
        {
            const JValue& c = list->items[i];
            const JValue* method = c.Get("method");
            const JValue* args = c.Get("args");
            if (!method)
                continue;
            ++methods[std::string(method->Str())];
            ++commands;
            if (!args || args->IsNull())
                continue;
            DecodeCheckFn fn = FindArgsDecoder(method->Str());
            if (!fn)
            {
                ++undecodable[std::string(method->Str())];
                continue;
            }
            ctx.where = "command " + std::to_string(i) + " " + std::string(method->Str());
            fn(ctx, *args);
            arena.Reset();
        }
    }

    std::printf("capture: %zu bytes, manifest %zu bytes\n", capture.FileSize(), capture.ManifestSize());
    std::printf("objects: %zu\n", objects);
    for (auto& [type, n] : objectTypes)
        std::printf("  %-32s %d\n", type.c_str(), n);
    std::printf("commands: %zu\n", commands);
    for (auto& [m, n] : methods)
        std::printf("  %-40s %d%s\n", m.c_str(), n, FindReplayCommand(m) ? "" : "  (not a recordable vkCmd)");
    if (!undecodable.empty())
    {
        std::printf("no decoder for:\n");
        for (auto& [m, n] : undecodable)
            std::printf("  %s (%d)\n", m.c_str(), n);
    }
    std::printf("problems: %zu\n", ctx.problems.size());
    PrintGrouped(ctx.problems, 60);
    return ctx.problems.empty() && undecodable.empty() ? 0 : 1;
}

int Replay(const CaptureFile& capture, const ReplayOptions& options, const std::string& dumpDir, const std::string& overdrawDir,
    const std::string& overdrawData, const std::string& pixelData, const std::string& drawData, const std::string& overlayData,
    const std::string& meshData, const std::string& ablationData, const std::string& counterData, const std::string& exportData,
    const std::string& targetData, const std::string& validationData)
{
    ReplayReport report;
    bool ran = false;
    {
        Replayer replayer;
        ran = replayer.Run(capture, options, report);
    }  // the device is destroyed here, so validation messages about teardown are in the report too
    std::printf("device: %s\n", report.device.c_str());
    std::printf("objects: %zu created, %zu left out\n", report.objectsCreated, report.objectsSkipped);
    std::printf("uploads: %zu sampled textures, %zu images as the frame found them, %zu buffer ranges\n", report.texturesUploaded,
        report.initialImagesUploaded, report.bufferUploads);
    std::printf("commands: %zu recorded in %zu submissions\n", report.commandsRecorded, report.submissions);
    if (report.earlierStructuresBuilt)
        std::printf("acceleration structures built before the capture: %zu, built again from what was read back when it began\n", report.earlierStructuresBuilt);
    size_t exact = 0, differing = 0, skipped = 0;
    std::printf("render targets: %zu\n", report.targets.size());
    for (const TargetComparison& t : report.targets)
    {
        std::printf("  image %llu (command buffer %llu, pass %u, attachment %u, %s %ux%u %s): ", (unsigned long long)t.image,
            (unsigned long long)t.commandBuffer, t.passIndex, t.attachment, t.format.c_str(), t.width, t.height, t.aspect.c_str());
        if (!t.compared)
        {
            ++skipped;
            std::printf("not compared: %s\n", t.note.c_str());
        }
        else if (t.differingTexels == 0 && t.note.empty())
        {
            ++exact;
            std::printf("identical (%llu texels)\n", (unsigned long long)t.texels);
        }
        else
        {
            ++differing;
            std::printf("%llu of %llu texels differ, largest byte difference %u%s%s\n", (unsigned long long)t.differingTexels,
                (unsigned long long)t.texels, t.maxByteDelta, t.note.empty() ? "" : "; ", t.note.c_str());
        }
    }
    if (options.overdraw)
    {
        std::printf("overdraw: %zu measurements\n", report.overdraw.size());
        for (const OverdrawResult& o : report.overdraw)
        {
            const double pixels = (double)o.width * o.height;
            std::printf("  command buffer %llu, pass %u, %s: ", (unsigned long long)o.commandBuffer, o.passIndex,
                o.depthTested ? "fragments passing depth" : "every rasterized fragment");
            if (o.counts.empty())
            {
                std::printf("not measured: %s\n", o.note.c_str());
                continue;
            }
            std::printf("%u draws%s, %llu fragments on %llu of %.0f pixels, %.3f per pixel, %.2f per covered pixel, max %u", o.draws,
                o.skippedDraws ? (" (" + std::to_string(o.skippedDraws) + " not counted)").c_str() : "",
                (unsigned long long)o.fragments, (unsigned long long)o.coveredPixels, pixels, pixels ? o.fragments / pixels : 0.0,
                o.coveredPixels ? (double)o.fragments / o.coveredPixels : 0.0, o.maxCount);
            if (o.capturedFragments >= 0)
                std::printf("; the capture measured %lld fragment shader invocations", (long long)o.capturedFragments);
            if (!o.note.empty())
                std::printf(" (%s)", o.note.c_str());
            std::printf("\n    pixels by count: 1: %llu, 2: %llu, 3: %llu, 4: %llu, 5-8: %llu, 9-16: %llu, 17-32: %llu, 33+: %llu\n",
                (unsigned long long)o.histogram[0], (unsigned long long)o.histogram[1], (unsigned long long)o.histogram[2],
                (unsigned long long)o.histogram[3], (unsigned long long)o.histogram[4], (unsigned long long)o.histogram[5],
                (unsigned long long)o.histogram[6], (unsigned long long)o.histogram[7]);
        }
        if (!overdrawDir.empty())
            WriteOverdraw(report, overdrawDir);
        if (!overdrawData.empty())
        {
            if (WriteOverdrawData(report, overdrawData))
                std::printf("  wrote %s\n", overdrawData.c_str());
            else
                std::printf("  could not write %s\n", overdrawData.c_str());
        }
    }
    if (options.drawStats)
    {
        PrintDraws(report);
        if (!drawData.empty())
        {
            if (WriteDrawData(report, drawData))
                std::printf("  wrote %s\n", drawData.c_str());
            else
                std::printf("  could not write %s\n", drawData.c_str());
        }
    }
    if (options.overlay.enabled)
    {
        PrintOverlays(report);
        if (!overlayData.empty())
        {
            if (WriteOverlayData(report, overlayData))
                std::printf("  wrote %s\n", overlayData.c_str());
            else
                std::printf("  could not write %s\n", overlayData.c_str());
        }
    }
    if (options.mesh.enabled)
    {
        PrintMeshes(report);
        if (!meshData.empty())
        {
            if (WriteMeshData(report, meshData))
                std::printf("  wrote %s\n", meshData.c_str());
            else
                std::printf("  could not write %s\n", meshData.c_str());
        }
    }
    if (options.ablation.enabled)
    {
        PrintAblations(report);
        if (!ablationData.empty())
        {
            if (WriteAblationData(report, ablationData))
                std::printf("  wrote %s\n", ablationData.c_str());
            else
                std::printf("  could not write %s\n", ablationData.c_str());
        }
    }
    if (options.counters.enabled)
    {
        PrintCounters(report);
        if (!counterData.empty())
        {
            if (WriteCounterData(report, counterData))
                std::printf("  wrote %s\n", counterData.c_str());
            else
                std::printf("  could not write %s\n", counterData.c_str());
        }
    }
    if (report.exported.requested)
    {
        PrintExport(report);
        if (!exportData.empty())
        {
            if (WriteExportData(report, exportData))
                std::printf("  wrote %s\n", exportData.c_str());
            else
                std::printf("  could not write %s\n", exportData.c_str());
        }
    }
    if (report.history.requested)
    {
        PrintHistory(report.history);
        if (!pixelData.empty())
        {
            if (WritePixelHistoryData(report, pixelData))
                std::printf("  wrote %s\n", pixelData.c_str());
            else
                std::printf("  could not write %s\n", pixelData.c_str());
        }
    }
    std::printf("problems: %zu\n", report.problems.size());
    PrintGrouped(report.problems, 80);
    if (options.validation)
    {
        std::printf("validation messages: %zu\n", report.validation.size());
        PrintGrouped(report.validation, 40);
        if (!validationData.empty())
        {
            const bool layerFound = std::none_of(report.problems.begin(), report.problems.end(),
                [](const std::string& p) { return p.find("validation layer is not installed") != std::string::npos; });
            if (WriteValidationData(report, layerFound, validationData))
                std::printf("  wrote %s\n", validationData.c_str());
            else
                std::printf("  could not write %s\n", validationData.c_str());
        }
    }
    if (!dumpDir.empty())
        DumpTargets(report, dumpDir);
    if (!targetData.empty())
        std::printf(WriteTargetData(report, targetData) ? "wrote %s\n" : "could not write %s\n", targetData.c_str());
    if (!ran)
        return 2;
    // A frame replayed with an edited shader is meant to differ.
    if (!options.replacements.empty())
        return 0;
    return differing == 0 && skipped == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------
// --serve: one replay kept alive for many analyses of the same capture (GPU Inspector's
// ReplayServer, src/app/src/main/replay.ts). The device and the capture's objects are created once;
// each request replays the frame with its analysis and writes the file the one-shot flag would.
//
// Requests arrive one per line on stdin, as JSON:
//   {"id": 1, "kind": "pixel", "image": 17, "x": 320, "y": 240, "mip": 0, "layer": 0, "out": "<file>"}
//   {"id": 2, "kind": "overdraw" | "draws", "out": "<file>"}
//   {"id": 3, "kind": "overlay" | "mesh", "commands": [17, 18], "out": "<file>"}
//   {"id": 6, "kind": "ablate", "in": "<request file>", "out": "<file>"}
//   {"id": 7, "kind": "counters" | "list-counters", "counters": ["sm__throughput..."], "out": "<file>"}
//   {"id": 4, "kind": "replay"}          the frame alone, comparing its render targets
//   {"kind": "quit"}
// Answers are lines on stdout beginning "@replay " (anything else a driver prints is not one):
//   @replay {"ready": true, "device": "...", "problems": 0}           once, after the setup
//   @replay {"id": 1, "ok": true, "ms": 84.2, "problems": 0}          per request
//   @replay {"id": 4, "ok": true, ..., "targets": {"identical": 12, "differing": 0, "notCompared": 0}}
//   @replay {"id": 5, "ok": false, "error": "..."}

void Answer(const std::string& json)
{
    std::fputs(("@replay " + json + "\n").c_str(), stdout);
    std::fflush(stdout);
}

int Serve(const CaptureFile& capture, bool validation)
{
    ReplayOptions setup;
    setup.allFeatures = true;
    setup.validation = validation;
    Replayer replayer;
    ReplayReport setupReport;
    if (!replayer.Setup(capture, setup, setupReport))
    {
        Answer("{\"ready\":false,\"error\":" + JsonString(setupReport.problems.empty() ? "the replay could not start" : setupReport.problems.front()) + "}");
        return 2;
    }
    Answer("{\"ready\":true,\"device\":" + JsonString(setupReport.device) + ",\"problems\":" + std::to_string(setupReport.problems.size()) + "}");

    std::string line;
    while (std::getline(std::cin, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        JsonDocument doc;
        std::string error;
        if (!doc.Parse(line.data(), line.size(), error))
        {
            Answer("{\"ok\":false,\"error\":" + JsonString("malformed request: " + error) + "}");
            continue;
        }
        const JValue& request = doc.Root();
        const std::string id = request.Get("id") ? std::to_string(request.Get("id")->Uint()) : "0";
        const std::string kind(request.Get("kind") ? request.Get("kind")->Str() : "");
        const std::string out(request.Get("out") ? request.Get("out")->Str() : "");
        if (kind == "quit")
            break;
        auto fail = [&](const std::string& message) { Answer("{\"id\":" + id + ",\"ok\":false,\"error\":" + JsonString(message) + "}"); };
        auto commands = [&](std::vector<uint32_t>& into) {
            if (const JValue* list = request.Get("commands"); list && list->IsArray())
                for (uint32_t i = 0; i < list->count; ++i)
                    into.push_back((uint32_t)list->items[i].Uint());
        };

        ReplayOptions options;
        options.compareTargets = kind == "replay";
        if (kind == "overdraw")
        {
            options.overdraw = true;
        }
        else if (kind == "draws")
        {
            options.drawStats = true;
        }
        else if (kind == "overlay")
        {
            options.overlay.enabled = true;
            commands(options.overlay.commands);
        }
        else if (kind == "mesh")
        {
            options.mesh.enabled = true;
            commands(options.mesh.commands);
        }
        else if (kind == "ablate")
        {
            std::string error;
            if (!ReadAblationRequest(request.Get("in") ? std::string(request.Get("in")->Str()) : std::string(), options, error))
            {
                fail(error);
                continue;
            }
        }
        else if (kind == "counters" || kind == "list-counters")
        {
            options.counters.enabled = true;
            options.counters.list = kind == "list-counters";
            options.counters.perDraw = request.Get("perDraw") && request.Get("perDraw")->boolean;
            if (const JValue* b = request.Get("backend"))
                options.counters.backend = std::string(b->Str());
            if (const JValue* list = request.Get("counters"); list && list->IsArray())
                for (uint32_t k = 0; k < list->count; ++k)
                    options.counters.names.push_back(std::string(list->items[k].Str()));
        }
        else if (kind == "pixel")
        {
            options.history.enabled = true;
            options.history.image = request.Get("image") ? request.Get("image")->Uint() : 0;
            options.history.x = request.Get("x") ? (uint32_t)request.Get("x")->Uint() : 0;
            options.history.y = request.Get("y") ? (uint32_t)request.Get("y")->Uint() : 0;
            options.history.mip = request.Get("mip") ? (uint32_t)request.Get("mip")->Uint() : 0;
            options.history.layer = request.Get("layer") ? (uint32_t)request.Get("layer")->Uint() : 0;
        }
        else if (kind != "replay")
        {
            fail("unknown request kind \"" + kind + "\"");
            continue;
        }
        if (kind != "replay" && out.empty())
        {
            fail("the request names no \"out\" file");
            continue;
        }

        const auto started = std::chrono::steady_clock::now();
        ReplayReport report;
        replayer.RunFrame(options, report);
        bool wrote = true;
        if (kind == "overdraw")
            wrote = WriteOverdrawData(report, out);
        else if (kind == "draws")
            wrote = WriteDrawData(report, out);
        else if (kind == "overlay")
            wrote = WriteOverlayData(report, out);
        else if (kind == "mesh")
            wrote = WriteMeshData(report, out);
        else if (kind == "ablate")
            wrote = WriteAblationData(report, out);
        else if (kind == "counters" || kind == "list-counters")
            wrote = WriteCounterData(report, out);
        else if (kind == "pixel")
            wrote = WritePixelHistoryData(report, out);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (!wrote)
        {
            fail("could not write " + out);
            continue;
        }
        char elapsed[32];
        std::snprintf(elapsed, sizeof(elapsed), "%.1f", ms);
        std::string answer = "{\"id\":" + id + ",\"ok\":true,\"ms\":" + elapsed + ",\"problems\":" + std::to_string(report.problems.size());
        if (validation)
            answer += ",\"validation\":" + std::to_string(report.validation.size());
        if (kind == "replay")
        {
            size_t identical = 0, differing = 0, skipped = 0;
            for (const TargetComparison& t : report.targets)
            {
                if (!t.compared)
                    ++skipped;
                else if (t.differingTexels == 0 && t.note.empty())
                    ++identical;
                else
                    ++differing;
            }
            answer += ",\"targets\":{\"identical\":" + std::to_string(identical) + ",\"differing\":" + std::to_string(differing) +
                ",\"notCompared\":" + std::to_string(skipped) + "}";
        }
        Answer(answer + "}");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string path;
    std::string dumpDir;
    std::string overdrawDir;
    std::string overdrawData;
    std::string pixelData;
    std::string drawData;
    std::string overlayData;
    std::string meshData;
    std::string ablationRequest;
    std::string ablationData;
    std::string counterData;
    std::string exportData;
    std::string replaceRequest;
    std::string targetData;
    std::string validationData;
    bool check = false;
    bool serve = false;
    ReplayOptions options;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--check"))
            check = true;
        else if (!std::strcmp(argv[i], "--serve"))
            serve = true;
        else if (!std::strcmp(argv[i], "--validate"))
            options.validation = true;
        else if (!std::strcmp(argv[i], "--validate-data") && i + 1 < argc)
        {
            validationData = argv[++i];
            options.validation = true;
        }
        else if (!std::strcmp(argv[i], "--trace"))
            options.trace = true;
        else if (!std::strcmp(argv[i], "--overdraw") && i + 1 < argc)
        {
            overdrawDir = argv[++i];
            options.overdraw = true;
        }
        else if (!std::strcmp(argv[i], "--overdraw-data") && i + 1 < argc)
        {
            overdrawData = argv[++i];
            options.overdraw = true;
        }
        else if (!std::strcmp(argv[i], "--pixel-data") && i + 1 < argc)
            pixelData = argv[++i];
        else if (!std::strcmp(argv[i], "--draws"))
            options.drawStats = true;
        else if (!std::strcmp(argv[i], "--draw-data") && i + 1 < argc)
        {
            drawData = argv[++i];
            options.drawStats = true;
        }
        else if (!std::strcmp(argv[i], "--overlay") && i + 1 < argc)
        {
            options.overlay.enabled = true;
            options.overlay.commands.push_back((uint32_t)std::strtoul(argv[++i], nullptr, 10));
        }
        else if (!std::strcmp(argv[i], "--overlay-data") && i + 1 < argc)
            overlayData = argv[++i];
        else if (!std::strcmp(argv[i], "--mesh") && i + 1 < argc)
        {
            options.mesh.enabled = true;
            options.mesh.commands.push_back((uint32_t)std::strtoul(argv[++i], nullptr, 10));
        }
        else if (!std::strcmp(argv[i], "--mesh-data") && i + 1 < argc)
            meshData = argv[++i];
        else if (!std::strcmp(argv[i], "--ablate") && i + 1 < argc)
            ablationRequest = argv[++i];
        else if (!std::strcmp(argv[i], "--replace") && i + 1 < argc)
            replaceRequest = argv[++i];
        else if (!std::strcmp(argv[i], "--target-data") && i + 1 < argc)
        {
            targetData = argv[++i];
            options.keepPixels = true;   // the pixels of what differs are what the file is for
        }
        else if (!std::strcmp(argv[i], "--ablate-data") && i + 1 < argc)
            ablationData = argv[++i];
        else if (!std::strcmp(argv[i], "--counters"))
            options.counters.enabled = true;
        else if (!std::strcmp(argv[i], "--counter") && i + 1 < argc)
        {
            options.counters.enabled = true;
            options.counters.names.push_back(argv[++i]);
        }
        else if (!std::strcmp(argv[i], "--counter-backend") && i + 1 < argc)
        {
            options.counters.enabled = true;
            options.counters.backend = argv[++i];
        }
        else if (!std::strcmp(argv[i], "--counter-draws"))
        {
            options.counters.enabled = true;
            options.counters.perDraw = true;
        }
        else if (!std::strcmp(argv[i], "--list-counters"))
        {
            options.counters.enabled = true;
            options.counters.list = true;
        }
        else if (!std::strcmp(argv[i], "--counter-data") && i + 1 < argc)
        {
            options.counters.enabled = true;
            counterData = argv[++i];
        }
        else if (!std::strcmp(argv[i], "--export") && i + 1 < argc)
            options.exportDir = argv[++i];
        else if (!std::strcmp(argv[i], "--export-data") && i + 1 < argc)
            exportData = argv[++i];
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc)
        {
            dumpDir = argv[++i];
            options.keepPixels = true;
        }
        else if (!std::strcmp(argv[i], "--pixel") && i + 3 < argc)
        {
            options.history.enabled = true;
            options.history.image = std::strtoull(argv[++i], nullptr, 10);
            options.history.x = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
            options.history.y = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        }
        else if (!std::strcmp(argv[i], "--mip") && i + 1 < argc)
            options.history.mip = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--layer") && i + 1 < argc)
            options.history.layer = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (argv[i][0] != '-' && path.empty())
            path = argv[i];
        else
        {
            PrintUsage();
            return 2;
        }
    }
    if (path.empty())
    {
        PrintUsage();
        return 2;
    }
    CaptureFile capture;
    std::string error;
    if (!capture.Load(path, error))
    {
        std::fprintf(stderr, "vkinsp_replay: %s\n", error.c_str());
        return 2;
    }
    if (!pixelData.empty() && !options.history.enabled)
    {
        std::fprintf(stderr, "vkinsp_replay: --pixel-data needs --pixel <image> <x> <y>\n");
        return 2;
    }
    if (!exportData.empty() && options.exportDir.empty())
    {
        std::fprintf(stderr, "vkinsp_replay: --export-data needs --export <directory>\n");
        return 2;
    }
    if (options.counters.enabled && !options.exportDir.empty())
    {
        // Counters replay the frame once per collection pass; the export is of one frame.
        std::fprintf(stderr, "vkinsp_replay: --export cannot be combined with hardware counters\n");
        return 2;
    }
    if (serve && !options.exportDir.empty())
    {
        // The objects are exported as they are created, which a served replay does once, before any request.
        std::fprintf(stderr, "vkinsp_replay: --export replays the capture once and cannot be combined with --serve\n");
        return 2;
    }
    if (serve && !replaceRequest.empty())
    {
        // A served replay makes the capture's pipelines once, before any request names another shader for one.
        std::fprintf(stderr, "vkinsp_replay: --replace replays the capture once and cannot be combined with --serve\n");
        return 2;
    }
    if (!replaceRequest.empty() && !ReadReplaceRequest(replaceRequest, options, error))
    {
        std::fprintf(stderr, "vkinsp_replay: %s\n", error.c_str());
        return 2;
    }
    if (serve)
        return Serve(capture, options.validation);
    if (!meshData.empty() && !options.mesh.enabled)
    {
        std::fprintf(stderr, "vkinsp_replay: --mesh-data needs --mesh <command>\n");
        return 2;
    }
    if (!overlayData.empty() && !options.overlay.enabled)
    {
        std::fprintf(stderr, "vkinsp_replay: --overlay-data needs --overlay <command>\n");
        return 2;
    }
    if (!ablationRequest.empty() && !ReadAblationRequest(ablationRequest, options, error))
    {
        std::fprintf(stderr, "vkinsp_replay: %s\n", error.c_str());
        return 2;
    }
    if (!ablationData.empty() && !options.ablation.enabled)
    {
        std::fprintf(stderr, "vkinsp_replay: --ablate-data needs --ablate <request>\n");
        return 2;
    }
    return check ? Check(capture)
                 : Replay(capture, options, dumpDir, overdrawDir, overdrawData, pixelData, drawData, overlayData, meshData, ablationData, counterData,
                       exportData, targetData, validationData);
}
