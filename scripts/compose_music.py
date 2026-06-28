#!/usr/bin/env python3
# compose_music.py — ACTUAL dark music via functional harmony + voice leading, not
# test arps. A chord PROGRESSION drives three theory-generated voices and renders them
# across the HARP fleet (compose.py mixes the stems):
#   bass   — chord roots + the 5th, in octave 2 (a real bassline, root motion)
#   arp    — the chord tones up-then-down (the harmonic bed, 8/bar), on jetson FM
#   melody — a 4-note MOTIF developed over the changes, chord-tone-anchored on the
#            downbeat, snapped to the mode, with an overall register ARC (peaks ~2/3 in)
# Dark modes (aeolian/dorian/phrygian/harmonic-minor), min7/9 colour. Edit __main__.
import json, subprocess, random

CH = {'m9':[0,3,7,10,14], 'm7':[0,3,7,10], 'm':[0,3,7], 'madd9':[0,3,7,14],
      'maj9':[0,4,7,11,14], 'maj7':[0,4,7,11], 'majadd9':[0,4,7,14],
      'mmaj9':[0,3,7,11,14], 'mmaj7':[0,3,7,11],          # minor-MAJOR 7 — the noir/melodic-minor tonic
      'sus2':[0,2,7], '7':[0,4,7,10], '9':[0,4,7,10,14], 'm7b5':[0,3,6,10]}
MODE = {'aeolian':[0,2,3,5,7,8,10], 'dorian':[0,2,3,5,7,9,10], 'phrygian':[0,1,3,5,7,8,10],
        'harmonic_minor':[0,2,3,5,7,8,11], 'melodic_minor':[0,2,3,5,7,9,11],
        'minor':[0,2,3,5,7,8,10]}

def scale_range(key, mode, lo=36, hi=84):
    pcs = set((key + d) % 12 for d in MODE[mode])
    return [n for n in range(lo, hi + 1) if n % 12 in pcs]

def snap(n, sc): return min(sc, key=lambda x: abs(x - n))

def bass_root(root):
    b = root
    while b > 47: b -= 12
    while b < 35: b += 12
    return b

def gen_bass(prog):                          # root, REST, root, 5th — groove + space (-1=rest)
    out = []
    for root, ct in prog:
        b = bass_root(root); out += [b, -1, b, b + 7]
    return out

def gen_arp(prog, reg_lo=52, reg_hi=74):     # VOICE-LED: chord tones placed in a STABLE
    out = []                                  # register so common tones recur + leaps are
    for root, ct in prog:                     # minimal between chords (smooth harmony, not
        pcs = set((root + i) % 12 for i in CH[ct])   # jumping octaves with the root)
        voicing = sorted(n for n in range(reg_lo, reg_hi + 1) if n % 12 in pcs)
        v = voicing[:5] if len(voicing) >= 5 else voicing
        pat = v + v[-2:0:-1]
        while len(pat) < 8: pat += pat
        out += pat[:8]
    return out

RHYTHMS = [[1, 1, 0, 1], [1, 0, 1, 1], [1, 1, 1, 0], [0, 1, 1, 1], [1, 0, 1, 0]]  # 0 = rest

def gen_melody(prog, sc, seed=7):            # motif + arc + RHYTHM (rests = phrasing), 4/bar
    random.seed(seed); out = []; n = len(prog); motif = [0, 2, 3, 1]
    for i, (root, ct) in enumerate(prog):
        tones = [root + t for t in CH[ct]]
        frac = i / (n - 1) if n > 1 else 0.0
        arc = int(round(5 * (1 - abs(frac - 0.66) / 0.66)))   # register peaks ~2/3 through
        cur = snap(random.choice(tones[:3]) + 7 + arc, sc)    # mid-high — audible, not buried
        mask = RHYTHMS[i % len(RHYTHMS)]                       # rotate rhythmic cells = phrasing
        mi = 0
        for beat in range(4):
            if mask[beat] == 0:
                out.append(-1)                                # REST — the melody breathes
            else:
                if mi > 0:
                    d = motif[mi % len(motif)]; d = d if random.random() > 0.35 else -d
                    cur = snap(cur + d, sc)
                out.append(cur); mi += 1
    return out

def compose(name, key, mode, prog, bpm=64, reps=2, seed=7, arp_ep='jetson.local:7777'):
    beat = 60.0 / bpm; bar = 4 * beat; chords = prog * reps
    sc = scale_range(key, mode)
    bass = gen_bass(chords); arp = gen_arp(chords); mel = gen_melody(chords, sc, seed)
    seconds = round(len(chords) * bar + 2.0, 1)
    csv = lambda L: ','.join(str(int(x)) for x in L)
    # FORM: the piece has an ARC. bass+arp carry a sparse, filter-closed INTRO and build;
    # the lead stays SILENT until ~1/4 in, enters for the climax, then everything RESOLVES.
    comp = {'name': name, 'seconds': seconds, 'layers': [
        {'tag': 'bass', 'ep': 'kria.local:47987',
         'sets': ['2=0.4', '4=0.22', '5=0.04', '6=0.55', '7=0.92'],
         'ramp': ['3=0.26:0.40'],                      # cutoff opens slowly — a low swell
         'env': [[0,0.55],[0.18,0.8],[0.6,1.0],[0.88,1.0],[1.0,0.6]],   # gentle swell + settle
         'notes': csv(bass), 'note_period': round(bar / 4, 3), 'level': 1.0, 'pan': 0.0},
        {'tag': 'arp', 'ep': arp_ep,
         'sets': ['1=0.06', '12=0.55', '16=0.04', '17=0.45'],
         'lfo': ['13=0.06'],                            # FM brightness breathes (slow LFO)
         'env': [[0,0.45],[0.12,0.7],[0.5,1.0],[0.85,1.0],[1.0,0.55]],  # the bed builds in
         'notes': csv(arp), 'note_period': round(bar / 8, 3), 'level': 0.4, 'pan': -0.32},
        {'tag': 'melody', 'ep': 'kria.local:47987',
         'sets': ['2=0.55', '4=0.4', '5=0.02', '6=0.38', '7=0.95'],
         'ramp': ['3=0.40:0.90'],                       # cutoff BUILDS — the lead opens up
         'env': [[0,0.0],[0.16,0.0],[0.26,1.0],[0.7,1.0],[0.9,0.85],[1.0,0.3]],  # enters late, leads, fades
         'notes': csv(mel), 'note_period': round(bar / 4, 3), 'level': 1.0, 'pan': 0.3},
    ]}
    json.dump(comp, open(f'/tmp/{name}.json', 'w'))
    print(f"{name}: {mode} on key {key}, {len(chords)} bars @ {bpm}bpm = {seconds}s; prog {prog} x{reps}")
    subprocess.run(['python3', '/Users/jak/src/harp-win/scripts/compose.py', f'/tmp/{name}.json'])

if __name__ == '__main__':
    # Movement VII — D MELODIC MINOR (noir/jazz-minor): i(mMaj9) - iv9 - bVImaj7 - V7,
    # the raised C# gives the A7→i leading-tone cadence; the minor-major tonic is the sound
    # of film noir. Now with a real FORM ARC (sparse intro → lead enters → peak → resolve).
    P = [(50, 'mmaj9'), (43, 'm9'), (46, 'maj7'), (45, '7')]
    compose('movement-vii-melodicminor', 50, 'melodic_minor', P, bpm=56, reps=2, seed=37)
