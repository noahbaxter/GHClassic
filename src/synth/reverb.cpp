#include "synth/reverb.h"

#include <algorithm>

namespace gh2::synth
{
    namespace
    {
        // psx-spx, "SPU Reverb Examples": the work area in bytes, then the
        // registers in port order
        //   dAPF1 dAPF2 vIIR vCOMB1 vCOMB2 vCOMB3 vCOMB4 vWALL
        //   vAPF1 vAPF2 mLSAME mRSAME mLCOMB1 mRCOMB1 mLCOMB2 mRCOMB2
        //   dLSAME dRSAME mLDIFF mRDIFF mLCOMB3 mRCOMB3 mLCOMB4 mRCOMB4
        //   dLDIFF dRDIFF mLAPF1 mRAPF1 mLAPF2 mRAPF2 vLIN vRIN
        // indexed by libsd mode. ROOM through HALL are psx-spx's first five;
        // SPACE is Space Echo, ECHO Chaos Echo, DELAY Delay, and PIPE is
        // taken to be Half Echo, the one example left over. GH2 sets only
        // STUDIO_C and ECHO.
        struct Preset
        {
            uint32_t bytes;
            uint16_t r[32];
        };
        constexpr Preset kPresets[10] = {
            {0x10, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1,
                    0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0}},
            {0x26C0, {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80,
                      0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
                      0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                      0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000}},
            {0x1F40, {0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00,
                      0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266,
                      0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7,
                      0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000}},
            {0x4840, {0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0,
                      0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
                      0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7,
                      0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000}},
            {0x6FE0, {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680,
                      0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973,
                      0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2,
                      0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000}},
            {0xADE0, {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000,
                      0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1,
                      0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD,
                      0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000}},
            {0xF6C0, {0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000,
                      0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2,
                      0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D,
                      0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000}},
            {0x18040, {0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100,
                       0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000,
                       0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                       0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000}},
            {0x18040, {0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000,
                       0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000,
                       0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                       0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000}},
            {0x3C00, {0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500,
                      0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7,
                      0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059,
                      0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000}},
        };

        // PCSX2's half-band filter (ReverbResample.cpp), for both directions;
        // upsampling doubles it, since every other input is zero.
        constexpr uint32_t kTaps = 39u;
        constexpr int16_t kFilter[kTaps] = {
            -1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246, 16384,
            10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1,
        };

        int32_t clamp16(int32_t v)
        {
            return std::clamp(v, -32768, 32767);
        }

