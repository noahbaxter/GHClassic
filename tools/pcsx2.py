#!/usr/bin/env python3
"""Run retail GH2 in PCSX2 and drive it with scenario files, over PINE. macOS only.

    tools/pcsx2.py setup                 make build/pcsx2: settings, BIOS, the mailbox patch
    tools/pcsx2.py boot [--secs 60]      boot headless, answer a memory read over PINE
    tools/pcsx2.py run <scenario> [out]  run a scenario file's steps against the retail game
    tools/pcsx2.py eval '<script>'       evaluate one script line in a PCSX2 already running

PCSX2 is the build of ~/Code/third_party/pcsx2 (or $PCSX2), the BIOS is read
from $PS2_BIOS. Everything written lands in build/pcsx2 and the run's folder.

The mailbox: a patch points the main loop's call of UIManager::Poll at a stub
in free RAM below the executable. Each frame the stub counts a poll, and when
the mailbox's flag is set it parses the text there (DataReadString), evaluates
each node, leaves the last value in the mailbox and clears the flag. Steps
that do not wait go in one mailbox, so they run in one frame, as
src/dev/scenario.cpp runs them.

(clock) and (freeze) are the same patch: TaskMgr::SetUISeconds takes its time
from the mailbox (a base plus 1/64 s a poll, up to a hold), CamShot::Shake
returns no shake and CharHair::Poll does nothing. (shot name) saves a state
and keeps its screenshot.
"""
import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOME = ROOT / "build" / "pcsx2" / "PCSX2"  # -datapath takes its parent
PCSX2 = Path(os.environ.get(
    "PCSX2", Path.home() / "Code/third_party/pcsx2/build/pcsx2-qt/PCSX2.app/Contents/MacOS/PCSX2"))
BIOS = Path(os.environ.get("PS2_BIOS", "/Volumes/terramox/Games/Emulation/bios/ps2/ps2-0230a-20080220.bin"))
DISC = Path(os.environ.get("GH2_DISC", ROOT.parent.parent / "ghrecomp" / "game" / "Guitar Hero II (USA).iso"))
ELF = ROOT.parent.parent / "ghrecomp" / "build" / "recomp" / "retail.elf"
SERIAL = "SLUS-21447"

# Retail addresses (src/addresses.h).
MAIN_LOOP_POLL = 0x100B90  # _start's jal UIManager::Poll
UI_MANAGER_POLL = 0x214510
DATA_READ_STRING = 0x2B28D0
DATA_NODE_EVALUATE = 0x2B7D38
SET_UI_SECONDS = 0x2C6738
SET_SECONDS_BEAT = 0x2C6798
CAM_SHOT_SHAKE = 0x262F38
CHAR_HAIR_POLL = 0x176FB8
THE_TASK_MGR = 0x51EE40

# The cave: code, then the mailbox, then the script text.
CAVE = 0x000E0000
STUB_UI = CAVE + 0x200
STUB_BEAT = CAVE + 0x280
STUB_SHAKE = CAVE + 0x300
STUB_HAIR = CAVE + 0x340
BOX = CAVE + 0x800
RUN, ROOT_ARRAY, VALUE, TYPE, DONE, POLLS = (BOX + 4 * n for n in range(6))
# The UI clock: BASE + CLOCK_POLLS / RATE, no later than HOLD, while MODE.
# RATE is 64, a step floats hold exactly, so this FPU and the host's agree.
MODE, BASE, HOLD, CLOCK_POLLS, RATE, STILL = (BOX + 0x20 + 4 * n for n in range(6))
# Taken up, with CLOCK_POLLS zeroed, in the frame the mailbox next runs.
ARM, NEW_BASE, NEW_HOLD, NEW_STILL = (BOX + 0x40 + 4 * n for n in range(4))
FREEZE_BEAT, SECONDS, BEAT = (BOX + 0x50 + 4 * n for n in range(3))
# While set the main loop waits in the stub, between one frame's draw and the next one's poll. On a page
# of its own: PCSX2 protects a page it has compiled code from, and a write to the stub's from here while
# the stub is what runs kills it.
PARK = CAVE + 0x7000
TEXT = CAVE + 0x1000
TEXT_MAX = 0x6000
NEVER = 3.0e38

