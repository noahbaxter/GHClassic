#include "movie/screen.h"

#include <mutex>

namespace gh2::movie
{
    namespace
    {
        std::mutex s_mutex;
        Picture s_picture;
        bool s_showing = false;
        uint64_t s_serial = 0;
    }

    void showPicture(Picture &picture)
    {
        std::lock_guard lock(s_mutex);
        picture.serial = ++s_serial;
        std::swap(s_picture, picture);
        s_showing = true;
    }

    void endPictures()
    {
        std::lock_guard lock(s_mutex);
        s_showing = false;
    }

    bool latestPicture(Picture &out)
    {
        std::lock_guard lock(s_mutex);
        if (s_showing && out.serial != s_picture.serial)
            out = s_picture;
        return s_showing;
    }
}
