#!/usr/bin/env python3
"""Replays a timed key script into the game under Proton on Hyprland, through a uinput keyboard.

Linux counterpart of ../run/input-player.ps1, same script format and same log lines, so the
Windows tools (co_run's mark wait, ab_compare.py, perf_fps.py) read its log unchanged:
  <at_ms> <key> [hold_ms]   press <key> at <at_ms> after time zero, hold it hold_ms (default 250)
  +<ms>   <key> [hold_ms]   same, <ms> after the previous event's start time
  <at_ms> mark <label>      write "mark <label>" to the log
Time zero is when the game's window first exists. Keys: return, space, escape, backspace, tab,
up/down/left/right, a letter or digit, chords like shift+e.

Needs write access to /dev/uinput (run.sh grants it with setfacl) and the Hyprland session's
hyprctl (focus checks). No Python packages: the uinput device is set up with raw ioctls.

usage: route_player.py --script route.keys --log input.log [--match pgr4_recompiled] [--timeout 120]
                       [--width 1280 --height 720]
"""
import argparse
import datetime as dt
import fcntl
import glob
import json
import os
import re
import struct
import subprocess
import time

EV_SYN, EV_KEY, SYN_REPORT = 0, 1, 0
UI_SET_EVBIT, UI_SET_KEYBIT = 0x40045564, 0x40045565
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502

KEYS = {"escape": 1, "backspace": 14, "tab": 15, "return": 28, "enter": 28, "space": 57,
        "shift": 42, "ctrl": 29, "alt": 56, "up": 103, "left": 105, "right": 106, "down": 108}
for i, ch in enumerate("1234567890"):
    KEYS[ch] = 2 + i
for row, start in (("qwertyuiop", 16), ("asdfghjkl", 30), ("zxcvbnm", 44)):
    for i, ch in enumerate(row):
        KEYS[ch] = start + i


class Keyboard:
    def __init__(self) -> None:
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        for code in set(KEYS.values()):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        # Legacy uinput_user_dev: name[80], id (bustype, vendor, product, version), ff_effects_max,
        # absmax/absmin/absfuzz/absflat[64] each.
        dev = struct.pack("80sHHHHi", b"pgr4-route-player", 0x03, 0x1234, 0x5678, 1, 0) + b"\0" * (4 * 64 * 4)
        os.write(self.fd, dev)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        time.sleep(1.0)  # let the compositor pick up the new device

    def send(self, code: int, down: bool) -> None:
        now = time.time()
        sec, usec = int(now), int((now % 1) * 1e6)
        os.write(self.fd, struct.pack("llHHi", sec, usec, EV_KEY, code, 1 if down else 0))
        os.write(self.fd, struct.pack("llHHi", sec, usec, EV_SYN, SYN_REPORT, 0))

    def close(self) -> None:
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)


def hypr_env() -> dict:
    env = dict(os.environ)
    run = env.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    env["XDG_RUNTIME_DIR"] = run
    if not env.get("HYPRLAND_INSTANCE_SIGNATURE"):
        socks = sorted(glob.glob(f"{run}/hypr/*/.socket.sock"), key=os.path.getmtime)
        if socks:
            env["HYPRLAND_INSTANCE_SIGNATURE"] = socks[-1].split("/")[-2]
    return env


def hyprctl(*args: str):
    out = subprocess.run(["hyprctl", "-j", *args], capture_output=True, text=True, env=hypr_env(), timeout=10).stdout
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return None


def find_window(match: str):
    for c in hyprctl("clients") or []:
        if re.search(match, f"{c.get('class', '')} {c.get('title', '')}", re.I) and c.get("mapped", True):
            return c
    return None


def focused(addr: str) -> bool:
    w = hyprctl("activewindow")
    return bool(w) and w.get("address") == addr


