#include "render/frame.h"

namespace gh2
{
    FrameMailbox &frames()
    {
        static FrameMailbox mailbox;
        return mailbox;
    }
}
