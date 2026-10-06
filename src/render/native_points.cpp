// PsRnd::DoPointTests, native: whether each tested point shows, from the
// frame's own draws instead of the GS's Z buffer.
//
// A flare asks whether its point shows (Rnd::TestPoint, 0x1d55b8, from
// RndFlare::DrawFlare, 0x1f9370), which keeps the point's pixel and Z in a
// list of the Rnd's (+0xe8: a node's pixel at +0x8 and +0xc, Z at +0x10 and
// object at +0x14). At the next frame's start retail (0x19a7f0) reads the Z
// buffer back at each pixel and stores, at the object's +0x104, whether what
// the last frame drew there is further than the point: one test, for the
// whole flare, and nothing as near as the point lets it show.
//
// There is no Z buffer to read on the game thread. The last frame's draws
// are here whole, so the same question is put to them: does any triangle
// that writes Z, through the camera the point was tested with, cross the
// line from the eye to the point. A skinned mesh is not tested (its verts
// are posed on the GPU), nor is a texture's alpha, so a character never
// hides a flare and a cut-out always does: an approximation of the GS's.

#include "render/native_points.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime_macros.h"
#include "render/frame.h"
#include "render/native_mesh.h"

#include <cmath>
#include <cstring>
#include <map>

namespace gh2
{
    namespace
    {
        constexpr uint32_t kTests = 0xe8u;       // the Rnd's list of tests, its sentinel node
        constexpr uint32_t kTestObject = 0x14u;  // a node's object
        constexpr uint32_t kObjectShows = 0x104u; // the object's answer
        // How much further than the point still hides it, as a part of its
        // distance: retail compares 16-bit Zs, so one drawn at the point
        // does. An approximation of that resolution.
        constexpr float kSlack = 0.002f;

        struct TestTag;

        void onTestPoint(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            Frame::PointTest test;
            test.object = GPR_U32(ctx, 6);
            for (uint32_t i = 0; i < 3; ++i)
                test.point[i] = load<float>(rdram, GPR_U32(ctx, 5) + i * 4u);
            test.camera = currentCamera(rdram);
            building().tests.push_back(test);
        }

        // Whether the segment from `eye` to `point`, both in the mesh's own
        // space, crosses one of its triangles (Moller and Trumbore's test).
        bool crosses(const MeshData &mesh, const float eye[3], const float point[3])
        {
            const float d[3] = {point[0] - eye[0], point[1] - eye[1], point[2] - eye[2]};
            for (size_t i = 0; i + 2u < mesh.indices.size(); i += 3u)
            {
                const float *a = mesh.verts[mesh.indices[i]].pos;
                const float *b = mesh.verts[mesh.indices[i + 1u]].pos;
                const float *c = mesh.verts[mesh.indices[i + 2u]].pos;
                const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
                const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
                if (std::fabs(det) < 1.0e-12f)
                    continue;
                const float inv = 1.0f / det;
                const float s[3] = {eye[0] - a[0], eye[1] - a[1], eye[2] - a[2]};
                const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
                if (u < 0.0f || u > 1.0f)
                    continue;
                const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
                const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
                if (v < 0.0f || u + v > 1.0f)
                    continue;
                const float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
                if (t > 0.0f && t <= 1.0f + kSlack)
                    return true;
            }
            return false;
        }

        // A world point in the space of a mesh drawn at `world`, whose rows
        // are its axes and position; false for one that cannot be inverted.
        bool toLocal(const Matrix &world, const float in[3], float out[3])
        {
            const float *m = world.data();
            const float c0[3] = {m[5] * m[10] - m[6] * m[9], m[6] * m[8] - m[4] * m[10], m[4] * m[9] - m[5] * m[8]};
            const float det = m[0] * c0[0] + m[1] * c0[1] + m[2] * c0[2];
            if (std::fabs(det) < 1.0e-12f)
                return false;
            const float r[3] = {in[0] - m[12], in[1] - m[13], in[2] - m[14]};
            // The inverse of the 3x3 of rows, applied to a row vector.
            const float inv[9] = {
                c0[0], m[2] * m[9] - m[1] * m[10], m[1] * m[6] - m[2] * m[5],
                c0[1], m[0] * m[10] - m[2] * m[8], m[2] * m[4] - m[0] * m[6],
                c0[2], m[1] * m[8] - m[0] * m[9],  m[0] * m[5] - m[1] * m[4],
            };
            for (int col = 0; col < 3; ++col)
                out[col] = (r[0] * inv[0 * 3 + col] + r[1] * inv[1 * 3 + col] + r[2] * inv[2 * 3 + col]) / det;
            return true;
        }

        bool hidden(const Frame &frame, const Frame::PointTest &test)
        {
            if (test.camera >= frame.cameras.size())
                return false;
            const Camera &camera = frame.cameras[test.camera];
            const float *eye = camera.eye;
            const DrawCall *last = nullptr;
            for (const DrawCall &draw : frame.draws)
            {
                // A camera is listed again after another's draws.
                if (draw.screen || draw.skinned || !draw.mesh ||
                    std::memcmp(&frame.cameras[draw.camera], &camera, sizeof(Camera)) != 0)
                    continue;
                const uint32_t z = draw.material.zMode;
                if (z != milo::mat::kZNormal && z != milo::mat::kZForce && z != milo::mat::kZDecal)
                    continue;
                // A mesh's passes are one mesh.
                if (last && draw.mesh == last->mesh && draw.world == last->world)
                    continue;
                last = &draw;
                float from[3], to[3];
                if (toLocal(draw.world, eye, from) && toLocal(draw.world, test.point, to) && crosses(*draw.mesh, from, to))
                    return true;
            }
            return false;
        }

        void doPointTests(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t sentinel = GPR_U32(ctx, 4) + kTests;
            const Frame &frame = previous();
            // The last test of each object still in the Rnd's list.
            std::map<uint32_t, const Frame::PointTest *> asked;
            for (const Frame::PointTest &test : frame.tests)
                asked[test.object] = &test;
            for (uint32_t node = load<uint32_t>(rdram, sentinel); node != sentinel && node != 0u; node = load<uint32_t>(rdram, node))
            {
                const uint32_t object = load<uint32_t>(rdram, node + kTestObject);
                if (const auto it = asked.find(object); it != asked.end())
                    store<uint32_t>(rdram, object + kObjectShows, hidden(frame, *it->second) ? 0u : 1u);
            }
            ctx->pc = returnTo;
        }
    }

    void installNativePoints(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<TestTag>::install(runtime, addresses.rndTestPoint, onTestPoint);
        runtime.replaceFunction(addresses.psRndDoPointTests, doPointTests);
    }
}
