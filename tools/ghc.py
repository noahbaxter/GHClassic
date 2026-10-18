#!/usr/bin/env python3
"""Build GH Classic from a disc, play it, and run scenarios, on macOS, Linux and Windows.

    tools/ghc.py play [--no-build] [--isolated] [disc] [GHClassic options, e.g. --res native]
    tools/ghc.py build [disc] [--skip-recomp] [--lto]
    tools/ghc.py bind [--isolated] [keyboard | <n>]   set up a controller; with no device, list them
    tools/ghc.py arm [out] [--scenario <file>] [--speed <x>] [--secs 30] [--disc <image>] [--shot-every 60]
                     [--res 1080p] [--set <name>=<value> ...]
    tools/ghc.py record <scenario> [--speed 2.5] [--res 1080p] [--secs 900] [--set <name>=<value> ...]
    tools/ghc.py scenarios [-j 4] [--speed 2] [--review] [--discs gh2,gh1] [--releases] [name ...]
    tools/ghc.py release [--skip-build] [disc]  this platform's player package, into build/release
    tools/ghc.py ci                          what builds without a disc, into build/ci

play builds everything from the disc on its first run, then brings the C++
build up to date before each launch (not the recompile) unless --no-build.
play and bind keep saves, settings and bindings where an installed stable
build does, whichever tree or track this is; --isolated keeps them in this
tree's build/data instead, apart from everything.
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
runs/<date>_<time>_<scenario's name>, so runs sort oldest first and none
overwrites another) gets run.log, frames/ (frame_0012.30s.png, seconds
since launch), shots/ from the scenario's (shot name) steps, and on macOS
stack.txt, a 3 s sample taken if it is still alive at --secs. The shots on
one sheet go to the main checkout's review/ as <date>_<time>_<name>.png,
where everything to look at goes. scenarios runs the files in scenarios/
through arm side by side, the sheets staying in each run as shots.png unless
--review: a run passes when it reaches its last step and quits with no FAIL
or STALL in its log. Both clear runs/ of runs older than three days.

record runs a scenario as arm does and makes it videos, picture and sound,
in review/ as <date>_<time>_<scenario's name>.mp4: the game pipes each frame
to ffmpeg as it draws and writes its mixed sound beside it, both on the
game's clock, so it can run faster than real time. Without (record) steps the
whole run is one video; with them, each (record on name) to (record off) is
its own, <scenario's name>_<name>.mp4. A run stopped at --secs keeps what it
drew.

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
import signal
import subprocess
import sys
import time
import urllib.request
import venv
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from releases import content, discs_arg, find_disc, main_checkout, releases

if sys.version_info < (3, 11):
    sys.exit(f"GH Classic needs Python 3.11 or newer, not {sys.version.split()[0]}")

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
VENV = BUILD / "venv"
TOOLS = BUILD / "tools"
RECOMP = BUILD / "recomp"
GAME = BUILD / "game"
ELF = RECOMP / "gh2.elf"
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


LOGS = BUILD / "logs"
TTY = sys.stderr.isatty()


def colour(code, text):
    return f"\033[{code}m{text}\033[0m" if TTY else text


def theirs(warning):
    """Whether a warning is third-party code's (a fetched dependency's, or
    libchdr's), which we report but do not fix here."""
    return "_deps/" in warning or "/lib/libchdr/" in warning


