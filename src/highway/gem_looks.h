#pragma once

// What gems of every kind share in how they are drawn (open and tap gems,
// star phrases' under star power): pieces added to the track's scene
// as it is read, GH2's gems' parts shown as those pieces, and the track's
// gems as they are added and as the track goes.
//
// A Gem (Gem::AddRep, 0x157960) has a GemRep for each fret it has, from its
// track's GemRepPool, and a GemRep is parts, each shown in a GemPartTemplate
// of its mesh, which draws them (GemPartTemplate::DrawAllParts, 0x15fb10):
// one by one with the part's mesh, or, for the parts after the gem's own two,
// all at once as its own mesh's multimesh. GemRepTemplate builds one for each
// lane's mesh of each part from track_graphics' (gem ...), so a part shown as
// any other mesh needs a template of its own (showPartAs).

#include "addresses.h"
#include "milo/milo.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class PS2Runtime;
struct R5900Context;

namespace gh2
{
    // Gem: its reps, a vector, and its frets, the low byte of a word of
    // flags (Gem::Gem, 0x157738): hit or passed (bits 9 and 10, which GH2
    // skips) and a HOPO (bit 11, which Gem::AddRepForSlot gives its reps).
    namespace gem
    {
        constexpr uint32_t kReps = 0x00u, kRepsEnd = 0x04u, kFrets = 0x1cu;
        constexpr uint32_t kHitOrPassed = 0x600u, kHopo = 1u << 11u;
    }

    // GemManager: its TrackConfig, its Gems (kGemSize each), the first on the
    // track and the next to add; whether star power is on and its phrase
    // lost (from OnMissPhrase).
    namespace gemManager
    {
        constexpr uint32_t kConfig = 0x00u, kGems = 0x04u, kFirst = 0x20u, kNext = 0x24u, kGemSize = 0x20u;
        constexpr uint32_t kBonus = 0x2f4u, kPhraseLost = 0x2f8u;
    }
    // NowBar: its TrackConfig. SlotViews: each slot's view, a pointer each.
    namespace nowBar
    {
        constexpr uint32_t kConfig = 0x2cu;
    }
    namespace slotViews
    {
        constexpr uint32_t kViews = 0x50u;
    }

    namespace gemRep
    {
        // Its parts, each its mesh first: the cap (a strum's top or a HOPO's),
        // the gem, its glow, a star's base, top and body, and the star's
        // shine. A part's template is at +0x10, less 8 (Part::SetTemplate,
        // 0x158028).
        constexpr uint32_t kCap = 0x04u, kGem = 0x1cu, kStarBase = 0x4cu, kStarTop = 0x64u, kStarBody = 0x7cu,
                           kStarShine = 0x94u;
        constexpr uint32_t kParts[] = {0x04u, 0x1cu, 0x34u, 0x4cu, 0x64u, 0x7cu, 0x94u};
        constexpr uint32_t kPartTemplate = 0x10u;
        // While it has a sustain, its glow (a RndLine) and its tail (a mesh
        // from its pool's, CreateTail 0x159b40).
        constexpr uint32_t kGlow = 0xacu, kTail = 0xb0u;
        constexpr uint32_t kTailAcross = 6u; // its tail's vertices a row
        constexpr uint32_t kPulse = 0x254u;    // the whammy's pulse on its glow, eased 1 to 2.5 (0x1594e0)
        constexpr uint32_t kType = 0x258u;     // as SetType sets it
        constexpr uint32_t kState = 0x25cu;    // kHeld once hit, as it is held (0x158cd8)
        constexpr uint32_t kTemplate = 0x26cu; // its GemRepTemplate
        // Its types: a star phrase's, and every gem's under star power.
        constexpr uint32_t kTypeStar = 1u, kTypeBonus = 2u;
        constexpr uint32_t kHeld = 2u;
    }

    // Star power's cyan, as a gem's colours go (1 a lane's full).
    constexpr float kStarPowerCyan[3] = {0.3f, 1.0f, 1.0f};
    // gem.tex turned to these hues (degrees): star power's cyan, an open's
    // purple.
    constexpr float kStarPowerHue = 180.0f, kOpenHue = 280.0f;

