#!/usr/bin/env python3
"""Restore missing UI glyphs from an immutable, matching fontconvert archive.

This standard-library-only repair preserves every current glyph and bitmap byte.
Normal full regeneration still uses build-cn-builtin-fonts.sh. The archived
fontconvert output has the same face, size, packing and metrics; all intersecting
glyphs must match before any output is written. No font rasterizer is installed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

REFERENCE = "17c300e4c0abbd3211fd59adb280f0bde3cba640"
ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = Path(__file__).resolve().parent


def parse_font(text, name):
    bitmap_pattern = r"static const uint8_t " + name + r"Bitmaps\[\d+\] = \{(.*?)\n\};"
    glyph_pattern = r"static const EpdGlyph " + name + r"Glyphs\[\] = \{(.*?)\n\};"
    interval_pattern = r"static const EpdUnicodeInterval " + name + r"Intervals\[\] = \{(.*?)\n\};"
    bitmap = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]+)", re.search(bitmap_pattern, text, re.S)[1]))
    rows = [tuple(int(value.strip(), 0) for value in row.split(",")) for row in
            re.findall(r"^\s*\{\s*([\d, -]+)\}", re.search(glyph_pattern, text, re.S)[1], re.M)]
    glyphs = {}
    intervals = re.search(interval_pattern, text, re.S)[1]
    for first, last, index in re.findall(r"\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)\s*\}", intervals):
        first, last, index = int(first, 16), int(last, 16), int(index, 16)
        for cp in range(first, last + 1):
            glyph = rows[index + cp - first]
            if len(glyph) != 7 or glyph[6] + glyph[5] > len(bitmap):
                raise ValueError("Unsupported or truncated raw font")
            glyphs[cp] = glyph
    return bitmap, glyphs


def glyph_content(bitmap, glyph):
    return glyph[:6], bitmap[glyph[6]:glyph[6] + glyph[5]]


def repair_font(text, archive, name, required):
    bitmap, glyphs = parse_font(text, name)
    old_bitmap, old_glyphs = parse_font(archive, name)
    shared = glyphs.keys() & old_glyphs.keys()
    if any(glyph_content(bitmap, glyphs[cp]) != glyph_content(old_bitmap, old_glyphs[cp]) for cp in shared):
        raise ValueError("Archive rasterization differs from current font; refusing repair")
    missing = sorted(required - glyphs.keys())
    if not missing:
        return text, {"added": 0, "existingPreserved": len(glyphs), "archiveIdenticalOverlap": len(shared)}
    output_bitmap = bytearray(bitmap)
    output_glyphs = dict(glyphs)
    for cp in missing:
        glyph = old_glyphs.get(cp)
        if glyph is None or glyph[0] <= 0 or glyph[1] <= 0 or glyph[5] <= 0:
            raise ValueError(f"No archived glyph for U+{cp:04X}")
        pixels = old_bitmap[glyph[6]:glyph[6] + glyph[5]]
        if not any(pixels):
            raise ValueError(f"Empty archived glyph for U+{cp:04X}")
        output_glyphs[cp] = (*glyph[:6], len(output_bitmap))
        output_bitmap.extend(pixels)
    bitmap_lines = ["    " + ", ".join(f"0x{value:02X}" for value in output_bitmap[i:i + 16]) + ","
                    for i in range(0, len(output_bitmap), 16)]
    bitmap_array = f"static const uint8_t {name}Bitmaps[{len(output_bitmap)}] = {{\n" + "\n".join(bitmap_lines) + "\n};"
    original_body = re.search(r"static const EpdGlyph " + name + r"Glyphs\[\] = \{(.*?)\n\};", text, re.S)[1]
    original_lines = re.findall(r"^([ \t]*\{[ \t]*[\d, -]+\},[^\n]*)", original_body, re.M)
    original_by_cp = dict(zip(sorted(glyphs), original_lines))
    if len(original_by_cp) != len(glyphs):
        raise ValueError("Unsupported glyph record layout")
    glyph_array = f"static const EpdGlyph {name}Glyphs[] = {{\n" + "\n".join(
        original_by_cp[cp] if cp in original_by_cp else
        "    { " + ", ".join(map(str, output_glyphs[cp])) + f" }}, // U+{cp:04X}"
        for cp in sorted(output_glyphs)) + "\n};"
    intervals = []
    for index, cp in enumerate(sorted(output_glyphs)):
        if intervals and cp == intervals[-1][1] + 1:
            intervals[-1][1] = cp
        else:
            intervals.append([cp, cp, index])
    interval_array = f"static const EpdUnicodeInterval {name}Intervals[] = {{\n" + "\n".join(
        f"    {{ 0x{first:X}, 0x{last:X}, 0x{index:X} }}," for first, last, index in intervals) + "\n};"
    output = re.sub(r"static const uint8_t " + name + r"Bitmaps\[\d+\] = \{.*?\n\};", lambda _: bitmap_array, text, count=1, flags=re.S)
    output = re.sub(r"static const EpdGlyph " + name + r"Glyphs\[\] = \{.*?\n\};", lambda _: glyph_array, output, count=1, flags=re.S)
    output = re.sub(r"static const EpdUnicodeInterval " + name + r"Intervals\[\] = \{.*?\n\};", lambda _: interval_array, output, count=1, flags=re.S)
    output = re.sub(r"(" + name + r"Intervals,\n\s*)\d+,", lambda match: match[1] + str(len(intervals)) + ",", output, count=1)
    notice = f" * Required UI glyph additions restored by restore-cn-required-glyphs.py from {REFERENCE}.\n"
    output = output.replace(" */\n", notice + " */\n", 1)
    checked_bitmap, checked_glyphs = parse_font(output, name)
    if checked_bitmap[:len(bitmap)] != bitmap or any(
        glyph_content(bitmap, glyph) != glyph_content(checked_bitmap, checked_glyphs[cp]) for cp, glyph in glyphs.items()
    ):
        raise ValueError("Existing glyph content changed")
    return output, {"added": len(missing), "codepoints": [f"U+{cp:04X}" for cp in missing],
                    "existingPreserved": len(glyphs), "archiveIdenticalOverlap": len(shared),
                    "bitmapBytesAdded": len(checked_bitmap) - len(bitmap),
                    "originalBitmapSha256": hashlib.sha256(bitmap).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", type=Path)
    args = parser.parse_args()
    required_chars = set()
    for path in (ROOT / "lib/I18n/translations/chinese.yaml", SCRIPTS / "cn_almanac_chars.txt"):
        required_chars.update(re.findall(r"[\u4E00-\u9FFF]", path.read_text(encoding="utf-8")))
    required = {ord(char) for char in required_chars}
    prepared = []
    record = {"reference": REFERENCE, "fonts": {}}
    for size in (14, 16, 18):
        name = f"notosans_cjk_{size}"
        relative = f"lib/EpdFont/builtinFonts/{name}.h"
        path = ROOT / relative
        archive = subprocess.check_output(["git", "show", f"{REFERENCE}:{relative}"], cwd=ROOT, text=True, encoding="utf-8")
        output, stats = repair_font(path.read_text(encoding="utf-8"), archive, name, required)
        prepared.append((path, output))
        record["fonts"][str(size)] = stats
    # Never remove previous input characters. Equality coverage tests detect any
    # stale extras separately; this repair only adds the missing current UI set.
    charset = SCRIPTS / "cn_i18n_chars.txt"
    existing_chars = set(charset.read_text(encoding="utf-8").strip())
    prepared.append((charset, "".join(sorted(existing_chars | required_chars)) + "\n"))
    for path, output in prepared:
        path.write_text(output, encoding="utf-8", newline="\n")
    if args.record:
        args.record.write_text(json.dumps(record, indent=2), encoding="utf-8")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
