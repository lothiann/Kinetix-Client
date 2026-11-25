#!/usr/bin/env python3
"""Regenerates the Kinetix storage metadata (storage/*/).

For every substorage (storage/scripts, storage/tas, ...) the tool walks its
files, parses the metadata headers and rewrites the corresponding *.json:
  - storage/scripts/*.lua  -> storage/scripts/scripts.json
  - storage/tas/*.tas      -> storage/tas/tas.json

Lua script headers use the same "-- KEY: value" lines the client parses:
NAME, VERSION, CEV, AUTHOR, DESCRIPTION. Run the tool after adding or
changing files, then commit both the files and the updated json.

Usage:
    python storage/update_metadata.py
"""

import datetime
import json
import math
import os
import re
import struct
import sys
import zlib

STORAGE_DIR = os.path.dirname(os.path.abspath(__file__))
TAS_HEADER = os.path.join(STORAGE_DIR, "..", "src", "game", "client",
                          "components", "kinetix", "tas.h")

# TuneZone::NUM in engine/shared/protocol.h; CTuningParams::Num() counts the
# CTuneParam fields generated from game/tuning.h (47 int-sized fields).
TAS_TUNING_ZONES = 256
TAS_TUNING_PARAMS = 47
TAS_TIMELINE_MAX_TUNING_PARAMS = 64

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


def native_tas_version():
    with open(TAS_HEADER, encoding="utf-8") as f:
        match = re.search(r"\bNATIVE_FILE_VERSION\s*=\s*(\d+)\s*;", f.read())
    if not match:
        raise ValueError("NATIVE_FILE_VERSION not found in tas.h")
    return int(match.group(1))


