#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace gh2::synth
{
    // One SPU2 core's reverb unit: the PS1 algorithm from psx-spx ("SPU
    // Reverb Formula"), one side per 48 kHz cycle, left then right, so each
    // side runs at 24 kHz. Its input and output go through the 39-tap
    // half-band filter PCSX2 uses (ReverbResample.cpp), which keeps the
    // rate change from folding or imaging anything above 12 kHz.
    class Reverb
    {
    public:
        // libsd's SD_REV_MODE_*: OFF, ROOM, STUDIO_A/B/C, HALL, SPACE, ECHO,
        // DELAY, PIPE. The depth is the core's EVOL per side.
        void setMode(uint32_t mode);
        void setDepth(int16_t left, int16_t right);

        bool active() const;

        // Adds the reverb of `send` into `out`, both interleaved stereo s16
        // scale.
        void process(const int32_t *send, int32_t *out, size_t frames);

    private:
        int32_t step(bool right, int32_t in);
        int16_t &at(int32_t offset);

        uint32_t m_mode = 0;
        int32_t m_depthL = 0;
        int32_t m_depthR = 0;
        std::vector<int16_t> m_buffer;
        uint32_t m_pos = 0;
        uint64_t m_cycle = 0;
        // Last 64 inputs and outputs per side, twice over so the filter
        // reads 39 in a row without wrapping.
        std::array<std::array<int16_t, 128>, 2> m_down{};
        std::array<std::array<int16_t, 128>, 2> m_up{};
        uint32_t m_bufPos = 0;
    };
}
