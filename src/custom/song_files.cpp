#include "custom/song_files.h"

#include "content/games.h"
#include "content/setlist_printout.h"
#include "custom/audio.h"
#include "custom/gh2_chart.h"
#include "custom/songs.h"
#include "custom/text.h"
#include "formats/dtb.h"
#include "render/texture_store.h"
#include "script.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>

namespace gh2::custom
{
    namespace
    {
        namespace fs = std::filesystem;

        // Each song's files, songs/<id>/<id>.mid and the rest, made as the
        // game reads them.
        constexpr const char *kFiles = "songs/cs_";

        // ui/image/ghc_art_<id>.png, as the script sets it on a Tex: a
        // stand-in for the song's album art, kept on the host
        // (render/texture_store.h). None for a song without.
        constexpr const char *kArtFiles = "ui/image/gen/ghc_art_";

        // Songs in the game's config at once: the list's window and those
        // listed last, so scrolling back and forth makes few again. Each is
        // a kilobyte or so of the game's heap.
        constexpr size_t kMostEntries = 256u;

        // The song last started, whose sections the locale names.
        std::shared_ptr<const Song> s_playing;
        std::string s_playingId;

        // Each song in the config, by when it was last listed.
        std::map<std::string, uint64_t> s_entries;
        uint64_t s_listings = 0u;

        std::string entry(const Song &song, const std::string &id)
        {
            using dtb::array;
            using dtb::integer;
            using dtb::real;
            using dtb::symbol;
            const std::string dir = "songs/" + id + "/" + id;
            dtb::Node out = array({
                symbol(id),
                array({symbol("name"), dtb::string(title(song))}),
                array({symbol("artist"), dtb::string(plain(tag(song.tags, "artist").value_or("")))}),
                array({
                    symbol("song"),
                    array({symbol("name"), symbol(dir)}),
                    array({symbol("tracks"), array({array({symbol("guitar"), array({integer(2), integer(3)})})})}),
                    array({symbol("pans"), array({real(-1.0f), real(1.0f), real(-1.0f), real(1.0f)})}),
                    array({symbol("vols"), array({real(0.0f), real(0.0f), real(0.0f), real(0.0f)})}),
                    array({symbol("cores"), array({integer(-1), integer(-1), integer(1), integer(1)})}),
                    array({symbol("midi_file"), symbol(dir + ".mid")}),
                }),
                array({symbol("bank"), symbol("sfx/song_default")}),
                array({symbol("anim_tempo"), symbol("kTempoMedium")}),
            });
            // song.ini's vocal_gender, as Rock Band's: GH2's female singer
            // in place of its default band's (gh2.dta's default_band).
            if (lower(tag(song.tags, "vocal_gender").value_or("")) == "female")
                out.nodes.push_back(
                    array({symbol("band"), symbol("metal_bass"), symbol("metal_drummer"), symbol("female_singer")}));
            // Fifteen seconds from song.ini's start, 30 s in where it has
            // none, kept inside the song: GH2 waits forever on audio past
            // the end.
            const int length = static_cast<int>(song.length * 1000.0);
            const int preview = std::clamp(static_cast<int>(number(song.tags, "preview_start_time").value_or(30000)), 0,
                                           std::max(length - 15000, 0));
            out.nodes.push_back(array({symbol("preview"), integer(preview), integer(std::min(preview + 15000, length))}));
            out.nodes.push_back(array({
                symbol("quickplay"),
                array({symbol("character_outfit"), symbol("punk1")}),
                array({symbol("guitar"), symbol("lespaul")}),
                array({symbol("venue"), symbol("small1")}),
            }));
            return dtb::text(out);
        }

        // Its stem's name.
        std::string playedStem(Part part)
        {
            return part == Part::Guitar || part == Part::Coop ? "guitar" : part == Part::Rhythm ? "rhythm" : "bass";
        }

