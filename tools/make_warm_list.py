#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the recompiler's warm list of a ROM: assets/warm/warm-<ROM MD5>.bin.

A warm list names the blocks the recompiler compiled in earlier runs (address, length of the source in
bytes, checksum of the source words: no game code), so that the program can compile them ahead of need in
the idle time after each frame instead of in the frame that first runs them (docs/PLAN.md, "Compiling
ahead of need"). The lists for the known ROMs ship in the program's romfs (data/warm/); a console adds
what it meets itself to its own copy in the profile folder.

This runs the Mac harness (build/mac-arm64/znx-bench, builder/build-mac.sh) over
  - a forced four-CPU VS match on every working stage of the ROM (docs/STAGE_MATRIX.md), the fighters
    taken four at a time from the ROM's fighter list so that every fighter plays at least once;
  - the same again (--rounds, 3 in all) with other groups of fighters on each stage;
  - the ROM's input scripts in bench/inputs (the menus on the way to a match, a mashed match and its
    results screen) and a run without input (logo, intro, title, demo fights),
each with the list only recorded (--precompile-ms 0), and merges what they recorded. Addresses and source
words are the game's, the same on every machine that runs the same core, so a list made on a Mac is the
Switch's list too.

usage: tools/make_warm_list.py ROM [--kind vanilla|remix|extra] [--frames N] [--rounds R] [--jobs J] [--out FILE]
       tools/make_warm_list.py --merge OUT IN [IN ...]     merge list files of one ROM
       tools/make_warm_list.py --info FILE [FILE ...]      what a list file holds
The kind is taken from the ROM's MD5 when it is one of the three known ROMs. Never copies ROM bytes.
"""
import argparse
import concurrent.futures
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BENCH = os.path.join(ROOT, "build", "mac-arm64", "znx-bench")
KNOWN = {
    "F7C52568A31AADF26E14DC2B6416B2ED": "vanilla",
    "2B2D6B295106C54216B7FC7A2F14346E": "remix",
    "823DFA3414285939A44FD715413421F6": "extra",
}
# Stages that do not run as a VS match (docs/STAGE_MATRIX.md, sections 3 and 4).
BROKEN = {"vanilla": {0x09, 0x0A, 0x10}, "remix": {0x8B}, "extra": {0x8B}}
VANILLA_STAGES = list(range(0x00, 0x11))
VANILLA_FIGHTERS = list(range(0x00, 0x1B))
# The input scripts of bench/inputs each kind of ROM understands: (file, controller ports, frames).
SCRIPTS = {
    "vanilla": [("ssb-usa-vs-mash.txt", 2, 17000), ("vs-vanilla-castle-2cpu.txt", 2, 4500),
                ("vs-vanilla-dreamland-2cpu.txt", 2, 4500), ("vs-vanilla-hyrule-4cpu.txt", 4, 4500),
                ("vs-vanilla-saffron-4cpu.txt", 4, 4500), ("vs-vanilla-sectorz-4cpu.txt", 4, 4500),
                ("ssb-usa-to-vs-select.txt", 4, 6000)],
    "remix": [("vs-remix-hyrule-4cpu.txt", 4, 4500), ("vs-remix-pokemonstadium2-4cpu.txt", 4, 4500)],
    "extra": [("vs-extra-halberd-4cpu.txt", 4, 4500)],
}


def fnv32(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def read_list(path):
    """(md5, set of (address, length, checksum)); raises ValueError on a damaged file."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 52 or data[:8] != b"ZNXWARM1" or struct.unpack_from("<I", data, 8)[0] != 1:
        raise ValueError(path + ": not a warm list")
    count = struct.unpack_from("<I", data, 12)[0]
    md5 = data[16:48].decode("ascii").upper()
    body = data[48:48 + count * 12]
    if len(data) != 48 + count * 12 + 4 or struct.unpack_from("<I", data, 48 + count * 12)[0] != fnv32(body):
        raise ValueError(path + ": damaged")
    words = struct.unpack("<%dI" % (count * 3), body)
    return md5, set(zip(words[0::3], words[1::3], words[2::3]))


def write_list(path, md5, entries):
    body = b"".join(struct.pack("<III", *e) for e in sorted(entries))
    data = b"ZNXWARM1" + struct.pack("<II", 1, len(entries)) + md5.encode("ascii") + body + struct.pack("<I", fnv32(body))
    with open(path, "wb") as f:
        f.write(data)


def describe(path):
    md5, entries = read_list(path)
    ahead = [e for e in entries if 0x80000000 <= e[0] < 0x80800000 and not e[0] & 3]
    addresses = {e[0] for e in entries}
    words = sum(e[1] for e in entries) // 4
    print("%s: ROM %s, %d entries at %d addresses (%d can be compiled ahead), %d instructions, %d bytes"
          % (path, md5, len(entries), len(addresses), len(ahead), words, os.path.getsize(path)))


