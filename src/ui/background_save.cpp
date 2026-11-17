#include "ui/background_save.h"

#include "content/card.h"
#include "content/games.h"
#include "formats/ps2_icon.h"
#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "render/camera.h"
#include "render/frame.h"
#include "script.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <string>

namespace gh2::background_save
{
    namespace
    {
        // The icon's height and its gap to the picture's edges, of the
        // picture's height; the least it stays up and its fades, in UI
        // seconds.
        constexpr float kHeight = 0.13f, kEdge = 0.05f, kLeast = 3.0f, kFade = 0.25f;
        // Its turn: the guitar pickers' way, about the upright the other way
        // round from the eye (guitar_single.tnm, as campaigns.dta's
        // case_spin turns the cases), half as fast again.
        constexpr float kDegreesASecond = -135.0f;

        const Addresses *s_addresses = nullptr;
        bool s_busy = false;
        float s_shown = -1.0f;  // its seconds up so far, -1 while down
        float s_startAt = 0.0f; // s_shown when the last save began
        float s_doneAt = -1.0f; // s_shown when it began to go
        float s_lastNow = 0.0f;
        std::string s_game;

        struct Model
        {
            ps2_icon::Icon icon;
            std::shared_ptr<const TextureData> texture;
            std::array<float, 3> centre{};
            float scale = 1.0f; // to kHeight tall
        };

        float uiSeconds(uint8_t *rdram)
        {
            return load<float>(rdram, load<uint32_t>(rdram, s_addresses->theTaskMgr + 0x28u) + 0x34u);
        }

        // A game's own save icon off its disc, GH2's for one with none. One
        // too big for a mesh's 16-bit indices is none.
        const Model *model(const std::string &game)
        {
            static std::map<std::string, std::optional<Model>> s_models;
            const auto kept = s_models.find(game);
            if (kept != s_models.end())
                return kept->second ? &*kept->second : game != "gh2" ? model("gh2") : nullptr;
            std::optional<Model> made;
            const std::optional<size_t> disc = game == "gh2" ? std::optional<size_t>(0u) : games::disc(game);
            const auto file = disc ? content::cardIcon(*disc) : std::nullopt;
            auto icon = file ? ps2_icon::parse(*file) : std::nullopt;
            if (icon && icon->normals.size() <= 0xffffu)
            {
                Model m;
                auto data = std::make_shared<TextureData>();
                data->width = data->height = ps2_icon::kTextureSide;
                data->rgba = std::move(icon->rgba);
                m.texture = std::move(data);
                std::array<float, 3> lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
                for (const auto &p : icon->shapes[0])
                    for (int i = 0; i < 3; ++i)
                    {
                        lo[i] = std::min(lo[i], p[i]);
                        hi[i] = std::max(hi[i], p[i]);
                    }
                for (int i = 0; i < 3; ++i)
                    m.centre[i] = (lo[i] + hi[i]) * 0.5f;
                m.scale = 2.0f * kHeight / std::max(hi[1] - lo[1], 1e-3f);
                std::cerr << "[save] " << game << "'s card icon: " << icon->shapes.size() << " shapes, "
                          << icon->normals.size() << " vertices" << std::endl;
                m.icon = std::move(*icon);
                made = std::move(m);
            }
            else if (game == "gh2")
                std::cerr << "[save] no card icon to show" << std::endl;
            s_models[game] = std::move(made);
            return model(game);
        }

        // {ghc_save_start <theme>}: a save begins, its icon the theme's game's.
        // One already up stays, now up kLeast from here.
        script::Node start(const script::Call &call)
        {
            s_game = call.symbol(1);
            if (s_shown < 0.0f)
                s_shown = 0.0f;
            s_startAt = s_shown;
            s_doneAt = -1.0f;
            s_busy = true;
            return {};
        }