def build_step(title, cmd, env, log):
    """One build step, its whole output into build/logs/<log>. On a terminal
    one line counts it along by Ninja's [n/m]. Done, it says how long it took
    and lists any warning in our own code, which should be fixed, beside a
    count of third-party code's; failed, it shows the log's end and stops."""
    cmd = [str(c) for c in cmd]
    # Windows looks a bare name up on this process's PATH, not env's.
    cmd[0] = shutil.which(cmd[0], path=env["PATH"]) or cmd[0]
    LOGS.mkdir(parents=True, exist_ok=True)
    path = LOGS / log
    started = time.time()
    warnings, steps, idle = {}, None, False
    if TTY:
        sys.stderr.write(f"  {title}\033[K")
        sys.stderr.flush()
    with open(path, "w") as out:
        proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                errors="replace")
        for line in proc.stdout:
            out.write(line)
            # A header's warning comes once for each file including it.
            if "warning:" in line or line.startswith("CMake Warning"):
                warnings.setdefault(line.strip(), None)
            idle = idle or line.startswith("ninja: no work to do")
            if step := re.match(r"\[(\d+)/(\d+)\]", line):
                steps = step[2]
                if TTY:
                    sys.stderr.write(f"\r  {title}  {step[1]}/{step[2]}\033[K")
                    sys.stderr.flush()
    code = proc.wait()
    if TTY:
        sys.stderr.write("\r\033[K")
    if code:
        sys.stderr.writelines(path.read_text(errors="replace").splitlines(True)[-30:])
        sys.exit(colour(31, f"{title} failed ({code}); the whole log is {path}"))
    if idle and not warnings:
        return path
    ours = [w for w in warnings if not theirs(w)]
    note = f"{round(time.time() - started)}s" + (f", {steps} step{'s' * (steps != '1')}" if steps else "")
    if ours:
        note += ", " + colour(33, f"{len(ours)} warning{'s' * (len(ours) != 1)} in our code")
    if len(warnings) > len(ours):
        note += f", {len(warnings) - len(ours)} in third-party code"
    print(f"  {title}  {note}" + (f"  ({path.relative_to(ROOT)})" if warnings else ""), file=sys.stderr)
    for warning in ours[:8]:
        print("    " + colour(33, warning), file=sys.stderr)
    return path


def built():
    return ELF.exists() and game_binary().exists()


def build_tools(env, jobs):
    """The analyzer and recompiler, a top-level build of lib/PS2Recomp."""
    configure = ["cmake", "-S", ROOT / "lib" / "PS2Recomp", "-B", TOOLS, "-DCMAKE_BUILD_TYPE=Release",
                 "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF"]
    build_step("configuring recompiler", configure, env, "tools-configure.log")
    targets = ["--target", "ps2_analyzer", "ps2_recomp", "-j", jobs]
    build_step("building recompiler", ["cmake", "--build", TOOLS, *targets], env, "tools.log")


def tree_version():
    """The tag this tree is, or how git describes it from the last one."""
    return subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], cwd=ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, text=True).stdout.strip() or "unknown"


def experimental(tag):
    """A tag with -exp (v0.6-exp.1) is the experimental track's: every game in
    one, named and installed apart from the stable one."""
    return "-exp" in tag


def build(disc=None, recomp=True, lto=False):
    env = tool_env()
    jobs = str(os.cpu_count() or 4)
    if recomp:
        disc = disc or find_disc(env)
        shutil.rmtree(RECOMP, ignore_errors=True)
        RECOMP.mkdir(parents=True)
        tools = ROOT / "tools"
        build_step("reading disc's executable",
                   [sys.executable, tools / "disc.py", "boot-elf", disc, RECOMP / "retail.elf"], env, "disc.log")
        build_step("naming its functions",
                   [sys.executable, tools / "symbolize.py", RECOMP / "retail.elf",
                    ROOT / "config" / "gh2-retail.symbols", ELF], env, "symbolize.log")

        build_tools(env, jobs)

        # The analyzer writes the recompiler config, and points the generated
        # code at output/ beside it.
        toml = RECOMP / "gh2.toml"
        build_step("analysing", [TOOLS / "ps2xAnalyzer" / f"ps2_analyzer{EXE}", ELF, toml], env, "analyzer.log")

        # The denylist goes into [general], the first table in the analyzer's output.
        deny = [line for line in (ROOT / "config" / "stub-denylist.txt").read_text().splitlines()
                if line and not line.startswith("#")]
        lines = toml.read_text().splitlines(True)
        if "[general]\n" not in lines:
            sys.exit("no [general] table in gh2.toml; the stub denylist was not applied")
        at = lines.index("[general]\n") + 1
        lines.insert(at, "stub_denylist = [" + ",".join(f'"{name}"' for name in deny) + "]\n")
        toml.write_text("".join(lines))

        build_step("recompiling", [TOOLS / "ps2xRecomp" / f"ps2_recomp{EXE}", toml], env, "recomp.log")
        print(f"  generated {len(list((RECOMP / 'output').glob('*.cpp')))} files", file=sys.stderr)

    # Configured after recompiling, so the glob in CMakeLists.txt sees the output.
    tag = tree_version()
    configure = ["cmake", "-S", ROOT, "-B", GAME, "-DCMAKE_BUILD_TYPE=Release",
                 f"-DGHC_GENERATED_DIR={RECOMP / 'output'}", f"-DGHC_ENABLE_LTO={'ON' if lto else 'OFF'}",
                 f"-DGHC_VERSION={tag}", f"-DGHC_EXPERIMENTAL={'ON' if experimental(tag) else 'OFF'}"]
    build_step("configuring game", configure, env, "game-configure.log")
    build_step("building game", ["cmake", "--build", GAME, "--target", "GHClassic", "-j", jobs], env, "game.log")


