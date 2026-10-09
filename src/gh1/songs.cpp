// GH1's songs in GH2's forms.
//
// An entry (config/songs.dta) differs in its audio: (tracks 1) and
// (slip_tracks ((2 3))) where GH2 names the guitar's channels, (tracks
// ((guitar (2 3)))); volumes as gains where GH2 takes dB; the chart beside
// the audio rather than in it. Its quickplay names GH1's guitarist folder,
// gibson_ guitars and GH1's venues.
//
// A chart keeps GH1's gems (T1 GEMS) and fret-hand notes (ANIM) apart, and
// its EVENTS track carries every band cue as <who>_<what> text. GH2 reads
// both from PART GUITAR, and each band member's cues from its own track as
// [what] (midi_parsers.dta's echo parsers).

#include "gh1/songs.h"

#include "disc/ark.h"
#include "content/encores.h"
#include "content/locale.h"
#include "content/setlists.h"
#include "content/songs.h"
#include "formats/dtb.h"
#include "gh1/face.h"
#include "gh1/guitarist.h"
#include "formats/midi.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>

namespace gh2::gh1
{
    namespace
    {
        // GH2's venues standing in for GH1's, by tier: each the one that
        // plays GH1's crowd for it (crowd_v1 to v6, world/<venue>/streams).
        const std::map<std::string, std::string> kVenues = {
            {"basement", "battle"}, {"small_club", "small1"}, {"big_club", "big"},
            {"theatre", "theatre"}, {"fest", "fest"},         {"arena", "arena"},
        };

        // A GH1 EVENTS text as GH2's track and text, or no track to drop it.
        std::pair<std::string, std::string> cue(const std::string &gh1)
        {
            static const std::map<std::string, std::string> kWho = {
                {"gtr", "PART GUITAR"}, {"guitar", "PART GUITAR"}, {"bass", "BAND BASS"},
                {"drum", "BAND DRUMS"}, {"sing", "BAND SINGER"},   {"keys", "BAND KEYS"},
            };
            static const std::map<std::string, std::string> kWhat = {
                {"on", "[play]"},
                {"off", "[idle]"},
                {"double_tempo", "[double_tempo]"},
                {"half_tempo", "[half_tempo]"},
                {"normal_tempo", "[normal_tempo]"},
            };
            static const std::map<std::string, std::string> kDrums = {
                {"allbeat", "[allbeat]"}, {"normal", "[allbeat]"}, {"half", "[half_time]"}, {"double", "[double_time]"},
            };
            static const std::map<std::string, std::string> kCrowd = {
                {"double_tempo", "[crowd_fast_tempo]"},
                {"half_tempo", "[crowd_half_tempo]"},
                {"normal_tempo", "[crowd_normal_tempo]"},
            };
            if (gh1 == "end" || gh1 == "verse" || gh1 == "chorus" || gh1 == "solo")
                return {"EVENTS", "[" + gh1 + "]"};
            if (gh1 == "wail_on" || gh1 == "wail_off" || gh1 == "solo_on" || gh1 == "solo_off")
                return {"PART GUITAR", "[" + gh1 + "]"};
            if (gh1.rfind("StrumMap_", 0) == 0)
                return {"PART GUITAR", "[map " + gh1 + "]"};
            const size_t split = gh1.find('_');
            if (split == std::string::npos)
                return {};
            const std::string who = gh1.substr(0, split), what = gh1.substr(split + 1);
            if (who == "crowd")
            {
                const auto it = kCrowd.find(what);
                return it != kCrowd.end() ? std::pair{std::string("EVENTS"), it->second} : std::pair<std::string, std::string>{};
            }
            const auto track = kWho.find(who);
            if (track == kWho.end())
                return {};
            if (who == "drum")
                if (const auto it = kDrums.find(what); it != kDrums.end())
                    return {track->second, it->second};
            const auto it = kWhat.find(what);
            return it != kWhat.end() ? std::pair{track->second, it->second} : std::pair<std::string, std::string>{};
        }

