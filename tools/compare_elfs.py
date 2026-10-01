#!/usr/bin/env python3
"""Inventory of GH executables and how much code they share.

    tools/compare_elfs.py <image-or-elf>...

Takes disc images (every executable on them) and bare ELFs. Prints what each
one is, then a matrix: the share of each row's code, by bytes, found
unchanged in each column, after masking address fields.

Functions come from the symbol table when there is one, and otherwise from
the same splitting match_symbols.py uses on stripped code. That splitting
sometimes merges two functions into one, which then matches nothing, so
figures involving a stripped ELF are lower bounds. Functions under four
instructions are left out as too generic to mean anything.
"""
import sys

import disc
import match_symbols as ms

MIN_WORDS = 4


def profile(label, data):
    sections, headers, name_at = ms.parse_elf(data)
    entry = ms.entry_point(data)
    code = ms.code_words(data, sections)
    words = dict(code)
    if ".symtab" in sections:
        funcs = [(a, s) for a, _, s in ms.debug_functions(data, sections, headers, name_at)]
    else:
        starts = ms.retail_function_starts(data, sections, code)
        end = code[-1][0] + 4
        funcs = [(a, b - a) for a, b in zip(starts, starts[1:] + [end])]

    bodies = {}
    for addr, size in funcs:
        # A stripped ELF's last function in a section would run on into the
        # gap before the next; stop at the section's end.
        body = []
        for k in range(size // 4):
            if addr + 4 * k not in words:
                break
            body.append(ms.mask(words[addr + 4 * k]))
        if len(body) < MIN_WORDS:
            continue
        key = hash(tuple(body))
        bodies[key] = bodies.get(key, 0) + 4 * len(body)

    sha1, known = disc.identify_bytes(data)
    return {
        "label": label,
        "name": known["name"] if known else "unknown",
        "sha1": sha1,
        "size": len(data),
        "entry": entry,
        "text": sections[".text"][5],
        "symbols": len(funcs) if ".symtab" in sections else 0,
        "bodies": bodies,
    }


def shared(a, b):
    total = sum(a["bodies"].values())
    return sum(size for key, size in a["bodies"].items() if key in b["bodies"]) / total


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    elfs = [profile(label, data)
            for path in sys.argv[1:] for label, data in disc.executables_in(path)]

    print(f"{'#':>2}  {'name':<26}{'size':>10}{'entry':>10}{'.text':>10}{'symbols':>9}  sha1        source")
    for i, e in enumerate(elfs, 1):
        print(f"{i:>2}  {e['name']:<26}{e['size']:>10}{e['entry']:>#10x}{e['text']:>10}"
              f"{e['symbols']:>9}  {e['sha1'][:10]}  {e['label']}")

    print("\n% of row's code found unchanged in column\n")
    print("    " + "".join(f"{i:>5}" for i in range(1, len(elfs) + 1)))
    for i, a in enumerate(elfs, 1):
        cells = "".join("    -" if a is b else f"{100 * shared(a, b):>5.0f}" for b in elfs)
        print(f"{i:>2}  {cells}  {a['name']}")


if __name__ == "__main__":
    main()
