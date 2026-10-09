// The releases come from GitHub's API through the system's curl, which
// Windows 10, macOS and nearly every Linux carry: no HTTP or TLS library of
// the game's own. Without curl, or offline, nothing is offered.

#include "host/update.h"

#include "build_info.h"
#include "host/releases.h"

#include <SDL3/SDL.h>

#include <string>

namespace gh2::update
{
    namespace
    {
        constexpr const char *kReleases = "https://api.github.com/repos/noahbaxter/GHClassic/releases?per_page=50";
        constexpr const char *kPage = "https://github.com/noahbaxter/GHClassic/releases/tag/";

        // What curl prints for the address, or nothing.
        std::string fetch(const char *url)
        {
            const char *args[] = {"curl", "--silent", "--fail", "--location", "--connect-timeout", "2",
                                  "--max-time", "4", "--header", "Accept: application/vnd.github+json", url, nullptr};
            SDL_Process *process = SDL_CreateProcess(args, true);
            if (!process)
                return {};
            size_t size = 0;
            int code = 1;
            void *data = SDL_ReadProcess(process, &size, &code);
            std::string out = data && code == 0 ? std::string(static_cast<const char *>(data), size) : std::string();
            SDL_free(data);
            SDL_DestroyProcess(process);
            return out;
        }
    }

    bool offer()
    {
        // An untagged build is no release and asks nothing.
        if (!isRelease(build::kVersion))
            return false;
        const std::string newest = newerRelease(fetch(kReleases), build::kVersion);
        if (newest.empty())
            return false;

        const char *name = build::kExperimental ? "GH Classic Experimental" : "GH Classic";
        const std::string message = std::string(name) + " " + newest + " is out. You have " + build::kVersion + ".";
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Not now"},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Download"},
        };
        const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, nullptr, "Update available", message.c_str(),
                                        2, buttons, nullptr};
        int chosen = 0;
        if (!SDL_ShowMessageBox(&box, &chosen) || chosen != 1)
            return false;
        SDL_OpenURL((kPage + newest).c_str());
        return true;
    }
}
