#!/usr/bin/env python3
"""Build GH Classic from a disc, play it, and run scenarios, on macOS, Linux and Windows.

    tools/ghc.py play [--no-build] [disc] [GHClassic options, e.g. --res native]
    tools/ghc.py build [disc] [--skip-recomp] [--no-lto]
    tools/ghc.py bind [keyboard | <n>]       set up a controller; with no device, list them
    tools/ghc.py arm [out] [--scenario <file>] [--speed <x>] [--secs 30] [--disc <image>] [--shot-every 60]
    tools/ghc.py scenarios [-j 4] [--speed 2] [name ...]
    tools/ghc.py release [--skip-build] [disc]  this platform's player package, into build/release
    tools/ghc.py ci                          what builds without a disc, into build/ci

play builds everything from the disc on its first run, then brings the C++
build up to date before each launch (not the recompile) unless --no-build.
The disc defaults to the GH2 image in game/, which is only ever read;
everything made lands in build/, which is safe to delete. build and release
also take the disc's boot executable alone (SLUS_214.47), which is all the
recompile reads.

From the system: Python 3.11+, git, and a C++ compiler (Xcode's on macOS, gcc
or clang on Linux, nothing on Windows, where llvm-mingw is fetched into
build/). CMake, Ninja and Pillow come from build/venv, made from
requirements.txt; CMake fetches the game's libraries at pinned versions.

arm runs the game once, hidden and muted, with --fast-boot --seed 1, no
save and default settings: a first boot. out (default
runs/<scenario's name>) gets run.log, frames/ (frame_0012.30s.png, seconds
since launch), shots/ and shots.png from the scenario's (shot name) steps,
and on macOS stack.txt, a 3 s sample taken if it is still alive at --secs.
scenarios runs the files in scenarios/ through arm side by side: a run
passes when it reaches its last step and quits with no FAIL or STALL in its
log.

release packages a build for players, who bring their own disc: a zip of
the game beside PUT_DISC_HERE on Windows and Linux, a disk image on macOS.
It only writes build/release, named for the tag it is built at (v0.5);
.github/workflows/release.yml runs it on each platform when a tag is pushed.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import venv
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

if sys.version_info < (3, 11):
    sys.exit(f"GH Classic needs Python 3.11 or newer, not {sys.version.split()[0]}")

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
VENV = BUILD / "venv"
TOOLS = BUILD / "tools"
RECOMP = BUILD / "recomp"
GAME = BUILD / "game"
ELF = RECOMP / "gh2.elf"
SERIAL = "SLUS-21447"
MACOS = sys.platform == "darwin"
WINDOWS = sys.platform == "win32"
EXE = ".exe" if WINDOWS else ""
VENV_BIN = VENV / ("Scripts" if WINDOWS else "bin")
VENV_PYTHON = VENV_BIN / f"python{EXE}"

LLVM_MINGW = "llvm-mingw-20260922-ucrt-x86_64"
LLVM_MINGW_URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/{LLVM_MINGW}.zip"
LLVM_MINGW_SHA256 = "e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666"


def game_binary():
    if MACOS:
        return GAME / "GHClassic.app" / "Contents" / "MacOS" / "GHClassic"
    return GAME / f"GHClassic{EXE}"


def ensure_venv():
    """build/venv with requirements.txt installed, redone when the file changes."""
    wanted = (ROOT / "requirements.txt").read_text()
    stamp = VENV / "requirements.txt"
    if VENV_PYTHON.exists() and stamp.exists() and stamp.read_text() == wanted:
        return
    print("setting up build/venv", file=sys.stderr)
    venv.create(VENV, with_pip=True, clear=True)
    subprocess.run([VENV_PYTHON, "-m", "pip", "install", "--quiet", "--disable-pip-version-check",
                    "-r", ROOT / "requirements.txt"], check=True)
    stamp.write_text(wanted)


def ensure_llvm_mingw():
    """llvm-mingw, unpacked into build/toolchain the first time."""
    home = BUILD / "toolchain" / LLVM_MINGW
    if (home / "bin" / "clang.exe").exists():
        return home
    archive = BUILD / "toolchain" / f"{LLVM_MINGW}.zip"
    archive.parent.mkdir(parents=True, exist_ok=True)
    print(f"downloading {LLVM_MINGW}", file=sys.stderr)
    urllib.request.urlretrieve(LLVM_MINGW_URL, archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != LLVM_MINGW_SHA256:
        archive.unlink()
        sys.exit(f"{archive.name}: sha256 {digest}, expected {LLVM_MINGW_SHA256}")
    with zipfile.ZipFile(archive) as zf:
        zf.extractall(archive.parent)
    archive.unlink()
    return home


def tool_env():
    """The environment every build step runs in: the venv's CMake and Ninja
    first on PATH, Ninja as CMake's generator, and on Windows llvm-mingw as
    the compiler. disc.py's own libchdr build inherits it too."""
    # A new worktree has lib/'s submodules empty, and every step needs them.
    if (ROOT / ".git").exists() and not all((ROOT / "lib" / m / "CMakeLists.txt").exists()
                                            for m in ("PS2Recomp", "libchdr")):
        print("fetching lib/'s submodules", file=sys.stderr)
        subprocess.run(["git", "submodule", "update", "--init", "--recursive"], cwd=ROOT, check=True)
    env = dict(os.environ)
    path = [str(VENV_BIN)]
    if WINDOWS:
        mingw = ensure_llvm_mingw()
        path.append(str(mingw / "bin"))
        env["CC"] = "x86_64-w64-mingw32-clang"
        env["CXX"] = "x86_64-w64-mingw32-clang++"
    env["PATH"] = os.pathsep.join(path + [env.get("PATH", "")])
    env["CMAKE_GENERATOR"] = "Ninja"
    return env