def ids_from_remix(extra, what):
    """Ids from tools/remix_ids.py (which reads the Remix sources in ref/): [(id, class)]."""
    command = [sys.executable, os.path.join(ROOT, "tools", "remix_ids.py"), "--" + what, "--tsv"]
    if extra:
        command.insert(2, "--extra")
    out = subprocess.run(command, capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        cells = line.split("\t")
        if cells and re.fullmatch(r"0x[0-9A-Fa-f]+", cells[0]):
            rows.append((int(cells[0], 16), cells[3] if len(cells) > 3 else ""))
    return rows


def stages_and_fighters(kind):
    if kind == "vanilla":
        stages, fighters = VANILLA_STAGES, VANILLA_FIGHTERS
    else:
        stages = [i for i, cls in ids_from_remix(False, "stages") if cls in ("BATTLE", "special")]
        fighters = [i for i, _ in ids_from_remix(False, "characters") if i not in (0x1B, 0x1C)]
        if kind == "extra":
            stages += [i for i, _ in ids_from_remix(True, "stages")]
            fighters += [i for i, _ in ids_from_remix(True, "characters")]
    return [s for s in stages if s not in BROKEN[kind]], fighters


def run_case(rom, args, list_path):
    """One harness run that only records. Returns (md5 or None, compiled, seconds, error text)."""
    command = [BENCH, "--rom", rom, "--hash-every", "0", "--precompile-ms", "0", "--warm", list_path] + args
    begin = time.time()
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return None, 0, time.time() - begin, "timed out"
    md5 = re.search(r'^rom ".*" md5 ([0-9A-Fa-f]{32})', done.stdout, re.M)
    compiled = re.search(r"(\d+) compiled on demand", done.stdout)
    if done.returncode != 0 or md5 is None or not os.path.exists(list_path):
        return None, 0, time.time() - begin, (done.stderr.strip().splitlines() or ["exit %d" % done.returncode])[-1]
    return md5.group(1).upper(), int(compiled.group(1)) if compiled else 0, time.time() - begin, ""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("rom", nargs="?")
    parser.add_argument("--kind", choices=["vanilla", "remix", "extra"])
    parser.add_argument("--frames", type=int, default=7200, help="frames of each forced match (default 7200)")
    parser.add_argument("--rounds", type=int, default=3, help="times every stage is played, with other fighters (default 3)")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    parser.add_argument("--out")
    parser.add_argument("--merge", nargs="+", metavar="FILE")
    parser.add_argument("--info", nargs="+", metavar="FILE")
    options = parser.parse_args()

    if options.info:
        for path in options.info:
            describe(path)
        return 0
    if options.merge:
        if len(options.merge) < 2:
            parser.error("--merge OUT IN [IN ...]")
        md5, merged = None, set()
        for path in options.merge[1:]:
            this_md5, entries = read_list(path)
            if md5 not in (None, this_md5):
                sys.exit("%s is the list of another ROM (%s, not %s)" % (path, this_md5, md5))
            md5 = this_md5
            merged |= entries
        write_list(options.merge[0], md5, merged)
        describe(options.merge[0])
        return 0
    if not options.rom:
        parser.error("a ROM is needed")
    if not os.path.exists(BENCH):
        sys.exit("build/mac-arm64/znx-bench is missing: run builder/build-mac.sh arm64")

    begin = time.time()
    with tempfile.TemporaryDirectory(prefix="znx-warm-") as work:
        # The run without input comes first: it also tells which ROM this is.
        first = os.path.join(work, "intro.bin")
        md5, compiled, seconds, error = run_case(options.rom, ["--frames", "10000"], first)
        if md5 is None:
            sys.exit("the harness could not run this ROM: " + error)
        kind = options.kind or KNOWN.get(md5)
        if kind is None:
            sys.exit("ROM %s is not one of the known three: say --kind vanilla|remix|extra" % md5)
        stages, fighters = stages_and_fighters(kind)
        cases, seen = [], set()
        for r in range(options.rounds):
            for k, stage in enumerate(stages):
                # Round 0: the fighter list four at a time. Later rounds: other pairings on other stages.
                at = 4 * k * (r + 1) + 5 * r
                group = [fighters[(at + port * (r + 1)) % len(fighters)] for port in range(4)]
                name = "vs-%02x-%s" % (stage, "-".join("%02x" % f for f in group))
                if name in seen:
                    continue
                seen.add(name)
                cases.append((name, ["--vs", "0x%02x" % stage, ",".join("0x%02x" % f for f in group),
                                     "--frames", str(options.frames)]))
        for script, ports, frames in SCRIPTS[kind]:
            path = os.path.join(ROOT, "bench", "inputs", script)
            if os.path.exists(path):
                cases.append((script, ["--inputs", path, "--input-players", str(ports), "--frames", str(frames)]))
        cases.append(("boot-to-mode-select.txt", ["--inputs", os.path.join(ROOT, "bench", "inputs", "boot-to-mode-select.txt"),
                                                  "--frames", "3000"]))
        print("ROM %s (%s): %d stages, %d fighters, %d runs on %d processes"
              % (md5, kind, len(stages), len(fighters), len(cases) + 1, options.jobs), flush=True)

        merged = set(read_list(first)[1])
        failed = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=options.jobs) as pool:
            jobs = {pool.submit(run_case, options.rom, args, os.path.join(work, "%d.bin" % n)): (n, name)
                    for n, (name, args) in enumerate(cases)}
            for job in concurrent.futures.as_completed(jobs):
                n, name = jobs[job]
                this_md5, compiled, seconds, error = job.result()
                if this_md5 != md5:
                    failed.append("%s: %s" % (name, error or "another ROM?"))
                    continue
                before = len(merged)
                merged |= read_list(os.path.join(work, "%d.bin" % n))[1]
                if len(jobs) <= 40 or (len(merged) - before) * 20 > compiled:
                    print("  %-40s %5d blocks, %5d new, %.1f s" % (name, compiled, len(merged) - before, seconds), flush=True)
        out = options.out or os.path.join(ROOT, "assets", "warm", "warm-%s.bin" % md5)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        write_list(out, md5, merged)
    for line in failed:
        print("  failed: " + line)
    describe(out)
    print("%d runs (%d failed) in %.0f s" % (len(cases) + 1, len(failed), time.time() - begin))
    return 0


if __name__ == "__main__":
    sys.exit(main())
