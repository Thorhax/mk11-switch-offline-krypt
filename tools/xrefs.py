"""Build work/bl_xrefs.npy-style index of BL/B call sites in main's text: xrefs.py build | xrefs.py <sym|0xaddr>"""
import os, sys, struct, pickle, numpy as np
W = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work")
IDX = os.path.join(W, "calls.pkl")
if sys.argv[1] == "build":
    segs = dict((l.split()[0], (int(l.split()[1], 16), int(l.split()[2], 16))) for l in open(os.path.join(W, "main.segs.txt")) if not l.startswith("build"))
    t0, tlen = segs["text"]
    ins = np.frombuffer(open(os.path.join(W, "main.bin"), "rb").read()[t0:t0 + tlen], dtype="<u4")
    pc = np.arange(len(ins), dtype=np.int64) * 4 + t0
    isbl = (ins & 0xFC000000) == 0x94000000
    isb = (ins & 0xFC000000) == 0x14000000
    imm = (ins & 0x03FFFFFF).astype(np.int64)
    imm = np.where(imm & 0x02000000, imm - 0x04000000, imm)
    tgt = pc + imm * 4
    m = isbl | isb
    pickle.dump((pc[m], tgt[m], isbl[m]), open(IDX, "wb"))
    print("indexed", m.sum())
else:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import dis as D
    a, _ = D.find(sys.argv[1])
    pc, tgt, isbl = pickle.load(open(IDX, "rb"))
    for p, b in zip(pc[tgt == a], isbl[tgt == a]):
        print(f"{p:#010x} {'bl' if b else 'b '} from {D.name_at(int(p)) or '?'}")
