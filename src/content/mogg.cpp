// A .vgs is "VgS!", version 2, then each of 15 channels' sample rate and
// block count, 0x80 bytes in all. PS-ADPCM blocks follow in rounds of one
// per channel, byte 1 of each its channel; each channel's last block is
// silent with byte 1 0x80 | channel. A .mogg is a version word (10 is
// unencrypted), the Ogg stream's offset, then the stream.
//
// A song's vgs is encoded a round at a time from wherever it is read,
// carrying on from the last read when the next follows it, so the decoder's
// history matches across reads; a jump starts afresh from that sample.

#include "content/mogg.h"

#include "disc/ark.h"
#include "synth/adpcm.h"

#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <list>
#include <memory>
#include <vector>

namespace gh2::mogg
{
    namespace
    {
        constexpr uint32_t kHeaderBytes = 0x80u;
        constexpr uint32_t kChannelSlots = 15u;
        constexpr uint32_t kRoundsAtOnce = 512u;   // decoded per step
        constexpr uint32_t kFollowRounds = 4096u;  // a read this far ahead decodes through
        constexpr size_t kOpenSongs = 3u;          // preview, song, one spare

        struct Source
        {
            size_t disc;
            std::string mogg;
        };

        // One song open for decoding: its Ogg bytes, the decoder, and the
        // rounds last encoded. Held in place: the decoder reads through it.
        struct Stream
        {
            std::vector<uint8_t> file;
            size_t at = 0u; // the decoder's read position in file
            OggVorbis_File vorbis{};
            bool decoding = false;
            uint32_t channels = 0u;
            uint32_t rate = 0u;
            uint32_t rounds = 0u; // blocks per channel, its silent last one too
            uint32_t next = 0u;   // the round the decoder is at
            std::vector<synth::AdpcmState> states;
            uint32_t windowStart = 0u; // the first round in window
            std::vector<uint8_t> window;

            ~Stream()
            {
                if (decoding)
                    ov_clear(&vorbis);
            }

            uint32_t roundBytes() const { return channels * synth::kAdpcmBlockBytes; }
        };

        // vorbisfile's reads, from the stream's bytes in memory.
        const ov_callbacks kFromMemory = {
            [](void *to, size_t size, size_t count, void *from) -> size_t
            {
                Stream &s = *static_cast<Stream *>(from);
                const size_t n = size ? std::min(count, (s.file.size() - s.at) / size) : 0u;
                std::memcpy(to, s.file.data() + s.at, n * size);
                s.at += n * size;
                return n;
            },
            [](void *from, ogg_int64_t offset, int whence) -> int
            {
                Stream &s = *static_cast<Stream *>(from);
                const int64_t base = whence == SEEK_CUR ? int64_t(s.at) : whence == SEEK_END ? int64_t(s.file.size()) : 0;
                if (base + offset < 0 || base + offset > int64_t(s.file.size()))
                    return -1;
                s.at = static_cast<size_t>(base + offset);
                return 0;
            },
            nullptr,
            [](void *from) -> long { return static_cast<long>(static_cast<Stream *>(from)->at); },
        };

        // Most recently used first.
        std::list<std::pair<const Source *, std::unique_ptr<Stream>>> s_open;

