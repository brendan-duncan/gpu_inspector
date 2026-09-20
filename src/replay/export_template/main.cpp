// A frame exported from a GPU Inspector capture (see README.md): re-creates the frame's objects,
// uploads what they held, and runs its command buffers.
//
//   frame [--batch] [--frames <n>] [--no-vsync] [--validate] [--data <frame_data.bin>] [--out <directory>] [--no-images]
//
// By default the frame is shown in a window, run again and again until the window is closed (or
// Escape is pressed, or --frames have been shown): each time the command buffers are recorded and
// submitted, what the frame leaves on screen is presented, and its images are put back in the
// layouts the frame expects to find them in. That is a program a profiler or a frame debugger can
// be pointed at. Exit code 0, 1 when the validation layer reported errors, 2 when a call failed.
//
// --batch runs the frame once without a window and compares every render target with what the
// capture holds, which is how to tell whether this machine draws what the captured one drew. Exit
// code 0 when every compared target is identical to the capture's, 1 when some differ or could not
// be compared, 2 when a Vulkan call failed. It is also what runs where no window can be opened.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "frame_handles.h"
#include "vk_support.h"

int main(int argc, char** argv) {
    bool batch = false;
    bool validate = false;
    bool writeImages = true;
    unsigned long long frames = 0;
    std::string dataPath;
    std::string outDir = "out";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--batch")) batch = true;
        else if (!std::strcmp(argv[i], "--validate")) validate = true;
        else if (!std::strcmp(argv[i], "--no-vsync")) SetOutputVsync(false);
        else if (!std::strcmp(argv[i], "--no-images")) writeImages = false;
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) dataPath = argv[++i];
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) outDir = argv[++i];
        else {
            std::fprintf(stderr, "usage: %s [--batch] [--frames <n>] [--no-vsync] [--validate] [--data <frame_data.bin>] [--out <directory>] [--no-images]\n", argv[0]);
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
    // A window needs the surface and swapchain extensions, which the instance and the device are created with.
    WantWindow(!batch);
    CreateInstance(validate);
    CreateDevice();
    std::printf("device: %s\n", DeviceName());

    CreateObjects();
    UploadContents();

    // The window is opened before the frame first runs: with one, the frame takes no read-backs.
    FrameOutputInfo output;
    if (!batch) {
        const bool named = FrameOutput(&output);
        if (!named) std::fprintf(stderr, "the frame has no output to show\n");
        if (!named || !OpenOutputWindow(output, "Exported frame (Vulkan)")) {
            std::fprintf(stderr, "running as --batch instead\n");
            batch = true;
        }
    }

    int code = 0;
    if (batch) {
        Frame();
        code = ReportResults(outDir, writeImages);
    } else {
        for (unsigned long long shown = 0;;) {
            Frame();
            if (!PresentOutput(output)) break;
            if (frames && ++shown >= frames) break;
            RestoreFrame();
        }
        code = CloseOutputWindow();
    }
    vkDeviceWaitIdle(device);
    DestroyObjects();
    DestroySupport();
    return code;
}
