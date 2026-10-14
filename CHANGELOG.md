# Changelog

## v0.7

**THIS IS UNFINISHED.** Expect bugs, missing features and things changing between releases.

### Imports
- GH1, GH80s and Xbox 360 GH2 roms load and play inside PS2 GH2 as their own campaigns and quickplay setlists; includes all original UIs, songs, career, store, venues, characters and guitars
- the primary game still requires a base SLUS-21447 GH2 rom, but additional content roms can be added from any region at any time and just bring their content with them
- All campaigns default to their authentic content. Quickplay allows the mixing and matching of characters, guitars, venues, and songs

### Xbox 360 graphics
- **REQUIRES** an xbox 360 rom of GH2!
- Characters, guitar and venue models, textures and art
- Authentic Xbox 360 rendering engine: per pixel lighting, specular and reflections, bloom, depth of field and character shadows

### New
- Photo mode: view all the venue's cameras, plus a noclip style free camera (WASD+mouse movement, Q+E roll, shift to go faster)
- Note speed per difficulty (replaces the hyperspeed cheat) and highway length, previewed live on any paused song
- Lefty flip makes your guitarist left-handed
- Graphics options: render scale (50-200%), MSAA, TAA, forcing high LOD models and a perf overlay

### Performance and latency
- Smoother frame pacing and no more shader stutter mid-song (thanks FelipeLuis)
- Dropped input latency by roughly 40 ms on Windows, 13 ms on Linux and 30 ms on Mac

## v0.6

- PS2 rendering parity: depth of field, spotlight beam smoke, fogged projections, flares, highway fading into the venue, hit glow on the track
- PS2's object culling as a setting (off by default)
- Holding the whammy bar down bends every sustain like in gh3 onward
- Letting go just before a sustain ends no longer cuts your guitar
- Echo and delay sound effects are now accurate
- Main menu lick fades out when you leave the menu
- Window opens widescreen when widescreen is on
- Version number on the main menu, and a prompt at launch when a newer release is out
- Linux builds run on more distros

## v0.5 and earlier

- Most of GH2's single player: career, quickplay, training, the store, encores, options and movies, running natively on macOS, Windows and Linux
- Native vulkan graphics renderer, audio and movie playback
- Any resolution, widescreen with the full crowd, uncapped frame rate (VSYNC is forced on), MSAA and mipmaps
- Keyboard, gamepad and guitar input, with built-in guitar profiles and remapping in `input.ini`
- Separate audio and video latency settings
- Saves in one file instead of a memory card, with PCSX2 memory card import and export
- Fast boot, a Quit button, menu music that cycles every loop, and your guitar track playing from the song's start
