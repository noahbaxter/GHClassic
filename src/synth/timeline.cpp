#include "synth/timeline.h"

#include "synth/spu.h"

#include <algorithm>
#include <cmath>

namespace gh2::synth
{
    int Timeline::addSound(const std::vector<int16_t> &pcm, uint32_t rate)
    {
        // Brought to the mix's rate once, linearly.
        std::vector<float> out;
        if (!pcm.empty() && rate != 0u)
        {
            const double step = double(rate) / Spu::kRate;
            const size_t frames = static_cast<size_t>(double(pcm.size()) / step);
            out.resize(frames);
            for (size_t i = 0; i < frames; ++i)
            {
                const double at = double(i) * step;
                const size_t a = static_cast<size_t>(at);
                const size_t b = std::min(a + 1u, pcm.size() - 1u);
                const double t = at - double(a);
                out[i] = static_cast<float>((pcm[a] * (1.0 - t) + pcm[b] * t) / 32768.0);
            }
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sounds.push_back(std::move(out));
        return static_cast<int>(m_sounds.size() - 1u);
    }

    void Timeline::setClock(std::optional<Clock> clock)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_clock = clock;
    }

    std::optional<Timeline::Clock> Timeline::clock() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_clock;
    }

    void Timeline::schedule(double ms, int sound, float gain, float pan)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Event e{ms, sound, gain * std::min(1.0f, 1.0f - pan), gain * std::min(1.0f, 1.0f + pan)};
        m_events.insert(std::upper_bound(m_events.begin(), m_events.end(), e,
                                         [](const Event &a, const Event &b) { return a.ms < b.ms; }),
                        e);
    }

    void Timeline::clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_events.clear();
    }

    uint64_t Timeline::started() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_started;
    }

    void Timeline::mix(float *out, size_t frames, double ms, double msPerFrame)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Those due in these frames start at their own.
        std::vector<std::pair<size_t, Playing>> starting;
        if (msPerFrame > 0.0)
        {
            const double end = ms + msPerFrame * double(frames);
            size_t taken = 0u;
            for (const Event &e : m_events)
            {
                if (e.ms >= end)
                    break;
                ++taken;
                if (e.ms < ms - kLateMs || e.sound < 0 || e.sound >= static_cast<int>(m_sounds.size()))
                    continue;
                const size_t at = e.ms <= ms ? 0u : static_cast<size_t>(std::floor((e.ms - ms) / msPerFrame));
                starting.push_back({at, Playing{e.sound, 0u, e.left, e.right}});
            }
            m_events.erase(m_events.begin(), m_events.begin() + static_cast<std::ptrdiff_t>(taken));
        }

        const auto play = [&](Playing &p, size_t from) {
            const std::vector<float> &s = m_sounds[static_cast<size_t>(p.sound)];
            for (size_t f = from; f < frames && p.at < s.size(); ++f, ++p.at)
            {
                out[f * 2u] += s[p.at] * p.left;
                out[f * 2u + 1u] += s[p.at] * p.right;
            }
        };
        for (Playing &p : m_playing)
            play(p, 0u);
        for (auto &[at, p] : starting)
        {
            play(p, at);
            m_playing.push_back(p);
            ++m_started;
        }
        std::erase_if(m_playing, [&](const Playing &p) { return p.at >= m_sounds[static_cast<size_t>(p.sound)].size(); });
    }
}
