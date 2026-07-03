#!/usr/bin/env python3
# wav-rms.py — a tiny, dependency-free (stdlib only: struct/math/hashlib) WAV helper
# for the headless-REAPER e2e (scripts/reaper-e2e-loopback.sh). Two jobs the Python
# `wave` module can't do for us:
#
#   1. rms FILE [THRESHOLD]  — the NON-SILENT floor (M2). Prints the RMS of the audio.
#      With a THRESHOLD it exits 0 iff rms > THRESHOLD (so `wav-rms.py rms x.wav 0.02`
#      gates "the render carried real audio, not silence"). We roll our own decode
#      because the render is 32-bit FLOAT WAV (format tag 3) — stdlib `wave` only reads
#      integer PCM and raises "unknown format: 3" on float files.
#
#   2. datahash FILE — the DETERMINISM oracle (M3). Prints sha256 of the `data`-chunk
#      PAYLOAD ONLY. REAPER stamps a render timestamp / BWF metadata into other chunks,
#      so hashing the whole file would spuriously differ run-to-run; the audio samples
#      (the `data` chunk) are what must be byte-identical. We WALK the RIFF chunk list
#      to find `data` — we do NOT assume the classic 44-byte header, because REAPER may
#      emit an extended fmt (WAVE_FORMAT_EXTENSIBLE), a `fact`, a `bext`/BWF or a `JUNK`
#      chunk ahead of `data`.
#
# --self-check runs the whole thing against WAVs this script generates in memory
# (int16 sine, float32 sine, silence, and a file with a JUNK chunk BEFORE `data` so the
# header is not 44 bytes) — proving the RMS floor and the chunk parser before CI leans
# on them. Exit 0 = ok.
import sys, struct, math, hashlib, os, tempfile


def parse_wav(path):
    """Return dict(fmt, channels, rate, bits, data) by walking RIFF chunks.
    Robust to extra chunks and a non-44-byte header; tolerant of a data size
    field that over-runs the file (some writers pad/round)."""
    with open(path, "rb") as f:
        buf = f.read()
    if len(buf) < 12 or buf[0:4] != b"RIFF" or buf[8:12] != b"WAVE":
        raise ValueError("%s: not a RIFF/WAVE file" % path)
    pos, n = 12, len(buf)
    fmt = None
    data = None
    while pos + 8 <= n:
        cid = buf[pos:pos + 4]
        size = struct.unpack_from("<I", buf, pos + 4)[0]
        body = pos + 8
        if cid == b"fmt ":
            fmt = buf[body:body + size]
        elif cid == b"data":
            # clamp to what's actually present (a declared size past EOF => truncate)
            data = buf[body:min(body + size, n)]
        pos = body + size + (size & 1)   # chunks are word-aligned: pad odd sizes
    if fmt is None:
        raise ValueError("%s: no fmt chunk" % path)
    if data is None:
        raise ValueError("%s: no data chunk" % path)
    tag, channels, rate, _byterate, _align, bits = struct.unpack_from("<HHIIHH", fmt, 0)
    if tag == 0xFFFE and len(fmt) >= 26:
        # WAVE_FORMAT_EXTENSIBLE: the real tag is the first 2 bytes of the SubFormat GUID.
        tag = struct.unpack_from("<H", fmt, 24)[0]
    return dict(fmt=tag, channels=channels, rate=rate, bits=bits, data=data)


def samples(w):
    """Decode the data chunk to a flat list of floats in [-1, 1] (all channels
    interleaved — fine for a whole-signal RMS)."""
    tag, bits, d = w["fmt"], w["bits"], w["data"]
    if tag == 3:                                   # IEEE float
        if bits == 32:
            c = len(d) // 4
            return list(struct.unpack("<%df" % c, d[:c * 4]))
        if bits == 64:
            c = len(d) // 8
            return list(struct.unpack("<%dd" % c, d[:c * 8]))
    elif tag == 1:                                 # PCM integer
        if bits == 8:                              # 8-bit PCM is UNSIGNED
            return [(b - 128) / 128.0 for b in d]
        if bits == 16:
            c = len(d) // 2
            return [x / 32768.0 for x in struct.unpack("<%dh" % c, d[:c * 2])]
        if bits == 24:
            out = []
            for i in range(0, len(d) - 2, 3):
                v = d[i] | (d[i + 1] << 8) | (d[i + 2] << 16)
                if v & 0x800000:
                    v -= 1 << 24
                out.append(v / 8388608.0)
            return out
        if bits == 32:
            c = len(d) // 4
            return [x / 2147483648.0 for x in struct.unpack("<%di" % c, d[:c * 4])]
    raise ValueError("unsupported WAV (format tag=%d bits=%d)" % (tag, bits))


