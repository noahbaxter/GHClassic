#include "content/songs.h"

#include "script.h"

namespace gh2::songs
{
    namespace
    {
        std::string s_text;
    }

    void add(const std::string &entry)
    {
        // SongProvider and GameConfig find songs in this one array
        // (SystemConfig(songs), 0x117398).
        s_text += "{push_back {find $syscfg songs} " + entry + "}\n";
    }

    void install()
    {
        script::runWhenUiReady(s_text);
    }
}
