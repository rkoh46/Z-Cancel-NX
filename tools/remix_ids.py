#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage and character ids of Smash Remix and Smash Remix +EXTRA, read from their sources.

Reads ref/smashremix (tag 2.0.1) and, with --extra, ref/smashremix-plus-extra (tag 0.6.0), and prints
Markdown tables (docs/STAGE_MATRIX.md was filled from this output). Nothing here comes from a ROM.

  Stages: `add_stage(...)` calls in src/Stages.asm are numbered 0x29 upwards in file order; +EXTRA appends
          its extra_stages folders (and their dl/remix variant folders) in sorted order after
          MAX_STAGE_ID, skipping 0xDE (the "random" marker) (character_appender.py).
  Characters: the 21-argument `define_character(...)` calls in src/Character.asm get the next free id in
          file order (0x1B RANDOM, 0x1C is NONE, 0x1D Falco ...); +EXTRA appends its extra_characters
          folders in sorted order.

usage: tools/remix_ids.py [--extra] [--stages|--characters] [--tsv]
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ref")
REMIX = os.path.join(ROOT, "smashremix", "src")
EXTRA = os.path.join(ROOT, "smashremix-plus-extra")

VANILLA_STAGES = [
    "Peach's Castle", "Sector Z", "Congo Jungle", "Planet Zebes", "Hyrule Castle", "Yoshi's Island",
    "Dream Land", "Saffron City", "Mushroom Kingdom", "Dream Land Beta 1", "Dream Land Beta 2",
    "How to Play", "Mini Yoshi's Island", "Meta Crystal", "Duel Zone", "Race to the Finish",
    "Final Destination"]
VANILLA_CHARS = [
    "Mario", "Fox", "Donkey Kong", "Samus", "Luigi", "Link", "Yoshi", "Captain Falcon", "Kirby", "Pikachu",
    "Jigglypuff", "Ness", "Master Hand", "Metal Mario", "Polygon Mario", "Polygon Fox", "Polygon DK",
    "Polygon Samus", "Polygon Luigi", "Polygon Link", "Polygon Yoshi", "Polygon Falcon", "Polygon Kirby",
    "Polygon Pikachu", "Polygon Jigglypuff", "Polygon Ness", "Giant DK"]


def split_args(text):
    """Split a macro argument list on top-level commas (strings and {...} may contain commas)."""
    out, cur, depth, quote = [], "", 0, False
    for ch in text:
        if ch == '"':
            quote = not quote
        if not quote:
            if ch in "({":
                depth += 1
            elif ch in ")}":
                depth -= 1
            elif ch == "," and depth == 0:
                out.append(cur.strip())
                cur = ""
                continue
        cur += ch
    out.append(cur.strip())
    return out


def stage_id_names():
    names = {}
    inside = False
    for line in open(os.path.join(REMIX, "Stages.asm"), encoding="utf-8", errors="replace"):
        if "scope id {" in line:
            inside = True
        elif inside and line.strip() == "}":
            break
        elif inside:
            m = re.match(r"\s*constant (\w+)\(0x([0-9A-Fa-f]+)\)", line)
            if m:
                names.setdefault(int(m.group(2), 16), m.group(1))
    return names


def remix_stages():
    """[(id, constant, title, class, variant_of, variant_type, hazard_type)]"""
    ids = stage_id_names()
    rows = []
    for i, title in enumerate(VANILLA_STAGES):
        rows.append((i, ids.get(i, ""), title, "BATTLE" if i not in (0x0B, 0x0F) else "special", "", "", ""))
    for i in range(0x11, 0x29):
        rows.append((i, ids.get(i, ""), "Break the Targets / Board the Platforms (vanilla)", "BTT/BTP", "", "", ""))
    next_id = 0x29
    for line in open(os.path.join(REMIX, "Stages.asm"), encoding="utf-8", errors="replace"):
        m = re.match(r"\s*add_stage\((.*)\)\s*(//.*)?$", line)
        if not m:
            continue
        a = split_args(m.group(1))
        variant_of = a[14].replace("id.", "") if a[14] != "-1" else ""
        variant = a[15].replace("variant_type.", "") if a[15] != "-1" else ""
        rows.append((next_id, ids.get(next_id, a[0].upper()), a[1].strip('"'), a[10].replace("class.", ""),
                     variant_of, variant, a[24].replace("Hazards.type.", "")))
        next_id += 1
    return rows


