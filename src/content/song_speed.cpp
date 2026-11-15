// A .vgs's channels each have a rate of their own (GH2's bass is often at
// half its guitar's), and their blocks are laid out a round of the fastest
// rate at a time, a slower channel's in each round that reaches its next
// block: "0 1 2 3 4 0 1 2 3" for four at 32250 Hz and one at 16125.

#include "content/song_speed.h"

#include "content/vgs.h"
#include "disc/ark.h"
#include "guest.h"
#include "hook.h"
#include "script.h"
#include "settings/latency.h"
#include "settings/settings.h"
#include "synth/adpcm.h"
#include "synth/synth.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>
#include <numeric>
#include <optional>

namespace gh2::song_speed
{
    namespace
    {
        constexpr char kPrefix[] = "ghc_speed/";
        constexpr uint32_t kHeaderBytes = 0x80u, kChunkBytes = 0x10000u;
        constexpr int kSlowest = 10, kFastest = 500;
        // BeatMatch's pitch correction, GH2's own slicing of a song slowed
        // with no practice files, on unless the song has practice_speeds.
        constexpr uint32_t kPitchCorrect = 0xC0u;

        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_restart = nullptr, s_setPracticeSpeed = nullptr;
        int s_percent = 100;
        // The song being made plays in practice, whose speed changes as it
        // plays; its BeatMatch, from its Restart.
        bool s_practice = false;
        uint32_t s_beatMatch = 0u;
        // The speed a practice song's audio is made at: played at 200% it is
        // at the SPU's most, 4x a 48 kHz stream (spu.cpp); below 50% it
        // plays under its own rate, its highest highs lost.
        constexpr int kPracticeBase = 50;
        int s_playing = 100; // the speed the game plays at now

        // A .vgs decoded, every channel read at the fastest one's rate.
        class Decoded final : public vgs::Pcm
        {
        public:
            static std::unique_ptr<Decoded> open(ark::Made file)
            {
                uint8_t header[kHeaderBytes];
                if (file.size < kHeaderBytes || file.read(0u, header, kHeaderBytes) != kHeaderBytes ||
                    std::memcmp(header, "VgS!", 4u) != 0)
                    return nullptr;
                auto out = std::unique_ptr<Decoded>(new Decoded(std::move(file)));
                for (uint32_t c = 0; c < 15u; ++c)
                {
                    uint32_t rate = 0u, blocks = 0u;
                    std::memcpy(&rate, header + 8u + 8u * c, 4u);
                    std::memcpy(&blocks, header + 12u + 8u * c, 4u);
                    if (rate == 0u)
                        break;
                    out->m_rates.push_back(rate);
                    out->rate = std::max(out->rate, rate);
                    out->frames = std::max(out->frames, uint64_t(blocks) * synth::kAdpcmBlockSamples * out->rate / rate);
                }
                out->channels = static_cast<uint32_t>(out->m_rates.size());
                if (out->channels == 0u)
                    return nullptr;
                // Rounds after which every channel starts a block together.
                for (uint32_t r : out->m_rates)
                    out->m_period = std::lcm(out->m_period, uint64_t(out->rate / std::gcd(out->rate, r)));
                out->m_held.resize(out->channels);
                out->m_first.resize(out->channels);
                out->seek(0u);
                return out;
            }

            bool seek(uint64_t frame) override
            {
                const uint64_t round = frame / synth::kAdpcmBlockSamples / m_period * m_period;
                uint64_t blocks = 0u;
                for (uint32_t r : m_rates)
                    blocks += round * r / rate;
                m_at = kHeaderBytes + blocks * synth::kAdpcmBlockBytes;
                m_chunk.clear();
                m_chunkAt = 0u;
                m_states.assign(channels, {});
                for (auto &h : m_held)
                    h.clear();
                m_out = round * synth::kAdpcmBlockSamples;
                for (uint32_t c = 0; c < channels; ++c)
                    m_first[c] = double(m_out * m_rates[c] / rate);
                // Decoded from the round every channel starts a block on,
                // up to the frame asked for.
                std::vector<std::vector<float>> skip(channels, std::vector<float>(4096u));
                std::vector<float *> to(channels);
                for (uint64_t left = frame - m_out; left > 0u;)
                {
                    for (uint32_t c = 0; c < channels; ++c)
                        to[c] = skip[c].data();
                    const uint32_t n = read(to.data(), static_cast<uint32_t>(std::min<uint64_t>(left, 4096u)));
                    if (n == 0u)
                        break;
                    left -= n;
                }
                return true;
            }

