#include "ui/zebra_wall.h"

#include "content/games.h"
#include "disc/ark.h"
#include "gh1/scene.h"
#include "milo/milo.h"
#include "zebra_wall_bitmap.h"

#include <algorithm>
#include <iostream>

namespace gh2
{
    void installZebraWall()
    {
        const auto disc = games::disc("gh80s");
        if (!disc)
            return;
        // Both song lists tile ss_bigwall.tex, ../image/og/amp.bmp, over the wall.
        for (const std::string path : {"ui/gen/sel_song.milo_ps2", "ui/gen/sel_song_quickplay.milo_ps2"})
            ark::addFix(*disc, path, [path](std::vector<uint8_t> &file)
                        {
                            auto scene = milo::read(file);
                            const auto at = scene ? milo::find(*scene, "ss_bigwall.tex") : std::nullopt;
                            if (!at)
                            {
                                std::cerr << "[zebra_wall] no ss_bigwall.tex in " << path << std::endl;
                                return;
                            }
                            const milo::Bytes bitmap(std::begin(kZebraWall), std::end(kZebraWall));
                            milo::Bytes body = gh1::texWith(scene->bodies[*at], bitmap, true);
                            if (body.empty())
                                return;
                            scene->bodies[*at] = std::move(body);
                            file = milo::write(*scene);
                        });
    }
}