K_INT, K_FLOAT, K_SYMBOL, K_ARRAY, K_STRING = 0x0, 0x1, 0x5, 0x10, 0x12

ZERO, AT, V0, V1, A0, A2, A3, S0, S1, S4, SP, RA = 0, 1, 2, 3, 4, 6, 7, 16, 17, 20, 29, 31


def i_type(op, rs, rt, imm):
    return op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFF)


def addiu(rt, rs, imm): return i_type(0x09, rs, rt, imm)
def lui(rt, imm): return i_type(0x0F, 0, rt, imm)
def lw(rt, off, base): return i_type(0x23, base, rt, off)
def lh(rt, off, base): return i_type(0x21, base, rt, off)
def sw(rt, off, base): return i_type(0x2B, base, rt, off)
def ld(rt, off, base): return i_type(0x37, base, rt, off)
def sd(rt, off, base): return i_type(0x3F, base, rt, off)
def sq(rt, off, base): return i_type(0x1F, base, rt, off)
def lwc1(ft, off, base): return i_type(0x31, base, ft, off)
def swc1(ft, off, base): return i_type(0x39, base, ft, off)
def beq(rs, rt, words): return i_type(0x04, rs, rt, words)
def bne(rs, rt, words): return i_type(0x05, rs, rt, words)
def jal(addr): return 0x0C000000 | addr >> 2
def j(addr): return 0x08000000 | addr >> 2
def sll(rd, rt, sa): return rt << 16 | rd << 11 | sa << 6
def addu(rd, rs, rt): return rs << 21 | rt << 16 | rd << 11 | 0x21
def slt(rd, rs, rt): return rs << 21 | rt << 16 | rd << 11 | 0x2A
def mtc1(rt, fs): return 0x44800000 | rt << 16 | fs << 11
def cvt_s_w(fd, fs): return 0x46800020 | fs << 11 | fd << 6
def div_s(fd, fs, ft): return 0x46000003 | ft << 16 | fs << 11 | fd << 6
def add_s(fd, fs, ft): return 0x46000000 | ft << 16 | fs << 11 | fd << 6
def mov_s(fd, fs): return 0x46000006 | fs << 11 | fd << 6
def c_lt_s(fs, ft): return 0x46000034 | ft << 16 | fs << 11
def bc1f(words): return 0x45000000 | (words & 0xFFFF)


JR_RA = 0x03E00008
NOP = 0


def box(addr):
    return addr - CAVE


def poll_stub():
    return [
        addiu(SP, SP, -0x20),
        sd(RA, 0x00, SP),
        sd(A0, 0x08, SP),
        sd(S0, 0x10, SP),
        sd(S1, 0x18, SP),
        lui(S0, CAVE >> 16),
        # parked
        lw(V0, box(PARK), S0),
        bne(V0, ZERO, -2),              # -> parked
        NOP,
        lw(V0, box(POLLS), S0),
        addiu(V0, V0, 1),
        sw(V0, box(POLLS), S0),
        lw(V0, box(CLOCK_POLLS), S0),
        addiu(V0, V0, 1),
        sw(V0, box(CLOCK_POLLS), S0),
        lw(V0, box(RUN), S0),
        beq(V0, ZERO, 38),              # -> out
        lw(V0, box(ARM), S0),
        beq(V0, ZERO, 11),              # -> parse
        NOP,
        lw(V0, box(NEW_BASE), S0),
        sw(V0, box(BASE), S0),
        lw(V0, box(NEW_HOLD), S0),
        sw(V0, box(HOLD), S0),
        lw(V0, box(NEW_STILL), S0),
        sw(V0, box(STILL), S0),
        sw(ZERO, box(CLOCK_POLLS), S0),
        addiu(V0, ZERO, 1),
        sw(V0, box(MODE), S0),
        sw(ZERO, box(ARM), S0),
        # parse
        jal(DATA_READ_STRING),
        addiu(A0, S0, box(TEXT)),
        sw(V0, box(ROOT_ARRAY), S0),
        beq(V0, ZERO, 16),              # -> done: a parse error
        addiu(S1, ZERO, 0),
        # each: evaluate node s1 of the root array
        lw(V0, box(ROOT_ARRAY), S0),
        lh(V1, 0x8, V0),
        slt(V1, S1, V1),
        beq(V1, ZERO, 11),              # -> done
        lw(A0, 0x0, V0),
        sll(V1, S1, 3),
        jal(DATA_NODE_EVALUATE),
        addu(A0, A0, V1),
        lw(V1, 0x0, V0),
        sw(V1, box(VALUE), S0),
        lw(V1, 0x4, V0),
        sw(V1, box(TYPE), S0),
        beq(ZERO, ZERO, -13),           # -> each
        addiu(S1, S1, 1),
        NOP,
        # done
        lw(V0, box(DONE), S0),
        addiu(V0, V0, 1),
        sw(V0, box(DONE), S0),
        sw(ZERO, box(RUN), S0),
        NOP,
        # out: on to the poll itself, which returns to the main loop
        ld(RA, 0x00, SP),
        ld(A0, 0x08, SP),
        ld(S0, 0x10, SP),
        ld(S1, 0x18, SP),
        j(UI_MANAGER_POLL),
        addiu(SP, SP, 0x20),
    ]


