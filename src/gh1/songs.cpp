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
#include "content/locale.h"
#include "content/setlists.h"
#include "content/songs.h"
#include "formats/dtb.h"
#include "gh1/guitarist.h"
#include "formats/midi.h"

#include <cmath>
#include <iostream>
#include <map>

namespace gh2::gh1
{
    namespace
    {
        // GH2's venues standing in for GH1's, by tier.
        const std::map<std::string, std::string> kVenues = {
            {"basement", "small1"}, {"small_club", "small2"}, {"big_club", "big"},
            {"theatre", "theatre"}, {"fest", "fest"},         {"arena", "arena"},
        };

        std::string venue(const std::string &gh1)
        {
            const auto it = kVenues.find(gh1);
            return it != kVenues.end() ? it->second : "small1";
        }

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
            out.nodes.push_back(array({
                symbol("quickplay"),
                array({symbol("character_outfit"), symbol(outfit)}),
                array({symbol("guitar"), symbol(axe)}),
                array({symbol("venue"), symbol(venue(stage ? stage->text : ""))}),
            }));
            return dtb::text(out);
        }
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
        setlists::add("gh1", std::move(tiers), "ui/sel_song_quickplay.milo", scoreNames);
        std::cerr << "[gh1] " << count << " songs converted" << std::endl;
    }
}
