"""Which disc images a run from this tree mounts.

game/ holds them: the GH2 disc the build is of, and beside it one disc of
each game that adds content (config/executables.toml's releases with role
"content"). Folders in game/ hold the other releases of those games, which
no run mounts unless it names one; releases() lists them all.
"""
import argparse
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERIAL = "SLUS-21447"


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


def content_games():
    """The games whose discs come along when their images are in game/, in
    the order their archives are searched: Rocks the 80s, GH1, then 360 GH2,
    then any other config/executables.toml gives content releases."""
    with open(ROOT / "config" / "executables.toml", "rb") as f:
        releases = tomllib.load(f)["release"]
    games = list(dict.fromkeys(r["game"] for r in releases if r.get("role") == "content"))
    order = ["gh80s", "gh1", "gh2x"]
    return sorted(games, key=lambda g: order.index(g) if g in order else len(order))


def discs_arg(value):
    """--discs gh2,gh1: the content discs to mount; gh2, the game disc, is
    always there."""
    games = [g for g in value.split(",") if g and g != "gh2"]
    unknown = [g for g in games if g not in content_games()]
    if unknown:
        raise argparse.ArgumentTypeError(f"unknown {', '.join(unknown)}; games are gh2, {', '.join(content_games())}")
    return games


def releases(env):
    """Every content disc image under game/, as (game, image): game/ itself
    holds the set a run mounts, folders in it the other releases of each."""
    found = []
    for image in sorted(p for p in discs_dir().rglob("*") if p.suffix.lower() in (".iso", ".bin", ".chd")):
        for game in content_games():
            if not subprocess.run([sys.executable, ROOT / "tools" / "disc.py", "find", game, str(image)], env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
                found.append((game, image))
    return found


def content(env, games=None, release=None):
    """GHClassic's --content for each content game's disc in game/, any of
    its releases (or only those of `games`), and --mods for mods/ when it
    exists. release, a (game, image) of releases(), is that game's disc."""
    images = sorted(str(p) for p in discs_dir().glob("*"))
    args = []
    for game in content_games():
        if games is not None and game not in games:
            continue
        if release and release[0] == game:
            args += ["--content", str(release[1])]
            continue
        result = subprocess.run([sys.executable, ROOT / "tools" / "disc.py", "find", game, *images],
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        if not result.returncode:
            args += ["--content", result.stdout.strip()]
    # The same as game/: this tree's own, else the main checkout's.
    mods = ROOT / "mods" if (ROOT / "mods").is_dir() else main_checkout() / "mods"
    if mods.is_dir():
        args += ["--mods", str(mods)]
    return args
