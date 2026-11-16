#!/usr/bin/env python3
"""xcstrings_to_json.py -- the Mac catalog becomes the Windows catalog.

    windows_UI/tools/xcstrings_to_json.py [--xcstrings FILE] [--out FILE]
        [--scan-windows] [--write-stats FILE]

The Mac's words live in `app/SpektraLab/Resources/Localizable.xcstrings`
(LOCALIZATION.md): the English literal is the key, and each key holds
`zh-Hans`, `zh-Hant` and `ja`. This converts that catalog into
`windows_UI/assets/localization.json`, the shape `Localization` (C++) and
`tools/localization_check_windows.py` read:

    {"version": 1, "languages": ["en", "zh-Hans", "zh-Hant", "ja"],
     "strings": {"<english key>": {"zh-Hans": "...", "zh-Hant": "...", "ja": "..."}}}

English is never stored: it is the literal itself (the key), byte for byte.
Only values marked `translated` with non-blank text are taken; anything else
would be a silent regression against the Mac's state, so the script fails
instead.

`--scan-windows` reads the Windows sources and reports how today's Chinese
literals map onto the catalog -- the reuse statistics task 6.1 migrates
against. A Windows literal is *reusable* when it equals some key's `zh-Hans`
value exactly (that Mac key becomes the `L()` key and three languages arrive
for free); otherwise the string needs a new key, coined in task 6.1.
"""
import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEFAULT_XCSTRINGS = REPO / "app" / "SpektraLab" / "Resources" / "Localizable.xcstrings"
DEFAULT_OUT = HERE.parent / "assets" / "localization.json"
DEFAULT_SOURCES = HERE.parent
LANGUAGES = ("zh-Hans", "zh-Hant", "ja")
CJK = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\u3040-\u30ff\uac00-\ud7af]")


def convert(xcstrings: Path, out: Path) -> int:
    data = json.loads(xcstrings.read_text(encoding="utf-8"))
    strings: dict = {}
    for key, entry in sorted(data.get("strings", {}).items()):
        values = {}
        for language in LANGUAGES:
            unit = entry.get("localizations", {}).get(language, {}).get("stringUnit", {})
            value = unit.get("value", "")
            if unit.get("state") != "translated" or not value.strip():
                print(f"xcstrings_to_json: {language} of {key!r} is not translated; aborting",
                      file=sys.stderr)
                return 1
            values[language] = value
        strings[key] = values
    catalog = {"version": 1, "languages": ["en", *LANGUAGES], "strings": strings}
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(catalog, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(f"xcstrings_to_json: {len(strings)} keys -> {out}")
    return 0


def string_literals(source: str):
    """Every `"`-literal of a QML/C++ source, comments stripped: (offset, text)."""
    out, i, n = [], 0, len(source)
    while i < n:
        c = source[i]
        if source.startswith("//", i):
            j = source.find("\n", i)
            i = n if j < 0 else j
        elif source.startswith("/*", i):
            j = source.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == '"':
            text, i = [], i + 1
            while i < n and source[i] != '"':
                if source[i] == "\\":
                    text.append({"n": "\n", "t": "\t"}.get(source[i + 1], source[i + 1]))
                    i += 2
                    continue
                text.append(source[i])
                i += 1
            out.append((i, "".join(text)))
            i += 1
        else:
            i += 1
    return out


def scan_windows(sources: Path, catalog: Path) -> list:
    strings = json.loads(catalog.read_text(encoding="utf-8"))["strings"]
    by_hans: dict = {}
    for key, values in strings.items():
        by_hans.setdefault(values["zh-Hans"], []).append(key)
    literals: dict = {}
    for path in sorted(list(sources.glob("qml/*.qml")) + list(sources.glob("src/*.cpp"))
                       + list(sources.glob("src/*.hpp")) + list(sources.glob("src/*.h"))):
        relative = str(path.relative_to(sources)).replace("\\", "/")
        for offset, text in string_literals(path.read_text(encoding="utf-8")):
            if CJK.search(text):
                literals.setdefault(text, []).append(f"{relative}:{source_line(path, offset)}")
    rows = []
    for text, places in sorted(literals.items()):
        keys = by_hans.get(text, [])
        rows.append({"literal": text, "places": places, "mac_keys": keys})
    return rows


def source_line(path: Path, offset: int) -> int:
    return path.read_text(encoding="utf-8").count("\n", 0, offset) + 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--xcstrings", default=str(DEFAULT_XCSTRINGS))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--scan-windows", action="store_true",
                    help="report how today's Windows literals map onto the catalog")
    ap.add_argument("--write-stats", default="",
                    help="with --scan-windows: also write the per-literal table as JSON")
    args = ap.parse_args()
    if args.scan_windows:
        rows = scan_windows(Path(DEFAULT_SOURCES), Path(args.out))
        reusable = [r for r in rows if r["mac_keys"]]
        ambiguous = [r for r in reusable if len(r["mac_keys"]) > 1]
        stats = {"distinct_cjk_literals": len(rows),
                 "reusable_via_mac_zh_hans": len(reusable),
                 "ambiguous_matches": len(ambiguous),
                 "need_new_keys": len(rows) - len(reusable)}
        print(json.dumps(stats, ensure_ascii=False, indent=1))
        for row in rows:
            tag = "reuse " if row["mac_keys"] else "new   "
            where = row["places"][0]
            keys = ",".join(row["mac_keys"][:3])
            print(f"{tag} {where}: {row['literal']!r}" + (f" <- {keys}" if keys else ""))
        if args.write_stats:
            Path(args.write_stats).write_text(
                json.dumps({"stats": stats, "literals": rows}, ensure_ascii=False, indent=1),
                encoding="utf-8")
            print(f"xcstrings_to_json: stats -> {args.write_stats}")
        return 0
    return convert(Path(args.xcstrings), Path(args.out))


if __name__ == "__main__":
    sys.exit(main())
