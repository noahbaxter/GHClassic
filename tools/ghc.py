#!/usr/bin/env python3
"""Build GH Classic from a disc, play it, and run scenarios, on macOS, Linux and Windows.

    tools/ghc.py play [--no-build] [disc] [GHClassic options, e.g. --res native]
    tools/ghc.py build [disc] [--skip-recomp] [--no-lto]
    tools/ghc.py bind [keyboard | <n>]       set up a controller; with no device, list them
    tools/ghc.py arm [out] [--scenario <file>] [--speed <x>] [--secs 30] [--disc <image>] [--shot-every 60]
    tools/ghc.py scenarios [-j 4] [--speed 2] [name ...]

play builds everything from the disc on its first run, then brings the C++
build up to date before each launch (not the recompile) unless --no-build.
The disc defaults to the GH2 image in game/, which is only ever read;
everything made lands in build/, which is safe to delete.

From the system: Python 3.11+, git, and a C++ compiler (Xcode's on macOS, gcc
or clang on Linux, nothing on Windows, where llvm-mingw is fetched into
build/). CMake, Ninja and Pillow come from build/venv, made from
requirements.txt; CMake fetches the game's libraries at pinned versions.

arm runs the game once, hidden and muted, with --fast-boot --seed 1, on a
blank memory card and default settings: a first boot. out (default
runs/<scenario's name>) gets run.log, frames/ (frame_0012.30s.png, seconds
since launch), shots/ and shots.png from the scenario's (shot name) steps,
and on macOS stack.txt, a 3 s sample taken if it is still alive at --secs.
scenarios runs the files in scenarios/ through arm side by side: a run
passes when it quits on its own with no FAIL or STALL in its log.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
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


def find_disc(env):
    """The GH2 image in game/. Builds libchdr on first use, so callers that
    go parallel call this first."""
    images = sorted(str(p) for p in (ROOT / "game").glob("*"))
    result = subprocess.run([sys.executable, ROOT / "tools" / "disc.py", "find", SERIAL, *images],
                            env=env, stdout=subprocess.PIPE, text=True)
    if result.returncode:
        sys.exit(f"no {SERIAL} disc image in game/; pass one")
    return result.stdout.strip()


def built():
    return ELF.exists() and game_binary().exists()


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

        run(["cmake", "-S", ROOT / "lib" / "PS2Recomp", "-B", TOOLS, "-DCMAKE_BUILD_TYPE=Release",
             "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF"], env, quiet=True)
        run(["cmake", "--build", TOOLS, "--target", "ps2_analyzer", "ps2_recomp", "-j", jobs], env)

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
    run(["cmake", "-S", ROOT, "-B", GAME, "-DCMAKE_BUILD_TYPE=Release",
         f"-DGHC_GENERATED_DIR={RECOMP / 'output'}", f"-DGHC_ENABLE_LTO={'ON' if lto else 'OFF'}"], env, quiet=True)
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
        # Quiet unless it fails: a no-op when nothing changed.
        log = BUILD / "play-build.log"
        run(["cmake", "--build", GAME, "--target", "GHClassic"], tool_env(), log=log)
        log.unlink()
    disc = disc or find_disc(tool_env())
    return subprocess.run([str(game_binary()), str(ELF), disc, *argv]).returncode


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

    # A blank card and default settings, so every run starts from a first
    # boot on any machine and never touches the player's.
    card = Path(tempfile.mkdtemp(prefix="arm-mc."))

    cmd = [str(game_binary()), str(ELF), disc]
    if scenario:
        cmd += ["--scenario", str(Path(scenario).resolve())]
    if speed:
        cmd += ["--speed", str(speed)]
    cmd += ["--fast-boot", "--seed", "1", "--hidden", "--mute", "--mc", str(card),
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
        shutil.rmtree(card, ignore_errors=True)
        for slot in card.parent.glob(card.name + "_slot*"):
            shutil.rmtree(slot, ignore_errors=True)

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
    names = args.names or sorted(p.stem for p in (ROOT / "scenarios").glob("*.dta"))
    env = tool_env()
    disc = find_disc(env)

    def one(name):
        out = ROOT / "runs" / name
        started = time.time()
        state = arm(out, ROOT / "scenarios" / f"{name}.dta", args.speed, 600, disc, env=env)
        log = (out / "run.log").read_text(errors="replace")
        failures = [line.removeprefix("[scenario] ") for line in log.splitlines()
                    if re.match(r"\[scenario\] (FAIL|STALL)", line)]
        verdict = "pass" if state.startswith("exited") and not failures else "FAIL"
        return f"{verdict:<4} {name:<16} {round(time.time() - started):4d}s  {failures[0] if failures else ''}".rstrip()

    (ROOT / "runs").mkdir(exist_ok=True)
    lines = []
    with ThreadPoolExecutor(args.jobs) as pool:
        for future in as_completed([pool.submit(one, name) for name in names]):
            lines.append(future.result())
            print(lines[-1], flush=True)
    (ROOT / "runs" / "scenarios.txt").write_text("\n".join(lines) + "\n")
    return 1 if any(line.startswith("FAIL") for line in lines) else 0


def cmd_bind(argv):
    if not built():
        sys.exit("no build; run tools/ghc.py build")
    return subprocess.run([str(game_binary()), "--bind", *argv]).returncode


COMMANDS = {"play": cmd_play, "build": cmd_build, "bind": cmd_bind, "arm": cmd_arm, "scenarios": cmd_scenarios}


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
