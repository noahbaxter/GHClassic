#include "synth/spu.h"

#include "synth/adpcm.h"
#include "synth/fixed.h"

#include <algorithm>
#include <cstring>

namespace gh2::synth
{
    namespace
    {
        // psx-spx, "SPU ADPCM Pitch": gauss[0x000..0x1ff].
        constexpr int16_t kGauss[512] = {
            -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1,
            0, 0, 0, 0, 0, 0, 0, 1,
            1, 1, 1, 2, 2, 2, 3, 3,
            3, 4, 4, 5, 5, 6, 7, 7,
            8, 9, 9, 10, 11, 12, 13, 14,
            15, 16, 17, 18, 19, 21, 22, 24,
            25, 27, 28, 30, 32, 33, 35, 37,
            39, 41, 44, 46, 48, 51, 53, 56,
            58, 61, 64, 67, 70, 73, 77, 80,
            84, 87, 91, 95, 99, 103, 107, 111,
            116, 120, 125, 130, 135, 140, 145, 150,
            156, 161, 167, 173, 179, 186, 192, 199,
            205, 212, 219, 227, 234, 242, 250, 257,
            266, 274, 283, 291, 300, 309, 319, 328,
            338, 348, 358, 369, 379, 390, 401, 412,
            424, 436, 448, 460, 473, 485, 498, 512,
            525, 539, 553, 567, 582, 597, 612, 627,
            643, 659, 675, 692, 708, 726, 743, 761,
            779, 797, 816, 835, 854, 874, 894, 914,
            935, 956, 977, 999, 1020, 1043, 1066, 1089,
            1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
            1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508,
            1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
            1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031,
            2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
            2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668,
            2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
            3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427,
            3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
            3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315,
            4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
            4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332,
            5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
            5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475,
            6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
            7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735,
            7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
            8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097,
            9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
            9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542,
            10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
            11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045,
            12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
            12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577,
            13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
            14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104,
            15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
            15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592,
            16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
            17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001,
            18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
            18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295,
            19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
            19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439,
            20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
            21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399,
            21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
            21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148,
            22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
            22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662,
            22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
            22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927,
            22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
        };

        // A volume register's 15 bits, as the -0x8000..+0x7ffe it stands for.
        int32_t fixedVolume(uint16_t reg)
        {
            return static_cast<int16_t>(static_cast<uint16_t>(reg << 1));
        }

        // psx-spx, "SPU Volume and ADSR Generator": one envelope step, after
        // waiting its cycles.
        struct Step
        {
            uint32_t cycles;
            int32_t step;
        };

        Step envelopeStep(int32_t level, uint32_t shift, int32_t stepValue, bool exponential, bool decrease)
        {
            Step s;
            s.cycles = 1u << std::max(0, static_cast<int32_t>(shift) - 11);
            s.step = stepValue * (1 << std::max(0, 11 - static_cast<int32_t>(shift)));
            if (exponential && !decrease && level > 0x6000)
                s.cycles *= 4u;
            if (exponential && decrease)
                s.step = s.step * level / 0x8000;
            return s;
        }
    }

    Spu::Spu() : m_ram(kRamBytes, 0u)
    {
        reset();
    }

