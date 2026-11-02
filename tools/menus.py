#!/usr/bin/env python3
"""Check config/menus.toml and write it out as the game's script variables.

    tools/menus.py config/menus.toml <out.dta>

CMake runs this when the build configures, and src/ui/dta/shared/ reads what
it writes: the menus' rules are kept in the TOML, the code that applies them
in the shared scripts, and a screen in its own file. Anything the TOML gets
wrong (a key missing or unknown, a name the scripts can't read) stops the
build with where it is.
"""

import re
import sys
import tomllib

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


def main(src, out):
    with open(src, "rb") as f:
        data = tomllib.load(f)
    keys(data, "menus", {"sounds"})
    lines = [f"; Made by tools/menus.py from {src}: change that, not this."]
    lines += sounds(data["sounds"])
    with open(out, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        main(sys.argv[1], sys.argv[2])
    except (Bad, tomllib.TOMLDecodeError) as e:
        sys.exit(f"{sys.argv[1]}: {e}")