def run(cmd, env, log=None, quiet=False):
    """cmd, stopping the driver if it fails. With log, its output goes there
    and the tail is shown on failure; quiet hides stdout only."""
    cmd = [str(c) for c in cmd]
    # Windows looks a bare name up on this process's PATH, not env's.
    cmd[0] = shutil.which(cmd[0], path=env["PATH"]) or cmd[0]
    if log:
        with open(log, "w") as out:
            result = subprocess.run(cmd, env=env, stdout=out, stderr=subprocess.STDOUT)
        if result.returncode:
            sys.stderr.writelines(Path(log).read_text(errors="replace").splitlines(True)[-20:])
    else:
        result = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL if quiet else None)
    if result.returncode:
        sys.exit(f"failed ({result.returncode}): {' '.join(cmd)}")


def main_checkout():
    """The repo's own checkout, which the worktrees in .worktrees/ sit inside."""
    # A tree copied out of git (a test box's) has no checkout: it is its own.
    common = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout.strip()
    return Path(common).parent if common else ROOT


def discs_dir():
    """game/, the disc images: this tree's own, else the main checkout's, so
    every worktree plays from the one set."""
    own = ROOT / "game"
    return own if own.is_dir() else main_checkout() / "game"


def find_disc(env):
    """The GH2 image in game/, a .chd before any other. Builds libchdr on
    first use, so callers that go parallel call this first."""
    images = sorted((str(p) for p in discs_dir().glob("*")), key=lambda p: (not p.lower().endswith(".chd"), p))
    result = subprocess.run([sys.executable, ROOT / "tools" / "disc.py", "find", SERIAL, *images],
                            env=env, stdout=subprocess.PIPE, text=True)
    if result.returncode:
        sys.exit(f"no {SERIAL} disc image in game/; pass one")
    return result.stdout.strip()


def built():
    return ELF.exists() and game_binary().exists()


def build_tools(env, jobs):
    """The analyzer and recompiler, a top-level build of lib/PS2Recomp."""
    run(["cmake", "-S", ROOT / "lib" / "PS2Recomp", "-B", TOOLS, "-DCMAKE_BUILD_TYPE=Release",
         "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF"], env, quiet=True)
    run(["cmake", "--build", TOOLS, "--target", "ps2_analyzer", "ps2_recomp", "-j", jobs], env)