def ui_seconds_stub():
    return [
        lui(V0, CAVE >> 16),
        lw(AT, box(MODE), V0),
        beq(AT, ZERO, 13),              # -> the function
        NOP,
        lw(AT, box(CLOCK_POLLS), V0),
        mtc1(AT, 0),
        cvt_s_w(0, 0),
        lwc1(12, box(RATE), V0),
        div_s(0, 0, 12),
        lwc1(12, box(BASE), V0),
        add_s(12, 12, 0),
        lwc1(0, box(HOLD), V0),
        c_lt_s(0, 12),
        bc1f(2),                        # -> the function
        NOP,
        mov_s(12, 0),
        # TaskMgr::SetUISeconds 0x2c6738
        lw(V0, 0x28, A0),
        addiu(V0, V0, 0x28),
        lwc1(0, 0xC, V0),
        swc1(12, 0xC, V0),
        JR_RA,
        swc1(0, 0x10, V0),
    ]


def seconds_beat_stub():
    return [
        lui(V0, CAVE >> 16),
        lw(V1, box(FREEZE_BEAT), V0),
        beq(V1, ZERO, 3),
        NOP,
        lwc1(12, box(SECONDS), V0),
        lwc1(13, box(BEAT), V0),
        # TaskMgr::SetSecondsBeat 0x2c6798
        sw(ZERO, 0x40, A0),
        lw(V1, 0x28, A0),
        lwc1(0, 0xC, V1),
        swc1(12, 0xC, V1),
        swc1(0, 0x10, V1),
        lwc1(0, 0x20, V1),
        swc1(13, 0x20, V1),
        JR_RA,
        swc1(0, 0x24, V1),
    ]


def shake_stub():
    # Shake(this, freq, amp, const Vector2 &, Vector3 &pos, Vector3 &rot)
    return [
        lui(V0, CAVE >> 16),
        lw(V0, box(STILL), V0),
        bne(V0, ZERO, 5),               # -> still
        NOP,
        addiu(SP, SP, -0x80),           # 0x262f38
        lui(AT, 0x3C8E),                # 0x262f3c
        j(CAM_SHOT_SHAKE + 8),
        NOP,
        # still
        sq(ZERO, 0x0, A2),
        sq(ZERO, 0x0, A3),
        JR_RA,
        NOP,
    ]


def hair_stub():
    return [
        lui(V0, CAVE >> 16),
        lw(V0, box(STILL), V0),
        bne(V0, ZERO, 5),               # -> still
        NOP,
        addiu(SP, SP, -0x1A0),          # 0x176fb8
        sq(S4, 0x130, SP),              # 0x176fbc
        j(CHAR_HAIR_POLL + 8),
        NOP,
        # still
        JR_RA,
        NOP,
    ]


def patch_words():
    words = {}
    for base, code in ((CAVE, poll_stub()), (STUB_UI, ui_seconds_stub()), (STUB_BEAT, seconds_beat_stub()),
                       (STUB_SHAKE, shake_stub()), (STUB_HAIR, hair_stub())):
        for n, word in enumerate(code):
            words[base + 4 * n] = word
    words[RATE] = f32(64.0)
    words[HOLD] = f32(NEVER)
    words[MAIN_LOOP_POLL] = jal(CAVE)
    for entry, stub in ((SET_UI_SECONDS, STUB_UI), (SET_SECONDS_BEAT, STUB_BEAT), (CAM_SHOT_SHAKE, STUB_SHAKE),
                        (CHAR_HAIR_POLL, STUB_HAIR)):
        words[entry] = j(stub)
        words[entry + 4] = NOP
    return words


