// GH Classic's entry point: the player's GH2 disc, and options for unattended
// runs.
//
//   GHClassic [disc] [--scenario <file>] [--hidden] [--mute] [--speed <x>] [--fast-boot] [--seed <n>]
//            [--save <file>] [--import-card <ps2>] [--export-card <ps2>]
//            [--settings <file>] [--shots <dir>] [--shot-every <n>]
//            [--res window|native|480p|720p|1080p|1440p|2160p]
//   GHClassic --bind [keyboard | <n>]
//
// Without a disc, the first image in PUT_DISC_HERE (disc/disc.h) with this
// build's executable. The game runs from that executable, read off the disc.
// --scenario runs a file of steps (dev/scenario.h). --speed runs the game's
// clock that many times real time, hidden and muted only, as nothing else
// paces it. --fast-boot boots straight to the main menu, past the logos'
// padding, the intro movie and the press-start splash, whatever settings.ini's
// fast_boot says. --seed starts the
// game's random numbers from n rather than the time of day.
// --res is what the scene is drawn at: the window's size (the default), the
// game's own (512x448), or that height at the picture's aspect. --settings
// and --save read and write that file in place of the user data directory's.
// --import-card loads the save off a PCSX2 card and makes it the save file;
// --export-card writes it to one (save/save.h). --bind
// sets up a controller or the keyboard in input.ini; with no device it
// lists them.

#include "disc/disc.h"
#include "ui/fast_boot.h"
#include "host/bind.h"
#include "host/update.h"
#include "host/vulkan_frontend.h"
#include "save/save.h"
#include "dev/scenario.h"
#include "dev/seed.h"
#include "settings/settings.h"
#include "ps2_runtime.h"
#include "runtime/host_clock.h"
#include "runtime/ps2_disc_image.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <io.h> // dup2
#else
#include <unistd.h>
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace
{
    // file:///... with anything but unreserved characters and '/' escaped,
    // as macOS takes no space in a URL.
    std::string fileUrl(const std::filesystem::path &path)
    {
        std::string url = "file://";
        std::string generic = path.generic_string();
        if (generic.empty() || generic[0] != '/')
            generic = "/" + generic; // C:/... on Windows
        for (unsigned char c : generic)
        {
            if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':')
                url += static_cast<char>(c);
            else
            {
                char hex[4];
                std::snprintf(hex, sizeof(hex), "%%%02X", c);
                url += hex;
            }
        }
        return url;
    }

    // To stderr, and in a message box for a player, who has no terminal.
    int fail(const PS2Runtime::HostOptions &options, const std::string &message)
    {
        std::cerr << message << std::endl;
        if (!options.hidden)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GH Classic", message.c_str(), nullptr);
        return 1;
    }
}

int main(int argc, char *argv[])
{
    if (argc >= 2 && std::string(argv[1]) == "--bind")
        return gh2::runBind(argc, argv);

    std::filesystem::path discPath;
    PS2Runtime::HostOptions hostOptions;
    gh2::RenderSize renderSize;
    double speed = 1.0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--hidden")
            hostOptions.hidden = true;
        else if (arg == "--mute")
            hostOptions.mute = true;
        else if (arg == "--speed" && hasValue)
            speed = std::strtod(argv[++i], nullptr);
        else if (arg == "--fast-boot")
            gh2::fast_boot::enable();
        else if (arg == "--seed" && hasValue)
            gh2::seed::fix(std::atoi(argv[++i]));
        else if (arg == "--scenario" && hasValue)
        {
            if (!gh2::scenario::load(argv[++i]))
                return 1;
        }
        else if (arg == "--settings" && hasValue)
            gh2::settings::usePath(argv[++i]);
        else if (arg == "--save" && hasValue)
            gh2::save::usePath(argv[++i]);
        else if (arg == "--import-card" && hasValue)
            gh2::save::importCard(argv[++i]);
        else if (arg == "--export-card" && hasValue)
            gh2::save::exportCard(argv[++i]);
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
    if (speed != 1.0)
    {
        // A window's swapchain and an audio device pace in real time.
        if (!(speed > 0.0) || !hostOptions.hidden || !hostOptions.mute)
        {
            std::cerr << "--speed takes a number above 0, with --hidden and --mute" << std::endl;
            return 2;
        }
        ps2x::host_clock::setSpeed(speed);
    }
    // After --settings, which names the file it is read from.
    if (gh2::settings::get(gh2::settings::kFastBoot))
        gh2::fast_boot::enable();

    // Started from a file manager there is no terminal to read, so the output
    // goes to GHClassic.log beside the settings. Hidden runs keep theirs. On
    // Windows the game is a GUI program with no console at all, and isatty
    // takes the NUL device for one.
#ifdef _WIN32
    const bool terminal = false;
#else
    const bool terminal = isatty(fileno(stderr));
#endif
    if (!hostOptions.hidden && !terminal)
    {
        const std::string log = gh2::settings::userDataPath("GHClassic.log");
        if (std::freopen(log.c_str(), "w", stderr))
            dup2(fileno(stderr), fileno(stdout));
    }

    // A player who takes a newer release is sent to it and not into this one.
    if (!hostOptions.hidden && gh2::settings::get(gh2::settings::kCheckUpdates) && gh2::update::offer())
        return 0;

    // The disc, and this build's executable on it, before any window opens.
    std::string why;
    if (discPath.empty())
    {
        discPath = gh2::disc::find(why);
        if (discPath.empty())
        {
            const std::filesystem::path folder = gh2::disc::folder();
            if (!hostOptions.hidden)
                SDL_OpenURL(fileUrl(folder).c_str());
            return fail(hostOptions, "Put your Guitar Hero II (USA) disc image (.iso, .chd or .bin) in\n" +
                                         folder.string() + "\nthen start GH Classic again.\n\n" + why);
        }
    }
    const std::vector<uint8_t> elf = gh2::disc::bootElf(discPath, why);
    if (elf.empty())
        return fail(hostOptions, discPath.string() + ": " + why);

    PS2Runtime runtime;
    runtime.setHostOptions(hostOptions);
    runtime.setHostFrontend(std::make_unique<gh2::VulkanFrontend>(renderSize));
    if (!runtime.initialize("GH Classic"))
        return fail(hostOptions, "GH Classic could not open its window. It needs a GPU with Vulkan 1.2 or newer.");

    // The disc serves cdrom0: in place of the ELF's directory.
    PS2Runtime::IoPaths discPaths = PS2Runtime::getIoPaths();
    discPaths.cdImage = discPath;
    PS2Runtime::setIoPaths(discPaths);
    if (!ps2ConfiguredDisc())
        return fail(hostOptions, "could not open " + discPath.string());

    // loadELF takes a file, so the executable goes through a temporary one.
    const std::filesystem::path elfPath =
        std::filesystem::temp_directory_path() / ("GHClassic-" + std::to_string(std::random_device()()) + ".elf");
    {
        std::ofstream out(elfPath, std::ios::binary);
        out.write(reinterpret_cast<const char *>(elf.data()), static_cast<std::streamsize>(elf.size()));
    }
    const bool loaded = runtime.loadELF(elfPath.string());
    std::error_code removeError;
    std::filesystem::remove(elfPath, removeError);
    if (!loaded)
        return fail(hostOptions, "could not load the executable from " + discPath.string());

    runtime.run();

    // Guest threads may still hold host resources; skip static destructors.
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(0);
}