        struct SaveData2Tag;
        void onSaveData2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!s_busy)
                return;
            s_busy = false;
            const EntryArgs args(ctx);
            script::run(rdram, ctx, runtime, "{ghc_saved " + std::to_string(static_cast<int32_t>(args.a[0])) + "}");
            args.restore(ctx);
        }

        // Where the shapes stand `t` seconds in: the browser's morph, a shape
        // a frame length's worth of 60ths, back round to the first.
        std::array<float, 3> position(const ps2_icon::Icon &icon, size_t v, float t)
        {
            const size_t count = icon.shapes.size();
            if (count == 1u || icon.frameLength == 0u)
                return icon.shapes[0][v];
            const float per = static_cast<float>(icon.frameLength) / 60.0f / std::max(icon.speed, 1e-3f) / count;
            const float at = std::fmod(t / per, static_cast<float>(count));
            const size_t a = static_cast<size_t>(at) % count, b = (a + 1u) % count;
            const float f = at - std::floor(at);
            std::array<float, 3> p{};
            for (int i = 0; i < 3; ++i)
                p[i] = icon.shapes[a][v][i] * (1.0f - f) + icon.shapes[b][v][i] * f;
            return p;
        }

        // Every screen change zeroes the UI clock, so it counts by the
        // frame's step, not from a start.
        void draw(uint8_t *rdram)
        {
            const float now = uiSeconds(rdram);
            const float step = now >= s_lastNow ? now - s_lastNow : 0.0f;
            s_lastNow = now;
            if (s_shown < 0.0f)
                return;
            s_shown += std::fmin(step, 0.1f);
            if (!s_busy && s_doneAt < 0.0f && s_shown >= s_startAt + kLeast - kFade)
                s_doneAt = s_shown;
            if (s_doneAt >= 0.0f && s_shown >= s_doneAt + kFade)
            {
                s_shown = s_doneAt = -1.0f;
                return;
            }
            const float alpha = std::clamp(std::min(s_shown / kFade, s_doneAt >= 0.0f ? 1.0f - (s_shown - s_doneAt) / kFade : 1.0f),
                                           0.0f, 1.0f);
            const Model *m = model(s_game);
            if (!m)
                return;
            const ps2_icon::Icon &icon = m->icon;
            Frame &frame = building();
            // Clip space: x and y from -1 to 1 across the picture, y down as
            // the icon's; a unit across is the aspect narrower than one tall.
            const float across = 1.0f / frame.displayAspect;
            const float cx = 1.0f - (2.0f * kEdge + kHeight * 0.75f) * across, cy = 1.0f - 2.0f * kEdge - kHeight;
            // Already turning as it fades in, facing straight out once whole.
            const float angle = (180.0f + (s_shown - kFade) * kDegreesASecond) * 0.017453293f;
            const float c = std::cos(angle), s = std::sin(angle);
            // A light from the front, above and left, turning with the eye.
            const std::array<float, 3> light{-0.4f, -0.6f, -0.7f};
            auto mesh = std::make_shared<MeshData>();
            mesh->transient = true;
            std::vector<float> depth;
            for (size_t v = 0; v < icon.normals.size(); ++v)
            {
                std::array<float, 3> p = position(icon, v, s_shown);
                for (int i = 0; i < 3; ++i)
                    p[i] = (p[i] - m->centre[i]) * m->scale;
                // The card's x runs the other way to the picture's.
                p[0] = -p[0];
                const auto &n = icon.normals[v];
                const float x = p[0] * c + p[2] * s, z = -p[0] * s + p[2] * c;
                const float nx = -n[0] * c + n[2] * s, nz = n[0] * s + n[2] * c;
                const float lit = 0.55f + 0.6f * std::max(0.0f, -(nx * light[0] + n[1] * light[1] + nz * light[2]));
                Vertex out{};
                out.pos[0] = cx + x * across;
                out.pos[1] = cy + p[1];
                out.pos[2] = 1.0f;
                for (int i = 0; i < 3; ++i)
                    out.color[i] = std::min(1.0f, icon.colors[v][i] / 128.0f * lit);
                out.color[3] = alpha;
                out.uv[0] = icon.uvs[v][0];
                out.uv[1] = icon.uvs[v][1];
                mesh->verts.push_back(out);
                depth.push_back(z);
            }
            // No depth test over the scene: its triangles far to near.
            std::vector<uint16_t> order(mesh->verts.size() / 3u);
            std::iota(order.begin(), order.end(), uint16_t{0});
            std::sort(order.begin(), order.end(), [&](uint16_t a, uint16_t b)
                      {
                          return depth[a * 3u] + depth[a * 3u + 1u] + depth[a * 3u + 2u] <
                                 depth[b * 3u] + depth[b * 3u + 1u] + depth[b * 3u + 2u];
                      });
            for (const uint16_t t : order)
                for (uint16_t i = 0; i < 3u; ++i)
                    mesh->indices.push_back(static_cast<uint16_t>(t * 3u + i));
            DrawCall call;
            call.mesh = mesh;
            call.screen = true;
            call.camera = internCamera(Camera{});
            call.world = identity();
            call.lightWorld = identity();
            call.material.blend = milo::mat::kBlendSrcAlpha;
            call.material.zMode = milo::mat::kZDisable;
            call.material.prelit = true;
            call.material.texWrap = false;
            call.material.texture = m->texture;
            frame.draws.push_back(call);
        }

        // Installed after native_rnd's, so it runs before the frame is
        // published: over everything else drawn.
        struct EndTag;
        void onEndDrawing(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            draw(rdram);
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("ghc_save_start", start);
        EntryHook<EndTag>::install(runtime, addresses.psRndEndDrawing, onEndDrawing);
        EntryHook<SaveData2Tag>::install(runtime, addresses.saveData2, onSaveData2);
    }
}
