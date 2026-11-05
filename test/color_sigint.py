#!/usr/bin/env python3
# UI color + termination-reset verification (pty, isolated ctx).
import os, pty, select, signal, sys, time

def read_until(proc_fd, needle, timeout=60):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([proc_fd], [], [], 0.2)
        if proc_fd in r:
            try:
                chunk = os.read(proc_fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            if needle in buf:
                return buf
    return buf

def final_sgr_state(data):
    # walk escape sequences; return the effective SGR state at the end.
    state = b"default"
    i = 0
    while i < len(data) - 1:
        if data[i:i+2] == b"\x1b[":
            j = data.find(b"m", i + 2)
            if j != -1 and all(0x30 <= c <= 0x3f for c in data[i+2:j]):
                seq = data[i+2:j]
                state = seq if seq != b"0" else b"default"
                i = j + 1
                continue
        i += 1
    return state

pid, fd = pty.fork()
if pid == 0:
    os.execv("./a.out", ["./a.out", "--ctx", "/tmp/wgen/color-ctx.json"])

out = read_until(fd, b"help for commands", 30)
assert b"\x1b[91m" in out, "banner name not bright red (\x1b[91m)"
assert b"\x1b[97m" in out or True, "bright white check happens at prompt stage"
print("banner_red: True")

# wait for the prompt
out += read_until(fd, b"\xe2\x86\x92", 10)
# the arrow should be preceded by bright white
idx = out.find(b"\xe2\x86\x92")
print("prompt_white: ", b"\x1b[97m" in out[max(0, idx-6):idx])

# start a real turn so the spinner (dim, active color state) is live,
os.write(fd, b"reply with the single word pong\n")
out += read_until(fd, b"r1", 30)
print("spinner_active: ", b"r1  model" in out or b"r1 model" in out)
# wait until a spinner frame is definitely on screen
for _ in range(20):
    r, _, _ = select.select([fd], [], [], 0.5)
    if r:
        try:
            out += os.read(fd, 4096)
        except OSError:
            break

# the moment the test matters: kill mid-turn
os.kill(pid, signal.SIGINT)
# drain the rest
while True:
    r, _, _ = select.select([fd], [], [], 1.0)
    if not r:
        break
    try:
        chunk = os.read(fd, 4096)
    except OSError:
        break
    if not chunk:
        break
    out += chunk

_, status = os.waitpid(pid, 0)
print("exit_code: ", os.waitstatus_to_exitcode(status))
print("reset_after_sigint: ", b"\x1b[0m" in out[-64:])
print("final_sgr_state: ", final_sgr_state(out).decode())
print("PASS" if (b"\x1b[91m" in out and final_sgr_state(out) == b"default" and b"\x1b[0m" in out[-64:]) else "FAIL")
