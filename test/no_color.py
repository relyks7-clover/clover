#!/usr/bin/env python3
# no-color regression: the UI must emit no SGR color sequences (\x1b[...m).
# the OSC title (\x1b]0;...) is allowed - it is not color.
import os, pty, re, select, signal, time

pid, fd = pty.fork()
if pid == 0:
    os.execv("./a.out", ["./a.out", "--ctx", "/tmp/wgen/nocolor-ctx.json"])

buf = b""
end = time.time() + 30
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.3)
    if r:
        try:
            c = os.read(fd, 4096)
        except OSError:
            break
        if not c:
            break
        buf += c
    if b"help for commands" in buf and b"\xe2\x86\x92" in buf:
        break
assert b"help for commands" in buf, "banner text missing"
os.write(fd, b"reply with the single word pong\n")
end = time.time() + 60
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.5)
    if not r:
        continue
    try:
        c = os.read(fd, 4096)
    except OSError:
        break
    if not c:
        break
    buf += c
    if b"r1" in buf:
        break
assert b"r1" in buf, "spinner missing"
os.kill(pid, signal.SIGINT)
while True:
    r, _, _ = select.select([fd], [], [], 1.0)
    if not r:
        break
    try:
        c = os.read(fd, 4096)
    except OSError:
        break
    if not c:
        break
    buf += c
os.waitpid(pid, 0)
color_seqs = re.findall(rb"\x1b\[[0-9;]*m", buf)
print("sgr_color_sequences:", len(color_seqs))
print("title_seq:", buf.count(b"\x1b]0;"))
print("PASS" if not color_seqs else "FAIL")