        // A tick's time in seconds by the tempo track, 120 beats a minute
        // until set.
        double secondsAt(const midi::File &in, uint32_t tick)
        {
            double at = 0.0, perTick = 0.5 / in.division;
            uint32_t from = 0u;
            for (const midi::Event &e : in.tracks[0].events)
            {
                if (e.tick >= tick)
                    break;
                if (e.status != 0xffu || e.meta != 0x51u || e.data.size() < 3u)
                    continue;
                at += (e.tick - from) * perTick;
                from = e.tick;
                perTick = ((e.data[0] << 16) | (e.data[1] << 8) | e.data[2]) / 1.0e6 / in.division;
            }
            return at + (tick - from) * perTick;
        }

        // The tick at a time, the inverse of secondsAt.
        uint32_t tickAt(const midi::File &in, double seconds)
        {
            double at = 0.0, perTick = 0.5 / in.division;
            uint32_t from = 0u;
            for (const midi::Event &e : in.tracks[0].events)
            {
                if (e.status != 0xffu || e.meta != 0x51u || e.data.size() < 3u)
                    continue;
                if (at + (e.tick - from) * perTick >= seconds)
                    break;
                at += (e.tick - from) * perTick;
                from = e.tick;
                perTick = ((e.data[0] << 16) | (e.data[1] << 8) | e.data[2]) / 1.0e6 / in.division;
            }
            return from + static_cast<uint32_t>(std::lround(std::max(seconds - at, 0.0) / perTick));
        }

        // The tick a measure, counted from 0, starts on, by the tempo
        // track's time signatures, 4/4 until set. One of no beats, or of
        // beats too short for a tick, is none.
        uint32_t measureTick(const midi::File &in, uint32_t measure)
        {
            uint32_t at = 0u, from = 0u, length = in.division * 4u;
            for (const midi::Event &e : in.tracks[0].events)
            {
                if (e.status != 0xffu || e.meta != 0x58u || e.data.size() < 2u)
                    continue;
                const uint32_t next = e.data[1] < 32u ? in.division * 4u * e.data[0] >> e.data[1] : 0u;
                if (next == 0u)
                    continue;
                const uint32_t whole = (e.tick - from) / length;
                if (at + whole >= measure)
                    break;
                at += whole;
                from += whole * length;
                length = next;
            }
            return from + (measure - at) * length;
        }

