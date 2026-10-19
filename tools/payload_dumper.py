#!/usr/bin/env python3
"""
Minimal Android OTA payload.bin extractor (full OTAs only: REPLACE / REPLACE_BZ / REPLACE_XZ / ZERO).

  python tools/payload_dumper.py <payload.bin>                    list partitions and sizes
  python tools/payload_dumper.py <payload.bin> <out_dir> <part>... extract those partitions

Used read-only on the OnePlus 13 OTA to fetch its my_product (OnePlus' own vibrator effect table).
No protobuf module needed: the few fields we care about are read straight off the wire format.
"""
import bz2
import lzma
import os
import struct
import sys

REPLACE, REPLACE_BZ, ZERO, REPLACE_XZ = 0, 1, 6, 8
OP_NAMES = {REPLACE: "REPLACE", REPLACE_BZ: "REPLACE_BZ", ZERO: "ZERO", REPLACE_XZ: "REPLACE_XZ"}


def fields(buf):
    """yield (field_number, wire_type, value) for one protobuf message; value is int or bytes"""
    i, n = 0, len(buf)
    while i < n:
        key, i = varint(buf, i)
        fn, wt = key >> 3, key & 7
        if wt == 0:
            val, i = varint(buf, i)
        elif wt == 2:
            ln, i = varint(buf, i)
            val, i = buf[i:i + ln], i + ln
        elif wt == 5:
            val, i = struct.unpack_from("<I", buf, i)[0], i + 4
        elif wt == 1:
            val, i = struct.unpack_from("<Q", buf, i)[0], i + 8
        else:
            raise ValueError("wire type %d" % wt)
        yield fn, wt, val


def varint(buf, i):
    out = shift = 0
    while True:
        b = buf[i]
        i += 1
        out |= (b & 0x7F) << shift
        if not b & 0x80:
            return out, i
        shift += 7


def parse_manifest(manifest):
    block_size, partitions = 4096, []
    for fn, _, val in fields(manifest):
        if fn == 3:
            block_size = val
        elif fn == 13:                                   # PartitionUpdate
            name, ops = None, []
            for pfn, _, pval in fields(val):
                if pfn == 1:
                    name = pval.decode()
                elif pfn == 8:                           # InstallOperation
                    op = dict(type=0, offset=0, length=0, extents=[])
                    for ofn, _, oval in fields(pval):
                        if ofn == 1:
                            op["type"] = oval
                        elif ofn == 2:
                            op["offset"] = oval
                        elif ofn == 3:
                            op["length"] = oval
                        elif ofn == 6:                   # dst_extents
                            ext = dict(start=0, num=0)
                            for efn, _, eval_ in fields(oval):
                                if efn == 1:
                                    ext["start"] = eval_
                                elif efn == 2:
                                    ext["num"] = eval_
                            op["extents"].append(ext)
                    ops.append(op)
            partitions.append((name, ops))
    return block_size, partitions


def open_payload(path):
    f = open(path, "rb")
    magic, version, manifest_size, sig_size = struct.unpack(">4sQQI", f.read(24))
    if magic != b"CrAU" or version != 2:
        sys.exit("not a payload.bin v2 (%r v%d)" % (magic, version))
    manifest = f.read(manifest_size)
    data_start = 24 + manifest_size + sig_size
    return f, data_start, parse_manifest(manifest)


def size_of(ops, block_size):
    end = 0
    for op in ops:
        for e in op["extents"]:
            end = max(end, (e["start"] + e["num"]) * block_size)
    return end


def extract(f, data_start, block_size, name, ops, out_dir):
    out = os.path.join(out_dir, name + ".img")
    total = size_of(ops, block_size)
    with open(out, "wb") as o:
        o.truncate(total)
        for op in ops:
            f.seek(data_start + op["offset"])
            raw = f.read(op["length"])
            t = op["type"]
            if t == REPLACE:
                data = raw
            elif t == REPLACE_BZ:
                data = bz2.decompress(raw)
            elif t == REPLACE_XZ:
                data = lzma.decompress(raw)
            elif t == ZERO:
                data = None
            else:
                sys.exit("%s: unsupported op %s (not a full OTA?)" % (name, OP_NAMES.get(t, t)))
            pos = 0
            for e in op["extents"]:
                n = e["num"] * block_size
                o.seek(e["start"] * block_size)
                o.write(bytes(n) if data is None else data[pos:pos + n])
                pos += n
    print("%-14s %10.1f MB -> %s" % (name, total / 1e6, out))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    f, data_start, (block_size, partitions) = open_payload(sys.argv[1])
    if len(sys.argv) == 2:
        print("block size %d, %d partitions:" % (block_size, len(partitions)))
        for name, ops in partitions:
            kinds = sorted({OP_NAMES.get(o["type"], str(o["type"])) for o in ops})
            print("  %-16s %9.1f MB  %3d ops  %s" % (name, size_of(ops, block_size) / 1e6, len(ops), ",".join(kinds)))
        return
    out_dir, want = sys.argv[2], set(sys.argv[3:])
    os.makedirs(out_dir, exist_ok=True)
    found = {n for n, _ in partitions} & want
    for missing in sorted(want - found):
        print("not in payload: %s" % missing)
    for name, ops in partitions:
        if name in want:
            extract(f, data_start, block_size, name, ops, out_dir)


if __name__ == "__main__":
    main()
