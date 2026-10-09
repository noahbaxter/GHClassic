#include "content/floor_spot.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "script.h"

#include <array>
#include <cmath>

namespace gh2::floor_spot
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // Each a RndTransformable *, or 0 with no pool.
        uint32_t s_mesh = 0u, s_light = 0u, s_who = 0u;

        constexpr uint32_t kCharacterTransform = 0xe0u; // Character's RndTransformable (Teleport, 0x162b30)
        constexpr float kScale = 100.0f;                // floor_spot_scale (GH1 0x363b6c)
        constexpr float kHeight = 1.5f;                 // floor_spot_height (GH1 0x363b68)

        using Vec3 = std::array<float, 3>;
        using Rows = std::array<Vec3, 3>;

        // A Transform's position.
        Vec3 position(uint8_t *rdram, uint32_t xfm)
        {
            return {load<float>(rdram, xfm + 0x30u), load<float>(rdram, xfm + 0x34u), load<float>(rdram, xfm + 0x38u)};
        }

        Vec3 cross(const Vec3 &a, const Vec3 &b)
        {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }

        Vec3 normalized(const Vec3 &v)
        {
            const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            return length > 0.0f ? Vec3{v[0] / length, v[1] / length, v[2] / length} : v;
        }

        // MakeRotMatrix(forward, up) (GH1 0x24adf8) with z up: y along
        // forward, x to its right.
        Rows rotation(const Vec3 &forward)
        {
            const Vec3 y = normalized(forward);
            const Vec3 x = normalized(cross(y, {0.0f, 0.0f, 1.0f}));
            return {x, y, cross(x, y)};
        }

        // RndTransformable::SetLocalRot and SetLocalPos (GH1 0x1da700,
        // 0x1da730).
        void setLocal(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t transformable, const Rows &rows,
                      const Vec3 *at)
        {
            const uint32_t local = transformable + milo::transformable::kLocal;
            for (uint32_t row = 0; row < 3u; ++row)
                for (uint32_t c = 0; c < 3u; ++c)
                    store<float>(rdram, local + row * 0x10u + c * 4u, rows[row][c]);
            for (uint32_t c = 0; at && c < 3u; ++c)
                store<float>(rdram, local + 0x30u + c * 4u, (*at)[c]);
            runtime->callGuestFunction(rdram, ctx, s_addresses->setDirty, {transformable});
        }

        // WorldDir::DrawShowing (0x26f6b8), as VenueSpotLight::Poll (GH1
        // 0x179420): the Light turned to where the character stands
        // (CharBase::GetXfm, GH1 0x18e238), and the mesh flat on the floor
        // there, a quarter of its length on from the Light and as much
        // longer as the light falls aslant.
        struct DrawTag;
        void onDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_mesh == 0u)
                return;
            const EntryArgs args(ctx);
            const Vec3 base = position(rdram, s_who + milo::transformable::kLocal);
            const Vec3 from = position(
                rdram, static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->worldXfm, {s_light})));
            const Vec3 fall = {base[0] - from[0], base[1] - from[1], base[2] - from[2]};
            setLocal(rdram, ctx, runtime, s_light, rotation(fall), nullptr);
            // A light level with the character falls on no floor: GH1
            // divides by 0 there (0x179540), and the mesh is left as it was.
            if (fall[2] != 0.0f)
            {
                const float stretch =
                    std::sqrt(fall[0] * fall[0] + fall[1] * fall[1] + fall[2] * fall[2]) / std::fabs(fall[2]);
                Rows rows = rotation({fall[0], fall[1], 0.0f});
                for (float &c : rows[0])
                    c *= kScale;
                for (float &c : rows[1])
                    c *= kScale * stretch;
                const Vec3 at = {base[0] + 0.25f * rows[1][0], base[1] + 0.25f * rows[1][1], base[2] + kHeight};
                setLocal(rdram, ctx, runtime, s_mesh, rows, &at);
            }
            args.restore(ctx);
        }

        // WorldDir's destructor (0x26ddb8): the three are in its dirs.
        struct GoneTag;
        void onGone(uint8_t *, R5900Context *, PS2Runtime *)
        {
            s_mesh = s_light = s_who = 0u;
        }

        // The object a script names, whole: the first entry of its
        // Hmx::Object's vtable has how far that is into it.
        uint32_t whole(const script::Call &call, int i)
        {
            const uint32_t object = call.object(i);
            if (object == 0u)
                return 0u;
            const int16_t delta = load<int16_t>(call.rdram, load<uint32_t>(call.rdram, object));
            return object + static_cast<uint32_t>(static_cast<int32_t>(delta));
        }

        script::Node floorSpotCommand(const script::Call &call)
        {
            const uint32_t mesh = whole(call, 1), light = whole(call, 2), who = whole(call, 3);
            if (mesh == 0u || light == 0u || who == 0u)
                return {};
            s_mesh = mesh + milo::mesh::kTransform;
            s_light = light;
            s_who = who + kCharacterTransform;
            return {};
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("floor_spot", floorSpotCommand);
        EntryHook<DrawTag>::install(runtime, addresses.worldDirDrawShowing, onDraw);
        EntryHook<GoneTag>::install(runtime, addresses.worldDirDtor, onGone);
    }
}
