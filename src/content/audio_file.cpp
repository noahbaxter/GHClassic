#include "content/audio_file.h"

#include "content/audio_kind.h"
#include "content/vorbis.h"

// Built in raylib's raudio, which the runtime links.
#include <dr_mp3.h>
#include <dr_wav.h>
#include <opusfile.h>

#include <algorithm>
#include <iostream>

namespace gh2::audio_file
{
    namespace
    {
        // A decoder that gives frames interleaved, its bytes held in place,
        // each read split into channels.
        class Interleaved : public vgs::Pcm
        {
        public:
            explicit Interleaved(std::shared_ptr<const std::vector<uint8_t>> bytes) : m_bytes(std::move(bytes)) {}

            uint32_t read(float *const *out, uint32_t most) override
            {
                m_scratch.resize(size_t(most) * channels);
                const uint32_t n = decode(m_scratch.data(), most);
                for (uint32_t i = 0; i < n; ++i)
                    for (uint32_t c = 0; c < channels; ++c)
                        out[c][i] = m_scratch[size_t(i) * channels + c];
                return n;
            }

        protected:
            // Up to `most` frames into `to`, interleaved, how many it gave.
            virtual uint32_t decode(float *to, uint32_t most) = 0;

            std::shared_ptr<const std::vector<uint8_t>> m_bytes;

        private:
            std::vector<float> m_scratch;
        };

        // Opus decodes at 48 kHz whatever it was made from, and as stereo
        // here, a mono stream on both sides and more mixed down.
        class Opus final : public Interleaved
        {
        public:
            using Interleaved::Interleaved;
            ~Opus() override
            {
                if (m_file)
                    op_free(m_file);
            }

            bool start(const std::string &name)
            {
                int error = 0;
                m_file = op_open_memory(m_bytes->data(), m_bytes->size(), &error);
                if (!m_file)
                {
                    std::cerr << "[audio] " << name << ": Opus error " << error << std::endl;
                    return false;
                }
                channels = 2u;
                rate = 48000u;
                frames = static_cast<uint64_t>(std::max<ogg_int64_t>(op_pcm_total(m_file, -1), 0));
                return true;
            }

            bool seek(uint64_t frame) override { return op_pcm_seek(m_file, static_cast<ogg_int64_t>(frame)) == 0; }

        private:
            uint32_t decode(float *to, uint32_t most) override
            {
                // A read gives at most a packet's worth; on until full.
                uint32_t got = 0u;
                while (got < most)
                {
                    const int n = op_read_float_stereo(m_file, to + size_t(got) * 2u, static_cast<int>((most - got) * 2u));
                    // A hole (a page lost or damaged) is gone past, not the end.
                    if (n == OP_HOLE)
                        continue;
                    if (n <= 0)
                        break;
                    got += static_cast<uint32_t>(n);
                }
                return got;
            }

            OggOpusFile *m_file = nullptr;
        };

        class Mp3 final : public Interleaved
        {
        public:
            using Interleaved::Interleaved;
            ~Mp3() override
            {
                if (m_started)
                    drmp3_uninit(&m_mp3);
            }

            bool start(const std::string &name)
            {
                m_started = drmp3_init_memory(&m_mp3, m_bytes->data(), m_bytes->size(), nullptr);
                if (!m_started || m_mp3.channels < 1u)
                {
                    std::cerr << "[audio] " << name << ": not MP3" << std::endl;
                    return false;
                }
                channels = m_mp3.channels;
                rate = m_mp3.sampleRate;
                frames = drmp3_get_pcm_frame_count(&m_mp3);
                // A point a second or so to seek from: without, a seek back
                // decodes from the start up to it (a preview 90 seconds in,
                // 90 seconds of every stem).
                drmp3_uint32 points = static_cast<drmp3_uint32>(std::min<uint64_t>(frames / std::max(rate, 1u) + 1u, kMostSeekPoints));
                m_seekPoints.resize(points);
                if (drmp3_calculate_seek_points(&m_mp3, &points, m_seekPoints.data()))
                {
                    m_seekPoints.resize(points);
                    drmp3_bind_seek_table(&m_mp3, points, m_seekPoints.data());
                }
                return true;
            }

            bool seek(uint64_t frame) override { return drmp3_seek_to_pcm_frame(&m_mp3, frame); }

        private:
            uint32_t decode(float *to, uint32_t most) override
            {
                return static_cast<uint32_t>(drmp3_read_pcm_frames_f32(&m_mp3, most, to));
            }

            static constexpr uint64_t kMostSeekPoints = 1024u;
            drmp3 m_mp3{};
            bool m_started = false;
            std::vector<drmp3_seek_point> m_seekPoints;
        };

        class Wav final : public Interleaved
        {
        public:
            using Interleaved::Interleaved;
            ~Wav() override
            {
                if (m_started)
                    drwav_uninit(&m_wav);
            }

            bool start(const std::string &name)
            {
                m_started = drwav_init_memory(&m_wav, m_bytes->data(), m_bytes->size(), nullptr);
                if (!m_started || m_wav.channels < 1u)
                {
                    std::cerr << "[audio] " << name << ": not WAV" << std::endl;
                    return false;
                }
                channels = m_wav.channels;
                rate = m_wav.sampleRate;
                frames = m_wav.totalPCMFrameCount;
                return true;
            }

            bool seek(uint64_t frame) override { return drwav_seek_to_pcm_frame(&m_wav, frame); }

        private:
            uint32_t decode(float *to, uint32_t most) override
            {
                return static_cast<uint32_t>(drwav_read_pcm_frames_f32(&m_wav, most, to));
            }

            drwav m_wav{};
            bool m_started = false;
        };

        template <typename Decoder>
        std::unique_ptr<vgs::Pcm> started(std::shared_ptr<const std::vector<uint8_t>> bytes, const std::string &name)
        {
            auto decoder = std::make_unique<Decoder>(std::move(bytes));
            if (!decoder->start(name))
                return nullptr;
            return decoder;
        }
    }

    std::unique_ptr<vgs::Pcm> open(std::shared_ptr<const std::vector<uint8_t>> bytes, const std::string &name)
    {
        if (!bytes)
            return nullptr;
        std::string other;
        const std::string what = kind(bytes->data(), bytes->size(), &other);
        if (what == "vorbis")
            return vorbis::open(std::move(bytes), name);
        if (what == "opus")
            return started<Opus>(std::move(bytes), name);
        if (what == "mp3")
            return started<Mp3>(std::move(bytes), name);
        if (what == "wav")
            return started<Wav>(std::move(bytes), name);
        std::cerr << "[audio] " << name << ": " << other << std::endl;
        return nullptr;
    }
}
