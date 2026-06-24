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
        n = _snd.snd_rawmidi_write(self.h_out, buf, len(data))
        _snd.snd_rawmidi_drain(self.h_out)
        return n

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
    """The HARP front panel. Each PARAM page puts the Part selector on knob 1 (turn
    to choose which of the 16 engine parts the knobs edit -> CC 116, the sidecar
    re-scopes), then the §9.3 params as CC faders (id N -> CC N). Env Attack/Release
    fold into ONE adr envelope on knob 6 (press it for the per-stage detail). Page 3
    is the Reconcile page (Phase 2)."""
    devices = [{"id": 1, "name": "HARP", "port": 1, "channel": MIDI_CH + 1, "rate": 15}]
    pages = [{"id": 1, "name": "Synth"}, {"id": 2, "name": "Arp + Glide"}]
    controls = []
    pname = {p["id"]: p["name"] for p in params}  # param id -> name

    def bounds(pot):
        col, row = (pot - 1) % COLS, (pot - 1) // COLS
        return [col * COL_W + PADX, TOP_Y if row == 0 else BOT_Y, COL_W - 2 * PADX, ROW_H]

    def part_selector(page):   # knob 1 on every param page: turn to pick the part
        return {"id": 220 + page, "type": "fader", "name": "Part", "color": "FFD24A",
                "bounds": bounds(1), "pageId": page, "controlSetId": 1,
                "inputs": [{"potId": 1, "valueId": "value"}],
                "values": [{"id": "value", "min": 1, "max": 16,
                            "message": {"deviceId": 1, "type": "cc7",
                                        "parameterNumber": 116, "min": 1, "max": 16}}]}

    def fader(pid, page, pot):
        return {"id": pid, "type": "fader", "name": pname.get(pid, "?")[:14],
                "color": control_color(pname.get(pid, "")),
                "bounds": bounds(pot), "pageId": page, "controlSetId": 1,
                "inputs": [{"potId": pot, "valueId": "value"}],
                "values": [{"id": "value", "min": 0, "max": 127,
                            "message": {"deviceId": 1, "type": "cc7",
                                        "parameterNumber": pid, "min": 0, "max": 127}}]}

    # Page 1 (Synth): Part(1), Osc Pitch(2), Osc Shape(3), Filter Cutoff(4),
    # Filter Reso(5), Env(6 = adr: attack CC5 + release CC6, press for the detail),
    # Drone Mix(7), Master Level(8).
    controls += [part_selector(1), fader(1, 1, 2), fader(2, 1, 3), fader(3, 1, 4),
                 fader(4, 1, 5)]
    controls.append({
        "id": 5, "type": "adr", "name": "Env", "color": control_color("Env"),
        "bounds": bounds(6), "pageId": 1, "controlSetId": 1,
        "inputs": [{"potId": 6, "valueId": "attack"}],   # detail screen exposes all three
        "values": [
            {"id": "attack", "min": 0, "max": 127,
             "message": {"deviceId": 1, "type": "cc7", "parameterNumber": 5, "min": 0, "max": 127}},
            # adr ALWAYS has a decay stage; the refdev has none. Bind it to a CC the
            # bridge ignores (60) — a harmless no-op decay — so the control is
            # well-formed (an unmapped stage froze the firmware).
            {"id": "decay", "min": 0, "max": 127,
             "message": {"deviceId": 1, "type": "cc7", "parameterNumber": 60, "min": 0, "max": 127}},
            {"id": "release", "min": 0, "max": 127,
             "message": {"deviceId": 1, "type": "cc7", "parameterNumber": 6, "min": 0, "max": 127}},
        ]})
    controls += [fader(7, 1, 7), fader(8, 1, 8)]
    # Page 2 (Arp + Glide): Part(1), then the 5 arp/glide params on knobs 2-6.
    controls += [part_selector(2), fader(9, 2, 2), fader(10, 2, 3), fader(11, 2, 4),
                 fader(12, 2, 5), fader(13, 2, 6)]
    # Phase-2 Reconcile page (§11.4): the four safe actions on knobs 1-4. The Mini
    # is non-touch, so a knob PRESS fires the action under it (caught as a CTRL pot
    # event); these controls are bound to CCs the bridge ignores (>13), so turning
    # one is harmless — only the press matters.
    pages.append({"id": 3, "name": "Reconcile"})
    actions = [("Push to Synth", "F49500"), ("Pull to DAW", "529DEC"),
               ("Read-only", "9AA0A6"), ("Duplicate", "C44795")]
    for j, (nm, color) in enumerate(actions):
        controls.append({
            # pad, not fader: a pad fires on CLICK (the Electra's "select"); a fader
            # only moves on a turn, which bleeds into the next page's knob after we
            # bounce back. Momentary -> sends onValue (127) on press, caught below.
            "id": 200 + j, "type": "pad", "name": nm, "color": color,
            "mode": "momentary",
            "bounds": [j * COL_W + PADX, TOP_Y, COL_W - 2 * PADX, ROW_H],
            "pageId": 3, "controlSetId": 1,
            "inputs": [{"potId": j + 1, "valueId": "value"}],
            "values": [{"id": "value",
                        "message": {"deviceId": 1, "type": "cc7",
                                    "parameterNumber": 110 + j, "onValue": 127, "offValue": 0}}],
        })
    # Headline: a group header at the top. (Bigger text would need a control, which
    # always carries chrome — a pad's box or a fader's bar — so a clean text-only
    # title is only available at the group's fixed small font.) Lua relabels it.
    groups = [{
        "id": 1, "pageId": 3, "controlSetId": 1,
        "name": "Project and Synth differ",
        "bounds": [0, 4, CANVAS_W, 34], "color": "F49500",
    }]
    return {"version": 2, "name": "HARP RefDev", "pages": pages, "devices": devices,
            "controls": controls, "groups": groups, "overlays": []}


