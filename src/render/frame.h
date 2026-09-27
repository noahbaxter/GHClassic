#pragma once

#include "render/scene.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace gh2
{
    // The camera a draw was made with, as the engine held it at the time.
    struct Camera
    {
        uint32_t id = 0;   // the RndCam's guest address, for telling cameras apart
        Matrix view{};     // RndCam +0xc0: the inverse of its world transform
        float nearPlane = 1.0f;
        float farPlane = 1000.0f;
        float yFov = 0.0f; // 0 is orthographic
        float zRange[2] = {0.0f, 1.0f};
        float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // normalized screen x, y, w, h
    };

    // Everything the host needs to draw one guest frame. Built on the game
    // thread from guest state, handed over whole, so the render thread never
    // reads guest memory.
    struct Frame
    {
        uint64_t serial = 0;
        uint32_t width = 640;
        uint32_t height = 448;
        float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float yRatio = 0.75f; // Rnd::YRatio for the current aspect
        std::vector<Camera> cameras;
        std::vector<DrawCall> draws;
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

    // The frame the game thread is building between BeginDrawing and
    // EndDrawing. Game thread only.
    Frame &building();
}
