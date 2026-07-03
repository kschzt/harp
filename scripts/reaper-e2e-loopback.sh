#!/usr/bin/env bash
# reaper-e2e-loopback.sh — headless REAPER §8.7 LOOPBACK end-to-end, on windows-2022,
# NO hardware. This is the Windows port of scripts/reaper-e2e.sh (which drives the real
# USB Pi under xvfb in hw.yml); it closes HARP's "no Windows DAW e2e" residual on the
# cloud runner by swapping the USB device for a localhost harp-deviced synth, exactly
# the way scripts/offline-golden-eth.sh fakes the hardware (fresh-state daemon per
# render + HARP_ETH_DEVICE=127.0.0.1:PORT dial).
#
# REAPER (a real third-party VST3 host) loads harp-shell.vst3, builds a MIDI chord on a
# HARP RefDev track and renders OFFLINE to WAV; the plugin dials the daemon over TCP and,
# because the DAW is rendering offline (kOffline), negotiates the deterministic host-paced
# §8.3-over-§8.7 bounce. The SAME cross-platform ReaScript (scripts/reaper-e2e.lua) is the
# __startup script — the device comes from the env, so the Lua is reused unchanged except
# it now picks 32-bit-float output when HARP_E2E_RENDER_FMT=f32 (see that file).
#
# Milestones (each prints PASS/FAIL so a CI log shows exactly how far the pipe got):
#   M0  binaries present (device + probe + REAPER + staged VST3)
#   M1  REAPER SCANNED harp-shell           (else a render would be silence)
#   M2  render exists AND non-silent (rms>0.02)  — the true DAW->plugin->loopback->audio proof
#   M3  two renders vs two FRESH daemons are byte-identical (data-chunk sha256) — determinism
#   Tier-B  optional drift-pin, DORMANT until a hash is captured from a green run
#
# It also ECHOES each harp-deviced log so we can SEE whether the dial was host-paced
# (kOffline propagated -> deterministic) or free-running RTP — the crux this run answers.
#
# Env overrides: HARP_REAPER, DEVICED, PROBE, HARP_VST3_DIR, PIN_REAPER_WIN.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
cd "$ROOT" || { echo "FAIL: cannot cd to repo root $ROOT"; exit 1; }

REAPER=${HARP_REAPER:-/c/REAPER/reaper.exe}
DEVICED=${DEVICED:-$ROOT/build-dev/harp-deviced.exe}
PROBE=${PROBE:-$ROOT/build-dev/harp-probe.exe}
VST3_DIR=${HARP_VST3_DIR:-/c/Program Files/Common Files/VST3}
PY=$(command -v python3 || command -v python)

# REAPER's resource dir. A reaper.ini next to reaper.exe SHOULD make REAPER run PORTABLE
# (resource dir = install dir C:\REAPER); the workflow ALSO seeds one in the roaming
# profile so that WHEREVER REAPER decides its resource dir is, it finds a config and never
# blocks on the first-run wizard (a headless modal = no scan = the empty-cache symptom).
# We do NOT trust the portable heuristic blindly: after the warm-up scan we adopt whichever
# candidate REAPER actually wrote its plugin cache into, so the M1 scan CHECK matches where
# REAPER writes. Candidate resource dirs, install-dir first then the roaming profile:
res_candidates() {
    printf '%s\n' /c/REAPER
    [ -n "${APPDATA:-}" ]     && printf '%s\n' "$(cygpath -u "$APPDATA" 2>/dev/null)/REAPER"
    [ -n "${USERPROFILE:-}" ] && printf '%s\n' "$(cygpath -u "$USERPROFILE" 2>/dev/null)/AppData/Roaming/REAPER"
}
uniq_res()  { res_candidates | awk 'NF && !seen[$0]++'; }
cache_in()  { echo "$1/reaper-vstplugins64.ini"; }