def parse_tas_v3(blob, native_version):
    """Read the compact V3 header and dummy slots from the tick stream."""
    offset = 0

    def take(size):
        nonlocal offset
        if size < 0 or offset + size > len(blob):
            raise ValueError("truncated TAS")
        result = blob[offset:offset + size]
        offset += size
        return result

    def number(fmt):
        return struct.unpack(fmt, take(struct.calcsize(fmt)))[0]

    def string():
        return take(number("<H")).decode("utf-8", "replace")

    def flag():
        value = number("<B")
        if value > 1:
            raise ValueError("invalid boolean")
        return value

    def floats(count):
        if not all(math.isfinite(value) for value in struct.unpack("<" + "f" * count, take(4 * count))):
            raise ValueError("invalid float")

    def fixed_string(capacity):
        length = number("<H")
        if length >= capacity:
            raise ValueError("invalid fixed string")
        return take(length)

    def settings():
        mode, entity = struct.unpack("<ii", take(8))
        fixed_string(64)
        fixed_string(64)
        take(4)
        for capacity in (64, 8, 8):
            fixed_string(capacity)
        take(12)
        flag()
        flag()
        floats(2)
        take(8)
        floats(2)
        take(16)
        for _ in range(3):
            flag()
        take(36)
        flag()
        take(4)
        floats(2)
        flag()
        take(4)
        return mode, entity

    def spawner_record():
        tick, mode, entity, _tile = struct.unpack("<4i", take(16))
        flag()
        floats(2)
        settings_mode, _settings_entity = settings()
        if mode not in (0, 1) or settings_mode != mode:
            raise ValueError("invalid spawner record")
        return tick, mode

    def projectile():
        floats(4)
        take(8)
        flag()
        take(4)
        flag()
        take(4)
        flag()
        take(8)

    version = number("<H")
    if version != native_version or version // 10 != 3:
        raise ValueError("unsupported TAS version")
    ticks, spawners, chats, rcons, starts = struct.unpack("<5I", take(20))
    if not 0 < ticks <= 2000000 or max(spawners, chats, rcons) > 100000 or starts > 128:
        raise ValueError("invalid TAS counts")
    practice = take(6)  # Practice, invincible, super, paused, spec, tuning dirty.
    if any(value > 1 for value in practice):
        raise ValueError("invalid practice flags")
    take(TAS_TUNING_ZONES * TAS_TUNING_PARAMS * 4)
    finish = number("<I")
    map_name, description, author, created = (string() for _ in range(4))
    slots = {0}
    for _ in range(ticks):
        take(76)  # Input and core position, velocity, hook and weapon.
        for _ in range(number("<B")):
            slot = number("<B")
            if not 0 < slot < 128:
                raise ValueError("invalid dummy slot")
            slots.add(slot)
            take(48)  # Input and position.
        for _ in range(number("<B")):
            slot = number("<B")
            if not 0 < slot < 128:
                raise ValueError("invalid dummy slot")
            take(9)  # Hook state and position.
        take(number("<I") * 43)  # Projectile.

    # Skip the remaining records using CKxTasCompactWriter's field order.
    for _ in range(spawners):
        take(16)
        flag()
        floats(2)
        settings()
    for _ in range(chats):
        take(8)
        string()
        string()
    for _ in range(rcons):
        string()
    start_slots = set()
    for _ in range(starts):
        slot = number("<I")
        if slot >= 128 or slot in start_slots:
            raise ValueError("invalid start slot")
        start_slots.add(slot)
        slots.add(slot)
        take(64)
    snapshot_tick, snapshot_count = struct.unpack("<II", take(8))
    if snapshot_tick > 0x7fffffff - ticks:
        raise ValueError("invalid snapshot tick")
    if snapshot_count > 100000 or snapshot_count > (len(blob) - offset) // 22:
        raise ValueError("invalid snapshot count")
    for _ in range(snapshot_count):
        entity, entity_id, _tick = struct.unpack("<iii", take(12))
        if not 0 <= entity < 7:
            raise ValueError("invalid snapshot entity")
        floats(2)
        flag()
        mode, settings_entity = settings()
        if mode != 0 or settings_entity != entity:
            raise ValueError("invalid snapshot settings")
        if entity == 0:  # Tee: network core and character state.
            values = struct.unpack("<18i", take(72))
            if not 0 <= entity_id < 128 or not 0 <= values[14] < 6 or not -1 <= values[10] < 128:
                raise ValueError("invalid snapshot tee")
            floats(4)
        elif entity == 1:
            projectile()
        elif entity == 2:
            floats(4)
            take(4)
            flag()
            take(16)
            flag()
            take(4)
            floats(5)
            take(8)
            flag()
    snapshot_end = snapshot_tick + ticks
    spawner_events = number("<I")
    if spawner_events > 100000:
        raise ValueError("invalid spawner event count")
    ordered_events = []
    previous_tick = snapshot_tick
    previous_order = -1
    for _ in range(spawner_events):
        kind = number("<B")
        order = number("<I")
        event_tick, mode = spawner_record()
        if (kind > 2 or not snapshot_tick <= event_tick <= snapshot_end or
                event_tick < previous_tick or order <= previous_order):
            raise ValueError("invalid spawner event")
        previous_tick, previous_order = event_tick, order
        ordered_events.append((order, event_tick))

    timeline_events = number("<I")
    if timeline_events > 100000:
        raise ValueError("invalid timeline event count")
    previous_tick = snapshot_tick
    previous_order = -1
    practice_event = False
    rcon_event = False
    for _ in range(timeline_events):
        kind = number("<B")
        event_tick, order, active_dummy, team, client_id = struct.unpack("<5I", take(20))
        has_cursor = flag()
        floats(2)
        value = flag()
        text = fixed_string(512)
        signed_client_id = client_id if client_id < 0x80000000 else client_id - 0x100000000
        if (kind > 4 or not snapshot_tick <= event_tick <= snapshot_end or
                event_tick < previous_tick or order <= previous_order or
                active_dummy >= 128):
            raise ValueError("invalid timeline event")
        if kind == 2 and (team > 1 or signed_client_id < -3 or signed_client_id >= 128):
            raise ValueError("invalid chat line event")
        if has_cursor and kind not in (0, 1):
            raise ValueError("unexpected cursor in timeline event")
        if kind in (0, 1) and not text:
            raise ValueError("empty command event")
        if kind == 4:
            zone, count = struct.unpack("<II", take(8))
            if (count > TAS_TIMELINE_MAX_TUNING_PARAMS or
                    (count == 0 and zone != 0xffffffff) or
                    (count != 0 and (count != TAS_TUNING_PARAMS or zone >= TAS_TUNING_ZONES))):
                raise ValueError("invalid tuning event")
            take(count * 4)
        previous_tick, previous_order = event_tick, order
        ordered_events.append((order, event_tick))
        rcon_event |= kind == 1
        practice_event |= kind == 3 or (kind in (0, 1) and value)

    ordered_events.sort()
    if any(ordered_events[i][0] == ordered_events[i - 1][0] or
           ordered_events[i][1] < ordered_events[i - 1][1]
           for i in range(1, len(ordered_events))):
        raise ValueError("timeline order mismatch")
    if offset != len(blob) or (finish != 0xffffffff and not 0 < finish <= ticks):
        raise ValueError("invalid TAS payload")
    used = []
    if spawners or snapshot_count or spawner_events:
        used.append("Spawner")
    if rcons or rcon_event:
        used.append("RCON")
    if any(practice[:5]) or practice_event:
        used.append("Practice")
    return {
        "Map": map_name, "Description": description, "Author": author,
        "Created": created, "Version": str(version), "Ticks": str(ticks),
        "Dummies": len(slots), "Used": ", ".join(used) or None,
        "FinishTicks": None if finish == 0xffffffff else finish,
    }, 3


