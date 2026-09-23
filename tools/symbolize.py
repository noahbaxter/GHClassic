#!/usr/bin/env python3
"""Write a symbols file into a copy of a stripped ELF as a real symbol table.

    tools/symbolize.py <stripped.elf-or-disc> <symbols> <out.elf>

Every function gets a symbol, because the recompiler takes symbol sizes as
function bounds. Starts are the named addresses plus the heuristic split
match_symbols.py uses; each function runs to the next start. Unnamed ones
are called func_XXXXXXXX. Program headers are untouched, so the result loads
exactly like the original.
"""
import struct
import sys
from pathlib import Path

import match_symbols as ms

STB_GLOBAL, STT_FUNC = 1, 2
SHT_SYMTAB, SHT_STRTAB = 2, 3


def read_symbols(path):
    out = {}
    for line in Path(path).read_text().splitlines():
        if line and not line.startswith("#"):
            addr, name = line.split(" ", 1)
            out[int(addr, 16)] = name
    return out


def align(buf, n):
    buf.extend(b"\0" * (-len(buf) % n))


def symbolize(elf, names):
    data, sections, headers, _ = ms.parse_elf(elf)
    if ".symtab" in sections:
        raise ValueError("ELF already has a symbol table")
    code = ms.code_words(data, sections)
    starts = sorted(set(names) | set(ms.retail_function_starts(code, ms.entry_point(data))))

    # Section index and end for each code section, to bound the last function.
    section_of = []
    for index, h in enumerate(headers):
        if h in ms.code_sections(sections):
            section_of.append((h[3], h[3] + h[5], index))

    strtab = bytearray(b"\0")
    symtab = bytearray(16)  # entry 0 is the null symbol
    for i, start in enumerate(starts):
        lo, hi, index = next(s for s in section_of if s[0] <= start < s[1])
        end = min(starts[i + 1] if i + 1 < len(starts) else hi, hi)
        name = names.get(start, f"func_{start:08x}")
        symtab += struct.pack("<IIIBBH", len(strtab), start, end - start,
                              (STB_GLOBAL << 4) | STT_FUNC, 0, index)
        strtab += name.encode() + b"\0"

    # New .shstrtab with the two names added, then .strtab, .symtab and a
    # section header table, all appended after the original contents.
    shoff, = struct.unpack_from("<I", data, 0x20)
    shnum, shstrndx = struct.unpack_from("<HH", data, 0x30)
    old_shstr = headers[shstrndx]
    shstr = bytearray(data[old_shstr[4]:old_shstr[4] + old_shstr[5]])
    symtab_name = len(shstr)
    shstr += b".symtab\0"
    strtab_name = len(shstr)
    shstr += b".strtab\0"

    out = bytearray(data)
    align(out, 4)
    shstr_off = len(out)
    out += shstr
    align(out, 4)
    strtab_off = len(out)
    out += strtab
    align(out, 4)
    symtab_off = len(out)
    out += symtab
    align(out, 4)
    new_shoff = len(out)

    for index, h in enumerate(headers):
        h = list(h)
        if index == shstrndx:
            h[4], h[5] = shstr_off, len(shstr)
        out += struct.pack("<IIIIIIIIII", *h)
    strtab_index = shnum + 1
    out += struct.pack("<IIIIIIIIII", symtab_name, SHT_SYMTAB, 0, 0, symtab_off,
                       len(symtab), strtab_index, 1, 4, 16)
    out += struct.pack("<IIIIIIIIII", strtab_name, SHT_STRTAB, 0, 0, strtab_off,
                       len(strtab), 0, 0, 1, 0)

    struct.pack_into("<I", out, 0x20, new_shoff)
    struct.pack_into("<H", out, 0x30, shnum + 2)
    named = sum(1 for s in starts if s in names)
    return bytes(out), named, len(starts)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    elf = ms.load_elf(sys.argv[1])
    out, named, total = symbolize(elf, read_symbols(sys.argv[2]))
    Path(sys.argv[3]).write_bytes(out)
    print(f"{total} functions, {named} named, {total - named} func_XXXXXXXX")


if __name__ == "__main__":
    main()
