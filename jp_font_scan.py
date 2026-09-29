#!/usr/bin/env python3
"""
jp_font_scan.py - collect every glyph the Japanese UI needs and print the
Range / Symbols strings for the LVGL font converter (lv_font_conv or
https://lvgl.io/tools/fontconverter).

Run from the repo root:
    python jp_font_scan.py
    python jp_font_scan.py components/weight_ui/i18n/lang_ja.h   # just one file

By default it scans lang_ja.h AND every .c/.h under components/weight_ui,
so Japanese text hard-coded in a screen (not via TR()) is caught too.
Only characters inside string literals are collected, so comments written
in Japanese don't bloat the font.
"""

import re
import sys
from pathlib import Path

FONT_TTF = "NotoSansJP-Bold.ttf"  # path to your TTF/OTF
SIZES = [14, 16, 24]  # lv_font_nato_sans_jp_bold_<size>
NAME = "lv_font_nato_sans_jp_bold_{size}"
BPP = 4

# Always include printable ASCII: the JP font also renders digits, "MQTT",
# "WiFi", "UUID", "%u", units, etc. that appear inside Japanese strings.
ASCII_RANGE = (0x20, 0x7E)

# Handy extras that commonly show up later (fullwidth punctuation, dots).
# Set to [] if you want the font strictly minimal.
EXTRA = "、。・ー「」（）：！？～"

STRING_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def files_to_scan(args):
    if args:
        return [Path(a) for a in args]
    root = Path("components/weight_ui")
    if not root.is_dir():
        sys.exit("run from the repo root, or pass file paths")
    return sorted(p for p in root.rglob("*") if p.suffix in (".c", ".h"))


def collect(files):
    chars = {}
    for f in files:
        text = f.read_text(encoding="utf-8", errors="replace")
        # drop comments so Japanese in comments isn't counted
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
        for lit in STRING_RE.findall(text):
            for ch in lit:
                if ord(ch) > 0x7E:
                    chars.setdefault(ch, set()).add(f.name)
    for ch in EXTRA:
        chars.setdefault(ch, set()).add("(extra)")
    return chars


def to_ranges(codepoints):
    cps = sorted(set(codepoints))
    out, start, prev = [], None, None
    for cp in cps:
        if start is None:
            start = prev = cp
        elif cp == prev + 1:
            prev = cp
        else:
            out.append((start, prev))
            start = prev = cp
    if start is not None:
        out.append((start, prev))
    return out


def fmt_range(r):
    a, b = r
    return "0x%04X" % a if a == b else "0x%04X-0x%04X" % (a, b)


def main():
    files = files_to_scan(sys.argv[1:])
    chars = collect(files)
    if "\ufffd" in chars:
        print(
            "WARNING: some file isn't UTF-8 (found U+FFFD). Save it as UTF-8 first.\n"
        )

    non_ascii = sorted(chars, key=ord)
    kanji = [c for c in non_ascii if 0x4E00 <= ord(c) <= 0x9FFF]
    kana = [c for c in non_ascii if 0x3040 <= ord(c) <= 0x30FF]
    other = [c for c in non_ascii if c not in kanji and c not in kana]

    ranges = [ASCII_RANGE] + to_ranges(ord(c) for c in non_ascii)
    range_str = ",".join(fmt_range(r) for r in ranges)
    symbols = "".join(non_ascii)

    print("Scanned %d file(s)" % len(files))
    print(
        "Unique non-ASCII glyphs: %d  (kanji %d, kana %d, other %d)\n"
        % (len(non_ascii), len(kanji), len(kana), len(other))
    )

    print("=== Kanji ===")
    print("".join(kanji) or "-")
    print("=== Kana ===")
    print("".join(kana) or "-")
    print("=== Other ===")
    print(" ".join("%s(U+%04X)" % (c, ord(c)) for c in other) or "-")

    print("\n=== RANGE (paste into converter 'Range' field; covers ASCII + all JP) ===")
    print(range_str)

    print("\n=== SYMBOLS (alternative: Range 0x20-0x7E + these in 'Symbols') ===")
    print(symbols)

    print("\n=== lv_font_conv commands ===")
    for size in SIZES:
        name = NAME.format(size=size)
        print(
            'npx lv_font_conv --font "%s" --size %d --bpp %d --format lvgl '
            '--no-compress --lv-include "lvgl.h" --lv-font-name %s '
            "-r %s -o %s.c" % (FONT_TTF, size, BPP, name, range_str, name)
        )

    # which file uses each glyph - useful when a glyph looks unexpected
    Path("jp_glyphs_report.txt").write_text(
        "\n".join(
            "%s U+%04X  %s" % (c, ord(c), ", ".join(sorted(chars[c])))
            for c in non_ascii
        ),
        encoding="utf-8",
    )
    print("\n(per-glyph file list written to jp_glyphs_report.txt)")


if __name__ == "__main__":
    main()
