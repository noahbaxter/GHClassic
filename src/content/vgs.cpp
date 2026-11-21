// A .vgs is "VgS!", version 2, then each of 15 channels' sample rate and
// block count, 0x80 bytes in all. PS-ADPCM blocks follow in rounds of one
// per channel, byte 1 of each its channel; each channel's last block is
// silent with byte 1 0x80 | channel.
//
// A song's vgs is encoded a round at a time from wherever it is read,
// carrying on from the last read when the next follows it, so the decoder's
// history matches across reads; a jump starts afresh from that sample. What
// follows a jump is kept a while, so a jump back there (practice's section
// again) has it at once, as the game reads it all again before it plays.
//
// The game thread reads it, and waits for its read. So a thread of its own
// encodes the song last read ahead of its reads, a step at a time: a read
// finds its rounds done, and waits at most for that thread's one step.

#include "content/vgs.h"

#include "disc/ark.h"
#include "synth/adpcm.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

namespace gh2::vgs
{
    namespace
    {
        constexpr uint32_t kHeaderBytes = 0x80u;
        constexpr uint32_t kChannelSlots = 15u;
        constexpr uint32_t kRoundsAtOnce = 512u;   // decoded per step
        constexpr uint32_t kFollowRounds = 4096u;  // a read this far ahead decodes through
        constexpr size_t kOpenSongs = 4u;          // preview, song, the song a speed's is made from, one spare
        constexpr uint32_t kKeptRounds = 16384u;   // after a jump, kept: more than the game reads before it plays
        constexpr size_t kKeptJumps = 3u;          // the file's start, a section's, one spare

        struct Source
        {
            std::string path;
            std::function<std::unique_ptr<Pcm>()> open;
        };

        // One song open for encoding, and the rounds last encoded.
        struct Stream
        {
            std::unique_ptr<Pcm> pcm;
            uint32_t rounds = 0u; // blocks per channel, its silent last one too
            uint32_t next = 0u;   // the round the decoder is at
            uint32_t ahead = 0u;  // encoded ahead of the reads up to here
            std::vector<synth::AdpcmState> states;
            uint32_t windowStart = 0u; // the first round in window
            std::vector<uint8_t> window;
            // From each of the last jumps on, the rounds encoded, up to
            // kKeptRounds, and the encoder as it was after them. The newest
            // first; the one being added to, if any.
            struct Kept
            {
                uint32_t from = 0u;
                std::vector<uint8_t> bytes;
                std::vector<synth::AdpcmState> states;
            };
            std::list<Kept> kept;
            Kept *keeping = nullptr;

            uint32_t channels() const { return pcm->channels; }
            uint32_t roundBytes() const { return channels() * synth::kAdpcmBlockBytes; }
        };

        // Most recently used first.
        std::list<std::pair<const Source *, std::unique_ptr<Stream>>> s_open;
        // Over s_open and every stream in it; recursive, as a speed's stream
        // reads the song's own vgs inside its read (song_speed.cpp). A read
        // about to take it is counted, so the thread encoding ahead lets it
        // in between steps.
        std::recursive_mutex s_mutex;
        std::condition_variable_any s_wake;
        std::atomic<int> s_reading{0};
        // The stream the game read last, not one read inside a read nor by
        // the thread encoding ahead.
        const Stream *s_lastRead = nullptr;
        thread_local bool t_inner = false;

        std::unique_ptr<Stream> open(const Source &source)
        {
            auto pcm = source.open();
            if (!pcm)
                return nullptr;
            if (pcm->channels < 1u || pcm->channels > kChannelSlots || pcm->rate == 0u)
            {
                std::cerr << "[vgs] " << source.path << ": " << pcm->channels << " channels at " << pcm->rate << std::endl;
                return nullptr;
            }
            auto stream = std::make_unique<Stream>();
            stream->rounds = static_cast<uint32_t>((pcm->frames + synth::kAdpcmBlockSamples - 1u) / synth::kAdpcmBlockSamples + 1u);
            stream->states.assign(pcm->channels, {});
            stream->pcm = std::move(pcm);
            return stream;
        }

