#include "ui/photo_mode.h"

#include "content/locale.h"
#include "dta.h"
#include "guest.h"
#include "host/input.h"
#include "milo/layout.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/host_clock.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // FreeCamera: the RndCam it moves, and nonzero while it has it.
        constexpr uint32_t kCam = 0x0u;
        constexpr uint32_t kActive = 0x20u;

        constexpr float kSpeed = 100.0f;  // world units a second
        constexpr float kFast = 4.0f;     // times that with shift held
        constexpr float kMouse = 0.0025f; // radians a pixel
        constexpr float kRoll = 1.0472f;  // radians a second, 60 degrees
        constexpr float kMaxPitch = 1.55f;

        ps2x::host_clock::Clock::time_point s_last;
        float s_roll = 0.0f; // as the last poll left it

        // The camera level at a heading and pitch: its right, the way it
        // looks and its up, x right, y forward and z up.
        struct Level
        {
            float rows[3][3];

            Level(float yaw, float pitch)
                : rows{{std::cos(yaw), std::sin(yaw), 0.0f},
                       {-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)},
                       {std::sin(yaw) * std::sin(pitch), -std::cos(yaw) * std::sin(pitch), std::cos(pitch)}}
            {
            }
        };

        // FreeCamera::Poll (retail 0x1b9370), the game's own free camera
        // ({world toggle_free_cam <pad>}, cheats_funcs.dta), flown from the
        // host: the game's reads a pad's two sticks, and port 0 is a guitar.
        // While it is active the CameraManager sets no shot, and leaving it
        // puts the camera back where it took it from.
        //
        // Its heading, pitch and roll are read back from the camera each
        // poll, so it starts as the shot it is taken from has it. The roll
        // only tilts the picture about the way it looks: it moves and turns
        // as if level.
        void poll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t free = GPR_U32(ctx, 4);
            const auto now = ps2x::host_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - s_last).count(), 0.1f);
            s_last = now;
            if (load<uint32_t>(rdram, free + kActive) != 0u)
            {
                const HostFly fly = takeFly();
                const uint32_t local = load<uint32_t>(rdram, free + kCam) + milo::transformable::kLocal;
                float right[3], forward[3], at[3];
                for (uint32_t i = 0; i < 3u; ++i)
                {
                    right[i] = load<float>(rdram, local + i * 4u);
                    forward[i] = load<float>(rdram, local + 0x10u + i * 4u);
                    at[i] = load<float>(rdram, local + 0x30u + i * 4u);
                }
                float yaw = std::atan2(-forward[0], forward[1]);
                float pitch = std::asin(std::clamp(forward[2], -1.0f, 1.0f));
                const Level was(yaw, pitch);
                float along = 0.0f, up = 0.0f;
                for (uint32_t i = 0; i < 3u; ++i)
                {
                    along += right[i] * was.rows[0][i];
                    up += right[i] * was.rows[2][i];
                }
                float roll = std::atan2(up, along);
                if (fly.rollLeft && fly.rollRight)
                    roll = 0.0f;
                else
                    roll += (fly.rollLeft ? kRoll : 0.0f) * dt - (fly.rollRight ? kRoll : 0.0f) * dt;
                s_roll = roll;
                yaw -= fly.mouseX * kMouse;
                pitch = std::clamp(pitch - fly.mouseY * kMouse, -kMaxPitch, kMaxPitch);
                const Level level(yaw, pitch);
                const float speed = kSpeed * (fly.fast ? kFast : 1.0f) * dt;
                for (uint32_t i = 0; i < 3u; ++i)
                {
                    at[i] += (level.rows[0][i] * fly.right + level.rows[1][i] * fly.forward) * speed;
                    store<float>(rdram, local + i * 4u,
                                 std::cos(roll) * level.rows[0][i] + std::sin(roll) * level.rows[2][i]);
                    store<float>(rdram, local + 0x10u + i * 4u, level.rows[1][i]);
                    store<float>(rdram, local + 0x20u + i * 4u,
                                 std::cos(roll) * level.rows[2][i] - std::sin(roll) * level.rows[0][i]);
                    store<float>(rdram, local + 0x30u + i * 4u, at[i]);
                }
                runtime->callGuestFunction(rdram, ctx, s_addresses->setDirty, {local - milo::transformable::kLocal});
            }
            ctx->pc = returnTo;
        }

        // How far a character moves the list's text on, in the scene's
        // units: the helveticablack label at the list's text size of 15
        // (photo_mode.dta), measured off runs of twenty drawn at 1080p.
        float advance(char c)
        {
            static const float kLetters[26] = {10.6f,  10.56f, 10.63f, 10.58f, 9.78f, 9.03f,  11.36f, 11.36f, 5.35f,
                                               9.03f,  11.36f, 9.03f,  12.91f, 11.31f, 11.36f, 9.83f,  11.36f, 10.56f,
                                               9.83f,  9.83f,  11.36f, 10.6f,  13.75f, 10.6f,  10.65f, 9.83f};
            if (c >= 'A' && c <= 'Z')
                return kLetters[c - 'A'];
            if (c >= '0' && c <= '9')
                return 9.05f;
            if (c == '_')
                return 6.98f;
            if (c == '.' || c == '-' || c == ' ')
                return 4.64f;
            return 10.6f;
        }

        // The WorldDir's CameraManager, and in it the time its shot started
        // at, in the shot's units: its frame is the time since
        // (CameraManager::Poll, 0x261690).
        constexpr uint32_t kCameraManager = 0x2acu;
        constexpr uint32_t kShotStart = 0x5cu;

        // {photo_mode fly <on>}: the mouse and the keys are the free
        // camera's while it is on.
        // {photo_mode shot_start <world>} is when the world's shot started,
        // and {photo_mode shot_start <world> <time>} has it started then: a
        // shot forced starts over, and the song's own goes on from where it
        // was.
        script::Node command(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "fly")
                setFlying(call.arg(2).value != 0u);
            else if (op == "roll")
                // {photo_mode roll}: the free camera's, in degrees, left of level.
                return script::floatNode(s_roll * 57.29578f);
            else if (op == "name" || op == "width" || op == "after")
            {
                // {photo_mode name <shot>}: its name in capitals, as a token
                // with no string, which a label shows as itself.
                // {photo_mode width <shot>}: how wide the list draws that,
                // which script has no way to ask a label.
                // {photo_mode after <shot> <other>}: whether its name
                // comes after the other's.
                const auto named = [&](int i)
                {
                    const uint32_t object = call.object(i);
                    const uint32_t name = object ? load<uint32_t>(call.rdram, object + milo::object::kName) : 0u;
                    std::string text = name ? reinterpret_cast<const char *>(getMemPtr(call.rdram, name)) : "";
                    std::transform(text.begin(), text.end(), text.begin(),
                                   [](unsigned char c) { return std::toupper(c); });
                    return text;
                };
                const std::string text = named(2);
                if (op == "after")
                    return {text > named(3) ? 1u : 0u, script::kInt};
                if (text.empty())
                    return {};
                if (op == "name")
                    return {script::symbol(call.rdram, call.ctx, call.runtime, text), script::kSymbol};
                float width = 0.0f;
                for (const char c : text)
                    width += advance(c);
                return script::floatNode(width);
            }
            else if (op == "shot_start")
            {
                // The WorldDir from its Hmx::Object, by the offset its
                // destructor's slot holds.
                const uint32_t object = call.object(2);
                if (object == 0u)
                    return {};
                const int16_t delta = load<int16_t>(call.rdram, load<uint32_t>(call.rdram, object) + 0x8u);
                const uint32_t start =
                    object + static_cast<uint32_t>(static_cast<int32_t>(delta)) + kCameraManager + kShotStart;
                if (call.size() < 4)
                    return script::floatNode(load<float>(call.rdram, start));
                store<float>(call.rdram, start, call.number(3));
            }
            return {};
        }
    }

    void installPhotoMode(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        runtime.replaceFunction(addresses.freeCameraPoll, poll);
        script::addCommand("photo_mode", command);
        script::patchUi(kPhotoModeDta);
        locale::add("ghc_photo_fly", "FREE CAMERA");
        locale::add("ghc_photo_shots", "CAMERAS");
        locale::add("ghc_photo_step", "CAMERA");
        locale::add("ghc_photo_hide", "HIDE");
    }
}
