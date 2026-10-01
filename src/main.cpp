// GH Classic's entry point: the symbolized GH2 ELF, the disc it came from, and
// options for unattended runs.
//
//   GHClassic <elf> [disc] [--scenario <file>] [--hidden] [--mute] [--mc <dir>] [--settings <file>]
//            [--shots <dir>] [--shot-every <n>] [--res window|native|480p|720p|1080p|1440p|2160p]
//   GHClassic --bind [keyboard | <n>]
//
// --scenario runs a file of steps (scenario.h). --res is what the scene is
// drawn at: the window's size (the default), the
// game's own (512x448), or that height at the picture's aspect. --settings
// reads and writes that file in place of the user data directory's. --bind
// sets up a controller or the keyboard in input.ini; with no device it
// lists them.

#include "host/bind.h"
#include "host/vulkan_frontend.h"
#include "scenario.h"
#include "settings.h"
#include "ps2_runtime.h"
#include "runtime/ps2_disc_image.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

int main(int argc, char *argv[])
{
    if (argc >= 2 && std::string(argv[1]) == "--bind")
        return gh2::runBind(argc, argv);
    if (argc < 2)
    {
        std::cerr << "usage: GHClassic <elf> [disc] [--hidden] [--mute] [--mc <dir>]"
                     " [--shots <dir>] [--shot-every <n>] [--res window|native|480p|720p|1080p|1440p|2160p]"
                  << std::endl;
        return 2;
    }

    const std::string elfPath = argv[1];
    std::filesystem::path discPath;
    std::filesystem::path mcRoot;
    PS2Runtime::HostOptions hostOptions;
    gh2::RenderSize renderSize;
    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--hidden")
            hostOptions.hidden = true;
        else if (arg == "--mute")
            hostOptions.mute = true;
        else if (arg == "--mc" && hasValue)
            mcRoot = argv[++i];
        else if (arg == "--scenario" && hasValue)
        {
            if (!gh2::scenario::load(argv[++i]))
                return 1;
        }
        else if (arg == "--settings" && hasValue)
            gh2::settings::usePath(argv[++i]);
        else if (arg == "--shots" && hasValue)
            hostOptions.shotDir = argv[++i];
        else if (arg == "--shot-every" && hasValue)
            hostOptions.shotEvery = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (arg == "--res" && hasValue)
        {
            const std::string res = argv[++i];
            if (res == "window")
                renderSize.mode = gh2::RenderSize::kWindow;
            else if (res == "native")
                renderSize.mode = gh2::RenderSize::kNative;
            else if (res == "480p" || res == "720p" || res == "1080p" || res == "1440p" || res == "2160p")
            {
                renderSize.mode = gh2::RenderSize::kHeight;
                renderSize.height = static_cast<uint32_t>(std::strtoul(res.c_str(), nullptr, 10));
            }
            else
            {
                std::cerr << "--res takes window, native, 480p, 720p, 1080p, 1440p or 2160p, not " << res
                          << std::endl;
                return 2;
            }
        }
        else if (!arg.empty() && arg[0] != '-' && discPath.empty())
            discPath = arg;
        else
        {
            std::cerr << "unknown argument: " << arg << std::endl;
            return 2;
        }
    }
    if (!hostOptions.shotDir.empty() && hostOptions.shotEvery == 0u)
        hostOptions.shotEvery = 60u;

    PS2Runtime runtime;
    runtime.setHostOptions(hostOptions);
    runtime.setHostFrontend(std::make_unique<gh2::VulkanFrontend>(renderSize));
    if (!runtime.initialize("GH Classic"))
    {
        std::cerr << "failed to initialize the runtime" << std::endl;
        return 1;
    }

    // The disc serves cdrom0: in place of the ELF's directory.
    if (!discPath.empty())
    {
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = discPath;
        PS2Runtime::setIoPaths(paths);
        if (!ps2ConfiguredDisc())
            return 1;
    }

    if (!runtime.loadELF(elfPath))
    {
        std::cerr << "failed to load " << elfPath << std::endl;
        return 1;
    }

    // loadELF points the memory card beside the ELF, so this goes after.
    if (!mcRoot.empty())
    {
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.mcRoot = mcRoot;
        PS2Runtime::setIoPaths(paths);
    }

    runtime.run();

    // Guest threads may still hold host resources; skip static destructors.
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(0);
}
