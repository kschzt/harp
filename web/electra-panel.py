#!/usr/bin/env python3
"""electra-panel.py — an Electra One (Mini) as a HARP front panel, beside the web one.

A sidecar (sibling of web/harp-panel.py) that bridges the Electra's MIDI to
harp-deviced's panel socket — the SAME front_panel_set path the web panel and the
protocol's vendor-knob method use, so the Electra is a true peer front panel:
turn a knob here and the web UI and a DAW's automation-echo all see it (§9.4).

  Electra knob  --CC-->  "knob <id> <v01>"   (front_panel_set on the device)
  device param  --poll-> CC out               (the Electra display tracks the device)

Pure stdlib + a small ctypes shim over libasound's snd_rawmidi — no extra
packages (the Pi stays dependency-minimal, like the stdlib-only web sidecar).
The preset is GENERATED from the live param list, so the Electra layout follows
the device's §9.3 params automatically.

  usage: electra-panel.py [PANEL_SOCK]   (default /tmp/harp-panel.sock)
"""
import ctypes
import json
import socket
import subprocess
import sys
import threading
import time

PANEL_SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/harp-panel.sock"
MIDI_CH = 0          # device channel 1 (status nibble 0); our private CC map
POLL_HZ = 15.0       # display refresh: device param -> Electra
ELECTRA_SYSEX = bytes([0xF0, 0x00, 0x21, 0x45])  # F0 + manufacturer id 00 21 45

# ---------------- ALSA rawmidi via ctypes (no python-rtmidi needed) ----------------
_snd = ctypes.CDLL("libasound.so.2")
_snd.snd_rawmidi_open.argtypes = [ctypes.POINTER(ctypes.c_void_p),
                                  ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p, ctypes.c_int]
_snd.snd_rawmidi_open.restype = ctypes.c_int
_snd.snd_rawmidi_read.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
_snd.snd_rawmidi_read.restype = ctypes.c_long
_snd.snd_rawmidi_write.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
_snd.snd_rawmidi_write.restype = ctypes.c_long
_snd.snd_rawmidi_drain.argtypes = [ctypes.c_void_p]
_snd.snd_rawmidi_drain.restype = ctypes.c_int
_snd.snd_rawmidi_nonblock.argtypes = [ctypes.c_void_p, ctypes.c_int]
_snd.snd_rawmidi_nonblock.restype = ctypes.c_int
_snd.snd_strerror.argtypes = [ctypes.c_int]
_snd.snd_strerror.restype = ctypes.c_char_p


class RawMidi:
    """One ALSA rawmidi endpoint (hw:card,dev,sub). in/out are independent handles
    from one open, so a reader thread and a writer thread don't contend."""

    def __init__(self, name, want_in=True, want_out=True):
        self.h_in = ctypes.c_void_p()
        self.h_out = ctypes.c_void_p()
        rc = _snd.snd_rawmidi_open(ctypes.byref(self.h_in) if want_in else None,
                                   ctypes.byref(self.h_out) if want_out else None,
                                   name.encode(), 0)  # 0 = blocking
        if rc < 0:
            raise OSError("snd_rawmidi_open(%s): %s" % (name, _snd.snd_strerror(rc).decode()))

    def write(self, data):
        buf = (ctypes.c_ubyte * len(data))(*data)
        _snd.snd_rawmidi_write(self.h_out, buf, len(data))
        _snd.snd_rawmidi_drain(self.h_out)

    def read(self, n=512):
        buf = (ctypes.c_ubyte * n)()
        r = _snd.snd_rawmidi_read(self.h_in, buf, n)  # blocks until >=1 byte
        return bytes(buf[:r]) if r > 0 else b""

    def nonblock(self, on=True):
        if self.h_in:
            _snd.snd_rawmidi_nonblock(self.h_in, 1 if on else 0)

    def read_window(self, secs):
        """Non-blocking gather of ALL reply bytes over `secs` (the Electra may emit
        several SysEx in response to an upload — an event plus the ACK)."""
        self.nonblock(True)
        got = b""
        t0 = time.time()
        while time.time() - t0 < secs:
            chunk = self.read()
            if chunk:
                got += chunk
            else:
                time.sleep(0.02)
        self.nonblock(False)
        return got


def find_electra():
    """The Electra's MIDI port + CTRL port addresses (hw:c,d,s), found by NAME so a
    card-number change across reboots doesn't matter. CTRL carries the SysEx
    preset upload; the (first) Electra Port carries the control CCs."""
    out = subprocess.check_output(["amidi", "-l"], text=True)
    port = ctrl = None
    for ln in out.splitlines():
        f = ln.split(None, 2)
        if len(f) >= 3 and f[1].startswith("hw:"):
            addr, name = f[1], f[2]
            if "CTRL" in name:
                ctrl = addr
            elif "Electra Port" in name and port is None:
                port = addr
    return port, ctrl


