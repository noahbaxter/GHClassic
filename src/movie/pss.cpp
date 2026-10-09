#include "movie/pss.h"

#include <algorithm>
#include <cstring>

namespace gh2::movie
{
    namespace
    {
        uint32_t be32(const uint8_t *p)
        {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }

        uint32_t le32(const uint8_t *p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        // The 33-bit timestamp in a PES header's 5 PTS bytes.
        int64_t readPts(const uint8_t *p)
        {
            return (int64_t(p[0] & 0x0e) << 29) | (int64_t(p[1]) << 22) | (int64_t(p[2] & 0xfe) << 14) |
                   (int64_t(p[3]) << 7) | (p[4] >> 1);
        }

        constexpr uint32_t kPackStart = 0x1bau;
        constexpr uint32_t kProgramEnd = 0x1b9u;
        constexpr uint32_t kAudioPrefix = 4u;
        constexpr uint32_t kSShdBytes = 0x18u;
    }

    bool demuxPack(const uint8_t *pack, size_t size, const std::function<void(const Packet &)> &each)
    {
        // MPEG-2 pack header: 14 bytes and up to 7 of stuffing.
        if (size < 14u || be32(pack) != kPackStart)
            return false;
        size_t pos = 14u + (pack[13] & 7u);
        while (pos + 6u <= size)
        {
            const uint32_t code = be32(pack + pos);
            if (code == kProgramEnd || (code >> 8) != 1u)
                break;
            const size_t length = (size_t(pack[pos + 4]) << 8) | pack[pos + 5];
            const uint8_t *body = pack + pos + 6u;
            pos += 6u + length;
            if (pos > size)
                break;
            const uint8_t stream = code & 0xffu;
            if ((stream != kVideoStream && stream != kAudioStream) || length < 3u)
                continue;
            const size_t header = 3u + body[2];
            if (header > length)
                continue;
            Packet packet;
            packet.stream = stream;
            if ((body[1] & 0x80u) && header >= 8u)
                packet.pts = readPts(body + 3);
            packet.data = body + header;
            packet.size = length - header;
            each(packet);
        }
        return true;
    }

    bool PssAudio::feed(const uint8_t *data, size_t size, const std::function<void(const int16_t *, size_t)> &each)
    {
        if (size < kAudioPrefix)
            return true;
        data += kAudioPrefix;
        size -= kAudioPrefix;
        if (!m_started)
        {
            constexpr size_t kHeaders = 8u + kSShdBytes + 8u;
            if (size < kHeaders || std::memcmp(data, "SShd", 4) != 0 || le32(data + 4) != kSShdBytes ||
                std::memcmp(data + 8u + kSShdBytes, "SSbd", 4) != 0)
                return false;
            const uint8_t *shd = data + 8;
            const uint32_t rate = le32(shd + 4);
            if (le32(shd) != 1u || (rate != 48000u && rate != 44100u) || le32(shd + 8) != 2u ||
                le32(shd + 12) != kInterleave)
                return false;
            m_rate = rate;
            data += kHeaders;
            size -= kHeaders;
            m_started = true;
        }
        while (size > 0u)
        {
            const size_t take = std::min(size, sizeof(m_block) - m_filled);
            std::memcpy(m_block + m_filled, data, take);
            m_filled += take;
            data += take;
            size -= take;
            if (m_filled < sizeof(m_block))
                break;
            int16_t frames[kInterleave];
            const uint8_t *left = m_block;
            const uint8_t *right = m_block + kInterleave;
            for (size_t i = 0; i < kInterleave / 2u; ++i)
            {
                frames[i * 2u] = int16_t(left[i * 2u] | (left[i * 2u + 1u] << 8));
                frames[i * 2u + 1u] = int16_t(right[i * 2u] | (right[i * 2u + 1u] << 8));
            }
            each(frames, kInterleave / 2u);
            m_filled = 0;
        }
        return true;
    }
}
