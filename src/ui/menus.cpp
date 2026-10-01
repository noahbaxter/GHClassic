#include "ui/menus.h"

#include "build_info.h"
#include "dta.h"
#include "script.h"

#include <SDL3/SDL.h>

#include <string>

namespace gh2
{
    namespace
    {
        // {quit_app}: closes the app as closing its window does.
        script::Node quitApp(const script::Call &)
        {
            SDL_Event event{};
            event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&event);
            return {};
        }
    }

    void installMenus(PS2Runtime &)
    {
        script::addCommand("quit_app", quitApp);
        // The main menu's version line (main_menu.dta).
        static const std::string version = "{set $ghc_version \"" + build::shownVersion() + "\"}";
        script::runWhenUiReady(version.c_str());
        script::runWhenUiReady(kMainMenuDta);
        script::runWhenUiReady(kHighscorePanelDta);
        script::runWhenUiReady(kSelCharacterDta);
    }
}
