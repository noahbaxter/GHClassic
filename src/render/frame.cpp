#include "render/frame.h"

#include <cstring>

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

    uint32_t internCamera(const Camera &camera)
    {
        std::vector<Camera> &cameras = building().cameras;
        if (cameras.empty() || std::memcmp(&cameras.back(), &camera, sizeof(Camera)) != 0)
            cameras.push_back(camera);
        return static_cast<uint32_t>(cameras.size() - 1u);
    }
}
