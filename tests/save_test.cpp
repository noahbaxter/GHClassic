// Round trips through the save code, with no disc and no game: the store's
// text and file, GH2's stream and its store sections, and a PCSX2 card.
// tools/ghc.py ci builds and runs it; it exits 1 on any failed check.

#include "content/sources.h"
#include "save/best.h"
#include "save/gh2_stream.h"
#include "save/gh2_text.h"
#include "save/ps2_card.h"
#include "save/store.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace
{
    using gh2::save::Best;
    using gh2::save::bestKey;
    using gh2::save::bestText;
    using gh2::save::better;
    using gh2::save::kBestSection;
    using gh2::save::parseBest;
    using gh2::save::Ps2Card;
    using gh2::save::readFile;
    using gh2::save::ReadResult;
    using gh2::save::Store;
    using gh2::save::writeFile;
    namespace codec = gh2::save::gh2;
    namespace fs = std::filesystem;

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

    std::vector<uint8_t> bytes(size_t size, uint8_t seed)
    {
        std::vector<uint8_t> out(size);
        for (size_t i = 0; i < size; ++i)
            out[i] = static_cast<uint8_t>(seed + i * 31u + (i >> 8));
        return out;
    }

    void writeBytes(const fs::path &path, const std::vector<uint8_t> &data)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    }

    std::vector<uint8_t> readBytes(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }

    void storeText()
    {
        Store store;
        store.section("band 1").set("name", "The Testers");
        store.section("gh2 scores expert").set("freebird.1", "123456 AAA");
        // A section no game owns, as a mod would keep.
        store.section("mod colours").set("highway", "ff00ff");
        const Store back = Store::parse(store.text());
        CHECK(back.text() == store.text());
        CHECK(back.find("mod colours") && *back.find("mod colours")->get("highway") == "ff00ff");
        CHECK(!back.find("band 2"));
        // A key twice, or a section met again, as one, the last value kept.
        const Store twice = Store::parse("[a]\nk = 1\n[b]\nj = 2\n[a]\nk = 3\nl = 4\n");
        CHECK(twice.text() == "[a]\nk = 3\nl = 4\n\n[b]\nj = 2\n");
    }

    // [best]'s lines: read back as written, the higher scoring run kept
    // whole with a full combo from either, and a line too short none.
    void bests()
    {
        const Best run{123456u, 5u, 98u, 412u, false}, other{99000u, 4u, 100u, 500u, true};
        const auto back = parseBest(bestText(run));
        CHECK(back && back->score == 123456u && back->stars == 5u && back->percent == 98u && back->streak == 412u &&
              !back->fullCombo);
        for (const Best &best : {better(run, other), better(other, run)})
            CHECK(best.score == 123456u && best.stars == 5u && best.percent == 98u && best.streak == 412u &&
                  best.fullCombo);
        CHECK(!parseBest("123 4"));
        CHECK(bestKey("gh2.freebird", "gh1", "guitar", "expert", "gamepad") == "gh2.freebird.gh1.guitar.expert.gamepad");
        // A save with them keeps them as any section: one build's [best] goes
        // through another's untouched.
        Store store;
        store.section(kBestSection).set("gh2.freebird.gh2.guitar.expert.guitar", bestText(run));
        CHECK(Store::parse(store.text()).text() == store.text());
    }

    void storeFile(const fs::path &dir)
    {
        const std::string path = (dir / "save.bin").string();
        Store out;
        CHECK(readFile(path, out) == ReadResult::kMissing);

        Store store;
        store.section("gh2").set("default_name", "ROCKER");
        store.section("band 3 gh2 hard").set("guitar", "flyingv");
        CHECK(writeFile(path, store));
        CHECK(readFile(path, out) == ReadResult::kOk);
        CHECK(out.text() == store.text());

        // A second write replaces the first whole.
        store.section("gh2").set("default_name", "SHREDDER");
        CHECK(writeFile(path, store));
        CHECK(readFile(path, out) == ReadResult::kOk && *out.find("gh2")->get("default_name") == "SHREDDER");

        // Cut short, then not a save at all.
        std::vector<uint8_t> raw = readBytes(path);
        raw.resize(raw.size() / 2);
        writeBytes(path, raw);
        CHECK(readFile(path, out) == ReadResult::kCorrupt);
        writeBytes(path, bytes(64, 7));
        CHECK(readFile(path, out) == ReadResult::kCorrupt);
    }

    // A campaign as the game builds one at boot: every band slot empty.
    codec::Save freshCampaign()
    {
        codec::Save save;
        save.version = 13;
        save.options[0] = 4; // OptionData's version
        save.profileItems = {
            {"battle", 0, std::vector<codec::ItemState>(codec::kProfiles)},
            {"sg", 3, std::vector<codec::ItemState>(codec::kProfiles)},
        };
        save.campaignItems = {
            {"shoutatthedevil", codec::kSongType, std::vector<codec::ItemState>(codec::kProfiles * codec::kDifficulties)},
            {"freebird", codec::kSongType, std::vector<codec::ItemState>(codec::kProfiles * codec::kDifficulties)},
        };
        save.scoreVersion = 1;
        for (const char *song : {"shoutatthedevil", "freebird"})
        {
            codec::SongScores &scores = save.scores.emplace_back();
            scores.song = song;
            for (auto &list : scores.lists)
                for (int rank = 0; rank < codec::kScoresPerList; ++rank)
                    list[rank] = {"HMX", 50000 - 10000 * rank};
        }
        save.defaultName = "AAA";
        save.coop.resize(2);
        return save;
    }

    codec::Save playedCampaign()
    {
        codec::Save save = freshCampaign();
        codec::Profile &band = save.profiles[2];
        band.empty = 0;
        band.name = "The Testers";
        band.tutorials = 3;
        band.difficulty = 2;
        band.cash = 1250;
        band.look[2] = {"metal", "metal_alt", "flyingv", "flames"};
        save.profileItems[1].states[2].flags = 0x3;              // bought and unlocked
        save.campaignItems[1].states[2 * codec::kDifficulties + 2] = {0x6, 187654, 5 | 0x80, 1};
        save.scores[1].lists[3][0] = {"TESTER", 187654};
        save.defaultName = "TESTER";
        save.careerBeaten = 1;
        save.coop[1] = {0x4, 99000, 4, 0};
        return save;
    }

    void gh2Stream()
    {
        const codec::Save save = playedCampaign();
        const std::vector<uint8_t> stream = codec::write(save);
        const std::optional<codec::Save> back = codec::parse(stream.data(), stream.size());
        CHECK(back.has_value());
        if (back)
        {
            CHECK(codec::write(*back) == stream);
            CHECK(back->profiles[2].name == "The Testers" && back->profiles[2].cash == 1250);
            CHECK(back->campaignItems[1].states[2 * codec::kDifficulties + 2].score == 187654);
        }

        // Cut short, and a version Campaign::Load would refuse.
        CHECK(!codec::parse(stream.data(), stream.size() - 10));
        std::vector<uint8_t> old = stream;
        old[0] = 12;
        CHECK(!codec::parse(old.data(), old.size()));
    }

    void gh2Sections()
    {
        const codec::Save fresh = freshCampaign();
        const codec::Save save = playedCampaign();

        // An untouched campaign round trips too.
        Store blank;
        codec::toStore(fresh, fresh, blank);
        CHECK(codec::write(codec::fromStore(blank, fresh)) == codec::write(fresh));

        Store store;
        store.section("mod colours").set("highway", "ff00ff");
        codec::toStore(save, fresh, store);
        CHECK(store.find("mod colours") != nullptr);
        // Through the file's text, as a save and a load are.
        const codec::Save back = codec::fromStore(Store::parse(store.text()), fresh);
        CHECK(codec::write(back) == codec::write(save));

        // Saving over an older save drops what it no longer holds.
        codec::toStore(fresh, fresh, store);
        CHECK(codec::write(codec::fromStore(store, fresh)) == codec::write(fresh));
        CHECK(store.find("mod colours") != nullptr);
    }

    // Songs go by <source>_<name> in the game (content/sources.h) and by
    // their disc's names in the store and on a card, as saves always have.
    void sourceNames()
    {
        const codec::Save bareFresh = freshCampaign(), bareSave = playedCampaign();
        const codec::Save fresh = codec::fromCard(bareFresh, "gh2"), save = codec::fromCard(bareSave, "gh2");
        CHECK(fresh.campaignItems[1].name == "gh2_freebird" && save.scores[1].song == "gh2_freebird");
        CHECK(codec::write(codec::asCard(save, "gh2")) == codec::write(bareSave));

        Store store;
        codec::toStore(save, fresh, store);
        const auto *scores = store.find("gh2 scores expert");
        CHECK(scores && scores->get("freebird.1") && !scores->get("gh2_freebird.1"));
        CHECK(codec::write(codec::fromStore(Store::parse(store.text()), fresh)) == codec::write(save));

        // A save kept before songs had sources reads the same.
        Store old;
        codec::toStore(bareSave, bareFresh, old);
        CHECK(old.text() == store.text());
        CHECK(codec::write(codec::fromStore(old, fresh)) == codec::write(save));

        // Another game's song keeps its scores in its game's sections.
        codec::Save mixed = save, mixedFresh = fresh;
        for (codec::Save *s : {&mixed, &mixedFresh})
            s->scores.push_back(s->scores[0]), s->scores.back().song = "gh80s_bangyourhead";
        mixed.scores.back().lists[3][0] = {"TESTER", 99999};
        const codec::Games games{"gh2", {{"gh80s_bangyourhead", "gh80s"}}};
        Store both;
        codec::toStore(mixed, mixedFresh, both, games);
        const auto *eighties = both.find("gh80s scores expert");
        CHECK(eighties && eighties->get("bangyourhead.1"));
        CHECK(codec::write(codec::fromStore(both, mixedFresh, games)) == codec::write(mixed));

        // The 360's songs were x_<name> before.
        CHECK(codec::storedSong("gh2x", "gh2x_surrender") == "x_surrender");
        CHECK(codec::ourSong("gh2x", "x_surrender") == "gh2x_surrender");
        CHECK(codec::storedSong("gh2", "cs_1234") == "cs_1234");
        // A section under another note engine is still its game's.
        CHECK(codec::storedSong("gh2@gh1", "gh2_surrender") == "surrender");
        CHECK(codec::ourSong("gh2@gh1", "surrender") == "gh2_surrender");

        // A venue goes by its own name in its game's sections: GH1's basement
        // by GH2's battle, which GH1's career saved it as when it stood in
        // for it.
        namespace sources = gh2::sources;
        sources::add(sources::Kind::Venue, "gh1", "basement", "battle");
        codec::Games gh1;
        gh1.own = "gh1";
        codec::Save stoodIn = bareSave, room = bareSave, roomFresh = bareFresh;
        stoodIn.profileItems[0].states[0].flags = room.profileItems[0].states[0].flags = 0x2;
        room.profileItems[0].name = roomFresh.profileItems[0].name = "gh1_basement";
        Store before, after;
        codec::toStore(stoodIn, bareFresh, before, gh1);
        codec::toStore(room, roomFresh, after, gh1);
        CHECK(before.text().find("venue.battle") != std::string::npos && after.text() == before.text());

        // A game's own outfit by its name on the disc, a character by its own
        // whichever game's sections, another game's outfit by its id, and the
        // names the outfits other games add went by before.
        sources::addShared(sources::Kind::Character, "gh2", "punk");
        sources::add(sources::Kind::Outfit, "gh2", "punk1");
        sources::add(sources::Kind::Outfit, "gh80s", "punk1");
        CHECK(codec::storedItem("gh2", "gh2_punk1") == "punk1" && codec::storedItem("gh80s", "gh80s_punk1") == "punk1");
        CHECK(codec::storedItem("gh2", "gh80s_punk1") == "gh80s_punk1");
        CHECK(sources::addShared(sources::Kind::Character, "gh80s", "punk").id == "gh2_punk" && codec::storedItem("gh80s", "gh2_punk") == "punk");
        CHECK(codec::ourItem("gh80s", "punk1", sources::Kind::Outfit) == "gh80s_punk1");
        CHECK(codec::ourItem("gh80s", "punk", sources::Kind::Character) == "gh2_punk");
        CHECK(codec::ourItem("gh2", "punk3", sources::Kind::Outfit) == "gh80s_punk1");
        // A guitar's finish is GH2's whichever game's sections hold it.
        sources::addShared(sources::Kind::Finish, "gh2", "lp_cherry");
        CHECK(codec::storedItem("gh80s", "gh2_lp_cherry") == "lp_cherry");
        CHECK(codec::ourItem("gh80s", "lp_cherry", sources::Kind::Finish) == "gh2_lp_cherry");
        codec::Save outfitFresh = bareFresh;
        outfitFresh.profileItems[0].name = "gh80s_punk1";
        outfitFresh.profileItems[0].type = 2;
        Store named;
        named.section("band 1 gh2").set("outfit.punk3", "unlocked");
        CHECK(codec::fromStore(named, outfitFresh).profileItems[0].states[0].flags == 0x2);
    }

    void ps2Card(const fs::path &dir)
    {
        const std::string path = (dir / "card.ps2").string();
        CHECK(!Ps2Card::open(path));

        const std::vector<Ps2Card::File> files = {
            {"BASLUS-21447", bytes(10, 1)},
            {"gh2.sav", bytes(0x21c00, 2)},
            {"icon.sys", bytes(964, 3)},
            {"gh.icn", bytes(88302, 4)},
        };
        Ps2Card card = Ps2Card::format();
        CHECK(!card.read("BASLUS-21447", "gh2.sav"));
        CHECK(card.replaceDir("BASLUS-21447", files));
        CHECK(card.save(path));
        CHECK(fs::file_size(path) == 16384u * 528u);

        std::optional<Ps2Card> back = Ps2Card::open(path);
        CHECK(back.has_value());
        if (!back)
            return;
        for (const Ps2Card::File &file : files)
        {
            const auto data = back->read("BASLUS-21447", file.first);
            CHECK(data && *data == file.second);
        }
        CHECK(!back->read("BASLUS-21447", "missing"));
        CHECK(!back->read("BASLUS-00000", "gh2.sav"));

        // A second save replaces the directory whole; another game's stays.
        const std::vector<Ps2Card::File> other = {{"BASLUS-21224", bytes(10, 5)}, {"gh1.sav", bytes(40000, 6)}};
        const std::vector<Ps2Card::File> smaller = {{"BASLUS-21447", bytes(10, 7)}, {"gh2.sav", bytes(0x21c00, 8)}};
        CHECK(back->replaceDir("BASLUS-21224", other));
        CHECK(back->replaceDir("BASLUS-21447", smaller));
        CHECK(back->save(path));
        back = Ps2Card::open(path);
        CHECK(back.has_value());
        if (!back)
            return;
        CHECK(back->read("BASLUS-21447", "gh2.sav") == std::optional(smaller[1].second));
        CHECK(!back->read("BASLUS-21447", "gh.icn"));
        CHECK(back->read("BASLUS-21224", "gh1.sav") == std::optional(other[1].second));

        // Replacing it many times over leaks no clusters: the card never fills.
        for (int i = 0; i < 80; ++i)
            CHECK(back->replaceDir("BASLUS-21447", files));
        CHECK(back->read("BASLUS-21447", "gh.icn") == std::optional(files[3].second));
        CHECK(back->read("BASLUS-21224", "gh1.sav") == std::optional(other[1].second));

        // More than the card holds.
        CHECK(!Ps2Card::format().replaceDir("BIG", {{"BIG", bytes(9u << 20, 9)}}));

        // Not a card: the wrong size, then the right size with no format.
        writeBytes(path, bytes(4096, 1));
        CHECK(!Ps2Card::open(path));
        writeBytes(path, std::vector<uint8_t>(16384u * 528u, 0xff));
        CHECK(!Ps2Card::open(path));
    }
}

int main()
{
    const fs::path dir = fs::temp_directory_path() / ("ghc-save-test-" + std::to_string(std::random_device()()));
    fs::create_directories(dir);
    storeText();
    bests();
    storeFile(dir);
    gh2Stream();
    gh2Sections();
    sourceNames();
    ps2Card(dir);
    std::error_code error;
    fs::remove_all(dir, error);
    std::cout << (s_failed ? "save_test: " + std::to_string(s_failed) + " failed" : std::string("save_test: ok"))
              << std::endl;
    return s_failed ? 1 : 0;
}
