#!/usr/bin/env python3
"""Exports a GFWL title's SPA (XDBF) into the pieces a Steamworks setup needs.

Usage: spa2steam.py <Game.exe | title.spa> <output dir> [--ach-format ACH_%u] [--lb-format LB_%u]

Writes achievements.csv, leaderboards.csv, presence.txt, icons/*.png and a starting
xlive_steamworks.json into the output directory.
"""

import csv
import gzip
import io
import json
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET

XDBF = 0x58444246
XACH = 0x58414348
XSTR = 0x58535452
XVC2 = 0x58564332
XSRC = 0x58535243
XTHD = 0x58544844
XSTC = 0x58535443

LANGUAGES = {
    1: "english", 2: "japanese", 3: "german", 4: "french", 5: "spanish", 6: "italian",
    7: "korean", 8: "tchinese", 9: "portuguese", 10: "schinese", 11: "polish", 12: "russian",
}

DATA_TYPES = {0: "context", 1: "int32", 2: "int64", 3: "double", 4: "unicode", 5: "float", 6: "binary", 7: "datetime"}


# --- PE resource extraction --------------------------------------------------------------------

def read_pe_resource(data, type_name, res_name):
    """Returns the bytes of the named RT_RCDATA-style resource, or None."""
    if data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    machine, sections, _, _, _, optsize, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dd_offset = opt + (96 if magic == 0x10B else 112)
    rsrc_rva, rsrc_size = struct.unpack_from("<II", data, dd_offset + 2 * 8)
    if not rsrc_rva:
        return None
    section_table = opt + optsize
    rsrc_file = None
    for i in range(sections):
        name, vsize, va, rawsize, raw = struct.unpack_from("<8sIIII", data, section_table + i * 40)
        if va <= rsrc_rva < va + max(vsize, rawsize):
            rsrc_file = raw + (rsrc_rva - va)
            rsrc_va = va
            rsrc_raw = raw
            break
    if rsrc_file is None:
        return None

    def rva_to_file(rva):
        return rsrc_raw + (rva - rsrc_va)

    def read_name(offset):
        length = struct.unpack_from("<H", data, rsrc_file + offset)[0]
        return data[rsrc_file + offset + 2:rsrc_file + offset + 2 + length * 2].decode("utf-16-le")

    def entries(dir_offset):
        _, _, _, _, named, ids = struct.unpack_from("<IIHHHH", data, rsrc_file + dir_offset)
        result = []
        for i in range(named + ids):
            name_field, off = struct.unpack_from("<II", data, rsrc_file + dir_offset + 16 + i * 8)
            if name_field & 0x80000000:
                key = read_name(name_field & 0x7FFFFFFF)
            else:
                key = name_field
            result.append((key, off))
        return result

    def find(dir_offset, wanted):
        for key, off in entries(dir_offset):
            if isinstance(key, str) and key.upper() == wanted.upper():
                return off
        return None

    type_dir = find(0, type_name)
    if type_dir is None:
        return None
    name_dir = find(type_dir & 0x7FFFFFFF, res_name)
    if name_dir is None:
        return None
    lang_entries = entries(name_dir & 0x7FFFFFFF)
    if not lang_entries:
        return None
    leaf = lang_entries[0][1]
    rva, size, _, _ = struct.unpack_from("<IIII", data, rsrc_file + leaf)
    start = rva_to_file(rva)
    return data[start:start + size]


# --- XDBF ---------------------------------------------------------------------------------------

