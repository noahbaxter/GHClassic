// A hack of GH2's form: GH2's code over its own files at GH2's paths, as
// Rocks the 80s is (gh80s/install.cpp), every name on it under the hack's
// id (content/sources.h), so its songs and outfits sit beside the retail
// discs' of the same name.

#include "hacks/install.h"

#include "content/campaigns.h"
#include "content/games.h"
#include "content/hidden_guitars.h"
#include "content/setlists.h"
#include "content/sources.h"
#include "disc/ark.h"

namespace gh2
{
    void installHacks()
    {
        games::nameHacks();
        for (const std::string &id : games::hacks())
        {
            const size_t disc = *games::disc(id);
            // Before its config is read: its names by our ids.
            sources::addDisc(id, disc);
            const size_t layer = ark::addLayer(disc);
            stockStore(id, disc, layer);
            campaigns::add(id, layer);
            setlists::addDisc(id, disc, false);
        }
    }
}