# RES/CACHE/SCRIPTS_DIR/INI are RE-RESOLVED for real in M1 (post-scan). Seed a best guess
# (the portable install dir) so M0 can print it and cleanup() has a value under `set -u`.
RES=/c/REAPER
CACHE=$(cache_in "$RES"); SCRIPTS_DIR="$RES/Scripts"; INI="$RES/reaper.ini"

# Repo-relative output dir (also the CI artifact dir). Keep it INSIDE the workspace so
# the harp-deviced --state-dir stays a MinGW-friendly relative path — Git Bash rewrites
# an absolute /tmp arg to C:/... whose drive component trips the device's recursive mkdir
# (the same gotcha offline-golden-eth.sh documents). REAPER needs a native path, so we
# hand the Lua the mixed (C:/…) form and read the files back through the POSIX form.
OUT="$ROOT/reaper-e2e-out"
OUT_M=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")
rm -rf "$OUT"; mkdir -p "$OUT"

fail=0
say()  { echo "[reaper-e2e-loopback] $*"; }
milestone() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }

cleanup() {
    rm -f "$SCRIPTS_DIR/__startup.lua" 2>/dev/null
    taskkill //F //IM reaper.exe //T >/dev/null 2>&1 || true
    # leave $OUT for the CI artifact upload; clean the daemon state dirs
    rm -rf "$ROOT"/reaper-eth-dev-* 2>/dev/null
}
trap cleanup EXIT INT TERM

# ── M0: everything built + staged? ───────────────────────────────────────────────────
STAGED_VST=$(find "$VST3_DIR" -maxdepth 2 -iname 'harp-shell.vst3' 2>/dev/null | head -1)
[ -x "$REAPER" ] || { echo "FAIL M0: REAPER not at $REAPER"; exit 1; }
[ -x "$DEVICED" ] || { echo "FAIL M0: harp-deviced not at $DEVICED"; exit 1; }
[ -x "$PROBE" ]   || { echo "FAIL M0: harp-probe not at $PROBE"; exit 1; }
[ -n "$STAGED_VST" ] || { echo "FAIL M0: harp-shell.vst3 not staged under $VST3_DIR"; exit 1; }
[ -n "$PY" ] || { echo "FAIL M0: no python3/python on PATH"; exit 1; }
milestone "M0 (binaries + staged VST3 present)" 0
say "REAPER=$REAPER  DEVICED=$DEVICED  PROBE=$PROBE"
say "staged VST3=$STAGED_VST"
say "resource-dir candidates (REAPER may use any; M1 adopts the one it scans into):"
for d in $(uniq_res); do
    c=$(cache_in "$d")
    say "  $d  [reaper.ini: $([ -f "$d/reaper.ini" ] && echo present || echo absent) | cache: $([ -f "$c" ] && echo present || echo absent)]"
done

# ── M1: REAPER must SCAN the shell (warm-up launch with NO __startup present) ─────────
# scanned_dir : echo the FIRST candidate whose cache NAMES harp-shell (the success signal).
scanned_dir() {
    for d in $(uniq_res); do
        grep -qiE 'harp[-_]shell|HARP RefDev' "$(cache_in "$d")" 2>/dev/null && { echo "$d"; return 0; }
    done
    return 1
}
# any_cache_dir : echo the first candidate with a NON-EMPTY cache (REAPER scanned SOMETHING
# there) — used to tell "scanned but the shell was rejected" apart from "never scanned".
any_cache_dir() {
    for d in $(uniq_res); do
        [ -s "$(cache_in "$d")" ] && { echo "$d"; return 0; }
    done
    return 1
}
# snapshot : copy every candidate cache + reaper.ini + the staged bundle tree into $OUT so
# the artifact upload carries a full diagnosis even when M1 fails (task: dump ALL candidates).
snapshot() {
    { echo "=== staged VST3 bundle tree ($STAGED_VST) ==="; find "$STAGED_VST" 2>/dev/null
      echo; echo "=== bundle parent dir ==="; ls -la "$(dirname "$STAGED_VST")" 2>/dev/null
      win="$STAGED_VST/Contents/x86_64-win"
      [ -d "$win" ] && { echo; echo "=== module dir $win (co-located runtime DLLs land here) ==="; ls -la "$win"; }
    } >"$OUT/staging.txt" 2>&1
    for d in $(uniq_res); do
        tag=$(printf '%s' "$d" | tr -c 'A-Za-z0-9' '_')
        c=$(cache_in "$d"); [ -f "$c" ]            && cp "$c" "$OUT/cache_${tag}.ini" 2>/dev/null || true
        [ -f "$d/reaper.ini" ]                     && cp "$d/reaper.ini" "$OUT/reaperini_${tag}.ini" 2>/dev/null || true
    done
}