    // The track's scene through `add` as it is read, for pieces of its own.
    void addToTrackScene(void (*add)(milo::Dir &scene));

    // A gem GemManager::AdvanceEnd (0x156868) is adding to the track: its
    // manager, its Gem (the manager's next, +0x24, of its Gems at +0x04,
    // 0x20 each), that index, the track's number, and its GameGem
    // (SongDB::GetGem), 0 for none.
    struct GemAdded
    {
        uint32_t manager, gem, index, track, game;
    };
    // Each called as a gem is added, the last given first; and as the
    // track's GemRepPool goes (~GemRepPool, 0x159810), its scene and reps
    // with it. Their calls into the guest need put back nothing.
    void onGemAdded(void (*added)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const GemAdded &gem));
    void onTrackGone(void (*gone)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t pool));

    // A mesh body's vertices: a count, then that many of position, normal,
    // colour (four floats) and uv, each kMeshVertex long, its uv at
    // kMeshVertexUv. Where the count is, found as the first whose vertices'
    // positions all read as such.
    constexpr size_t kMeshVertex = 48u, kMeshVertexUv = 40u;
    std::optional<size_t> meshVertices(const milo::Bytes &body);
    // Those vertices, each's bytes, and their lowest height (z).
    struct MeshVertices
    {
        uint8_t *first;
        uint32_t count;
        float bottom;
        uint8_t *operator[](uint32_t i) const { return first + i * kMeshVertex; }
    };
    std::optional<MeshVertices> vertices(milo::Bytes &body);

    // The this a mesh's Sync takes, through its vtable (+0x34), moved by the
    // int16 at +0x48 (as at 0x1592e0).
    uint32_t syncThis(uint8_t *rdram, uint32_t mesh);

    // `name`'s body in the scene copied as `as`, changed by `change`. False if
    // the scene has no `name` or has `as` already, or `change` fails.
    bool addCopy(milo::Dir &scene, const std::string &name, const std::string &as,
                 const std::function<bool(milo::Bytes &)> &change);
    // A Tex body's palette, each colour (rgb, 0 to 1) through `turn`. False if
    // it has none.
    bool recolor(milo::Bytes &body, uint32_t version, const std::function<void(float *rgb)> &turn);
    // A Tex body of 4 or 8 bits a pixel painted anew, left unswizzled so a
    // column stays one: each palette entry (rgba, the PS2's) through
    // `colour`, given its index and how many there are, then each level's
    // every pixel the index `index` gives for its place. False if it has no
    // such palette or is short.
    bool repaint(milo::Bytes &body, uint32_t version,
                 const std::function<void(size_t index, size_t entries, uint8_t *rgba)> &colour,
                 const std::function<uint8_t(uint32_t x, uint32_t y, uint32_t w, uint32_t h)> &index);

    // A TrackConfig's track (TrackConfig::TrackNum).
    uint32_t trackNum(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t config);

    // `dir`'s object called `name` (its Hmx::Object), by ObjectDir::FindObject;
    // 0 if none. Kept until the track goes.
    uint32_t findObject(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t dir, const std::string &name);
    // An object's name, "" for none.
    std::string_view objectName(uint8_t *rdram, uint32_t object);
    // The Object of the mesh a rep's part shows, 0 if none or it is in no
    // directory.
    uint32_t partObject(uint8_t *rdram, uint32_t rep, uint32_t part);

    // The rep's part shown as the mesh whose Object is `object`, in a template
    // of its own drawn with the rep's GemRepTemplate: a multimesh, as GH2's
    // star parts are, or each part with its own mesh. With `turnsWith` (a
    // mesh's Object), it takes that mesh's world transform as it is drawn: a
    // copy added to the scene is not among the children its parent turns.
    void showPartAs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t rep, uint32_t part,
                    uint32_t object, bool multimesh = false, uint32_t turnsWith = 0u);
    // The template a rep's part is shown in, and it put back.
    uint32_t partTemplate(uint8_t *rdram, uint32_t rep, uint32_t part);
    void setPartTemplate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t rep, uint32_t part,
                         uint32_t shown);

    void installGemLooks(PS2Runtime &runtime, const Addresses &addresses);
}
