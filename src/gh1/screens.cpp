#include "gh1/screens.h"

namespace gh2::gh1
{
    const std::vector<Screen> &screens()
    {
        static const std::vector<Screen> kScreens = {
            {"main"},
        };
        return kScreens;
    }
}