def cmd_build(argv):
    parser = argparse.ArgumentParser(prog="ghc.py build")
    parser.add_argument("disc", nargs="?")
    parser.add_argument("--skip-recomp", action="store_true", help="rebuild from the existing generated code")
    parser.add_argument("--lto", action="store_true",
                        help="link-time optimisation, as releases have; a minute and a half on every link")
    args = parser.parse_args(argv)
    build(args.disc, recomp=not args.skip_recomp, lto=args.lto)
    print("run: scripts/play.sh" + (f' "{args.disc}"' if args.disc else ""))


def stable_data():
    """Where an installed stable build keeps saves and settings: SDL's pref
    path for org "" and app GHClassic on each platform."""
    if MACOS:
        return Path.home() / "Library" / "Application Support" / "GHClassic"
    if WINDOWS:
        return Path(os.environ.get("APPDATA", Path.home())) / "GHClassic"
    return Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share")) / "GHClassic"


def show_launch(disc, args):
    """What a run from here plays from, said before the window opens: the
    build, the game disc and any content discs, mods, and where saves go,
    all read from the arguments the game is given."""
    branch = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True).stdout.strip()
    tag = tree_version()
    track = "experimental" if experimental(tag) else "stable"
    home = str(Path.home())
    print(colour(1, f"GH Classic {tag}") + f"  {track}" + (f", {branch}" if branch else ""), file=sys.stderr)
    rows = [("disc", Path(disc).name)]
    flags = dict(zip(args[::2], args[1::2]))
    rows += [("content", Path(path).name) for flag, path in zip(args, args[1:]) if flag == "--content"]
    rows.append(("mods", flags.get("--mods", "none").replace(home, "~")))
    data = flags.get("--data", "")
    rows.append(("saves", (str(stable_data()) + "  (shared)" if data == "stable" else data).replace(home, "~")))
    for label, value in rows:
        print(f"  {colour(2, f'{label:<8}')}{value}", file=sys.stderr)


def data_dir(isolated):
    """GHClassic's --data for a run from here: the stable track's own
    directory, which holds the player's saves, settings and bindings, or with
    isolated this tree's build/data."""
    return ["--data", str(BUILD / "data") if isolated else "stable"]


def cmd_play(argv):
    rebuild = True
    isolated = False
    games = None
    while argv[:1] in (["--no-build"], ["--isolated"], ["--discs"]):
        if argv[0] == "--discs":
            games = discs_arg(argv[1]) if len(argv) > 1 else sys.exit("--discs needs a list")
            argv = argv[2:]
            continue
        rebuild = rebuild and argv[0] != "--no-build"
        isolated = isolated or argv[0] == "--isolated"
        argv = argv[1:]
    disc = None
    if argv and not argv[0].startswith("-"):
        disc, argv = argv[0], argv[1:]

    if not built():
        if not rebuild:
            sys.exit("no build; run tools/ghc.py build")
        build(disc)
    elif rebuild:
        # The C++ brought up to date: a no-op when nothing changed.
        build_step("building game", ["cmake", "--build", GAME, "--target", "GHClassic"], tool_env(), "game.log")
    disc = disc or find_disc(tool_env())
    args = [*content(tool_env(), games), *data_dir(isolated)]
    show_launch(disc, args)
    return subprocess.run([str(game_binary()), disc, *args, *argv]).returncode


