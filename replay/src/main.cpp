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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "decode.h"
#include "gpucap.h"
#include "replayer.h"
#include "util.h"
#include "vk_decode.gen.h"

using namespace vkreplay;

namespace {

void PrintUsage() {
    std::fprintf(stderr, "usage: vkinsp_replay <capture.gpucap> [--validate] [--dump <dir>] [--overdraw <dir>] [--overdraw-data <file>]\n"
                         "                     [--pixel <image> <x> <y> [--mip <n>] [--layer <n>] [--pixel-data <file>]]\n"
                         "                     [--draws [--draw-data <file>]] [--trace] | --check\n");
}

// ---------------------------------------------------------------------------------------------
// --overdraw-data: every overdraw measurement with its per-pixel counts, for GPU Inspector to show
// (parseOverdrawFile in app/src/renderer/overdraw.ts). The layout is the capture file's: a magic
// line, a little-endian u32 with the length of a JSON manifest, the manifest, then the counts of
// each measurement (u16 little endian, row by row), which the manifest names as [offset, length].

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

bool WriteOverdrawData(const ReplayReport& report, const std::string& path) {
    std::string json = "{\"format\":\"gpu-inspector-overdraw\",\"version\":1,\"device\":" + JsonString(report.device) + ",\"passes\":[";
    uint64_t offset = 0;
    for (size_t i = 0; i < report.overdraw.size(); ++i) {
        const OverdrawResult& o = report.overdraw[i];
        const uint64_t size = (uint64_t)o.counts.size() * 2;
        std::string histogram;
        for (size_t b = 0; b < o.histogram.size(); ++b) histogram += (b ? "," : "") + std::to_string(o.histogram[b]);
        json += std::string(i ? "," : "") + "{\"frame\":" + std::to_string(o.frame) + ",\"commandBuffer\":" + std::to_string(o.commandBuffer) +
                ",\"passIndex\":" + std::to_string(o.passIndex) + ",\"depthTested\":" + (o.depthTested ? "true" : "false") +
                ",\"measured\":" + (o.counts.empty() ? "false" : "true") + ",\"width\":" + std::to_string(o.width) +
                ",\"height\":" + std::to_string(o.height) + ",\"fragments\":" + std::to_string(o.fragments) +
                ",\"coveredPixels\":" + std::to_string(o.coveredPixels) + ",\"maxCount\":" + std::to_string(o.maxCount) +
                ",\"draws\":" + std::to_string(o.draws) + ",\"skippedDraws\":" + std::to_string(o.skippedDraws) +
                ",\"histogram\":[" + histogram + "],\"size\":" + std::to_string(size);
        if (o.capturedFragments >= 0) json += ",\"capturedFragments\":" + std::to_string(o.capturedFragments);
        if (!o.note.empty()) json += ",\"note\":" + JsonString(o.note);
        if (size) json += ",\"payload\":[" + std::to_string(offset) + "," + std::to_string(size) + "]";
        json += "}";
        offset += size;
    }
    json += "],\"problems\":[";
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i) json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const char magic[] = "OVERDRAW 1\n";
    out.write(magic, sizeof(magic) - 1);
    const uint32_t length = (uint32_t)json.size();
    const uint8_t le[4] = {(uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16), (uint8_t)(length >> 24)};
    out.write((const char*)le, 4);
    out.write(json.data(), (std::streamsize)json.size());
    std::vector<uint8_t> bytes;
    for (const OverdrawResult& o : report.overdraw) {
        bytes.resize(o.counts.size() * 2);
        for (size_t p = 0; p < o.counts.size(); ++p) {
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
std::string FormatTexel(const std::string& format, const std::vector<uint8_t>& b, bool depthAspect) {
    if (b.empty()) return "?";
    auto has = [&](const char* s) { return format.find(s) != std::string::npos; };
    auto f32 = [&](size_t o) { float v = 0; if (o + 4 <= b.size()) std::memcpy(&v, &b[o], 4); return v; };
    auto u16 = [&](size_t o) { return o + 2 <= b.size() ? (uint16_t)(b[o] | (b[o + 1] << 8)) : (uint16_t)0; };
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= b.size()) std::memcpy(&v, &b[o], 4); return v; };
    char text[200];
    if (depthAspect) {
        if (has("D32_SFLOAT")) std::snprintf(text, sizeof(text), "%.6f", f32(0));
        else if (has("D24_UNORM") || has("X8_D24")) std::snprintf(text, sizeof(text), "%.6f", (u32(0) & 0xFFFFFF) / 16777215.0);
        else if (has("D16_UNORM")) std::snprintf(text, sizeof(text), "%.6f", u16(0) / 65535.0);
        else goto hex;
        return text;
    }
    if ((has("R8G8B8A8_") || has("B8G8R8A8_")) && b.size() >= 4) {
        const bool bgr = has("B8G8R8A8_");
        const uint8_t r = b[bgr ? 2 : 0], g = b[1], bl = b[bgr ? 0 : 2], a = b[3];
        std::snprintf(text, sizeof(text), "rgba(%u, %u, %u, %u) = (%.3f, %.3f, %.3f, %.3f)", r, g, bl, a, r / 255.0, g / 255.0, bl / 255.0, a / 255.0);
        return text;
    }
    if (has("R16G16B16A16_SFLOAT") && b.size() >= 8) {
        std::snprintf(text, sizeof(text), "(%.4f, %.4f, %.4f, %.4f)", HalfToFloat(u16(0)), HalfToFloat(u16(2)), HalfToFloat(u16(4)), HalfToFloat(u16(6)));
        return text;
    }
    if (has("R32G32B32A32_SFLOAT") && b.size() >= 16) {
        std::snprintf(text, sizeof(text), "(%.4f, %.4f, %.4f, %.4f)", f32(0), f32(4), f32(8), f32(12));
        return text;
    }
    if (format == "VK_FORMAT_R32_SFLOAT") { std::snprintf(text, sizeof(text), "%.6f", f32(0)); return text; }
    if (format == "VK_FORMAT_R16_SFLOAT") { std::snprintf(text, sizeof(text), "%.4f", HalfToFloat(u16(0))); return text; }
    if (has("B10G11R11_UFLOAT_PACK32")) {
        // Unsigned small floats: 6-bit (red, green) or 5-bit (blue) mantissa over a 5-bit exponent, biased by 15.
        auto small = [](uint32_t bits, int mantissaBits) {
            const uint32_t mantissa = bits & ((1u << mantissaBits) - 1);
            const int exponent = (int)(bits >> mantissaBits) & 0x1F;
            if (exponent == 31) return mantissa ? (double)NAN : (double)INFINITY;
            if (exponent == 0) return std::ldexp((double)mantissa / (1u << mantissaBits), -14);
            return std::ldexp(1.0 + (double)mantissa / (1u << mantissaBits), exponent - 15);
        };
        const uint32_t v = u32(0);
        std::snprintf(text, sizeof(text), "(%.4f, %.4f, %.4f)", small(v & 0x7FF, 6), small((v >> 11) & 0x7FF, 6), small(v >> 22, 5));
        return text;
    }
    if (has("A2B10G10R10_UNORM") || has("A2R10G10B10_UNORM")) {
        const uint32_t v = u32(0);
        const bool bgr = has("A2R10G10B10");
        const uint32_t lo = v & 0x3FF, mid = (v >> 10) & 0x3FF, hi = (v >> 20) & 0x3FF;
        std::snprintf(text, sizeof(text), "(%.3f, %.3f, %.3f, %.3f)", (bgr ? hi : lo) / 1023.0, mid / 1023.0, (bgr ? lo : hi) / 1023.0, (v >> 30) / 3.0);
        return text;
    }
hex:
    std::string out = "bytes";
    for (uint8_t byte : b) {
        std::snprintf(text, sizeof(text), " %02x", byte);
        out += text;
    }
    return out;
}

/** What a draw's fragments at the pixel met, from its occlusion queries. */
std::string DrawOutcome(const PixelEvent& e) {
    auto measured = [&](int bit) { return (e.testsMeasured >> bit) & 1; };
    if (e.scissored) return "outside the scissor";
    if (!e.testsMeasured) return "not measured (the draw's pipeline could not be copied)";
    if (measured(0) && !e.covered) return "does not cover the pixel";
    if (measured(1) && !e.facing) return "culled";
    if (measured(2) && !e.shaded) return "discarded by the fragment shader";
    const bool depthFailed = measured(3) && !e.depthPassed;
    const bool stencilFailed = measured(4) && !e.stencilPassed;
    if (depthFailed && stencilFailed) return "failed the depth and stencil tests";
    if (depthFailed) return "failed the depth test";
    if (stencilFailed) return "failed the stencil test";
    if (measured(5) && !e.passed) return "failed the depth and stencil tests together";
    // Samples of every fragment the draw put there, tested against the depth and stencil from before the draw.
    if (measured(5)) return "wrote the pixel (" + std::to_string(e.passed) + (e.passed == 1 ? " sample passed)" : " samples passed)");
    return "covers the pixel";
}

/**
 * --pixel-data: the history as JSON, for GPU Inspector to show (parsePixelHistory in
 * app/src/renderer/pixel_history.ts). Every event is kept, the draws that do not reach the pixel
 * too; texels are hex strings of the bytes the replay read, in the formats it names.
 */
bool WritePixelHistoryData(const ReplayReport& report, const std::string& path) {
    const PixelHistoryResult& h = report.history;
    auto hex = [](const std::vector<uint8_t>& bytes) {
        static const char digits[] = "0123456789abcdef";
        std::string out;
        for (uint8_t b : bytes) {
            out += digits[b >> 4];
            out += digits[b & 15];
        }
        return "\"" + out + "\"";
    };
    auto strings = [](const std::vector<std::string>& list, size_t limit) {
        std::string out = "[";
        for (size_t i = 0; i < list.size() && i < limit; ++i) out += (i ? "," : "") + JsonString(list[i]);
        return out + "]";
    };
    std::string json = "{\"format\":\"gpu-inspector-pixel-history\",\"version\":1,\"device\":" + JsonString(report.device) +
                       ",\"image\":" + std::to_string(h.image) + ",\"x\":" + std::to_string(h.x) + ",\"y\":" + std::to_string(h.y) +
                       ",\"mip\":" + std::to_string(h.mip) + ",\"layer\":" + std::to_string(h.layer) +
                       ",\"pixelFormat\":" + JsonString(h.format) + ",\"depthFormat\":" + JsonString(h.depthFormat) + ",\"events\":[";
    for (size_t i = 0; i < h.events.size(); ++i) {
        const PixelEvent& e = h.events[i];
        json += std::string(i ? "," : "") + "{\"kind\":" + JsonString(e.kind) + ",\"command\":" + std::to_string(e.command) +
                ",\"method\":" + JsonString(e.method) + ",\"detail\":" + JsonString(e.detail) +
                ",\"commandBuffer\":" + std::to_string(e.commandBuffer) + ",\"frame\":" + std::to_string(e.frame) +
                ",\"passIndex\":" + std::to_string(e.passIndex) + ",\"pipeline\":" + std::to_string(e.pipeline) +
                ",\"scissored\":" + (e.scissored ? "true" : "false") + ",\"testsMeasured\":" + std::to_string(e.testsMeasured) +
                ",\"covered\":" + std::to_string(e.covered) + ",\"facing\":" + std::to_string(e.facing) +
                ",\"shaded\":" + std::to_string(e.shaded) + ",\"depthPassed\":" + std::to_string(e.depthPassed) +
                ",\"stencilPassed\":" + std::to_string(e.stencilPassed) + ",\"passed\":" + std::to_string(e.passed) +
                ",\"value\":" + hex(e.value) + ",\"depth\":" + hex(e.depth) + "}";
    }
    json += "],\"notes\":" + strings(h.notes, 100) + ",\"problems\":" + strings(report.problems, 100) + "}";
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(json.data(), (std::streamsize)json.size());
    return (bool)out;
}

/**
 * --draw-data: every draw and dispatch of the frame with the time it took and the counters it ran
 * up, for GPU Inspector's Shader Flame Graph (parseDrawStats in app/src/renderer/draw_stats.ts).
 */
bool WriteDrawData(const ReplayReport& report, const std::string& path) {
    std::string json = "{\"format\":\"gpu-inspector-draw-stats\",\"version\":1,\"device\":" + JsonString(report.device) +
                       ",\"note\":" + JsonString(report.drawStatsNote) + ",\"draws\":[";
    for (size_t i = 0; i < report.draws.size(); ++i) {
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
    for (size_t i = 0; i < report.problems.size() && i < 100; ++i) json += (i ? "," : "") + JsonString(report.problems[i]);
    json += "]}";
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(json.data(), (std::streamsize)json.size());
    return (bool)out;
}

void PrintDraws(const ReplayReport& report) {
    double total = 0;
    uint64_t fragments = 0;
    for (const DrawResult& d : report.draws) {
        total += d.durationMs;
        fragments += d.fragmentInvocations;
    }
    std::printf("draws measured: %zu, %.3f ms of draw time, %llu fragment shader invocations%s\n", report.draws.size(), total,
                (unsigned long long)fragments, report.drawStatsNote.empty() ? "" : (" (" + report.drawStatsNote + ")").c_str());
    for (size_t i = 0; i < report.draws.size() && i < 20; ++i) {
        const DrawResult& d = report.draws[i];
        const std::string where = d.passIndex == UINT32_MAX ? "outside a render pass" : "pass " + std::to_string(d.passIndex);
        const std::string samples = d.sampled ? ", " + std::to_string(d.samplesPassed) + " samples passed" : std::string();
        std::printf("  [%u] %s: %.4f ms%s, %llu vertex, %llu primitives, %llu fragment, %llu compute invocations%s\n", d.command, where.c_str(),
                    d.durationMs, d.timed ? "" : " (not timed)", (unsigned long long)d.vertexInvocations, (unsigned long long)d.primitives,
                    (unsigned long long)d.fragmentInvocations, (unsigned long long)d.computeInvocations, samples.c_str());
    }
    if (report.draws.size() > 20) std::printf("  ... %zu more\n", report.draws.size() - 20);
}

void PrintHistory(const PixelHistoryResult& h) {
    std::printf("pixel history: image %llu, pixel (%u, %u), mip %u, layer %u%s%s\n", (unsigned long long)h.image, h.x, h.y, h.mip, h.layer,
                h.format.empty() ? "" : (", " + h.format).c_str(), h.depthFormat.empty() ? "" : (", depth " + h.depthFormat).c_str());
    size_t untouched = 0;
    for (const PixelEvent& e : h.events) {
        const bool touched = e.kind != "draw" || (!e.scissored && (e.covered || !e.testsMeasured));
        if (!touched) {
            ++untouched;
            continue;
        }
        std::string line = "  [" + std::to_string(e.command) + "] ";
        if (e.kind == "load") {
            line += "command buffer " + std::to_string(e.commandBuffer) + ", pass " + std::to_string(e.passIndex) + " begins (" + e.detail + ")";
        } else if (e.kind == "clear") {
            line += e.method;
        } else {
            line += e.method + " (pipeline " + std::to_string(e.pipeline) + "): " + DrawOutcome(e);
        }
        std::printf("%s\n", line.c_str());
        std::printf("      value %s", FormatTexel(h.format, e.value, h.format.find("VK_FORMAT_D") == 0).c_str());
        if (!e.depth.empty()) std::printf(", depth %s", FormatTexel(h.depthFormat, e.depth, true).c_str());
        std::printf("\n");
    }
    if (untouched) std::printf("  %zu other draws in these passes do not reach the pixel\n", untouched);
    for (const std::string& n : h.notes) std::printf("  note: %s\n", n.c_str());
}

// ---------------------------------------------------------------------------------------------
// --dump: the captured, replayed and difference images of each compared target as PNG files.

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/** An 8-bit RGBA PNG with uncompressed (stored) deflate blocks: large, but needs no zlib. */
bool WritePng(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve(((size_t)width * 4 + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + (size_t)y * width * 4, rgba.begin() + ((size_t)y + 1) * width * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size();) {
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
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back((uint8_t)(adler >> s));

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
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
bool ToRgba(const TargetComparison& t, const std::vector<uint8_t>& bytes, std::vector<uint8_t>& rgba) {
    const size_t texels = (size_t)t.width * t.height;
    rgba.assign(texels * 4, 255);
    if (t.format.find("B8G8R8A8") != std::string::npos || t.format.find("R8G8B8A8") != std::string::npos) {
        const bool bgr = t.format.find("B8G8R8A8") != std::string::npos;
        if (bytes.size() < texels * 4) return false;
        for (size_t i = 0; i < texels; ++i) {
            rgba[i * 4] = bytes[i * 4 + (bgr ? 2 : 0)];
            rgba[i * 4 + 1] = bytes[i * 4 + 1];
            rgba[i * 4 + 2] = bytes[i * 4 + (bgr ? 0 : 2)];
        }
        return true;
    }
    if (t.format == "VK_FORMAT_D32_SFLOAT" || t.format == "VK_FORMAT_D32_SFLOAT_S8_UINT") {
        if (bytes.size() < texels * 4) return false;
        float lo = INFINITY, hi = -INFINITY;
        for (size_t i = 0; i < texels; ++i) {
            float v;
            std::memcpy(&v, &bytes[i * 4], 4);
            if (std::isfinite(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
        }
        for (size_t i = 0; i < texels; ++i) {
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
void Heat(uint16_t n, uint8_t* rgb) {
    static const struct { uint16_t upTo; uint8_t r, g, b; } kRamp[] = {
        {0, 0, 0, 0}, {1, 20, 40, 150}, {2, 0, 120, 230}, {3, 0, 190, 170}, {4, 110, 210, 40},
        {6, 240, 210, 0}, {10, 250, 120, 0}, {16, 220, 20, 20}, {32, 240, 0, 200}, {65535, 255, 255, 255},
    };
    for (const auto& step : kRamp) {
        if (n <= step.upTo) {
            rgb[0] = step.r;
            rgb[1] = step.g;
            rgb[2] = step.b;
            return;
        }
    }
}

void WriteOverdraw(const ReplayReport& report, const std::string& dir) {
    std::filesystem::create_directories(dir);
    for (const OverdrawResult& o : report.overdraw) {
        if (o.counts.empty()) continue;
        const std::string path = dir + "/overdraw_cb" + std::to_string(o.commandBuffer) + "_pass" + std::to_string(o.passIndex) +
                                 (report.overdraw.size() && o.frame ? "_frame" + std::to_string(o.frame) : "") +
                                 (o.depthTested ? "_tested.png" : "_all.png");
        std::vector<uint8_t> rgba((size_t)o.width * o.height * 4, 255);
        for (size_t i = 0; i < o.counts.size(); ++i) Heat(o.counts[i], &rgba[i * 4]);
        WritePng(path, o.width, o.height, rgba);
        std::printf("  wrote %s\n", path.c_str());
    }
}

void DumpTargets(const ReplayReport& report, const std::string& dir) {
    std::filesystem::create_directories(dir);
    for (const TargetComparison& t : report.targets) {
        if (!t.compared || t.captured.empty()) continue;
        const std::string base = dir + "/image" + std::to_string(t.image) + "_cb" + std::to_string(t.commandBuffer) + "_pass" +
                                 std::to_string(t.passIndex) + "_att" + std::to_string(t.attachment);
        std::vector<uint8_t> captured, replayed;
        if (!ToRgba(t, t.captured, captured) || !ToRgba(t, t.replayed, replayed)) {
            std::printf("  (%s: no PNG for this format)\n", base.c_str());
            continue;
        }
        std::vector<uint8_t> diff(captured.size(), 255);
        const size_t texel = t.texels ? t.captured.size() / t.texels : 4;
        for (size_t i = 0; i < t.texels && i * 4 < diff.size(); ++i) {
            uint32_t delta = 0;
            for (size_t k = 0; k < texel; ++k) delta = std::max<uint32_t>(delta, (uint32_t)std::abs((int)t.captured[i * texel + k] - (int)t.replayed[i * texel + k]));
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

/** Decodes every object and command of the capture; returns the process exit code. */
int Check(const CaptureFile& capture) {
    Arena arena;
    DecodeContext ctx(arena);
    // Every object the manifest holds resolves (to a stand-in handle: its id); others do not.
    ctx.resolve = [&](uint64_t id, std::string_view) -> uint64_t { return capture.Object(id) ? id : 0; };

    std::map<std::string, int> objectTypes;
    std::map<std::string, int> methods;
    std::map<std::string, int> undecodable;
    size_t objects = 0, commands = 0;

    if (const JValue* list = capture.Objects(); list && list->IsArray()) {
        for (uint32_t i = 0; i < list->count; ++i) {
            const JValue& o = list->items[i];
            const JValue* type = o.Get("type");
            const JValue* cmd = o.Get("cmd");
            const JValue* args = o.Get("args");
            std::string typeName(type ? type->Str() : "?");
            ++objectTypes[typeName];
            ++objects;
            if (!cmd || !args || args->IsNull()) continue;
            DecodeCheckFn fn = FindArgsDecoder(cmd->Str());
            if (!fn) {
                ++undecodable[std::string(cmd->Str())];
                continue;
            }
            ctx.where = typeName + " " + std::to_string(o.Get("id") ? o.Get("id")->Uint() : 0) + " (" + std::string(cmd->Str()) + ")";
            fn(ctx, *args);
            arena.Reset();
        }
    }
    if (const JValue* list = capture.Commands(); list && list->IsArray()) {
        for (uint32_t i = 0; i < list->count; ++i) {
            const JValue& c = list->items[i];
            const JValue* method = c.Get("method");
            const JValue* args = c.Get("args");
            if (!method) continue;
            ++methods[std::string(method->Str())];
            ++commands;
            if (!args || args->IsNull()) continue;
            DecodeCheckFn fn = FindArgsDecoder(method->Str());
            if (!fn) {
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
    for (auto& [type, n] : objectTypes) std::printf("  %-32s %d\n", type.c_str(), n);
    std::printf("commands: %zu\n", commands);
    for (auto& [m, n] : methods) std::printf("  %-40s %d%s\n", m.c_str(), n, FindReplayCommand(m) ? "" : "  (not a recordable vkCmd)");
    if (!undecodable.empty()) {
        std::printf("no decoder for:\n");
        for (auto& [m, n] : undecodable) std::printf("  %s (%d)\n", m.c_str(), n);
    }
    std::printf("problems: %zu\n", ctx.problems.size());
    PrintGrouped(ctx.problems, 60);
    return ctx.problems.empty() && undecodable.empty() ? 0 : 1;
}

int Replay(const CaptureFile& capture, const ReplayOptions& options, const std::string& dumpDir, const std::string& overdrawDir,
           const std::string& overdrawData, const std::string& pixelData, const std::string& drawData) {
    ReplayReport report;
    bool ran = false;
    {
        Replayer replayer;
        ran = replayer.Run(capture, options, report);
    }  // the device is destroyed here, so validation messages about teardown are in the report too
    std::printf("device: %s\n", report.device.c_str());
    std::printf("objects: %zu created, %zu left out\n", report.objectsCreated, report.objectsSkipped);
    std::printf("uploads: %zu sampled textures, %zu buffer ranges\n", report.texturesUploaded, report.bufferUploads);
    std::printf("commands: %zu recorded in %zu submissions\n", report.commandsRecorded, report.submissions);
    size_t exact = 0, differing = 0, skipped = 0;
    std::printf("render targets: %zu\n", report.targets.size());
    for (const TargetComparison& t : report.targets) {
        std::printf("  image %llu (command buffer %llu, pass %u, attachment %u, %s %ux%u %s): ", (unsigned long long)t.image,
                    (unsigned long long)t.commandBuffer, t.passIndex, t.attachment, t.format.c_str(), t.width, t.height, t.aspect.c_str());
        if (!t.compared) {
            ++skipped;
            std::printf("not compared: %s\n", t.note.c_str());
        } else if (t.differingTexels == 0 && t.note.empty()) {
            ++exact;
            std::printf("identical (%llu texels)\n", (unsigned long long)t.texels);
        } else {
            ++differing;
            std::printf("%llu of %llu texels differ, largest byte difference %u%s%s\n", (unsigned long long)t.differingTexels,
                        (unsigned long long)t.texels, t.maxByteDelta, t.note.empty() ? "" : "; ", t.note.c_str());
        }
    }
    if (options.overdraw) {
        std::printf("overdraw: %zu measurements\n", report.overdraw.size());
        for (const OverdrawResult& o : report.overdraw) {
            const double pixels = (double)o.width * o.height;
            std::printf("  command buffer %llu, pass %u, %s: ", (unsigned long long)o.commandBuffer, o.passIndex,
                        o.depthTested ? "fragments passing depth" : "every rasterized fragment");
            if (o.counts.empty()) {
                std::printf("not measured: %s\n", o.note.c_str());
                continue;
            }
            std::printf("%u draws%s, %llu fragments on %llu of %.0f pixels, %.3f per pixel, %.2f per covered pixel, max %u", o.draws,
                        o.skippedDraws ? (" (" + std::to_string(o.skippedDraws) + " not counted)").c_str() : "",
                        (unsigned long long)o.fragments, (unsigned long long)o.coveredPixels, pixels, pixels ? o.fragments / pixels : 0.0,
                        o.coveredPixels ? (double)o.fragments / o.coveredPixels : 0.0, o.maxCount);
            if (o.capturedFragments >= 0) std::printf("; the capture measured %lld fragment shader invocations", (long long)o.capturedFragments);
            if (!o.note.empty()) std::printf(" (%s)", o.note.c_str());
            std::printf("\n    pixels by count: 1: %llu, 2: %llu, 3: %llu, 4: %llu, 5-8: %llu, 9-16: %llu, 17-32: %llu, 33+: %llu\n",
                        (unsigned long long)o.histogram[0], (unsigned long long)o.histogram[1], (unsigned long long)o.histogram[2],
                        (unsigned long long)o.histogram[3], (unsigned long long)o.histogram[4], (unsigned long long)o.histogram[5],
                        (unsigned long long)o.histogram[6], (unsigned long long)o.histogram[7]);
        }
        if (!overdrawDir.empty()) WriteOverdraw(report, overdrawDir);
        if (!overdrawData.empty()) {
            if (WriteOverdrawData(report, overdrawData)) std::printf("  wrote %s\n", overdrawData.c_str());
            else std::printf("  could not write %s\n", overdrawData.c_str());
        }
    }
    if (options.drawStats) {
        PrintDraws(report);
        if (!drawData.empty()) {
            if (WriteDrawData(report, drawData)) std::printf("  wrote %s\n", drawData.c_str());
            else std::printf("  could not write %s\n", drawData.c_str());
        }
    }
    if (report.history.requested) {
        PrintHistory(report.history);
        if (!pixelData.empty()) {
            if (WritePixelHistoryData(report, pixelData)) std::printf("  wrote %s\n", pixelData.c_str());
            else std::printf("  could not write %s\n", pixelData.c_str());
        }
    }
    std::printf("problems: %zu\n", report.problems.size());
    PrintGrouped(report.problems, 80);
    if (options.validation) {
        std::printf("validation messages: %zu\n", report.validation.size());
        PrintGrouped(report.validation, 40);
    }
    if (!dumpDir.empty()) DumpTargets(report, dumpDir);
    if (!ran) return 2;
    return differing == 0 && skipped == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string dumpDir;
    std::string overdrawDir;
    std::string overdrawData;
    std::string pixelData;
    std::string drawData;
    bool check = false;
    ReplayOptions options;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--check")) check = true;
        else if (!std::strcmp(argv[i], "--validate")) options.validation = true;
        else if (!std::strcmp(argv[i], "--trace")) options.trace = true;
        else if (!std::strcmp(argv[i], "--overdraw") && i + 1 < argc) {
            overdrawDir = argv[++i];
            options.overdraw = true;
        }
        else if (!std::strcmp(argv[i], "--overdraw-data") && i + 1 < argc) {
            overdrawData = argv[++i];
            options.overdraw = true;
        }
        else if (!std::strcmp(argv[i], "--pixel-data") && i + 1 < argc) pixelData = argv[++i];
        else if (!std::strcmp(argv[i], "--draws")) options.drawStats = true;
        else if (!std::strcmp(argv[i], "--draw-data") && i + 1 < argc) {
            drawData = argv[++i];
            options.drawStats = true;
        }
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) {
            dumpDir = argv[++i];
            options.keepPixels = true;
        }
        else if (!std::strcmp(argv[i], "--pixel") && i + 3 < argc) {
            options.history.enabled = true;
            options.history.image = std::strtoull(argv[++i], nullptr, 10);
            options.history.x = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
            options.history.y = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        }
        else if (!std::strcmp(argv[i], "--mip") && i + 1 < argc) options.history.mip = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--layer") && i + 1 < argc) options.history.layer = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (argv[i][0] != '-' && path.empty()) path = argv[i];
        else {
            PrintUsage();
            return 2;
        }
    }
    if (path.empty()) {
        PrintUsage();
        return 2;
    }
    CaptureFile capture;
    std::string error;
    if (!capture.Load(path, error)) {
        std::fprintf(stderr, "vkinsp_replay: %s\n", error.c_str());
        return 2;
    }
    if (!pixelData.empty() && !options.history.enabled) {
        std::fprintf(stderr, "vkinsp_replay: --pixel-data needs --pixel <image> <x> <y>\n");
        return 2;
    }
    return check ? Check(capture) : Replay(capture, options, dumpDir, overdrawDir, overdrawData, pixelData, drawData);
}
