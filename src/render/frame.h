#pragma once

#include <cstdint>
#include <mutex>

namespace gh2
{
    // Everything the host needs to draw one guest frame. Built on the game
    // thread from guest state, handed over whole, so the render thread never
    // reads guest memory.
    struct Frame
    {
        uint64_t serial = 0;
        uint32_t width = 640;
        uint32_t height = 448;
        float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };

    // The latest finished frame. The game thread publishes at EndDrawing; the
    // host thread takes whatever is newest when it presents.
    class FrameMailbox
    {
    public:
        void publish(const Frame &frame)
        {
            std::lock_guard lock(m_mutex);
            m_frame = frame;
        }

        Frame latest() const
        {
            std::lock_guard lock(m_mutex);
            return m_frame;
        }

    private:
        mutable std::mutex m_mutex;
        Frame m_frame;
    };

    FrameMailbox &frames();
}