def upload_preset(ctrl, preset):
    js = json.dumps(preset, separators=(",", ":")).encode("ascii")  # JSON is 7-bit
    ctrl.write(ELECTRA_SYSEX + bytes([0x01, 0x01]) + js + bytes([0xF7]))


# The preset's Lua: one user function per §11.4 action, each emitting a CC the
# sidecar catches. These appear in the Mini's on-device "Assign Buttons" menu
# (fw >= 4.1.0) where the four hardware buttons get bound to them — a deliberate
# press, unlike knob touch which the firmware overloads (usePotTouchSelections /
# openDetail). Sent to PORT_1, the same Electra Port the sidecar reads.
RECONCILE_LUA = """
function harpPush() midi.sendControlChange(PORT_1, 1, 110, 127) end
function harpPull() midi.sendControlChange(PORT_1, 1, 111, 127) end
function harpReadOnly() midi.sendControlChange(PORT_1, 1, 112, 127) end
function harpDuplicate() midi.sendControlChange(PORT_1, 1, 113, 127) end
preset.userFunctions = {
  pot1 = {call = harpPush, name = "Push to Synth"},
  pot2 = {call = harpPull, name = "Pull to DAW"},
  pot3 = {call = harpReadOnly, name = "Read-only"},
  pot4 = {call = harpDuplicate, name = "Duplicate"},
}
-- Host page-switch over MIDI is unreliable while the sidecar holds the ports
-- (09 0A is dropped; a control CC just drags focus). So switch ON-DEVICE: the
-- sidecar sends CC 119 with value = target page, and this handler flips the page
-- locally via pages.display(), the firmware's own page API.
function midi.onControlChange(midiInput, channel, controllerNumber, value)
  if controllerNumber == 119 and value >= 1 and value <= 12 then
    pages.display(value)          -- CC 119 = target page
  elseif controllerNumber == 118 then  -- CC 118 = dirty flag: relabel the headline
    local g = groups.get(1)
    if g then
      if value == 1 then g:setLabel("Synth has unsaved edits")
      else g:setLabel("Project and Synth differ") end
    end
  end
end
"""