class Spa:
    def __init__(self, blob):
        if struct.unpack_from(">I", blob, 0)[0] != XDBF:
            raise ValueError("not an XDBF file")
        _, _, table_len, count, free_len, _ = struct.unpack_from(">IIIIII", blob, 0)
        body = 24 + table_len * 18 + free_len * 8
        self.blob = blob
        self.entries = []
        for i in range(count):
            ns, ident, offset, length = struct.unpack_from(">HQII", blob, 24 + i * 18)
            self.entries.append((ns, ident, body + offset, length))
        self.title_id = 0
        self.default_language = 1
        self.strings = {}       # language -> {id: text}
        self.achievements = []  # dicts
        self.images = {}        # id -> png bytes
        self.views = {}         # view id -> {"name_id", "columns": [...]}
        self.view_names = {}
        self.property_names = {}
        self.presence_modes = {}
        self.context_values = {}
        self.context_defaults = {}
        self.queries = {}
        self._parse()

    def section(self, ns, ident):
        for e in self.entries:
            if e[0] == ns and e[1] == ident:
                return self.blob[e[2]:e[2] + e[3]]
        return None

    def _parse(self):
        blob = self.blob
        for ns, ident, offset, length in self.entries:
            data = blob[offset:offset + length]
            if ns == 1 and ident == XTHD and length >= 16:
                self.title_id = struct.unpack_from(">I", data, 12)[0]
            elif ns == 1 and ident == XSTC and length >= 16:
                self.default_language = struct.unpack_from(">I", data, 12)[0]
            elif ns == 2:
                self.images[ident] = data
            elif ns == 3:
                self.strings[ident] = self._parse_xstr(data)
        xach = self.section(1, XACH)
        if xach:
            count = struct.unpack_from(">H", xach, 12)[0]
            for i in range(count):
                base = 14 + i * 36
                aid, label, desc, unach, image, cred, _, flags = struct.unpack_from(">HHHHIHHI", xach, base)
                self.achievements.append({
                    "id": aid, "label": label, "description": desc, "unachieved": unach,
                    "image": image, "cred": cred, "flags": flags,
                })
        xsrc = self.section(1, XSRC)
        if xsrc:
            self._parse_xsrc(xsrc)
        xvc2 = self.section(1, XVC2)
        if xvc2:
            self._parse_xvc2(xvc2)

    @staticmethod
    def _parse_xstr(data):
        result = {}
        count = struct.unpack_from(">H", data, 12)[0]
        pos = 14
        for _ in range(count):
            sid, length = struct.unpack_from(">HH", data, pos)
            pos += 4
            result[sid] = data[pos:pos + length].decode("utf-8", "replace")
            pos += length
        return result

    def _parse_xsrc(self, data):
        filename_length = struct.unpack_from(">I", data, 12)[0]
        pos = 16 + filename_length
        uncompressed, compressed = struct.unpack_from(">II", data, pos)
        pos += 8
        try:
            xml_bytes = gzip.decompress(data[pos:pos + compressed])
        except Exception as error:
            print("XSRC: inflate failed:", error)
            return
        if xml_bytes[:2] == b"\xff\xfe" or (len(xml_bytes) > 1 and xml_bytes[1] == 0):
            text = xml_bytes.decode("utf-16-le", "replace")
        else:
            text = xml_bytes.decode("utf-8", "replace")
        text = text.lstrip("﻿")
        # XLAST files carry bare ampersands and control characters that a strict parser rejects.
        text = re.sub(r"&(?!amp;|lt;|gt;|quot;|apos;|#)", "&amp;", text)
        text = re.sub(r"[^\x09\x0A\x0D\x20-퟿-�]", "", text)
        try:
            root = ET.fromstring(text)
        except ET.ParseError as error:
            print("XSRC: XML parse failed:", error)
            return
        for element in root.iter():
            tag = element.tag.split("}")[-1]
            if tag == "StatsView" and element.get("friendlyName"):
                self.view_names[int(element.get("id"), 0)] = element.get("friendlyName")
            elif tag == "Property" and element.get("friendlyName"):
                self.property_names[int(element.get("id"), 0)] = element.get("friendlyName")
            elif tag == "PresenceMode":
                self.presence_modes[int(element.get("contextValue"), 0)] = int(element.get("stringId"), 0)
            elif tag == "Context":
                cid = int(element.get("id"), 0)
                if element.get("defaultValue") is not None:
                    self.context_defaults[cid] = int(element.get("defaultValue"), 0)
                values = self.context_values.setdefault(cid, {})
                for child in element:
                    if child.tag.split("}")[-1] == "ContextValue":
                        values[int(child.get("value"), 0)] = int(child.get("stringId"), 0)
            elif tag == "Query":
                self.queries[int(element.get("id"), 0)] = element

    def _parse_xvc2(self, data):
        pos = 12
        shared_count = struct.unpack_from(">H", data, pos)[0]
        pos += 2
        shared = []
        for _ in range(shared_count):
            column_count, row_count = struct.unpack_from(">HH", data, pos)
            pos += 12
            columns = []
            for _ in range(column_count):
                size, prop, flags, attr, sid, agg, ordinal, ftype, fmt = struct.unpack_from(">IIIHHHBBI", data, pos)
                pos += 32
                columns.append({
                    "property_id": prop, "column_id": prop & 0x7FFF, "type": (prop >> 28) & 0xF,
                    "system": bool(prop & 0x8000), "string_id": sid, "ordinal": ordinal,
                })
            pos += row_count * 32
            _, _, _, contexts, properties = struct.unpack_from(">IIIII", data, pos)
            pos += 20 + (contexts + properties) * 4
            columns.sort(key=lambda c: c["ordinal"])
            shared.append(columns)
        view_count = struct.unpack_from(">H", data, pos)[0]
        pos += 2
        for _ in range(view_count):
            vid, flags, shared_index, sid = struct.unpack_from(">IIHH", data, pos)
            pos += 16
            if shared_index < len(shared):
                self.views[vid] = {"string_id": sid, "columns": shared[shared_index]}

    def text(self, sid, language=None):
        language = language or self.default_language
        table = self.strings.get(language) or (next(iter(self.strings.values())) if self.strings else {})
        return table.get(sid, "")


