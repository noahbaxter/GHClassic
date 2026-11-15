#include "custom/command.h"

#include "content/locale.h"
#include "custom/audio.h"
#include "custom/gh2_chart.h"
#include "custom/scan_status.h"
#include "custom/song_files.h"
#include "custom/songs.h"
#include "custom/text.h"
#include "save/results.h"
#include "save/save.h"
#include "script.h"

#include <algorithm>
#include <iostream>

namespace gh2::custom
{
    namespace
    {
        // The art of the song on and of the songs either side of it in the
        // list, nearest first, read ahead for a scroll to find it there.
        void artAround(const std::string &id)
        {
            constexpr ptrdiff_t kAround = 20;
            // Again for the same song once the list is made again under it.
            static std::string s_on;
            static Snapshot s_list;
            const Snapshot list = catalog().now();
            if (id == s_on && list == s_list)
                return;
            s_on = id;
            s_list = list;
            const auto it = std::find_if(list->begin(), list->end(), [&](const Listed &l) { return l.id == id; });
            if (it == list->end())
                return wantArt({catalog().find(id)});
            const ptrdiff_t at = it - list->begin(), n = static_cast<ptrdiff_t>(list->size());
            std::vector<std::shared_ptr<const Song>> songs{it->song};
            for (ptrdiff_t d = 1; d <= kAround; ++d)
                for (const ptrdiff_t i : {at + d, at - d})
                    if (i >= 0 && i < n)
                        songs.push_back((*list)[static_cast<size_t>(i)].song);
            wantArt(songs);
        }

        // A chart's sections by name at most, each a locale token: past
        // this a row shows its token.
        constexpr size_t kMostSections = 1000u;

        // Its best on the hardest difficulty with one: score, stars, and percent
        // or FC.
        std::string bestLine(const std::string &id)
        {
            for (const char *difficulty : {"expert", "hard", "medium", "easy"})
                if (const auto best = save::keptBest(save::playedKey(id, difficulty)))
                    return std::to_string(best->score) + "  " + std::to_string(best->stars) + "*  " +
                           (best->fullCombo ? std::string("FC") : std::to_string(best->percent) + "%");
            return "NOT PLAYED";
        }

        script::Node customCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "instrument")
            {
                if (call.size() > 2)
                {
                    setInstrument(call.symbol(2) == "bass" ? Part::Bass : Part::Guitar);
                    keepListing();
                }
                return call.word(instrumentName());
            }
            if (op == "sort")
            {
                // sort_<name>, as the picker lists them (ui/dta/quickplay.dta).
                const std::string prefix = "sort_";
                if (call.size() > 2)
                {
                    const std::string to = call.symbol(2);
                    const auto sort = to.rfind(prefix, 0u) == 0u ? sortNamed(to.substr(prefix.size())) : std::nullopt;
                    if (!sort || *sort == catalog().sort())
                        return {0u, script::kInt};
                    catalog().sortBy(*sort, instrumentName());
                    keepListing();
                    return {1u, script::kInt};
                }
                return call.word(prefix + kSortNames[static_cast<int>(catalog().sort())]);
            }
            if (op == "scan")
            {
                // With two numbers, the scan said to be that far along
                // (for a scenario to look at); 0 of 0 ends it.
                if (call.size() > 3)
                {
                    ScanStatus &status = scanStatus();
                    status.seen = static_cast<size_t>(call.number(2));
                    status.expected = static_cast<size_t>(call.number(3));
                    status.running = status.expected != 0u;
                }
                return call.word(scanText());
            }
            if (op == "section")
            {
                const auto playing = playingSong();
                const size_t n = call.index(2);
                const std::string name = playing && n < playing->sections.size() ? plain(playing->sections[n]) : "";
                return call.word(name);
            }
            const std::string id = call.symbol(2);
            const auto song = id.rfind("cs_", 0u) == 0u ? catalog().find(id) : nullptr;
            if (op == "ours")
                return {song ? 1u : 0u, script::kInt};
            if (op == "tag")
            {
                const std::string key = call.symbol(3);
                const std::string text = !song           ? ""
                                         : key == "name" ? title(*song)
                                                         : plain(tag(song->tags, key).value_or(""));
                return call.word(text);
            }
            if (op == "art")
            {
                if (song)
                    artAround(id);
                return {song && heldArt(*song) ? 1u : 0u, script::kInt};
            }
            if (op == "ready")
                return {song && stemsReady(*song) ? 1u : 0u, script::kInt};
            if (op == "level")
            {
                const auto at = song ? level(*song, call.symbol(3)) : std::nullopt;
                return call.word(at ? std::to_string(*at) : "");
            }
            if (op == "track")
                return {song && has(*song, call.symbol(3)) ? 1u : 0u, script::kInt};
            if (op == "length")
                return call.word(lengthText(song ? song->length : 0.0));
            if (op == "best")
                return call.word(bestLine(id));
            // The part it plays: quickplay's instrument's, or with a TRUE
            // after the rest its lead, as on guitar (Face-Off's).
            const auto part = [&](int lead)
            { return call.size() > lead && call.number(lead) != 0.0f ? playedPart(song->parts) : played(*song); };
            if (op == "base")
            {
                if (!song)
                    return {0u, script::kSymbol};
                return call.word(startSong(id, song, part(3)));
            }
            const uint8_t mask = song ? static_cast<uint8_t>((song->charted >> (4 * int(part(4)))) & 0xfu) : 0xfu;
            const int wanted = static_cast<int>(call.number(3));
            if (op == "has")
                return {(mask >> std::clamp(wanted, 0, 3)) & 1u, script::kInt};
            if (op == "nearest")
            {
                for (int step = 0; step < 4; ++step)
                    for (const int d : {wanted + step, wanted - step})
                        if (d >= 0 && d < 4 && (mask >> d) & 1u)
                            return {static_cast<uint32_t>(d), script::kInt};
                return {static_cast<uint32_t>(std::clamp(wanted, 0, 3)), script::kInt};
            }
            std::cerr << "[custom] usage: {custom ours|has|nearest|base|tag|length|best|art|ready|level|track <song> ...}, "
                      << "{custom instrument [guitar|bass]}, {custom sort [sort_<name>]}, "
                      << "{custom scan [<seen> <expected>]}, {custom section <n>}" << std::endl;
            return {};
        }
    }

    void installCommand()
    {
        script::addCommand("custom", customCommand);
        for (size_t n = 0; n < kMostSections; ++n)
            locale::addExpression(sectionToken(n), "{custom section " + std::to_string(n) + "}");
    }
}
