#pragma once

// Sony's PSS movie container: an MPEG-2 program stream in 16 KiB packs, one
// MPEG-2 video stream (0xE0) and PCM audio in private stream 1 (0xBD). Every
// GH2 movie carries the same audio: each 0xBD payload opens with 4 bytes
// (ffa00000), the first then holds a Sony "SShd" header (PCM16LE, 48000 Hz,
// 2 channels, 0x200-byte interleave) and an "SSbd" header before samples.
// GH1's intro (videos/ghintro.pss) is the same at 44100 Hz.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace gh2::movie
{
    constexpr uint32_t kPackBytes = 0x4000u;
    constexpr uint8_t kVideoStream = 0xe0u;
    constexpr uint8_t kAudioStream = 0xbdu;

    // One PES packet's payload. `pts` is 90 kHz, -1 when the packet has none.
    struct Packet
    {
        uint8_t stream = 0;
        int64_t pts = -1;
        const uint8_t *data = nullptr;
        size_t size = 0;
    };

    // Each PES packet in one pack, in order. False when the pack is not one.
    bool demuxPack(const uint8_t *pack, size_t size, const std::function<void(const Packet &)> &each);

    // The SShd/SSbd-headed PCM in the 0xBD packets, as stereo frames at its
    // own rate. Each channel comes in 0x200-byte blocks, left then right.
    class PssAudio
    {
    public:
        // One 0xBD payload; `each` gets whole stereo frames as they complete.
        // False when the header is not the PCM GH1's and GH2's movies use.
        bool feed(const uint8_t *data, size_t size, const std::function<void(const int16_t *, size_t)> &each);

        // The header's rate, 48000 or 44100; 0 before the header.
        uint32_t rate() const { return m_rate; }

    private:
        static constexpr size_t kInterleave = 0x200u;
        uint32_t m_rate = 0;
        bool m_started = false;
        uint8_t m_block[kInterleave * 2] = {};
        size_t m_filled = 0;
    };
}
