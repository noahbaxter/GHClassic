#pragma once

#include "render/scene.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace gh2
{
    // The camera a draw was made with, as the engine held it at the time.
    struct Camera
    {
        uint32_t id = 0;   // the RndCam's guest address, for telling cameras apart
        Matrix view{};     // RndCam +0xc0: the inverse of its world transform
        float eye[3] = {}; // its world position, which PsCam::Select uploads as qw698 (0x19c9d4)
        float nearPlane = 1.0f;
        float farPlane = 1000.0f;
        float yFov = 0.0f; // 0 is orthographic
        float zRange[2] = {0.0f, 1.0f};
        float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // normalized screen x, y, w, h
        // The RndTex it draws into (RndCam +0x2ec), 0 for the screen, and
        // that texture's size, which PsCam::Select takes as the viewport
        // (0x19c4d0) and UpdateLocal as the aspect (0x1b1f7c).
        uint32_t target = 0;
        uint32_t targetWidth = 0;
        uint32_t targetHeight = 0;
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
        // What the picture is shown at. PS2 pixels are not square: the frame
        // fills a 4:3 screen, or a 16:9 one when the aspect squeezes the
        // cameras for it.
        float displayAspect = 4.0f / 3.0f;
        std::vector<Camera> cameras;
        std::vector<DrawCall> draws;
    };

    // Finished frames, oldest first. The game thread publishes at EndDrawing;
    // the host thread shows each once, in order. Game frames finish at uneven
    // host times, so taking only the newest at each vsync showed one in eight
    // twice and skipped the next, which the scrolling highway shows as judder.
    class FrameQueue
    {
    public:
        void publish(const Frame &frame)
        {
            {
                std::lock_guard lock(m_mutex);
                // Room for one frame of jitter and no more, so a host stall
                // leaves at most one frame of extra delay.
                if (m_frames.size() >= kDepth)
                    m_frames.pop_front();
                m_frames.push_back(frame);
            }
            m_ready.notify_one();
        }

        // The next frame not yet shown, waiting up to `wait` for one. False
        // when none came: the host shows its last again.
        bool next(Frame &out, std::chrono::microseconds wait)
        {
            std::unique_lock lock(m_mutex);
            if (!m_ready.wait_for(lock, wait, [this] { return !m_frames.empty(); }))
                return false;
            out = std::move(m_frames.front());
            m_frames.pop_front();
            return true;
        }

        // The newest frame, dropping any older not yet shown, for a game
        // running faster than the display. False when none came.
        bool latest(Frame &out)
        {
            std::lock_guard lock(m_mutex);
            if (m_frames.empty())
                return false;
            out = std::move(m_frames.back());
            m_frames.clear();
            return true;
        }

    private:
        static constexpr size_t kDepth = 2;
        std::mutex m_mutex;
        std::condition_variable m_ready;
        std::deque<Frame> m_frames;
    };

    FrameQueue &frames();

    // The frame the game thread is building between BeginDrawing and
    // EndDrawing. Game thread only.
    Frame &building();
}
