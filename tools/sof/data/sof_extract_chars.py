#!/usr/bin/env python3
"""
Extracts the character skins/models needed to debug how SoF characters look
from your own pak0.pak into sof_chars.zip (about 20 MB).

Usage (Mac Terminal):
    python3 sof_extract_chars.py "/path/to/Soldier of Fortune/Base/pak0.pak"

Reads the pak only; nothing is changed.
"""
import struct
import sys
import zipfile

WANT_PREFIXES = ("ghoul/comskin/", "ghoul/enemy/bolt/", "ghoul/weapon/", "textures/sprites/",
                 "ghoul/items/projectiles/", "ghoul/objects/generic/chunks_")
WANT_MESO_EXT = (".tga", ".ifl", ".gsq")
WANT_FILES = ("ghoul/enemy/meso/meso_tut1.ghb", "ghoul/enemy/ecto/ecto_tut1.ghb")


def wanted(name):
    n = name.lower()
    if n.startswith(WANT_PREFIXES):
        return True
    for d in ("ghoul/enemy/meso/", "ghoul/enemy/ecto/", "ghoul/enemy/female/"):
        if n.startswith(d) and (n.endswith(WANT_MESO_EXT) or n.endswith("_tut1.ghb")):
            return True
    return n in WANT_FILES


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    with open(sys.argv[1], "rb") as f:
        ident, ofs, length = struct.unpack("<4sii", f.read(12))
        if ident != b"PACK":
            sys.exit("not a pak file")
        f.seek(ofs)
        directory = f.read(length)
        count = 0
        with zipfile.ZipFile("sof_chars.zip", "w", zipfile.ZIP_DEFLATED) as z:
            for i in range(length // 64):
                raw, pos, size = struct.unpack("<56sii", directory[i * 64:i * 64 + 64])
                name = raw.split(b"\0")[0].decode("latin1")
                if not wanted(name):
                    continue
                f.seek(pos)
                z.writestr(name, f.read(size))
                count += 1
    print("wrote sof_chars.zip with %d files" % count)


if __name__ == "__main__":
    main()
