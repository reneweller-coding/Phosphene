#!/usr/bin/env python3
"""The Kaleidoscope cue bridge (PLAN 8.3), checked from outside the program.

Three things are proved here, none of them by listening:

1. **The wire format is OSC, not "whatever the encoder wrote."** The decoder below is written from
   the OSC 1.0 specification -- null-terminated strings padded to a multiple of four, a type tag
   string that begins with a comma, big-endian arguments -- and knows nothing about the C++ code. If
   the two agree, the format is right; if only the C++ round-trips, it proves nothing.

2. **The cues are the score.** `phos_cuedemo --sections` prints what the composer wrote, straight
   out of `Composer::sections()`. The datagrams captured from a run of the same set have to carry
   the same section boundaries on the same bars, in the same order. A bar number never travels with
   a section message: the bar is read the way a visualiser reads it, from the last `/phos/bar`.

3. **The delay is small and measured.** `phos_cuedemo --realtime` renders in real time and receives
   its own datagrams, so the whole chain -- tap, queue, sender's wait, encoder, sendto(), loopback,
   recvfrom() -- is measured on one clock.

4. **Both ends agree, on the same bytes.** With `--kaleidoscope <Kaleidoscope.exe>` the captured
   datagrams are written to a file and put through Kaleidoscope's own receiver
   (`Kaleidoscope.exe -q` with `KALEIDO_CUE_DECODE` set), and what it says is compared with what the
   Python decoder says. Three decoders -- the C++ encoder's hand-checked byte layout, this one, and
   the receiver in the other program -- over one set of bytes, with no shared code between them.

Usage:  python Tools/cue_check.py --demo <path to phos_cuedemo> [--port 9111] [--bars 96]
                                  [--kaleidoscope "C:\\...\\Release\\Kaleidoscope.exe"]
"""

import argparse
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading


# --------------------------------------------------------------------------- the decoder

class BadMessage(Exception):
    """The bytes are not an OSC message this program should ever have sent."""


def _read_string(data, pos):
    """One OSC string: bytes up to a null, then padding to the next multiple of four."""
    end = data.find(b"\0", pos)
    if end < 0:
        raise BadMessage("unterminated string at %d" % pos)
    text = data[pos:end].decode("ascii")
    nxt = end + 1
    while nxt % 4:
        if nxt >= len(data) or data[nxt] != 0:
            raise BadMessage("string at %d is not padded with nulls" % pos)
        nxt += 1
    return text, nxt


def decode(data):
    """An OSC 1.0 message -> (address, [arguments]).

    Written from the specification, not from the sender: that is the whole point of it being here.
    """
    if len(data) % 4:
        raise BadMessage("length %d is not a multiple of four" % len(data))
    address, pos = _read_string(data, 0)
    if not address.startswith("/"):
        raise BadMessage("address %r does not start with a slash" % address)
    tags, pos = _read_string(data, pos)
    if not tags.startswith(","):
        raise BadMessage("type tag string %r does not start with a comma" % tags)
    args = []
    for tag in tags[1:]:
        if tag == "i":
            args.append(struct.unpack_from(">i", data, pos)[0])
            pos += 4
        elif tag == "f":
            args.append(struct.unpack_from(">f", data, pos)[0])
            pos += 4
        elif tag == "s":
            text, pos = _read_string(data, pos)
            args.append(text)
        else:
            raise BadMessage("unsupported type tag %r" % tag)
    if pos != len(data):
        raise BadMessage("%d bytes left over" % (len(data) - pos))
    return address, args


# --------------------------------------------------------------------------- the run

class Listener(threading.Thread):
    """Binds the cue port and keeps every datagram, in arrival order."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.sock.bind(("127.0.0.1", port))
        self.sock.settimeout(0.5)
        self.messages = []
        self.stop = False

    def run(self):
        while not self.stop:
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            self.messages.append(data)


def run_demo(demo, args):
    out = subprocess.run([demo] + args, capture_output=True, text=True, timeout=600)
    if out.returncode != 0:
        raise SystemExit("phos_cuedemo %s failed (%d):\n%s" % (" ".join(args), out.returncode, out.stderr))
    return out.stdout


def parse_report(text):
    """The demo's `key value` lines as a dict; repeated keys become lists."""
    report = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            report.setdefault(parts[0], []).append(parts[1:])
    return report