        Stream *opened(const Source &source)
        {
            for (auto it = s_open.begin(); it != s_open.end(); ++it)
                if (it->first == &source)
                {
                    s_open.splice(s_open.begin(), s_open, it);
                    return s_open.front().second.get();
                }
            auto stream = open(source);
            if (!stream)
                return nullptr;
            s_open.emplace_front(&source, std::move(stream));
            if (s_open.size() > kOpenSongs)
                s_open.pop_back();
            return s_open.front().second.get();
        }

        // Encodes up to kRoundsAtOnce rounds onto the window. The same steps
        // ahead as on a read, as a speed's stretch gives other samples for
        // input cut up otherwise.
        void encode(Stream &s)
        {
            const uint32_t count = std::min(kRoundsAtOnce, s.rounds - s.next);
            const uint32_t frames = count * synth::kAdpcmBlockSamples;
            std::vector<std::vector<float>> decoded(s.channels(), std::vector<float>(frames, 0.0f));
            std::vector<float *> to(s.channels());
            for (uint32_t c = 0; c < s.channels(); ++c)
                to[c] = decoded[c].data();
            const uint32_t got = s.pcm->fill(to.data(), frames);
            std::vector<std::vector<int16_t>> pcm(s.channels(), std::vector<int16_t>(frames, 0));
            for (uint32_t c = 0; c < s.channels(); ++c)
                for (uint32_t i = 0; i < got; ++i)
                    pcm[c][i] = static_cast<int16_t>(std::clamp(decoded[c][i] * 32768.0f, -32768.0f, 32767.0f));
            const size_t from = s.window.size();
            s.window.resize(from + size_t(count) * s.roundBytes());
            uint8_t *out = s.window.data() + from;
            for (uint32_t r = 0; r < count; ++r)
                for (uint32_t c = 0; c < s.channels(); ++c, out += synth::kAdpcmBlockBytes)
                {
                    if (s.next + r + 1u == s.rounds)
                    {
                        std::memset(out, 0, synth::kAdpcmBlockBytes);
                        out[1] = static_cast<uint8_t>(0x80u | c);
                        continue;
                    }
                    synth::encodeAdpcm(pcm[c].data() + r * synth::kAdpcmBlockSamples, s.states[c],
                                       static_cast<uint8_t>(c), out);
                }
            if (s.keeping)
            {
                s.keeping->bytes.insert(s.keeping->bytes.end(), s.window.begin() + static_cast<std::ptrdiff_t>(from),
                                        s.window.end());
                s.keeping->states = s.states;
                if (s.keeping->bytes.size() >= size_t(kKeptRounds) * s.roundBytes())
                    s.keeping = nullptr;
            }
            s.next += count;
        }

        // Rounds [first, last] encoded and in the window.
        void cover(Stream &s, uint32_t first, uint32_t last)
        {
            if (first < s.windowStart || first > s.next + kFollowRounds)
            {
                const auto at = std::find_if(s.kept.begin(), s.kept.end(),
                                             [&](const Stream::Kept &k) { return k.from == first && !k.bytes.empty(); });
                if (at != s.kept.end())
                {
                    // Back where a jump went before: what was kept, then on
                    // from its end as it was encoded, kept on with it.
                    s.kept.splice(s.kept.begin(), s.kept, at);
                    s.keeping = &s.kept.front();
                    s.window = s.keeping->bytes;
                    s.windowStart = first;
                    s.next = first + static_cast<uint32_t>(s.window.size() / s.roundBytes());
                    s.states = s.keeping->states;
                    s.pcm->seek(uint64_t(s.next) * synth::kAdpcmBlockSamples);
                    if (s.window.size() >= size_t(kKeptRounds) * s.roundBytes())
                        s.keeping = nullptr;
                }
                else
                {
                    s.pcm->seek(uint64_t(first) * synth::kAdpcmBlockSamples);
                    s.states.assign(s.channels(), {});
                    s.next = s.windowStart = first;
                    s.window.clear();
                    s.kept.remove_if([&](const Stream::Kept &k) { return k.from == first; });
                    s.kept.push_front(Stream::Kept{first, {}, {}});
                    s.keeping = &s.kept.front();
                    if (s.kept.size() > kKeptJumps)
                        s.kept.pop_back();
                }
            }
            // Keep from `first` on, so a reread a little back still finds
            // what it read.
            if (first > s.windowStart)
            {
                const size_t drop = std::min<size_t>(size_t(first - s.windowStart) * s.roundBytes(), s.window.size());
                s.window.erase(s.window.begin(), s.window.begin() + static_cast<std::ptrdiff_t>(drop));
                s.windowStart += static_cast<uint32_t>(drop / s.roundBytes());
            }
            while (s.next <= last && s.next < s.rounds)
                encode(s);
        }

