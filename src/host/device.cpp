#include "host/device.h"

namespace gh2::input
{
    namespace
    {
        std::vector<Device> s_devices;
    }

    std::vector<Device> &devices()
    {
        return s_devices;
    }
}
