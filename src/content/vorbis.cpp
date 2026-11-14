#include "content/vorbis.h"

#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace gh2::vorbis
{
    namespace
    {
        // The stream's bytes and the decoder reading through them, held in
        // place.
        class Decoder final : public vgs::Pcm
        {
        public:
            explicit Decoder(FileBytes bytes) : m_in{std::move(bytes)} {}

            ~Decoder() override
            {
                if (m_decoding)
                    ov_clear(&m_vorbis);
            }

            bool start(const std::string &name);

            bool seek(uint64_t frame) override { return ov_pcm_seek(&m_vorbis, static_cast<ogg_int64_t>(frame)) == 0; }

            uint32_t read(float *const *out, uint32_t most) override
            {
                float **decoded = nullptr;
                int section = 0;
                long n = OV_HOLE;
                // A hole (a page lost or damaged) is gone past, not the end.
                while (n == OV_HOLE)
                    n = ov_read_float(&m_vorbis, &decoded, static_cast<int>(most), &section);
                if (n <= 0)
                    return 0u;
                // A chained stream's link may have fewer channels than the
                // first: its last on the rest.
                const vorbis_info *link = ov_info(&m_vorbis, section);
                const uint32_t has = link ? std::max(1u, static_cast<uint32_t>(link->channels)) : channels;
                for (uint32_t c = 0; c < channels; ++c)
                    std::memcpy(out[c], decoded[std::min(c, has - 1u)], size_t(n) * sizeof(float));
                return static_cast<uint32_t>(n);
            }

            FileCursor m_in;

        private:
            OggVorbis_File m_vorbis{};
            bool m_decoding = false;
        };

        const ov_callbacks kFromBytes = {
            [](void *to, size_t size, size_t count, void *from) -> size_t
            {
                return size ? static_cast<Decoder *>(from)->m_in.take(to, size * count) / size : 0u;
            },
            [](void *from, ogg_int64_t offset, int whence) -> int
            {
                return static_cast<Decoder *>(from)->m_in.to(offset, whence) ? 0 : -1;
            },
            nullptr,
            [](void *from) -> long { return static_cast<long>(static_cast<Decoder *>(from)->m_in.at); },
        };

        bool Decoder::start(const std::string &name)
        {
            if (const int error = ov_open_callbacks(this, &m_vorbis, nullptr, 0, kFromBytes))
            {
                std::cerr << "[vorbis] " << name << ": Vorbis error " << error << std::endl;
                return false;
            }
            m_decoding = true;
            const vorbis_info *info = ov_info(&m_vorbis, -1);
            if (!info || info->channels < 1)
                return false;
            channels = static_cast<uint32_t>(info->channels);
            if (channels == 3u || channels >= 5u)
                right = 2u;
            rate = static_cast<uint32_t>(info->rate);
            frames = static_cast<uint64_t>(std::max<ogg_int64_t>(ov_pcm_total(&m_vorbis, -1), 0));
            return true;
        }
    }

    std::unique_ptr<vgs::Pcm> open(FileBytes bytes, const std::string &name)
    {
        auto decoder = std::make_unique<Decoder>(std::move(bytes));
        if (!decoder->start(name))
            return nullptr;
        return decoder;
    }
}