        std::optional<midi::Bytes> chart(const midi::Bytes &gh1)
        {
            const auto in = midi::parse(gh1);
            if (!in || in->tracks.empty())
                return std::nullopt;
            const auto named = [&](const std::string &name) -> const midi::Track *
            {
                for (const midi::Track &t : in->tracks)
                    if (t.name == name)
                        return &t;
                return nullptr;
            };
            const midi::Track *gems = named("T1 GEMS"), *events = named("EVENTS");
            const midi::Track *anim = named("ANIM"), *triggers = named("TRIGGERS");
            if (!gems || !events)
                return std::nullopt;
            const auto textOf = [](const midi::Event &e) -> std::optional<std::string>
            {
                if (e.status != 0xffu || e.meta != 0x01u)
                    return std::nullopt;
                std::string s(e.data.begin(), e.data.end());
                s.erase(s.find_last_not_of(' ') + 1u);
                return s;
            };

            const char *const order[] = {"PART GUITAR", "EVENTS", "BAND BASS", "BAND DRUMS", "BAND SINGER", "BAND KEYS"};
            std::map<std::string, midi::Track> made;
            made["PART GUITAR"].events = gems->events;
            made["EVENTS"];
            if (anim)
                for (const midi::Event &e : anim->events)
                {
                    if (const auto s = textOf(e))
                    {
                        if (s->rfind("HandMap_", 0) == 0)
                            made["PART GUITAR"].events.push_back(midi::text(e.tick, "[map " + *s + "]"));
                    }
                    else if (e.status != 0xffu)
                        made["PART GUITAR"].events.push_back(e);
                }
            for (const midi::Event &e : events->events)
                if (const auto s = textOf(e))
                    if (const auto [track, text] = cue(*s); !track.empty())
                        made[track].events.push_back(midi::text(e.tick, text));
            // GH2 starts the crowd's level loops, and the world's
            // music_start, from a [music_start] text (CrowdAudio::Handle,
            // 0x1245a8); GH1's songs have none, its BeatMatch sends it at the
            // third measure (UpdateSongPos, GH1 0x10e75c). Without it the
            // crowd's intro, feedback and all, loops the whole song.
            made["EVENTS"].events.push_back(midi::text(measureTick(*in, 2u), "[music_start]"));

            // GH1's kick and bass hits are TRIGGERS notes 60 and 61
            // (config/midi_triggers.dta), GH2's note 36 of the drummer's and
            // the bassist's tracks (midi_parsers.dta's drummer_kick_drum and
            // speaker_pulse). Each fires its lead earlier, 90 and 50 ms
            // (midi_triggers.dta's third field, SongDB::AddTrigger GH1
            // 0x10bf08).
            if (triggers)
                for (midi::Event e : triggers->events)
                    if (e.status != 0xffu && (e.status & 0xe0u) == 0x80u && !e.data.empty() &&
                        (e.data[0] == 60u || e.data[0] == 61u))
                    {
                        const bool kick = e.data[0] == 60u;
                        const char *track = kick ? "BAND DRUMS" : "BAND BASS";
                        e.tick = tickAt(*in, std::max(secondsAt(*in, e.tick) - (kick ? 0.09 : 0.05), 0.0));
                        e.data[0] = 36u;
                        made[track].events.push_back(std::move(e));
                    }

            midi::File out{in->format, in->division, {in->tracks[0]}};
            for (const char *name : order)
                if (const auto it = made.find(name); it != made.end())
                {
                    it->second.name = name;
                    out.tracks.push_back(std::move(it->second));
                }
            if (triggers)
                out.tracks.push_back(*triggers);
            return midi::write(out);
        }

        // When the singer's mouth is open, in seconds: the gems track's
        // note 108 (charsys.dta's singer_events), by the tempo track.
        std::vector<std::pair<float, float>> sung(const midi::Bytes &gh1)
        {
            const auto in = midi::parse(gh1);
            std::vector<std::pair<float, float>> out;
            if (!in || in->tracks.empty())
                return out;
            const auto seconds = [&](uint32_t tick)
            {
                double at = 0.0, perTick = 0.5 / in->division; // 120 beats a minute until set
                uint32_t from = 0u;
                for (const midi::Event &e : in->tracks[0].events)
                {
                    if (e.tick >= tick)
                        break;
                    if (e.status != 0xffu || e.meta != 0x51u || e.data.size() < 3u)
                        continue;
                    at += (e.tick - from) * perTick;
                    from = e.tick;
                    perTick = ((e.data[0] << 16) | (e.data[1] << 8) | e.data[2]) / 1.0e6 / in->division;
                }
                return static_cast<float>(at + (tick - from) * perTick);
            };
            for (const midi::Track &track : in->tracks)
            {
                if (track.name != "T1 GEMS")
                    continue;
                std::optional<uint32_t> on;
                for (const midi::Event &e : track.events)
                {
                    if (e.status == 0xffu || e.data.size() < 2u || e.data[0] != 108u)
                        continue;
                    const bool down = (e.status & 0xf0u) == 0x90u && e.data[1] != 0u;
                    if (down && !on)
                        on = e.tick;
                    else if (!down && on && ((e.status & 0xf0u) == 0x80u || (e.status & 0xf0u) == 0x90u))
                    {
                        out.emplace_back(seconds(*on), seconds(e.tick));
                        on.reset();
                    }
                }
            }
            return out;
        }

