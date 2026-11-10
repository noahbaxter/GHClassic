#pragma once

#include "addresses.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

class PS2Runtime;
struct R5900Context;

namespace gh2
{
    // Each one's value is its place in kNoteEngines.
    enum class NoteEngine : uint8_t
    {
        kGh2,
        kGh1,
    };
    // Every note engine, GH2's first: the save keeps each one's careers.
    constexpr std::array kNoteEngines{NoteEngine::kGh2, NoteEngine::kGh1};

    // As scripts, state.ini and the save's sections name it: gh2, gh1.
    const char *noteEngineName(NoteEngine engine);
    std::optional<NoteEngine> noteEngineNamed(std::string_view name);
    // Whether that one can be picked: GH1's only with its disc mounted.
    bool noteEnginePlayable(NoteEngine engine);

    // The one picked, which every career is entered under and quickplay
    // plays: state.ini's, or GH2's while that one cannot be played.
    NoteEngine pickedNoteEngine();
    void pickNoteEngine(NoteEngine engine);
    // The one judging now, from any thread: the picked one from boot on,
    // until judgeNoteEngine.
    NoteEngine noteEngine();
    // Judging from the next song loaded. The career loaded now is kept under
    // the engine judging it, and the same game's comes out of `engine`'s.
    void judgeNoteEngine(NoteEngine engine, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    void installNoteEngine(PS2Runtime &runtime, const Addresses &addresses);
}
