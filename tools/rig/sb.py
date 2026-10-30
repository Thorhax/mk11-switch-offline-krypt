"""sys-botbase helper: sb.py <command...> ; 'shot <file.jpg>' saves a screenshot."""
import socket, sys, time
HOST = ("192.168.1.203", 6000)

def cmd(s, line, reply=False):
    s.sendall((line + "\r\n").encode())
    if not reply: return None
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(1 << 20)
        if not chunk: break
        buf += chunk
    return buf.strip()

if __name__ == "__main__":
    s = socket.create_connection(HOST, timeout=30)
    a = sys.argv[1:]
    if a[0] == "shot":
        data = cmd(s, "pixelPeek", True)
        open(a[1], "wb").write(bytes.fromhex(data.decode()))
        print("saved", a[1])
    elif a[0] in ("getTitleID",):
        print(cmd(s, a[0], True).decode())
    else:
        for c in a:
            cmd(s, c); time.sleep(1.0)
    s.close()
