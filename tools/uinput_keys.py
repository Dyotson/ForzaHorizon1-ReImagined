#!/usr/bin/env python3
"""uinput_keys.py FIFO: a virtual keyboard and mouse for testing --mnk_mode=true.

Creates the FIFO and presses what is written to it, one command per line:
  down KEY | up KEY | tap KEY [secs] | move DX DY [ms] | click lmb|rmb|mmb | wheel N
  | sleep S | quit
KEY is an evdev name without KEY_ (W, SPACE, LEFTSHIFT, ENTER, BACKSPACE, UP...). The
events go through the compositor to the window that has focus, so the game has to be the
focused window (python3 needs write access to /dev/uinput, e.g. group input or an ACL).
"""
import sys, os, time, evdev
from evdev import UInput, ecodes as e
fifo = sys.argv[1]
caps = {e.EV_KEY: [v for k, v in e.ecodes.items() if k.startswith('KEY_') and v < 249] + [e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE],
        e.EV_REL: [e.REL_X, e.REL_Y, e.REL_WHEEL]}
ui = UInput(caps, name='mnk-test-device')
time.sleep(1)
def code(n):
    return getattr(e, 'KEY_' + n.upper())
btn = {'lmb': e.BTN_LEFT, 'rmb': e.BTN_RIGHT, 'mmb': e.BTN_MIDDLE}
def key(c, v): ui.write(e.EV_KEY, c, v); ui.syn()
if not os.path.exists(fifo): os.mkfifo(fifo)
while True:
    with open(fifo) as f:
        for line in f:
            a = line.split()
            if not a: continue
            c = a[0]
            try:
                if c == 'down': key(code(a[1]), 1)
                elif c == 'up': key(code(a[1]), 0)
                elif c == 'tap':
                    key(code(a[1]), 1); time.sleep(float(a[2]) if len(a) > 2 else 0.15); key(code(a[1]), 0)
                elif c == 'move':
                    dx, dy = int(a[1]), int(a[2]); n = max(1, int(a[3]) // 10 if len(a) > 3 else max(abs(dx), abs(dy)) // 5)
                    for i in range(n):
                        ui.write(e.EV_REL, e.REL_X, dx // n); ui.write(e.EV_REL, e.REL_Y, dy // n); ui.syn(); time.sleep(0.01)
                elif c == 'click':
                    key(btn[a[1]], 1); time.sleep(0.1); key(btn[a[1]], 0)
                elif c == 'bdown': key(btn[a[1]], 1)
                elif c == 'bup': key(btn[a[1]], 0)
                elif c == 'wheel': ui.write(e.EV_REL, e.REL_WHEEL, int(a[1])); ui.syn()
                elif c == 'sleep': time.sleep(float(a[1]))
                elif c == 'quit': ui.close(); sys.exit(0)
            except Exception as ex:
                print('err', line, ex, flush=True)
