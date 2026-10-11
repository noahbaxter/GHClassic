#include "gh2x/shadows.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "render/frame.h"
#include "render/native_mesh.h"
#include "settings/settings.h"

#include <algorithm>
#include <cmath>

namespace gh2::gh2x
{
    namespace
    {
        struct ShadowMapTag;

        const Addresses *s_addresses = nullptr;
        constexpr float kPi = 3.14159265f;
        // The map DrawShadowBuffer last drew, until SetShadowMap takes it up.
        int32_t s_made = -1;
        // Radii of its sphere past which a caster's vert is not the
        // character's pose.
        constexpr float kReachLimit = 1.5f;

        // The inverse of a transform: three rows and a position.
        Matrix inverse(const Matrix &m)
        {
            const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
            const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
            const double r[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                                 (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                                 (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
            Matrix out{};
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                {
                    out[static_cast<size_t>(row * 4 + col)] = static_cast<float>(r[row * 3 + col]);
                    out[static_cast<size_t>(12 + col)] -=
                        static_cast<float>(m[static_cast<size_t>(12 + row)] * r[row * 3 + col]);
                }
            out[15] = 1.0f;
            return out;
        }

        // How far the casters drawn from `first` on reach from `centre`, as
        // posed, leaving out what is past `limit` (a guitar not yet in its
        // guitarist's hands is several radii off for a few frames).
        float reach(const Frame &frame, size_t first, const float centre[3], float limit)
        {
            float farthest = 0.0f;
            for (size_t i = first; i < frame.draws.size(); ++i)
            {
                const DrawCall &draw = frame.draws[i];
                if (!draw.shadowCaster || !draw.mesh)
                    continue;
                for (const Vertex &v : draw.mesh->verts)
                {
                    float at[3] = {};
                    const auto through = [&](const Matrix &m, float weight)
                    {
                        for (size_t c = 0; c < 3; ++c)
                            at[c] += weight * (v.pos[0] * m[c] + v.pos[1] * m[4u + c] + v.pos[2] * m[8u + c] + m[12u + c]);
                    };
                    if (draw.skinned)
                        for (size_t b = 0; b < draw.bones.size(); ++b)
                            through(draw.bones[b], v.color[b]);
                    else
                        through(draw.world, 1.0f);
                    const float d[3] = {at[0] - centre[0], at[1] - centre[1], at[2] - centre[2]};
                    const float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (distance <= limit)
                        farthest = std::max(farthest, distance);
                }
            }
            return farthest;
        }

        // A shadow map's camera: the light's own rotation, back from the
        // sphere's centre along the light's y by d = radius / tan(pi/8), with
        // a 45 degree field of view, from d - radius to d + radius.
        Camera shadowCamera(Matrix world, const float centre[3], float radius, int32_t index)
        {
            const float back = radius / std::tan(kPi / 8.0f);
            for (uint32_t i = 0; i < 3; ++i)
                world[12u + i] = centre[i] - back * world[4u + i];
            Camera camera;
            camera.id = kShadowTex + static_cast<uint32_t>(index);
            camera.view = inverse(world);
            std::copy_n(world.data() + 12, 3, camera.eye);
            camera.nearPlane = back - radius;
            camera.farPlane = back + radius;
            camera.yFov = kPi / 4.0f;
            camera.target = camera.id;
            camera.targetWidth = camera.targetHeight = kShadowSize;
            return camera;
        }

        // Character::DrawShadowBuffer, with shadowCamera's camera. The PS2's
        // code has sin where that has tan; a Xenia frame's three shadow
        // matrices all have a far to near of 2.41421, which is tan's
        // (.planning/gh2x-look/xenia/shadow_matrices.json).
        void drawShadowBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t self = GPR_U32(ctx, 4);
            ctx->pc = returnTo;
            s_made = -1;
            Frame &frame = building();
            if (settings::get(settings::kShading) != settings::kX360 || frame.casting >= 0 ||
                frame.shadows.size() >= kMaxShadows)
                return;
            uint32_t light = 0u;
            const uint32_t env = load<uint32_t>(rdram, s_addresses->rndEnvironCurrent);
            for (uint32_t node = env != 0u ? load<uint32_t>(rdram, env + ::milo::environment::kFirstLight) : 0u;
                 node != 0u && light == 0u; node = load<uint32_t>(rdram, node + 4u))
                if (load<uint32_t>(rdram, load<uint32_t>(rdram, node) + ::milo::light::kType) ==
                    ::milo::light::kDirectional)
                    light = load<uint32_t>(rdram, node);
            if (light == 0u)
                return;

            // The guest's stack is moved down past a Sphere for the calls.
            const uint32_t sp = GPR_U32(ctx, 29);
            const uint32_t sphere = (sp - 0x40u) & ~0xfu;
            SET_GPR_U32(ctx, 29, sphere - 0x20u);
            const uint32_t vtable = load<uint32_t>(rdram, self + ::milo::character::kVtable);
            const auto call = [&](uint32_t slot, uint32_t a1, uint32_t a2)
            {
                const uint32_t drawable =
                    self + ::milo::character::kDrawable +
                    static_cast<uint32_t>(static_cast<int32_t>(load<int16_t>(rdram, vtable + slot)));
                runtime->callGuestFunction(rdram, ctx, load<uint32_t>(rdram, vtable + slot + 4u), {drawable, a1, a2});
            };
            store<float>(rdram, sphere + 0x10u, 0.0f);
            call(::milo::character::kMakeWorldSphere, sphere, 0u);
            const float radius = load<float>(rdram, sphere + 0x10u);
            if (radius > 0.0f)
            {
                const Matrix world = readTransform(rdram, static_cast<uint32_t>(runtime->callGuestFunction(
                                                              rdram, ctx, s_addresses->worldXfm, {light})));
                float centre[3];
                for (uint32_t i = 0; i < 3; ++i)
                    centre[i] = load<float>(rdram, sphere + i * 4u);
                const int32_t index = static_cast<int32_t>(frame.shadows.size());
                const uint32_t camera = internCamera(shadowCamera(world, centre, radius, index));
                frame.shadows.push_back(camera);
                frame.casting = index;
                const size_t first = frame.draws.size();
                call(::milo::character::kDrawShowing, ::milo::character::kDrawShadow, 0u);
                frame.casting = -1;
                s_made = index;
                // The file's sphere does not hold every pose (a guitarist
                // with a guitar reaches 1.15 of it, a band member 1.14), and
                // what is outside the map casts nothing: the map grows to
                // what was drawn into it. The 360's own spheres are not the
                // file's either, by 0.87 to 1.43 in one frame.
                const float reached = reach(frame, first, centre, radius * kReachLimit);
                if (reached > radius)
                    frame.cameras[camera] = shadowCamera(world, centre, reached, index);
            }
            SET_GPR_U32(ctx, 29, sp);
            ctx->pc = returnTo;
        }

        // Rnd::SetShadowMap(tex, cam): Character::DrawShowing's, with the
        // shadow buffer just drawn before the character's own draw and with
        // nothing after it.
        void onSetShadowMap(uint8_t *, R5900Context *, PS2Runtime *)
        {
            building().lit = s_made;
            s_made = -1;
        }
    }

    void installShadows(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        runtime.replaceFunction(addresses.characterDrawShadowBuffer, drawShadowBuffer);
        EntryHook<ShadowMapTag>::install(runtime, addresses.rndSetShadowMap, onSetShadowMap);
    }
}
