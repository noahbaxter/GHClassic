#include "highway/gem_looks.h"

#include "disc/ark.h"
#include "formats/bitmap.h"
#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "milo/objects.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        constexpr const char *kTrackScene = "track/gen/track.milo_ps2";

        // GemPartTemplate: 0x14 bytes, its shown parts a list from +0x08,
        // its end node at +0x0c.
        constexpr uint32_t kPartTemplateSize = 0x14u, kPartTemplateEnd = 0x0cu;
        constexpr size_t kXfmBytes = 0x40u;

        // Each GemRepTemplate's own templates, by the mesh each shows.
        std::map<uint32_t, std::map<uint32_t, uint32_t>> s_templates;
        // Each mesh shown that turns with another, that one.
        std::map<uint32_t, uint32_t> s_turnsWith;
        std::map<std::pair<uint32_t, std::string>, uint32_t> s_found;

        std::vector<void (*)(uint8_t *, R5900Context *, PS2Runtime *, const GemAdded &)> s_gemAdded;
        std::vector<void (*)(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)> s_trackGone;

        // GemRepTemplate::DrawShowing(DrawType): its own templates drawn with
        // it, on the draw that shows them (type 0).
        struct DrawShowingTag;
        void onDrawShowing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            const auto it = s_templates.find(args.a[0]);
            if (args.a[1] != 0u || it == s_templates.end())
                return;
            for (const auto &[mesh, source] : s_turnsWith)
                std::memcpy(getMemPtr(rdram, mesh + ::milo::transformable::kWorld),
                            getMemPtr(rdram, source + ::milo::transformable::kWorld), kXfmBytes);
            for (const auto &[mesh, shown] : it->second)
                runtime->callGuestFunction(rdram, ctx, s_addresses->gemPartTemplateDrawAllParts, {shown});
            args.restore(ctx);
        }

        // Own templates whose GemRepTemplate went while they still showed a
        // part (a pool's rep may outlive it), each deleted once it shows none.
        std::vector<uint32_t> s_orphans;

        bool showsNone(uint8_t *rdram, uint32_t shown)
        {
            return load<uint32_t>(rdram, shown + kPartTemplateEnd) == shown + kPartTemplateEnd;
        }

        void deleteShown(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t shown)
        {
            runtime->callGuestFunction(rdram, ctx, s_addresses->gemPartTemplateDtor, {shown, 1u});
        }

        void deleteOrphans(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::vector<uint32_t> left;
            for (const uint32_t shown : s_orphans)
                if (showsNone(rdram, shown))
                    deleteShown(rdram, ctx, runtime, shown);
                else
                    left.push_back(shown);
            s_orphans = std::move(left);
        }

        // ~GemRepTemplate: its own templates go with it, or once they show
        // nothing.
        struct TemplateGoneTag;
        void onTemplateGone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            if (const auto it = s_templates.find(args.a[0]); it != s_templates.end())
            {
                for (const auto &[mesh, shown] : it->second)
                    if (showsNone(rdram, shown))
                        deleteShown(rdram, ctx, runtime, shown);
                    else
                        s_orphans.push_back(shown);
                s_templates.erase(it);
            }
            deleteOrphans(rdram, ctx, runtime);
            args.restore(ctx);
        }

        // GemManager::AdvanceEnd, which adds its next gem to the track.
        struct AdvanceEndTag;
        void onAdvanceEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            GemAdded added{};
            added.manager = args.a[0];
            added.index = load<uint32_t>(rdram, added.manager + gemManager::kNext);
            added.gem = load<uint32_t>(rdram, added.manager + gemManager::kGems) + added.index * gemManager::kGemSize;
            added.track = trackNum(rdram, ctx, runtime, load<uint32_t>(rdram, added.manager + gemManager::kConfig));
            added.game = static_cast<uint32_t>(runtime->callGuestFunction(
                rdram, ctx, s_addresses->songDbGetGem,
                {load<uint32_t>(rdram, s_addresses->theSongDb), added.track, added.index}));
            for (auto it = s_gemAdded.rbegin(); it != s_gemAdded.rend(); ++it)
            {
                (*it)(rdram, ctx, runtime, added);
                args.restore(ctx);
            }
            args.restore(ctx);
        }

        // ~GemRepPool: the track goes, and its scene with it.
        struct PoolGoneTag;
        void onPoolGone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            for (auto it = s_trackGone.rbegin(); it != s_trackGone.rend(); ++it)
            {
                (*it)(rdram, ctx, runtime, args.a[0]);
                args.restore(ctx);
            }
            s_found.clear();
            s_turnsWith.clear();
        }
    }

    void addToTrackScene(void (*add)(milo::Dir &scene))
    {
        ark::addFix(0u, kTrackScene, [add](std::vector<uint8_t> &file)
                    {
                        auto scene = milo::read(file);
                        if (!scene)
                            return;
                        add(*scene);
                        file = milo::write(*scene);
                    });
    }

    void onGemAdded(void (*added)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const GemAdded &gem))
    {
        s_gemAdded.push_back(added);
    }

    void onTrackGone(void (*gone)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t pool))
    {
        s_trackGone.push_back(gone);
    }

    std::optional<size_t> meshVertices(const milo::Bytes &body)
    {
        const auto plausible = [&](size_t o)
        {
            float f;
            std::memcpy(&f, body.data() + o, 4u);
            return f == 0.0f || (std::fabs(f) > 1e-4f && std::fabs(f) < 1e4f);
        };
        for (size_t o = 0; o + 8u <= body.size(); ++o)
        {
            const uint32_t n = milo::u32(body, o);
            if (n < 4u || n > 400u || o + 4u + n * kMeshVertex > body.size())
                continue;
            bool ok = true;
            for (uint32_t i = 0; i < n && ok; ++i)
                for (size_t k = 0; k < 3u && ok; ++k)
                    ok = plausible(o + 4u + i * kMeshVertex + 4u * k);
            if (ok)
                return o;
        }
        return std::nullopt;
    }

    bool addCopy(milo::Dir &scene, const std::string &name, const std::string &as,
                 const std::function<bool(milo::Bytes &)> &change)
    {
        const auto at = milo::find(scene, name);
        if (!at || milo::find(scene, as))
            return false;
        milo::Bytes body = scene.bodies[*at];
        if (!change(body))
            return false;
        milo::add(scene, scene.entries[*at].first, as, std::move(body));
        return true;
    }

    bool recolor(milo::Bytes &body, uint32_t version, const std::function<void(float *rgb)> &turn)
    {
        const auto tex = milo::readTex(body, version);
        const auto header = tex ? bitmap::read(body.data() + tex->bitmap, body.size() - tex->bitmap) : std::nullopt;
        if (!header || bitmap::paletteBytes(*header) == 0u)
            return false;
        uint8_t *palette = body.data() + tex->bitmap + bitmap::kHeaderBytes;
        for (size_t k = 0; k < bitmap::paletteBytes(*header) / 4u; ++k)
        {
            uint8_t *c = palette + bitmap::paletteSlot(k, header->bpp) * 4u;
            float rgb[3] = {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f};
            turn(rgb);
            for (int ch = 0; ch < 3; ++ch)
                c[ch] = static_cast<uint8_t>(std::lround(std::clamp(rgb[ch], 0.0f, 1.0f) * 255.0f));
        }
        return true;
    }

    bool repaint(milo::Bytes &body, uint32_t version,
                 const std::function<void(size_t index, size_t entries, uint8_t *rgba)> &colour,
                 const std::function<uint8_t(uint32_t x, uint32_t y, uint32_t w, uint32_t h)> &index)
    {
        const auto tex = milo::readTex(body, version);
        auto header = tex ? bitmap::read(body.data() + tex->bitmap, body.size() - tex->bitmap) : std::nullopt;
        if (!header || bitmap::paletteBytes(*header) == 0u || (header->bpp != 4u && header->bpp != 8u))
            return false;
        header->encoding &= ~uint32_t{::milo::bitmap::kSwizzled};
        milo::Bytes fresh;
        bitmap::write(fresh, *header);
        std::memcpy(body.data() + tex->bitmap, fresh.data(), bitmap::kHeaderBytes);
        uint8_t *palette = body.data() + tex->bitmap + bitmap::kHeaderBytes;
        const size_t entries = bitmap::paletteBytes(*header) / 4u;
        for (size_t k = 0; k < entries; ++k)
            colour(k, entries, palette + bitmap::paletteSlot(k, header->bpp) * 4u);
        size_t at = tex->bitmap + bitmap::kHeaderBytes + bitmap::paletteBytes(*header);
        for (uint32_t level = 0; level <= header->mips; ++level)
        {
            const uint32_t w = std::max(1u, uint32_t{header->width} >> level),
                           h = std::max(1u, uint32_t{header->height} >> level);
            const size_t row = (size_t{w} * header->bpp + 7u) / 8u;
            if (at + row * h > body.size())
                return false;
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint8_t i = index(x, y, w, h);
                    uint8_t &byte = body[at + y * row + (x * header->bpp) / 8u];
                    if (header->bpp == 8u)
                        byte = i;
                    else
                        byte = x & 1u ? static_cast<uint8_t>((byte & 0x0fu) | (i << 4u))
                                      : static_cast<uint8_t>((byte & 0xf0u) | i);
                }
            at += row * h;
        }
        return true;
    }

    uint32_t trackNum(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t config)
    {
        return static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->trackConfigTrackNum, {config}));
    }

    uint32_t findObject(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t dir, const std::string &name)
    {
        // Each as it was found, while it still is that object.
        auto [it, fresh] = s_found.try_emplace({dir, name}, 0u);
        if (!fresh && (it->second == 0u || (load<uint32_t>(rdram, it->second + ::milo::object::kDir) == dir &&
                                            objectName(rdram, it->second) == name)))
            return it->second;
        const uint32_t sp = GPR_U32(ctx, 29);
        const uint32_t text = (sp - 0x40u - static_cast<uint32_t>(name.size())) & ~0xfu;
        std::memcpy(getMemPtr(rdram, text), name.c_str(), name.size() + 1u);
        SET_GPR_U32(ctx, 29, text - 0x20u);
        it->second =
            static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->objectDirFindObject, {dir, text, 0u}));
        SET_GPR_U32(ctx, 29, sp);
        return it->second;
    }

    std::string_view objectName(uint8_t *rdram, uint32_t object)
    {
        const uint32_t chars = object ? load<uint32_t>(rdram, object + ::milo::object::kName) : 0u;
        return chars ? reinterpret_cast<const char *>(getMemPtr(rdram, chars)) : "";
    }

    uint32_t partObject(uint8_t *rdram, uint32_t rep, uint32_t part)
    {
        const uint32_t mesh = load<uint32_t>(rdram, rep + part);
        const uint32_t object = mesh ? mesh + ::milo::mesh::kObjectBase : 0u;
        return object && load<uint32_t>(rdram, object + ::milo::object::kDir) && !objectName(rdram, object).empty()
                   ? object
                   : 0u;
    }

    void showPartAs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t rep, uint32_t part,
                    uint32_t object, bool multimesh, uint32_t turnsWith)
    {
        const uint32_t mesh = object - ::milo::mesh::kObjectBase;
        uint32_t &shown = s_templates[load<uint32_t>(rdram, rep + gemRep::kTemplate)][mesh];
        if (shown == 0u)
        {
            shown = static_cast<uint32_t>(
                runtime->callGuestFunction(rdram, ctx, s_addresses->builtinNew, {kPartTemplateSize}));
            runtime->callGuestFunction(rdram, ctx, s_addresses->gemPartTemplateCtor, {shown, mesh, multimesh ? 1u : 0u});
        }
        if (turnsWith != 0u)
            s_turnsWith[mesh] = turnsWith - ::milo::mesh::kObjectBase;
        setPartTemplate(rdram, ctx, runtime, rep, part, shown);
    }

    uint32_t partTemplate(uint8_t *rdram, uint32_t rep, uint32_t part)
    {
        return load<uint32_t>(rdram, rep + part + gemRep::kPartTemplate) - 8u;
    }

    void setPartTemplate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t rep, uint32_t part,
                         uint32_t shown)
    {
        runtime->callGuestFunction(rdram, ctx, s_addresses->gemRepPartSetTemplate, {rep + part, shown});
    }

    void installGemLooks(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<DrawShowingTag>::install(runtime, addresses.gemRepTemplateDrawShowing, onDrawShowing);
        EntryHook<TemplateGoneTag>::install(runtime, addresses.gemRepTemplateDtor, onTemplateGone);
        EntryHook<AdvanceEndTag>::install(runtime, addresses.gemManagerAdvanceEnd, onAdvanceEnd);
        EntryHook<PoolGoneTag>::install(runtime, addresses.gemRepPoolDtor, onPoolGone);
    }
}
