#!/usr/bin/env python3
"""Carry function names from GH2's debug ELF onto the stripped retail ELF.

Both builds come from the same source and compiler, but every address differs,
and the debug build carries extra code: asserts, stats, debug-only calls.

Pass 1, exact. A debug function is found in retail by its instruction pattern
with address-dependent fields masked out: jump targets, lui immediates, and the
immediates of addiu, ori and loads/stores, which carry %lo halves and
gp-relative offsets. Masking those also hides struct offsets, so small
functions collide; ties are broken by raw-word agreement and by link order,
which both builds share.

Pass 2, fuzzy. Retail code between two matched neighbours is split into
functions and aligned against the unresolved debug functions in the same span,
scored on instruction similarity and on agreement between their named call
targets. Each round names more call targets, so it repeats until nothing new
resolves.

    tools/match_symbols.py [--check] <debug.elf> <retail.elf-or-disc> [out.symbols]

The symbols file is committed as config/<game>-retail.symbols, since users do
not have the debug ELF to regenerate it.

--check measures the map against evidence the matcher never scores on:
string literals each pair references, and a holdout that hides known exact
matches and counts how many the fuzzy pass puts back correctly.
"""
import bisect
import difflib
import random
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

import disc

CODE_SECTIONS = (".text", "obj_load", "rnd_load", "rnd_draw")
JUMPS = {0x02, 0x03}  # j, jal
JAL = 0x03
LUI = 0x0F
# addiu daddiu ori, loads and stores including FPU, VU0 and 128-bit forms
MASKED_IMM = {0x09, 0x19, 0x0D, 0x1A, 0x1B, 0x1E, 0x1F,
              0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
              0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E,
              0x31, 0x36, 0x37, 0x39, 0x3E, 0x3F}
JR_RA = 0x03E00008
NOP = 0
KEY_WORDS = 4
MIN_WORDS = 4
FUZZY_THRESHOLD = 0.6

STATUSES = ("exact", "exact_by_order", "fuzzy", "ambiguous", "unmatched", "tiny")


def parse_elf(data):
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    headers = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
               for i in range(shnum)]
    strtab_off = headers[shstrndx][4]

    def name_at(table_off, off):
        end = data.index(b"\0", table_off + off)
        return data[table_off + off:end].decode("latin-1")

    sections = {name_at(strtab_off, h[0]): h for h in headers}
    return data, sections, headers, name_at


def code_sections(sections):
    """The code sections this ELF has. GH1 predates obj_load and rnd_*."""
    return [sections[n] for n in CODE_SECTIONS if n in sections]


def code_words(data, sections):
    """(address, word) for every instruction in the code sections, in address order."""
    out = []
    for _, _, _, addr, off, size, *_ in code_sections(sections):
        words = struct.unpack_from(f"<{size // 4}I", data, off)
        out.extend((addr + i * 4, w) for i, w in enumerate(words))
    return out