            uint32_t read(float *const *out, uint32_t most) override
            {
                most = static_cast<uint32_t>(std::min<uint64_t>(most, frames - std::min(m_out, frames)));
                if (most == 0u)
                    return 0u;
                // Each channel's samples through the one after the last
                // frame asked for, read between by a straight line.
                for (uint32_t c = 0; c < channels; ++c)
                {
                    const double step = double(m_rates[c]) / rate;
                    const double start = double(m_out) * step - m_first[c];
                    const size_t need = static_cast<size_t>(start + (most - 1u) * step) + 2u;
                    while (m_held[c].size() < need && decodeBlock())
                        ;
                    m_held[c].resize(std::max(m_held[c].size(), need), 0.0f);
                    for (uint32_t i = 0; i < most; ++i)
                    {
                        const double x = start + i * step;
                        const size_t a = static_cast<size_t>(x);
                        out[c][i] = m_held[c][a] + (m_held[c][a + 1u] - m_held[c][a]) * float(x - double(a));
                    }
                }
                m_out += most;
                // Let go of samples behind the next frame.
                for (uint32_t c = 0; c < channels; ++c)
                {
                    const double next = double(m_out) * m_rates[c] / rate - m_first[c];
                    const size_t drop = std::min(static_cast<size_t>(next), m_held[c].size());
                    m_held[c].erase(m_held[c].begin(), m_held[c].begin() + static_cast<std::ptrdiff_t>(drop));
                    m_first[c] += double(drop);
                }
                return most;
            }

        private:
            explicit Decoded(ark::Made file) : m_file(std::move(file)) {}

            // The next block onto its channel's samples; false at the end.
            bool decodeBlock()
            {
                if (m_chunkAt + synth::kAdpcmBlockBytes > m_chunk.size())
                {
                    if (m_at >= m_file.size)
                        return false;
                    m_chunk.resize(static_cast<size_t>(std::min<uint64_t>(kChunkBytes, m_file.size - m_at)));
                    m_chunk.resize(m_file.read(m_at, m_chunk.data(), m_chunk.size()) / synth::kAdpcmBlockBytes *
                                   synth::kAdpcmBlockBytes);
                    m_at += m_chunk.size();
                    m_chunkAt = 0u;
                    if (m_chunk.empty())
                        return false;
                }
                const uint8_t *block = m_chunk.data() + m_chunkAt;
                m_chunkAt += synth::kAdpcmBlockBytes;
                const uint32_t c = block[1] & 0x0fu;
                if (c >= channels)
                    return true;
                int16_t samples[synth::kAdpcmBlockSamples];
                synth::decodeAdpcm(block, m_states[c], samples);
                for (int16_t s : samples)
                    m_held[c].push_back(s / 32768.0f);
                return true;
            }

            ark::Made m_file;
            std::vector<uint32_t> m_rates;
            uint64_t m_period = 1u;
            uint64_t m_at = 0u; // the file's next byte to read
            std::vector<uint8_t> m_chunk;
            size_t m_chunkAt = 0u;
            std::vector<synth::AdpcmState> m_states;
            std::vector<std::deque<float>> m_held;
            std::vector<double> m_first; // each channel's sample m_held starts at
            uint64_t m_out = 0u;         // the next frame out
        };

        // Audio stretched by Signalsmith Stretch to 1 / speed its length,
        // its pitch kept, rated at its rate over the speed. Its frames are
        // the song's time at any speed the game plays it at.
        class Stretched final : public vgs::Pcm
        {
        public:
            Stretched(std::unique_ptr<vgs::Pcm> from, double speed) : m_from(std::move(from)), m_speed(speed)
            {
                channels = m_from->channels;
                rate = static_cast<uint32_t>(std::lround(m_from->rate / speed));
                frames = static_cast<uint64_t>(double(m_from->frames) / speed);
                m_stretch.presetDefault(static_cast<int>(channels), float(m_from->rate));
                seek(0u);
            }