if ! scanned_dir >/dev/null; then
    say "warm-up launch to scan the VST3 folder (no __startup present) ..."
    timeout 90 "$REAPER" -nosplash -noactivate >"$OUT/warmup.reaper.log" 2>&1 &
    wpid=$!
    for _ in $(seq 1 80); do scanned_dir >/dev/null && break; sleep 1; done
    # GRACEFUL close FIRST so REAPER FLUSHES reaper-vstplugins64.ini to disk — a //F
    # force-kill loses an unflushed cache, the prime suspect for the empty-cache M1 fail
    # (the Linux path exits REAPER via SIGTERM, which flushes; taskkill //F does not).
    taskkill //IM reaper.exe //T >/dev/null 2>&1 || true
    for _ in $(seq 1 12); do kill -0 "$wpid" 2>/dev/null || break; sleep 1; done
    taskkill //F //IM reaper.exe //T >/dev/null 2>&1 || true   # fallback if it ignored WM_CLOSE
    kill "$wpid" 2>/dev/null || true; wait "$wpid" 2>/dev/null || true
    sleep 2   # let the on-exit flush settle before we judge
fi

snapshot   # capture caches/inis/bundle for artifacts regardless of the verdict below

if RES_HIT=$(scanned_dir); then
    RES="$RES_HIT"; CACHE=$(cache_in "$RES"); SCRIPTS_DIR="$RES/Scripts"; INI="$RES/reaper.ini"
    say "resolved resource-dir=$RES  cache=$CACHE"
    echo "  --- $CACHE (full) ---"; sed 's/^/  | /' "$CACHE" 2>/dev/null
    milestone "M1 (REAPER scanned harp-shell — resource-dir=$RES)" 0
elif SOME=$(any_cache_dir); then
    # REAPER DID scan (a non-empty cache exists) but harp-shell is not named in it: the
    # module was FOUND but REJECTED (failed to LOAD — a missing runtime dep or a bad DLL).
    RES="$SOME"; CACHE=$(cache_in "$RES"); SCRIPTS_DIR="$RES/Scripts"; INI="$RES/reaper.ini"
    echo "FAIL M1: REAPER scanned (cache=$CACHE) but harp-shell is NOT in it"
    echo "         -> the shell was FOUND but the module FAILED TO LOAD (missing runtime dep / bad DLL) -> would render silence"
    echo "  --- $CACHE (full) ---"; sed 's/^/  | /' "$CACHE" 2>/dev/null
    exit 1
else
    # No cache in ANY candidate: REAPER never completed a scan pass at all.
    echo "FAIL M1: REAPER wrote NO plugin cache in ANY candidate resource dir"
    echo "         candidates: $(uniq_res | tr '\n' ' ')"
    echo "         -> REAPER never completed a scan (first-run modal? crash? a resource dir not in the list) -> cannot proceed"
    echo "  --- warm-up REAPER log (tail) ---"; tail -40 "$OUT/warmup.reaper.log" 2>/dev/null | sed 's/^/  | /'
    exit 1
fi

# install the SHARED ReaScript as REAPER's auto-run startup script (device via the env)
mkdir -p "$SCRIPTS_DIR"
rm -f "$SCRIPTS_DIR/__startup.lua"      # ensure the render passes start from a clean slot
cp "$HERE/reaper-e2e.lua" "$SCRIPTS_DIR/__startup.lua"

