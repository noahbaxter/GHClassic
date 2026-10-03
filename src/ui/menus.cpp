#include "ui/menus.h"

#include "build_info.h"
#include "content/locale.h"
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
        script::patchUi("{set $ghc_version \"" + build::shownVersion() + "\"}");
        script::patchUi(kMainMenuDta);
        script::patchUi(kHighscorePanelDta);
        script::patchUi(kSelCharacterDta);
        script::patchUi(kQuickplayDta);
        locale::add("setlist_gh1", "GH1");
        locale::add("setlist_gh2", "GH2");
        locale::add("setlist_gh2x", "360");
        locale::add("setlist_gh80s", "80s");
    }
}
