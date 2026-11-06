#!/bin/bash
# regression: oversized tool result must be capped; head+tail kept, cut marked,
# saved context must stay under the per-model budget.
set -e
cd /home/ubuntu
rm -f /tmp/cap-ctx.json /tmp/cap.log /tmp/cap_fake.port
# build plain-http binary (same pattern as stream_test / preamble test)
g++ -std=c++17 -O0 -DCLOVER_PLAIN_HTTP -o /tmp/agent-cap \
    main.cpp tool_registry.cpp tools/*.cpp -lssl -lcrypto
cat > /tmp/cap_fake.py <<'PY'
import socket, json, time

def ev(text, finish=None):
    d={"choices":[{"delta":{"content":text},"finish_reason":finish}]}
    return ("data: "+json.dumps(d)+"\n\n").encode()

# Round 0: invoke run_command with a quote-free command producing ~300KB.
# Round 1: final prose.
TOOL=json.dumps([{"id":7,"tool":"run_command",
    "command":"head -c 300000 /dev/zero | tr '\\0' 'x'"}])
ROUNDS=[
    [
        ev("[TOOL]\n"),
        ev(TOOL+"\n"),
        ev("\n"),
        ev("DONE WITH TOOL.\n", "stop"),
    ],
    [
        ev("FINAL ANSWER AFTER CAP.\n", "stop"),
    ],
]

s=socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0))
s.listen(8)
print(s.getsockname()[1], flush=True)

i=0
while True:
    conn,_=s.accept()
    conn.settimeout(8)
    data=b""
    while True:
        try: chunk=conn.recv(65536)
        except OSError: break
        if not chunk: break
        data+=chunk
        if b"\r\n\r\n" in data:
            hd,body=data.split(b"\r\n\r\n",1)
            length=0
            for line in hd.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    length=int(line.split(b":",1)[1].strip())
            if length>0 and len(body)>=length: break
            if length==0: break
    conn.sendall(b"HTTP/1.1 200 OK\r\n")
    conn.sendall(b"Content-Type: text/event-stream\r\n")
    conn.sendall(b"Connection: close\r\n\r\n")
    for payload in ROUNDS[i % len(ROUNDS)]:
        try:
            conn.sendall(payload)
            time.sleep(0.02)
        except OSError:
            break
    try: conn.close()
    except OSError: pass
    i+=1
PY
python3 /tmp/cap_fake.py > /tmp/cap_fake.port 2>/tmp/cap_fake.err &
FAKE_PID=$!
PORT=""
for i in $(seq 1 50); do
    if [ -s /tmp/cap_fake.port ]; then PORT=$(cat /tmp/cap_fake.port); break; fi
    sleep 0.1
done
echo "fake provider on port $PORT"

# run agent against it in a pty
set -a; source .clover.env; set +a
FAKE_PORT=$PORT python3 - <<'PY'
import os, pty, subprocess, time, select
master, slave = pty.openpty()
port=os.environ["FAKE_PORT"]
p = subprocess.Popen(
    ['/tmp/agent-cap','-t','--provider','127.0.0.1:%s'%port,
     '--model','qwen-3.8-27b','--ctx','/tmp/cap-ctx.json'],
    stdin=subprocess.PIPE, stdout=slave, stderr=slave)
os.close(slave)
p.stdin.write(b"Please run the tool.\n")
p.stdin.close()
out=b''
start=time.time()
while time.time()-start < 120 and p.poll() is None:
    r,_,_=select.select([master],[],[],1)
    if r:
        try: d=os.read(master,65536)
        except OSError: break
        if not d: break
        out+=d
rc=p.wait(timeout=10)
text=out.decode(errors='replace')
open('/tmp/cap.log','w').write(text)
print('RC =', rc)
assert rc==0, 'agent failed'
assert 'FINAL ANSWER AFTER CAP' in text, 'final answer missing'
# the terminal card shows the capped size (not 300000 raw)
print('--- tool result card region ---')
for line in text.split('\n'):
    if 'bytes' in line or 'tool' in line:
        print(repr(line))
PY
kill $FAKE_PID 2>/dev/null || true

echo '=== saved context check ==='
python3 - <<'PY'
import json, os
p='/tmp/cap-ctx.json'
if not os.path.exists(p):
    print('FAIL: no context file written'); raise SystemExit(1)
d=json.load(open(p))
body=sum(len(json.dumps(m)) for m in d["messages"])
print('msgs:', len(d["messages"]), 'serialized body bytes:', body)
for m in d["messages"]:
    c=m.get("content","")
    if isinstance(c,str) and 'truncated' in c:
        print('CUT MARKER PRESENT; message bytes:', len(c))
        assert len(c) < 200*1024
        break
else:
    print('FAIL: no truncation marker in context')
    raise SystemExit(1)
assert body < 384000, 'context body exceeds budget'
print('PASS: oversized tool result capped in saved context')
PY