        dtb::Node symbol(const std::string &text) { return {dtb::kSymbol, 0, 0.0f, text}; }
        dtb::Node array(std::vector<dtb::Node> nodes) { return {dtb::kArray, 0, 0.0f, {}, std::move(nodes)}; }

        // `key`'s value in an entry's (key value) array, or none.
        const dtb::Node *value(const dtb::Node &node, const std::string &key)
        {
            const dtb::Node *found = dtb::find(node, key);
            return found && found->nodes.size() > 1u ? &found->nodes[1] : nullptr;
        }

        std::optional<std::string> entry(const dtb::Node &gh1)
        {
            const dtb::Node *audio = dtb::find(gh1, "song");
            const dtb::Node *slip = audio ? value(*audio, "slip_tracks") : nullptr;
            const dtb::Node *gains = audio ? value(*audio, "vols") : nullptr;
            const dtb::Node *pans = audio ? dtb::find(*audio, "pans") : nullptr;
            const dtb::Node *cores = audio ? dtb::find(*audio, "cores") : nullptr;
            const dtb::Node *path = audio ? value(*audio, "name") : nullptr;
            const dtb::Node *midiFile = value(gh1, "midi_file");
            const dtb::Node *quickplay = dtb::find(gh1, "quickplay");
            if (!slip || slip->nodes.empty() || !gains || !pans || !cores || !path || !midiFile || !quickplay)
                return std::nullopt;

            dtb::Node vols = array({symbol("vols"), array({})});
            for (const dtb::Node &gain : gains->nodes)
                if (const auto g = dtb::number(gain))
                    vols.nodes[1].nodes.push_back({dtb::kFloat, 0, *g > 0.0f ? 20.0f * std::log10(*g) : -96.0f});
            dtb::Node song = array({
                symbol("song"),
                array({symbol("name"), *path}),
                array({symbol("tracks"), array({array({symbol("guitar"), slip->nodes[0]})})}),
                *pans,
                vols,
                *cores,
                array({symbol("midi_file"), *midiFile}),
            });

            const dtb::Node *character = value(*quickplay, "character");
            const dtb::Node *guitar = value(*quickplay, "guitar");
            const dtb::Node *stage = value(*quickplay, "venue");
            std::string outfit = "punk1";
            for (const Guitarist &g : kGuitarists)
                if (character && character->text == g.folder)
                    outfit = g.name();
            std::string axe = guitar ? guitar->text : "lespaul";
            if (axe.rfind("gibson_", 0) == 0)
                axe = axe.substr(7);

            dtb::Node out = array({gh1.nodes[0]});
            for (const char *key : {"name", "artist"})
                if (const dtb::Node *n = dtb::find(gh1, key))
                    out.nodes.push_back(*n);
            out.nodes.push_back(song);
            for (const char *key : {"anim_tempo", "preview", "bpm"})
                if (const dtb::Node *n = dtb::find(gh1, key))
                    out.nodes.push_back(*n);
            // A band is GH1's archetypes (band_chars.dta), each in the folder
            // GH2 names the same character by: charsys/metal_bass.
            if (const dtb::Node *band = dtb::find(gh1, "band"))
            {
                dtb::Node members = array({symbol("band")});
                for (size_t i = 1u; i < band->nodes.size(); ++i)
                {
                    const dtb::Node *model = dtb::find(band->nodes[i], "outfit");
                    if (const dtb::Node *folder = model ? value(*model, "directory") : nullptr)
                        members.nodes.push_back(symbol(folder->text.substr(folder->text.rfind('/') + 1u)));
                }
                out.nodes.push_back(std::move(members));
            }
            out.nodes.push_back(array({
                symbol("quickplay"),
                array({symbol("character_outfit"), symbol(outfit)}),
                array({symbol("guitar"), symbol(axe)}),
                array({symbol("venue"), symbol(venue(stage ? stage->text : ""))}),
            }));
            return dtb::text(out);
        }
    }

