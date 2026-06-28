#!/usr/bin/env python3
# harp-readback.py — a FIDELITY analyzer for HARP captures. Inspired by the synth
# folks' perceptual2.py, but inverted: perceptual2 asks "does it sound good?"
# (synth tuning); this asks "did HARP deliver the audio FAITHFULLY?" — it hunts
# transport/runtime DEFECTS in a read-back WAV, the kind a streaming bug leaves.
#
# DESIGN NOTE (learned the hard way on a real refdev capture): a naive per-sample
# "click" detector fires on every bright/aliased oscillator edge — 650 false hits
# on a CLEAN deterministic render. So the detectors here are built to be
# WAVEFORM-INDEPENDENT: they must read a defect-free synth render as clean.
#   - clicks  : single-sample SPIKES via 3-tap median residual. The median
#               PRESERVES a smooth peak and a monotonic waveform edge, but a 1-2
#               sample impulse juts out of it. Band-limited audio never deviates
#               a large fraction of its range in ONE sample; a transport spike does.
#   - gaps    : DROPOUTS = interior silence whose onset is a CLIFF (level → ~0 in
#               ≤2ms). A musical note-off RELEASE decays slower than that, so it is
#               NOT counted. (Drive continuous content in a soak and any gap is real.)
#   - stutter : exact-repeat of a buffer-sized block (ring re-read / pad replay).
#               A synth never bit-repeats a 256-sample block; the transport can.
#   - clip%/dc/active%/rms/pk : level + the "silence sails through an oracle" floor.
#   - cent/flux : gross spectral health (corruption shifts these).
#
# THE PRIMARY TRANSPORT ORACLE is compare-mode: --ref OFFLINE.wav LIVE.wav. The
# deterministic offline render is "what HARP should deliver"; the realtime capture
# is "what the transport delivered". After alignment, per-window correlation
# localizes where the live stream DIVERGED — synth-independent, because the
# identical waveform cancels. Low-correlation windows = real dropouts/glitches/drift.
#
# usage:  harp-readback.py CAPTURE.wav [CAPTURE2.wav ...]
#         harp-readback.py --ref OFFLINE.wav LIVE.wav      (transport deviation, PRIMARY)
#         harp-readback.py --json ...                      (machine-readable)
# Exit 0 = all clean; 1 = at least one file FLAGGED. (so a soak loop can gate on it.)
import sys, wave, json, numpy as np

def load(path):
    w = wave.open(path, 'rb')
    sr, n, ch, sw = w.getframerate(), w.getnframes(), w.getnchannels(), w.getsampwidth()
    raw = w.readframes(n); w.close()
    dt = {1: np.int8, 2: np.int16, 4: np.int32}.get(sw, np.int16)
    full = (1 << (8 * sw - 1))
    a = np.frombuffer(raw, dtype=dt).astype(np.float64) / full
    if ch > 1:
        a = a.reshape(-1, ch); mono = a.mean(axis=1)
    else:
        mono = a; a = a.reshape(-1, 1)
    return sr, ch, a, mono

