#include "render/frame.h"

namespace gh2
{
    FrameMailbox &frames()
    {
        static FrameMailbox mailbox;
        return mailbox;
    }

    Frame &building()
    {
        static Frame frame;
        return frame;
    }
}
