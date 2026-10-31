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
            Decoder(std::shared_ptr<const std::vector<uint8_t>> bytes, size_t from)
                : m_held(std::move(bytes)), m_file(m_held->data() + from), m_size(m_held->size() - from)
            {
            }

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

            std::shared_ptr<const std::vector<uint8_t>> m_held;
            const uint8_t *m_file;
            size_t m_size;
            size_t m_at = 0u; // the decoder's read position in m_file

        private:
            OggVorbis_File m_vorbis{};
            bool m_decoding = false;
        };

        // vorbisfile's reads, from the bytes in memory.
        const ov_callbacks kFromMemory = {
            [](void *to, size_t size, size_t count, void *from) -> size_t
            {
                Decoder &d = *static_cast<Decoder *>(from);
                const size_t n = size ? std::min(count, (d.m_size - d.m_at) / size) : 0u;
                std::memcpy(to, d.m_file + d.m_at, n * size);
                d.m_at += n * size;
                return n;
            },
            [](void *from, ogg_int64_t offset, int whence) -> int
            {
                Decoder &d = *static_cast<Decoder *>(from);
                const int64_t base = whence == SEEK_CUR ? int64_t(d.m_at) : whence == SEEK_END ? int64_t(d.m_size) : 0;
                if (base + offset < 0 || base + offset > int64_t(d.m_size))
                    return -1;
                d.m_at = static_cast<size_t>(base + offset);
                return 0;
            },
            nullptr,
            [](void *from) -> long { return static_cast<long>(static_cast<Decoder *>(from)->m_at); },
        };

        bool Decoder::start(const std::string &name)
        {
            if (const int error = ov_open_callbacks(this, &m_vorbis, nullptr, 0, kFromMemory))
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

    std::unique_ptr<vgs::Pcm> open(std::shared_ptr<const std::vector<uint8_t>> bytes, const std::string &name, size_t from)
    {
        if (!bytes || from > bytes->size())
            return nullptr;
        auto decoder = std::make_unique<Decoder>(std::move(bytes), from);
        if (!decoder->start(name))
            return nullptr;
        return decoder;
    }
}
