#!/bin/bash
# archive-before-push-test — §11.4 archive-BEFORE-push ordering against the fake-hardware
# device (harp-deviced --port). Proves the displacing recall Push obeys the §11.4 contract
# encoded in do_restore (host/harp-probe.c:1199–1239):
#
#     (1) snapshot the dirty live -> device_head
#     (2) refset(archive/<ts>, NULL, &device_head, force=true)  — "archived device state as …"
#     (3) push_closure (transfer the saved closure)
#     (4) refset(live/project, expect=device_head, new=saved.hash, force=false)
#
# i.e. the DISPLACED device state is captured into an archive ref BEFORE the live ref is moved
# onto the saved hash — so a Push that overwrites the front-panel edits can never lose them: the
# pre-push state stays recoverable under archive/.
#
# Flow (model: gc-test.sh + recall-eth-test.sh):
#   baseline -> knob 3 0.1 + save (mutated+saved state) -> knob 3 0.9 (scramble live so the
#   restore has a REAL displacement) -> restore -> assert
#     * restore logged "archived device state as archive/"
#     * live/project now == the SAVED hash (the recall landed)
#     * a NEW archive ref exists pointing at the SCRAMBLED (pre-restore) state
#     * archive-hash != live-hash  (the archive holds the displaced state, not the new live —
#       which can only be true if the archive was written from device_head BEFORE the live CAS)
#
# Headless recall (HARP_RECONCILE_TIMEOUT_MS=0) still archives — do_restore archives
# unconditionally on a dirty/mismatched live; the timeout only gates the interactive dialog,
# not the archive step. Runs on any cloud runner (eth.yml). Exit 0 pass / 1 fail.
set -u
cd "$(dirname "$0")/.."

# Headless: no panel answers the loopback; the archive-before-push is on the Push code path
# regardless of the dialog timeout, so 0 (skip the dialog) keeps the test fast + deterministic.
export HARP_RECONCILE_TIMEOUT_MS=0

DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-47925}"
DEVDIR=archive-before-push-state   # workspace-RELATIVE (MSYS path-arg safety, same as gc-test)
STOREDIR=archive-before-push-store # the probe's local store — `save` writes the saved project
                                   # here, `restore` reads it back; MUST persist across invocations
DEVLOG=/tmp/archive-before-push-dev.log
RESTORELOG=/tmp/archive-before-push-restore.log
D="127.0.0.1:$PORT"
fail() { echo "ARCHIVE-BEFORE-PUSH FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; rm -rf "$DEVDIR" "$STOREDIR"' EXIT INT TERM
rm -rf "$DEVDIR" "$STOREDIR"; : > "$DEVLOG"
"$DEVICED" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

P() { "$PROBE" -d "$D" -s "$STOREDIR" "$@"; }

# `refs` is PLAIN TEXT: "  <name>  <12hex>  gen=N  clean/DIRTY". Parse with awk: col1=name,
# col2=12-hex. (NOT JSON.) hash_of <name> -> the 12-hex short hash of the ref, or empty.
hash_of() {
    P refs 2>/dev/null | awk -v n="$1" '$1==n {print $2; exit}'
}
# count of archive/ refs (names carry colons: archive/2026-06-26T12:34:56Z)
arch_count() { P refs 2>/dev/null | awk '$1 ~ /^archive\// {c++} END {print c+0}'; }

# baseline live hash (clean fresh device)
LIVE0=$(hash_of live/project)

# 1. mutate the live, then SAVE it into the probe store (the saved/target project).
#    `restore` reads the saved project from THIS local store, so `save` is required.
P knob 3 0.1 >/dev/null 2>&1 || fail "knob 3 0.1 (mutate) failed"
P save        >/dev/null 2>&1 || fail "save failed"
SAVED=$(hash_of live/project)   # the device live == the saved project right after save
[ -n "$SAVED" ] || fail "no live/project hash after save"

# 2. SCRAMBLE the live so restore faces a real displacement to archive.
P knob 3 0.9 >/dev/null 2>&1 || fail "knob 3 0.9 (scramble) failed"

ARCH_BEFORE=$(arch_count)

# 3. restore = the §12.2 reopen Push-with-archive path.
P restore >"$RESTORELOG" 2>&1 || { cat "$RESTORELOG"; fail "restore command failed"; }

# ── assertions ────────────────────────────────────────────────────────────────────────────
# (a) restore logged the archive step (the §11.4 ordering marker)
grep -q "archived device state as archive/" "$RESTORELOG" \
    || { cat "$RESTORELOG"; fail "restore did not log 'archived device state as archive/'"; }

# (b) the recall landed: live/project now == the SAVED hash
LIVE_NOW=$(hash_of live/project)
[ -n "$LIVE_NOW" ] || fail "no live/project hash after restore"
[ "$LIVE_NOW" = "$SAVED" ] \
    || fail "live/project after restore ($LIVE_NOW) != saved hash ($SAVED) — recall did not land"

# (c) a NEW archive ref appeared
ARCH_AFTER=$(arch_count)
[ "$ARCH_AFTER" -gt "$ARCH_BEFORE" ] \
    || fail "no new archive ref ($ARCH_BEFORE -> $ARCH_AFTER) — displaced state was not archived"

# (d) THE ORDERING PROOF: the archive ref points at the SCRAMBLED (pre-restore) state, i.e.
#     archive-hash != live-hash. The archive can only hold the displaced head if it was written
#     from device_head BEFORE the live ref was CAS'd onto the saved hash. An overwrite-then-
#     (or never-)archive would leave the archive equal to the new live (or absent).
ARCH_NAME=$(P refs 2>/dev/null | awk '$1 ~ /^archive\// {print $1; exit}')
[ -n "$ARCH_NAME" ] || fail "could not read the archive ref name"
ARCH_HASH=$(hash_of "$ARCH_NAME")
[ -n "$ARCH_HASH" ] || fail "could not read the archive ref hash ($ARCH_NAME)"
[ "$ARCH_HASH" != "$LIVE_NOW" ] \
    || fail "archive hash == live hash ($ARCH_HASH) — the displaced (scrambled) state was NOT preserved; archive was written AFTER the live overwrite (ordering violated)"
# and the archive must NOT be the saved/baseline either — it must be the distinct scramble
[ "$ARCH_HASH" != "$SAVED" ] \
    || fail "archive hash == saved hash — archive captured the wrong (post-recall) state"

# (e) cross-check on the FILESYSTEM (gc-test style) — the archive ref is materialized on disk
FS_ARCH=$(find "$DEVDIR/refs/archive" -type f 2>/dev/null | wc -l | tr -d ' ')
[ "$FS_ARCH" -ge 1 ] || fail "no archive ref on disk under $DEVDIR/refs/archive"

echo "ARCHIVE-BEFORE-PUSH PASS: baseline=$LIVE0 saved=$SAVED scramble->archive=$ARCH_HASH (!= live=$LIVE_NOW), archives $ARCH_BEFORE->$ARCH_AFTER (fs=$FS_ARCH), order verified"
kill -9 "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; DP=""
rm -rf "$DEVDIR" "$STOREDIR"