            bool seek(uint64_t frame) override
            {
                m_out = frame;
                m_in = static_cast<uint64_t>(std::llround(double(frame) * m_speed));
                m_from->seek(m_in);
                // Output from here on comes from input from m_in on.
                const int length = m_stretch.outputSeekLength(float(m_speed));
                take(static_cast<uint32_t>(length));
                m_stretch.outputSeek(m_buffers, length);
                m_in += static_cast<uint64_t>(length);
                return true;
            }

            uint32_t read(float *const *out, uint32_t most) override
            {
                most = static_cast<uint32_t>(std::min<uint64_t>(most, frames - std::min(m_out, frames)));
                if (most == 0u)
                    return 0u;
                const uint64_t through = static_cast<uint64_t>(std::llround(double(m_out + most) * m_speed)) +
                                         static_cast<uint64_t>(m_stretch.outputSeekLength(float(m_speed)));
                const uint32_t n = static_cast<uint32_t>(through - std::min(through, m_in));
                take(n);
                m_stretch.process(m_buffers, static_cast<int>(n), out, static_cast<int>(most));
                m_in += n;
                m_out += most;
                return most;
            }

        private:
            // The next n input frames into m_buffers, silence past the end.
            void take(uint32_t n)
            {
                m_buffers.resize(channels);
                std::vector<float *> to(channels);
                for (uint32_t c = 0; c < channels; ++c)
                {
                    m_buffers[c].assign(n, 0.0f);
                    to[c] = m_buffers[c].data();
                }
                m_from->fill(to.data(), n);
            }

            std::unique_ptr<vgs::Pcm> m_from;
            double m_speed;
            signalsmith::stretch::SignalsmithStretch<float> m_stretch;
            std::vector<std::vector<float>> m_buffers;
            uint64_t m_in = 0u, m_out = 0u; // the next input frame and output frame
        };

