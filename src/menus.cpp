#include "menus.h"

#include "dta.h"
#include "script.h"

#include <SDL3/SDL.h>

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
        script::runWhenUiReady(kMainMenuDta);
        script::runWhenUiReady(kHighscorePanelDta);
    }
}
