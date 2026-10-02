# GH Classic

![Guitar Hero II running in GH Classic](assets/gameplay.gif)

This is an ai assisted recompilation project intended to produce a macOS, Windows and Linux port for the ps2 version of guitar hero 2, perhaps with some added bells and whistles. This is currently experimental so expect missing features and crashes!

To run this **you must provide your own disc image** for SLUS-21447 in either `.chd`, `.iso` or `.bin` format.

## Status

Nearly feature complete with all of single player working including: career, quickplay, training, the store, encores, options.

Additional features:
- Autodetects keyboard, gamepad and guitar input with the ability to remap
- Independent audio and video latency controls
- Any resolution, uncapped frame rate, MSAA and mipmaps

Still to come:
- Multiplayer (not wired up yet)
- Movies
- Some rendering effects

## Building & Running

Needs Git, Python 3.11+, and Xcode Command Line Tools on macOS. Linux also needs a C++ compiler and SDL's build headers (Fedora below; on Bazzite or SteamOS, build in a distrobox).

```sh
git clone --recurse-submodules https://github.com/noahbaxter/GHClassic.git && cd GHClassic
scripts/play.sh "/path/to/Guitar Hero II (USA).iso"   # scripts\play.cmd on Windows
```

<details><summary>Linux packages (Fedora)</summary>

```sh
sudo dnf install gcc-c++ libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel libXScrnSaver-devel libXtst-devel libXinerama-devel libxkbcommon-devel wayland-devel wayland-protocols-devel mesa-libGL-devel mesa-libEGL-devel libdrm-devel mesa-libgbm-devel libdecor-devel alsa-lib-devel pulseaudio-libs-devel pipewire-devel dbus-devel ibus-devel systemd-devel libusb1-devel
```
</details>

## Controls

| | Keyboard | Gamepad |
|---|---|---|
| Frets | `1` `2` `3` `4` `5` | L2 L1 R1 R2 Cross |
| Strum | Up, Down | D-pad up, down |
| Start | Enter | Start |
| Star power | Right Shift | Select |
| Whammy | | Left stick up |

Certain guitars with built-in profiles should auto detect: PS3, Wii (through a raphnet adapter), Xbox 360, Rock Band 4 and World Tour PC. Anything else can be mapped with `python3 tools/ghc.py bind`.
Fullscreen is `Cmd+F` on macOS, `F11` elsewhere.

## Credits

All i've done here is build a thin layer over excellent work done by the community. Please give most of the credit to these fine folk.

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) ([fork](https://github.com/noahbaxter/PS2Recomp/tree/ghrecomp))
- [MiloHax](https://github.com/hmxmilohax): decompiled function names ([milo-executable-library](https://github.com/hmxmilohax/milo-executable-library)), menu script references ([milo-script-library](https://github.com/hmxmilohax/milo-script-library)), and [gh2-calibration-fix](https://github.com/hmxmilohax/gh2-calibration-fix)
- [PCSX2](https://github.com/PCSX2/pcsx2) and [psx-spx](https://psx-spx.consoledev.net/): SPU2 reference code/reverb coefficients
- ([SteamGridDB](https://www.steamgriddb.com/grid/378598)): Temp icon

## License

GPL-3.0, see [LICENSE](LICENSE).

This is an unofficial fan project, not affiliated with or endorsed by Activision, Harmonix or RedOctane. Guitar Hero and all related names, marks and games belong to their owners. This repository does not include any game assets. You need your own copy of the game to build or play it.