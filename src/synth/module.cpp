#include "synth/module.h"

#include "synth/adpcm.h"
#include "synth/fixed.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numbers>
#include <set>
#include <utility>

namespace gh2::synth
{
    namespace
    {
        constexpr uint32_t kTickFrames = Spu::kRate / 200u; // 5 ms
        constexpr int32_t kSpeedOne = 1000;      // speeds are in thousandths
        constexpr int16_t kFullVolume = 0x7fff;

        // Stream buffers: an IOP ring per channel the EE fills on request, and
        // a ring of 2 KB blocks in sound RAM that the channel's voice loops
        // over, refilled from the IOP ring behind it.
        constexpr uint32_t kIopRingBytes = 0x4000u;
        constexpr uint32_t kRequestBelow = 0x2000u;
        constexpr uint32_t kBlockBytes = 0x800u;
        constexpr uint32_t kSpuHeap = 0x5010u;

        // libsd's default ADSR as the module sets it (0x55f0).
        constexpr uint16_t kAdsr1 = 0x8f1fu;
        constexpr uint16_t kAdsr2 = 0x03c7u;

        // Replies, CtlDispatch_impl's table on the EE.
        constexpr uint32_t kReplyStreamData = 0u;
        constexpr uint32_t kReplyStreamEnd = 1u;
        constexpr uint32_t kReplyStreamReady = 2u;
        constexpr uint32_t kReplyStreamPosition = 3u;
        constexpr uint32_t kReplySlipOffset = 4u;
        constexpr uint32_t kReplySampleProgress = 9u;
        constexpr uint32_t kReplySampleDone = 11u;
        constexpr uint32_t kReplySpuDone = 13u;
        constexpr uint32_t kReplyTerminated = 14u;

        // Channel states (+0x4038).
        enum : int32_t
        {
            kPriming = 0,
            kPrimed = 1,
            kPlaying = 2,
            kPaused = 3,
            kStarved = 4,
            kEnded = 5,
        };

        // Stream states (+0x8).
        enum : int32_t
        {
            kStreamPriming = 0,
            kStreamReady = 1,
            kStreamPlaying = 2,
            kStreamPaused = 3,
            kStreamEnded = 4,
        };

        uint32_t u32(const uint8_t *p, uint32_t off)
        {
            uint32_t v;
            std::memcpy(&v, p + off, 4);
            return v;
        }

        int32_t s32(const uint8_t *p, uint32_t off)
        {
            return static_cast<int32_t>(u32(p, off));
        }

        int16_t s16(const uint8_t *p, uint32_t off)
        {
            int16_t v;
            std::memcpy(&v, p + off, 2);
            return v;
        }

        // The pan curve at .data 0x5dd0: a quarter sine, 255 * sin(i * pi / 508)
        // for i in 0..254, checked byte for byte against the IRX.
        const std::array<int32_t, 255> &panCurve()
        {
            static const std::array<int32_t, 255> table = []() {
                std::array<int32_t, 255> t{};
                for (int32_t i = 0; i < 255; ++i)
                    t[i] = static_cast<int32_t>(std::lround(255.0 * std::sin(i * std::numbers::pi / 508.0)));
                return t;
            }();
            return table;
        }

        // One side's gain (0x4d50, 0x4e78): the curve walked around a circle
        // of 1016 steps, so pan past +-127 swings into negative, phase
        // inverted gains, the rear positions. Centre is 180/255 each side.
        int32_t panGain(int32_t volume, int32_t p)
        {
            const int32_t x = wrap(p, 1016);
            const int32_t q = x / 254;
            const int32_t r = x % 254;
            const std::array<int32_t, 255> &t = panCurve();
            const int32_t g = q == 0 ? t[r] : q == 1 ? t[254 - r] : q == 2 ? -t[r] : -t[254 - r];
            return static_cast<int16_t>((g * volume + 127) / 255);
        }

        // SD_VPARAM_PITCH for a rate at a speed in thousandths (0x5764).
        uint16_t pitchFor(uint32_t rate, int32_t speed)
        {
            const int32_t base = static_cast<int32_t>(static_cast<uint64_t>(rate) * Spu::kPitchOne / Spu::kRate);
            return static_cast<uint16_t>(base * speed / kSpeedOne);
        }

        std::vector<uint8_t> words(std::initializer_list<uint32_t> list)
        {
            std::vector<uint8_t> data(list.size() * 4u);
            size_t i = 0;
            for (uint32_t w : list)
                std::memcpy(data.data() + 4u * i++, &w, 4);
            return data;
        }
    }

    struct Module::Channel
    {
        uint32_t index = 0;
        uint32_t rate = 0;
        bool slip = false;
        int32_t state = kPriming;

        int16_t volume = kFullVolume;
        int16_t pan = 0;
        bool fx = false;
        int32_t core = -1;
        int32_t speed = kSpeedOne;
        int32_t slipSpeed = kSpeedOne;
        uint16_t adsr1 = kAdsr1, adsr2 = kAdsr2;