def parse_tas_header(path, native_version=None):
    """Extracts the text header of a .tas file (KXT magic + zlib blob:
    [int32 BinSize]["Key": "Value", ... NUL][binary tick data]).
    Returns (header, bin_version): header maps header keys to values,
    bin_version is the binary format version from the tick block (1/2)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 16 or data[0:3] != b"KXT" or data[3:4] not in (b"1", b"2", b"3"):
        return {}, 0
    try:
        raw_size, comp_size = struct.unpack_from("<ii", data, 4)
        blob = zlib.decompress(data[12:12 + comp_size])
        if data[3:4] == b"3":
            if comp_size <= 0 or comp_size != len(data) - 12 or raw_size != len(blob):
                return {}, 0
            if native_version is None:
                native_version = native_tas_version()
            return parse_tas_v3(blob, native_version)
    except (struct.error, zlib.error, ValueError):
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
    native_version = native_tas_version()
    for name in sorted(os.listdir(tas_dir)):
        if not name.lower().endswith(".tas"):
            continue
        path = os.path.join(tas_dir, name)
        header, bin_version = parse_tas_header(path, native_version)
        updated = datetime.date.fromtimestamp(os.path.getmtime(path)).isoformat()
        version_raw = header.get("Version", "")
        version = int(version_raw) if re.fullmatch(r"\d+", version_raw) else 0
        if version:
            versions.append(version)
        # v2 files renamed the header field "Name" to "Description"; v1
        # files keep "Name" and the value is listed as the description.
        if bin_version >= 2:
            description = header.get("Description", "")
        else:
            description = header.get("Name", "")
        items.append({
            "file": name,
            "map": header.get("Map", ""),
            "description": description,
            "author": header.get("Author", ""),
            "version": version,
            "dummies": header.get("Dummies"),
            "used": header.get("Used"),
            "ticks": int(header["Ticks"]) if header.get("Ticks", "").isdigit() else None,
            "finish_ticks": header.get("FinishTicks"),
            "updated": updated,
            "created": header.get("Created", updated),
        })

    # Last supported TAS version = the highest header Version among the files.
    last = max(versions, default=0)
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