def f32(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def elf_crc(path):
    data = path.read_bytes()
    crc = 0
    for (word,) in struct.iter_unpack("<I", data[:len(data) & ~3]):
        crc ^= word
    return crc


INI = """[UI]
SettingsVersion = 1
SetupWizardIncomplete = false
ConfirmShutdown = false
StartFullscreen = false

[Folders]
Bios = bios
Snapshots = snaps
Savestates = sstates
MemoryCards = memcards
Logs = logs
Cheats = cheats
Patches = patches
Cache = cache
Textures = textures
InputProfiles = inputprofiles
Videos = videos

[Filenames]
BIOS = {bios}

[EmuCore]
EnablePINE = true
PINESlot = 28011
EnableCheats = true
EnablePatches = true
EnableFastBoot = true
ManuallySetRealTimeClock = true
RtcYear = 6
RtcMonth = 11
RtcDay = 7
RtcHour = 12
RtcMinute = 0
RtcSecond = 0
InhibitScreensaver = false
WarnAboutUnsafeSettings = false

[EmuCore/GS]
Renderer = 13
upscale_multiplier = 1
VsyncEnable = false
OsdShowMessages = false
GSDumpCompression = 0

[SPU2/Output]
OutputMuted = true
Backend = Null

[Achievements]
Enabled = false

[AutoUpdater]
CheckAtStartup = false
"""


def setup(_args):
    for name in ("bios", "cheats", "inis", "memcards", "sstates", "snaps", "logs"):
        (HOME / name).mkdir(parents=True, exist_ok=True)
    bios = HOME / "bios" / BIOS.name
    if not bios.exists():
        shutil.copyfile(BIOS, bios)
    (HOME / "inis" / "PCSX2.ini").write_text(INI.format(bios=BIOS.name), newline="\n")
    crc = elf_crc(ELF)
    lines = ["gametitle=Guitar Hero II (USA) mailbox", ""]
    lines += [f"patch=0,EE,{addr:08X},word,{word:08X}" for addr, word in sorted(patch_words().items())]
    pnach = HOME / "cheats" / f"{SERIAL}_{crc:08X}.pnach"
    pnach.write_text("\n".join(lines) + "\n", newline="\n")
    print(f"{pnach.relative_to(ROOT)}: {len(lines) - 2} words")


class Pine:
    def __init__(self, timeout=60.0):
        path = os.path.join(os.environ.get("TMPDIR", "/tmp"), "pcsx2.sock")
        deadline = time.monotonic() + timeout
        while True:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                self.sock.connect(path)
                return
            except OSError:
                self.sock.close()
                if time.monotonic() > deadline:
                    raise SystemExit(f"no PINE socket at {path}")
                time.sleep(0.25)

    def message(self, body):
        self.sock.sendall(struct.pack("<I", len(body) + 4) + body)
        data = b""
        while len(data) < 4 or len(data) < struct.unpack("<I", data[:4])[0]:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("PINE closed")
            data += chunk
        if data[4] != 0:
            raise RuntimeError("PINE refused")
        return data[5:]

    def call(self, body, reply):
        """One message of commands; reply is the struct format of what they return."""
        return struct.unpack_from("<" + reply, self.message(body))

    def read32(self, addr):
        return self.call(struct.pack("<BI", 2, addr), "I")[0]

    def write32(self, addr, value):
        self.call(struct.pack("<BII", 6, addr, value & 0xFFFFFFFF), "")

    def read(self, addr, size):
        body = b"".join(struct.pack("<BI", 0, addr + n) for n in range(size))
        return bytes(self.call(body, f"{size}B"))

    def write(self, addr, data):
        self.call(b"".join(struct.pack("<BIB", 4, addr + n, byte) for n, byte in enumerate(data)), "")

    def cstring(self, addr, limit=256):
        if addr == 0:
            return ""
        return self.read(addr, limit).split(b"\0")[0].decode("latin-1")

    def text(self, opcode):
        try:
            return self.message(bytes([opcode]))[4:].split(b"\0")[0].decode()
        except RuntimeError:
            return None

    def version(self): return self.text(8)
    def game_id(self): return self.text(0xC)

    def status(self):
        try:
            return ("running", "paused", "shutdown")[self.call(bytes([0xF]), "I")[0]]
        except RuntimeError:
            return "none"

    def save_state(self, slot):
        self.call(bytes([9, slot]), "")

    def gs_dump(self, png, frames):
        """A GS dump of the next frames beside png (a local patch to PCSX2's PINE.cpp)."""
        path = str(png).encode()
        self.call(struct.pack("<BBI", 0x10, frames, len(path)) + path, "")

    def polls(self):
        return self.read32(POLLS)

    def wait_polls(self, count, timeout=120.0):
        target = self.polls() + count
        deadline = time.monotonic() + timeout
        while self.polls() < target:
            if time.monotonic() > deadline:
                raise TimeoutError(f"the game stopped polling at {self.polls()}")
            time.sleep(0.002)

    def evaluate(self, script, timeout=30.0, raw=False):
        """Runs script text in one frame of the game; the last node's value, or with raw its word."""
        data = script.encode("latin-1") + b"\0"
        if len(data) > TEXT_MAX:
            raise ValueError("script too long for the mailbox")
        done = self.read32(DONE)
        self.write(TEXT, data)
        self.write32(RUN, 1)
        deadline = time.monotonic() + timeout
        while self.read32(DONE) == done:
            if time.monotonic() > deadline:
                raise TimeoutError("the game did not run the script")
            time.sleep(0.002)
        if self.read32(ROOT_ARRAY) == 0:
            raise RuntimeError(f"parse error: {script}")
        value, kind = self.read32(VALUE), self.read32(TYPE)
        if raw:
            return value
        if kind == K_INT:
            return value - (1 << 32) if value & 0x80000000 else value
        if kind == K_FLOAT:
            return struct.unpack("<f", struct.pack("<I", value))[0]
        if kind == K_SYMBOL:
            return self.cstring(value)
        if kind == K_STRING:
            return self.cstring(self.read32(value)) if value else ""
        return f"<type {kind:#x} {value:#x}>"


def launch(log):
    if not PCSX2.exists():
        raise SystemExit(f"no PCSX2 at {PCSX2}")
    sock = Path(os.environ.get("TMPDIR", "/tmp")) / "pcsx2.sock"
    if sock.exists():
        sock.unlink()
    # Through open, in the background and hidden: its window never takes the focus.
    app = PCSX2.parents[2]
    before = set(pids())
    subprocess.run(
        ["open", "-g", "-j", "-n", "-a", str(app), "--stdout", str(log), "--stderr", str(log), "--env", "PCSX2_SURFACELESS=1",
         "--args",
         "-datapath", str(HOME.parent), "-batch", "-nogui", "-fastboot", "--", str(DISC)], check=True)
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        new = set(pids()) - before
        if new:
            return Process(new.pop())
        time.sleep(0.1)
    raise SystemExit("PCSX2 did not start")


def pids():
    found = subprocess.run(["pgrep", "-f", f"{PCSX2} -datapath {HOME.parent}"], capture_output=True, text=True)
    return [int(x) for x in found.stdout.split()]


class Process:
    """The PCSX2 this run started, and only that one."""

    def __init__(self, pid):
        self.pid = pid

    def terminate(self):
        try:
            os.kill(self.pid, 15)
        except ProcessLookupError:
            pass

    def wait(self):
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            try:
                os.kill(self.pid, 0)
            except ProcessLookupError:
                return
            time.sleep(0.1)
        os.kill(self.pid, 9)


def wait_game(pine, timeout=120.0):
    """Until the patched main loop is polling."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            if pine.status() == "running" and pine.read32(MAIN_LOOP_POLL) == jal(CAVE) and pine.polls() > 0:
                return
        except RuntimeError:
            pass
        if time.monotonic() > deadline:
            raise TimeoutError("the game never reached its main loop")
        time.sleep(0.25)


def boot(args):
    log = HOME / "logs" / "boot.log"
    proc = launch(log)
    try:
        pine = Pine()
        print("PINE:", pine.version())
        deadline = time.monotonic() + args.secs
        while time.monotonic() < deadline and pine.status() != "running":
            time.sleep(0.25)
        time.sleep(3.0)
        print("status:", pine.status(), "id:", pine.game_id())
        print(f"entry {0x100BF0:#x}: {pine.read32(0x100BF0):#010x}")
        print(f"main loop {MAIN_LOOP_POLL:#x}: {pine.read32(MAIN_LOOP_POLL):#010x} (patched is {jal(CAVE):#010x})")
        wait_game(pine, args.secs)
        first = pine.polls()
        time.sleep(2.0)
        print(f"polls: {first} then {pine.polls()} two seconds on")
    finally:
        proc.terminate()
        proc.wait()


# Kept in a variable: a string's text is freed with its node, before it can be read from here.
SCREEN = '{set $pine {if_else {|| {ui in_transition} {! {ui current_screen}}} "" {{ui current_screen} name}}}'


def forms(path):
    """A scenario file's top-level forms, its #include lines replaced by their files."""
    text = ""
    for line in path.read_text().splitlines():
        if line.startswith("#include "):
            yield from forms(path.parent / line[9:].strip())
        else:
            text += line + "\n"
    depth, start, i = 0, 0, 0
    while i < len(text):
        c = text[i]
        if c == ";" and depth == 0:
            i = text.find("\n", i)
            if i < 0:
                break
        elif c == '"':
            i = text.find('"', i + 1)
        elif c in "({":
            if depth == 0:
                start = i
            depth += 1
        elif c in ")}":
            depth -= 1
            if depth == 0:
                yield text[start:i + 1]
        i += 1


class Runner:
    def __init__(self, pine, out):
        self.pine = pine
        self.out = out
        self.start = pine.polls()
        self.batch = []      # commands for one frame
        self.clock = None    # the UI clock's base, once a (clock) ran
        self.arm = None      # (base, hold, still) for that frame
        self.times = None    # (seconds, beat) a freeze writes after it
        self.slot = 0

    def flush(self):
        pine = self.pine
        if self.arm:
            base, hold, still = self.arm
            pine.write32(NEW_BASE, f32(base))
            pine.write32(NEW_HOLD, f32(hold))
            pine.write32(NEW_STILL, still)
            pine.write32(ARM, 1)
        if self.batch or self.arm:
            pine.evaluate("\n".join(self.batch) or "0")
        if self.times:
            # GamePanel leaves these alone while paused: current and last, at TaskMgr +0x28.
            timers = pine.read32(THE_TASK_MGR + 0x28)
            for at in (0xC, 0x10):
                pine.write32(timers + at, f32(self.times[0]))
                pine.write32(timers + 0x14 + at, f32(self.times[1]))
        self.batch, self.arm, self.times = [], None, None

    def shot(self, name):
        (self.out / "shots").mkdir(exist_ok=True)
        (self.out / "shots" / f"{name}.png").write_bytes(self.state("Screenshot.png"))

    def state(self, entry):
        """Saves a state and returns one file of it."""
        states = HOME / "sstates"
        before = {p: p.stat().st_mtime_ns for p in states.glob("*.p2s")}
        self.slot = self.slot % 9 + 1
        self.pine.save_state(self.slot)
        deadline = time.monotonic() + 30.0
        while True:
            fresh = [p for p in states.glob("*.p2s") if before.get(p) != p.stat().st_mtime_ns]
            if fresh:
                time.sleep(0.5)
                try:
                    with zipfile.ZipFile(fresh[0]) as state:
                        return state.read(entry)
                except (zipfile.BadZipFile, KeyError):
                    pass
            if time.monotonic() > deadline:
                raise TimeoutError("no save state written")
            time.sleep(0.1)

    def run(self, path):
        pine = self.pine
        for form in forms(path):
            seconds = (pine.polls() - self.start) / 60.0
            if form[0] == "{":
                self.batch.append(form)
                continue
            words = form[1:-1].split(None, 1)
            verb, rest = words[0], (words[1] if len(words) > 1 else "")
            if verb == "clock":
                self.clock = float(rest)
                self.arm = (self.clock, NEVER, 0)
                continue
            if verb == "freeze":
                ui, at, beat = (float(x) for x in rest.split())
                self.arm = (ui if self.clock is None else self.clock, ui, 1)
                self.times = (at, beat)
                continue
            self.flush()
            print(f"[scenario] {seconds:.2f} s: ({verb})", flush=True)
            if verb == "wait":
                pine.wait_polls(int(float(rest) * 60.0), timeout=float(rest) * 4.0 + 30.0)
            elif verb in ("wait_screen", "wait_until"):
                if verb == "wait_screen":
                    name, _, limit = rest.partition(" ")
                    def ready(): return pine.evaluate(SCREEN) == name
                else:
                    script, _, limit = rest.rpartition("}")
                    def ready(): return pine.evaluate(script + "}") != 0
                deadline = time.monotonic() + (float(limit) if limit.strip() else 30.0)
                while not ready():
                    if time.monotonic() > deadline:
                        print(f"[scenario] FAIL: ({form[1:-1]}) timed out, on '{pine.evaluate(SCREEN)}'")
                        return False
                    time.sleep(0.02)
            elif verb == "print":
                print("[scenario]", " ".join(str(pine.evaluate("{set $pine %s}" % x)) for x in forms_in(rest)))
            elif verb == "poke":
                # A word of the object the first argument evaluates to: an int or a float's bits.
                script, _, numbers = rest.rpartition("}")
                offset, value = numbers.split()
                target = pine.evaluate(script + "}", raw=True)
                if target:
                    pine.write32(target + int(offset, 0), f32(float(value)) if "." in value else int(value, 0))
            elif verb == "ram":
                # The EE's memory, out of a save state: what (transplant) takes in our build. Taken with
                # the main loop parked, or it is of the middle of a poll, with bones half worked out.
                (self.out / "dumps").mkdir(exist_ok=True)
                pine.write32(PARK, 1)
                polls = -1
                while polls != pine.polls():
                    polls = pine.polls()
                    time.sleep(0.2)
                memory = self.state("eeMemory.bin")
                pine.write32(PARK, 0)
                (self.out / "dumps" / f"{rest.strip()}.ram").write_bytes(memory)
            elif verb == "peek":
                script, _, offset = rest.rpartition("}")
                target = pine.evaluate(script + "}", raw=True)
                word = pine.read32(target + int(offset, 0)) if target else 0
                print(f"[scenario] peek {word} {struct.unpack('<f', struct.pack('<I', word))[0]:g}")
            elif verb == "shot":
                self.shot(rest.strip())
            elif verb == "dump":
                # Two frames: the second's draws are whole, whatever the first began in.
                (self.out / "dumps").mkdir(exist_ok=True)
                gs = self.out / "dumps" / f"{rest.strip()}.gs"
                gs.unlink(missing_ok=True)
                pine.gs_dump(gs.with_suffix(".png"), 2)
                deadline = time.monotonic() + 30.0
                size = -1
                while not gs.exists() or gs.stat().st_size != size:
                    size = gs.stat().st_size if gs.exists() else -1
                    if time.monotonic() > deadline:
                        raise TimeoutError("no GS dump written")
                    time.sleep(0.5)
            elif verb == "quit":
                return True
            else:
                print(f"[scenario] unknown step ({verb})")
        self.flush()
        return True


def forms_in(text):
    """The {...} forms and bare words of a step's arguments."""
    depth, start, out = 0, 0, []
    for i, c in enumerate(text):
        if c == "{":
            if depth == 0:
                start = i
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                out.append(text[start:i + 1])
    return out


def run(args):
    out = Path(args.out).resolve() if args.out else ROOT / "runs" / ("pcsx2-" + Path(args.scenario).stem)
    out.mkdir(parents=True, exist_ok=True)
    proc = launch(out / "run.log")
    ok = False
    try:
        pine = Pine()
        wait_game(pine)
        ok = Runner(pine, out).run(Path(args.scenario))
    finally:
        if args.keep:
            print("pcsx2 pid", proc.pid)
        else:
            proc.terminate()
            proc.wait()
    sys.exit(0 if ok else 1)


def evaluate(args):
    print(Pine(timeout=2.0).evaluate(args.script))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("setup").set_defaults(func=setup)
    p = sub.add_parser("boot")
    p.add_argument("--secs", type=float, default=60.0)
    p.set_defaults(func=boot)
    p = sub.add_parser("run")
    p.add_argument("scenario")
    p.add_argument("out", nargs="?")
    p.add_argument("--keep", action="store_true", help="leave PCSX2 running")
    p.set_defaults(func=run)
    p = sub.add_parser("eval")
    p.add_argument("script")
    p.set_defaults(func=evaluate)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
