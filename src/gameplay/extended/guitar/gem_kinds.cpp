// SongParser reads a gem's frets and length and passes no kind: its
// MultiGemInfo's no-strum byte (+0x18) is always 0 (0x245fec), and
// GameGemList::AddMultiGem overwrites what the gem takes of it with
// WillBeNoStrum. So a kind the chart decides goes into that byte as
// SongData::AddMultiGem takes the gem, with bit 1 set to say so, and
// WillBeNoStrum answers with it where it is set; bit 2 says to answer the
// opposite of the engine judging. GameGem's constructor (0x2363e8) copies
// bit 0 alone. WillBeNoStrum reads the threshold from its GameGemList's
// +4, where the song's goes.
//
// A song's tracks hold one difficulty each, the player's, which the gem
// does not say but its MIDI note does: SongParser::OnGemEnd (0x245ee8) is
// given its pitch, 60 + 12 a difficulty (PitchToSlot, 0x246628), and adds
// it to SongData from there.

#include "gameplay/extended/guitar/gem_kinds.h"

#include "guest.h"
#include "guest/data_array.h"
#include "hook.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstring>
#include <memory>
#include <utility>

namespace gh2::guitar
{
    namespace
    {
        using Gems = std::array<std::map<uint32_t, Gh2Gem>, 4>;

        // MultiGemInfo's no-strum byte: bit 1 the kind decided here, bit 2
        // the engine's flipped.
        constexpr uint8_t kDecided = 0x2u, kFlip = 0x4u;

        struct Song
        {
            Gems gems;
            std::optional<uint32_t> hopoThreshold;
        };
        std::map<std::string, std::shared_ptr<const Song>> s_songs;
        // The song loading's midi_file, and its gems once its MIDI is read:
        // made then, the first time it is (custom/song_files.cpp).
        std::string s_loadingMidi;
        std::shared_ptr<const Song> s_loading;
        std::map<uint32_t, int> s_difficultyOf; // by SongData track
        int s_endingDifficulty = -1;            // the gem OnGemEnd is adding's, -1 for none

        const Addresses *s_addresses = nullptr;

        // The song's (midi_file ...), in its entry's (song ...) or in it.
        std::string midiFile(uint8_t *rdram, uint32_t array)
        {
            const guest::DataArray entry(rdram, array);
            guest::DataArray found = entry.find("midi_file");
            if (found.size() < 2)
                found = entry.find("song").find("midi_file");
            if (found.size() < 2)
                return {};
            // A symbol, or a string as quickplay's {sprint} sets it, its
            // chars at its +0.
            const guest::DataNode file = found.node(1);
            const uint32_t chars = file.type() == script::kString ? load<uint32_t>(rdram, file.value()) : file.value();
            return chars ? reinterpret_cast<const char *>(getMemPtr(rdram, chars)) : "";
        }

        struct LoadTag;
        struct OnGemEndTag;
        struct AddMultiGemTag;

        // SongData::Load(BinStream &, const DataArray *song, ...), 0x243da8,
        // which both of SongDB::Load's ways reach.
        void onLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            s_loadingMidi = midiFile(rdram, GPR_U32(ctx, 6));
            s_loading.reset();
            s_difficultyOf.clear();
        }

        // SongParser::OnGemEnd(int tick, unsigned char pitch), 0x245ee8: a
        // guitar part's five frets a difficulty from 60.
        void onGemEnd(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const int from = static_cast<int>(GPR_U32(ctx, 6) & 0xffu) - 60;
            s_endingDifficulty = from >= 0 && from % 12 < 5 && from / 12 < 4 ? from / 12 : -1;
        }

        // SongData::AddMultiGem(int track, const MultiGemInfo &), 0x244890.
        void onAddMultiGem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const int ending = std::exchange(s_endingDifficulty, -1);
            if (!s_loading)
                if (const auto it = s_songs.find(s_loadingMidi); it != s_songs.end())
                    s_loading = it->second;
            if (!s_loading)
                return;
            const uint32_t track = GPR_U32(ctx, 5);
            const guest::MultiGemInfo info(rdram, GPR_U32(ctx, 6));
            if (ending >= 0)
                s_difficultyOf[track] = ending;
            const auto known = s_difficultyOf.find(track);
            if (known == s_difficultyOf.end())
                return;
            const auto frets = static_cast<uint8_t>(info.slots());
            const auto it = s_loading->gems[known->second].find(static_cast<uint32_t>(info.tick()));
            if (it == s_loading->gems[known->second].end() || it->second.frets != frets)
                return;
            const Gh2Gem *gem = &it->second;
            if (gem->decided)
                info.setNoStrum(static_cast<uint8_t>(kDecided | (gem->hopo ? 1u : 0u)));
            else if (gem->flip)
                info.setNoStrum(kFlip);
        }

        // GameGemList::WillBeNoStrum(const MultiGemInfo &), 0x24d360, GH1's
        // in its place under GH1's engine (gameplay/engine/gh1/watcher.cpp).
        PS2Runtime::RecompiledFunction s_willBeNoStrum = nullptr;
        void willBeNoStrum(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint8_t decided = guest::MultiGemInfo(rdram, GPR_U32(ctx, 5)).noStrum();
            if (decided & kDecided)
            {
                SET_GPR_U32(ctx, 2, decided & 1u);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            if (s_loading && s_loading->hopoThreshold)
                guest::GameGemList(rdram, GPR_U32(ctx, 4)).setHopoThreshold(static_cast<int32_t>(*s_loading->hopoThreshold));
            s_willBeNoStrum(rdram, ctx, runtime);
            if (decided & kFlip)
                SET_GPR_U32(ctx, 2, GPR_U32(ctx, 2) ^ 1u);
        }
    }

    void setGems(const std::string &midi, const std::array<std::map<uint32_t, Gh2Gem>, 4> &gems,
                 std::optional<uint32_t> hopoThreshold)
    {
        s_songs[midi] = std::make_shared<const Song>(Song{gems, hopoThreshold});
    }

    void installGemKinds(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<LoadTag>::install(runtime, addresses.songDataLoad, onLoad);
        EntryHook<OnGemEndTag>::install(runtime, addresses.songParserOnGemEnd, onGemEnd);
        EntryHook<AddMultiGemTag>::install(runtime, addresses.songDataAddMultiGem, onAddMultiGem);
        replaceKeeping(runtime, addresses.willBeNoStrum, &willBeNoStrum, s_willBeNoStrum);
    }
}
