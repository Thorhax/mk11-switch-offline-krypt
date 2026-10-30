"""Disassemble a function (or address range) of the flat MK11 main image, with symbol names.

usage: dis.py <symbol substring | 0xaddr> [length]
Uses work/main.bin and work/main.syms.dm.txt from nso.py + c++filt.
"""
import bisect, os, re, subprocess, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work")
OBJDUMP = "/root/tmp/claudecode/switch-claude/shaolinmonks/work/devkitpro/devkitA64/bin/aarch64-none-elf-objdump"

def load_syms():
    syms = []
    for line in open(os.path.join(ROOT, "main.syms.dm.txt"), encoding="latin1"):
        p = line.rstrip("\n").split(" ", 3)
        if len(p) < 4 or p[2] != "D": continue
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    syms.sort()
    return syms

SYMS = load_syms()
ADDRS = [s[0] for s in SYMS]

def name_at(a):
    i = bisect.bisect_right(ADDRS, a) - 1
    if i < 0: return None
    s = SYMS[i]
    off = a - s[0]
    if off == 0: return s[2]
    if off < max(s[1], 1) or off < 0x40: return f"{s[2]}+{off:#x}"
    return None

def find(q):
    if q.startswith("0x"): return int(q, 16), None
    hits = [s for s in SYMS if q in s[2]]
    if not hits: sys.exit("no symbol: " + q)
    hits.sort(key=lambda s: len(s[2]))
    if len(hits) > 1:
        for h in hits[:10]: print(f"  candidate {h[0]:#x} {h[1]:#x} {h[2]}", file=sys.stderr)
    return hits[0][0], hits[0][1]

def dis(addr, length):
    out = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "aarch64", "--adjust-vma=0",
                          f"--start-address={addr:#x}", f"--stop-address={addr + length:#x}",
                          os.path.join(ROOT, "main.bin")], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"\s+([0-9a-f]+):\s+[0-9a-f]{8}\s+(.*)", line)
        if not m: continue
        a = int(m.group(1), 16)
        lbl = name_at(a) if a in ADDRS_SET else None
        if lbl: print(f"\n<{lbl}>:")
        ins = m.group(2)
        t = re.search(r"\b(?:bl?|b\.\w+|cbn?z\s+\w+,|tbn?z\s+\w+,\s*#\d+,)\s+0x([0-9a-f]+)", ins)
        if t:
            n = name_at(int(t.group(1), 16))
            if n: ins += f"    ; {n}"
        print(f"  {a:08x}:  {ins}")

ADDRS_SET = set(ADDRS)

if __name__ == "__main__":
    a, sz = find(sys.argv[1])
    ln = int(sys.argv[2], 0) if len(sys.argv) > 2 else (sz or 0x80)
    print(f"; {a:#x} {name_at(a)}")
    dis(a, ln)