        // BeatMatch::Restart, then the speed, which GamePanel::Enter sets
        // itself only in practice.
        void restart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31), beatMatch = GPR_U32(ctx, 4);
            callOriginal(rdram, ctx, runtime, s_addresses->beatMatchRestart, s_restart);
            // The audio is read again from here, all of it at this speed.
            s_beatMatch = beatMatch;
            s_playing = s_percent;
            if (s_percent != 100)
                runtime->callGuestFunction(rdram, ctx, s_addresses->beatMatchSetPracticeSpeed, {beatMatch});
            pushSyncOffset(rdram, ctx, runtime);
            ctx->pc = returnTo;
        }

        // The game plays at {song_speed} from now (SetPracticeSpeed, through
        // setPracticeSpeed below), the audio it holds already with it: its
        // pitch is put back in the mix, not in its data.
        void playAtSpeed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const R5900Context saved = *ctx;
            runtime->callGuestFunction(rdram, ctx, s_addresses->beatMatchSetPracticeSpeed, {s_beatMatch});
            *ctx = saved;
            s_playing = s_percent;
            pushSyncOffset(rdram, ctx, runtime);
        }

        // BeatMatch::SetPracticeSpeed(float) at our speed, the audio being
        // stretched already.
        void setPracticeSpeed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_percent != 100 || s_practice)
            {
                ctx->f[12] = s_percent / 100.0f;
                store<uint32_t>(rdram, GPR_U32(ctx, 4) + kPitchCorrect, 0u);
            }
            s_setPracticeSpeed(rdram, ctx, runtime);
        }

        // The song's blocks' audio names passed through {song_speed <op>}.
        void rename(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *op)
        {
            const EntryArgs args(ctx);
            script::run(rdram, ctx, runtime,
                        std::string("{foreach $b (song song_coop song_vs) {if {find_exists $syscfg songs {game get_song} $b name}"
                                    " {set_elem {find $syscfg songs {game get_song} $b name} 1 {song_speed ") +
                            op + " {elem {find $syscfg songs {game get_song} $b name} 1}}}}}");
            args.restore(ctx);
        }

        // BeatMatch reads its song's audio at the speed, practice's with
        // the played part alone (ghc_practice_solo, ui/dta/game_settings.dta),
        // and leaves the song as it was for the menus' previews. Only
        // quickplay, practice and multiplayer play at a speed: a career or
        // tutorial song reached past the menus that set it plays at 100%.
        void onBeatMatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            {
                const EntryArgs args(ctx);
                script::run(rdram, ctx, runtime,
                            "{song_speed practice {== {gamecfg get mode} practice}}"
                            "{if {! {find_elem (quickplay practice multi_coop multi_fo multi_vs) {gamecfg get mode}}}"
                            " {song_speed set 100}}");
                args.restore(ctx);
            }
            rename(rdram, ctx, runtime, "name");
            const EntryArgs args(ctx);
            script::run(rdram, ctx, runtime, "{if {== {gamecfg get mode} practice} {ghc_practice_solo}}");
            args.restore(ctx);
        }
        void onBeatMatchGone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            rename(rdram, ctx, runtime, "base");
            s_beatMatch = 0u;
            const EntryArgs args(ctx);
            script::run(rdram, ctx, runtime, "{song_speed unsolo} {set $ghc_practice_solo FALSE}");
            args.restore(ctx);
        }

        // A DataArray: nodes at +0, their count at +8; a node is a value and
        // a type, a symbol's value its text.
        uint32_t nodeAt(uint8_t *rdram, uint32_t array, int i) { return load<uint32_t>(rdram, array) + 8u * uint32_t(i); }
        int sizeOf(uint8_t *rdram, uint32_t array) { return load<int16_t>(rdram, array + 8u); }
        std::string symbolAt(uint8_t *rdram, uint32_t node)
        {
            if (load<uint32_t>(rdram, node + 4u) != script::kSymbol)
                return "";
            return reinterpret_cast<const char *>(getMemPtr(rdram, load<uint32_t>(rdram, node)));
        }
        // The (key ...) in an array, 0 if none.
        uint32_t entry(uint8_t *rdram, uint32_t array, const std::string &key)
        {
            for (int i = 0; i < sizeOf(rdram, array); ++i)
            {
                const uint32_t node = nodeAt(rdram, array, i);
                if (load<uint32_t>(rdram, node + 4u) == script::kArray &&
                    symbolAt(rdram, nodeAt(rdram, load<uint32_t>(rdram, node), 0)) == key)
                    return load<uint32_t>(rdram, node);
            }
            return 0u;
        }

        // The vols put at silence, each node as it was.
        std::vector<std::array<uint32_t, 3>> s_muted;

        // A song block's channels not the played part's put at -96 dB: the
        // guitar's, else bass or rhythm's, never a drum track a hack's song
        // has. TRUE if the part has channels of its own; with `apply`
        // false, nothing is muted.
        bool solo(uint8_t *rdram, uint32_t block, bool guitar, bool apply)
        {
            // (tracks ((guitar (2 3)) (bass 4))), (vols (-3.5 ...)): each's list.
            const auto list = [&](const char *key) -> uint32_t
            {
                const uint32_t e = entry(rdram, block, key);
                return e && sizeOf(rdram, e) > 1 && load<uint32_t>(rdram, nodeAt(rdram, e, 1) + 4u) == script::kArray
                           ? load<uint32_t>(rdram, nodeAt(rdram, e, 1))
                           : 0u;
            };
            const uint32_t tracks = list("tracks"), vols = list("vols");
            if (!tracks || !vols)
                return false;
            std::vector<bool> mine(16, false);
            bool any = false;
            for (int t = 0; t < sizeOf(rdram, tracks); ++t)
            {
                const uint32_t track = load<uint32_t>(rdram, nodeAt(rdram, tracks, t));
                const std::string name = symbolAt(rdram, nodeAt(rdram, track, 0));
                if ((guitar ? name != "guitar" : name != "bass" && name != "rhythm") || sizeOf(rdram, track) < 2)
                    continue;
                const uint32_t channels = nodeAt(rdram, track, 1);
                const bool many = load<uint32_t>(rdram, channels + 4u) == script::kArray;
                const uint32_t array = load<uint32_t>(rdram, channels);
                for (int c = 0; c < (many ? sizeOf(rdram, array) : 1); ++c)
                {
                    const uint32_t channel = load<uint32_t>(rdram, many ? nodeAt(rdram, array, c) : channels);
                    if (channel < mine.size())
                        mine[channel] = any = true;
                }
            }
            if (!any || !apply)
                return any;
            for (int c = 0; c < sizeOf(rdram, vols); ++c)
                if (static_cast<size_t>(c) >= mine.size() || !mine[c])
                {
                    const uint32_t node = nodeAt(rdram, vols, c);
                    s_muted.push_back({node, load<uint32_t>(rdram, node), load<uint32_t>(rdram, node + 4u)});
                    store<uint32_t>(rdram, node, script::floatNode(-96.0f).value);
                    store<uint32_t>(rdram, node + 4u, script::kFloat);
                }
            return true;
        }

        // {song_speed}: the percent. {song_speed set <percent>}; live: the
        // same, and the song playing in practice goes to it as it plays.
        // {song_speed name <audio name>}: that name at the speed; base: as
        // it was. {song_speed solo <song block> <guitar>}: only the played
        // part heard (solo), TRUE if it can be; soloable: TRUE if it can
        // be, nothing muted; unsolo: all of it again.
        // {song_speed practice <bool>}: whether the song made next plays in
        // practice (its BeatMatch's constructor). {song_speed playing}: the
        // speed the song in play plays at (playing()).
        script::Node command(const script::Call &call)
        {
            const std::string op = call.size() > 1 ? call.symbol(1) : "";
            if (op == "set" || op == "live")
                s_percent = std::clamp(static_cast<int>(call.number(2)), kSlowest, kFastest);
            if (op == "practice")
                s_practice = call.number(2) != 0.0f;
            if (op == "playing")
                return {static_cast<uint32_t>(playing()), script::kInt};
            if (op == "live" && s_practice && s_beatMatch != 0u && s_percent != s_playing)
                playAtSpeed(call.rdram, call.ctx, call.runtime);
            if (op == "solo" || op == "soloable")
            {
                const script::Node block = call.arg(2);
                return {block.type == script::kArray && solo(call.rdram, block.value, call.number(3) != 0.0f, op == "solo"),
                        script::kInt};
            }
            if (op == "unsolo")
            {
                for (const auto &[node, value, type] : s_muted)
                {
                    store<uint32_t>(call.rdram, node, value);
                    store<uint32_t>(call.rdram, node + 4u, type);
                }
                s_muted.clear();
            }
            if (op == "name" || op == "base")
            {
                std::string name = call.text(2);
                if (name.rfind(kPrefix, 0) == 0)
                    name = name.substr(name.find('/', sizeof kPrefix - 1u) + 1u);
                // Pitch let go, the song's own file plays at the speed, like
                // a tape. Practice's is made at 50%, its pitch put back in
                // the mix at whatever speed it plays.
                const bool tape = !settings::get(settings::kMaintainPitch);
                synth::keepPitch(op == "name" && s_practice && !tape ? kPracticeBase : 0);
                if (op == "base" || tape || (s_percent == 100 && !s_practice))
                    return call.word(name);
                return call.word(kPrefix + std::to_string(s_practice ? kPracticeBase : s_percent) + "/" + name);
            }
            return {static_cast<uint32_t>(s_percent), script::kInt};
        }

        struct CtorTag;
        struct DtorTag;
    }

    int percent() { return s_percent; }
    int playing() { return s_beatMatch != 0u ? s_playing : s_percent; }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        replaceKeeping(runtime, addresses.beatMatchRestart, &restart, s_restart);
        replaceKeeping(runtime, addresses.beatMatchSetPracticeSpeed, &setPracticeSpeed, s_setPracticeSpeed);
        EntryHook<CtorTag>::install(runtime, addresses.beatMatchCtor, onBeatMatch);
        EntryHook<DtorTag>::install(runtime, addresses.beatMatchDtor, onBeatMatchGone);
        script::addCommand("song_speed", command);
        ark::addMaker(kPrefix, [](const std::string &path) -> std::optional<ark::Made>
                      {
                          const size_t slash = path.find('/', sizeof kPrefix - 1u);
                          if (slash == std::string::npos)
                              return std::nullopt;
                          const int percent = std::atoi(path.c_str() + sizeof kPrefix - 1u);
                          const std::string source = path.substr(slash + 1u);
                          if (percent < kSlowest || percent > kFastest || !ark::open(source))
                          {
                              std::cerr << "[song_speed] no " << path << std::endl;
                              return std::nullopt;
                          }
                          return vgs::made(path, [source, percent]() -> std::unique_ptr<vgs::Pcm>
                                           {
                                               auto file = ark::open(source);
                                               auto decoded = file ? Decoded::open(std::move(*file)) : nullptr;
                                               if (!decoded)
                                                   return nullptr;
                                               return std::make_unique<Stretched>(std::move(decoded), percent / 100.0);
                                           });
                      });
    }
}