def review_path(what, suffix=".png"):
    """Where something to look at goes: the main checkout's review/, flat and
    named by when it was made, so it sorts by time."""
    review = main_checkout() / "review"
    review.mkdir(exist_ok=True)
    return review / f"{time.strftime('%Y-%m-%d_%H%M')}_{what}{suffix}"


def prune_runs(days=3):
    """Runs older than that: their output is the tools', rebuilt by the next run."""
    for run in (ROOT / "runs").glob("*"):
        if time.time() - run.stat().st_mtime > days * 86400:
            shutil.rmtree(run, ignore_errors=True) if run.is_dir() else run.unlink()


def arm(out, scenario=None, speed=None, secs=30, disc=None, shot_every=60, env=None, res=None, games=None,
        review=None, release=None, settings=(), perf=False, record=None):
    """One unattended run into out. Returns 'exited before Ns' or 'alive at Ns'.
    With review, its shot sheet goes to review/ under that name rather than out.
    With record, the run's clips also go in that folder (the game's --record)."""
    from PIL import Image

    out = Path(out)
    shutil.rmtree(out, ignore_errors=True)
    (out / "frames").mkdir(parents=True)
    # --set's lines, over the defaults.
    if settings:
        (out / "settings.ini").write_text("".join(f"{s.replace('=', ' = ', 1)}\n" for s in settings))
    out = out.resolve()
    if not built():
        sys.exit("no build; run tools/ghc.py build")
    disc = str(Path(disc or find_disc(env or tool_env())).resolve())

    # No save and default settings, so every run starts from a first boot on
    # any machine and never touches the player's.
    cmd = [str(game_binary()), disc, *content(env or tool_env(), games, release)]
    if scenario:
        cmd += ["--scenario", str(Path(scenario).resolve())]
    if speed:
        cmd += ["--speed", str(speed)]
    if res:
        cmd += ["--res", res]
    if perf:
        cmd += ["--profiler"]
    if record:
        Path(record).mkdir(parents=True, exist_ok=True)
        cmd += ["--record", str(Path(record).resolve())]
    cmd += ["--fast-boot", "--seed", "1", "--hidden", "--mute", "--save", str(out / "save.bin"),
            "--settings", str(out / "settings.ini"), "--shots", str(out / "frames"), "--shot-every", str(shot_every)]

    # Frames are named by seconds since here, on the clock their mtimes use.
    start = time.time()
    with open(out / "run.log", "wb") as log:
        # Recording, in a group of its own with its ffmpegs.
        proc = subprocess.Popen(cmd, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=bool(record) and not WINDOWS)
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
        if record and not WINDOWS:
            # A killed game's ffmpeg sees its pipe close and ends the clip
            # with what it was sent. Given 30 s to, then killed too.
            for _ in range(300):
                try:
                    os.killpg(proc.pid, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.1)
            else:
                os.killpg(proc.pid, signal.SIGKILL)

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
        sheet.save(review_path(review) if review else out / "shots.png")
    return state


