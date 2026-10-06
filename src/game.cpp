// GH2's hooks, installed once the runtime has loaded a matching executable.

#include "addresses.h"
#include "disc/ark.h"
#include "content/encores.h"
#include "content/games.h"
#include "content/locale.h"
#include "content/outfits.h"
#include "content/campaigns.h"
#include "content/setlists.h"
#include "content/songs.h"
#include "ui/fast_boot.h"
#include "frame_step.h"
#include "game_overrides.h"
#include "gh1/face.h"
#include "gh1/install.h"
#include "gh80s/install.h"
#include "gh2x/install.h"
#include "ui/help_bar.h"
#include "settings/latency.h"
#include "ui/locale.h"
#include "ui/menus.h"
#include "ui/meta_music.h"
#include "movie/movie.h"
#include "dev/cheats.h"
#include "dev/null_rnd.h"
#include "host/pad.h"
#include "render/mesh_capture.h"
#include "render/native_cull.h"
#include "render/native_environ.h"
#include "render/native_mesh.h"
#include "render/native_particles.h"
#include "render/native_points.h"
#include "render/native_rect.h"
#include "render/native_rnd.h"
#include "render/texture_capture.h"
#include "save/card.h"
#include "save/save.h"
#include "dev/scenario.h"
#include "script.h"
#include "dev/seed.h"
#include "dev/transplant.h"
#include "synth/synth.h"
#include "settings/video_options.h"
#include "sustain_release.h"
#include "track_start.h"
#include "whammy_hold.h"

namespace
{
    void applySlus21447(PS2Runtime &runtime)
    {
        gh2::ark::install(runtime, gh2::kSlus21447);
        gh2::installEighties();
        gh2::installGh1();
        gh2::installGh2x();
        gh2::installGh1Face(runtime, gh2::kSlus21447);
        gh2::outfits::install(runtime, gh2::kSlus21447);
        gh2::installNullRnd(runtime, gh2::kSlus21447);
        gh2::installNativeRnd(runtime, gh2::kSlus21447);
        gh2::installMeshCapture(runtime, gh2::kSlus21447);
        gh2::installTextureCapture(runtime, gh2::kSlus21447);
        gh2::installNativeMesh(runtime, gh2::kSlus21447);
        gh2::installNativeEnviron(runtime, gh2::kSlus21447);
        gh2::installNativeRect(runtime, gh2::kSlus21447);
        gh2::installNativeParticles(runtime, gh2::kSlus21447);
        gh2::installNativePoints(runtime, gh2::kSlus21447);
        gh2::installNativeCull(runtime, gh2::kSlus21447);
        gh2::installSynth(runtime, gh2::kSlus21447);
        gh2::installMetaMusic(runtime, gh2::kSlus21447);
        gh2::installTrackStart(runtime, gh2::kSlus21447);
        gh2::installSustainRelease(runtime, gh2::kSlus21447);
        gh2::installWhammyHold(runtime, gh2::kSlus21447);
        gh2::installMovies(runtime, gh2::kSlus21447);
        gh2::installVideoOptions(runtime, gh2::kSlus21447);
        gh2::card::install(runtime, gh2::kSlus21447);
        gh2::save::install(runtime, gh2::kSlus21447);
        gh2::script::install(runtime, gh2::kSlus21447);
        gh2::installLocale(runtime, gh2::kSlus21447);
        gh2::installLatency(runtime, gh2::kSlus21447);
        gh2::installPad(runtime, gh2::kSlus21447);
        gh2::installFrameStep(runtime, gh2::kSlus21447);
        gh2::installMenus(runtime);
        gh2::installHelpBar(runtime, gh2::kSlus21447);
        gh2::setlists::install(runtime, gh2::kSlus21447);
        gh2::encores::install(runtime, gh2::kSlus21447);
        gh2::songs::install();
        gh2::locale::install(runtime, gh2::kSlus21447);
        gh2::games::install(runtime, gh2::kSlus21447);
        gh2::campaigns::install(runtime, gh2::kSlus21447);
        gh2::scenario::install(runtime, gh2::kSlus21447);
        gh2::transplant::install(runtime, gh2::kSlus21447);
        gh2::cheats::install(runtime, gh2::kSlus21447);
        gh2::fast_boot::install(runtime, gh2::kSlus21447);
        gh2::seed::install(runtime, gh2::kSlus21447);
    }
}

// Matched on the entry point, which the symbolized ELF keeps from retail.
PS2_REGISTER_GAME_OVERRIDE("Guitar Hero II (USA)", "", gh2::kSlus21447.entry, 0u, applySlus21447)
