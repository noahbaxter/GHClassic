// GH2's hooks, installed once the runtime has loaded a matching executable.

#include "addresses.h"
#include "fast_boot.h"
#include "frame_step.h"
#include "game_overrides.h"
#include "latency.h"
#include "menus.h"
#include "meta_music.h"
#include "movie/movie.h"
#include "null_rnd.h"
#include "pad.h"
#include "render/mesh_capture.h"
#include "render/native_environ.h"
#include "render/native_mesh.h"
#include "render/native_particles.h"
#include "render/native_rect.h"
#include "render/native_rnd.h"
#include "render/texture_capture.h"
#include "scenario.h"
#include "script.h"
#include "seed.h"
#include "synth/synth.h"
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
        gh2::installNativeEnviron(runtime, gh2::kSlus21447);
        gh2::installNativeRect(runtime, gh2::kSlus21447);
        gh2::installNativeParticles(runtime, gh2::kSlus21447);
        gh2::installSynth(runtime, gh2::kSlus21447);
        gh2::installMetaMusic(runtime, gh2::kSlus21447);
        gh2::installMovies(runtime, gh2::kSlus21447);
        gh2::installVideoOptions(runtime, gh2::kSlus21447);
        gh2::script::install(runtime, gh2::kSlus21447);
        gh2::installLatency(runtime, gh2::kSlus21447);
        gh2::installPad(runtime, gh2::kSlus21447);
        gh2::installFrameStep(runtime, gh2::kSlus21447);
        gh2::installMenus(runtime);
        gh2::scenario::install(runtime, gh2::kSlus21447);
        gh2::fast_boot::install(runtime, gh2::kSlus21447);
        gh2::seed::install(runtime, gh2::kSlus21447);
    }
}

// Matched on the entry point, which the symbolized ELF keeps from retail.
PS2_REGISTER_GAME_OVERRIDE("Guitar Hero II (USA)", "", gh2::kSlus21447.entry, 0u, applySlus21447)