def cmd_arm(argv):
    parser = argparse.ArgumentParser(prog="ghc.py arm")
    parser.add_argument("out", nargs="?")
    parser.add_argument("--scenario")
    parser.add_argument("--speed", type=float)
    parser.add_argument("--secs", type=int, default=30, help="real seconds before it is stopped")
    parser.add_argument("--disc")
    parser.add_argument("--shot-every", type=int, default=60)
    parser.add_argument("--res", help="the game's own --res, the size frames and shots come out at")
    parser.add_argument("--discs", type=discs_arg, help="content discs to mount, as gh2,gh1; default every one in game/")
    parser.add_argument("--set", action="append", default=[], metavar="NAME=VALUE", dest="settings",
                        help="a settings.ini line over the defaults, e.g. msaa=1; may repeat")
    parser.add_argument("--profiler", action="store_true", dest="perf",
                        help="the game's --profiler: frametimes.csv in the run, and its summary printed here")
    args = parser.parse_args(argv)
    name = Path(args.scenario).stem if args.scenario else "run"
    out = Path(args.out) if args.out else ROOT / "runs" / f"{time.strftime('%Y-%m-%d_%H%M%S')}_{name}"
    prune_runs()
    state = arm(out, args.scenario, args.speed, args.secs, args.disc, args.shot_every, res=args.res, games=args.discs,
                review=Path(args.out).name if args.out else name, settings=args.settings, perf=args.perf)
    frames = len(list((out / "frames").iterdir()))
    print(f"{state}, {frames} frames, log {out.resolve() / 'run.log'}")
    for line in (out / "run.log").read_text(errors="replace").splitlines():
        if line.startswith("[perf]"):
            print(line)
    stack = out / "stack.txt"
    if stack.exists():
        # Samples per recompiled function, callers counting their callees' time.
        counts = {}
        for match in re.finditer(r"(\d+) ([A-Za-z_]\w*_0x[0-9a-f]+)", stack.read_text(errors="replace")):
            counts[match[2]] = counts.get(match[2], 0) + int(match[1])
        print("game functions by samples:")
        for fn, n in sorted(counts.items(), key=lambda kv: -kv[1])[:8]:
            print(f"{n:7d}  {fn}")


def cmd_record(argv):
    parser = argparse.ArgumentParser(prog="ghc.py record")
    parser.add_argument("scenario")
    parser.add_argument("--speed", type=float, default=2.5,
                        help="game seconds a real one; too high and frames repeat, which the log counts")
    parser.add_argument("--res", default="1080p")
    parser.add_argument("--secs", type=int, default=900, help="real seconds before it is stopped")
    parser.add_argument("--disc")
    parser.add_argument("--discs", type=discs_arg, help="content discs to mount, as gh2,gh1; default every one in game/")
    parser.add_argument("--set", action="append", default=[], metavar="NAME=VALUE", dest="settings",
                        help="a settings.ini line over the defaults, e.g. msaa=1; may repeat")
    args = parser.parse_args(argv)
    if not shutil.which("ffmpeg"):
        sys.exit("record needs ffmpeg")
    name = Path(args.scenario).stem
    out = ROOT / "runs" / f"{time.strftime('%Y-%m-%d_%H%M%S')}_{name}"
    clips = out / "clips"
    prune_runs()
    # No periodic frames: (shot) steps still come out as a sheet.
    state = arm(out, args.scenario, args.speed, args.secs, args.disc, 1 << 30, res=args.res, games=args.discs,
                review=name, settings=args.settings, record=clips)
    print(f"{state}, log {out / 'run.log'}")
    for line in (out / "run.log").read_text(errors="replace").splitlines():
        if line.startswith("[record]") or "FAIL" in line or "STALL" in line:
            print(line)
    # Each clip's picture and sound as one .mp4, in the order they were made.
    videos = sorted(clips.glob("*.ts"), key=lambda p: p.stat().st_mtime)
    made = 0
    for video in videos:
        # A run's one unnamed clip is the scenario's name alone.
        mp4 = review_path(name if len(videos) == 1 and video.stem.isdigit() else f"{name}_{video.stem}", ".mp4")
        # Another run's in the same minute stays.
        stem, again = mp4.stem, 2
        while mp4.exists():
            mp4, again = mp4.with_stem(f"{stem}_{again}"), again + 1
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", video]
        sound = video.with_suffix(".f32")
        if sound.exists() and sound.stat().st_size:
            # At the game's kAudioRate (host/audio.h).
            cmd += ["-f", "f32le", "-ar", "48000", "-ac", "2", "-i", sound, "-c:a", "aac", "-b:a", "192k",
                    "-shortest"]
        if subprocess.run([*cmd, "-c:v", "copy", mp4]).returncode == 0:
            print(mp4)
            made += 1
    if not made:
        return 1


