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
import re
import struct
import sys
import zlib

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


def parse_tas_header(path):
    """Extracts the text header of a .tas file (KXT magic + zlib blob:
    [int32 BinSize]["Key": "Value", ... NUL][binary tick data]).
    Returns (header, bin_version): header maps header keys to values,
    bin_version is the binary format version from the tick block (1/2)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 16 or data[0:3] != b"KXT" or data[3:4] not in (b"1", b"2"):
        return {}, 0
    try:
        _, comp_size = struct.unpack_from("<ii", data, 4)
        blob = zlib.decompress(data[12:12 + comp_size])
    except (struct.error, zlib.error):
        return {}, 0
    nul = blob.find(b"\x00", 4)
    if nul < 0:
        return {}, 0
    text = blob[4:nul].decode("utf-8", "replace")
    bin_version = struct.unpack_from("<i", blob, nul + 1)[0] if nul + 5 <= len(blob) else 0
    return dict(re.findall(r'"(\w+)":\s*"([^"]*)"', text)), bin_version


def tas_version_display(raw):
    """Header stores "20" — displayed as "v2.0" (like the client UI)."""
    if re.fullmatch(r"\d+", raw or "") and len(raw) >= 2:
        return f"v{raw[0]}.{raw[1:]}"
    return f"v{raw}" if raw else ""


def update_tas():
    tas_dir = os.path.join(STORAGE_DIR, "tas")
    output = os.path.join(tas_dir, "tas.json")
    items = []
    versions = []
    for name in sorted(os.listdir(tas_dir)):
        if not name.lower().endswith(".tas"):
            continue
        path = os.path.join(tas_dir, name)
        header, bin_version = parse_tas_header(path)
        updated = datetime.date.fromtimestamp(os.path.getmtime(path)).isoformat()
        version_raw = header.get("Version", "")
        if re.fullmatch(r"\d+", version_raw):
            versions.append(int(version_raw))
        # v2 files renamed the header field "Name" to "Description"; v1
        # files keep "Name" and the value is listed as the description.
        if bin_version >= 2:
            description = header.get("Description", "")
        else:
            description = header.get("Name", "")
        items.append({
            "file": name,
            "map": header.get("Map", ""),
            "description": description or os.path.splitext(name)[0],
            "author": header.get("Author", ""),
            "version": tas_version_display(version_raw),
            "created": header.get("Created", updated),
            "updated": updated,
        })

    # Last supported TAS version = the highest header Version among the files.
    last = f"v{max(versions) // 10}.{max(versions) % 10}" if versions else "v0.0"
    data = {
        "list_updated": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
        "last_supported_version": last,
        "items": items,
    }
    with open(output, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")
    return output, len(items)


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
