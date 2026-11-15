#include "custom/songs.h"

#include "custom/gh2_chart.h"
#include "save/save.h"

namespace gh2::custom
{
    namespace
    {
        Part s_instrument = Part::Guitar;
    }

    Catalog &catalog()
    {
        static Catalog &s_catalog = *new Catalog;
        return s_catalog;
    }

    void setInstrument(Part part)
    {
        s_instrument = part;
        catalog().sortBy(catalog().sort(), instrumentName());
    }

    const char *instrumentName() { return s_instrument == Part::Bass ? "bass" : "guitar"; }

    void keepListing()
    {
        save::keepState("custom", "instrument", instrumentName());
        save::keepState("custom", "sort", kSortNames[static_cast<int>(catalog().sort())]);
    }

    Part playedPart(uint8_t parts) { return leadPart(parts).value_or(Part::Bass); }

    Part played(const Song &song)
    {
        return s_instrument == Part::Bass && hasPart(song.parts, Part::Bass) ? Part::Bass : playedPart(song.parts);
    }

    std::string playedStem(Part part)
    {
        return part == Part::Guitar || part == Part::Coop ? "guitar" : part == Part::Rhythm ? "rhythm" : "bass";
    }

    std::string base(const std::string &id, uint8_t parts, Part part)
    {
        const std::string dir = "songs/" + id + "/" + id;
        return part == Part::Bass && playedPart(parts) != Part::Bass ? dir + "_bass" : dir;
    }
}
