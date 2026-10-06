#pragma once

#include "host/mesh_push.h"
#include "render/frame.h"

#include <filesystem>
#include <string>

// A frame's draws as numbers, to set beside a GS dump of the retail game
// (tools/gsdump.py): each draw's state and every vertex as mesh.vert leaves
// it, worked out here from what the GPU is given, and beside it each
// texture's first level (draws_<name>.rgba, indexed by the JSON's
// "textures": hash to offset, width, height).
namespace gh2
{
    // The next frame drawn is written to `dir` as draws_<name>.json. From
    // any thread.
    void setDrawDumpDir(const std::filesystem::path &dir);
    // With fromSerial, the first frame drawn of that serial or later.
    void requestDrawDump(const std::string &name, uint64_t fromSerial = 0);
    // A requested dump not yet written.
    bool drawDumpPending();

    // The renderer's side: writes the dump asked for, if one was, from the
    // frame's draws, their push constants and the frame data they index.
    void writeDrawDump(const Frame &frame, const PushConstants *pushes, const Vec4 *data);
}