        // The chart in GH2's form, made from the song's folder, PART GUITAR
        // that part.
        std::optional<ark::Made> chartOf(const Song &song, Part part)
        {
            std::string why;
            const auto chart = parseSongChart(song.chart, songFile(songPath(song, song.chart)), options(song.tags), why);
            const double length = number(song.tags, "song_length").value_or(0) / 1000.0;
            const auto gh2 = chart ? toGh2(*chart, length, part) : std::nullopt;
            if (!gh2)
            {
                std::cerr << "[custom] " << utf8(song.dir) << ": chart no longer reads" << (why.empty() ? "" : ": " + why)
                          << std::endl;
                return std::nullopt;
            }
            return ark::held(gh2->mid);
        }

        // songs/<id>/<id>[_bass].<mid|vgs|voc>: its chart and audio for its
        // own part, or for the bass where it has one besides; no lip sync
        // of its own, GH2's neutral track, as GH1's songs have.
        std::optional<ark::Made> fileOf(const std::string &path)
        {
            const size_t slash = path.find('/', 6u), dot = path.rfind('.');
            if (slash == std::string::npos || dot == std::string::npos || dot < slash)
                return std::nullopt;
            const std::string id = path.substr(6u, slash - 6u);
            const auto song = catalog().find(id);
            if (!song)
                return std::nullopt;
            const std::string name = path.substr(slash + 1u, dot - slash - 1u), ext = path.substr(dot);
            const bool bass = name == id + "_bass";
            if (!bass && name != id)
                return std::nullopt;
            if (bass && !hasPart(song->parts, Part::Bass))
                return std::nullopt;
            const Part part = bass ? Part::Bass : playedPart(song->parts);
            if (ext == ".mid")
                return chartOf(*song, part);
            if (ext == ".vgs")
                return madeMix(path, *song, playedStem(part));
            if (ext == ".voc")
            {
                static const auto s_voc =
                    ark::readFile(games::disc("gh2").value_or(0u), "songs/_blinktrack/_blinktrack.voc");
                return s_voc ? std::optional(ark::held(*s_voc)) : std::nullopt;
            }
            return std::nullopt;
        }

        std::optional<ark::Made> artFile(const std::string &path)
        {
            const std::string suffix = ".png_ps2";
            const size_t prefix = std::char_traits<char>::length(kArtFiles);
            if (path.size() <= prefix + suffix.size() || path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0)
                return std::nullopt;
            const auto song = catalog().find(path.substr(prefix, path.size() - prefix - suffix.size()));
            if (!song || song->art.empty())
                return std::nullopt;
            const fs::path art = songPath(*song, song->art);
            std::vector<uint8_t> standIn = textures::standIn(
                textures::add({.path = utf8(art), .file = [art] { return songFile(art); }}), setlist_printout::kArtStandIn, setlist_printout::kArtStandIn);
            return standIn.empty() ? std::nullopt : std::optional(ark::held(std::move(standIn)));
        }
    }

    void readyEntries(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::vector<std::string> &ids)
    {
        ++s_listings;
        std::string text;
        for (const std::string &id : ids)
        {
            if (!s_entries.count(id))
            {
                const auto song = catalog().find(id);
                if (!song)
                    continue;
                text += "{push_back {find $syscfg songs} " + entry(*song, id) + "}\n";
            }
            s_entries[id] = s_listings;
        }
        if (s_entries.size() > kMostEntries)
        {
            std::vector<std::pair<uint64_t, std::string>> byAge;
            for (const auto &[id, when] : s_entries)
                if (when != s_listings && id != s_playingId)
                    byAge.emplace_back(when, id);
            std::sort(byAge.begin(), byAge.end());
            for (size_t i = 0; i < byAge.size() && s_entries.size() > kMostEntries; ++i)
            {
                const std::string &id = byAge[i].second;
                text += "{remove_elem {find $syscfg songs} {find $syscfg songs " + id + "}}\n";
                s_entries.erase(id);
            }
        }
        if (!text.empty())
            script::run(rdram, ctx, runtime, text);
    }

    std::string startSong(const std::string &id, std::shared_ptr<const Song> song)
    {
        s_playing = std::move(song);
        s_playingId = id;
        return base(id, s_playing->parts, played(*s_playing));
    }

    std::shared_ptr<const Song> playingSong() { return s_playing; }

    void installSongFiles()
    {
        ark::addMaker(kFiles, fileOf);
        ark::addMaker(kArtFiles, artFile);
    }
}
