#!/bin/bash
# regression: prose streamed BEFORE a tool header must surface on a tty.
# fake local SSE provider (plain http) + agent built with CLOVER_PLAIN_HTTP.
set -e
cd /home/ubuntu
rm -f /tmp/preamble-ctx.json /tmp/preamble.log /tmp/sse_fake.port /tmp/probe.out
g++ -std=c++17 -O0 -DCLOVER_PLAIN_HTTP -o /tmp/agent-plain \
    main.cpp tool_registry.cpp tools/*.cpp -lssl -lcrypto
python3 /tmp/sse_fake.py > /tmp/sse_fake.port 2>/tmp/sse_fake.err &
FAKE_PID=$!
PORT=""
for i in $(seq 1 50); do
    if [ -s /tmp/sse_fake.port ]; then PORT=$(cat /tmp/sse_fake.port); break; fi
    sleep 0.1
done
if [ -z "$PORT" ]; then
    echo "FAIL: fake provider never announced a port"; cat /tmp/sse_fake.err; exit 1
fi
echo "fake provider on port $PORT"
FAKE_PORT=$PORT python3 - <<'PY'
import os, pty, subprocess, time, select
master, slave = pty.openpty()
port = os.environ["FAKE_PORT"]
p = subprocess.Popen(
    ['/tmp/agent-plain', '-t', '--provider', '127.0.0.1:%s' % port,
     '--model', 'qwen-3.8-27b', '--ctx', '/tmp/preamble-ctx.json'],
    stdin=subprocess.PIPE, stdout=slave, stderr=slave)
os.close(slave)
p.stdin.write(b"Test that preamble prose survives a tool header.\n")
p.stdin.close()
out=b''
start=time.time()
while time.time()-start < 60 and p.poll() is None:
    r,_,_=select.select([master],[],[],1)
    if r:
        try: d=os.read(master,65536)
        except OSError: break
        if not d: break
        out+=d
try:
    while True:
        r,_,_=select.select([master],[],[],0.5)
        if not r: break
        d=os.read(master,65536)
        if not d: break
        out+=d
except OSError: pass
rc=p.wait(timeout=5)
os.close(master)
text=out.decode(errors='replace')
open('/tmp/preamble.log','w').write(text)
print('RC =', rc)
assert rc==0, 'agent failed'
assert 'LET ME FIRST SAY' in text, 'preamble prose missing!'
assert 'HELLO.' in text, 'preamble missing'
assert '[TOOL]' not in text, 'raw tool header leaked'
assert 'get_time' in text, 'tool card missing'
assert 'FINAL PROSE AFTER TOOL' in text, 'final answer missing'
print('PREAMBLE STREAMING TEST PASSED')
PY
kill $FAKE_PID 2>/dev/null || true
