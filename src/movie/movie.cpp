// GH2's full-screen movie player, native: the PSS decoded on the host, its
// pictures shown in place of the game's frames, its soundtrack mixed over the
// synth. The pictures keep time to the soundtrack.

#include "movie/movie.h"
#include "movie/pss.h"
#include "movie/screen.h"

#include "host/audio.h"
#include "host/input.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_disc_image.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

extern "C"
{
#include <mpeg2.h>
}

namespace gh2
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // The soundtrack, indexed by its frame number from the first. The
        // device takes frames whether or not they have arrived, so a late
        // frame is dropped rather than shifting everything after it.
        constexpr uint64_t kRingFrames = 1u << 17; // 2.7 s
        int16_t s_ring[kRingFrames * 2u];
        std::atomic<uint64_t> s_written{0};
        std::atomic<uint64_t> s_played{0};
        std::atomic<bool> s_running{false};
        std::atomic<bool> s_playing{false};

        void mixSoundtrack(float *out, size_t frames)
        {
            if (!s_running.load(std::memory_order_acquire))
                return;
            const uint64_t from = s_played.load(std::memory_order_relaxed);
            const uint64_t written = s_written.load(std::memory_order_acquire);
            for (size_t i = 0; i < frames && from + i < written; ++i)
            {
                const int16_t *frame = s_ring + ((from + i) % kRingFrames) * 2u;
                out[i * 2u] += frame[0] * (1.0f / 32768.0f);
                out[i * 2u + 1u] += frame[1] * (1.0f / 32768.0f);
            }
            s_played.store(from + frames, std::memory_order_release);
        }

        // The IPU's colour conversion (CSC) to RGB32, as PCSX2 models it
        // (pcsx2/IPU/yuv2rgb.cpp): fixed BT.601 coefficients over 64, the
        // nearest chroma sample, no dither.
        void convert(const mpeg2_sequence_t &sequence, const mpeg2_fbuf_t &fbuf, movie::Picture &out)
        {
            out.width = sequence.picture_width;
            out.height = sequence.picture_height;
            out.rgba.resize(size_t(out.width) * out.height * 4u);
            uint8_t *dst = out.rgba.data();
            for (uint32_t y = 0; y < out.height; ++y)
            {
                const uint8_t *luma = fbuf.buf[0] + size_t(y) * sequence.width;
                const uint8_t *cb = fbuf.buf[1] + size_t(y >> 1) * sequence.chroma_width;
                const uint8_t *cr = fbuf.buf[2] + size_t(y >> 1) * sequence.chroma_width;
                for (uint32_t x = 0; x < out.width; ++x)
                {
                    const int32_t lum = (0x95 * std::max(0, luma[x] - 16)) >> 6;
                    const int32_t u = cb[x >> 1] - 128;
                    const int32_t v = cr[x >> 1] - 128;
                    const int32_t r = lum + ((0xcc * v) >> 6);
                    const int32_t g = lum + ((-0x68 * v) >> 6) + ((-0x32 * u) >> 6);
                    const int32_t b = lum + ((0x102 * u) >> 6);
                    *dst++ = uint8_t(std::clamp((r + 1) >> 1, 0, 255));
                    *dst++ = uint8_t(std::clamp((g + 1) >> 1, 0, 255));
                    *dst++ = uint8_t(std::clamp((b + 1) >> 1, 0, 255));
                    *dst++ = 0xff;
                }
            }
        }

        class Player
        {
        public:
            Player(DiscImage &disc, const DiscImage::Extent &extent, float minSkipSeconds, PS2Runtime &runtime)
                : m_disc(disc), m_extent(extent), m_runtime(runtime),
                  m_skipAfter(Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                 std::chrono::duration<float>(minSkipSeconds)))
            {
                m_decoder = mpeg2_init();
                s_written = 0;
                s_played = 0;
                s_running = false;
                setMovieAudioSource(mixSoundtrack);
                s_playing = true;
            }

            ~Player()
            {
                s_playing = false;
                s_running = false;
                setMovieAudioSource(nullptr);
                movie::endPictures();
                mpeg2_close(m_decoder);
            }

            void run()
            {
                std::vector<uint8_t> pack(movie::kPackBytes);
                for (uint64_t offset = 0; offset < m_extent.size && !m_done; offset += movie::kPackBytes)
                {
                    const size_t size = m_disc.readExtent(m_extent, offset, pack.data(), pack.size());
                    if (!movie::demuxPack(pack.data(), size, [this](const movie::Packet &packet) { take(packet); }))
                        break;
                    showDue();
                }
                // A sequence end code puts out the last picture if the stream
                // did not.
                const uint8_t end[4] = {0x00, 0x00, 0x01, 0xb7};
                decode(end, sizeof(end));
                s_running = true;
                while (!m_queue.empty() && !m_done)
                {
                    waitUntil(m_queue.front().pts);
                    showDue();
                }
                if (!m_done)
                    waitUntil(m_lastPts + m_period);
            }

        private:
            void take(const movie::Packet &packet)
            {
                if (m_done)
                    return;
                if (packet.stream == movie::kVideoStream)
                {
                    if (packet.pts >= 0)
                        mpeg2_tag_picture(m_decoder, uint32_t(packet.pts), uint32_t(packet.pts >> 32));
                    decode(packet.data, packet.size);
                    return;
                }
                if (m_audioBad)
                    return;
                if (m_clockBase < 0 && packet.pts >= 0)
                    m_clockBase = packet.pts;
                if (!m_audio.feed(packet.data, packet.size,
                                  [this](const int16_t *frames, size_t count) { push(frames, count); }))
                {
                    std::cerr << "[movie] soundtrack is not PCM16 stereo 48 kHz; playing without it" << std::endl;
                    m_audioBad = true;
                }
            }

            void push(const int16_t *frames, size_t count)
            {
                uint64_t at = s_written.load(std::memory_order_relaxed);
                for (size_t i = 0; i < count; ++i, ++at)
                {
                    while (at >= s_played.load(std::memory_order_acquire) + kRingFrames)
                    {
                        s_written.store(at, std::memory_order_release);
                        // Full before the pictures are: start the clock rather
                        // than wait on it.
                        s_running = true;
                        if (stopping())
                            return;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    s_ring[(at % kRingFrames) * 2u] = frames[i * 2u];
                    s_ring[(at % kRingFrames) * 2u + 1u] = frames[i * 2u + 1u];
                }
                s_written.store(at, std::memory_order_release);
            }

            void decode(const uint8_t *data, size_t size)
            {
                // libmpeg2 only reads the buffer, and is done with it at
                // STATE_BUFFER.
                uint8_t *start = const_cast<uint8_t *>(data);
                mpeg2_buffer(m_decoder, start, start + size);
                const mpeg2_info_t *info = mpeg2_info(m_decoder);
                for (;;)
                {
                    const mpeg2_state_t state = mpeg2_parse(m_decoder);
                    if (state == STATE_BUFFER)
                        return;
                    if (state == STATE_SEQUENCE)
                        m_period = info->sequence->frame_period / 300u; // 27 MHz to 90 kHz
                    if ((state == STATE_SLICE || state == STATE_END || state == STATE_INVALID_END) &&
                        info->display_fbuf && !m_done)
                        present(*info);
                }
            }

            // A decoded picture onto the queue. A full queue waits for its
            // oldest to be due; the first time it fills, the clock starts.
            void present(const mpeg2_info_t &info)
            {
                const mpeg2_picture_t *picture = info.display_picture;
                if (picture && (picture->flags & PIC_FLAG_TAGS))
                    m_lastPts = int64_t(picture->tag) | (int64_t(picture->tag2) << 32);
                else
                    m_lastPts += m_period;
                if (m_clockBase < 0)
                    m_clockBase = m_lastPts;
                Queued queued;
                if (!m_spare.empty())
                {
                    queued.picture = std::move(m_spare.back());
                    m_spare.pop_back();
                }
                queued.pts = m_lastPts;
                convert(*info.sequence, *info.display_fbuf, queued.picture);
                m_queue.push_back(std::move(queued));
                while (m_queue.size() >= kQueueDepth && !m_done)
                {
                    s_running = true;
                    waitUntil(m_queue.front().pts);
                    showDue();
                }
            }

            // The newest queued picture the clock has reached, dropping any
            // older ones it passed.
            void showDue()
            {
                if (!s_running.load(std::memory_order_relaxed))
                    return;
                const int64_t now = clock();
                bool any = false;
                while (!m_queue.empty() && m_queue.front().pts <= now)
                {
                    if (any)
                        m_spare.push_back(std::move(m_shown));
                    m_shown = std::move(m_queue.front().picture);
                    m_queue.pop_front();
                    any = true;
                }
                if (!any)
                    return;
                movie::showPicture(m_shown);
                m_spare.push_back(std::move(m_shown));
            }

            // The soundtrack's position, as a 90 kHz PTS.
            int64_t clock() const
            {
                const uint64_t played = s_played.load(std::memory_order_acquire);
                return m_clockBase + int64_t(played * 90000u / movie::PssAudio::kRate);
            }

            // Until the soundtrack reaches `pts`, or the player skips.
            void waitUntil(int64_t pts)
            {
                while (!stopping() && clock() < pts)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            // Retail skips on JoypadData buttons once the minimum time is up
            // (0x21bde4): green (kPad_R2, 0x2) on a guitar, else X (0x40), and
            // neither while Start (0x800) is held. Port 0 is always a guitar
            // (host/pad.cpp), so green.
            bool stopping()
            {
                if (m_done)
                    return true;
                const uint16_t pressed = hostPad().pressed;
                const bool skip = Clock::now() >= m_skipAfter && (pressed & pad::kR2) && !(pressed & pad::kStart);
                m_done = skip || m_runtime.isStopRequested();
                return m_done;
            }

            DiscImage &m_disc;
            DiscImage::Extent m_extent;
            PS2Runtime &m_runtime;
            Clock::time_point m_skipAfter;
            mpeg2dec_t *m_decoder = nullptr;
            movie::PssAudio m_audio;
            bool m_audioBad = false;
            // Half a second of pictures decoded ahead, so the soundtrack in
            // between keeps arriving while they wait to be shown.
            struct Queued
            {
                int64_t pts = 0;
                movie::Picture picture;
            };
            static constexpr size_t kQueueDepth = 15;
            std::deque<Queued> m_queue;
            std::vector<movie::Picture> m_spare;
            movie::Picture m_shown;
            int64_t m_clockBase = -1; // the soundtrack's first PTS
            int64_t m_lastPts = 0;
            int64_t m_period = 3003;
            bool m_done = false;
        };

        // PlayMovie(path, minSkipSeconds, alloc, free), retail 0x21bb60. Retail
        // plays the PSS through libmpeg, the IPU and a libsdr channel until
        // sceMpegIsEnd or a skip and returns 1 (0x21bebc), or 0 when the file
        // will not open. Its one caller, MetaPanel::OnPlayMovie (0x134c08),
        // passes "videos/<name>" and 0.
        void playMovie(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const std::string path = reinterpret_cast<const char *>(getMemPtr(rdram, GPR_U32(ctx, 4)));
            const float minSkipSeconds = ctx->f[12];
            uint32_t result = 0;
            DiscImage *disc = ps2ConfiguredDisc();
            DiscImage::Extent extent;
            if (disc && disc->find(path, extent) && !extent.isDir)
            {
                std::cerr << "[movie] playing " << path << std::endl;
                Player(*disc, extent, minSkipSeconds, *runtime).run();
                result = 1;
            }
            else
            {
                std::cerr << "[movie] couldn't open " << path << std::endl;
            }
            SET_GPR_U32(ctx, 2, result);
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void installMovies(PS2Runtime &runtime, const Addresses &addresses)
    {
        runtime.replaceFunction(addresses.playMovie, playMovie);
    }

    bool moviePlaying()
    {
        return s_playing.load(std::memory_order_relaxed);
    }
}
