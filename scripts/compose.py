#!/usr/bin/env python3
# compose.py COMPOSITION.json — render a LAYERED dark/modal composition across the
# HARP fleet (multi-synth, multi-timbre) and mix it balanced + clip-safe, then read
# it back for HARP fidelity. Each LAYER renders on its own device/engine, serially
# (the devices are single-claim); collisions with the soak's churn are retried.
#
# COMPOSITION.json = { name, seconds, layers:[ {tag, ep, sets:["id=val",...], notes,
#   note_period, level, pan(-1..1)} ] }.  ep = HARP_ETH_DEVICE (host:port).
# This is the growth path beyond single-line arps: add layers / length / harmonic
# motion / sections over time. Watch clipping — the mix normalizes to -0.4 dBFS.
import subprocess, wave, json, sys, os, re, time, numpy as np
ROOT = '/Users/jak/src/harp-win'
BUNDLE = subprocess.check_output(f'find {ROOT}/build-vst -name harp-shell.vst3 -type d|head -1',
                                 shell=True, text=True).strip()

def render(L, path, seconds):
    a = [f'{ROOT}/build-vst/harp-vst3-host', BUNDLE]
    for s in L['sets']: a += ['--set', s]
    for lf in L.get('lfo', []):  a += ['--lfo', lf]     # param MOTION: "ID=HZ[:pts[:shape]]"
    for rp in L.get('ramp', []): a += ['--ramp', rp]    # param SWEEP over the take: "ID=V0:V1"
    a += ['--channel', '1', '--part', '1', '--notes', L['notes'],
          '--note-period', str(L.get('note_period', 0.5)), '--seconds', str(seconds),
          '--realtime', '--out', path]
    env = dict(os.environ, HARP_ETH_DEVICE=L['ep'], HARP_RECONCILE_TIMEOUT_MS='0')
    rms = 0.0
    for _ in range(5):                      # retry past soak-churn collisions
        o = subprocess.run(a, env=env, capture_output=True, text=True, timeout=seconds + 35).stdout
        m = re.search(r'rms=([0-9.]+)', o); rms = float(m.group(1)) if m else 0.0
        if rms > 0.01: break
        time.sleep(5)
    return rms

def loadw(p):
    w = wave.open(p, 'rb'); n = w.getnframes(); ch = w.getnchannels()
    d = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64) / 32768
    d = d.reshape(-1, ch) if ch > 1 else d.reshape(-1, 1)
    return np.repeat(d, 2, axis=1) if d.shape[1] == 1 else d

def env_gain(env, n):                        # piecewise-linear amplitude ENVELOPE (dynamics/form)
    if not env: return None                   # env = [(frac, gain), ...] over the take
    fr = [pt[0] for pt in env]; gn = [pt[1] for pt in env]
    return np.interp(np.linspace(0, 1, n), fr, gn)

def main():
    spec = json.load(open(sys.argv[1])); secs = spec.get('seconds', 12); name = spec['name']
    out = os.path.expanduser(f'~/Desktop/muswav/{name}.wav')
    parts = []
    for i, L in enumerate(spec['layers']):
        p = f'/tmp/cmp_layer{i}.wav'; r = render(L, p, secs)
        print(f"  layer {i} [{L.get('tag','?')}] rms={r:.3f}{'  (FAILED)' if r <= 0.01 else ''}")
        parts.append((p, L.get('level', 0.7), L.get('pan', 0.0), L.get('env'), r))
    live = [(p, lvl, pan, env) for p, lvl, pan, env, r in parts if r > 0.01]
    if not live: print("composition FAILED — no layer rendered"); return
    ml = max(len(loadw(p)) for p, _, _, _ in live)
    Lm = np.zeros(ml); Rm = np.zeros(ml)
    for p, lvl, pan, env in live:
        d = loadw(p); ln = min(len(d), ml)
        srms = float(np.sqrt((d * d).mean())) + 1e-9
        g = (0.20 / srms) * lvl       # normalize EACH stem to ~0.2 RMS, THEN apply level —
                                       # so `level` is consistent loudness across synths
                                       # (jetson FM is ~10x louder than refdev raw)
        gl = np.sqrt((1 - pan) / 2) * 1.414 if pan else 1.0   # equal-power pan
        gr = np.sqrt((1 + pan) / 2) * 1.414 if pan else 1.0
        sL = d[:ln, 0] * g * gl; sR = d[:ln, 1] * g * gr
        eg = env_gain(env, ln)        # dynamics/form: shape the take's loudness over time
        if eg is not None: sL *= eg; sR *= eg
        Lm[:ln] += sL; Rm[:ln] += sR
    pk = max(np.abs(Lm).max(), np.abs(Rm).max(), 1e-9)
    if pk > 0.95: Lm *= 0.95 / pk; Rm *= 0.95 / pk            # clip-safe normalize
    inter = np.empty(ml * 2); inter[0::2] = Lm; inter[1::2] = Rm
    w = wave.open(out, 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes((np.clip(inter, -1, 1) * 32767).astype('<i2').tobytes()); w.close()
    print(f"-> {out}  ({len(live)} voices, pre-norm peak {pk:.3f})")
    os.system(f"python3 {ROOT}/scripts/harp-readback.py {out}")

main()
