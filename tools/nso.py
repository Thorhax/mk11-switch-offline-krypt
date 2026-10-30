"""Decompress an NSO into a flat memory image and dump its dynamic symbols.

usage: nso.py <nso> <out_prefix>
writes <out_prefix>.bin (image based at 0), <out_prefix>.segs.txt and <out_prefix>.syms.txt
"""
import struct, sys, lz4.block

def load(path):
    d = open(path, "rb").read()
    assert d[:4] == b"NSO0"
    flags = struct.unpack_from("<I", d, 0xC)[0]
    segs = []
    for i in range(3):
        foff, moff, msize = struct.unpack_from("<III", d, 0x10 + i * 0x10)
        csize = struct.unpack_from("<I", d, 0x60 + i * 4)[0]
        raw = d[foff:foff + csize]
        if flags & (1 << i):
            raw = lz4.block.decompress(raw, uncompressed_size=msize)
        segs.append((moff, msize, raw))
    bss = struct.unpack_from("<I", d, 0x3C)[0]
    build_id = d[0x40:0x60].hex().upper()
    end = max(m + s for m, s, _ in segs) + bss
    img = bytearray(end)
    for m, s, raw in segs:
        img[m:m + len(raw)] = raw
    return img, segs, bss, build_id

def dynsyms(img):
    mod0 = struct.unpack_from("<I", img, 4)[0]
    assert img[mod0:mod0 + 4] == b"MOD0"
    dyn = mod0 + struct.unpack_from("<i", img, mod0 + 4)[0]
    tags = {}
    off = dyn
    while True:
        tag, val = struct.unpack_from("<qQ", img, off)
        off += 16
        if tag == 0: break
        tags.setdefault(tag, []).append(val)
    strtab, symtab = tags[5][0], tags[6][0]
    # Symbol count from DT_HASH (4) or DT_GNU_HASH; fall back to strtab bound.
    if 4 in tags:
        nsyms = struct.unpack_from("<I", img, tags[4][0] + 4)[0]
    else:
        nsyms = (strtab - symtab) // 24
    syms = []
    for i in range(nsyms):
        name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", img, symtab + i * 24)
        n = img[strtab + name:img.index(b"\0", strtab + name)].decode("latin1")
        syms.append((value, size, shndx, info, n))
    return syms, tags

if __name__ == "__main__":
    img, segs, bss, bid = load(sys.argv[1])
    out = sys.argv[2]
    open(out + ".bin", "wb").write(img)
    with open(out + ".segs.txt", "w") as f:
        f.write(f"build_id {bid}\n")
        for name, (m, s, _) in zip(("text", "rodata", "data"), segs):
            f.write(f"{name} {m:#x} {s:#x}\n")
        f.write(f"bss {segs[2][0] + segs[2][1]:#x} {bss:#x}\n")
    syms, _ = dynsyms(img)
    with open(out + ".syms.txt", "w") as f:
        for v, s, sh, info, n in syms:
            f.write(f"{v:#010x} {s:#x} {'U' if sh == 0 else 'D'} {n}\n")
    print(bid, [(hex(m), hex(s)) for m, s, _ in segs], hex(bss), len(syms), "syms")