        std::unique_ptr<Stream> open(const Source &source)
        {
            auto stream = std::make_unique<Stream>();
            auto bytes = ark::readFile(source.disc, source.mogg);
            uint32_t version = 0u, start = 0u;
            if (bytes && bytes->size() >= 8u)
            {
                std::memcpy(&version, bytes->data(), 4u);
                std::memcpy(&start, bytes->data() + 4u, 4u);
            }
            if (!bytes || version != 10u || start >= bytes->size())
            {
                std::cerr << "[mogg] " << source.mogg << ": not an unencrypted mogg" << std::endl;
                return nullptr;
            }
            stream->file.assign(bytes->begin() + start, bytes->end());
            if (const int error = ov_open_callbacks(stream.get(), &stream->vorbis, nullptr, 0, kFromMemory))
            {
                std::cerr << "[mogg] " << source.mogg << ": Vorbis error " << error << std::endl;
                return nullptr;
            }
            stream->decoding = true;
            const vorbis_info *info = ov_info(&stream->vorbis, -1);
            if (!info || info->channels < 1 || static_cast<uint32_t>(info->channels) > kChannelSlots)
            {
                std::cerr << "[mogg] " << source.mogg << ": " << (info ? info->channels : 0) << " channels" << std::endl;
                return nullptr;
            }
            stream->channels = static_cast<uint32_t>(info->channels);
            stream->rate = static_cast<uint32_t>(info->rate);
            const uint64_t samples = static_cast<uint64_t>(std::max<ogg_int64_t>(ov_pcm_total(&stream->vorbis, -1), 0));
            stream->rounds = static_cast<uint32_t>((samples + synth::kAdpcmBlockSamples - 1u) / synth::kAdpcmBlockSamples + 1u);
            stream->states.assign(stream->channels, {});
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

        // Encodes up to kRoundsAtOnce rounds onto the window.
        void encode(Stream &s)
        {
            const uint32_t count = std::min(kRoundsAtOnce, s.rounds - s.next);
            const uint32_t frames = count * synth::kAdpcmBlockSamples;
            std::vector<std::vector<int16_t>> pcm(s.channels, std::vector<int16_t>(frames, 0));
            uint32_t got = 0u;
            while (got < frames)
            {
                float **decoded = nullptr;
                int section = 0;
                const long n = ov_read_float(&s.vorbis, &decoded, static_cast<int>(frames - got), &section);
                if (n <= 0)
                    break;
                for (uint32_t c = 0; c < s.channels; ++c)
                    for (long i = 0; i < n; ++i)
                        pcm[c][got + i] = static_cast<int16_t>(std::clamp(decoded[c][i] * 32768.0f, -32768.0f, 32767.0f));
                got += static_cast<uint32_t>(n);
            }
            const size_t from = s.window.size();
            s.window.resize(from + size_t(count) * s.roundBytes());
            uint8_t *out = s.window.data() + from;
            for (uint32_t r = 0; r < count; ++r)
                for (uint32_t c = 0; c < s.channels; ++c, out += synth::kAdpcmBlockBytes)
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
            s.next += count;
        }

        // Rounds [first, last] encoded and in the window.
        void cover(Stream &s, uint32_t first, uint32_t last)
        {
            if (first < s.windowStart || first > s.next + kFollowRounds)
            {
                ov_pcm_seek(&s.vorbis, ogg_int64_t(first) * synth::kAdpcmBlockSamples);
                s.states.assign(s.channels, {});
                s.next = s.windowStart = first;
                s.window.clear();
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

        size_t read(const Source &source, uint64_t offset, uint8_t *dst, size_t size)
        {
            Stream *s = opened(source);
            if (!s)
                return 0u;
            const uint64_t length = kHeaderBytes + uint64_t(s->rounds) * s->roundBytes();
            size = static_cast<size_t>(std::min<uint64_t>(size, offset < length ? length - offset : 0u));
            size_t done = 0u;
            if (offset < kHeaderBytes && size)
            {
                uint8_t header[kHeaderBytes] = {'V', 'g', 'S', '!', 2u};
                for (uint32_t c = 0; c < s->channels; ++c)
                {
                    std::memcpy(header + 8u + 8u * c, &s->rate, 4u);
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
            cover(*s, first, last);
            const size_t at = static_cast<size_t>(body - uint64_t(s->windowStart) * s->roundBytes());
            const size_t n = std::min(size - done, s->window.size() - std::min(at, s->window.size()));
            std::memcpy(dst + done, s->window.data() + at, n);
            return done + n;
        }
    }

    void serveAsVgs(const std::string &vgs, size_t disc, const std::string &mogg)
    {
        static std::deque<Source> s_sources; // the reader and open list point at them
        const Source *source = &s_sources.emplace_back(Source{disc, mogg});
        ark::addMade(vgs, [source]() -> std::optional<ark::Made>
                     {
                         Stream *s = opened(*source);
                         if (!s)
                             return std::nullopt;
                         const uint32_t size = kHeaderBytes + s->rounds * s->roundBytes();
                         return ark::Made{size, [source](uint64_t offset, uint8_t *dst, size_t n)
                                          { return read(*source, offset, dst, n); }};
                     });
    }
}
