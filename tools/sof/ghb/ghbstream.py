#!/usr/bin/env python3
"""
ghbstream.py - parse the metadata stream of a SoF .ghb file (clean-room, written from
observing the file layout and the sequence of reads the original loader performs).

Container: u32 magic 0x198237FE, u32 a, u32 b; stream = file[12:a]; bulk block = file[a:a+b].
Stream:    u32 0x033FB1A3, u32 version (74), u32 thunk_flag(0), string src, ...
Primitives: int(4) bool(1) vec3(12) string(int len + bytes) raw(n)
"""
import struct, sys


class Stream:
    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def need(self, n):
        if self.p + n > len(self.d):
            raise EOFError("read past end at %d (+%d of %d)" % (self.p, n, len(self.d)))

    def int(self):
        self.need(4)
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def bool(self):
        self.need(1)
        v = self.d[self.p] == 1
        self.p += 1
        return v

    def vec3(self):
        self.need(12)
        v = struct.unpack_from("<3f", self.d, self.p)
        self.p += 12
        return v

    def raw(self, n):
        if n < 0:
            raise ValueError("negative raw size %d at %d" % (n, self.p))
        self.need(n)
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def str(self):
        n = self.int()
        if n < 0 or n > 100000:
            raise ValueError("bad string length %d at %d" % (n, self.p - 4))
        return self.raw(n).decode("latin1")

    # helper shapes seen repeatedly in the loader
    def const_or_array(self, elem):
        if self.bool():              # constant: one element
            return self.raw(elem)
        n = self.int()               # array: count then elements
        return self.raw(n * elem)

    def bitset(self):
        if self.bool():
            return self.bool()
        nbits = self.int()
        words = (nbits + 31) // 32
        return self.raw(words * 4)


def read_bounds(s):                  # bool, bool, vec3, vec3
    return (s.bool(), s.bool(), s.vec3(), s.vec3())


def read_base_node(s):
    n = {}
    n["name"] = s.str()
    n["name2"] = s.str()
    n["bits"] = s.bitset()
    n["flags"] = (s.bool(), s.bool(), s.bool())
    n["bounds"] = read_bounds(s)
    n["kind"] = s.int()
    if n["kind"] == 0:
        if not s.bool():             # array of 0x44-byte (4x4 float + 1) entries
            cnt = s.int()
            n["xforms"] = s.raw(cnt * 0x44)
        else:
            n["xforms"] = s.raw(0x44)
    else:                            # references the bulk block
        n["v"] = (s.vec3(), s.vec3(), s.vec3())
        n["bulk_ofs"] = s.int()
    return n


def read_node_ec(s):                 # base + 4 vec3 const-or-array channels
    n = read_base_node(s)
    for _ in range(4):
        s.const_or_array(12)
    return n


def read_node_108(s):                # base + bounds
    n = read_base_node(s)
    n["bounds2"] = read_bounds(s)
    return n


def read_seq_ba50(s):                # per-sequence block (0xd8-byte records, two of them)
    r = {"name": s.str()}
    s.bool(); s.bool(); s.int(); s.int()
    for _ in range(2):
        s.str(); s.str()
        s.bool(); s.bool(); s.bool(); s.bool()
        s.const_or_array(1)
        s.const_or_array(0x44)
        s.bool()
    s.const_or_array(12); s.const_or_array(12)
    for _ in range(4):
        s.const_or_array(4)
    return r


def parse(path, verbose=True):
    d = open(path, "rb").read()
    magic, a, b = struct.unpack_from("<III", d, 0)
    assert magic == 0x198237FE and a + b == len(d), "bad container"
    s = Stream(d[:a], 12)
    out = {"file": path, "a": a, "b": b}
    assert s.int() == 0x033FB1A3, "bad ghb magic"
    out["version"] = s.int()
    out["thunks"] = s.int()
    out["src"] = s.str()
    out["i13c"] = s.int()
    n1 = s.int()
    out["n1"] = n1
    objs = []
    for _ in range(n1):
        o = {"v": s.vec3(), "i": s.int(), "name": s.str()}
        nsub = s.int()
        o["sub"] = [(s.int(), s.int(), s.str(), s.str()) for _ in range(nsub)]
        s.int(); s.int()
        objs.append(o)
    out["objs"] = objs
    k1 = s.int()
    out["seqs"] = [read_seq_ba50(s) for _ in range(k1)]
    out["bounds"] = read_bounds(s)
    s.int()
    m = s.int()
    out["M"] = m
    out["nodes"] = [read_base_node(s) for _ in range(m)]
    out["pos_after_M"] = s.p
    s.int()
    k = s.int()
    out["K"] = k
    out["nodes_ec"] = [read_node_ec(s) for _ in range(k)]
    s.int()
    l = s.int()
    out["L"] = l
    out["nodes_108"] = [read_node_108(s) for _ in range(l)]
    out["pos_after_L"] = s.p
    out["end"] = len(s.d)
    return out


if __name__ == "__main__":
    for f in sys.argv[1:]:
        try:
            r = parse(f)
            print(f, "OK-so-far: src=%r n1=%d seqs=%d M=%d K=%d L=%d pos=%d/%d" %
                  (r["src"], r["n1"], len(r["seqs"]), r["M"], r["K"], r["L"], r["pos_after_L"], r["end"]))
        except Exception as e:
            print(f, "FAIL:", type(e).__name__, e)
