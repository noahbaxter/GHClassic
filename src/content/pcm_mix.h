#pragma once

// Audio made of other audio (content/vgs.h's Pcm): one read at another
// rate, and several summed into pairs of channels.

#include "content/vgs.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace gh2::vgs
{
    // A stem at another rate than the mix's (Opus is always 48 kHz,
    // beside 44.1 kHz Vorbis or MP3), read between its frames by a
    // straight line.
    class Resampled final : public Pcm
    {
    public:
        Resampled(std::unique_ptr<Pcm> from, uint32_t to)
            : m_from(std::move(from)), m_step(double(m_from->rate) / to), m_held(m_from->channels)
        {
            channels = m_from->channels;
            right = m_from->right;
            rate = to;
            frames = static_cast<uint64_t>(double(m_from->frames) / m_step);
        }

        bool seek(uint64_t frame) override
        {
            m_out = frame;
            m_at = double(frame) * m_step;
            m_first = static_cast<uint64_t>(m_at);
            for (auto &c : m_held)
                c.clear();
            return m_from->seek(m_first);
        }

        uint32_t read(float *const *out, uint32_t most) override
        {
            most = static_cast<uint32_t>(std::min<uint64_t>(most, frames - std::min(m_out, frames)));
            if (most == 0u)
                return 0u;
            // The source frames behind let go, those up to the one after
            // the last asked for held.
            const size_t drop = std::min<size_t>(static_cast<uint64_t>(m_at) - m_first, m_held[0].size());
            for (auto &c : m_held)
                c.erase(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(drop));
            m_first += drop;
            const uint64_t last = static_cast<uint64_t>(m_at + (most - 1u) * m_step) + 1u;
            const size_t had = m_held[0].size();
            if (m_first + had <= last)
            {
                std::vector<float *> to(channels);
                for (uint32_t c = 0; c < channels; ++c)
                {
                    m_held[c].resize(had + (last + 1u - m_first - had));
                    to[c] = m_held[c].data() + had;
                }
                const uint32_t got = m_from->fill(to.data(), static_cast<uint32_t>(last + 1u - m_first - had));
                for (auto &c : m_held)
                    c.resize(had + got);
            }
            uint32_t i = 0u;
            for (; i < most; ++i, m_at += m_step)
            {
                const size_t a = static_cast<uint64_t>(m_at) - m_first;
                if (a >= m_held[0].size())
                    break;
                const size_t b = std::min(a + 1u, m_held[0].size() - 1u);
                const float t = static_cast<float>(m_at - std::floor(m_at));
                for (uint32_t c = 0; c < channels; ++c)
                    out[c][i] = m_held[c][a] + (m_held[c][b] - m_held[c][a]) * t;
            }
            m_out += i;
            return i;
        }

    private:
        std::unique_ptr<Pcm> m_from;
        double m_step;     // source frames an output frame
        double m_at = 0.0; // the source frame being read, between two
        std::vector<std::vector<float>> m_held;
        uint64_t m_first = 0u; // the source frame m_held starts at
        uint64_t m_out = 0u;   // the next frame out
    };

    // Every stem's channels summed into the mix's pairs, its first two
    // where it has more, a mono one on both sides.
    class Mixer final : public Pcm
    {
    public:
        struct Input
        {
            std::unique_ptr<Pcm> pcm;
            uint32_t pair = 0u;
            bool done = false;
        };

        std::vector<Input> inputs;
        int64_t leadFrames = 0; // the stems' frame the mix's first is; under 0, silence first

        bool seek(uint64_t frame) override
        {
            at = static_cast<int64_t>(frame);
            const int64_t source = std::max<int64_t>(at + leadFrames, 0);
            for (Input &in : inputs)
            {
                in.done = source >= static_cast<int64_t>(in.pcm->frames);
                if (!in.done)
                    in.pcm->seek(static_cast<uint64_t>(source));
            }
            return true;
        }

        uint32_t read(float *const *out, uint32_t most) override
        {
            if (at >= static_cast<int64_t>(frames))
                return 0u;
            most = static_cast<uint32_t>(std::min<int64_t>(most, static_cast<int64_t>(frames) - at));
            for (uint32_t c = 0; c < channels; ++c)
                std::fill(out[c], out[c] + most, 0.0f);
            // Silence ahead of the audio, where it starts late.
            uint32_t silent = 0u;
            if (at + leadFrames < 0)
                silent = static_cast<uint32_t>(std::min<int64_t>(most, -(at + leadFrames)));
            const uint32_t want = most - silent;
            for (Input &in : inputs)
            {
                std::vector<std::vector<float>> buffer(in.pcm->channels, std::vector<float>(want));
                std::vector<float *> to(in.pcm->channels);
                for (uint32_t c = 0; c < in.pcm->channels; ++c)
                    to[c] = buffer[c].data();
                const uint32_t got = in.done ? 0u : in.pcm->fill(to.data(), want);
                in.done = got < want;
                const uint32_t left = in.pair * 2u;
                for (uint32_t i = 0; i < got; ++i)
                {
                    const float l = buffer[0][i], r = in.pcm->channels > 1u ? buffer[in.pcm->right][i] : l;
                    out[left][silent + i] += l;
                    out[left + 1u][silent + i] += r;
                }
            }
            at += most;
            return most;
        }

    private:
        int64_t at = 0; // the next frame out
    };
}
