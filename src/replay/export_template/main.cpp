// A frame exported from a GPU Inspector capture (see README.md): re-creates the frame's objects,
// uploads what they held, records and submits its command buffers, and compares every render
// target with what the capture holds.
//
//   frame [--validate] [--data <frame_data.bin>] [--out <directory>] [--no-images]
//
// Exit code 0 when every compared target is identical to the capture's, 1 when some differ or
// could not be compared, 2 when a Vulkan call failed.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "frame_handles.h"
#include "vk_support.h"

int main(int argc, char** argv) {
    bool validate = false;
    bool writeImages = true;
    std::string dataPath;
    std::string outDir = "out";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--validate")) validate = true;
        else if (!std::strcmp(argv[i], "--no-images")) writeImages = false;
        else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) dataPath = argv[++i];
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) outDir = argv[++i];
        else {
            std::fprintf(stderr, "usage: %s [--validate] [--data <frame_data.bin>] [--out <directory>] [--no-images]\n", argv[0]);
            return 2;
        }
    }
    // The data file: as given, else in the working directory, else beside the executable.
    if (dataPath.empty()) {
        dataPath = "frame_data.bin";
        std::error_code ec;
        if (!std::filesystem::exists(dataPath, ec)) dataPath = (std::filesystem::path(argv[0]).parent_path() / "frame_data.bin").string();
    }
    if (!LoadData(dataPath)) {
        std::fprintf(stderr, "could not read %s (--data names it)\n", dataPath.c_str());
        return 2;
    }

    PFN_vkGetInstanceProcAddr getInstanceProcAddr = LoadVulkanLoader();
    if (!getInstanceProcAddr) {
        std::fprintf(stderr, "the Vulkan loader was not found\n");
        return 2;
    }
    LoadGlobalFunctions(getInstanceProcAddr);
    CreateInstance(validate);
    CreateDevice();
    std::printf("device: %s\n", DeviceName());

    CreateObjects();
    UploadContents();
    Frame();

    const int code = ReportResults(outDir, writeImages);
    vkDeviceWaitIdle(device);
    DestroyObjects();
    DestroySupport();
    return code;
}
