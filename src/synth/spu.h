#pragma once

// The SPU2 as SYNTH_S drives it through libsd: 2 MB of sound RAM, two cores
// of 24 voices, and a reverb unit per core. Voices walk PS-ADPCM in sound
// RAM at a pitch, through the 4-point gaussian filter, shaped by an ADSR
// envelope, at 48 kHz.
//
// Registers are taken as libsd's sceSdSetParam would write them. Hardware
// behaviour follows psx-spx, and PCSX2's SPU2 where the PS2 differs (loop
// addresses, below).

#include "synth/reverb.h"

#include <array>
#include <cstdint>
#include <vector>

namespace gh2::synth
{
    class Spu
    {
    public:
        static constexpr uint32_t kRamBytes = 2u * 1024u * 1024u;
        static constexpr uint32_t kVoices = 48u; // 0..23 core 0, 24..47 core 1

        Spu();

        void reset();
        void write(uint32_t address, const uint8_t *bytes, uint32_t len);
        uint8_t *ram(uint32_t address);

        // VOLL/VOLR as written: 15 bits of volume/2, bit 14 its sign.
        void setVolume(uint32_t voice, uint16_t left, uint16_t right);
        void setPitch(uint32_t voice, uint16_t pitch);
        void setAdsr(uint32_t voice, uint16_t adsr1, uint16_t adsr2);
        void setStart(uint32_t voice, uint32_t address);
        void setLoop(uint32_t voice, uint32_t address);
        void setReverbSend(uint32_t voice, bool on);

        void keyOn(uint32_t voice);
        void keyOff(uint32_t voice);

        uint32_t nextAddress(uint32_t voice) const; // NAX
        bool ended(uint32_t voice) const;           // ENDX
        uint16_t envelope(uint32_t voice) const;    // ENVX

        void setMasterVolume(uint16_t left, uint16_t right); // both cores
        Reverb &reverb(uint32_t core);

        // Adds `frames` of interleaved stereo at 48 kHz into `out`.
        void mix(float *out, size_t frames);

    private:
        enum class Phase : uint8_t
        {
            kOff,
            kAttack,
            kDecay,
            kSustain,
            kRelease,
        };

        struct Voice
        {
            int32_t volL = 0, volR = 0;
            uint16_t pitch = 0;
            uint16_t adsr1 = 0, adsr2 = 0;
            uint32_t start = 0;
            uint32_t loop = 0;
            bool loopWritten = false; // since key on: a loop-start flag leaves it
            bool reverbSend = false;
            bool ended = false;

            uint32_t nax = 0;       // the block being played
            uint8_t flags = 0;      // its flag byte
            int16_t block[28] = {};
            uint32_t blockPos = 28; // next sample in `block`; 28 fetches the next
            int32_t hist1 = 0, hist2 = 0;
            int16_t fifo[4] = {};   // oldest .. newest
            uint32_t counter = 0;

            Phase phase = Phase::kOff;
            int32_t level = 0;
            uint32_t wait = 0;
        };

        int16_t nextSample(Voice &v);
        int32_t sample(Voice &v);
        void envelope(Voice &v);

        std::vector<uint8_t> m_ram;
        std::array<Voice, kVoices> m_voices;
        int32_t m_masterL = 0, m_masterR = 0;
        Reverb m_reverb[2];
        // Scratch for mix: per core, stereo.
        std::vector<int32_t> m_dry[2];
        std::vector<int32_t> m_send[2];
        std::vector<int32_t> m_wet[2];
    };
}
