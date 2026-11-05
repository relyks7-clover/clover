#!/bin/bash
# UI pty verification - ISOLATED: --ctx scratch so it never touches the live
# context.json. opmem is shared (known); context is protected.
cd /home/ubuntu
mkdir -p /tmp/wgen
SCRATCH=/tmp/wgen/ui-ctx.json
rm -f "$SCRATCH" "$SCRATCH".*
printf 'reply with the single word pong\n/quit\n' | timeout 90 script -qec "./a.out --ctx $SCRATCH" /dev/null > /tmp/wgen/tty2.out 2>&1
echo run_rc=$?
python3 - "$SCRATCH" <<'PY'
import sys
d = open('/tmp/wgen/tty2.out','rb').read()
print('bytes:', len(d))
print('esc_bytes:', d.count(b'\x1b'))
print('banner:', b'help for commands' in d)
print('title_seq:', d.count(b'\x1b]0;'))
print('spinner_r1_model:', b'r1  model' in d)
print('braille_kbd:', d.count(b'\xe2\xa0\x8b'), d.count(b'\xe2\xa0\x99'), d.count(b'\xe2\xa0\xb9'), d.count(b'\xe2\xa0\xb8'))
print('prompt_arrows:', d.count(b'\xe2\x86\x92'))
print('pong_reply:', b'pong' in d)
i = d.find(b'r1')
print('spinner_region:', d[max(0,i-14):i+24])
# the scratch ctx must contain the pong exchange, and nothing else
import json
c = json.load(open(sys.argv[1]))
print('scratch_ctx_msgs:', len(c['messages']))
print('scratch_roles:', [m['role'] for m in c['messages']])
PY