        std::deque<uint8_t> iop;
        bool endOfData = false;
        uint32_t padded = 0;

        uint32_t base = 0;   // sound RAM ring
        uint32_t blocks = 0;
        uint32_t next = 0;   // the block to fill next
        uint32_t laps = 0;
        uint32_t readAt = 0; // the reference voice's NAX, read at refill

        int32_t main = -1;      // the voice heard
        int32_t reference = -1; // the voice refills follow; main unless slipping
        int32_t link = -1;      // 0x19e: the channel whose slip target this one takes
        uint32_t target = 0;    // the last slip target, from base
        int32_t slipOffset = 0; // +0x4060: the last reported, in samples
    };

    struct Module::Stream
    {
        uint32_t id = 0;
        int32_t state = kStreamPriming;
        bool requested = false;
        std::vector<Channel> channels;
        std::vector<std::pair<uint32_t, uint32_t>> spuBlocks; // first, count
    };

    struct Module::Sample
    {
        uint32_t id = 0;
        uint32_t address = 0;
        uint32_t rate = 0;
        uint32_t startOffset = 0;
        int16_t volume = kFullVolume;
        int16_t pan = 0;
        int32_t speed = kSpeedOne;
        uint16_t adsr1 = kAdsr1, adsr2 = kAdsr2;
        int32_t core = -1;
        int32_t voice = -1;
    };

    Module::Module()
    {
        reset();
    }

    Module::~Module() = default;

