"""Add SVC permissions to a main.npdm (ACI0 and ACID kernel caps), re-laying out sections as needed,
and grant full fs permissions (FAH/FAC) so the module can log to the SD card.
usage: npdm_addsvc.py in out 0x74 0x75 ..."""
import struct, sys
d = open(sys.argv[1], "rb").read()
want = [int(x, 0) for x in sys.argv[3:]]

def add_svcs(kac):
    caps = [struct.unpack_from("<I", kac, i)[0] for i in range(0, len(kac), 4)]
    for s in want:
        idx, bit = divmod(s, 24)
        for i, v in enumerate(caps):
            if v & 0x1f == 0xf and (v >> 29) & 7 == idx:
                caps[i] = v | (1 << (5 + bit)); break
        else:
            caps.append((idx << 29) | (1 << (5 + bit)) | 0xf)
    return b"".join(struct.pack("<I", v) for v in caps)

def align(b, n=0x10):
    return b + b"\0" * (-len(b) % n)

aci_off, aci_sz, acid_off, acid_sz = struct.unpack_from("<IIII", d, 0x70)
aci = bytearray(d[aci_off:aci_off + aci_sz]); acid = bytearray(d[acid_off:acid_off + acid_sz])

# ACI0: header 0x40; fah/sac/kac (off,size) at 0x20/0x28/0x30
fah = bytearray(aci[slice(*(lambda o, s: (o, o + s))(*struct.unpack_from("<II", aci, 0x20)))])
struct.pack_into("<Q", fah, 4, 0xFFFFFFFFFFFFFFFF)
sac = aci[slice(*(lambda o, s: (o, o + s))(*struct.unpack_from("<II", aci, 0x28)))]
kac = add_svcs(aci[slice(*(lambda o, s: (o, o + s))(*struct.unpack_from("<II", aci, 0x30)))])
hdr = bytearray(aci[:0x40]); body = b""; o = 0x40
for field, blob in ((0x20, fah), (0x28, sac), (0x30, kac)):
    struct.pack_into("<II", hdr, field, o + len(body), len(blob))
    body += align(blob)
aci = bytes(hdr) + body

# ACID: header 0x240 (sig 0x100, key 0x100, then 'ACID'); fac/sac/kac at 0x220/0x228/0x230 (relative to ACID start)
fac = bytearray(acid[slice(*(lambda o, s: (o, o + s))(*struct.unpack_from("<II", acid, 0x220)))])
struct.pack_into("<Q", fac, 4, 0xFFFFFFFFFFFFFFFF)
sac2 = acid[slice(*(lambda o, s: (o, o + s))(*struct.unpack_from("<II", acid, 0x228)))]
kac2 = add_svcs(acid[slice(*(lambda o, s: (o, o + s))(*struct.unpack_from("<II", acid, 0x230)))])
hdr = bytearray(acid[:0x240]); body = b""
for field, blob in ((0x220, fac), (0x228, sac2), (0x230, kac2)):
    struct.pack_into("<II", hdr, field, 0x240 + len(body), len(blob))
    body += align(blob)
acid = bytes(hdr) + body
struct.pack_into("<I", bytearray(acid), 0x204, len(acid) - 0x100)

meta = bytearray(d[:0x80])
new_acid_off = 0x80; new_aci_off = new_acid_off + len(align(acid))
struct.pack_into("<IIII", meta, 0x70, new_aci_off, len(aci), new_acid_off, len(acid))
acid = bytearray(acid); struct.pack_into("<I", acid, 0x204, len(acid) - 0x100)  # ACID size field (excl. signature)
open(sys.argv[2], "wb").write(bytes(meta) + align(bytes(acid)) + aci)
print("patched", [hex(s) for s in want])
