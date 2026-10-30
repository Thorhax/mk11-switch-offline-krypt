"""Print string literals loaded via adrp+add in a function: strrefs.py <sym|0xaddr> [len]"""
import os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dis as D
img = open(os.path.join(D.ROOT, "main.bin"), "rb").read()
a, sz = D.find(sys.argv[1]); ln = int(sys.argv[2], 0) if len(sys.argv) > 2 else sz
out = subprocess.run([D.OBJDUMP, "-D", "-b", "binary", "-m", "aarch64", f"--start-address={a:#x}", f"--stop-address={a+ln:#x}", os.path.join(D.ROOT, "main.bin")], capture_output=True, text=True).stdout
regs = {}
for line in out.splitlines():
    m = re.match(r"\s+([0-9a-f]+):\s+[0-9a-f]{8}\s+(\w+)\s+(.*)", line)
    if not m: continue
    pc, op, args = int(m.group(1), 16), m.group(2), m.group(3)
    if op == "adrp":
        r, v = re.match(r"(x\d+), 0x([0-9a-f]+)", args).groups(); regs[r] = int(v, 16)
    elif op == "add":
        mm = re.match(r"(x\d+), (x\d+), #0x([0-9a-f]+)", args)
        if mm and mm.group(2) in regs:
            t = regs[mm.group(2)] + int(mm.group(3), 16)
            if 0x5e6b000 <= t < 0xa615000:
                e = img.find(b"\0", t); s = img[t:e]
                if 0 < len(s) < 200 and all(32 <= c < 127 for c in s):
                    print(f"{pc:#010x} {s.decode()!r}")
