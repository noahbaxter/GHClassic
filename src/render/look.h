#pragma once

// A 360 material's shading beyond what the PS2's Mat holds: its normal,
// specular, glow, diffuse ramp and cube maps, kept on the host as texture
// payloads (render/texture_store.h), and the values only the 360's shader
// reads. Filled as a converted scene loads (gh2x/models.cpp) and found at
// draw time (render/native_mat.cpp) by the Mat's name and the payload its
// diffuse texture stands in for, which names its 360 file: a scene's own
// directory has no name when a proxy loads it (battle's geometry).

#include "render/scene.h"

#include <cstdint>
#include <memory>
#include <string>

namespace gh2::look
{
    struct Look
    {
        // Payload ids, 0 for none. The diffuse's is the key's, not a map.
        uint32_t diffuse = 0;
        uint32_t normal = 0;
        uint32_t specular = 0;
        uint32_t glow = 0;
        uint32_t ramp = 0;
        uint32_t cube = 0;
        float emissive = 1.0f;
        float specularColor[4] = {}; // rgb without a specular map, then the power
        bool normalize = false;
    };

    void add(const std::string &mat, const Look &look);

    // The shading of the Mat of that name whose diffuse texture stands in
    // for that payload (0 for none), its maps read: none when no scene
    // registered one. Game thread only.
    std::shared_ptr<const Shading> find(const char *mat, uint32_t diffuse);

    // A 360 ParticleSys whose particles turn, by its name and its Mat's
    // (two venues share a system's name), and the turns a minute each
    // particle takes, somewhere between the two.
    void addSpin(const std::string &system, const std::string &mat, float rpmLow, float rpmHigh);
    // That system's two rates, null when its particles do not turn.
    const float *findSpin(const char *system, const char *mat);

    // The Mat of that diffuse payload, when one look alone has it: for a
    // Mat the game made by copying a scene's, which has no name (the track's
    // side rails). Empty for none.
    std::string matOf(uint32_t diffuse);

    // A track's board: the Mat its track code made and put the guitarist's
    // fretboard on (CharTrackSurface::SetCharTexture), a guest address. It
    // has no name and its picture is no scene's. A song has one a player;
    // one added long after the last begins the next song's.
    void addBoard(uint32_t mat);
    bool isBoard(uint32_t mat);
    bool hasBoard();
    constexpr const char *kBoardMat = "track_surface.mat";
    // The environ the 360 draws the board under, where the PS2's scene has
    // the track's one environ for everything.
    constexpr const char *kBoardEnviron = "wood_track_surface.env";
    // Star power, as the game starts and stops it (gh2x/track.cpp). The
    // 360's board light swings while it is on, with the track's lightning;
    // when it goes off the PS2's track leaves that animation where it
    // stood, so the board's lights are then put back as they were before
    // the song's first.
    void setStarPower(bool on);
    void restBoard(Environ &board);
    // And the one it draws the bar and beat lines under (its
    // bar_widgets.grp's), which are known by their Mats.
    constexpr const char *kLineEnviron = "frets_beatmarkers.env";
    constexpr const char *kLineMats[] = {"bar_line.mat", "beat_line.mat", "half_beat_line.mat"};

    // Whether the track fades into the distance in its shader, as the 360's
    // does: with its track and its shading. The PS2's Track::Draw (0x151188)
    // copies the picture behind the track's far end and draws it back over
    // in two rects, each with a rect that adds over it; a Xenia frame has
    // none of them, so then neither the copies nor these Mats' rects are
    // drawn.
    bool trackFades();
    constexpr const char *kStripMats[] = {"track_fade.mat", "track_fade2.mat", "track_add.mat"};

    // Maps no draw has asked for in a while are let go.
    void endFrame();

    // Whether the frame last finished drew a venue (render/frame.h).
    void setInVenue(bool drawn);
    bool inVenue();

    // The 360's luminance map, which its post stage scales the world's
    // colour by (render/frame.h), as a payload: 0 for none.
    void setLuminance(uint32_t id);
    // Its pixels. Game thread only.
    std::shared_ptr<const TextureData> luminance();
}
