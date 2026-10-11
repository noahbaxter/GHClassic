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

#include <iostream>
#include <map>
#include <optional>
#include <string>

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

        // A GH1 .bnk: its version and two words, a count of regions and
        // that many of 16 bytes (the sample each plays first), 16 bytes, a
        // count of names and each (u16 region, a byte, the name, u32 1000),
        // then a count of samples and each one's offset, size and rate in
        // the .nse beside it, PS-ADPCM.
        std::map<std::string, Sample> gh1Samples(const Bytes &bnk, const Bytes &nse)
        {
            std::map<std::string, Sample> out;
            const uint32_t regions = milo::u32(bnk, 12u);
            size_t o = 16u + regions * 16u + 16u;
            const uint32_t names = milo::u32(bnk, o);
            o += 4u;
            std::map<std::string, uint32_t> region;
            for (uint32_t i = 0; i < names && o + 7u <= bnk.size(); ++i)
            {
                const uint32_t r = bnk[o] | (bnk[o + 1u] << 8);
                size_t at = o + 3u;
                const std::string name = milo::str(bnk, at);
                region.emplace(name, r);
                o = at + 4u;
            }
            const uint32_t count = milo::u32(bnk, o);
            o += 4u;
            for (const auto &[name, r] : region)
            {
                const uint32_t index = r < regions ? milo::u32(bnk, 16u + r * 16u) : count;
                if (index >= count || o + index * 12u + 12u > bnk.size())
                    continue;
                const uint32_t offset = milo::u32(bnk, o + index * 12u), size = milo::u32(bnk, o + index * 12u + 4u);
                if (offset + size > nse.size())
                    continue;
                // GH1 ends a sample on a block flagged 1, then 07 77 77...;
                // GH2's blocks are each flagged 2 and end at the count.
                Sample s{{}, milo::u32(bnk, o + index * 12u + 8u)};
                for (size_t b = offset; b + 16u <= offset + size; b += 16u)
                {
                    const uint8_t flags = nse[b + 1u];
                    s.adpcm.insert(s.adpcm.end(), nse.begin() + static_cast<std::ptrdiff_t>(b),
                                   nse.begin() + static_cast<std::ptrdiff_t>(b + 16u));
                    s.adpcm[s.adpcm.size() - 15u] = 2u;
                    if (flags & 1u)
                        break;
                }
                out.emplace(name, std::move(s));
            }
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

    std::optional<std::vector<uint8_t>> silenced(const std::vector<uint8_t> &bank)
    {
        auto dir = milo::read(bank);
        if (!dir)
            return std::nullopt;
        for (size_t i = 0; i < dir->entries.size(); ++i)
            if (dir->entries[i].first == "SynthSample")
                if (auto body = withData(dir->bodies[i], kSilence, 22050u))
                    dir->bodies[i] = std::move(*body);
        return milo::write(*dir);
    }
}