    void Module::reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_spu.reset();
        m_streams.clear();
        m_samples.clear();
        freeAllVoices();
        m_voiceState = {};
        m_keyOn.assign(Spu::kVoices, false);
        m_keyOff.assign(Spu::kVoices, false);
        m_voicesOnCore1 = false;
        m_slipMs = 0;
        m_spuBlockUsed.clear();
        m_uploadDone = m_terminate = false;
        m_dataStream = m_dataChannel = -1;
        m_tickReplies.clear();
        m_sent.clear();
        m_framesToTick = 0;
    }

    Module::Stream *Module::stream(uint32_t id)
    {
        auto it = m_streams.find(id);
        return it == m_streams.end() ? nullptr : it->second.get();
    }

    Module::Sample *Module::sample(uint32_t id)
    {
        auto it = m_samples.find(id);
        return it == m_samples.end() ? nullptr : it->second.get();
    }

    void Module::queue(uint32_t cmd, std::initializer_list<uint32_t> list)
    {
        m_tickReplies.push_back({cmd, words(list)});
    }

    // Voices ------------------------------------------------------------------

    // Least recently freed first (0x458c), from either core unless the
    // caller or command 5 names one (0x42fc, 0x433c).
    int32_t Module::allocVoice(int32_t core)
    {
        if (core < 0 && m_voicesOnCore1)
            core = 1;
        for (auto it = m_freeVoices.begin(); it != m_freeVoices.end(); ++it)
        {
            if (core < 0 || *it / static_cast<int32_t>(Spu::kVoicesPerCore) == core)
            {
                const int32_t v = *it;
                m_freeVoices.erase(it);
                m_voiceState[v] = {};
                return v;
            }
        }
        return -1;
    }

    // 0x4420 then 0x43a4: a key on still pending is taken back rather than
    // answered with a key off.
    void Module::freeVoice(int32_t voice)
    {
        if (voice < 0)
            return;
        if (m_keyOn[voice])
            m_keyOn[voice] = false;
        else
            m_keyOff[voice] = true;
        m_freeVoices.push_back(voice);
    }

    void Module::freeAllVoices()
    {
        m_freeVoices.clear();
        for (uint32_t v = 0; v < Spu::kVoices; ++v)
            m_freeVoices.push_back(static_cast<int32_t>(v));
    }

    void Module::keyOn(int32_t voice)
    {
        if (voice >= 0)
            m_keyOn[voice] = true;
    }

    // 0x4648: VOLL and VOLR from a volume and pan, held back while paused.
    void Module::setVolumePan(int32_t voice, int16_t volume, int16_t pan)
    {
        VoiceState &vs = m_voiceState[voice];
        vs.volume = volume;
        vs.pan = pan;
        if (vs.paused)
            return;
        const uint16_t left = static_cast<uint16_t>(panGain(volume, pan + 0x17d)) >> 1;
        const uint16_t right = static_cast<uint16_t>(panGain(volume, pan + 0x7f)) >> 1;
        m_spu.setVolume(static_cast<uint32_t>(voice), left, right);
    }

    // 0x447c: a paused voice stays keyed on with no volume and no pitch.
    void Module::pause(int32_t voice)
    {
        m_voiceState[voice].paused = true;
        m_spu.setVolume(static_cast<uint32_t>(voice), 0u, 0u);
        m_spu.setPitch(static_cast<uint32_t>(voice), 0u);
    }

    // Unpaused, a voice takes back its pitch; its volume comes back with the
    // caller's next setVolumePan.
    void Module::resume(int32_t voice)
    {
        m_voiceState[voice].paused = false;
        m_spu.setPitch(static_cast<uint32_t>(voice), m_voiceState[voice].pitch);
    }

    void Module::setPitch(int32_t voice, uint16_t pitch)
    {
        m_voiceState[voice].pitch = pitch;
        if (!m_voiceState[voice].paused)
            m_spu.setPitch(static_cast<uint32_t>(voice), pitch);
    }

    // 0x487c: the tick's key offs, then its key ons, except a voice keyed off
    // in the same tick, which keys on a tick later.
    void Module::flushKeys()
    {
        std::vector<bool> later(Spu::kVoices, false);
        for (uint32_t v = 0; v < Spu::kVoices; ++v)
        {
            if (m_keyOff[v])
                m_spu.keyOff(v);
            if (m_keyOn[v])
            {
                if (m_keyOff[v])
                    later[v] = true;
                else
                    m_spu.keyOn(v);
            }
        }
        m_keyOn = later;
        m_keyOff.assign(Spu::kVoices, false);
    }

    // Commands ----------------------------------------------------------------

    void Module::writeRam(uint32_t address, const uint8_t *data, uint32_t len)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // A voice whose stop is still queued (CtlClientPoll) would play the
        // new bytes: retail's upload goes through that queue, behind it.
        m_spu.silence(address, len);
        m_spu.write(address, data, len);
    }

    void Module::command(uint32_t cmd, const uint8_t *d, uint32_t len)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        switch (cmd)
        {
        case 0x0: // SynthConfig {?, max samples, max streams, SPU blocks, ?, slip ms}
            if (len >= 24u)
            {
                m_slipMs = u32(d, 20);
                m_spuBlockUsed.assign(u32(d, 12), false);
            }
            m_spu.setMasterVolume(kFullVolume >> 1, kFullVolume >> 1);
            break;
        case 0x1: // Terminate, taken at the next tick
            m_terminate = true;
            break;
        case 0x2: // master volume for both cores
        {
            const uint16_t v = static_cast<uint16_t>(s16(d, 0)) >> 1;
            m_spu.setMasterVolume(v, v);
            break;
        }
        case 0x3: // sceSdEffectAttr {core, mode, s16 depthL, s16 depthR, delay, feedback}
            m_spu.reverb(u32(d, 0)).setMode(u32(d, 4));
            m_spu.reverb(u32(d, 0)).setDepth(s16(d, 8), s16(d, 10));
            break;
        case 0x4: // the same, for depth, delay and feedback only
            m_spu.reverb(u32(d, 0)).setDepth(s16(d, 8), s16(d, 10));
            break;
        case 0x5: // effect chain: new voices on core 1
            m_voicesOnCore1 = u32(d, 0) != 0u;
            break;
        case 0xc8: // SPUSendPoll's destination
            m_uploadDest = u32(d, 0);
            break;
        case 0xc9: // and its bytes; done a tick later, as the DMA would be
            m_spu.write(m_uploadDest, d, len);
            m_uploadDone = true;
            break;

        case 0x190: // StreamInfoArg {id, s16 channels, int rate[15], s8 slip[15]}
        {
            // The IRX (0x3e48) never checks the id and its lookup (0x3c3c)
            // finds the first, so a second stream under one id is never
            // reached: the first plays on.
            if (stream(u32(d, 0)))
            {
                static bool s_logged = false;
                if (!s_logged)
                    std::cerr << "[synth] stream id " << u32(d, 0) << " created twice; the first kept" << std::endl;
                s_logged = true;
                break;
            }
            auto s = std::make_unique<Stream>();
            s->id = u32(d, 0);
            const uint32_t count = std::min<uint32_t>(static_cast<uint16_t>(s16(d, 4)), 15u);
            s->channels.resize(count);
            for (uint32_t c = 0; c < count; ++c)
            {
                Channel &ch = s->channels[c];
                ch.index = c;
                ch.rate = u32(d, 8u + c * 4u);
                ch.slip = d[0x44u + c] != 0u;
                // Two blocks, or with slip enough for the window either way
                // plus four (0x2d34).
                ch.blocks = 2u;
                if (ch.slip)
                    ch.blocks = 2u * (m_slipMs * ch.rate / 1000u / 512u / 7u) + 4u;
                // First fit in the stream heap, 2 KB blocks from 0x5010 (0x2950).
                uint32_t run = 0;
                for (uint32_t b = 0; b < m_spuBlockUsed.size(); ++b)
                {
                    run = m_spuBlockUsed[b] ? 0u : run + 1u;
                    if (run == ch.blocks)
                    {
                        const uint32_t first = b + 1u - run;
                        std::fill_n(m_spuBlockUsed.begin() + first, run, true);
                        s->spuBlocks.emplace_back(first, run);
                        ch.base = kSpuHeap + first * kBlockBytes;
                        break;
                    }
                }
            }
            m_streams[s->id] = std::move(s);
            break;
        }
        case 0x191: // {id, s16 ch, s16 core}: the voice's core
        case 0x195: // {id, s16 ch, s16 volume, s16 pan}
        case 0x196: // {id, s16 ch, s16 on}: the reverb send
        case 0x19f: // {id, s16 ch, u16 adsr1, u16 adsr2}
        {
            Stream *s = stream(u32(d, 0));
            const uint32_t c = static_cast<uint16_t>(s16(d, 4));
            if (!s || c >= s->channels.size())
                break;
            Channel &ch = s->channels[c];
            if (cmd == 0x191)
                ch.core = s16(d, 6);
            else if (cmd == 0x195)
            {
                ch.volume = s16(d, 6);
                ch.pan = s16(d, 8);
                if (ch.state == kPlaying && ch.main >= 0)
                    setVolumePan(ch.main, ch.volume, ch.pan);
            }
            else if (cmd == 0x196)
            {
                ch.fx = s16(d, 6) != 0;
                if (ch.state == kPlaying && ch.main >= 0)
                    m_spu.setReverbSend(static_cast<uint32_t>(ch.main), ch.fx);
            }
            else
            {
                // 0x3404: kept, and set on the heard voice if there is one.
                ch.adsr1 = static_cast<uint16_t>(s16(d, 6));
                ch.adsr2 = static_cast<uint16_t>(s16(d, 8));
                if (ch.main >= 0)
                    m_spu.setAdsr(static_cast<uint32_t>(ch.main), ch.adsr1, ch.adsr2);
            }
            break;
        }
        case 0x192: // play, or resume (0x22ec)
            if (Stream *s = stream(u32(d, 0)))
            {
                for (Channel &ch : s->channels)
                    playChannel(ch);
                s->state = kStreamPlaying;
            }
            break;
        case 0x193: // pause (0x2358)
            if (Stream *s = stream(u32(d, 0)))
            {
                for (Channel &ch : s->channels)
                    pauseChannel(ch);
                s->state = kStreamPaused;
            }
            break;
        case 0x194: // destroy
            if (Stream *s = stream(u32(d, 0)))
            {
                // The reference is a voice of its own only when slipping (0x2e18).
                for (Channel &ch : s->channels)
                {
                    freeVoice(ch.main);
                    if (ch.slip)
                        freeVoice(ch.reference);
                }
                for (auto [first, count] : s->spuBlocks)
                    std::fill_n(m_spuBlockUsed.begin() + first, count, false);
                m_streams.erase(s->id);
            }
            break;
        case 0x197: // {id, s16 speed * 1000}, every channel
            if (Stream *s = stream(u32(d, 0)))
                for (Channel &ch : s->channels)
                    setChannelSpeed(ch, s16(d, 4));
            break;
        case 0x198: // {id, s16 ch}: where the next 0x199 goes
            m_dataStream = s32(d, 0);
            m_dataChannel = static_cast<uint16_t>(s16(d, 4));
            break;
        case 0x199: // up to 0x1000 bytes of the channel's ADPCM (0x2e8c)
        {
            Stream *s = m_dataStream >= 0 ? stream(static_cast<uint32_t>(m_dataStream)) : nullptr;
            const int32_t c = m_dataChannel;
            m_dataStream = m_dataChannel = -1;
            if (!s || c < 0 || static_cast<uint32_t>(c) >= s->channels.size())
                break;
            Channel &ch = s->channels[static_cast<uint32_t>(c)];
            // Dropped whole when it does not fit (0x5460). Every line's
            // flags become repeat-only; flag bit 7 marks the stream's end
            // (0x38a0).
            if (ch.iop.size() + len > kIopRingBytes)
                break;
            for (uint32_t off = 0; off < len; ++off)
            {
                uint8_t b = d[off];
                if (off % kAdpcmBlockBytes == 1u)
                {
                    if (b & 0x80u)
                        ch.endOfData = true;
                    b = kAdpcmRepeat;
                }
                ch.iop.push_back(b);
            }
            break;
        }
        case 0x19a: // {id}: the requests are answered
            if (Stream *s = stream(u32(d, 0)))
                s->requested = false;
            break;
        case 0x19b: // {id, s16 ch, int samples}: slip to an offset from the song
        case 0x19c: // {id, int ch}: stop slipping
        case 0x19d: // {id, int ch, int speed * 1000}
        {
            Stream *s = stream(u32(d, 0));
            const uint32_t c = cmd == 0x19b ? static_cast<uint16_t>(s16(d, 4)) : u32(d, 4);
            if (!s || c >= s->channels.size())
                break;
            Channel &ch = s->channels[c];
            if (cmd == 0x19b)
                slipJump(*s, ch, s32(d, 8));
            else if (cmd == 0x19c)
            {
                freeVoice(ch.main);
                ch.main = -1;
            }
            else
            {
                ch.slipSpeed = s32(d, 8);
                setChannelSpeed(ch, ch.speed);
            }
            break;
        }
        case 0x19e: // {id, int a, int b}: b slips with a
            if (Stream *s = stream(u32(d, 0)))
            {
                const uint32_t a = u32(d, 4), b = u32(d, 8);
                if (a < s->channels.size() && b < s->channels.size())
                    s->channels[b].link = static_cast<int32_t>(a);
            }
            break;

        case 0x2bc: // {id, spu address, rate}
        {
            auto s = std::make_unique<Sample>();
            s->id = u32(d, 0);
            s->address = u32(d, 4);
            s->rate = u32(d, 8);
            m_samples[s->id] = std::move(s);
            break;
        }
        case 0x2bd: // destroy
            if (Sample *s = sample(u32(d, 0)))
            {
                stopSample(*s);
                m_samples.erase(s->id);
            }
            break;
        case 0x2be: // start
            if (Sample *s = sample(u32(d, 0)))
                startSample(*s);
            break;
        case 0x2bf: // stop
            if (Sample *s = sample(u32(d, 0)))
                stopSample(*s);
            break;
        case 0x2c0: // {id, int paused}
            if (Sample *s = sample(u32(d, 0)); s && s->voice >= 0)
            {
                if (u32(d, 4) != 0u)
                    pause(s->voice);
                else
                {
                    resume(s->voice);
                    setVolumePan(s->voice, s->volume, s->pan);
                }
            }
            break;
        case 0x2c1: // {id, s16 volume, s16 pan}
            if (Sample *s = sample(u32(d, 0)))
            {
                s->volume = s16(d, 4);
                s->pan = s16(d, 6);
                if (s->voice >= 0)
                    setVolumePan(s->voice, s->volume, s->pan);
            }
            break;
        case 0x2c2: // {id, int speed * 1000}
            if (Sample *s = sample(u32(d, 0)))
            {
                s->speed = s32(d, 4);
                if (s->voice >= 0)
                    setPitch(s->voice, pitchFor(s->rate, s->speed));
            }
            break;
        case 0x2c3: // {id, u16 adsr1, u16 adsr2}
            if (Sample *s = sample(u32(d, 0)))
            {
                s->adsr1 = static_cast<uint16_t>(s16(d, 4));
                s->adsr2 = static_cast<uint16_t>(s16(d, 6));
                if (s->voice >= 0)
                    m_spu.setAdsr(static_cast<uint32_t>(s->voice), s->adsr1, s->adsr2);
            }
            break;
        case 0x2c4: // {id, int core}: the voice's core, and a reverb send
            if (Sample *s = sample(u32(d, 0)))
                s->core = s32(d, 4);
            break;
        case 0x2c5: // {id, int byte offset}, for the next start
            if (Sample *s = sample(u32(d, 0)))
                s->startOffset = u32(d, 4) / kAdpcmBlockBytes * kAdpcmBlockBytes;
            break;
        default:
            // 0x6 input FX and 0x12c..0x136 mics: nothing sounds from them.
            // 0x258 level meters: not modelled, so reply 10 never comes.
            break;
        }
    }

    // Streams -----------------------------------------------------------------

    // 0x2f68: a voice for the channel at `address`, set up and keyed on,
    // pitched without the slip speed.
    int32_t Module::keyChannel(Channel &ch, uint32_t address)
    {
        const int32_t v = allocVoice(ch.core);
        if (v < 0)
            return -1;
        setVolumePan(v, ch.volume, ch.pan);
        setPitch(v, pitchFor(ch.rate, ch.speed));
        m_spu.setAdsr(static_cast<uint32_t>(v), ch.adsr1, ch.adsr2);
        m_spu.setStart(static_cast<uint32_t>(v), address);
        m_spu.setReverbSend(static_cast<uint32_t>(v), ch.fx);
        return v;
    }

    // 0x301c: a primed channel keys on at its ring; a slipping one keys only
    // a silent reference voice, its heard voice waiting for the first
    // 0x19b. A paused one resumes. Any other state ignores it.
    void Module::playChannel(Channel &ch)
    {
        if (ch.state == kPrimed)
        {
            ch.state = kPlaying;
            if (ch.slip)
            {
                ch.main = -1;
                ch.reference = keyChannel(ch, ch.base);
                if (ch.reference >= 0)
                    setVolumePan(ch.reference, 0, 0);
            }
            else
            {
                ch.main = ch.reference = keyChannel(ch, ch.base);
            }
            ch.readAt = ch.base;
            keyOn(ch.reference);
        }
        else if (ch.state == kPaused)
        {
            ch.state = kPlaying;
            if (ch.main >= 0)
            {
                resume(ch.main);
                setVolumePan(ch.main, ch.volume, ch.pan);
                setPitch(ch.main, pitchFor(ch.rate, ch.speed * ch.slipSpeed / kSpeedOne));
                m_spu.setReverbSend(static_cast<uint32_t>(ch.main), ch.fx);
            }
            if (ch.slip && ch.reference >= 0)
            {
                resume(ch.reference);
                setVolumePan(ch.reference, 0, 0);
                setPitch(ch.reference, pitchFor(ch.rate, ch.speed));
            }
        }
    }

    // 0x32f0.
    void Module::pauseChannel(Channel &ch)
    {
        if (ch.state != kPlaying && ch.state != kStarved)
            return;
        ch.state = kPaused;
        if (ch.main >= 0)
            pause(ch.main);
        if (ch.reference >= 0 && ch.reference != ch.main)
            pause(ch.reference);
    }

    // 0x31b4: the heard voice at speed times slip speed, the reference at
    // speed alone, applied only while playing.
    void Module::setChannelSpeed(Channel &ch, int32_t speed)
    {
        ch.speed = speed;
        if (ch.state != kPlaying)
            return;
        if (ch.main >= 0)
            setPitch(ch.main, pitchFor(ch.rate, ch.speed * ch.slipSpeed / kSpeedOne));
        if (ch.slip && ch.reference >= 0)
            setPitch(ch.reference, pitchFor(ch.rate, ch.speed));
    }

    // 0x3490: the heard voice re-keyed `offset` samples from where the
    // reference was at the last refill (not where it is now: the first jump
    // comes in the same batch as the play, before the reference is keyed on),
    // or where its linked channel went, with the ring's base as its loop.
    void Module::slipJump(Stream &s, Channel &ch, int32_t offset)
    {
        if (ch.state != kPlaying)
            return;
        freeVoice(ch.main);
        ch.main = -1;
        const uint32_t from = ch.readAt / kAdpcmBlockBytes * kAdpcmBlockBytes;
        if (from < ch.base || from >= ch.base + ch.blocks * kBlockBytes)
            return;
        const int32_t ring = static_cast<int32_t>(ch.blocks * kBlockBytes);
        int32_t at;
        if (ch.link >= 0)
            at = static_cast<int32_t>(s.channels[static_cast<uint32_t>(ch.link)].target);
        else
            at = static_cast<int32_t>(from - ch.base) + adpcmBytes(offset);
        at = wrap(at, ring);
        ch.target = static_cast<uint32_t>(at);
        ch.main = keyChannel(ch, ch.base + ch.target);
        if (ch.main < 0)
            return;
        m_spu.setLoop(static_cast<uint32_t>(ch.main), ch.base);
        keyOn(ch.main);
    }

    // 0x32a8: 16-byte lines played, from the NAX the last refill read.
    uint32_t Module::position(const Channel &ch) const
    {
        const uint32_t ringLines = ch.blocks * (kBlockBytes / kAdpcmBlockBytes);
        int32_t lines = static_cast<int32_t>(ch.readAt - ch.base) / static_cast<int32_t>(kAdpcmBlockBytes);
        if (lines < 0 || lines > static_cast<int32_t>(ringLines))
            lines = 0;
        return ch.laps * ringLines + static_cast<uint32_t>(lines);
    }

    // 0x38f0: the next ring block from the IOP ring, padded with silent
    // lines past the end of the data. The ring's last line loops it and its
    // first marks where.
    void Module::transfer(Channel &ch)
    {
        uint8_t block[kBlockBytes];
        const uint32_t take = std::min<uint32_t>(kBlockBytes, static_cast<uint32_t>(ch.iop.size()));
        std::copy_n(ch.iop.begin(), take, block);
        ch.iop.erase(ch.iop.begin(), ch.iop.begin() + take);
        if (take < kBlockBytes)
        {
            for (uint32_t off = take; off < kBlockBytes; ++off)
                block[off] = off % kAdpcmBlockBytes == 1u ? kAdpcmRepeat : 0u;
            ++ch.padded;
        }
        if (ch.next == ch.blocks - 1u)
            block[kBlockBytes - kAdpcmBlockBytes + 1u] |= kAdpcmLoopEnd;
        if (ch.next == 0u)
            block[1] |= kAdpcmLoopStart;
        m_spu.write(ch.base + ch.next * kBlockBytes, block, kBlockBytes);
        ch.next = (ch.next + 1u) % ch.blocks;
    }

    void Module::serviceChannel(Channel &ch)
    {
        if (ch.blocks == 0u || ch.base == 0u)
            return;
        if (ch.state == kPriming)
        {
            // Fill the whole ring, then wait to be played.
            while (ch.state == kPriming && (ch.iop.size() >= kBlockBytes || ch.endOfData))
            {
                transfer(ch);
                if (ch.next == 0u)
                    ch.state = kPrimed;
            }
            return;
        }
        if (ch.state != kPlaying || ch.reference < 0 || m_keyOn[ch.reference])
            return;

        // Refill (0x35b4) while the reference voice plays outside the half of
        // the ring after the block to fill next.
        for (;;)
        {
            ch.readAt = m_spu.nextAddress(static_cast<uint32_t>(ch.reference));
            const uint32_t playing = (ch.readAt - ch.base) / kBlockBytes % ch.blocks;
            if ((playing + ch.blocks - ch.next) % ch.blocks < ch.blocks / 2u)
                return;
            if (ch.iop.size() < kBlockBytes && !ch.endOfData)
            {
                starveChannel(ch);
                return;
            }
            if ((ch.next + ch.blocks / 2u) % ch.blocks == 0u)
                ++ch.laps;
            transfer(ch);
            if (ch.padded >= ch.blocks)
            {
                ch.state = kEnded;
                return;
            }
        }
    }

    // Starved (0x37f4): both voices hold still until data comes.
    void Module::starveChannel(Channel &ch)
    {
        ch.state = kStarved;
        if (ch.main >= 0)
            m_spu.setPitch(static_cast<uint32_t>(ch.main), 0u);
        if (ch.reference >= 0)
            m_spu.setPitch(static_cast<uint32_t>(ch.reference), 0u);
    }

    // 0x3874: speed, then volume and pan, put back.
    void Module::unstarveChannel(Channel &ch)
    {
        ch.state = kPlaying;
        setChannelSpeed(ch, ch.speed);
        if (ch.main >= 0)
            setVolumePan(ch.main, ch.volume, ch.pan);
    }

    void Module::tickStream(Stream &s)
    {
        // A stream's channels starve and come back together. The IRX starves
        // each on its own, so a hitch that drains one ring before another
        // leaves them apart for the rest of the song (the guitar off the
        // band). Here none plays on while another waits for data, and they
        // come back with half an IOP ring each, not the block the IRX waits
        // for, so a hitch's backlog doesn't starve them again a block later.
        const auto starved = [](const Channel &ch) { return ch.state == kStarved; };
        const bool fed = std::all_of(s.channels.begin(), s.channels.end(), [&](const Channel &ch) {
            return !starved(ch) || ch.iop.size() >= kRequestBelow || ch.endOfData;
        });
        if (fed)
            for (Channel &ch : s.channels)
                if (starved(ch))
                    unstarveChannel(ch);

        for (Channel &ch : s.channels)
            serviceChannel(ch);

        if (std::any_of(s.channels.begin(), s.channels.end(), starved))
            for (Channel &ch : s.channels)
                if (ch.state == kPlaying)
                    starveChannel(ch);

        const auto all = [&s](int32_t state) {
            return std::all_of(s.channels.begin(), s.channels.end(),
                               [state](const Channel &ch) { return ch.state == state; });
        };
        if (s.state == kStreamPriming && all(kPrimed))
        {
            s.state = kStreamReady;
            queue(kReplyStreamReady, {s.id});
        }
        if (s.state != kStreamEnded && !s.channels.empty() && all(kEnded))
        {
            s.state = kStreamEnded;
            queue(kReplyStreamEnd, {s.id});
        }

        // Data requests: every channel's free room, once any runs low, and
        // not again until 0x19a says they were answered.
        if (s.state <= kStreamPaused && !s.requested)
        {
            const bool low = std::any_of(s.channels.begin(), s.channels.end(), [](const Channel &ch) {
                return ch.state != kEnded && ch.iop.size() < kRequestBelow;
            });
            if (low)
            {
                for (const Channel &ch : s.channels)
                {
                    const uint32_t room = kIopRingBytes - static_cast<uint32_t>(ch.iop.size());
                    if (ch.state != kEnded && room != 0u)
                        queue(kReplyStreamData, {s.id, ch.index, room});
                }
                s.requested = true;
            }
        }

        if (s.state >= kStreamReady && s.state <= kStreamPaused)
        {
            const uint32_t pos = s.channels.empty() || s.channels[0].reference < 0 ? 0u : position(s.channels[0]);
            queue(kReplyStreamPosition, {s.id, pos});
            // Slip offsets in samples, the heard voice against the reference
            // (0x3644), wrapped to half the ring either way. Taken only while
            // the heard voice plays inside the ring (0x2c54): one keyed on
            // and not yet started keeps the last.
            for (Channel &ch : s.channels)
            {
                if (!ch.slip || ch.main < 0 || ch.reference < 0)
                    continue;
                const int32_t ring = static_cast<int32_t>(ch.blocks * kBlockBytes);
                const uint32_t heard = m_spu.nextAddress(static_cast<uint32_t>(ch.main));
                if (heard >= ch.base && heard < ch.base + static_cast<uint32_t>(ring))
                {
                    int32_t off = static_cast<int32_t>(heard) -
                                  static_cast<int32_t>(m_spu.nextAddress(static_cast<uint32_t>(ch.reference)));
                    if (off > ring / 2)
                        off -= ring;
                    else if (off < -ring / 2)
                        off += ring;
                    ch.slipOffset = adpcmSamples(off);
                }
                queue(kReplySlipOffset, {s.id, ch.index, static_cast<uint32_t>(ch.slipOffset)});
            }
        }
    }

    // Samples -----------------------------------------------------------------

    // 0xd04: any voice it had goes; a new one plays from the start offset,
    // with a reverb send exactly when a core was named.
    void Module::startSample(Sample &s)
    {
        stopSample(s);
        s.voice = allocVoice(s.core);
        if (s.voice < 0)
            return;
        const uint32_t v = static_cast<uint32_t>(s.voice);
        setVolumePan(s.voice, s.volume, s.pan);
        setPitch(s.voice, pitchFor(s.rate, s.speed));
        m_spu.setAdsr(v, s.adsr1, s.adsr2);
        m_spu.setStart(v, s.address + s.startOffset);
        m_spu.setReverbSend(v, s.core != -1);
        keyOn(s.voice);
    }

    void Module::stopSample(Sample &s)
    {
        freeVoice(s.voice);
        s.voice = -1;
    }

    // 0xe58: a voice plays while its key on is pending, until ENDX, and past
    // ENDX while its envelope holds (a loop). Playing, it reports its byte
    // offset; done, it goes and says so.
    void Module::tickSamples()
    {
        for (auto &[id, s] : m_samples)
        {
            if (s->voice < 0)
                continue;
            const uint32_t v = static_cast<uint32_t>(s->voice);
            if (m_keyOn[v] || !m_spu.ended(v) || m_spu.envelope(v) != 0u)
            {
                queue(kReplySampleProgress, {id, m_spu.nextAddress(v) - s->address});
                continue;
            }
            freeVoice(s->voice);
            s->voice = -1;
            queue(kReplySampleDone, {id});
        }
    }

    // The tick --------------------------------------------------------------

    void Module::tick()
    {
        if (m_terminate)
        {
            // 0x1754: everything goes, libsd starts over, and the EE hears.
            m_terminate = false;
            m_streams.clear();
            m_samples.clear();
            for (uint32_t v = 0; v < Spu::kVoices; ++v)
                m_spu.keyOff(v);
            freeAllVoices();
            // The IRX skips this tick's key flush (0x17fc): nothing pending
            // lands on a voice no stream owns any more.
            std::fill(m_keyOn.begin(), m_keyOn.end(), false);
            std::fill(m_keyOff.begin(), m_keyOff.end(), false);
            std::fill(m_spuBlockUsed.begin(), m_spuBlockUsed.end(), false);
            queue(kReplyTerminated);
        }
        for (auto &[id, s] : m_streams)
            tickStream(*s);
        if (m_uploadDone)
        {
            m_uploadDone = false;
            queue(kReplySpuDone);
        }
        tickSamples();
        flushKeys();
        const auto now = ps2x::host_clock::now();
        for (Reply &r : m_tickReplies)
        {
            r.at = now;
            m_sent.push_back(std::move(r));
        }
        m_tickReplies.clear();
    }

    void Module::render(float *out, size_t frames)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (frames > 0u)
        {
            if (m_framesToTick == 0u)
            {
                tick();
                m_framesToTick = kTickFrames;
            }
            const size_t n = std::min<size_t>(frames, m_framesToTick);
            m_spu.mix(out, n);
            out += n * 2u;
            frames -= n;
            m_framesToTick -= static_cast<uint32_t>(n);
        }
    }

    // Positions, slip offsets and sample progress each overwrite what the
    // EE held, so of several since the last poll only the newest is passed.
    void Module::takeReplies(std::vector<Reply> &out)
    {
        std::vector<Reply> sent;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sent.swap(m_sent);
        }
        std::set<std::pair<uint32_t, uint64_t>> seen;
        std::vector<Reply> kept;
        for (auto it = sent.rbegin(); it != sent.rend(); ++it)
        {
            const uint32_t cmd = it->cmd;
            if (cmd == kReplyStreamPosition || cmd == kReplySlipOffset || cmd == kReplySampleProgress)
            {
                uint64_t key = u32(it->data.data(), 0);
                if (cmd == kReplySlipOffset)
                    key |= static_cast<uint64_t>(u32(it->data.data(), 4)) << 32;
                if (!seen.insert({cmd, key}).second)
                    continue;
            }
            kept.push_back(std::move(*it));
        }
        out.insert(out.end(), std::make_move_iterator(kept.rbegin()), std::make_move_iterator(kept.rend()));
    }
}
