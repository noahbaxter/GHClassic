// note_engine: whose rules judge a song, set as a career is entered and from
// quickplay's pause menu, and taken as the next song loads. GH2's are
// retail's own: no code of ours, every hook hands GH2's calls to the
// original. GH1's are GH2's with GH1's differences put back, in gh1_*.cpp.

#include "gameplay/note_engine/note_engine.h"

#include "content/campaigns.h"
#include "content/games.h"
#include "gameplay/note_engine/gh1.h"
#include "save/save.h"
#include "script.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>

namespace gh2
{
    namespace
    {
        constexpr const char *kNames[] = {"gh2", "gh1"}; // kNoteEngines's

        std::atomic<NoteEngine> s_engine{NoteEngine::kGh2};

        // How many can be picked.
        int playable()
        {
            return static_cast<int>(std::count_if(kNoteEngines.begin(), kNoteEngines.end(), noteEnginePlayable));
        }

        //   {note_engine}              the engine judging now
        //   {note_engine next <engine>}  the playable one after it, wrapping
        //   {note_engine choices}      how many are playable: with one,
        //                              nothing offers a pick
        //   {note_engine picked}       the one picked, which a career is
        //                              entered under
        //   {note_engine pick <engine>}  picked from now; out of a career it
        //                              judges now too, as set
        //   {note_engine locked}       whether in a career, whose engine
        //                              stays until the next is entered
        //   {note_engine set <engine>} picked, and judging now, and the
        //                              careers' it holds with it; TRUE, or
        //                              FALSE in a career, whose engine stays,
        //                              or for one that is not playable
        script::Node command(const script::Call &call)
        {
            const auto sym = [&](NoteEngine engine) {
                return script::Node{script::symbol(call.rdram, call.ctx, call.runtime, noteEngineName(engine)),
                                    script::kSymbol};
            };
            if (call.size() < 2)
                return sym(noteEngine());
            const std::string op = call.symbol(1);
            const std::optional<NoteEngine> engine = noteEngineNamed(call.size() > 2 ? call.symbol(2) : "");
            if (op == "next" && engine)
            {
                size_t i = static_cast<size_t>(*engine);
                do
                    i = (i + 1u) % kNoteEngines.size();
                while (!noteEnginePlayable(kNoteEngines[i]));
                return sym(kNoteEngines[i]);
            }
            if (op == "choices")
                return {static_cast<uint32_t>(playable()), script::kInt};
            if (op == "picked")
                return sym(pickedNoteEngine());
            if (op == "locked")
                return {campaigns::career() ? 1u : 0u, script::kInt};
            if ((op != "set" && op != "pick") || !engine)
            {
                std::cerr << "[note_engine] usage: {note_engine}, {note_engine picked|locked}, "
                             "{note_engine set|pick|next gh2|gh1}"
                          << std::endl;
                return {};
            }
            if (!noteEnginePlayable(*engine))
                return {0u, script::kInt};
            if (campaigns::career())
            {
                if (op == "pick")
                    pickNoteEngine(*engine);
                return {op == "pick" ? 1u : 0u, script::kInt};
            }
            judgeNoteEngine(*engine, call.rdram, call.ctx, call.runtime);
            pickNoteEngine(*engine);
            return {1u, script::kInt};
        }
    }

    const char *noteEngineName(NoteEngine engine)
    {
        return kNames[static_cast<size_t>(engine)];
    }

    std::optional<NoteEngine> noteEngineNamed(std::string_view name)
    {
        for (const NoteEngine engine : kNoteEngines)
            if (name == noteEngineName(engine))
                return engine;
        return std::nullopt;
    }

    bool noteEnginePlayable(NoteEngine engine)
    {
        return engine == NoteEngine::kGh2 || games::mounted("gh1");
    }

    NoteEngine pickedNoteEngine()
    {
        // One kept that cannot be played now (GH1's with its disc out) plays
        // as GH2's, and stays kept for when it can.
        const std::optional<NoteEngine> engine = noteEngineNamed(save::keptEngine());
        return engine && noteEnginePlayable(*engine) ? *engine : NoteEngine::kGh2;
    }

    void pickNoteEngine(NoteEngine engine)
    {
        save::keepEngine(noteEngineName(engine));
    }

    NoteEngine noteEngine()
    {
        return s_engine;
    }

    void judgeNoteEngine(NoteEngine engine, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (engine == s_engine)
            return;
        save::leaveGame(rdram, ctx, runtime);
        s_engine = engine;
        save::reloadGame(rdram, ctx, runtime);
        std::cout << "[note_engine] " << noteEngineName(engine) << std::endl;
    }

    void installNoteEngine(PS2Runtime &runtime, const Addresses &addresses)
    {
        // After ark::install, with every disc mounted, before the boot's load.
        s_engine = pickedNoteEngine();
        script::addCommand("note_engine", command);
        gh1_rules::installWatcher(runtime, addresses);
        gh1_rules::installStarPower(runtime, addresses);
        gh1_rules::installScoring(runtime, addresses);
    }
}