def yaml_value(path, key):
    """The few scalar keys needed from a config.yaml, without a YAML dependency."""
    if not os.path.isfile(path):
        return None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*%s:\s*(.*?)\s*(#.*)?$" % re.escape(key), line)
        if m:
            return m.group(1).strip().strip('"').strip("'")
    return None


def extra_stages(first_id):
    base = os.path.join(EXTRA, "extra_stages")
    folders = [f for f in os.listdir(base) if not f.startswith("_") and os.path.isdir(os.path.join(base, f))]
    for f in list(folders):
        for variant in ("dl", "omega", "remix", "remix2", "remix3", "remix4", "remix5", "remix6"):
            if os.path.isdir(os.path.join(base, f, variant)):
                folders.append(f + "/" + variant)
    folders.sort()
    rows, next_id = [], first_id
    for f in folders:
        if next_id == 0xDE:          # the "random" marker: +EXTRA inserts a dummy stage there
            rows.append((next_id, "RANDOM", "(dummy entry: 0xDE means random)", "-", "", "", ""))
            next_id += 1
        cfg = os.path.join(base, f, "config.yaml")
        name = yaml_value(cfg, "name") or f
        parent, _, variant = f.partition("/")
        rows.append((next_id, "STAGE_" + f.upper().replace("/", "_"), name, "BATTLE",
                     parent.upper() if variant else "", variant.upper(), yaml_value(cfg, "hazard_type") or "NONE"))
        next_id += 1
    return rows


def remix_characters():
    """[(id, constant, parent, variant_type)]"""
    rows = [(i, n, "", "") for i, n in enumerate(VANILLA_CHARS)]
    next_id = 0x1B
    for line in open(os.path.join(REMIX, "Character.asm"), encoding="utf-8", errors="replace"):
        m = re.match(r"\s*define_character\((.*)\)\s*(//.*)?$", line)
        if not m:
            continue
        a = split_args(m.group(1))
        if len(a) < 21:
            continue                 # the one-argument form only names a vanilla character
        if next_id == 0x1C:
            rows.append((0x1C, "NONE", "", "(empty slot marker)"))
            next_id += 1
        rows.append((next_id, a[0], a[1], a[20].replace("variant_type.", "")))
        next_id += 1
    return rows


def extra_characters(first_id):
    base = os.path.join(EXTRA, "extra_characters")
    folders = sorted(f for f in os.listdir(base) if not f.startswith("_") and os.path.isdir(os.path.join(base, f)))
    rows = []
    for i, f in enumerate(folders):
        cfg = os.path.join(base, f, "config.yaml")
        rows.append((first_id + i, f.upper(), yaml_value(cfg, "base_character") or "", "+EXTRA"))
    return rows


def table(rows, header, tsv):
    if tsv:
        for r in rows:
            print("\t".join("0x%02X" % c if isinstance(c, int) else str(c) for c in r))
        return
    print("| " + " | ".join(header) + " |")
    print("|" + "---|" * len(header))
    for r in rows:
        print("| " + " | ".join("0x%02X" % c if isinstance(c, int) else str(c) for c in r) + " |")


def main():
    args = set(sys.argv[1:])
    if args - {"--extra", "--stages", "--characters", "--tsv"}:
        sys.exit(__doc__)
    tsv = "--tsv" in args
    if "--characters" not in args:
        rows = remix_stages()
        if "--extra" in args:
            rows = extra_stages(rows[-1][0] + 1)
        table(rows, ["id", "constant", "name", "class", "variant of", "variant", "hazards"], tsv)
    if "--stages" not in args:
        rows = remix_characters()
        if "--extra" in args:
            rows = extra_characters(rows[-1][0] + 1)
        table(rows, ["id", "constant", "parent", "kind"], tsv)


if __name__ == "__main__":
    main()
