// vkinsp_replay: re-executes a GPU Inspector capture (.gpucap) on this machine's GPU.
//
//   vkinsp_replay <capture.gpucap> [--validate]
//       Re-creates the capture's objects, replays its command buffers, and compares every render
//       target the capture read back with the replay's own copy at the same point. Exit code 0
//       when every compared target matches exactly, 1 when some differ or could not be compared,
//       2 when the replay could not run.
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
#include "vk_decode.gen.h"

using namespace vkreplay;

namespace {

void PrintUsage() {
    std::fprintf(stderr, "usage: vkinsp_replay <capture.gpucap> [--validate] [--dump <dir>] | --check\n");
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

int Replay(const CaptureFile& capture, const ReplayOptions& options, const std::string& dumpDir) {
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
    bool check = false;
    ReplayOptions options;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--check")) check = true;
        else if (!std::strcmp(argv[i], "--validate")) options.validation = true;
        else if (!std::strcmp(argv[i], "--trace")) options.trace = true;
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) {
            dumpDir = argv[++i];
            options.keepPixels = true;
        }
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
    return check ? Check(capture) : Replay(capture, options, dumpDir);
}