# --- Export -------------------------------------------------------------------------------------

def sanitize(name):
    return "".join(c if c.isalnum() or c in "_-" else "_" for c in name)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    source, out = argv[1], argv[2]
    ach_format = "ACH_%u"
    lb_format = "LB_%u"
    args = argv[3:]
    for i, a in enumerate(args):
        if a == "--ach-format" and i + 1 < len(args):
            ach_format = args[i + 1]
        if a == "--lb-format" and i + 1 < len(args):
            lb_format = args[i + 1]

    with open(source, "rb") as f:
        data = f.read()
    blob = data if data[:4] == b"XDBF" else read_pe_resource(data, "RT_RCDATA", "SPAFILE")
    if not blob:
        print("no SPAFILE resource found in", source)
        return 1
    spa = Spa(blob)
    os.makedirs(os.path.join(out, "icons"), exist_ok=True)
    languages = sorted(spa.strings.keys())

    def ach_name(aid):
        return ach_format.replace("%u", str(aid)).replace("%d", str(aid))

    def lb_name(vid):
        return lb_format.replace("%u", str(vid)).replace("%d", str(vid))

    with open(os.path.join(out, "achievements.csv"), "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        header = ["id", "api_name", "gamerscore", "hidden", "type", "image_id"]
        for lang in languages:
            tag = LANGUAGES.get(lang, str(lang))
            header += [f"name_{tag}", f"description_{tag}", f"unachieved_{tag}"]
        writer.writerow(header)
        for a in spa.achievements:
            hidden = 0 if a["flags"] & 0x8 else 1
            row = [a["id"], ach_name(a["id"]), a["cred"], hidden, a["flags"] & 7, a["image"]]
            for lang in languages:
                row += [spa.text(a["label"], lang), spa.text(a["description"], lang), spa.text(a["unachieved"], lang)]
            writer.writerow(row)
            png = spa.images.get(a["image"])
            if png:
                with open(os.path.join(out, "icons", sanitize(ach_name(a["id"])) + ".png"), "wb") as icon:
                    icon.write(png)

    with open(os.path.join(out, "leaderboards.csv"), "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["view_id", "api_name", "view_name", "column_id", "column_name", "type", "is_rating", "system"])
        for vid, view in sorted(spa.views.items()):
            view_name = spa.view_names.get(vid) or spa.text(view["string_id"])
            first = True
            for c in view["columns"]:
                if c["system"]:
                    continue
                column_name = spa.property_names.get(c["property_id"]) or spa.text(c["string_id"])
                writer.writerow([vid, lb_name(vid), view_name, c["column_id"], column_name, DATA_TYPES.get(c["type"], c["type"]), int(first), 0])
                first = False

    with open(os.path.join(out, "presence.txt"), "w", encoding="utf-8") as f:
        f.write(f"title id 0x{spa.title_id:08X}  name: {spa.text(0x8000)}\n\n")
        f.write("Presence modes (X_CONTEXT_PRESENCE value -> format):\n")
        for value, sid in sorted(spa.presence_modes.items()):
            f.write(f"  {value}: {spa.text(sid)!r}\n")
        f.write("\nContext values:\n")
        for cid, values in sorted(spa.context_values.items()):
            f.write(f"  context 0x{cid:08X} (default {spa.context_defaults.get(cid, 'none')}):\n")
            for value, sid in sorted(values.items()):
                f.write(f"    {value}: {spa.text(sid)!r}\n")
        f.write("\nMatchmaking queries (procedure index -> filters):\n")
        for qid, element in sorted(spa.queries.items()):
            f.write(f"  query {qid}:\n")
            for filt in element.iter():
                if filt.tag.split("}")[-1] == "Filter":
                    f.write(f"    {filt.attrib}\n")

    config = {
        "title": {"title_id": spa.title_id},
        "achievements": {"name_format": ach_format, "map": {str(a["id"]): ach_name(a["id"]) for a in spa.achievements}},
        "leaderboards": {
            "name_format": lb_format,
            "map": {},
        },
    }
    for vid, view in sorted(spa.views.items()):
        columns = [c for c in view["columns"] if not c["system"]]
        if not columns:
            continue
        config["leaderboards"]["map"][str(vid)] = {
            "name": lb_name(vid),
            "rating_column": columns[0]["column_id"],
            "detail_columns": [c["column_id"] for c in columns[1:]],
            "keep_best": True,
            "ascending": False,
            "display_type": 1,
        }
    with open(os.path.join(out, "xlive_steamworks.json"), "w", encoding="utf-8") as f:
        json.dump(config, f, indent=2)

    print(f"title 0x{spa.title_id:08X} '{spa.text(0x8000)}': {len(spa.achievements)} achievements, "
          f"{len(spa.views)} stats views, {len(spa.images)} images, {len(languages)} languages -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