        int32_t mul(int32_t a, int32_t b)
        {
            return (a * b) >> 15;
        }
    }

    void Reverb::setMode(uint32_t mode)
    {
        mode &= 0xffu; // SD_REV_MODE_CLEAR_WA is 0x100
        if (mode >= 10u)
            mode = 0u;
        if (mode == m_mode && !m_buffer.empty())
            return;
        m_mode = mode;
        m_buffer.assign(std::max<uint32_t>(kPresets[mode].bytes / 2u, 8u), 0);
        m_pos = 0;
        m_down = {};
        m_up = {};
    }

    void Reverb::setDepth(int16_t left, int16_t right)
    {
        m_depthL = left;
        m_depthR = right;
    }

    bool Reverb::active() const
    {
        return m_mode != 0u && (m_depthL != 0 || m_depthR != 0);
    }

    // Offsets are relative to the buffer address and wrap within the work
    // area, all in samples: a register's 8-byte units are four, and psx-spx's
    // [m-2] (bytes) is one sample back.
    int16_t &Reverb::at(int32_t offset)
    {
        const int32_t n = static_cast<int32_t>(m_buffer.size());
        int32_t i = (static_cast<int32_t>(m_pos) + offset) % n;
        if (i < 0)
            i += n;
        return m_buffer[static_cast<size_t>(i)];
    }

    // One side's step, as PCSX2's DoReverb runs it: the same-side and
    // other-side reflections, the combs and the two all-pass filters, with
    // what it writes back saturated.
    int32_t Reverb::step(bool right, int32_t in)
    {
        const uint16_t *r = kPresets[m_mode].r;
        auto v = [r](int i) { return static_cast<int32_t>(static_cast<int16_t>(r[i])); };
        auto m = [r](int i) { return static_cast<int32_t>(r[i]) * 4; };
        const int32_t vIIR = v(2), vWALL = v(7), vAPF1 = v(8), vAPF2 = v(9);
        const int32_t dAPF1 = m(0), dAPF2 = m(1);
        const int32_t sameDst = m(right ? 11 : 10);  // mRSAME : mLSAME
        const int32_t sameSrc = m(right ? 17 : 16);  // dRSAME : dLSAME
        const int32_t diffDst = m(right ? 19 : 18);  // mRDIFF : mLDIFF
        const int32_t diffSrc = m(right ? 24 : 25);  // dLDIFF : dRDIFF
        const int32_t comb1 = m(right ? 13 : 12), comb2 = m(right ? 15 : 14);
        const int32_t comb3 = m(right ? 21 : 20), comb4 = m(right ? 23 : 22);
        const int32_t apf1 = m(right ? 27 : 26), apf2 = m(right ? 29 : 28);

        in = mul(v(right ? 31 : 30), in);
        const int32_t same = mul(vIIR, in + mul(vWALL, at(sameSrc)) - at(sameDst - 1)) + at(sameDst - 1);
        const int32_t diff = mul(vIIR, in + mul(vWALL, at(diffSrc)) - at(diffDst - 1)) + at(diffDst - 1);
        int32_t out = mul(v(3), at(comb1)) + mul(v(4), at(comb2)) + mul(v(5), at(comb3)) + mul(v(6), at(comb4));
        const int32_t a1 = out - mul(vAPF1, at(apf1 - dAPF1));
        out = at(apf1 - dAPF1) + mul(vAPF1, a1);
        const int32_t a2 = out - mul(vAPF2, at(apf2 - dAPF2));
        out = at(apf2 - dAPF2) + mul(vAPF2, a2);
        at(sameDst) = static_cast<int16_t>(clamp16(same));
        at(diffDst) = static_cast<int16_t>(clamp16(diff));
        at(apf1) = static_cast<int16_t>(clamp16(a1));
        at(apf2) = static_cast<int16_t>(clamp16(a2));
        return clamp16(out);
    }

    // Every cycle the input goes into the down filter's history; one side
    // steps on its filtered input, and its output goes into the up filter's
    // history with a zero for the other side. The buffer address moves on
    // after the right side.
    void Reverb::process(const int32_t *send, int32_t *out, size_t frames)
    {
        for (size_t f = 0; f < frames; ++f)
        {
            for (uint32_t side = 0; side < 2u; ++side)
            {
                const int16_t in = static_cast<int16_t>(clamp16(send[f * 2u + side]));
                m_down[side][m_bufPos] = in;
                m_down[side][m_bufPos | 64u] = in;
            }
            const bool right = m_cycle & 1u;
            const uint32_t first = (m_bufPos + 64u - kTaps) & 63u; // the 39 before this one
            int32_t down = 0;
            for (uint32_t i = 0; i < kTaps; ++i)
                down += m_down[right][first + i] * kFilter[i];
            const int32_t wet = step(right, clamp16(down >> 15));

            m_up[right][m_bufPos] = m_up[right][m_bufPos | 64u] = static_cast<int16_t>(wet);
            m_up[!right][m_bufPos] = m_up[!right][m_bufPos | 64u] = 0;
            int32_t l = 0, r = 0;
            for (uint32_t i = 0; i < kTaps; ++i)
            {
                const int32_t tap = std::min(kFilter[i] * 2, 32767);
                l += m_up[0][first + i] * tap;
                r += m_up[1][first + i] * tap;
            }
            out[f * 2u] += mul(clamp16(l >> 15), m_depthL);
            out[f * 2u + 1u] += mul(clamp16(r >> 15), m_depthR);

            if (right)
                m_pos = (m_pos + 1u) % static_cast<uint32_t>(m_buffer.size());
            m_bufPos = (m_bufPos + 1u) & 63u;
            ++m_cycle;
        }
    }
}