def cmd_scenarios(argv):
    parser = argparse.ArgumentParser(prog="ghc.py scenarios")
    parser.add_argument("-j", type=int, default=4, dest="jobs")
    # Song clock checks hold to about 4x on an M4 Pro and 3x on a Ryzen 7800X3D,
    # bound by the game thread; 2x holds on both, four at once.
    parser.add_argument("--speed", type=float, default=2)
    parser.add_argument("--review", action="store_true", help="each run's shot sheet into review/")
    parser.add_argument("--discs", type=discs_arg,
                        help="content discs to mount, as gh2,gh1; default every one in game/. A scenario that "
                             "(needs) another skips")
    parser.add_argument("--releases", action="store_true",
                        help="each content disc under game/, folders in it too, in turn as its game's: the "
                             "scenarios that (needs) that game, once per disc")
    parser.add_argument("names", nargs="*", help="files in scenarios/ without .dta; none means all")
    args = parser.parse_args(argv)
    # Not dotfiles: pathlib's glob matches them, and a tar made on macOS
    # brings a ._ copy of every file.
    names = args.names or sorted(p.stem for p in (ROOT / "scenarios").glob("*.dta") if not p.name.startswith("."))
    env = tool_env()
    disc = find_disc(env)

    def needs(name):
        found = re.search(r"\(needs ([^)]*)\)", (ROOT / "scenarios" / f"{name}.dta").read_text())
        return found[1].split() if found else []

    runs = [(name, None) for name in names]
    if args.releases:
        runs = [(name, release) for release in releases(env) for name in names if release[0] in needs(name)]

    def one(name, release):
        scenario = ROOT / "scenarios" / f"{name}.dta"
        if release:
            name = f"{name}@{release[1].stem}"
        out = ROOT / "runs" / name
        started = time.time()
        # The longest plays a career's whole first tier, five songs.
        state = arm(out, scenario, args.speed, 900, disc, env=env, games=args.discs,
                    review=name if args.review else None, release=release)
        log = (out / "run.log").read_text(errors="replace")
        # Anywhere in a line: the watchdog's STALL lands mid-line in the game thread's output.
        failures = [m.group(1) for m in re.finditer(r"\[scenario\] ((?:FAIL|STALL).*)", log)]
        skips = [line.removeprefix("[scenario] ") for line in log.splitlines()
                 if line.startswith("[scenario] SKIP")]
        # A game that dies mid-run exits too, with its steps unfinished.
        if not failures and not skips and not re.search(r"^\[scenario\] (done|\S+ s: \(quit\))", log, re.M):
            failures = ["ended before its last step"]
        verdict = "FAIL" if failures or not state.startswith("exited") else "skip" if skips else "pass"
        note = failures[0] if failures else skips[0] if skips else ""
        return f"{verdict:<4} {name:<16} {round(time.time() - started):4d}s  {note}".rstrip()

    (ROOT / "runs").mkdir(exist_ok=True)
    prune_runs()
    lines = []
    started = time.time()
    with ThreadPoolExecutor(args.jobs) as pool:
        for future in as_completed([pool.submit(one, name, release) for name, release in runs]):
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
    isolated = argv[:1] == ["--isolated"]
    argv = argv[1:] if isolated else argv
    return subprocess.run([str(game_binary()), "--bind", *argv, *data_dir(isolated)]).returncode


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
        ("libogg", deps / "ogg-src" / "COPYING"),
        ("libvorbis", deps / "vorbis-src" / "COPYING"),
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
        build(args.disc, recomp=not (RECOMP / "output").is_dir(), lto=True)
    if not game_binary().exists():
        sys.exit("no build; run tools/ghc.py build")
    version = tree_version()
    out = BUILD / "release"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    archive = release_macos(out, version) if MACOS else release_portable(out, version)
    print(f"made {archive}")


COMMANDS = {"play": cmd_play, "build": cmd_build, "bind": cmd_bind, "arm": cmd_arm, "record": cmd_record,
            "scenarios": cmd_scenarios, "release": cmd_release, "ci": cmd_ci}


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