def analyze(path):
    sr, ch, multi, d = load(path)
    n = len(d); dur = n / sr if sr else 0
    out = {'file': path.split('/')[-1], 'sr': sr, 'ch': ch, 'dur_s': round(dur, 2)}
    flags = []

    # ---- level / activity ----
    rms = float(np.sqrt((d * d).mean())) if n else 0.0
    pk = float(np.abs(d).max()) if n else 0.0
    floor = max(1e-4, pk * 0.02)                 # noise floor: 2% of peak (or -74 dBFS)
    active_pct = float((np.abs(d) > floor).mean() * 100) if n else 0.0
    clip_pct = float((np.abs(d) >= 0.999).mean() * 100) if n else 0.0
    dc = float(d.mean()) if n else 0.0
    out.update(rms=round(rms, 5), pk=round(pk, 4), active=round(active_pct, 1),
               clip=round(clip_pct, 3), dc=round(dc, 5))
    if rms < 1e-4:
        flags.append('SILENT (rms~0 — stream never delivered audio)')
    if active_pct < 5 and rms >= 1e-4:
        flags.append('mostly-silent (active<5%% — heavy dropout?)')
    if clip_pct > 0.1:
        flags.append('clipping %.2f%%' % clip_pct)
    if abs(dc) > 0.01:
        flags.append('DC offset %.4f (corruption)' % dc)

    # ---- gaps: DROPOUTS — interior silence whose ONSET is a cliff (not a note-off) ----
    gap_ms = 0.0; gap_n = 0
    if n and active_pct > 5:
        hop = max(1, sr // 1000)                              # 1ms envelope
        env = np.abs(d[:(n // hop) * hop].reshape(-1, hop)).max(axis=1)
        ef = env > floor
        act = np.where(ef)[0]
        if len(act) > 2:
            lo, hi = act[0], act[-1]
            f = ef[lo:hi + 1]; e = env[lo:hi + 1]
            i = 0; L = len(f)
            while i < L:
                if not f[i]:
                    j = i
                    while j < L and not f[j]: j += 1
                    run = j - i
                    # CLIFF onset test: level 2ms before the gap was loud and
                    # dropped to silence within ~2ms (a dropout), vs a slow note-off.
                    pre = e[max(0, i - 3):i]
                    cliff = (len(pre) and pre.max() > 4 * floor)
                    if run >= 8 and cliff:                    # >=8ms silence, sharp onset
                        gap_n += 1; gap_ms = max(gap_ms, float(run))
                    i = j
                else:
                    i += 1
    out.update(gaps=gap_n, gap_ms=round(gap_ms, 1))
    if gap_ms >= 12:
        flags.append('DROPOUT %dx worst %.0fms (cliff-onset silence)' % (gap_n, gap_ms))

    # ---- clicks: single-sample spikes via 3-tap median residual (waveform-robust) ----
    clicks = 0
    if n > 16 and pk > 1e-4:
        med = np.median(np.stack([d[:-2], d[1:-1], d[2:]]), axis=0)
        resid = np.abs(d[1:-1] - med)
        scale = np.percentile(np.abs(d), 99) + 1e-9          # robust signal peak
        clicks = int((resid > 0.5 * scale).sum())            # juts >50% of peak in 1 sample
    out.update(clicks=clicks)
    if clicks > 0:
        flags.append('%d spike(s) (impulse glitch / buffer seam)' % clicks)

    # ---- stutter: exact-repeat of a buffer-sized non-silent block (ring re-read) ----
    stutter = 0; stutter_b = 0
    for B in (64, 128, 256, 512):
        if n < 4 * B: continue
        nb = n // B
        blk = d[:nb * B].reshape(nb, B)
        eq = np.all(np.isclose(blk[1:], blk[:-1], atol=1e-7), axis=1)
        nz = (np.abs(blk[:-1]).max(axis=1) > floor)
        s = int((eq & nz).sum())
        if s > stutter: stutter, stutter_b = s, B
    out.update(stutter=stutter, stutter_block=stutter_b)
    if stutter > 0:
        flags.append('%d exact-repeat %d-smp block(s) (ring re-read / pad replay)' % (stutter, stutter_b))

    # ---- gross spectral health ----
    if n >= 4096:
        N, H = 4096, 2048; win = np.hanning(N)
        S = np.array([np.abs(np.fft.rfft(d[i:i + N] * win)) for i in range(0, n - N, H)])
        if len(S) > 1:
            freqs = np.fft.rfftfreq(N, 1.0 / sr)
            avg = S.mean(axis=0) + 1e-12
            cent = float((freqs * avg).sum() / avg.sum())
            diff = np.diff(S, axis=0)
            flux = float((np.sqrt((np.maximum(diff, 0) ** 2).sum(axis=1)) / (S[:-1].sum(axis=1) + 1e-9)).mean())
            out.update(cent=round(cent, 0), flux=round(flux, 4))

    out['flags'] = flags
    out['verdict'] = 'FLAG' if flags else 'clean'
    return out

def compare(ref_path, cap_path):
    # PRIMARY transport oracle: align the live capture to the deterministic offline
    # reference, then per-window correlation localizes where the stream diverged.
    sr, _, _, r = load(ref_path); _, _, _, c = load(cap_path)
    out = {'ref': ref_path.split('/')[-1], 'cap': cap_path.split('/')[-1]}
    flags = []
    # coarse global lag (live stream primes late) via cross-correlation, ±100ms
    lagmax = sr // 10
    wlen = min(len(r), len(c), sr * 2)
    a = r[:wlen] - r[:wlen].mean(); b = c[:wlen] - c[:wlen].mean()
    best_lag, best = 0, -1e18
    step = max(1, lagmax // 200)
    for lag in range(-lagmax, lagmax + 1, step):
        if lag >= 0: x, y = a[:wlen - lag], b[lag:wlen]
        else: x, y = a[-lag:wlen], b[:wlen + lag]
        if len(x) < 256: continue
        v = float((x * y).sum())
        if v > best: best, best_lag = v, lag
    # align
    if best_lag >= 0: R, C = r[:len(r) - best_lag], c[best_lag:]
    else: R, C = r[-best_lag:], c[:len(c) + best_lag]
    m = min(len(R), len(C)); R, C = R[:m], C[:m]
    out['drift_smp'] = best_lag
    rmse = float(np.sqrt(((R - C) ** 2).mean())) if m else 0.0
    out['rmse'] = round(rmse, 6)
    out['identical'] = bool(rmse < 1e-6)
    # per-window normalized correlation (50ms) — only over windows active in the ref
    W = max(1, int(sr * 0.05)); lowc = 0; worst = 1.0; worst_t = -1; nwin = 0
    rfloor = np.abs(R).max() * 0.02 + 1e-6
    for i in range(0, m - W, W):
        rr = R[i:i + W]; cc = C[i:i + W]
        if np.abs(rr).max() < rfloor: continue              # skip silent ref windows
        nwin += 1
        rr0 = rr - rr.mean(); cc0 = cc - cc.mean()
        den = (np.sqrt((rr0 * rr0).sum()) * np.sqrt((cc0 * cc0).sum())) + 1e-12
        corr = float((rr0 * cc0).sum() / den)
        if corr < 0.85: lowc += 1
        if corr < worst: worst, worst_t = corr, i / sr
    out['win'] = nwin; out['diverged'] = lowc
    out['worst_corr'] = round(worst, 3); out['worst_t_s'] = round(worst_t, 2)
    if lowc > 0:
        flags.append('%d/%d windows diverged (worst corr %.2f @ %.2fs) — transport defect' % (lowc, nwin, worst, worst_t))
    out['flags'] = flags
    out['verdict'] = 'FLAG' if flags else 'clean'
    return out

def main():
    args = [a for a in sys.argv[1:] if a != '--json']
    as_json = '--json' in sys.argv[1:]
    def guard(fn, label, *a):
        try: return fn(*a)
        except Exception as e: return {'file': label, 'flags': ['ERROR: %s' % e], 'verdict': 'FLAG'}
    if '--ref' in args:
        i = args.index('--ref'); ref = args[i + 1]; caps = args[i + 2:]
        results = [guard(compare, c.split('/')[-1], ref, c) for c in caps]
    else:
        results = [guard(analyze, p.split('/')[-1], p) for p in args]
    if not results:
        print('usage: harp-readback.py [--json] [--ref OFFLINE.wav] CAPTURE.wav ...', file=sys.stderr)
        return 2
    bad = 0
    if as_json:
        print(json.dumps(results, indent=2))
        bad = sum(1 for r in results if r.get('verdict') == 'FLAG')
    else:
        for r in results:
            v = r.pop('verdict'); f = r.pop('flags', [])
            name = r.pop('file', None) or '%s<-%s' % (r.get('cap', '?'), r.get('ref', '?'))
            line = ' '.join('%s=%s' % (k, r[k]) for k in r if k not in ('cap', 'ref'))
            mark = '  <<< ' + '; '.join(f) if v == 'FLAG' else '  ok'
            print('%-26s %s%s' % (name, line, mark))
            if v == 'FLAG': bad += 1
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