def rms_of(path):
    w = parse_wav(path)
    s = samples(w)
    if not s:
        return 0.0, w
    return math.sqrt(sum(x * x for x in s) / len(s)), w


def data_hash(path):
    return hashlib.sha256(parse_wav(path)["data"]).hexdigest()


# ── self-check (no CI dependency: generates its own fixtures) ────────────────────────
def _encode(tag, bits, floats):
    if tag == 3 and bits == 32:
        return struct.pack("<%df" % len(floats), *floats)
    if tag == 1 and bits == 16:
        clip = [max(-32768, min(32767, int(round(x * 32767)))) for x in floats]
        return struct.pack("<%dh" % len(clip), *clip)
    raise ValueError("test encoder: unsupported")


def _write_riff(path, tag, bits, channels, rate, data, pre_chunks=()):
    align = channels * (bits // 8)
    fmt = struct.pack("<HHIIHH", tag, channels, rate, rate * align, align, bits)
    chunks = [(b"fmt ", fmt)] + list(pre_chunks) + [(b"data", data)]
    body = b""
    for cid, cb in chunks:
        body += cid + struct.pack("<I", len(cb)) + cb + (b"\x00" if len(cb) & 1 else b"")
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body)


def _self_check():
    rate, n = 48000, 4800
    sine = [0.7 * math.sin(2 * math.pi * 440 * i / rate) for i in range(n)]
    silence = [0.0] * n
    tmp = tempfile.mkdtemp(prefix="wavrms-")
    ok = True

    def check(desc, cond):
        nonlocal ok
        print("  %-52s %s" % (desc, "ok" if cond else "FAIL"))
        ok = ok and cond

    # int16 sine -> RMS well above the 0.02 floor (0.7/sqrt(2) ~= 0.495)
    p = os.path.join(tmp, "sine16.wav")
    _write_riff(p, 1, 16, 1, rate, _encode(1, 16, sine))
    r, _ = rms_of(p)
    check("int16 sine rms=%.4f > 0.02" % r, r > 0.02)

    # float32 sine -> stdlib `wave` would choke here; we must parse it ourselves
    p = os.path.join(tmp, "sine32f.wav")
    _write_riff(p, 3, 32, 1, rate, _encode(3, 32, sine))
    r, w = rms_of(p)
    check("float32 sine rms=%.4f > 0.02 (fmt tag=%d)" % (r, w["fmt"]), r > 0.02 and w["fmt"] == 3)

    # silence -> below the floor (this is the vacuous-render guard)
    p = os.path.join(tmp, "silent.wav")
    _write_riff(p, 3, 32, 1, rate, _encode(3, 32, silence))
    r, _ = rms_of(p)
    check("float32 silence rms=%.4f < 0.02" % r, r < 0.02)

    # data-chunk parser must NOT assume a 44-byte header: inject a 40-byte JUNK
    # chunk before `data`, and prove the hash equals the same audio with no JUNK.
    audio = _encode(3, 32, sine)
    plain = os.path.join(tmp, "plain.wav")
    junked = os.path.join(tmp, "junked.wav")
    _write_riff(plain, 3, 32, 1, rate, audio)
    _write_riff(junked, 3, 32, 1, rate, audio, pre_chunks=[(b"JUNK", b"\x00" * 40)])
    check("data-chunk hash ignores a pre-`data` JUNK chunk", data_hash(plain) == data_hash(junked))
    # ...and a DIFFERENT signal must hash differently (the oracle isn't vacuous)
    other = os.path.join(tmp, "other.wav")
    _write_riff(other, 3, 32, 1, rate, _encode(3, 32, [x * 0.5 for x in sine]))
    check("data-chunk hash differs for different audio", data_hash(plain) != data_hash(other))

    print("wav-rms self-check: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print("usage: wav-rms.py rms FILE [THRESHOLD] | datahash FILE | --self-check",
              file=sys.stderr)
        return 2
    cmd = argv[0]
    if cmd == "--self-check":
        return _self_check()
    if cmd == "rms":
        if len(argv) < 2:
            print("rms needs a FILE", file=sys.stderr)
            return 2
        r, w = rms_of(argv[1])
        print("%.6f" % r)
        if len(argv) >= 3:
            thr = float(argv[2])
            if r > thr:
                return 0
            print("wav-rms: rms %.6f <= threshold %.6f (SILENT?)" % (r, thr), file=sys.stderr)
            return 1
        return 0
    if cmd == "datahash":
        if len(argv) < 2:
            print("datahash needs a FILE", file=sys.stderr)
            return 2
        print(data_hash(argv[1]))
        return 0
    print("unknown command: %s" % cmd, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
