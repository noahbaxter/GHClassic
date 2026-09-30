#include "menus.h"

#include "dta.h"
#include "script.h"

namespace gh2
{
    void installMenus(PS2Runtime &)
    {
        script::runWhenUiReady(kHighscorePanelDta);
    }
}