# REAPER reopens the last (now-deleted) project + can pop a crash-recovery modal; scrub
# that state before every launch so it always starts clean (mirrors reaper-e2e.sh).
scrub_ini() {
    [ -f "$INI" ] && sed -i -E '/^projecttab/d; /^(faultyproject|lastproject|numrecent|hasrecentsec)=/d; /^recent[0-9]+=/d' "$INI"
    return 0
}

# pin the device to a known, non-default state over TCP (NOT -d usb): knobs 1-7=0.5,
# arp (knob 8) OFF so determinism never rides transport-anchored arp timing — identical
# to reaper-e2e.sh's pin(0.5). Best-effort: a fresh-state daemon is already deterministic
# (offline-golden proves this without any pin), so a knob hiccup can't make M3 vacuous.
pin() {
    p="$1"; miss=0
    for i in 1 2 3 4 5 6 7; do "$PROBE" -d "127.0.0.1:$p" knob "$i" 0.5 >/dev/null 2>&1 || miss=1; done
    "$PROBE" -d "127.0.0.1:$p" knob 8 0.0 >/dev/null 2>&1 || miss=1
    [ "$miss" = 0 ] || say "warn: a knob pin on :$p failed (fresh-state default is still deterministic)"
}

# render NAME PORT: start a FRESH-state daemon on PORT, pin it, run REAPER once to render
# through the shell over §8.7 host-paced TCP, then stop the daemon and echo its log.
render() {
    name="$1"; port="$2"
    sd="reaper-eth-dev-$port"                     # relative => MinGW-mkdir-safe
    rm -rf "$sd"; dlog="$OUT/dev-$port.log"; : > "$dlog"
    "$DEVICED" --port "$port" --state-dir "$sd" --panel-sock "" >"$dlog" 2>&1 &
    dpid=$!
    i=0
    while [ $i -lt 50 ]; do grep -q "listening on $port" "$dlog" 2>/dev/null && break; sleep 0.2; i=$((i + 1)); done
    if ! grep -q "listening on $port" "$dlog" 2>/dev/null; then
        echo "FAIL: harp-deviced did not come up on $port -> render would be SILENCE"
        sed 's/^/  | /' "$dlog"; kill -9 "$dpid" 2>/dev/null; fail=1; return 1
    fi
    pin "$port"

    rm -f "$OUT/$name.wav" "$OUT/status.txt"
    scrub_ini
    say "render '$name' via REAPER against harp-deviced :$port (kOffline -> host-paced) ..."
    HARP_ETH_DEVICE="127.0.0.1:$port" HARP_RECONCILE_TIMEOUT_MS=0 \
    HARP_E2E_RENDER_FMT=f32 HARP_E2E_MODE=build \
    HARP_E2E_OUTDIR="$OUT_M" HARP_E2E_NAME="$name" HARP_E2E_STATUS="$OUT_M/status.txt" \
        timeout 150 "$REAPER" -nosplash >"$OUT/$name.reaper.log" 2>&1 &
    rpid=$!
    # poll the ReaScript's status file — REAPER does not self-exit, so we drive it: as
    # soon as the deferred render reports done (or errors), force-kill reaper.exe. The
    # `timeout` above and this loop's cap are the hung/modal watchdog the task asks for.
    done=0
    for _ in $(seq 1 130); do
        grep -q rendered "$OUT/status.txt" 2>/dev/null && { done=1; break; }
        grep -q ERROR    "$OUT/status.txt" 2>/dev/null && break
        kill -0 "$rpid" 2>/dev/null || break     # reaper died / timeout fired
        sleep 1
    done
    taskkill //F //IM reaper.exe //T >/dev/null 2>&1 || true
    kill "$rpid" 2>/dev/null || true; wait "$rpid" 2>/dev/null || true
    say "  status: $(cat "$OUT/status.txt" 2>/dev/null || echo '<none>')  (done=$done)"

    kill -9 "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null || true
    echo "  --- harp-deviced(:$port) log — DIAL MODE OBSERVATION (host-paced vs free-running RTP) ---"
    sed 's/^/  | /' "$dlog"
    if grep -qi 'host-paced' "$dlog"; then
        echo "  >> DIAL :$port = HOST-PACED (kOffline propagated through REAPER's offline render)"
    else
        echo "  >> DIAL :$port = NO host-paced marker (REAPER may have rendered real-time -> free-running RTP -> non-deterministic)"
    fi

    if [ ! -f "$OUT/$name.wav" ]; then
        echo "FAIL: no render output $OUT/$name.wav"
        echo "  --- REAPER log tail ---"; tail -30 "$OUT/$name.reaper.log" 2>/dev/null | sed 's/^/  | /'
        fail=1; return 1
    fi
    return 0
}

