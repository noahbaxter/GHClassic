// GH2's hooks, installed once the runtime has loaded a matching executable.

#include "addresses.h"
#include "game_overrides.h"
#include "null_movie.h"
#include "null_rnd.h"
#include "pad.h"
#include "render/mesh_capture.h"
#include "render/native_environ.h"
#include "render/native_mat.h"
#include "render/native_mesh.h"
#include "render/native_rnd.h"
#include "render/texture_capture.h"
#include "silent_synth.h"
#include "video_options.h"

namespace
{
    void applySlus21447(PS2Runtime &runtime)
    {
        gh2::installNullRnd(runtime, gh2::kSlus21447);
        gh2::installNativeRnd(runtime, gh2::kSlus21447);
        gh2::installMeshCapture(runtime, gh2::kSlus21447);
        gh2::installTextureCapture(runtime, gh2::kSlus21447);
        gh2::installNativeMesh(runtime, gh2::kSlus21447);
        gh2::installNativeMat(runtime, gh2::kSlus21447);
        gh2::installNativeEnviron(runtime, gh2::kSlus21447);
        gh2::installSilentSynth(runtime, gh2::kSlus21447);
        gh2::installNullMovie(runtime, gh2::kSlus21447);
        gh2::installVideoOptions(runtime, gh2::kSlus21447);
        gh2::installPad(runtime, gh2::kSlus21447);
    }
}

// Matched on the entry point, which the symbolized ELF keeps from retail.
PS2_REGISTER_GAME_OVERRIDE("Guitar Hero II (USA)", "", gh2::kSlus21447.entry, 0u, applySlus21447)