def debug_functions(data, sections, headers, name_at):
    """Named FUNC symbols in code sections, one per address: [(addr, name, size)]."""
    _, _, _, _, off, size, link, *_ = sections[".symtab"]
    strtab_off = headers[link][4]
    code_ranges = [(s[3], s[3] + s[5]) for s in code_sections(sections)]
    by_addr = {}
    for i in range(size // 16):
        st_name, value, sym_size, info, _, _ = struct.unpack_from("<IIIBBH", data, off + i * 16)
        if info & 0xF != 2 or sym_size < 4:
            continue
        if not any(lo <= value < hi for lo, hi in code_ranges):
            continue
        by_addr.setdefault(value, (name_at(strtab_off, st_name), sym_size))
    return sorted((addr, name, size) for addr, (name, size) in by_addr.items())


def entry_point(data):
    return struct.unpack_from("<I", data, 0x18)[0]


def data_code_refs(data, sections, addrs):
    """Code addresses held in .data, as two sets.

    vtable: the function word of a gcc 2 vtable entry, which is 8 bytes, a
    zero delta word then the function at +4. Always a function start.
    table: any code address after a zero or another code address, which also
    takes in callback tables. A function start only right after a return.

    Jump tables live in .rodata and point inside functions, so it is left
    out, as are code-range values among plain integers (unwind records,
    constants like 0x400000)."""
    if ".data" not in sections:
        return set(), set()
    _, _, _, base, off, size, *_ = sections[".data"]
    words = struct.unpack_from(f"<{size // 4}I", data, off)
    vtable, table = set(), set()
    for i in range(1, len(words)):
        w, prev = words[i], words[i - 1]
        if w not in addrs:
            continue
        if prev == 0 and (base + 4 * i) % 8 == 4:
            vtable.add(w)
        if prev == 0 or prev in addrs:
            table.add(w)
    return vtable, table


def code_built_refs(code, addrs, window=8):
    """Code addresses built in code by `lui rX, hi` then `addiu/ori rY, rX, lo`
    within a few instructions: function pointers passed as arguments, like a
    qsort comparator."""
    stores = {0x28, 0x29, 0x2B, 0x3F}  # sb, sh, sw, sd leave rt unchanged
    out = set()
    for i, (_, w) in enumerate(code):
        if w >> 26 != LUI:
            continue
        base, hi = (w >> 16) & 31, (w & 0xFFFF) << 16
        for _, w2 in code[i + 1:i + 1 + window]:
            op, rs, rt = w2 >> 26, (w2 >> 21) & 31, (w2 >> 16) & 31
            if op in (0x09, 0x0D) and rs == base:  # addiu, ori
                lo = w2 & 0xFFFF
                if op == 0x09 and lo & 0x8000:
                    lo -= 0x10000
                target = (hi + lo) & 0xFFFFFFFF
                if target in addrs:
                    out.add(target)
                break
            if rt == base and op not in stores:
                break
    return out


def retail_function_starts(data, sections, code):
    """Addresses that begin a function: the ELF entry point, every jal target,
    every vtable entry, and whatever follows a return that is a stack-frame
    prologue, a table entry or an address built in code. The entry needs
    naming outright: nothing calls _start, and the function before it in GH2
    is the main loop, which never returns. Vtable entries catch leaf virtuals
    like AsyncFile::Fail, two instructions with no prologue that nothing calls
    directly; they need no return before them because filler between
    functions is not always nops. Built addresses catch callbacks like
    DataArray::Sort's qsort comparator. Checked against the GH1, GH2 and 80s
    debug symbol tables, none of these rules adds a false start."""
    addrs = {a for a, _ in code}
    vtable, refs = data_code_refs(data, sections, addrs)
    refs |= code_built_refs(code, addrs)
    starts = {entry_point(data)} | vtable | {(w & 0x3FFFFFF) << 2 for _, w in code if w >> 26 == JAL}
    for i in range(len(code) - 2):
        if code[i][1] != JR_RA:
            continue
        j = i + 2  # past the delay slot
        while j < len(code) and code[j][1] == NOP:
            j += 1
        if j == len(code):
            continue
        a, w = code[j]
        # addiu $sp, $sp, -N
        if (w >> 16 == 0x27BD and w & 0x8000) or a in refs:
            starts.add(a)
    return sorted(s for s in starts if s in addrs)


def mask(word):
    op = word >> 26
    if op in JUMPS:
        return op << 26
    if op == LUI or op in MASKED_IMM:
        return word & 0xFFFF0000
    return word


def opcode(word):
    """Coarse instruction class for fuzzy comparison: opcode, plus funct for SPECIAL."""
    op = word >> 26
    return (op << 6) | (word & 0x3F) if op == 0 else op << 6


def callees(words):
    return [(w & 0x3FFFFFF) << 2 for w in words if w >> 26 == JAL]


def lcs(a, b):
    if not a or not b:
        return 0
    prev = [0] * (len(b) + 1)
    for x in a:
        cur = [0]
        for j, y in enumerate(b):
            cur.append(prev[j] + 1 if x == y else max(prev[j + 1], cur[j]))
        prev = cur
    return prev[-1]


def longest_increasing(pairs):
    """Indices into pairs (sorted by first element) whose second elements increase."""
    tails, tail_idx, prev = [], [], [-1] * len(pairs)
    for i, (_, r) in enumerate(pairs):
        k = bisect.bisect_left(tails, r)
        if k == len(tails):
            tails.append(r)
            tail_idx.append(i)
        else:
            tails[k] = r
            tail_idx[k] = i
        prev[i] = tail_idx[k - 1] if k else -1
    keep, i = set(), tail_idx[-1] if tail_idx else -1
    while i >= 0:
        keep.add(i)
        i = prev[i]
    return keep


class Matcher:
    def __init__(self, debug_elf, retail_elf):
        d_data, d_secs, d_hdrs, d_name = parse_elf(debug_elf)
        r_data, r_secs, _, _ = parse_elf(retail_elf)
        self.d_secs, self.r_secs = d_secs, r_secs
        self.d_data, self.r_data = d_data, r_data
        self.funcs = debug_functions(d_data, d_secs, d_hdrs, d_name)

        d_words = dict(code_words(d_data, d_secs))
        self.d_body = {a: [d_words[a + 4 * k] for k in range(s // 4)] for a, _, s in self.funcs}

        r_code = code_words(r_data, r_secs)
        self.r_addr = [a for a, _ in r_code]
        self.r_raw = [w for _, w in r_code]
        self.r_mask = [mask(w) for w in self.r_raw]
        self.r_pos = {a: i for i, a in enumerate(self.r_addr)}
        self.r_starts = retail_function_starts(r_data, r_secs, r_code)

        self.results = {}  # debug addr -> (status, retail addr or None, score)

    def exact(self):
        index = defaultdict(list)
        for i in range(len(self.r_mask) - KEY_WORDS + 1):
            index[tuple(self.r_mask[i:i + KEY_WORDS])].append(i)

        candidates = {}
        for addr, _, size in self.funcs:
            raw = self.d_body[addr]
            n = len(raw)
            if n < MIN_WORDS:
                self.results[addr] = ("tiny", None, 0)
                continue
            pattern = [mask(w) for w in raw]
            hits = [i for i in index.get(tuple(pattern[:KEY_WORDS]), ())
                    if self.r_mask[i:i + n] == pattern]
            if not hits:
                self.results[addr] = ("unmatched", None, 0)
                continue
            # Prefer the hit whose unmasked words agree most; a tie stays ambiguous.
            scored = sorted(((sum(a == b for a, b in zip(raw, self.r_raw[i:i + n])), i)
                             for i in hits), reverse=True)
            candidates[addr] = [self.r_addr[i] for s, i in scored if s == scored[0][0]]
            self.results[addr] = ("ambiguous", None, 0)

        # Link order is shared, so true matches increase together. Keep the
        # longest increasing run of unique matches.
        unique = sorted((a, c[0]) for a, c in candidates.items() if len(c) == 1)
        keep = longest_increasing(unique)
        for i, (a, r) in enumerate(unique):
            if i in keep:
                self.results[a] = ("exact", r, 1.0)

        # A tie resolves to the one candidate between its matched neighbours.
        anchors = self.anchors()
        anchor_d = [a for a, _ in anchors]
        for a, c in candidates.items():
            if len(c) == 1:
                continue
            k = bisect.bisect_left(anchor_d, a)
            lo = anchors[k - 1][1] if k else -1
            hi = anchors[k][1] if k < len(anchors) else 1 << 32
            inside = [r for r in c if lo < r < hi]
            if len(inside) == 1:
                self.results[a] = ("exact_by_order", inside[0], 1.0)

    def anchors(self):
        return sorted((a, r) for a, (_, r, _) in self.results.items() if r is not None)

    def retail_body(self, start, end):
        i, j = self.r_pos[start], self.r_pos.get(end, len(self.r_raw))
        return self.r_raw[i:j]

    def similarity(self, d_addr, r_words, r_to_d):
        d_words = self.d_body[d_addr]
        ops = difflib.SequenceMatcher(None, [opcode(w) for w in d_words],
                                      [opcode(w) for w in r_words], autojunk=False).ratio()
        # Only call targets named on both sides can be compared.
        r_calls = [r_to_d[c] for c in callees(r_words) if c in r_to_d]
        named = set(r_to_d.values())
        d_calls = [c for c in callees(d_words) if c in named]
        if not r_calls and not d_calls:
            return ops
        calls = lcs(d_calls, r_calls) / max(len(d_calls), len(r_calls))
        return (ops + calls) / 2

    def fuzzy_round(self):
        anchors = self.anchors()
        r_to_d = {r: d for d, r in anchors}
        matched_r = set(r_to_d)
        unresolved = sorted(a for a, (_, r, _) in self.results.items() if r is None)
        bounds = [(-1, -1)] + anchors + [(1 << 32, 1 << 32)]
        found = 0
        for (d_lo, r_lo), (d_hi, r_hi) in zip(bounds, bounds[1:]):
            d_gap = unresolved[bisect.bisect_right(unresolved, d_lo):
                               bisect.bisect_left(unresolved, d_hi)]
            if not d_gap:
                continue
            lo = bisect.bisect_right(self.r_starts, r_lo)
            hi = bisect.bisect_left(self.r_starts, r_hi)
            r_gap = [s for s in self.r_starts[lo:hi] if s not in matched_r]
            if not r_gap:
                continue
            ends = self.r_starts[lo + 1:hi + 1] + [r_hi]
            end_of = dict(zip(self.r_starts[lo:hi], ends))
            bodies = [self.retail_body(s, end_of[s]) for s in r_gap]
            for d, r, score in self.align(d_gap, r_gap, bodies, r_to_d):
                self.results[d] = ("fuzzy", r, score)
                found += 1
        return found

    def align(self, d_gap, r_gap, bodies, r_to_d):
        """Order-preserving pairing of two function lists, maximising total
        similarity; either side may skip (debug-only functions, missed splits)."""
        n, m = len(d_gap), len(r_gap)
        if n * m > 4000:  # a huge unresolved span is left for later rounds
            return []
        sim = [[self.similarity(d, b, r_to_d) for b in bodies] for d in d_gap]
        best = [[0.0] * (m + 1) for _ in range(n + 1)]
        for i in range(n - 1, -1, -1):
            for j in range(m - 1, -1, -1):
                take = sim[i][j] + best[i + 1][j + 1] if sim[i][j] >= FUZZY_THRESHOLD else -1
                best[i][j] = max(take, best[i + 1][j], best[i][j + 1])
        pairs, i, j = [], 0, 0
        while i < n and j < m:
            if sim[i][j] >= FUZZY_THRESHOLD and best[i][j] == sim[i][j] + best[i + 1][j + 1]:
                pairs.append((d_gap[i], r_gap[j], sim[i][j]))
                i, j = i + 1, j + 1
            elif best[i][j] == best[i + 1][j]:
                i += 1
            else:
                j += 1
        return pairs

    def run(self):
        self.exact()
        rounds = 0
        while self.fuzzy_round():
            rounds += 1
        return rounds


def report(m, rounds):
    count, nbytes = defaultdict(int), defaultdict(int)
    for addr, _, size in m.funcs:
        status = m.results[addr][0]
        count[status] += 1
        nbytes[status] += size
    total_n, total_b = len(m.funcs), sum(s for _, _, s in m.funcs)
    print(f"{'status':<16}{'functions':>10}{'%':>8}{'bytes':>11}{'%':>8}")
    for status in STATUSES:
        print(f"{status:<16}{count[status]:>10}{100 * count[status] / total_n:>7.1f}%"
              f"{nbytes[status]:>11}{100 * nbytes[status] / total_b:>7.1f}%")
    print(f"{'total':<16}{total_n:>10}{'':>8}{total_b:>11}")
    print(f"fuzzy rounds: {rounds}, retail function starts found: {len(m.r_starts)}")


def write_symbols(m, path):
    """The retail symbols file symbolize.py reads: one `address name` per line.

    Byte-identical helpers (type info, template destructors) can put several
    debug names on one retail address; the first in debug link order wins."""
    named = {}
    for a, name, _ in m.funcs:
        r = m.results[a][1]
        if r is not None:
            named.setdefault(r, name)
    with open(path, "w") as f:
        f.write("# Retail function names, from tools/match_symbols.py. Regenerate, do not edit.\n")
        f.writelines(f"{r:08x} {named[r]}\n" for r in sorted(named))


def load_elf(path):
    """An ELF file, or the boot executable of a disc image."""
    if disc.is_image(path):
        with disc.Image(path) as img:
            return img.read(img.boot_name())
    return Path(path).read_bytes()


def string_literals(words, data, sections):
    """C strings a function builds with lui/addiu into .rodata."""
    _, _, _, ro_addr, ro_off, ro_size, *_ = sections[".rodata"]
    hi, out = {}, set()
    for w in words:
        op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == LUI:
            hi[rt] = imm << 16
        elif op == 0x09 and rs in hi:  # addiu
            k = ((hi[rs] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF) - ro_addr
            if 0 <= k < ro_size:
                s = data[ro_off + k:data.index(b"\0", ro_off + k)]
                if len(s) >= 4 and all(32 <= c < 127 for c in s):
                    out.add(s)
    return out


def check(m):
    """Evidence the matcher never scores on."""
    def retail_words(r):
        i = bisect.bisect_right(m.r_starts, r)
        return m.retail_body(r, m.r_starts[i] if i < len(m.r_starts) else r + 4)

    # A checker artifact, not a bad match, shows up on exact matches too: a
    # pointer into the middle of a merged string reads as a different string.
    tally = Counter()
    for a, (status, r, _) in m.results.items():
        if r is None:
            continue
        ds = string_literals(m.d_body[a], m.d_data, m.d_secs)
        rs = string_literals(retail_words(r), m.r_data, m.r_secs)
        verdict = "no strings" if not ds or not rs else "agree" if ds & rs else "disagree"
        tally[(status, verdict)] += 1
    print("\nstring literals")
    for status in ("exact", "exact_by_order", "fuzzy"):
        print(f"  {status:<16}" + "".join(f"{v} {tally[(status, v)]:<7}"
                                        for v in ("agree", "disagree", "no strings")))

    random.seed(1)
    truth = {a: r for a, (s, r, _) in m.results.items() if s == "exact" and len(m.d_body[a]) >= 8}
    hidden = random.sample(sorted(truth), len(truth) * 15 // 100)
    h = Matcher(m.d_data, m.r_data)
    h.exact()
    for a in hidden:
        h.results[a] = ("unmatched", None, 0)
    while h.fuzzy_round():
        pass
    right = sum(h.results[a][1] == truth[a] for a in hidden)
    wrong = sum(h.results[a][1] not in (None, truth[a]) for a in hidden)
    print(f"\nholdout: {len(hidden)} exact matches hidden, {right} recovered, "
          f"{wrong} wrong, {len(hidden) - right - wrong} left unmatched")


def main():
    args = [a for a in sys.argv[1:] if a != "--check"]
    if len(args) < 2:
        sys.exit(__doc__)
    m = Matcher(load_elf(args[0]), load_elf(args[1]))
    rounds = m.run()
    report(m, rounds)
    if "--check" in sys.argv:
        check(m)
    if len(args) > 2:
        write_symbols(m, args[2])


if __name__ == "__main__":
    main()