def tree_version():
    """The tag this tree is, or how git describes it from the last one."""
    return subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], cwd=ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, text=True).stdout.strip() or "unknown"


def experimental(tag):
    """A tag with -exp (v0.6-exp.1) is the experimental track's: every game in
    one, named and installed apart from the stable one."""
    return "-exp" in tag


def build(disc=None, recomp=True, lto=True):
    env = tool_env()
    jobs = str(os.cpu_count() or 4)
    if recomp:
        disc = disc or find_disc(env)
        shutil.rmtree(RECOMP, ignore_errors=True)
        RECOMP.mkdir(parents=True)
        run([sys.executable, ROOT / "tools" / "disc.py", "boot-elf", disc, RECOMP / "retail.elf"], env)
        run([sys.executable, ROOT / "tools" / "symbolize.py", RECOMP / "retail.elf",
             ROOT / "config" / "gh2-retail.symbols", ELF], env)

        build_tools(env, jobs)

        # The analyzer writes the recompiler config, and points the generated
        # code at output/ beside it. Both are verbose, so their output goes to logs.
        toml = RECOMP / "gh2.toml"
        run([TOOLS / "ps2xAnalyzer" / f"ps2_analyzer{EXE}", ELF, toml], env, log=RECOMP / "analyzer.log")

        # The denylist goes into [general], the first table in the analyzer's output.
        deny = [line for line in (ROOT / "config" / "stub-denylist.txt").read_text().splitlines()
                if line and not line.startswith("#")]
        lines = toml.read_text().splitlines(True)
        if "[general]\n" not in lines:
            sys.exit("no [general] table in gh2.toml; the stub denylist was not applied")
        at = lines.index("[general]\n") + 1
        lines.insert(at, "stub_denylist = [" + ",".join(f'"{name}"' for name in deny) + "]\n")
        toml.write_text("".join(lines))

        run([TOOLS / "ps2xRecomp" / f"ps2_recomp{EXE}", toml], env, log=RECOMP / "recomp.log")
        print(f"generated {len(list((RECOMP / 'output').glob('*.cpp')))} files")

    # Configured after recompiling, so the glob in CMakeLists.txt sees the output.
    tag = tree_version()
    run(["cmake", "-S", ROOT, "-B", GAME, "-DCMAKE_BUILD_TYPE=Release",
         f"-DGHC_GENERATED_DIR={RECOMP / 'output'}", f"-DGHC_ENABLE_LTO={'ON' if lto else 'OFF'}",
         f"-DGHC_VERSION={tag}", f"-DGHC_EXPERIMENTAL={'ON' if experimental(tag) else 'OFF'}"], env, quiet=True)
    run(["cmake", "--build", GAME, "--target", "GHClassic", "-j", jobs], env)


def cmd_build(argv):
    parser = argparse.ArgumentParser(prog="ghc.py build")
    parser.add_argument("disc", nargs="?")
    parser.add_argument("--skip-recomp", action="store_true", help="rebuild from the existing generated code")
    parser.add_argument("--no-lto", action="store_true", help="skip link-time optimisation, for iterating")
    args = parser.parse_args(argv)
    build(args.disc, recomp=not args.skip_recomp, lto=not args.no_lto)
    print("run: scripts/play.sh" + (f' "{args.disc}"' if args.disc else ""))


