#!/usr/bin/env python3
# normal-exit reset check: /quit must leave the terminal SGR state clean too.
import os, pty, select, time

pid, fd = pty.fork()
if pid == 0:
    os.execv("./a.out", ["./a.out", "--ctx", "/tmp/wgen/quit-ctx.json"])

buf = b""
end = time.time() + 20
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
    if b"\xe2\x86\x92" in buf and b"help for commands" in buf:
        break
os.write(fd, b"/quit\n")
while True:
    r, _, _ = select.select([fd], [], [], 1.5)
    if not r:
        break
    try:
        c = os.read(fd, 4096)
    except OSError:
        break
    if not c:
        break
    buf += c
_, status = os.waitpid(pid, 0)
print("exit_code:", os.waitstatus_to_exitcode(status))
print("reset_at_end:", b"\x1b[0m" in buf[-32:])
print("tail:", buf[-32:])