    void Spu::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), 0u);
        m_voices = {};
        m_masterL = m_masterR = 0;
        for (Reverb &r : m_reverb)
            r = Reverb{};
    }

    void Spu::write(uint32_t address, const uint8_t *bytes, uint32_t len)
    {
        for (uint32_t i = 0; i < len; ++i)
            m_ram[(address + i) % kRamBytes] = bytes[i];
    }

    void Spu::setVolume(uint32_t voice, uint16_t left, uint16_t right)
    {
        m_voices[voice].volL = fixedVolume(left);
        m_voices[voice].volR = fixedVolume(right);
    }

    void Spu::setPitch(uint32_t voice, uint16_t pitch)
    {
        m_voices[voice].pitch = pitch;
    }

    void Spu::setAdsr(uint32_t voice, uint16_t adsr1, uint16_t adsr2)
    {
        m_voices[voice].adsr1 = adsr1;
        m_voices[voice].adsr2 = adsr2;
    }

    void Spu::setStart(uint32_t voice, uint32_t address)
    {
        m_voices[voice].start = address & (kRamBytes - 1u) & ~15u;
    }

    void Spu::setLoop(uint32_t voice, uint32_t address)
    {
        m_voices[voice].loop = address & (kRamBytes - 1u) & ~15u;
        m_voices[voice].loopWritten = true;
    }

    void Spu::setReverbSend(uint32_t voice, bool on)
    {
        m_voices[voice].reverbSend = on;
    }

    // Key on starts the attack from zero at the start address and clears
    // ENDX. The loop address is left alone: on the SPU2 a loop-start flag
    // sets it only until software writes it (PCSX2, Mixer.cpp LoopMode), so
    // one written before key on survives until the voice reaches a flag.
    void Spu::keyOn(uint32_t voice)
    {
        Voice &v = m_voices[voice];
        v.nax = v.start;
        v.loopWritten = false;
        v.ended = false;
        v.blockPos = kAdpcmBlockSamples;
        v.hist1 = v.hist2 = 0;
        std::fill(std::begin(v.fifo), std::end(v.fifo), int16_t{0});
        v.counter = 0u;
        v.phase = Phase::kAttack;
        v.level = 0;
        v.wait = 0u;
    }

    void Spu::keyOff(uint32_t voice)
    {
        Voice &v = m_voices[voice];
        if (v.phase != Phase::kOff)
        {
            v.phase = Phase::kRelease;
            v.wait = 0u;
        }
    }

    uint32_t Spu::nextAddress(uint32_t voice) const
    {
        return m_voices[voice].nax;
    }

    bool Spu::ended(uint32_t voice) const
    {
        return m_voices[voice].ended;
    }

    uint16_t Spu::envelope(uint32_t voice) const
    {
        return static_cast<uint16_t>(m_voices[voice].level);
    }

    void Spu::setMasterVolume(uint16_t left, uint16_t right)
    {
        m_masterL = fixedVolume(left);
        m_masterR = fixedVolume(right);
    }

    Reverb &Spu::reverb(uint32_t core)
    {
        return m_reverb[core & 1u];
    }

    // The next sample of the voice's ADPCM. A block's loop-start flag marks
    // the loop address as the block is fetched; its loop-end flag, once its
    // last sample is out, sets ENDX and jumps to the loop address, and
    // without the repeat flag stops the voice there.
    int16_t Spu::nextSample(Voice &v)
    {
        if (v.blockPos == kAdpcmBlockSamples)
        {
            const uint8_t *block = m_ram.data() + v.nax;
            v.flags = block[1];
            if ((v.flags & kAdpcmLoopStart) && !v.loopWritten)
                v.loop = v.nax;
            AdpcmState state{v.hist1, v.hist2};
            decodeAdpcm(block, state, v.block);
            v.hist1 = state.hist1;
            v.hist2 = state.hist2;
            v.blockPos = 0u;
        }
        const int16_t s = v.block[v.blockPos++];
        if (v.blockPos == kAdpcmBlockSamples)
        {
            if (v.flags & kAdpcmLoopEnd)
            {
                v.ended = true;
                v.nax = v.loop;
                if (!(v.flags & kAdpcmRepeat))
                {
                    v.phase = Phase::kOff;
                    v.level = 0;
                }
            }
            else
            {
                v.nax = (v.nax + kAdpcmBlockBytes) % kRamBytes;
            }
        }
        return s;
    }

    // psx-spx, "4-Point Gaussian Interpolation", indexed by counter bits 4-11.
    int32_t Spu::sample(Voice &v)
    {
        const uint32_t i = (v.counter >> 4) & 0xffu;
        int32_t out = mul15(kGauss[0x0ff - i], v.fifo[0]);
        out += mul15(kGauss[0x1ff - i], v.fifo[1]);
        out += mul15(kGauss[0x100 + i], v.fifo[2]);
        out += mul15(kGauss[0x000 + i], v.fifo[3]);
        return out;
    }

    void Spu::envelope(Voice &v)
    {
        if (v.wait > 0u)
        {
            --v.wait;
            return;
        }
        Step s{};
        switch (v.phase)
        {
        case Phase::kAttack:
            s = envelopeStep(v.level, (v.adsr1 >> 10) & 0x1fu, 7 - ((v.adsr1 >> 8) & 3), v.adsr1 & 0x8000u, false);
            break;
        case Phase::kDecay:
            s = envelopeStep(v.level, (v.adsr1 >> 4) & 0x0fu, -8, true, true);
            break;
        case Phase::kSustain:
        {
            const bool decrease = v.adsr2 & 0x4000u;
            const int32_t step = (v.adsr2 >> 6) & 3;
            s = envelopeStep(v.level, (v.adsr2 >> 8) & 0x1fu, decrease ? -8 + step : 7 - step, v.adsr2 & 0x8000u,
                             decrease);
            break;
        }
        case Phase::kRelease:
            s = envelopeStep(v.level, v.adsr2 & 0x1fu, -8, v.adsr2 & 0x20u, true);
            break;
        case Phase::kOff:
            return;
        }
        v.level = std::clamp(v.level + s.step, 0, 0x7fff);
        v.wait = s.cycles - 1u;

        const int32_t sustainLevel = static_cast<int32_t>((v.adsr1 & 0x0fu) + 1u) * 0x800;
        if (v.phase == Phase::kAttack && v.level >= 0x7fff)
            v.phase = Phase::kDecay;
        else if (v.phase == Phase::kDecay && v.level <= sustainLevel)
            v.phase = Phase::kSustain;
        else if (v.phase == Phase::kRelease && v.level == 0)
            v.phase = Phase::kOff;
    }

    void Spu::mix(float *out, size_t frames)
    {
        for (uint32_t core = 0; core < kCores; ++core)
        {
            m_dry[core].assign(frames * 2u, 0);
            m_send[core].assign(frames * 2u, 0);
            m_wet[core].assign(frames * 2u, 0);
        }

        for (uint32_t n = 0; n < kVoices; ++n)
        {
            Voice &v = m_voices[n];
            if (v.phase == Phase::kOff)
                continue;
            const uint32_t core = n / kVoicesPerCore;
            int32_t *coreDry = m_dry[core].data();
            int32_t *send = m_send[core].data();
            const uint32_t step = std::min<uint32_t>(v.pitch, 0x3fffu);
            for (size_t f = 0; f < frames && v.phase != Phase::kOff; ++f)
            {
                const int32_t s = mul15(sample(v), v.level);
                const int32_t l = mul15(s, v.volL);
                const int32_t r = mul15(s, v.volR);
                coreDry[f * 2u] += l;
                coreDry[f * 2u + 1u] += r;
                if (v.reverbSend)
                {
                    send[f * 2u] += l;
                    send[f * 2u + 1u] += r;
                }
                envelope(v);
                v.counter += step;
                while (v.counter >= kPitchOne && v.phase != Phase::kOff)
                {
                    v.counter -= kPitchOne;
                    v.fifo[0] = v.fifo[1];
                    v.fifo[1] = v.fifo[2];
                    v.fifo[2] = v.fifo[3];
                    v.fifo[3] = nextSample(v);
                }
            }
        }

        for (uint32_t core = 0; core < kCores; ++core)
            if (m_reverb[core].active())
                m_reverb[core].process(m_send[core].data(), m_wet[core].data(), frames);
        for (size_t f = 0; f < frames; ++f)
        {
            for (uint32_t c = 0; c < 2u; ++c)
            {
                const int32_t master = c == 0u ? m_masterL : m_masterR;
                int32_t sum = 0;
                for (uint32_t core = 0; core < kCores; ++core)
                {
                    const int32_t in = clamp16(m_dry[core][f * 2u + c] + m_wet[core][f * 2u + c]);
                    sum += mul15(in, master);
                }
                out[f * 2u + c] += clamp16(sum) / 32768.0f;
            }
        }
    }
}