def cmd_play(argv):
    rebuild = True
    if argv[:1] == ["--no-build"]:
        rebuild = False
        argv = argv[1:]
    disc = None
    if argv and not argv[0].startswith("-"):
        disc, argv = argv[0], argv[1:]

    if not built():
        if not rebuild:
            sys.exit("no build; run tools/ghc.py build")
        build(disc)
    elif rebuild:
        # Quiet unless it fails: a no-op when nothing changed. A relink with
        # LTO takes a minute or two, so one still going after a few seconds
        # says so rather than seem to hang. Ninja prints a step only once it
        # is done when not on a terminal, so its output cannot tell sooner.
        env = tool_env()
        log = BUILD / "play-build.log"
        with open(log, "w") as out:
            proc = subprocess.Popen([shutil.which("cmake", path=env["PATH"]) or "cmake", "--build", GAME, "--target",
                                     "GHClassic"], env=env, stdout=out, stderr=subprocess.STDOUT)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                print("building GHClassic, a minute or two", file=sys.stderr)
        if proc.wait():
            sys.stderr.writelines(log.read_text(errors="replace").splitlines(True)[-20:])
            sys.exit(f"failed ({proc.returncode}): cmake --build {GAME}")
        log.unlink()
    disc = disc or find_disc(tool_env())
    return subprocess.run([str(game_binary()), disc, *argv]).returncode


