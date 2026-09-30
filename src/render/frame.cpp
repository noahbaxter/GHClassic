#include "render/frame.h"

namespace gh2
{
    FrameQueue &frames()
    {
        static FrameQueue queue;
        return queue;
    }

    Frame &building()
    {
        static Frame frame;
        return frame;
    }
}
