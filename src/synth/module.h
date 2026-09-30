#pragma once

// SYNTH_S.IRX, Harmonix's IOP synth module, over the SPU2 model. What it does
// with each command, and when it answers, follows the module's own code;
// the .text offsets named here are the IRX's.
//
// The module runs a tick every 5 ms (thread 0x530): streams, samples, the
// batched key on/off, then the replies it gathered go to the EE in one call.
// Here the tick runs on the audio clock, every 240 frames of the mix, so the
// voices it reads positions from have played exactly that long.

#include "synth/spu.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace gh2::synth
{
    // One call into SynthEE::CtlDispatch(cmd, data, len).
    struct Reply
    {
        uint32_t cmd = 0;
        std::vector<uint8_t> data;
    };

    class Module
    {
    public:
        Module();
        ~Module();

        void reset();

        // One of the EE's records, as the RPC thread's dispatcher (0x185c)
        // takes it.
        void command(uint32_t cmd, const uint8_t *data, uint32_t len);

        // What the module has sent the EE since the last call.
        void takeReplies(std::vector<Reply> &out);

        // Audio thread: `frames` of interleaved stereo at 48 kHz added to `out`.
        void render(float *out, size_t frames);

    private:
        struct Channel;
        struct Stream;
        struct Sample;

        Stream *stream(uint32_t id);
        Sample *sample(uint32_t id);

        void tick();
        void tickStream(Stream &s);
        void serviceChannel(Channel &ch);
        void transfer(Channel &ch);
        void tickSamples();
        void flushKeys();

        // What the module keeps per voice (0x18 bytes at 0x379d0): what it
        // last wrote, so a pause can zero the registers and a resume restore
        // them.
        struct VoiceState
        {
            int16_t volume = 0;
            int16_t pan = 0;
            uint16_t pitch = 0;
            bool paused = false;
        };

        int32_t allocVoice(int32_t core);
        void freeVoice(int32_t voice);
        void keyOn(int32_t voice);
        void setVolumePan(int32_t voice, int16_t volume, int16_t pan);
        void setPitch(int32_t voice, uint16_t pitch);
        void pause(int32_t voice);
        void resume(int32_t voice);

        int32_t keyChannel(Channel &ch, uint32_t address);
        void playChannel(Channel &ch);
        void pauseChannel(Channel &ch);
        void setChannelSpeed(Channel &ch, int32_t speed);
        void slipJump(Stream &s, Channel &ch, int32_t offset);
        uint32_t position(const Channel &ch) const;

        void startSample(Sample &s);
        void stopSample(Sample &s);

        void queue(uint32_t cmd, std::initializer_list<uint32_t> words = {});

        std::mutex m_mutex;
        Spu m_spu;

        // Command 0's SynthConfig.
        uint32_t m_spuBlocks = 0;
        uint32_t m_slipMs = 0;

        std::array<VoiceState, Spu::kVoices> m_voiceState;
        std::deque<int32_t> m_freeVoices; // least recently freed first
        std::vector<bool> m_keyOn, m_keyOff, m_keyOnLater;
        bool m_voicesOnCore1 = false; // command 5

        std::vector<bool> m_spuBlockUsed;
        std::map<uint32_t, std::unique_ptr<Stream>> m_streams;
        std::map<uint32_t, std::unique_ptr<Sample>> m_samples;

        uint32_t m_uploadDest = 0;
        bool m_uploadDone = false;
        bool m_terminate = false;
        int32_t m_dataStream = -1;
        int32_t m_dataChannel = -1;

        std::vector<Reply> m_tickReplies;
        std::vector<Reply> m_sent;
        uint32_t m_framesToTick = 0;
    };
}
