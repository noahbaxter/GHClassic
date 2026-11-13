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
#include "hub/hub_scene.h"
#include "hub/menu_guitarist.h"
#include "content/campaigns.h"
#include "content/setlists.h"
#include "content/songs.h"
#include "content/practice_sounds.h"
#include "content/song_speed.h"
#include "content/status_line.h"
#include "ui/fast_boot.h"
#include "frame_step.h"
#include "game_overrides.h"
#include "gh1/face.h"
#include "custom/install.h"
#include "highway/cyan_stars.h"
#include "gameplay/extended/guitar/gem_kinds.h"
#include "gameplay/extended/guitar/hammer_ons.h"
#include "gameplay/extended/guitar/lane_hits.h"
#include "highway/extended/guitar/lane_bands.h"
#include "gameplay/extended/guitar/open_chord_hits.h"
#include "highway/extended/guitar/open_gems.h"
#include "highway/extended/guitar/open_bursts.h"
#include "highway/extended/guitar/open_tails.h"
#include "highway/extended/guitar/sustain_tails.h"
#include "highway/extended/guitar/sustain_flames.h"
#include "gameplay/extended/guitar/sustain_hold.h"
#include "highway/extended/guitar/tap_gems.h"
#include "highway/extended/guitar/tap_line.h"
#include "highway/gem_looks.h"
#include "gh1/install.h"
#include "gh80s/install.h"
#include "gh2x/install.h"
#include "ui/help_bar.h"
#include "ui/controllers.h"
#include "ui/players.h"
#include "ui/classic_scene.h"
#include "ui/song_crown.h"
#include "ui/zebra_wall.h"
#include "settings/latency.h"
#include "settings/settings.h"
#include "ui/list_scroll.h"
#include "ui/transitions.h"
#include "ui/press_size.h"
#include "guest/bin_reads.h"
#include "ui/text_fit.h"
#include "ui/locale.h"
#include "ui/menus.h"
#include "ui/meta_music.h"
#include "ui/photo_mode.h"
#include "ui/song_progress.h"
#include "ui/slider_scale.h"
#include "movie/movie.h"
#include "dev/cheats.h"
#include "gameplay/engine/hopo_lock.h"
#include "lefty_guitarist.h"
#include "highway/note_speed.h"
#include "gameplay/engine/note_engine.h"
#include "highway/retrack.h"
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
#include "gameplay/engine/star_power_overlap.h"
#include "gameplay/engine/strum_limit.h"
#include "gameplay/engine/sustain_release.h"
#include "gameplay/engine/track_start.h"
#include "gameplay/engine/whammy_hold.h"

namespace
{
    void applySlus21447(PS2Runtime &runtime)
    {
        gh2::ark::install(runtime, gh2::kSlus21447);
        gh2::installHeaps();
        gh2::installGuitarCopy();
        // Before GH1's lists, built from GH2's song list and its type.
        gh2::installSongCrown(runtime, gh2::kSlus21447);
        // Before the 80s, whose setlist reads its song list scene.
        gh2::installZebraWall();
        gh2::installEighties();
        gh2::installGh1();
        gh2::installClassicLogo();
        gh2::installGh2x();
        gh2::installCustomSongs();
        gh2::song_speed::install(runtime, gh2::kSlus21447);
        gh2::practice_sounds::install(runtime, gh2::kSlus21447);
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
        gh2::installGemLooks(runtime, gh2::kSlus21447);
        gh2::guitar::installSustainTails(runtime, gh2::kSlus21447);
        gh2::guitar::installOpenGems(runtime, gh2::kSlus21447);
        gh2::guitar::installOpenTails(runtime, gh2::kSlus21447);
        gh2::guitar::installOpenBursts(runtime, gh2::kSlus21447);
        gh2::installWhammyHold(runtime, gh2::kSlus21447);
        gh2::installStarPowerOverlap(runtime, gh2::kSlus21447);
        gh2::guitar::installCyanStars(runtime, gh2::kSlus21447);
        // After the cyan stars, which it copies.
        gh2::guitar::installTapGems(runtime, gh2::kSlus21447);
        gh2::guitar::installTapLine(runtime, gh2::kSlus21447);
        gh2::installHopoLock(runtime, gh2::kSlus21447);
        // Off, neither is installed: the score and rock meters stay where
        // retail has them.
        if (gh2::settings::get(gh2::settings::kNoteStreak))
        {
            gh2::installNoteStreak(runtime, gh2::kSlus21447);
            gh2::installStreakCounter(runtime, gh2::kSlus21447);
        }
        if (gh2::settings::get(gh2::settings::kSongProgress))
            gh2::installSongProgress(runtime, gh2::kSlus21447);
        gh2::installFontBake();
        gh2::installNoteEngine(runtime, gh2::kSlus21447);
        // Over the note engine's: a custom chart's kinds answer before GH1's
        // marking, its taps and opens before GH1's hammer-on rules, and the
        // frets held for its sustains are left out of what either engine sees.
        gh2::guitar::installGemKinds(runtime, gh2::kSlus21447);
        gh2::guitar::installHammerOns(runtime, gh2::kSlus21447);
        // Over hammer_ons' CanHopo.
        gh2::guitar::installOpenChordHits(runtime, gh2::kSlus21447);
        // Over hammer_ons' FretButtonUp.
        gh2::guitar::installSustainHold(runtime, gh2::kSlus21447);
        gh2::guitar::installSustainFlames(runtime, gh2::kSlus21447);
        // Its strums seen before either engine's Swing, its misses and
        // passes answered before theirs.
        gh2::guitar::installLaneHits(runtime, gh2::kSlus21447);
        gh2::guitar::installLaneBands(runtime, gh2::kSlus21447);
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
        gh2::list_scroll::install(runtime, gh2::kSlus21447);
        gh2::transitions::install(runtime, gh2::kSlus21447);
        gh2::press_size::install(runtime, gh2::kSlus21447);
        gh2::bin_reads::install(runtime, gh2::kSlus21447);
        gh2::text_fit::install(gh2::kSlus21447);
        gh2::installFrameStep(runtime, gh2::kSlus21447);
        gh2::installMenus(runtime);
        gh2::installPhotoMode(runtime, gh2::kSlus21447);
        gh2::installHelpBar(runtime, gh2::kSlus21447);
        gh2::menu_guitarist::install(runtime, gh2::kSlus21447);
        gh2::installPlayers(runtime);
        gh2::installControllers(runtime);
        // After the help bar, which recolours its scene's own pixels.
        gh2::installGh2xAssets(runtime, gh2::kSlus21447);
        gh2::prebakeFonts();
        // After the 360's scenes, the streak counter and the font bake: its
        // highway starts from theirs, and is served over the 360's.
        gh2::installGh1Highway();
        // After the 360's menu art, whose speed poster it puts the amp over.
        gh2::campaign_case::install();
        gh2::hub_scene::install();
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