# ── M2 + M3: render twice against two fresh daemons (17990/17991), fresh state dirs ────
render r1 17990
render r2 17991

# M2 — the residual-closer: the DAW->plugin->loopback->audio path produced real signal.
if [ -f "$OUT/r1.wav" ]; then
    rms=$("$PY" "$HERE/wav-rms.py" rms "$OUT/r1.wav" 2>/dev/null)
    echo "──── M2 non-silent: r1 rms=${rms:-<none>}  (silence sails through determinism, so this floor is required)"
    if "$PY" "$HERE/wav-rms.py" rms "$OUT/r1.wav" 0.02 >/dev/null 2>&1; then
        milestone "M2 (render is NON-SILENT — real DAW->plugin->loopback->audio, rms=$rms)" 0
    else
        milestone "M2 (render SILENT rms=$rms)" 1
    fi
else
    milestone "M2 (no r1.wav to measure)" 1
fi

# M3 — determinism: the two data chunks (PCM only, not the metadata REAPER stamps) match.
h1=""; h2=""
[ -f "$OUT/r1.wav" ] && h1=$("$PY" "$HERE/wav-rms.py" datahash "$OUT/r1.wav" 2>/dev/null)
[ -f "$OUT/r2.wav" ] && h2=$("$PY" "$HERE/wav-rms.py" datahash "$OUT/r2.wav" 2>/dev/null)
echo "──── M3 determinism: data-chunk sha256  r1=${h1:-<none>}  r2=${h2:-<none>}"
if [ -n "$h1" ] && [ "$h1" = "$h2" ]; then
    milestone "M3 (two REAPER renders byte-identical through §8.7 — data-chunk $h1)" 0
else
    milestone "M3 (renders DIFFER — offline bounce not deterministic, see the DIAL MODE lines above)" 1
fi

# ── Tier-B drift pin: ARMED. Absolute ground-truth hash of the REAPER offline render's
# WAV data-chunk, captured from two INDEPENDENT green runs on fresh windows-2022 runners
# (byte-identical across machines: MSVC build -> REAPER offline render -> §8.7 host-paced
# device -> WAV is bit-exact), catching silent DSP/engine drift that run-to-run (Tier-A)
# cannot. Override/disable via the env (PIN_REAPER_WIN= to skip). Re-capture the printed
# r1 hash here if the shell DSP, the note input, or the pinned REAPER version changes.
PIN_REAPER_WIN=${PIN_REAPER_WIN-"46f6160f1e8e7ef914a6fbfdad24ba1311d8fb20a539a3bfb6d7d22a313d115e"}
if [ -n "$PIN_REAPER_WIN" ]; then
    if [ -n "$h1" ] && [ "$h1" = "$PIN_REAPER_WIN" ]; then
        milestone "Tier-B (data-chunk matches pinned ground truth $PIN_REAPER_WIN)" 0
    else
        milestone "Tier-B (DRIFTED from pin: got $h1 expected $PIN_REAPER_WIN — engine changed?)" 1
    fi
else
    echo "──── Tier-B pin DORMANT: capture r1=$h1 from a green run to arm PIN_REAPER_WIN"
fi

echo "════ reaper-e2e-loopback: $([ "$fail" = 0 ] && echo 'ALL PASS' || echo 'FAILURES ABOVE')"
exit "$fail"