# ---------------- panel socket (line-JSON over the device's unix socket) ----------------
class Panel:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self._open()

    def _open(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(self.path)
        self.f = self.s.makefile("rw")

    def cmd(self, line):
        with self.lock:
            for _ in range(2):  # one reconnect attempt
                try:
                    self.f.write(line + "\n")
                    self.f.flush()
                    return self.f.readline().strip()
                except OSError:
                    try:
                        self._open()
                    except OSError:
                        time.sleep(0.5)
            return ""


# ---------------- preset (generated from the live §9.3 param list) ----------------
# Electra One Mini layout: 8 knobs in a 4x2 grid (top row 1-4, bottom row 5-8) +
# 6 hardware buttons (MENU, CONTEXT fixed; BUTTON 1-4 assignable). The docs don't
# publish the Mini's pixel grid, so the canvas is a single tunable constant: the
# top control row sits under the top knobs, the bottom row under the bottom knobs.
CANVAS_W, CANVAS_H = 800, 480   # Electra One Mini: 5" 800x480 panel
COLS = 4
COL_W = CANVAS_W // COLS        # 200 px per column
# Cell height trades off the value↔bar gap: too tall strands the number far below
# the bar, too short collapses it onto the bar (white-on-white). ~130 lifts the
# value clear onto the dark background. Top row stays at 44 (good under the top
# knobs); bottom row pulled well up, clear of the MENU/CONTEXT strip at the screen
# foot (the bottom knobs map by potId regardless of exact y).
PADX, TOP_Y, ROW_H = 8, 44, 130
BOT_Y = 210

# Colour the faders by function group so they pop on the dark screen (white bars
# read as dull grey). Falls back to white for anything unmatched.
COLOR_BY_GROUP = {
    "Osc": "F49500",     # orange  — oscillator
    "Filter": "529DEC",  # blue    — filter
    "Env": "03A598",     # teal    — envelope
    "Drone": "F45C51",   # red     — drone / level
    "Master": "F45C51",
    "Arp": "C44795",     # magenta — arpeggiator
    "Glide": "FFD24A",   # yellow  — glide
}


def control_color(name):
    for prefix, color in COLOR_BY_GROUP.items():
        if name.startswith(prefix):
            return color
    return "FFFFFF"


def build_preset(params):
    """A 2-page Mini preset: the §9.3 params as CC faders (param id N -> CC N), 8 on
    page 1 (the 8 knobs) and the rest on page 2. The CC binding is what the bridge
    mirrors; the four-action buttons come in Phase 2."""
    devices = [{"id": 1, "name": "HARP", "port": 1, "channel": MIDI_CH + 1, "rate": 15}]
    pages = [{"id": 1, "name": "Synth"}, {"id": 2, "name": "Arp + Glide"}]
    controls = []
    for i, p in enumerate(params):
        page = 1 if i < 8 else 2
        slot = i if i < 8 else i - 8        # 0..7 -> potId 1..8 (4x2 grid)
        col, row = slot % COLS, slot // COLS
        controls.append({
            "id": p["id"], "type": "fader", "name": p["name"][:14],  # device's own casing
            "color": control_color(p["name"]),
            "bounds": [col * COL_W + PADX, TOP_Y if row == 0 else BOT_Y,
                       COL_W - 2 * PADX, ROW_H],
            "pageId": page, "controlSetId": 1,
            "inputs": [{"potId": slot + 1, "valueId": "value"}],
            "values": [{"id": "value", "min": 0, "max": 127,
                        "message": {"deviceId": 1, "type": "cc7",
                                    "parameterNumber": p["id"], "min": 0, "max": 127}}],
        })
    return {"version": 2, "name": "HARP RefDev", "pages": pages, "devices": devices,
            "controls": controls, "groups": [], "overlays": []}


def upload_preset(ctrl, preset):
    js = json.dumps(preset, separators=(",", ":")).encode("ascii")  # JSON is 7-bit
    ctrl.write(ELECTRA_SYSEX + bytes([0x01, 0x01]) + js + bytes([0xF7]))


def force_page(ctrl, page):
    """Jump the Electra to a given active page (SysEx 09 0A). This is the host
    forcing the screen — the mechanism that auto-opens the Phase-2 Reconcile page
    when a recall conflict fires (so the user is taken to the actions, not asked
    to navigate). Pages are 1-indexed by their order in the preset."""
    ctrl.write(ELECTRA_SYSEX + bytes([0x09, 0x0A, page & 0x7F, 0xF7]))


# ---------------- a tiny streaming MIDI parser (channel-voice messages) ----------------
class MidiParse:
    """Yields (status, d1, d2) for 2-data-byte channel messages (CC, note). Our
    preset only emits CC, so the 2-byte assumption holds; realtime/system bytes
    are skipped. Handles MIDI running status."""

    def __init__(self):
        self.status = 0
        self.data = []

    def feed(self, bs):
        out = []
        for b in bs:
            if b >= 0xF8:           # realtime — interleaves anywhere, ignore
                continue
            if b >= 0x80:           # status byte
                if b >= 0xF0:       # system/sysex — not used on the control port
                    self.status = 0
                    self.data = []
                    continue
                self.status = b
                self.data = []
            else:
                if not self.status:
                    continue
                self.data.append(b)
                if len(self.data) >= 2:
                    out.append((self.status, self.data[0], self.data[1]))
                    self.data = []
        return out


def main():
    port_name, ctrl_name = find_electra()
    if not port_name or not ctrl_name:
        sys.stderr.write("electra-panel: Electra One not found on the MIDI bus\n")
        sys.exit(2)
    sys.stderr.write("electra-panel: Electra port=%s ctrl=%s\n" % (port_name, ctrl_name))

    panel = Panel(PANEL_SOCK)
    params = json.loads(panel.cmd("params"))["params"]

    ctrl = RawMidi(ctrl_name, want_in=True, want_out=True)
    upload_preset(ctrl, build_preset(params))
    # The Electra replies on the CTRL port: ACK = F0 00 21 45 7E 01 ..., NACK = ... 7E 00 ...
    ack = ctrl.read_window(0.8)
    if ELECTRA_SYSEX + b"\x7e\x00" in ack:
        sys.stderr.write("electra-panel: preset upload NACK [%s]\n" % ack.hex())
    elif ELECTRA_SYSEX + b"\x7e\x01" in ack:
        sys.stderr.write("electra-panel: preset upload ACK (%d params)\n" % len(params))
    else:
        sys.stderr.write("electra-panel: no clear upload ACK [%s]\n" % (ack.hex() or "(silence)"))
    time.sleep(0.4)  # let the controller swap to the new preset

    # Phase-2 note — firing actions from the Mini (non-touch):
    # The 4 assignable buttons emit no MIDI by default, BUT the "Assign Buttons"
    # feature (fw >= 4.1.0) binds a button to a preset Lua user function
    # (preset.userFunctions) that can midi.sendControlChange/sendSysex — caught
    # here. That binding is a one-time device-menu step (not storable in the preset
    # or the uploadable config). The KNOBS are press/touch-sensitive and DO emit
    # CTRL-port events, and ARE preset-automatable:
    #   subscribe   F0 00 21 45 14 79 <flags> F7
    #   press/rel   F0 00 21 45 7E 0A <potId> <ctrlId-lo> <ctrlId-hi> <pressed> F7
    # So the four reconcile actions (Push/Pull/Read-only/Duplicate) will be knob
    # PRESSES on a dedicated Reconcile page — no manual button-assign needed. Pages
    # navigate via MENU -> press the knob matching the page in the screen list.

    port = RawMidi(port_name, want_in=True, want_out=True)
    by_cc = {p["id"] for p in params}
    shown = {}  # param id -> last CC value exchanged, so we never echo a value back

    def reader():
        mp = MidiParse()
        while True:
            data = port.read()
            if not data:
                time.sleep(0.005)
                continue
            for st, d1, d2 in mp.feed(data):
                if (st & 0xF0) == 0xB0 and d1 in by_cc:  # a knob moved
                    shown[d1] = d2
                    panel.cmd("knob %d %.5f" % (d1, d2 / 127.0))

    threading.Thread(target=reader, daemon=True).start()

    # display poll: the device's live param values -> CC out, so the Electra tracks
    # the device however it changed (web panel, a DAW, or a hardware front-panel move).
    period = 1.0 / POLL_HZ
    while True:
        try:
            cur = json.loads(panel.cmd("params"))["params"]
            out = bytearray()
            for p in cur:
                cc = max(0, min(127, round(p["value"] * 127)))
                if shown.get(p["id"]) != cc:
                    shown[p["id"]] = cc
                    out += bytes([0xB0 | MIDI_CH, p["id"], cc])
            if out:
                port.write(out)
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(period)


if __name__ == "__main__":
    main()
