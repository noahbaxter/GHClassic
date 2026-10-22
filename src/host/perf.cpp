#include "host/perf.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace gh2::perf
{
    namespace
    {
        bool s_logging = false;

        // A 5x7 face, each row's five pixels in its low bits, the left one
        // highest.
        struct Glyph
        {
            char c;
            uint8_t rows[7];
        };
        constexpr Glyph kGlyphs[] = {
            {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
            {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, {'3', {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}},
            {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
            {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}}, {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
            {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}, {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
            {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}, {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}},
            {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}}, {'D', {0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c}},
            {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}}, {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
            {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}}, {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
            {'I', {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c}},
            {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
            {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
            {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
            {'Q', {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}}, {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
            {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}}, {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
            {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}},
            {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}}, {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
            {'Y', {0x11, 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04}}, {'Z', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}},
            {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}}, {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
        };

        // The panel before it is scaled: a letter's cell, a line's, the
        // graph's height, and the room round it all.
        constexpr uint32_t kCellW = 6, kLineH = 9, kGraphH = 32, kPad = 4;
        constexpr uint32_t kChars = 26;
        constexpr uint32_t kWidth = kPad * 2 + kChars * kCellW;
        // The graph's top is this many milliseconds: two frames at 60 a
        // second, with a line across at one.
        constexpr float kGraphMs = 1000.0f / 30.0f;

        constexpr uint8_t kBack[3] = {16, 16, 16}, kText[3] = {255, 255, 255}, kLine[3] = {90, 90, 90};
        constexpr uint8_t kGood[3] = {80, 220, 100}, kLate[3] = {240, 200, 60}, kBad[3] = {240, 70, 60};

        void put(std::vector<uint8_t> &into, uint32_t x, uint32_t y, const uint8_t colour[3])
        {
            std::memcpy(&into[(size_t(y) * kWidth + x) * 3u], colour, 3);
        }

        void text(std::vector<uint8_t> &into, uint32_t line, const char *words)
        {
            for (uint32_t i = 0; words[i] != '\0' && i < kChars; ++i)
                for (const Glyph &glyph : kGlyphs)
                    if (glyph.c == std::toupper(static_cast<unsigned char>(words[i])))
                        for (uint32_t y = 0; y < 7u; ++y)
                            for (uint32_t x = 0; x < 5u; ++x)
                                if (glyph.rows[y] & (0x10u >> x))
                                    put(into, kPad + i * kCellW + x, kPad + line * kLineH + y, kText);
        }

        float percentile(std::vector<float> values, double of)
        {
            if (values.empty())
                return 0.0f;
            const size_t at = std::min(values.size() - 1, static_cast<size_t>(of * values.size()));
            std::nth_element(values.begin(), values.begin() + at, values.end());
            return values[at];
        }
    }

    void logThisRun()
    {
        s_logging = true;
    }

    bool logging()
    {
        return s_logging;
    }

    void Times::add(bool isNew, float waitMs, float recordMs, const SceneStats &scene)
    {
        const auto now = std::chrono::steady_clock::now();
        if (isNew)
        {
            if (s_logging && m_lastNew != std::chrono::steady_clock::time_point{})
                m_allHeld.push_back(std::chrono::duration<float, std::milli>(now - m_lastNew).count());
            m_lastNew = now;
        }
        const float ms = m_frames == 0 ? 0.0f : std::chrono::duration<float, std::milli>(now - m_last).count();
        m_last = now;
        if (m_frames++ == 0)
            return;
        m_recent[m_at] = ms;
        m_at = (m_at + 1) % kRecent;
        m_wait = waitMs;
        m_record = recordMs;
        std::copy(std::begin(scene.gpuMs), std::end(scene.gpuMs), m_gpu);
        if (m_frames % 15u == 0)
        {
            // The last half second's mean, and the ring's worst.
            float sum = 0.0f;
            for (size_t i = 1; i <= 30; ++i)
                sum += m_recent[(m_at + kRecent - i) % kRecent];
            m_shownMs = sum / 30.0f;
            m_shownWorst = *std::max_element(std::begin(m_recent), std::end(m_recent));
            m_shownWait = m_wait;
            m_shownRecord = m_record;
            std::copy(std::begin(m_gpu), std::end(m_gpu), m_shownGpu);
        }
        if (s_logging)
        {
            m_all.push_back(ms);
            m_allWait.push_back(waitMs);
            m_allRecord.push_back(recordMs);
            for (size_t i = 0; i < std::size(kGpuSpans); ++i)
                m_allGpu[i].push_back(scene.gpuMs[i]);
        }
    }

    const std::vector<uint8_t> &Times::draw(int level, uint32_t scale, uint32_t &width, uint32_t &height)
    {
        m_pixels.clear();
        width = height = 0;
        if (level <= kOff)
            return m_pixels;
        // Two of the GPU's passes to a line.
        const uint32_t lines = level == kFps ? 1u : level == kGraph ? 2u : 4u + (std::size(kGpuSpans) + 1u) / 2u;
        const uint32_t graphTop = kPad + lines * kLineH;
        const uint32_t small = graphTop + (level >= kGraph ? kGraphH + kPad : kPad - 2u);
        m_small.resize(size_t(kWidth) * small * 3u);
        for (size_t i = 0; i < m_small.size(); i += 3)
            std::memcpy(&m_small[i], kBack, 3);

        char words[48];
        std::snprintf(words, sizeof(words), "%3.0f FPS      FRAME %5.1fMS", m_shownMs > 0.0f ? 1000.0f / m_shownMs : 0.0f,
                      m_shownMs);
        text(m_small, 0, words);
        if (level >= kGraph)
        {
            // What the frame took to draw, of what the display gave it.
            float cost = m_shownRecord;
            for (float span : m_shownGpu)
                cost += span;
            std::snprintf(words, sizeof(words), "COST %4.1fMS  WORST %5.1fMS", cost, m_shownWorst);
            text(m_small, 1, words);
            for (uint32_t x = 0; x < kRecent; ++x)
            {
                const float ms = m_recent[(m_at + x) % kRecent];
                const uint32_t tall = std::min(kGraphH, static_cast<uint32_t>(ms / kGraphMs * kGraphH + 0.5f));
                const uint8_t *colour = ms > kGraphMs ? kBad : ms > kGraphMs * 0.625f ? kLate : kGood;
                for (uint32_t y = 0; y < tall; ++y)
                    put(m_small, kPad + x, graphTop + kGraphH - 1u - y, colour);
            }
            for (uint32_t x = 0; x < kRecent; x += 2u)
                put(m_small, kPad + x, graphTop + kGraphH / 2u, kLine);
        }
        if (level >= kFull)
        {
            float gpu = 0.0f;
            for (float span : m_shownGpu)
                gpu += span;
            std::snprintf(words, sizeof(words), "CPU  WAIT %4.1f  DRAWS %4.1f", m_shownWait, m_shownRecord);
            text(m_small, 2, words);
            std::snprintf(words, sizeof(words), "GPU  %4.1fMS IN ALL", gpu);
            text(m_small, 3, words);
            for (size_t i = 0; i < std::size(kGpuSpans); i += 2)
            {
                int used = std::snprintf(words, sizeof(words), "%-7.7s %4.1f", kGpuSpans[i], m_shownGpu[i]);
                if (i + 1 < std::size(kGpuSpans))
                    std::snprintf(words + used, sizeof(words) - used, "  %-7.7s %4.1f", kGpuSpans[i + 1], m_shownGpu[i + 1]);
                text(m_small, 4u + static_cast<uint32_t>(i / 2), words);
            }
        }

        width = kWidth * scale;
        height = small * scale;
        m_pixels.resize(size_t(width) * height * 4u);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t *from = &m_small[(size_t(y / scale) * kWidth + x / scale) * 3u];
                uint8_t *to = &m_pixels[(size_t(y) * width + x) * 4u];
                to[0] = from[0];
                to[1] = from[1];
                to[2] = from[2];
                to[3] = 255;
            }
        return m_pixels;
    }

    std::string Times::summary() const
    {
        if (m_all.empty())
            return "[perf] no frames\n";
        double total = 0.0;
        for (float ms : m_all)
            total += ms;
        const float median = percentile(m_all, 0.5);
        // A frame half as long again as most is one a player sees.
        const float late = median * 1.5f;
        char line[256];
        std::string out;
        std::snprintf(line, sizeof(line), "[perf] %zu frames in %.1f s, %.1f a second\n", m_all.size(), total / 1000.0,
                      m_all.size() * 1000.0 / total);
        out += line;
        std::snprintf(line, sizeof(line), "[perf] frame ms: median %.2f, 99th %.2f, worst %.2f, %td over %.1f\n", median,
                      percentile(m_all, 0.99), *std::max_element(m_all.begin(), m_all.end()),
                      std::count_if(m_all.begin(), m_all.end(), [late](float ms) { return ms > late; }), late);
        out += line;
        if (!m_allHeld.empty())
        {
            // 50 ms frozen is seen at any frame rate.
            constexpr float kFrozenMs = 50.0f;
            std::snprintf(line, sizeof(line),
                          "[perf] game frame shown ms: median %.2f, 99th %.2f, longest %.2f, %td over %.0f\n",
                          percentile(m_allHeld, 0.5), percentile(m_allHeld, 0.99),
                          *std::max_element(m_allHeld.begin(), m_allHeld.end()),
                          std::count_if(m_allHeld.begin(), m_allHeld.end(),
                                        [](float ms) { return ms > kFrozenMs; }),
                          kFrozenMs);
            out += line;
        }
        std::snprintf(line, sizeof(line), "[perf] host ms, median: wait for game's frame %.2f, record draws %.2f\n",
                      percentile(m_allWait, 0.5), percentile(m_allRecord, 0.5));
        out += line;
        out += "[perf] gpu ms, median:";
        float gpu = 0.0f;
        for (size_t i = 0; i < std::size(kGpuSpans); ++i)
        {
            const float span = percentile(m_allGpu[i], 0.5);
            gpu += span;
            std::snprintf(line, sizeof(line), " %s %.2f", kGpuSpans[i], span);
            out += line;
        }
        std::snprintf(line, sizeof(line), ", all %.2f\n", gpu);
        return out + line;
    }
}