def arm(out, scenario=None, speed=None, secs=30, disc=None, shot_every=60, env=None):
    """One unattended run into out. Returns 'exited before Ns' or 'alive at Ns'."""
    from PIL import Image

    out = Path(out)
    shutil.rmtree(out, ignore_errors=True)
    (out / "frames").mkdir(parents=True)
    out = out.resolve()
    if not built():
        sys.exit("no build; run tools/ghc.py build")
    disc = str(Path(disc or find_disc(env or tool_env())).resolve())

    # No save and default settings, so every run starts from a first boot on
    # any machine and never touches the player's.
    cmd = [str(game_binary()), disc]
    if scenario:
        cmd += ["--scenario", str(Path(scenario).resolve())]
    if speed:
        cmd += ["--speed", str(speed)]
    cmd += ["--fast-boot", "--seed", "1", "--hidden", "--mute", "--save", str(out / "save.bin"),
            "--settings", str(out / "settings.ini"), "--shots", str(out / "frames"), "--shot-every", str(shot_every)]

    # Frames are named by seconds since here, on the clock their mtimes use.
    start = time.time()
    with open(out / "run.log", "wb") as log:
        proc = subprocess.Popen(cmd, cwd=out, stdout=log, stderr=subprocess.STDOUT)
    try:
        proc.wait(timeout=secs)
        state = f"exited before {secs}s"
    except subprocess.TimeoutExpired:
        if MACOS:
            subprocess.run(["sample", str(proc.pid), "3", "-file", str(out / "stack.txt")],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        state = f"alive at {secs}s"
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

    for ppm in sorted((out / "frames").glob("frame_" + "[0-9]" * 6 + ".ppm")):
        Image.open(ppm).save(out / "frames" / f"frame_{ppm.stat().st_mtime - start:07.2f}s.png")
        ppm.unlink()
    # A scenario's (shot name) steps, into shots/, and all of them on one
    # sheet in rows of six, in the order taken.
    shots = []
    for ppm in sorted((out / "frames").glob("shot_*.ppm"), key=lambda p: p.stat().st_mtime):
        (out / "shots").mkdir(exist_ok=True)
        png = out / "shots" / (ppm.stem.removeprefix("shot_") + ".png")
        Image.open(ppm).save(png)
        ppm.unlink()
        shots.append(png)
    if shots:
        cell_w, cell_h, cols = 324, 228, 6
        sheet = Image.new("RGB", (cell_w * min(cols, len(shots)), cell_h * -(-len(shots) // cols)))
        for i, png in enumerate(shots):
            shot = Image.open(png).convert("RGB")
            shot.thumbnail((320, 224))
            x = (i % cols) * cell_w + (cell_w - shot.width) // 2
            y = (i // cols) * cell_h + (cell_h - shot.height) // 2
            sheet.paste(shot, (x, y))
        sheet.save(out / "shots.png")
    return state


def cmd_arm(argv):
    parser = argparse.ArgumentParser(prog="ghc.py arm")
    parser.add_argument("out", nargs="?")
    parser.add_argument("--scenario")
    parser.add_argument("--speed", type=float)
    parser.add_argument("--secs", type=int, default=30, help="real seconds before it is stopped")
    parser.add_argument("--disc")
    parser.add_argument("--shot-every", type=int, default=60)
    args = parser.parse_args(argv)
    name = Path(args.scenario).stem if args.scenario else "run"
    out = Path(args.out) if args.out else ROOT / "runs" / name
    state = arm(out, args.scenario, args.speed, args.secs, args.disc, args.shot_every)
    frames = len(list((out / "frames").iterdir()))
    print(f"{state}, {frames} frames, log {out.resolve() / 'run.log'}")
    stack = out / "stack.txt"
    if stack.exists():
        # Samples per recompiled function, callers counting their callees' time.
        counts = {}
        for match in re.finditer(r"(\d+) ([A-Za-z_]\w*_0x[0-9a-f]+)", stack.read_text(errors="replace")):
            counts[match[2]] = counts.get(match[2], 0) + int(match[1])
        print("game functions by samples:")
        for fn, n in sorted(counts.items(), key=lambda kv: -kv[1])[:8]:
            print(f"{n:7d}  {fn}")


def cmd_scenarios(argv):
    parser = argparse.ArgumentParser(prog="ghc.py scenarios")
    parser.add_argument("-j", type=int, default=4, dest="jobs")
    # Song clock checks hold to about 4x on an M4 Pro and 3x on a Ryzen 7800X3D,
    # bound by the game thread; 2x holds on both, four at once.
    parser.add_argument("--speed", type=float, default=2)
    parser.add_argument("names", nargs="*", help="files in scenarios/ without .dta; none means all")
    args = parser.parse_args(argv)
    # Not dotfiles: pathlib's glob matches them, and a tar made on macOS
    # brings a ._ copy of every file.
    names = args.names or sorted(p.stem for p in (ROOT / "scenarios").glob("*.dta") if not p.name.startswith("."))
    env = tool_env()
    disc = find_disc(env)

    def one(name):
        out = ROOT / "runs" / name
        started = time.time()
        state = arm(out, ROOT / "scenarios" / f"{name}.dta", args.speed, 600, disc, env=env)
        log = (out / "run.log").read_text(errors="replace")
        # Anywhere in a line: the watchdog's STALL lands mid-line in the game thread's output.
        failures = [m.group(1) for m in re.finditer(r"\[scenario\] ((?:FAIL|STALL).*)", log)]
        # A game that dies mid-run exits too, with its steps unfinished.
        if not failures and not re.search(r"^\[scenario\] (done|\S+ s: \(quit\))", log, re.M):
            failures = ["ended before its last step"]
        verdict = "pass" if state.startswith("exited") and not failures else "FAIL"
        return f"{verdict:<4} {name:<16} {round(time.time() - started):4d}s  {failures[0] if failures else ''}".rstrip()

    (ROOT / "runs").mkdir(exist_ok=True)
    lines = []
    started = time.time()
    with ThreadPoolExecutor(args.jobs) as pool:
        for future in as_completed([pool.submit(one, name) for name in names]):
            lines.append(future.result())
            print(lines[-1], flush=True)
    print(f"{len(lines)} scenarios in {round(time.time() - started)}s, {args.jobs} at a time")
    (ROOT / "runs" / "scenarios.txt").write_text("\n".join(lines) + "\n")
    return 1 if any(line.startswith("FAIL") for line in lines) else 0


def cmd_ci(argv):
    """Everything that builds without a disc, with the toolchain a player's
    build uses: the recompiler tools, then the runtime and every library the
    game links, in build/ci. The game itself needs the generated code."""
    argparse.ArgumentParser(prog="ghc.py ci").parse_args(argv)
    env = tool_env()
    jobs = str(os.cpu_count() or 4)
    build_tools(env, jobs)
    ci = BUILD / "ci"
    run(["cmake", "-S", ROOT, "-B", ci, "-DCMAKE_BUILD_TYPE=Release", f"-DGHC_GENERATED_DIR={ci / 'none'}"],
        env, quiet=True)
    run(["cmake", "--build", ci, "-j", jobs, "--target", "ps2_runtime", "chdr-static", "SDL3-static", "volk",
         "vk-bootstrap", "glslang-standalone", "mpeg2", "save_test", "releases_test"], env)
    run([ci / f"save_test{EXE}"], env)
    run([ci / f"releases_test{EXE}"], env)


def cmd_bind(argv):
    if not built():
        sys.exit("no build; run tools/ghc.py build")
    return subprocess.run([str(game_binary()), "--bind", *argv]).returncode


PLACEHOLDER = "Put your Guitar Hero II (USA) disc image here (.iso, .chd or .bin).txt"
SOURCE_URL = "https://github.com/noahbaxter/GHClassic"


def notices():
    """Each library built into this platform's game, and its license file.
    glslang only compiles shaders at build time; lzma and miniz are public
    domain; GCC's and LLVM's runtimes waive notices for compiled programs."""
    deps = GAME / "_deps"
    found = [
        ("SDL", deps / "sdl3-src" / "LICENSE.txt"),
        ("volk", deps / "volk-src" / "LICENSE.md"),
        ("vk-bootstrap", deps / "vk-bootstrap-src" / "LICENSE.txt"),
        ("Vulkan Memory Allocator", deps / "vulkanmemoryallocator-src" / "LICENSE.txt"),
        ("Vulkan-Headers", deps / "vulkanheaders-src" / "LICENSE.md"),
        ("raylib", deps / "raylib-src" / "LICENSE"),
        ("GLFW", deps / "raylib-src" / "src" / "external" / "glfw" / "LICENSE.md"),
        ("libchdr", ROOT / "lib" / "libchdr" / "LICENSE.txt"),
        ("libmpeg2", deps / "libmpeg2-src" / "COPYING"),
        ("zstd", ROOT / "config" / "licenses" / "zstd.txt"),
        ("PS2Recomp", ROOT / "lib" / "PS2Recomp" / "LICENSE"),
    ]
    if MACOS:
        found += [("MoltenVK", deps / "moltenvk-src" / "LICENSE"),
                  ("sse2neon", deps / "sse2neon-src" / "LICENSE")]
    if WINDOWS:
        runtime = BUILD / "toolchain" / LLVM_MINGW / "x86_64-w64-mingw32" / "share" / "mingw32"
        found += [("mingw-w64 runtime", runtime / "COPYING.MinGW-w64-runtime.txt"),
                  ("winpthreads", runtime / "COPYING.winpthreads.txt")]
    return found


def write_licenses(dest, version):
    """LICENSES.txt into dest: GH Classic's own license, then each bundled
    library's."""
    parts = [f"GH Classic {version}. Source: {SOURCE_URL}\n"
             "GH Classic is GPL-3.0; the libraries in it are under their own licenses.\n"]
    for name, path in [("GH Classic", ROOT / "LICENSE"), *notices()]:
        if not path.is_file():
            sys.exit(f"no license file for {name}: {path}")
        text = path.read_text(encoding="utf-8", errors="replace").strip()
        parts.append(f"\n{'=' * 78}\n{name}\n{'=' * 78}\n\n{text}\n")
    # UTF-8 everywhere: Windows' default is cp1252, which some licenses' names
    # do not fit.
    (dest / "LICENSES.txt").write_text("".join(parts), encoding="utf-8")


def release_portable(out, version):
    """A folder to unzip anywhere: the game, its icon and PUT_DISC_HERE, whose
    presence makes the game keep its settings and saves beside it too."""
    name = "GHClassic-experimental" if experimental(version) else "GHClassic"
    stage = out / name
    (stage / "PUT_DISC_HERE").mkdir(parents=True)
    (stage / "PUT_DISC_HERE" / PLACEHOLDER).write_text("")
    shutil.copy2(game_binary(), stage)
    shutil.copy2(GAME / "icon.png", stage)
    write_licenses(stage, version)
    archive = out / f"{name}-{'windows' if WINDOWS else 'linux'}-x64-{version}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted([stage, *stage.rglob("*")]):
            info = zipfile.ZipInfo.from_file(path, path.relative_to(out).as_posix())
            if path.is_dir():
                zf.writestr(info, "")
            else:
                info.compress_type = zipfile.ZIP_DEFLATED
                zf.writestr(info, path.read_bytes())
    return archive


def release_macos(out, version):
    """A disk image of the app beside an Applications link: dragged across, it
    leaves quarantine and App Translocation behind, and keeps its disc and
    saves in Application Support. Signed with GHC_SIGN_IDENTITY (else ad hoc)
    and notarized when APPLE_ID, APPLE_TEAM_ID and APPLE_APP_PASSWORD are set."""
    track = experimental(version)
    stage = out / "dmg"
    stage.mkdir()
    app = stage / ("GHClassic Experimental.app" if track else "GHClassic.app")
    shutil.copytree(GAME / "GHClassic.app", app, symlinks=True)
    write_licenses(app / "Contents" / "Resources", version)
    (stage / "Applications").symlink_to("/Applications")
    identity = os.environ.get("GHC_SIGN_IDENTITY", "-")
    sign = ["codesign", "--force", "--sign", identity]
    # The hardened runtime notarization needs only with a real identity: its
    # library validation turns away an ad hoc MoltenVK, having no team ID.
    if identity != "-":
        sign += ["--options", "runtime", "--timestamp"]
    env = dict(os.environ)
    run([*sign, app / "Contents" / "Frameworks" / "libMoltenVK.dylib"], env)
    run([*sign, app], env)
    archive = out / f"{'GHClassic-experimental' if track else 'GHClassic'}-macos-arm64-{version}.dmg"
    run(["hdiutil", "create", "-volname", "GH Classic Experimental" if track else "GH Classic", "-srcfolder", stage,
         "-ov", "-format", "UDZO", archive], env, quiet=True)
    shutil.rmtree(stage)
    if identity == "-":
        print("ad hoc signed: set GHC_SIGN_IDENTITY to a Developer ID to sign", file=sys.stderr)
        return archive
    run([*sign[:2], "--sign", identity, "--timestamp", archive], env)
    notary = [os.environ.get(k) for k in ("APPLE_ID", "APPLE_TEAM_ID", "APPLE_APP_PASSWORD")]
    if not all(notary):
        print("not notarized: set APPLE_ID, APPLE_TEAM_ID and APPLE_APP_PASSWORD", file=sys.stderr)
        return archive
    run(["xcrun", "notarytool", "submit", archive, "--apple-id", notary[0], "--team-id", notary[1],
         "--password", notary[2], "--wait"], env)
    run(["xcrun", "stapler", "staple", archive], env)
    return archive


def cmd_release(argv):
    parser = argparse.ArgumentParser(prog="ghc.py release")
    parser.add_argument("--skip-build", action="store_true", help="package the existing build")
    parser.add_argument("disc", nargs="?")
    args = parser.parse_args(argv)
    if not args.skip_build:
        build(args.disc, recomp=not (RECOMP / "output").is_dir())
    if not game_binary().exists():
        sys.exit("no build; run tools/ghc.py build")
    version = tree_version()
    out = BUILD / "release"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    archive = release_macos(out, version) if MACOS else release_portable(out, version)
    print(f"made {archive}")


COMMANDS = {"play": cmd_play, "build": cmd_build, "bind": cmd_bind, "arm": cmd_arm, "scenarios": cmd_scenarios,
            "release": cmd_release, "ci": cmd_ci}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        sys.exit(__doc__.split("\n\n")[1])
    # Everything runs inside build/venv, where Pillow is.
    ensure_venv()
    if Path(sys.prefix).resolve() != VENV.resolve():
        sys.exit(subprocess.run([str(VENV_PYTHON), __file__, *sys.argv[1:]]).returncode)
    sys.exit(COMMANDS[sys.argv[1]](sys.argv[2:]) or 0)


if __name__ == "__main__":
    main()