def dispatch(lua: str) -> None:
    """Hyprland 0.56 with a Lua config takes dispatchers as Lua (hl.dsp.*), not as words."""
    subprocess.run(["hyprctl", "dispatch", lua], capture_output=True, env=hypr_env(), timeout=10)


def focus(addr: str) -> bool:
    dispatch(f'hl.dsp.focus({{ window = "address:{addr}" }})')
    time.sleep(0.2)
    return focused(addr)


def place(addr: str, w: int, h: int) -> None:
    """Float the game at its own size, centred, like the Windows runs (no tiling resize)."""
    window = f'window = "address:{addr}"'
    dispatch(f'hl.dsp.window.float({{ {window}, action = "on" }})')
    dispatch(f"hl.dsp.window.resize({{ {window}, x = {w}, y = {h} }})")
    dispatch(f"hl.dsp.window.center({{ {window} }})")


def parse(text: str):
    events, prev = [], 0
    for raw in re.split(r"[\r\n;]+", text):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        f = line.split()
        at = prev + int(f[0][1:]) if f[0].startswith("+") else int(f[0])
        if at < prev:
            raise SystemExit(f"events out of order at: {raw}")
        prev = at
        if f[1] == "mark":
            events.append({"at": at, "mark": " ".join(f[2:])})
        else:
            codes = [KEYS[k] for k in f[1].lower().split("+")]
            events.append({"at": at, "keys": f[1], "codes": codes, "hold": int(f[2]) if len(f) > 2 else 250})
    return events


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--script", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--match", default="pgr4_recompiled")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    args = ap.parse_args()
    logf = open(args.log, "a", buffering=1)

    def say(msg: str) -> None:
        logf.write(f"{dt.datetime.now().astimezone().isoformat()} {msg}\n")

    events = parse(open(args.script).read())
    kb = Keyboard()
    held = []
    try:
        deadline = time.monotonic() + args.timeout
        win = None
        while not win:
            win = find_window(args.match)
            if time.monotonic() > deadline:
                say(f"error: no window matching {args.match} after {args.timeout} s")
                raise SystemExit(1)
            time.sleep(0.1)
        t0 = time.monotonic()
        clock = lambda: int((time.monotonic() - t0) * 1000)
        addr = win["address"]
        place(addr, args.width, args.height)
        say(f"start pid={win.get('pid')} window={addr} events={len(events)} script={args.script}")
        actions = []
        for n, e in enumerate(events):
            if "mark" in e:
                actions.append((e["at"], 1, n, "mark", e))
            else:
                actions.append((e["at"], 1, n, "down", e))
                actions.append((e["at"] + e["hold"], 0, n, "up", e))
        for at, _, _, kind, e in sorted(actions, key=lambda a: a[:3]):
            while (wait := at - clock()) > 0:
                time.sleep(min(wait, 500) / 1000)
                if not focused(addr):
                    ok = focus(addr)
                    if ok:
                        for h in held:
                            for c in h["codes"]:
                                kb.send(c, True)
                    say(f"refocus t={clock()} ok={ok} held={','.join(h['keys'] for h in held)}")
            if kind == "up":
                for c in reversed(e["codes"]):
                    kb.send(c, False)
                if e in held:
                    held.remove(e)
                continue
            if not find_window(args.match):
                say(f"error: game window gone at {clock()} ms")
                raise SystemExit(1)
            if kind == "mark":
                say(f"mark {e['mark']} t={clock()}")
                continue
            if not focused(addr) and not focus(addr):
                say(f"error: could not focus the game for {e['keys']} at {clock()} ms")
                raise SystemExit(1)
            late = clock() - e["at"]
            for c in e["codes"]:
                kb.send(c, True)
            held.append(e)
            say(f"key {e['keys']} hold={e['hold']} at={e['at']} late={late}")
        say("done")
    finally:
        for e in held:
            for c in reversed(e["codes"]):
                kb.send(c, False)
        kb.close()


if __name__ == "__main__":
    main()
