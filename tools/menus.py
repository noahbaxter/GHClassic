#!/usr/bin/env python3
"""Check config/menus.toml and write it out for the game.

    tools/menus.py config/menus.toml <out.dta> <out.inc>

CMake runs this when the build configures. The .dta is script variables,
read by src/ui/dta/shared/; the .inc is C++ tables, read by the host where
the rule is the engine's to apply (ui/transitions.cpp). The menus' rules are
kept in the TOML, the code that applies them there, and a screen in its own
file. Anything the TOML gets wrong (a key missing or unknown, a fret that is
not one) stops the build with where it is.
"""

import re
import sys
import tomllib

SLOTS = {"fret1", "fret2", "fret3", "fret4", "fret5", "select", "start"}
NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_.]*$")
PAD = re.compile(r"^kPad_[A-Za-z0-9]+$")
SOUNDS = {
    "select": str, "back": str, "toggle": str, "start": str,
    "strum": list, "start_screens": list, "start_buttons": list, "quiet_scroll": list,
}


class Bad(Exception):
    pass


def keys(table, where, wanted):
    if not isinstance(table, dict):
        raise Bad(f"{where}: a table")
    for key in table:
        if key not in wanted:
            raise Bad(f"{where}.{key}: unknown; it takes {', '.join(sorted(wanted))}")
    for key in wanted:
        if key not in table:
            raise Bad(f"{where}.{key}: missing")


def name(value, where, pattern=NAME):
    if not isinstance(value, str) or not pattern.match(value):
        raise Bad(f"{where}: {value!r} is not a name the scripts can read")
    return value


def frets(table):
    if not isinstance(table, dict) or not table:
        raise Bad("frets: a table of actions")
    rows = []
    for action, slot in table.items():
        where = f"frets.{action}"
        name(action, where)
        if slot not in SLOTS:
            raise Bad(f"{where}: {slot!r}; one of {', '.join(sorted(SLOTS))}")
        rows.append(f"({action} {slot})")
    return [f"{{set $ghc_frets ({' '.join(rows)})}}"]


def sounds(table):
    keys(table, "sounds", set(SOUNDS))
    lines = []
    for key, kind in SOUNDS.items():
        where = f"sounds.{key}"
        value = table[key]
        if not isinstance(value, kind):
            raise Bad(f"{where}: {'a list' if kind is list else 'a name'}")
        if kind is str:
            lines.append(f"{{set $ghc_sound_{key} {name(value, where)}}}")
        else:
            pattern = PAD if key == "strum" else NAME
            items = [name(v, f"{where}[{i}]", pattern) for i, v in enumerate(value)]
            lines.append(f"{{set $ghc_sound_{key} ({' '.join(items)})}}")
    return lines


def names(value, where, pattern=NAME):
    if not isinstance(value, list):
        raise Bad(f"{where}: a list")
    return [name(v, f"{where}[{i}]", pattern) for i, v in enumerate(value)]


def transitions(table):
    keys(table, "transitions", {"still", "same_room"})
    still = names(table["still"], "transitions.still")
    pairs = table["same_room"]
    if not isinstance(pairs, list):
        raise Bad("transitions.same_room: a list of pairs")
    for i, pair in enumerate(pairs):
        if len(names(pair, f"transitions.same_room[{i}]")) != 2:
            raise Bad(f"transitions.same_room[{i}]: two screens")
    if len(set(still)) != len(still):
        raise Bad("transitions.still: a screen named twice")
    inc = ["// Screens the camera never turns for.",
           "constexpr const char *kStill[] = {" + ", ".join(f'"{s}"' for s in still) + "};",
           "// Pairs of screens in one room, between which it stays.",
           "constexpr const char *kSameRoom[][2] = {" + ", ".join(f'{{"{a}", "{b}"}}' for a, b in pairs) + "};"]
    return inc


def main(src, out, out_inc):
    with open(src, "rb") as f:
        data = tomllib.load(f)
    keys(data, "menus", {"frets", "sounds", "transitions"})
    made = f"Made by tools/menus.py from {src}: change that, not this."
    lines = [f"; {made}"]
    lines += frets(data["frets"])
    lines += sounds(data["sounds"])
    inc = transitions(data["transitions"])
    with open(out, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    with open(out_inc, "w", newline="\n") as f:
        f.write("\n".join([f"// {made}"] + inc) + "\n")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    try:
        main(sys.argv[1], sys.argv[2], sys.argv[3])
    except (Bad, tomllib.TOMLDecodeError) as e:
        sys.exit(f"{sys.argv[1]}: {e}")
