// Custom songs' charts read on the host: a small .chart and .mid of each
// note type, checked; then, given folders, every song.ini's chart under
// them read and tallied. Exits 1 on any failed check.
//
//   custom_chart_test [folder ...]

#include "custom/chart.h"
#include "custom/gh2_chart.h"
#include "custom/song_ini.h"
#include "formats/md5.h"
#include "formats/midi.h"
#include "formats/sng.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using namespace gh2;
    using namespace gh2::custom;
    namespace fs = std::filesystem;
    namespace midi = gh2::midi;

    int s_failed = 0;

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
            ++s_failed; \
        } \
    } while (0)

    const Gem *at(const Difficulty &d, uint32_t tick)
    {
        for (const Gem &g : d.gems)
            if (g.tick == tick)
                return &g;
        return nullptr;
    }

    void chartTypes()
    {
        const std::string text = "[Song]\n{\n  Resolution = 192\n  Offset = 0.5\n}\n"
                                 "[SyncTrack]\n{\n  0 = TS 4\n  0 = B 120000\n  768 = B 240000\n}\n"
                                 "[Events]\n{\n  0 = E \"section Intro\"\n}\n"
                                 "[ExpertSingle]\n{\n"
                                 "  0 = N 0 0\n"             // green
                                 "  48 = N 1 0\n"            // red
                                 "  96 = N 1 0\n"            // red again
                                 "  144 = N 2 0\n  144 = N 5 0\n" // yellow, flipped: the flag left out
                                 "  384 = N 3 0\n"           // blue
                                 "  432 = N 4 0\n  432 = N 6 0\n" // orange, tapped: the flag left out
                                 "  480 = N 7 96\n"          // open: no gem
                                 "  768 = N 0 384\n  768 = N 2 96\n" // chord, held to its longest lane
                                 "  768 = S 2 384\n"
                                 "  900 = E solo\n  1000 = E soloend\n"
                                 "}\n";
        std::string why;
        const auto chart = parseChart(text, {}, why);
        CHECK(chart);
        if (!chart)
            return;
        CHECK(chart->resolution == 192u);
        CHECK(chart->offset == 0.5);
        CHECK(chart->tempos.size() == 2u && chart->tempos[1].usPerBeat == 250000u);
        CHECK(chart->seconds(768u) == 2.0);
        CHECK(chart->seconds(960u) == 2.25);
        CHECK(chart->sections.size() == 1u && chart->sections[0].second == "Intro");
        CHECK(chart->parts.count(Part::Guitar));
        const Difficulty &x = chart->parts.at(Part::Guitar)[3];
        CHECK(x.gems.size() == 7u);
        CHECK(at(x, 144) && at(x, 144)->lanes == 1u << 2);
        CHECK(at(x, 432) && at(x, 432)->lanes == 1u << 4);
        CHECK(!at(x, 480));
        const Gem *chord = at(x, 768);
        CHECK(chord && chord->chord() && chord->length == 384u);
        CHECK(x.starPower.size() == 1u && x.starPower[0].length == 384u);
        CHECK(x.solos.size() == 1u && x.solos[0].tick == 900u && x.solos[0].length == 100u);

        // A tempo listed out of order put in its place, no 120 put before it.
        const auto unsorted = parseChart("[SyncTrack]\n{\n  768 = B 240000\n  0 = B 60000\n}\n", {}, why);
        CHECK(unsorted && unsorted->tempos.size() == 2u && unsorted->tempos[0].usPerBeat == 1000000u);

        // As GH2 takes it: 480 a beat, the open gone, star power on 103.
        const auto gh2 = toGh2(*chart, 0.0);
        CHECK(gh2);
        if (!gh2)
            return;
        const auto back = gh2::midi::parse(gh2->mid);
        CHECK(back && back->division == 480u && back->tracks.size() == 3u);
        if (!back || back->tracks.size() != 3u)
            return;
        std::map<uint8_t, std::vector<uint32_t>> ons;
        for (const midi::Event &e : back->tracks[1].events)
            if (midi::kind(e) == midi::kNoteOn)
                ons[e.data[0]].push_back(e.tick);
        CHECK(ons[96] == (std::vector<uint32_t>{0u, 1920u}));
        CHECK(ons[97] == (std::vector<uint32_t>{120u, 240u}));
        CHECK(ons[103] == (std::vector<uint32_t>{1920u}));
        CHECK(ons.size() == 6u); // five frets and star power: no open
        bool end = false, section = false;
        for (const midi::Event &e : back->tracks[2].events)
        {
            const std::string text(e.data.begin(), e.data.end());
            end |= e.status == midi::kMeta && text == "[end]";
            section |= e.status == midi::kMeta && text == "[section custom_section_0]";
        }
        CHECK(end && section);
    }

    using midi::note;

    void midTypes()
    {
        midi::File file{1u, 480u, {}};
        midi::Track tempo;
        tempo.events.push_back({0u, midi::kMeta, midi::kTempo, {0x07u, 0xa1u, 0x20u}});
        midi::Track guitar;
        guitar.name = "PART GUITAR";
        guitar.events.push_back(midi::text(0u, "[ENHANCED_OPENS]"));
        const auto hold = [&](uint32_t tick, uint8_t pitch, uint32_t length)
        {
            guitar.events.push_back(note(tick, pitch, true));
            guitar.events.push_back(note(tick + length, pitch, false));
        };
        // Forced strums (102) and HOPOs (101), taps (104) and opens (95, and
        // Phase Shift's at 2880) left out, the frets under them kept.
        hold(0u, 96u, 60u);     // green, too short to sustain
        hold(120u, 97u, 60u);   // red
        hold(240u, 98u, 60u);   // yellow, forced strum
        hold(240u, 102u, 60u);
        hold(960u, 99u, 60u);   // blue, forced HOPO
        hold(960u, 101u, 60u);
        hold(1200u, 100u, 60u); // orange tap
        hold(1200u, 104u, 60u);
        hold(1440u, 95u, 480u); // open
        hold(1920u, 96u, 960u); // a chord held to its longest lane
        hold(1920u, 98u, 480u);
        hold(1920u, 116u, 960u);
        hold(1920u, 103u, 240u);
        // A Phase Shift open, all difficulties.
        guitar.events.push_back({2880u, midi::kSysex, 0u, {'P', 'S', 0u, 0u, 0xffu, 1u, 1u, midi::kSysexEnd}});
        hold(2880u, 96u, 60u);
        guitar.events.push_back({2940u, midi::kSysex, 0u, {'P', 'S', 0u, 0u, 0xffu, 1u, 0u, midi::kSysexEnd}});
        std::stable_sort(guitar.events.begin(), guitar.events.end(),
                         [](const midi::Event &a, const midi::Event &b) { return a.tick < b.tick; });
        midi::Track events;
        events.name = "EVENTS";
        events.events.push_back(midi::text(0u, "[section verse_1]"));
        events.events.push_back(midi::text(960u, "[prc_chorus_1]"));
        file.tracks = {tempo, guitar, events};
        std::string why;
        const auto bytes = midi::write(file);
        const auto chart = parseMid(bytes, {}, why);
        CHECK(chart);
        if (!chart)
            return;
        // Told by its name, whatever its case.
        CHECK(parseSongChart("Notes.MID", std::string(bytes.begin(), bytes.end()), {}, why));
        CHECK(chart->resolution == 480u);
        CHECK(chart->tempos.size() == 1u && chart->tempos[0].usPerBeat == 500000u);
        CHECK(chart->sections.size() == 2u && chart->sections[1].second == "chorus_1");
        CHECK(chart->parts.count(Part::Guitar));
        const Difficulty &x = chart->parts.at(Part::Guitar)[3];
        CHECK(x.gems.size() == 6u);
        CHECK(at(x, 0) && at(x, 0)->length == 0u);
        CHECK(at(x, 240) && at(x, 240)->lanes == 1u << 2);
        CHECK(at(x, 960) && at(x, 960)->lanes == 1u << 3);
        CHECK(at(x, 1200) && at(x, 1200)->lanes == 1u << 4);
        CHECK(!at(x, 1440) && !at(x, 2880));
        CHECK(at(x, 1920) && at(x, 1920)->lanes == 0x5u && at(x, 1920)->length == 960u);
        CHECK(x.starPower.size() == 1u && x.solos.size() == 1u);
    }

    // Charts as written in the wild: a note struck again before the last's
    // end on its tick, no ticks a beat, tempos out of MIDI's reach,
    // negative and wild lengths, a mark and a space before the first
    // section, sections out of order. Each still makes a .mid that reads.
    void chartEdges()
    {
        midi::File file{1u, 480u, {}};
        midi::Track tempo;
        tempo.events.push_back({0u, midi::kMeta, midi::kTempo, {0u, 0u, 0u}});
        midi::Track guitar;
        guitar.name = "PART GUITAR";
        guitar.events = {note(0u, 96u, true), note(480u, 96u, true), note(480u, 96u, false), note(1440u, 96u, false)};
        file.tracks = {tempo, guitar};
        std::string why;
        auto mid = parseMid(midi::write(file), {}, why);
        CHECK(mid && mid->tempos.size() == 1u && mid->tempos[0].usPerBeat == 500000u);
        const Difficulty *x = mid && mid->parts.count(Part::Guitar) ? &mid->parts.at(Part::Guitar)[3] : nullptr;
        CHECK(x && at(*x, 0) && at(*x, 0)->length == 480u && at(*x, 480) && at(*x, 480)->length == 960u);
        file.division = 0u;
        CHECK(!parseMid(midi::write(file), {}, why));

        const std::string text = "\xef\xbb\xbf [Song]\n{\n  Resolution = 192\n}\n"
                                 "[SyncTrack]\n{\n  0 = B 2000\n  0 = TS 0\n}\n"
                                 "[Events]\n{\n  500 = E \"section B\"\n  0 = E \"section A\"\n}\n"
                                 "[ExpertSingle]\n{\n  0 = N 0 -1\n  192 = N 1 4000000000\n}\n";
        const auto chart = parseChart(text, {}, why);
        CHECK(chart && chart->resolution == 192u && chart->signatures.empty());
        if (!chart)
            return;
        CHECK(chart->tempos.size() == 1u && chart->tempos[0].usPerBeat == 0xffffffu);
        CHECK(chart->sections.size() == 2u && chart->sections[0].second == "A");
        const Difficulty &c = chart->parts.at(Part::Guitar)[3];
        CHECK(at(c, 0) && at(c, 0)->length == 0u && at(c, 192) && at(c, 192)->length > 0u);
        const auto gh2 = toGh2(*chart, 60.0);
        CHECK(gh2 && midi::parse(gh2->mid));
    }

    // `files` written as an .sng, as SngFileFormat's packer does: song.ini's
    // tags as its metadata, each other file after the index, masked.
    void pack(const fs::path &out, const Tags &tags, const std::map<std::string, std::string> &files)
    {
        std::string b = "SNGPKG", meta;
        // `v` little-endian, `bytes` long, onto `to` (b unless named).
        const auto u = [&](uint64_t v, int bytes, std::string *to = nullptr)
        {
            for (int i = 0; i < bytes; ++i)
                (to ? *to : b) += static_cast<char>((v >> (8 * i)) & 0xffu);
        };
        const uint8_t mask[16] = {0x13, 0x37, 0xa5, 0x5a, 0x01, 0xfe, 0x80, 0x7f, 9, 8, 7, 6, 5, 4, 3, 2};
        u(1, 4);
        b.append(reinterpret_cast<const char *>(mask), 16);
        for (const auto &[key, value] : tags)
            for (const std::string *s : {&key, &value})
            {
                u(s->size(), 4, &meta);
                meta += *s;
            }
        u(meta.size() + 8u, 8);
        u(tags.size(), 8);
        b += meta;
        size_t index = 8u;
        for (const auto &[name, contents] : files)
            index += 17u + name.size();
        u(index, 8);
        u(files.size(), 8);
        // The data's start: past the index and the data's own length.
        uint64_t at = b.size() + (index - 8u) + 8u;
        std::string data;
        for (const auto &[name, contents] : files)
        {
            b += static_cast<char>(name.size());
            b += name;
            u(contents.size(), 8);
            u(at, 8);
            at += contents.size();
            for (size_t i = 0; i < contents.size(); ++i)
                data += static_cast<char>(contents[i] ^ mask[i & 15u] ^ static_cast<uint8_t>(i & 0xffu));
        }
        u(data.size(), 8);
        b += data;
        std::ofstream(out, std::ios::binary) << b;
    }

    // A song as a folder and as an .sng read the same, and its files come
    // out of the package as they went in.
    void package()
    {
        const fs::path root = fs::temp_directory_path() / "ghc_sng_test";
        fs::remove_all(root);
        fs::create_directories(root / "Folder");
        const std::string ini = "[song]\nname = Packed\nartist = Someone\ndelay = 0\n";
        const std::string chart = "[Song]\n{\n  Resolution = 192\n}\n[SyncTrack]\n{\n  0 = B 120000\n}\n"
                                  "[ExpertSingle]\n{\n  0 = N 0 0\n  192 = N 1 0\n  384 = N 7 0\n}\n";
        // An Ogg page that names Vorbis, long enough to cross the mask's
        // 16 and 256.
        std::string ogg = kVorbisHead;
        for (int i = 0; ogg.size() < 700u; ++i)
            ogg += static_cast<char>(i * 31);
        std::ofstream(root / "Folder" / "song.ini") << ini;
        std::ofstream(root / "Folder" / "notes.chart", std::ios::binary) << chart;
        std::ofstream(root / "Folder" / "song.ogg", std::ios::binary) << ogg;
        pack(root / "Packed.sng", parseIni(ini), {{"notes.chart", chart}, {"song.ogg", ogg}});

        const Song folder = read(root / "Folder", "notes.chart"), packed = read(root / "Packed.sng", "");
        CHECK(folder.why.empty() && packed.why.empty());
        CHECK(packed.tags == folder.tags && packed.stems == folder.stems && packed.parts == folder.parts);
        CHECK(packed.chart == "notes.chart" && packed.chartMd5 == folder.chartMd5 && packed.chartMd5 == md5::hex(chart));
        CHECK(songFile(root / "Packed.sng" / "song.ogg") == ogg && songFile(root / "Folder" / "song.ogg") == ogg);
        CHECK(title(packed) == "Packed");
        const std::vector<Song> scanned = scan({root}, {});
        CHECK(scanned.size() == 2u && copies(scanned).size() == 1u);
        fs::remove_all(root);
    }

    // RFC 1321's own examples, and one past a block's padding.
    void md5Vectors()
    {
        CHECK(md5::hex("") == "d41d8cd98f00b204e9800998ecf8427e");
        CHECK(md5::hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
        CHECK(md5::hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
        CHECK(md5::hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
              "57edf4a22be3c955ac49da2e2107b67a");
    }

    // A folder as chart sites spell it, its files in any case; an .sng that
    // claims a file past its end is none.
    void spelled()
    {
        const fs::path root = fs::temp_directory_path() / "ghc_spelled_test";
        fs::remove_all(root);
        fs::create_directories(root / "Song");
        std::ofstream(root / "Song" / "Song.ini") << "[Song]\nName = Spelled\n";
        std::ofstream(root / "Song" / "NOTES.chart") << kOneGem;
        std::ofstream(root / "Song" / "Guitar.ogg", std::ios::binary) << kVorbisHead;
        std::ofstream(root / "Song" / "Album.PNG") << "png";
        const std::vector<Song> scanned = scan({root}, {});
        CHECK(scanned.size() == 1u);
        if (scanned.size() == 1u)
        {
            const Song &s = scanned[0];
            CHECK(s.why.empty() && s.chart == "NOTES.chart" && tag(s.tags, "name") == "Spelled" && s.iniTime != 0);
            CHECK(s.stems == std::vector<std::string>{"Guitar.ogg"} && s.art == "Album.PNG");
            CHECK(songFile(songPath(s, s.art)) == "png");
        }
        pack(root / "Cut.sng", {}, {{"notes.chart", "[Song]\n{\n}\n"}});
        fs::resize_file(root / "Cut.sng", fs::file_size(root / "Cut.sng") - 4u);
        std::string why;
        CHECK(!sng::read(root / "Cut.sng", why) && !why.empty());
        fs::remove_all(root);
    }

    // .chart's Offset as song.ini's delay where it has none, never over it.
    void offsetLead()
    {
        const fs::path root = fs::temp_directory_path() / "ghc_offset_test";
        fs::remove_all(root);
        const std::string chart = "[Song]\n{\n  Resolution = 192\n  Offset = 0.25\n}\n[SyncTrack]\n{\n  0 = B 120000\n}\n"
                                  "[ExpertSingle]\n{\n  0 = N 0 0\n}\n";
        for (const auto &[name, ini] : {std::pair{"None", "[song]\nname = A\n"}, std::pair{"Zero", "[song]\ndelay = 0\n"},
                                        std::pair{"Own", "[song]\ndelay = 100\n"}})
        {
            fs::create_directories(root / name);
            std::ofstream(root / name / "song.ini") << ini;
            std::ofstream(root / name / "notes.chart") << chart;
        }
        CHECK(number(read(root / "None", "notes.chart").tags, "delay") == 250);
        CHECK(number(read(root / "Zero", "notes.chart").tags, "delay") == 250);
        CHECK(number(read(root / "Own", "notes.chart").tags, "delay") == 100);
        fs::remove_all(root);
    }

}

int main(int argc, char **argv)
{
    chartTypes();
    midTypes();
    chartEdges();
    md5Vectors();
    package();
    spelled();
    offsetLead();
    if (s_failed)
        std::cerr << s_failed << " failed" << std::endl;
    else
        std::cout << "custom_chart_test: all passed" << std::endl;
    return s_failed ? 1 : 0;
}