#: Pitch class of a key name's leading note, derived here rather than taken from either program.
PITCH_CLASS = {}
for _i, _n in enumerate(["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]):
    PITCH_CLASS[_n] = _i
for _flat, _pc in [("Db", 1), ("Eb", 3), ("Gb", 6), ("Ab", 8), ("Bb", 10)]:
    PITCH_CLASS[_flat] = _pc

CHECKS = []


def check(ok, what, detail=""):
    CHECKS.append(bool(ok))
    print("  [%s] %s%s" % (" ok " if ok else "FAIL", what, ("  -- " + detail) if detail else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True, help="path to phos_cuedemo")
    ap.add_argument("--port", type=int, default=9111, help="a port of its own, so a real visualiser is not disturbed")
    ap.add_argument("--bars", type=int, default=96)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--realtime-bars", type=int, default=8)
    ap.add_argument("--kaleidoscope", default=os.environ.get("KALEIDOSCOPE_EXE", ""),
                    help="Kaleidoscope.exe; its receiver then decodes the same captured bytes")
    args = ap.parse_args()

    common = ["--seed", str(args.seed), "--bars", str(args.bars)]

    print("cue bridge (PLAN 8.3), checked from outside")

    # ---------------------------------------------------------------- the score, from the composer
    sections = []
    keys = []
    for parts in parse_report(run_demo(args.demo, common + ["--sections"])).get("section", []):
        sections.append((int(parts[0]), parts[1] if len(parts) > 1 else "", float(parts[2])))
    for parts in parse_report(run_demo(args.demo, common + ["--sections"])).get("key", []):
        keys.append((int(parts[0]), " ".join(parts[1:])))
    check(len(sections) > 0, "the composer wrote a form to compare against",
          "%d section marks, %d keys" % (len(sections), len(keys)))

    # ---------------------------------------------------------------- the cues, off the wire
    listener = Listener(args.port)
    listener.start()
    report = parse_report(run_demo(args.demo, common + ["--port", str(args.port)]))
    # The renderer is far faster than real time and the sender is in immediate mode; give the socket
    # a moment to hand over what is still in its buffer.
    import time
    time.sleep(0.6)
    listener.stop = True
    listener.join(timeout=3)
    listener.sock.close()

    staged = int(report["staged"][0][0])
    sent = int(report["sent"][0][0])
    dropped = int(report["dropped"][0][0])
    check(dropped == 0 and sent == staged, "every cue the tap made was sent",
          "%d staged, %d sent, %d dropped" % (staged, sent, dropped))
    check(len(listener.messages) == sent, "every datagram arrived on the loopback",
          "%d of %d" % (len(listener.messages), sent))

    decoded = []
    bad = []
    for data in listener.messages:
        try:
            decoded.append(decode(data))
        except BadMessage as e:
            bad.append(str(e))
    check(not bad, "every datagram decodes as OSC 1.0 by an independent decoder",
          bad[0] if bad else "%d messages" % len(decoded))

    # ---------------------------------------------------------------- bar for bar
    bar = -1
    got_sections = []
    got_keys = []
    bars_seen = []
    beats_seen = []
    drops = 0
    for address, params in decoded:
        if address == "/phos/bar":
            bar = params[0]
            bars_seen.append(bar)
        elif address == "/phos/beat":
            beats_seen.append(params[0])
        elif address == "/phos/section":
            got_sections.append((bar, params[0], params[1]))
        elif address == "/phos/key":
            got_keys.append((bar, params[0]))
        elif address == "/phos/drop":
            drops += 1
        else:
            check(False, "unknown address", address)

    same = [(b, t) for b, t, _ in got_sections] == [(b, t) for b, t, _ in sections]
    check(same, "every section boundary of the score arrived as a cue, bar for bar",
          "%d cues against %d marks" % (len(got_sections), len(sections)))
    if not same:
        print("    score:", sections[:12])
        print("    wire :", got_sections[:12])

    energy_ok = all(abs(g[2] - s[2]) < 1e-3 for g, s in zip(got_sections, sections))
    check(energy_ok and got_sections, "the energy of each section survives the float32 argument")

    cores = sum(1 for _, t, _ in sections if t == "Drop")
    check(cores > 0 and drops == cores, "/phos/drop accompanies every core section and no other",
          "%d drops, %d cores" % (drops, cores))

    check([(b, k) for b, k in got_keys] == [(b, k) for b, k in keys],
          "/phos/key carries the key of every track, on the bar the track starts on",
          "%d keys against %d tracks" % (len(got_keys), len(keys)))

    check(bars_seen == list(range(args.bars)), "every bar, once, in order",
          "%d bars" % len(bars_seen))
    check(beats_seen == list(range(args.bars * 4)), "every beat, once, in order",
          "%d beats" % len(beats_seen))

    # ---------------------------------------------------------------- the other end's decoder
    if args.kaleidoscope:
        # The same datagrams, byte for byte, through Kaleidoscope's receiver. A capture file rather
        # than a second socket: two programs cannot both receive one unicast datagram, and what has
        # to be compared is the decoding, not the delivery.
        capture = os.path.join(tempfile.gettempdir(), "phos_cues_capture.bin")
        with open(capture, "wb") as f:
            for data in listener.messages:
                f.write(struct.pack(">I", len(data)))
                f.write(data)
        env = dict(os.environ, KALEIDO_CUE_DECODE=capture)
        out = subprocess.run([args.kaleidoscope, "-q"], capture_output=True, text=True,
                             timeout=300, env=env, cwd=os.path.dirname(args.kaleidoscope) or None)
        theirs = [line.strip() for line in out.stdout.splitlines() if line.strip()]
        ours = []
        for address, params in decoded:
            if address == "/phos/beat":
                ours.append("beat %d" % params[0])
            elif address == "/phos/bar":
                ours.append("bar %d" % params[0])
            elif address == "/phos/section":
                ours.append("section %s %.4f" % (params[0], params[1]))
            elif address == "/phos/key":
                ours.append("key %s %d" % (params[0], PITCH_CLASS.get(params[0].split()[0], -1)))
            elif address == "/phos/drop":
                ours.append("drop")
        check(out.returncode == 0, "Kaleidoscope's receiver decoded every captured datagram",
              out.stderr.strip().splitlines()[-1] if out.stderr.strip() else "")
        check(theirs == ours, "and read exactly what this decoder reads, message for message",
              "%d lines against %d" % (len(theirs), len(ours)))
        if theirs != ours:
            for a, b in zip(theirs, ours):
                if a != b:
                    print("    theirs: %r  ours: %r" % (a, b))
                    break

    # ---------------------------------------------------------------- the delay, measured
    rt = parse_report(run_demo(args.demo, ["--seed", str(args.seed), "--bars", str(args.realtime_bars),
                                           "--realtime", "--port", str(args.port + 1)]))
    if "delay_ms_mean" in rt:
        mean = float(rt["delay_ms_mean"][0][0])
        median = float(rt["delay_ms_median"][0][0])
        p95 = float(rt["delay_ms_p95"][0][0])
        worst = float(rt["delay_ms_max"][0][0])
        least = float(rt["delay_ms_min"][0][0])
        matched = int(rt["matched"][0][0])
        mismatched = int(rt["mismatched"][0][0])
        check(matched > 0 and mismatched == 0, "every datagram arrived in the order it was staged",
              "%d matched, %d out of order" % (matched, mismatched))
        print("  end-to-end delay against the instant the listener hears the beat, over localhost:")
        print("    mean %.3f ms, median %.3f ms, min %.3f ms, p95 %.3f ms, max %.3f ms (%d cues)"
              % (mean, median, least, p95, worst, matched))
        # A cue that arrives late is worse than none. One frame of a 60 Hz visualiser is 16.7 ms,
        # and the whole point of the lead arithmetic is that the residual is far inside that.
        check(p95 < 16.7, "95 % of the cues land inside one frame of a 60 Hz visualiser",
              "p95 %.3f ms" % p95)
        check(worst < 50.0, "and none of them is a sixteenth note late at 145 BPM (103 ms)",
              "worst %.3f ms" % worst)
    else:
        check(False, "the real-time run measured a delay")

    failed = CHECKS.count(False)
    print("\n%d passed, %d failed" % (CHECKS.count(True), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
