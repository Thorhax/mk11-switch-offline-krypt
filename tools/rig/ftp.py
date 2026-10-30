"""Small FTP helper for the test console: ftp.py put <local> <remote> | get <remote> <local> | rm <remote> | ls <dir> | cat <remote>"""
import ftplib, io, json, os, sys
# Console address and login live in rig.local.json next to this script (not committed):
# {"host": "...", "ftp_port": 5000, "ftp_user": "...", "ftp_pass": "..."}
cfg = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "rig.local.json")))
f = ftplib.FTP(); f.connect(cfg["host"], cfg["ftp_port"], timeout=60); f.login(cfg["ftp_user"], cfg["ftp_pass"])
op = sys.argv[1]
if op == "put":
    with open(sys.argv[2], "rb") as i: f.storbinary("STOR " + sys.argv[3], i)
elif op == "get":
    with open(sys.argv[3], "wb") as o: f.retrbinary("RETR " + sys.argv[2], o.write)
elif op == "rm":
    f.delete(sys.argv[2])
elif op == "ls":
    for n, fa in f.mlsd(sys.argv[2]): print(fa.get("type"), fa.get("size", ""), n)
elif op == "cat":
    b = io.BytesIO(); f.retrbinary("RETR " + sys.argv[2], b.write); sys.stdout.write(b.getvalue().decode(errors="replace"))
f.quit()