    std::string venue(const std::string &gh1)
    {
        const auto it = kVenues.find(gh1);
        return it != kVenues.end() ? it->second : "small1";
    }

    void addSetlist(size_t disc, size_t gh2Disc)
    {
        const dtb::Files files = [disc](const std::string &path) { return ark::readFile(disc, path); };
        dtb::Macros macros;
        const auto entries = dtb::read("config/songs.dta", macros, files);
        const auto campaign = dtb::read("config/campaign.dta", macros, files);
        const auto store = dtb::read("config/store.dta", macros, files);
        const auto strings = dtb::read("ghui/eng/locale.dta", macros, files);
        const dtb::Node *order = campaign ? dtb::find(*campaign, "order") : nullptr;
        const dtb::Node *bonus = store ? dtb::find(*store, "song") : nullptr;
        if (!entries || !order || !bonus || !strings)
        {
            std::cerr << "[gh1] cannot read GH1's songs" << std::endl;
            return;
        }

        // The basement's results headlines, by stars (gh1/menus.dta).
        for (int stars = 3; stars <= 5; ++stars)
            if (const dtb::Node *line = value(*strings, "headline_basement_star" + std::to_string(stars)))
                locale::add("headline_basement_star" + std::to_string(stars), line->text);

        // The career's tiers, then the store's songs as GH1 heads them.
        std::vector<std::pair<std::string, const dtb::Node *>> groups;
        for (size_t i = 1u; i < order->nodes.size(); ++i)
            if (!order->nodes[i].nodes.empty())
                groups.emplace_back(order->nodes[i].nodes[0].text, &order->nodes[i]);
        groups.emplace_back("store", bonus);

        std::vector<setlists::Tier> tiers;
        size_t count = 0u;
        for (const auto &[group, list] : groups)
        {
            setlists::Tier tier{group == "store" ? venue(groups.front().first) : venue(group), "gh1_" + group, {},
                                group != "store"};
            if (const dtb::Node *header = value(*strings, "song_header_" + group))
                locale::add("song_header_" + tier.header, header->text);
            for (size_t s = 1u; s < list->nodes.size(); ++s)
            {
                const dtb::Node &item = list->nodes[s];
                const std::string name = item.type == dtb::kArray && !item.nodes.empty() ? item.nodes[0].text : item.text;
                const dtb::Node *gh1 = dtb::find(*entries, name);
                const auto text = gh1 ? entry(*gh1) : std::nullopt;
                const std::string mid = "songs/" + name + "/" + name + ".mid";
                const auto file = ark::readFile(disc, mid);
                const auto converted = file ? chart(*file) : std::nullopt;
                if (!text || !converted)
                {
                    std::cerr << "[gh1] cannot convert " << name << std::endl;
                    continue;
                }
                songs::add(*text);
                ark::addFile(mid, *converted);
                addSinging(name, sung(*file));
                // GH2's singer lip-syncs to <song>.voc; GH1's sang to chart
                // events, so GH2's neutral track stands in.
                ark::rename("songs/" + name + "/" + name + ".voc", gh2Disc, "songs/_blinktrack/_blinktrack.voc");
                ark::rename("songs/" + name + "/", disc, "songs/" + name + "/");
                tier.songs.push_back(name);
                ++count;
            }
            tiers.push_back(std::move(tier));
        }
        std::array<std::string, 5> scoreNames;
        for (size_t i = 0; i < scoreNames.size(); ++i)
            if (const dtb::Node *name = value(*strings, "highscore_dummy_" + std::to_string(i)))
                scoreNames[i] = name->text;
        // In its own setlist scene, as GH2's (gh1/menus.h).
        setlists::add("gh1", std::move(tiers), "gh1/ui/sel_song_quickplay.milo", scoreNames,
                      setlists::required(*campaign));
        // Its campaign.dta asks a venue only for a count of songs.
        encores::none("gh1");
        std::cerr << "[gh1] " << count << " songs converted" << std::endl;
    }
}
