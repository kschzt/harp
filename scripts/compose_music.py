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
      'm11':[0,3,7,10,14,17], 'm6':[0,3,7,9],             # lush minor-11 / bittersweet minor-6
      '7b9':[0,4,7,10,13], 'dim7':[0,3,6,9],              # noir dominant-b9 / fully-diminished passing
      'sus2':[0,2,7], '7':[0,4,7,10], '9':[0,4,7,10,14], 'm7b5':[0,3,6,10]}
MODE = {'aeolian':[0,2,3,5,7,8,10], 'dorian':[0,2,3,5,7,9,10], 'phrygian':[0,1,3,5,7,8,10],
        'harmonic_minor':[0,2,3,5,7,8,11], 'melodic_minor':[0,2,3,5,7,9,11],
        'phrygian_dominant':[0,1,4,5,7,8,10],            # phrygian w/ a MAJOR 3rd — flamenco/exotic dark
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

def gen_bass(prog):                          # root-anchored bassline with MOVEMENT (-1=rest) — rotating
    cells = [                                 # cells of root/5th/octave + a rest for groove, not static
        lambda b: [b, -1, b, b + 7],          # root, REST, root, 5th (the groove)
        lambda b: [b, b + 7, -1, b + 12],     # root, 5th, REST, octave (an ascent)
        lambda b: [b, -1, b + 7, b],          # root, REST, 5th, root (a rock)
        lambda b: [b + 12, b + 7, b, -1],     # octave, 5th, root, REST (a descent)
    ]
    out = []
    for i, (root, ct) in enumerate(prog):
        b = bass_root(root); out += cells[i % len(cells)](b)
    return out

def gen_arp(prog, reg_lo=52, reg_hi=74):     # VOICE-LED: chord tones placed in a STABLE
    out = []                                  # register so common tones recur + leaps are
    for root, ct in prog:                     # minimal between chords (smooth harmony, not
        pcs = set((root + i) % 12 for i in CH[ct])   # jumping octaves with the root)
        voicing = sorted(n for n in range(reg_lo, reg_hi + 1) if n % 12 in pcs)
        v = voicing[:5] if len(voicing) >= 5 else voicing
        if len(v) >= 4:                        # DROP-2: drop the 2nd-from-top an octave — opens the
            v = sorted(v[:-2] + [v[-2] - 12, v[-1]])   # close voicing into a lusher, wider spread
        pat = v + v[-2:0:-1]
        while len(pat) < 8: pat += pat
        out += pat[:8]
    return out

RHYTHMS = [[1, 1, 0, 1], [1, 0, 1, 1], [1, 1, 1, 0], [0, 1, 1, 1], [1, 0, 1, 0]]  # 0 = rest

def gen_melody(prog, sc, seed=7):            # motif + register arc + RHYTHM + PHRASE contour (Q&A), 4/bar
    random.seed(seed); out = []; n = len(prog); motif = [0, 2, 3, 1]
    for i, (root, ct) in enumerate(prog):
        tones = [root + t for t in CH[ct]]
        frac = i / (n - 1) if n > 1 else 0.0
        arc = int(round(5 * (1 - abs(frac - 0.66) / 0.66)))   # register peaks ~2/3 through
        cur = snap(random.choice(tones[:3]) + 7 + arc, sc)    # mid-high — audible, not buried
        mask = RHYTHMS[i % len(RHYTHMS)]                       # rotate rhythmic cells = phrasing
        rising = (i % 2 == 0)    # PHRASE: antecedent bars rise (a question), consequent fall (an answer)
        mi = 0; last = None
        for beat in range(4):
            if mask[beat] == 0:
                out.append(-1)                                # REST — the melody breathes
            else:
                if mi > 0:
                    d = abs(motif[mi % len(motif)])
                    d = d if rising else -d                    # bias the contour by phrase direction
                    if random.random() < 0.25: d = -d         # an occasional surprise turn
                    cur = snap(cur + d, sc)
                out.append(cur); mi += 1; last = len(out) - 1
        if not rising and last is not None:                   # CONSEQUENT cadence: land on a chord tone
            out[last] = min((snap(root + t + 7 + arc, sc) for t in CH[ct]), key=lambda x: abs(x - out[last]))
    return out

def gen_counter(prog, sc, seed=11):          # ANSWERS the lead — call-and-response. Lower
    random.seed(seed); out = []               # register, CONTRARY (descending) motion, and an
    cmotif = [0, -2, -1, -3]                   # INTERLOCKED rhythm: it plays in the lead's rests.
    for i, (root, ct) in enumerate(prog):
        tones = [root + t for t in CH[ct]]
        cur = snap(random.choice(tones[:3]) + 3, sc)     # mid — under the lead, over the arp
        mask = [1 - b for b in RHYTHMS[i % len(RHYTHMS)]] # COMPLEMENT of the lead's cell
        mi = 0
        for beat in range(4):
            if mask[beat] == 0:
                out.append(-1)                            # rest — yield to the lead
            else:
                if mi > 0:
                    cur = snap(cur + cmotif[mi % len(cmotif)], sc)
                out.append(cur); mi += 1
    return out

def gen_pad(prog):                          # ATMOSPHERIC pad: the chord root sustained ONE per bar —
    return [bass_root(root) + 12 for root, ct in prog]   # a mid drone an 8ve over the bass, long release

def section_env(nsec, lo=0.6, hi=1.0, peak=0.62):  # TERRACED dynamics — BUILD to a climax ~2/3 in, resolve
    span = max(peak, 1.0 - peak); pts = []          # (for A-B-A the peak lands on B; for A-B-C-A, on C)
    for i in range(nsec):
        c = (i + 0.5) / nsec                          # this section's centre fraction
        w = round(hi - (hi - lo) * abs(c - peak) / span, 3)
        a, b = i / nsec, (i + 1) / nsec
        pts += [[round(a + 0.03, 3), w], [round(b - 0.03, 3), w]]   # flat across the section, ramps between
    return [[0.0, pts[0][1]]] + pts + [[1.0, pts[-1][1]]]

def melody_section_env(nsec):              # the lead: silent INTRO, then follow the section dynamics
    return [[0.0, 0.0], [0.14, 0.0]] + [p for p in section_env(nsec, 0.62, 1.0) if p[0] > 0.14]

def add_frame(key, bass, arp, mel, counter, pad):   # a pad-only INTRO bar + a resolving CODA bar —
    pn = bass_root(key) + 12; bn = bass_root(key)     # open on the pad alone, close on the tonic with
    return ([-1, -1, -1, -1] + bass + [bn, -1, -1, -1],   # the pad sustaining (everyone else rests)
            [-1] * 8 + arp + [-1] * 8,
            [-1, -1, -1, -1] + mel + [-1, -1, -1, -1],
            [-1, -1, -1, -1] + counter + [-1, -1, -1, -1],
            [pn] + pad + [pn])

def reframe(env, nbars):       # remap a [0..1]-over-main env into the framed piece (+1 intro, +1 coda bar)
    tot = nbars + 2.0; lo, hi = 1.0 / tot, (nbars + 1.0) / tot
    return [[0.0, env[0][1]]] + [[round(lo + f * (hi - lo), 4), g] for f, g in env] + [[1.0, env[-1][1]]]

def _render(name, seconds, bar, bass, arp, mel, counter, pad, arp_ep, envs=None):  # build 5 voices + mix
    csv = lambda L: ','.join(str(int(x)) for x in L); E = envs or {}
    # FORM: the piece has an ARC. bass+arp carry a sparse, filter-closed INTRO and build;
    # the lead stays SILENT until ~1/4 in, enters for the climax, then everything RESOLVES.
    comp = {'name': name, 'seconds': seconds, 'layers': [
        {'tag': 'bass', 'ep': 'kria.local:47987',
         'sets': ['2=0.4', '4=0.22', '5=0.04', '6=0.55', '7=0.92'],
         'ramp': ['3=0.26:0.40'],                      # cutoff opens slowly — a low swell
         'env': E.get('bass', [[0,0.55],[0.18,0.8],[0.6,1.0],[0.88,1.0],[1.0,0.6]]),  # form arc (or per-section)
         'notes': csv(bass), 'note_period': round(bar / 4, 3), 'level': 1.0, 'pan': 0.0},
        {'tag': 'arp', 'ep': arp_ep,
         'sets': ['1=0.06', '12=0.55', '16=0.04', '17=0.45'],
         'lfo': ['13=0.06'],                            # FM brightness breathes (slow LFO)
         'env': E.get('arp', [[0,0.45],[0.12,0.7],[0.5,1.0],[0.85,1.0],[1.0,0.55]]),  # the bed builds in
         'notes': csv(arp), 'note_period': round(bar / 8, 3), 'level': 0.4, 'pan': -0.32},
        {'tag': 'melody', 'ep': 'kria.local:47987',
         'sets': ['2=0.55', '4=0.4', '5=0.02', '6=0.38', '7=0.95'],
         'ramp': ['3=0.40:0.90'],                       # cutoff BUILDS — the lead opens up
         'env': E.get('melody', [[0,0.0],[0.16,0.0],[0.26,1.0],[0.7,1.0],[0.9,0.85],[1.0,0.3]]),  # enters late, leads
         'notes': csv(mel), 'note_period': round(bar / 4, 3), 'level': 1.0, 'pan': 0.3},
        {'tag': 'counter', 'ep': 'kria.local:47987',    # ANSWERS the lead across the stereo field
         'sets': ['2=0.45', '3=0.42', '4=0.3', '5=0.03', '6=0.5', '7=0.9'],   # darker, FIXED cutoff
         'env': E.get('counter', [[0,0.45],[0.16,0.66],[0.5,0.6],[0.85,0.68],[1.0,0.42]]),  # carries intro, ducks
         'notes': csv(counter), 'note_period': round(bar / 4, 3), 'level': 0.72, 'pan': -0.18},
        {'tag': 'pad', 'ep': arp_ep,                    # ATMOSPHERIC sustained drone — depth under all
         'sets': ['1=0.06', '12=0.5', '13=0.28', '16=0.5', '17=0.85'],   # FM, dark, long attack + release
         'env': E.get('pad', [[0,0.4],[0.3,0.55],[0.7,0.6],[1.0,0.4]]),  # a quiet bed beneath the bed
         'notes': csv(pad), 'note_period': round(bar, 3), 'level': 0.5, 'pan': 0.0},
    ]}
    json.dump(comp, open(f'/tmp/{name}.json', 'w'))
    subprocess.run(['python3', '/Users/jak/src/harp-win/scripts/compose.py', f'/tmp/{name}.json'])

def compose(name, key, mode, prog, bpm=64, reps=2, seed=7, arp_ep='jetson.local:7777'):
    beat = 60.0 / bpm; bar = 4 * beat; chords = prog * reps; sc = scale_range(key, mode)
    bass = gen_bass(chords); arp = gen_arp(chords)
    mel = gen_melody(chords, sc, seed); counter = gen_counter(chords, sc, seed + 4); pad = gen_pad(chords)
    bass, arp, mel, counter, pad = add_frame(key, bass, arp, mel, counter, pad)   # pad-only INTRO + CODA bars
    seconds = round((len(chords) + 2) * bar + 2.0, 1)
    print(f"{name}: {mode} on key {key}, {len(chords)}+2 bars @ {bpm}bpm = {seconds}s; prog {prog} x{reps}")
    _render(name, seconds, bar, bass, arp, mel, counter, pad, arp_ep)

def compose_form(name, key, sections, bpm=60, reps_each=1, seed=7, arp_ep='jetson.local:7777'):
    # sections = [(mode, prog), ...] in order. A-B-A modal contrast = [A, B, A] over ONE tonic
    # (key), so only the MODE colour shifts between sections (e.g. aeolian->dorian->aeolian: just
    # the 6th moves). Each voice is generated per-section against that section's scale and
    # concatenated; the form-arc env spans the whole piece (so B, the middle, is the peak).
    beat = 60.0 / bpm; bar = 4 * beat
    bass = []; arp = []; mel = []; counter = []; pad = []; nbars = 0
    for si, sec in enumerate(sections):
        skey, mode, prog = sec if len(sec) == 3 else (key, sec[0], sec[1])  # per-section KEY = MODULATION
        chords = prog * reps_each; sc = scale_range(skey, mode)
        bass += gen_bass(chords); arp += gen_arp(chords); pad += gen_pad(chords)
        mel += gen_melody(chords, sc, seed + si); counter += gen_counter(chords, sc, seed + si + 4)
        nbars += len(chords)
    bass, arp, mel, counter, pad = add_frame(key, bass, arp, mel, counter, pad)   # pad-only INTRO + CODA bars
    seconds = round((nbars + 2) * bar + 2.0, 1)
    nsec = len(sections); se = reframe(section_env(nsec), nbars)   # terraced dynamics, shifted into the frame
    envs = {'bass': se, 'arp': se, 'counter': se, 'pad': se, 'melody': reframe(melody_section_env(nsec), nbars)}
    tags = '-'.join((f"{s[0]}:{s[1]}" if len(s) == 3 else s[0]) for s in sections)
    print(f"{name}: form [{tags}] on key {key}, {nbars}+2 bars @ {bpm}bpm = {seconds}s")
    _render(name, seconds, bar, bass, arp, mel, counter, pad, arp_ep, envs)

if __name__ == '__main__':
    # Movement XXII — an A-B-C-A DESCENT: each section sinks a whole step (E aeolian -> D aeolian ->
    # C aeolian) before climbing home to E — a feeling of falling deeper, the build-to-C dynamics
    # making the lowest point (C) also the loudest, a heavy dark trough rather than a bright crest.
    # The bittersweet minor-6 iv glints through each level. Then the homecoming lifts back to E.
    A = [(52, 'madd9'), (48, 'maj7'), (45, 'm6'), (52, 'madd9')]  # E aeolian: Em(add9) Cmaj7 Am6 Em(add9)
    B = [(50, 'madd9'), (46, 'maj7'), (43, 'm6'), (50, 'madd9')]  # D aeolian (down a step): Dm(add9) Bbmaj7 Gm6
    C = [(48, 'm9'), (44, 'maj7'), (41, 'm6'), (46, 'majadd9')]   # C aeolian (down again): Cm9 Abmaj7 Fm6 Bbadd9
    compose_form('movement-xxii-abca-descent', 52,
                 [('aeolian', A), (50, 'aeolian', B), (48, 'aeolian', C), ('aeolian', A)], bpm=60, reps_each=1, seed=103)
