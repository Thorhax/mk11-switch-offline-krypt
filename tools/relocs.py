"""Index R_AARCH64_RELATIVE relocs of main: relocs.py build | relocs.py <0xtarget> (where is a pointer to target stored)"""
import os, sys, struct, pickle, numpy as np
W = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
if sys.argv[1] == "build":
    import nso
    img = open(os.path.join(W, "main.bin"), "rb").read()
    _, tags = nso.dynsyms(img)
    rela, sz = tags[7][0], tags[8][0]
    a = np.frombuffer(img[rela:rela + sz], dtype="<u8").reshape(-1, 3)
    rel = a[(a[:, 1] & 0xffffffff) == 1027]
    np.save(os.path.join(W, "relrel.npy"), rel[:, [0, 2]])
    print("relative relocs", len(rel), "of", len(a))
else:
    r = np.load(os.path.join(W, "relrel.npy"))
    t = int(sys.argv[1], 16)
    for off in r[r[:, 1] == t][:, 0]: print(hex(off))