        // The song last read, encoded on ahead of its reads.
        void encodeAhead()
        {
            t_inner = true;
            std::unique_lock lock(s_mutex);
            for (;;)
            {
                Stream *s = nullptr;
                for (auto &open : s_open)
                    if (open.second.get() == s_lastRead)
                        s = open.second.get();
                if (!s || s->next >= std::min(s->ahead, s->rounds))
                {
                    s_wake.wait(lock);
                    continue;
                }
                encode(*s);
                lock.unlock();
                while (s_reading.load() > 0)
                    std::this_thread::yield();
                lock.lock();
            }
        }

        // Before any other use of s_open.
        std::unique_lock<std::recursive_mutex> lockStreams()
        {
            static const bool started = (std::thread(encodeAhead).detach(), true);
            (void)started;
            ++s_reading;
            std::unique_lock lock(s_mutex);
            --s_reading;
            return lock;
        }

        size_t read(const Source &source, uint64_t offset, uint8_t *dst, size_t size)
        {
            const auto lock = lockStreams();
            Stream *s = opened(source);
            if (!s)
                return 0u;
            const uint64_t length = kHeaderBytes + uint64_t(s->rounds) * s->roundBytes();
            size = static_cast<size_t>(std::min<uint64_t>(size, offset < length ? length - offset : 0u));
            size_t done = 0u;
            if (offset < kHeaderBytes && size)
            {
                uint8_t header[kHeaderBytes] = {'V', 'g', 'S', '!', 2u};
                for (uint32_t c = 0; c < s->channels(); ++c)
                {
                    std::memcpy(header + 8u + 8u * c, &s->pcm->rate, 4u);
                    std::memcpy(header + 12u + 8u * c, &s->rounds, 4u);
                }
                done = std::min<size_t>(size, kHeaderBytes - offset);
                std::memcpy(dst, header + offset, done);
            }
            if (done == size)
                return done;
            const uint64_t body = offset + done - kHeaderBytes;
            const uint32_t first = static_cast<uint32_t>(body / s->roundBytes());
            const uint32_t last = static_cast<uint32_t>((body + (size - done) - 1u) / s->roundBytes());
            const bool outer = !t_inner;
            t_inner = true;
            cover(*s, first, last);
            t_inner = !outer;
            s->ahead = last + kFollowRounds;
            if (outer)
            {
                s_lastRead = s;
                s_wake.notify_one();
            }
            const size_t at = static_cast<size_t>(body - uint64_t(s->windowStart) * s->roundBytes());
            const size_t n = std::min(size - done, s->window.size() - std::min(at, s->window.size()));
            std::memcpy(dst + done, s->window.data() + at, n);
            return done + n;
        }
    }

    std::optional<ark::Made> made(const std::string &path, std::function<std::unique_ptr<Pcm>()> open)
    {
        static std::deque<Source> s_sources; // the reader and open list point at them
        const auto lock = lockStreams();
        // One a path, so a file opened again (a section gone round) finds
        // its song still open, and what it kept; opened afresh (reopen) with
        // the newest way to open it.
        const auto known = std::find_if(s_sources.begin(), s_sources.end(), [&](const Source &s) { return s.path == path; });
        Source *source = nullptr;
        if (known == s_sources.end())
        {
            source = &s_sources.emplace_back(Source{path, std::move(open)});
        }
        else
        {
            source = &*known;
            source->open = std::move(open);
        }
        Stream *s = opened(*source);
        if (!s)
            return std::nullopt;
        const uint32_t size = kHeaderBytes + s->rounds * s->roundBytes();
        return ark::Made{size, [source](uint64_t offset, uint8_t *dst, size_t n) { return read(*source, offset, dst, n); }};
    }

    void reopen(const std::string &path)
    {
        const auto lock = lockStreams();
        s_open.remove_if([&](const auto &open) { return open.first->path == path; });
    }

    void serve(const std::string &path, std::function<std::unique_ptr<Pcm>()> open)
    {
        ark::addMade(path, [path, open = std::move(open)] { return made(path, open); });
    }
}
