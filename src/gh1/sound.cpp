// GH1's sounds where GH2's differ.
//
// GH1's banks are GH2's recordings: the buttons, the cash register, the
// nowbar ticks and the star power gem hits byte for byte, and its star
// power (sp_deployed, sp_depleted, sp_available) the same takes mixed
// louder, which GH2 re-encoded at half the level. Its stingers for beating
// the game (beat_easy, beat_other) are the same takes at 22050 Hz, where
// GH2 keeps them at 12000, so GH2's in-game bank, every game's, takes
// GH1's. GH2's riser to star power (sp_deploystart.cue) and its sound for
// a lost streak (streak_broken_<n>x.cue), which GH1 never had, stay in for
// every game too.

#include "gh1/sound.h"

#include "disc/ark.h"
#include "milo/milo.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace gh2::gh1
{
    namespace
    {
        using milo::Bytes;

        struct Sample
        {
            Bytes adpcm;
            uint32_t rate = 0u;
        };

        // A GH1 bank's samples, and each sound's: one sample per region it
        // plays, a sound of several (a left and a right, or layers) named
        // once for each.
        struct Bank
        {
            std::vector<Sample> samples;
            std::map<std::string, std::vector<uint32_t>> sounds;
        };

        // A GH1 .bnk: its version and two words, a count of regions and
        // that many of 16 bytes (the sample each plays first), a count of
        // 12-byte entries and those, a count of names and each (u16 region,
        // a byte, the name, u32 1000), then a count of samples and each
        // one's offset, size and rate in the .nse beside it, PS-ADPCM.
        Bank gh1Bank(const Bytes &bnk, const Bytes &nse)
        {
            Bank out;
            const uint32_t regions = milo::u32(bnk, 12u);
            size_t o = 16u + regions * 16u;
            o += 4u + 12u * milo::u32(bnk, o);
            const uint32_t names = milo::u32(bnk, o);
            o += 4u;
            for (uint32_t i = 0; i < names && o + 7u <= bnk.size(); ++i)
            {
                const uint32_t r = bnk[o] | (bnk[o + 1u] << 8);
                size_t at = o + 3u;
                const std::string name = milo::str(bnk, at);
                if (r < regions)
                    out.sounds[name].push_back(milo::u32(bnk, 16u + r * 16u));
                o = at + 4u;
            }
            const uint32_t count = milo::u32(bnk, o);
            o += 4u;
            for (uint32_t index = 0; index < count && o + index * 12u + 12u <= bnk.size(); ++index)
            {
                const uint32_t offset = milo::u32(bnk, o + index * 12u), size = milo::u32(bnk, o + index * 12u + 4u);
                // GH1 ends a sample on a block flagged 1, then 07 77 77...;
                // GH2's blocks are each flagged 2 and end at the count.
                Sample s{{}, milo::u32(bnk, o + index * 12u + 8u)};
                for (size_t b = offset; offset + size <= nse.size() && b + 16u <= offset + size; b += 16u)
                {
                    const uint8_t flags = nse[b + 1u];
                    s.adpcm.insert(s.adpcm.end(), nse.begin() + static_cast<std::ptrdiff_t>(b),
                                   nse.begin() + static_cast<std::ptrdiff_t>(b + 16u));
                    s.adpcm[s.adpcm.size() - 15u] = 2u;
                    if (flags & 1u)
                        break;
                }
                out.samples.push_back(std::move(s));
            }
            return out;
        }

        // The sample a sound plays first, as the in-game bank's are swapped.
        std::map<std::string, Sample> gh1Samples(const Bytes &bnk, const Bytes &nse)
        {
            const Bank bank = gh1Bank(bnk, nse);
            std::map<std::string, Sample> out;
            for (const auto &[name, indices] : bank.sounds)
                if (!indices.empty() && indices.front() < bank.samples.size() && !bank.samples[indices.front()].adpcm.empty())
                    out.emplace(name, bank.samples[indices.front()]);
            return out;
        }

        // A SynthSample 5 (its rev, two words, a byte, its file, a loop
        // flag and points, then 11, 2 for PS-ADPCM, frames, rate, bytes, a
        // byte 1 and the data) with other data, played through once.
        std::optional<Bytes> withData(const Bytes &body, const Bytes &adpcm, uint32_t rate)
        {
            size_t o = 13u;
            if (body.size() < o || milo::u32(body, 0u) != 5u)
                return std::nullopt;
            milo::str(body, o);
            if (o + 30u > body.size() || milo::u32(body, o + 9u) != 11u || milo::u32(body, o + 13u) != 2u)
                return std::nullopt;
            Bytes out(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(o));
            out.push_back(0u);
            milo::putU32(out, 0u);
            milo::putU32(out, 0xffffffffu);
            milo::putU32(out, 11u);
            milo::putU32(out, 2u);
            milo::putU32(out, static_cast<uint32_t>(adpcm.size() / 16u * 28u));
            milo::putU32(out, rate);
            milo::putU32(out, static_cast<uint32_t>(adpcm.size()));
            out.push_back(1u);
            out.insert(out.end(), adpcm.begin(), adpcm.end());
            return out;
        }

        // One block of nothing, flagged as GH2's are.
        const Bytes kSilence = [] {
            Bytes b(16u, 0u);
            b[0] = 0x0cu;
            b[1] = 2u;
            return b;
        }();
    }

    void addSounds(size_t layer, size_t disc)
    {
        // GH1's credits music, at the path GH2's has.
        ark::lend(layer, "sfx/streams/credits.vgs", disc, "sfx/streams/credits.vgs");

        const auto bnk = ark::readFile(disc, "sfx/gen/ingame.bnk"), nse = ark::readFile(disc, "sfx/gen/ingame.nse");
        const auto bank = ark::readFile(0u, "sfx/gen/ingame_bank.milo_ps2");
        auto dir = bank ? milo::read(*bank) : std::nullopt;
        if (!bnk || !nse || !dir)
        {
            std::cerr << "[gh1] cannot read the in-game sound banks" << std::endl;
            return;
        }
        const std::map<std::string, Sample> theirs = gh1Samples(*bnk, *nse);
        // GH2's sample of each stinger, by its file, and GH1's in its place.
        const std::map<std::string, const char *> kSwap = {
            {"beat_easy.wav", "beat_easy"},
            {"beat_other.wav", "beat_other"},
        };
        size_t done = 0u;
        for (size_t i = 0; i < dir->entries.size(); ++i)
        {
            const auto it = kSwap.find(dir->entries[i].second);
            if (dir->entries[i].first != "SynthSample" || it == kSwap.end())
                continue;
            const auto sample = theirs.find(it->second);
            if (sample == theirs.end())
                continue;
            if (auto body = withData(dir->bodies[i], sample->second.adpcm, sample->second.rate))
            {
                dir->bodies[i] = std::move(*body);
                ++done;
            }
        }
        if (done != kSwap.size())
            std::cerr << "[gh1] " << done << " of " << kSwap.size() << " stingers made GH1's" << std::endl;
        ark::addFile("sfx/gen/ingame_bank.milo_ps2", milo::write(*dir));
    }

    std::optional<std::vector<uint8_t>> venueBank(const std::vector<uint8_t> &standIn, size_t disc, const std::string &venue)
    {
        const std::string stem = "venues/" + venue + "/gen/" + venue;
        const auto bnk = ark::readFile(disc, stem + ".bnk"), nse = ark::readFile(disc, stem + ".nse");
        auto dir = milo::read(standIn);
        if (!bnk || !nse || !dir)
            return std::nullopt;
        const Bank bank = gh1Bank(*bnk, *nse);
        std::vector<std::set<std::string>> theirs(bank.samples.size());
        for (const auto &[name, indices] : bank.sounds)
            for (const uint32_t index : indices)
                if (index < theirs.size())
                    theirs[index].insert(name);
        // Each of GH2's samples, by the GH1 sounds whose Sfx name it.
        std::map<std::string, std::set<std::string>> ours;
        for (const auto &[cls, name] : dir->entries)
            if (cls == "SynthSample")
                ours[name];
        for (size_t i = 0; i < dir->entries.size(); ++i)
        {
            if (dir->entries[i].first != "Sfx" || bank.sounds.count(dir->entries[i].second) == 0u)
                continue;
            const Bytes &body = dir->bodies[i];
            for (auto &[sample, sounds] : ours)
            {
                Bytes named;
                milo::putStr(named, sample);
                if (std::search(body.begin(), body.end(), named.begin(), named.end()) != body.end())
                    sounds.insert(dir->entries[i].second);
            }
        }
        size_t matched = 0u;
        for (size_t i = 0; i < dir->entries.size(); ++i)
        {
            if (dir->entries[i].first != "SynthSample")
                continue;
            const std::set<std::string> &sounds = ours[dir->entries[i].second];
            const auto it = sounds.empty() ? theirs.end() : std::find(theirs.begin(), theirs.end(), sounds);
            const Sample *sample = it != theirs.end() ? &bank.samples[static_cast<size_t>(it - theirs.begin())] : nullptr;
            if (sample && sample->adpcm.empty())
                sample = nullptr;
            if (sample)
                ++matched;
            else if (!sounds.empty())
                std::cerr << "[gh1] " << venue << ": no sample of GH1's for " << dir->entries[i].second << std::endl;
            if (auto body = sample ? withData(dir->bodies[i], sample->adpcm, sample->rate)
                                   : withData(dir->bodies[i], kSilence, 22050u))
                dir->bodies[i] = std::move(*body);
        }
        if (matched == 0u)
            return std::nullopt;
        return milo::write(*dir);
    }
}
