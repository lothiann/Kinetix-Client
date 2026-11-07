#!/usr/bin/env python3
"""Regenerates the Kinetix storage metadata (storage/*/).

For every substorage (storage/scripts, storage/tas, ...) the tool walks its
files, parses the metadata headers and rewrites the corresponding *.json:
  - storage/scripts/*.lua  -> storage/scripts/scripts.json
  - storage/tas/*.tas      -> storage/tas/tas.json (planned, not yet)

Lua script headers use the same "-- KEY: value" lines the client parses:
NAME, VERSION, CEV, AUTHOR, DESCRIPTION. Run the tool after adding or
changing files, then commit both the files and the updated json.

Usage:
    python storage/update_metadata.py
"""

import datetime
import json
import os
import sys

STORAGE_DIR = os.path.dirname(os.path.abspath(__file__))

# Header keys recognized in the leading "-- KEY: value" block of .lua files.
LUA_KEYS = {"NAME": "name", "VERSION": "version", "CEV": "ce_version",
            "AUTHOR": "author", "DESCRIPTION": "description"}


def parse_lua_header(path):
    """Parses the leading comment header of a Lua script."""
    meta = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            stripped = line.strip()
            if not stripped.startswith("--"):
                break  # header ends at the first non-comment line
            body = stripped[2:].strip()
            for key, field in LUA_KEYS.items():
                if body.upper().startswith(key + ":"):
                    value = body[len(key) + 1:].strip()
                    if field == "ce_version":
                        try:
                            meta[field] = int(value.lstrip("vV"))
                        except ValueError:
                            pass
                    else:
                        meta[field] = value
                    break
    return meta


def update_scripts():
    scripts_dir = os.path.join(STORAGE_DIR, "scripts")
    output = os.path.join(scripts_dir, "scripts.json")
    scripts = []
    for name in sorted(os.listdir(scripts_dir)):
        if not name.lower().endswith(".lua"):
            continue
        path = os.path.join(scripts_dir, name)
        meta = parse_lua_header(path)
        updated = datetime.date.fromtimestamp(os.path.getmtime(path)).isoformat()
        scripts.append({
            "file": name,
            "name": meta.get("name", os.path.splitext(name)[0]),
            "version": meta.get("version", ""),
            "ce_version": meta.get("ce_version", 1),
            "author": meta.get("author", ""),
            "description": meta.get("description", ""),
            "updated": updated,
        })

    # Last supported CE version = the highest ce_version among the scripts.
    last_ce = max((s["ce_version"] for s in scripts), default=1)
    data = {
        "list_updated": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
        "last_supported_version": f"v{last_ce}",
        "scripts": scripts,
    }
    with open(output, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")
    return output, len(scripts)


def update_tas():
    # TODO: walk storage/tas/*.tas and rewrite tas.json once the client
    # gains the TAS repository UI.
    return None, 0


def main():
    total = 0
    out, count = update_scripts()
    print(f"wrote {out}: {count} script(s)")
    total += count
    out, count = update_tas()
    if out:
        print(f"wrote {out}: {count} item(s)")
        total += count
    return 0


if __name__ == "__main__":
    sys.exit(main())