def upload_lua(ctrl, script):
    """Upload the preset's Lua script (SysEx 01 0C)."""
    ctrl.write(ELECTRA_SYSEX + bytes([0x01, 0x0C]) + script.encode("ascii") + bytes([0xF7]))


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
    # "—" (U+2014) marks a slot the current engine ignores — skip it so the Electra
    # only shows the engine's real controls.
    HIDDEN = "—"
    params = [p for p in json.loads(panel.cmd("params"))["params"] if p["name"] != HIDDEN]

    # CTRL is a quiet command channel: used only at startup to upload the preset +
    # Lua and read their ACKs. We do NOT subscribe to its events or read it after
    # that. Page-switching is done ON-DEVICE by Lua (the host's 09 0A page-switch
    # was unreliable while the sidecar holds the ports, and a control CC just drags
    # focus); the action pads send their CC on the Electra Port instead.
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
    upload_lua(ctrl, RECONCILE_LUA)  # user functions for the Assign-Buttons menu
    lack = ctrl.read_window(0.5)
    if ELECTRA_SYSEX + b"\x7e\x00" in lack:
        sys.stderr.write("electra-panel: Lua upload NACK [%s]\n" % lack.hex())
    else:
        sys.stderr.write("electra-panel: reconcile Lua uploaded (Push/Pull/Read-only/Duplicate)\n")

    port = RawMidi(port_name, want_in=True, want_out=True)
    by_cc = {p["id"] for p in params}
    shown = {}  # param id -> last CC value exchanged, so we never echo a value back
    last_sig = tuple((p["id"], p["name"]) for p in params)  # detect a param-MAP change (engine switch)

    # Phase-2 reconcile: when the device parks a §11.4 offer (a shell found
    # live/project != its saved bundle), open the Reconcile page (CC 119 -> the Lua
    # pages.display handler) and let the user pick with a PAD click (the Electra's
    # "select"; a fader's turn bleeds into the next page's knob after the bounce).
    # The pad sends CC 110-113, caught in the port reader -> reconcile-choose.
    RECONCILE_PAGE = 3
    ACTIONS = ["Push to Synth", "Pull to DAW", "Read-only", "Duplicate"]
    ACTION_CC = {110: 0, 111: 1, 112: 2, 113: 3}  # pad-fired CC -> action index
    recon = {"active": False}

    # Multitimbral: the 8 knobs edit the FOCUSED part; the "Part" knob (CC 116) on
    # the Arp+Glide page picks it (1..16 -> part 0..15). The display poll + every
    # knob edit are scoped to focus["part"].
    NPARTS = 16
    PART_CC = 116
    focus = {"part": 0}

    def fire(idx):
        r = panel.cmd("reconcile-choose %d" % idx)
        sys.stderr.write("electra-panel: reconcile -> %s %s\n" % (ACTIONS[idx], r))
        port.write(bytes([0xB0 | MIDI_CH, 119, 1]))  # Lua -> back to Synth (page 1)
        recon["active"] = False

    def reader():  # the Electra Port: knob CCs in, plus the pad-fired action CCs
        mp = MidiParse()
        while True:
            data = port.read()
            if not data:
                time.sleep(0.005)
                continue
            for st, d1, d2 in mp.feed(data):
                if (st & 0xF0) != 0xB0:
                    continue
                if d1 in by_cc:                       # a knob moved -> edit the focused part
                    shown[d1] = d2
                    panel.cmd("knob %d %d %.5f" % (focus["part"], d1, d2 / 127.0))
                elif d1 == PART_CC:                   # the Part knob -> change focus
                    p = max(0, min(NPARTS - 1, int(d2) - 1))
                    if p != focus["part"]:
                        focus["part"] = p
                        shown.clear()                 # force the display to the new part
                        sys.stderr.write("electra-panel: focus part -> %d\n" % p)
                elif d1 in ACTION_CC and d2 >= 64 and recon["active"]:  # a pad fired
                    fire(ACTION_CC[d1])

    threading.Thread(target=reader, daemon=True).start()

    # display poll: the device's live param values -> CC out, so the Electra tracks
    # the device however it changed (web panel, a DAW, or a hardware front-panel move).
    period = 1.0 / POLL_HZ
    while True:
        # Offer FIRST: go active + open Reconcile BEFORE any param CC goes out this
        # iteration. The Electra follows an incoming CC to that control's page, so a
        # stray param CC after we open would drag the screen back off Reconcile.
        try:
            rj = json.loads(panel.cmd("reconcile-get"))
            if rj.get("pending") and not recon["active"]:
                recon["active"] = True
                # CC 118 = dirty flag (Lua relabels the headline), then CC 119 =
                # target page (Lua pages.display). Label set before the page shows.
                port.write(bytes([0xB0 | MIDI_CH, 118, 1 if rj.get("dirty") else 0]))
                port.write(bytes([0xB0 | MIDI_CH, 119, RECONCILE_PAGE]))
                sys.stderr.write("electra-panel: reconcile offer expect=%s live=%s%s -> Reconcile (Lua)\n"
                                 % (rj.get("expect"), rj.get("live"), " dirty" if rj.get("dirty") else ""))
        except (OSError, ValueError):
            pass
        # Param display CCs only when NOT reconciling (else they'd pull the screen off).
        if not recon["active"]:
            try:
                cur = [p for p in json.loads(panel.cmd("params %d" % focus["part"]))["params"]
                       if p["name"] != HIDDEN]
                sig = tuple((p["id"], p["name"]) for p in cur)
                if sig != last_sig:  # the engine swapped its param map -> rebuild the Electra preset
                    sys.stderr.write("electra-panel: param map changed -> re-uploading preset (%d params)\n" % len(cur))
                    upload_preset(ctrl, build_preset(cur)); ctrl.read_window(0.5); time.sleep(0.3)
                    upload_lua(ctrl, RECONCILE_LUA); ctrl.read_window(0.3)
                    shown.clear(); by_cc.clear(); by_cc.update(p["id"] for p in cur)
                    last_sig = sig
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
