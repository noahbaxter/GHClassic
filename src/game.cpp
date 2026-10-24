// GH2's hooks, installed once the runtime has loaded a matching executable.

#include "addresses.h"
#include "disc/ark.h"
#include "content/band.h"
#include "content/crowd_cards.h"
#include "content/focus.h"
#include "content/floor_spot.h"
#include "content/encores.h"
#include "content/quickplay.h"
#include "content/games.h"
#include "content/heaps.h"
#include "content/hidden_guitars.h"
#include "content/tour_or_higher.h"
#include "content/locale.h"
#include "content/outfits.h"
#include "content/campaign_case.h"
#include "content/campaigns.h"
#include "content/setlists.h"
#include "content/songs.h"
#include "content/status_line.h"
#include "ui/fast_boot.h"
#include "frame_step.h"
#include "game_overrides.h"
#include "gh1/face.h"
#include "gh1/install.h"
#include "gh80s/install.h"
#include "gh2x/install.h"
#include "ui/help_bar.h"
#include "ui/classic_scene.h"
#include "ui/zebra_wall.h"
#include "settings/latency.h"
#include "settings/settings.h"
#include "ui/locale.h"
#include "ui/menus.h"
#include "ui/meta_music.h"
#include "ui/photo_mode.h"
#include "ui/slider_scale.h"
#include "movie/movie.h"
#include "dev/cheats.h"
#include "gameplay/hopo_lock.h"
#include "lefty_guitarist.h"
#include "gameplay/note_speed.h"
#include "gameplay/note_engine/note_engine.h"
#include "gameplay/retrack.h"
#include "streak/counter.h"
#include "streak/note_streak.h"
#include "dev/null_rnd.h"
#include "host/pad.h"
#include "render/char_lod.h"
#include "render/font_bake.h"
#include "render/mesh_capture.h"
#include "render/native_cull.h"
#include "render/native_environ.h"
#include "render/native_mat.h"
#include "render/native_mesh.h"
#include "render/native_particles.h"
#include "render/native_points.h"
#include "render/native_rect.h"
#include "render/native_rnd.h"
#include "render/texture_capture.h"
#include "save/card.h"
#include "save/save.h"
#include "dev/scenario.h"
#include "dev/scripted_play.h"
#include "script.h"
#include "dev/seed.h"
#include "dev/transplant.h"
#include "synth/synth.h"
#include "settings/video_options.h"
#include "gameplay/strum_limit.h"
#include "gameplay/sustain_release.h"
#include "gameplay/track_start.h"
#include "gameplay/whammy_hold.h"

namespace
{
    void applySlus21447(PS2Runtime &runtime)
    {
        gh2::ark::install(runtime, gh2::kSlus21447);
        gh2::installHeaps();
        gh2::installGuitarCopy();
        // Before the 80s, whose setlist reads its song list scene.
        gh2::installZebraWall();
        gh2::installEighties();
        gh2::installGh1();
        gh2::installClassicLogo();
        gh2::installGh2x();
        gh2::installHiddenGuitars();
        gh2::installGh1Face(runtime, gh2::kSlus21447);
        gh2::outfits::install(runtime, gh2::kSlus21447);
        gh2::band::install(runtime, gh2::kSlus21447);
        gh2::quickplay::install(runtime, gh2::kSlus21447);
        gh2::crowd_cards::install(runtime, gh2::kSlus21447);
        gh2::focus::install(runtime, gh2::kSlus21447);
        gh2::floor_spot::install(runtime, gh2::kSlus21447);
        gh2::installNullRnd(runtime, gh2::kSlus21447);
        gh2::installNativeRnd(runtime, gh2::kSlus21447);
        gh2::installMeshCapture(runtime, gh2::kSlus21447);
        gh2::installCharLod(runtime, gh2::kSlus21447);
        gh2::installTextureCapture(runtime, gh2::kSlus21447);
        gh2::installNativeMat(runtime, gh2::kSlus21447);
        gh2::installNativeMesh(runtime, gh2::kSlus21447);
        // After focus, whose hook on a shot's Interp runs inside this one's,
        // and the native mesh, whose draw this one's hook runs before.
        gh2::installLeftyGuitarist(runtime, gh2::kSlus21447);
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
        gh2::installHopoLock(runtime, gh2::kSlus21447);
        // Off, neither is installed: the score and rock meters stay where
        // retail has them.
        if (gh2::settings::get(gh2::settings::kNoteStreak))
        {
            gh2::installNoteStreak(runtime, gh2::kSlus21447);
            gh2::installStreakCounter(runtime, gh2::kSlus21447);
        }
        gh2::installFontBake();
        gh2::installNoteEngine(runtime, gh2::kSlus21447);
        // After the note engine, whose hook on BeatMatcher::Swing it wraps:
        // a strum it drops never reaches the engine.
        gh2::installStrumLimit(runtime, gh2::kSlus21447);
        gh2::installTourOrHigher(runtime, gh2::kSlus21447);
        gh2::installNoteSpeed(runtime, gh2::kSlus21447);
        gh2::installRetrack(runtime, gh2::kSlus21447);
        gh2::slider_scale::install(runtime, gh2::kSlus21447);
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
        gh2::installPhotoMode(runtime, gh2::kSlus21447);
        gh2::installHelpBar(runtime, gh2::kSlus21447);
        // After the help bar, which recolours its scene's own pixels.
        gh2::installGh2xAssets(runtime, gh2::kSlus21447);
        // After the 360's scenes, the streak counter and the font bake: its
        // highway starts from theirs, and is served over the 360's.
        gh2::installGh1Highway();
        // After the 360's menu art, whose speed poster it puts the amp over.
        gh2::campaign_case::install();
        gh2::setlists::install(runtime, gh2::kSlus21447);
        gh2::encores::install(runtime, gh2::kSlus21447);
        gh2::status_line::install(runtime, gh2::kSlus21447);
        gh2::songs::install();
        gh2::locale::install(runtime, gh2::kSlus21447);
        gh2::games::install(runtime, gh2::kSlus21447);
        gh2::campaigns::install(runtime, gh2::kSlus21447);
        gh2::scenario::install(runtime, gh2::kSlus21447);
        gh2::scripted_play::install(runtime, gh2::kSlus21447);
        gh2::transplant::install(runtime, gh2::kSlus21447);
        gh2::cheats::install(runtime, gh2::kSlus21447);
        gh2::fast_boot::install(runtime, gh2::kSlus21447);
        gh2::seed::install(runtime, gh2::kSlus21447);
    }
}

// Matched on the entry point, which the symbolized ELF keeps from retail.
PS2_REGISTER_GAME_OVERRIDE("Guitar Hero II (USA)", "", gh2::kSlus21447.entry, 0u, applySlus21447)
