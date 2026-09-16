"""Tests for the MIDI parser of ``Tools/corpus/build_corpus.py``.

Why this file exists. ``read_midi`` generates the shipped stage-A tables (``Core/src/CorpusTables.cpp``)
and, through ``Tools/train/dataset.py``, every token the learned models are trained on. It is a
hand-written SMF reader of fifty lines, and the corpus it is pointed at is 28 000 files from a dozen
vendors, so the interesting cases are not the well-formed files but the ones a sloppy exporter wrote.
Each test below is one such case, with a byte sequence small enough to read.

The case that prompted the file: 26 files of the Star Samples super pack (the "DMS ... Single
Patches" folders) are 192-byte synthesiser patch dumps -- one System Exclusive message and then
trailing bytes. On those the old parser raised ``TypeError: unsupported operand type(s) for &:
'NoneType' and 'int'``, because it read a data byte while no status byte had ever been seen in that
track and then masked ``None``. That is not the same thing as "the file uses running status": running
status is legal and common (MIDI 1.0 Detailed Specification, section 2.1.2 "Running Status"), and it
has to keep working. What cannot be decoded is a byte whose status was never established, and the
answer to that is to end the track and keep the rest of the file, not to raise.

Run:
    python Tools/corpus/test_build_corpus.py
"""
import importlib.util
import os
import struct
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load(path):
    """Imports build_corpus.py from an explicit path (it is not on a package path)."""
    spec = importlib.util.spec_from_file_location("build_corpus_under_test", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def vlq(n):
    """A MIDI variable-length quantity, the inverse of ``build_corpus.read_vlq``."""
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def track(*events):
    """One MTrk chunk from ``(delta, payload)`` pairs, with the end-of-track meta event appended."""
    body = b"".join(vlq(dt) + bytes(payload) for dt, payload in events) + vlq(0) + b"\xFF\x2F\x00"
    return b"MTrk" + struct.pack(">I", len(body)) + body


def smf(*tracks, division=96, fmt=1):
    return b"MThd" + struct.pack(">IHHH", 6, fmt, len(tracks), division) + b"".join(tracks)


def write(data):
    fh = tempfile.NamedTemporaryFile(suffix=".mid", delete=False)
    fh.write(data)
    fh.close()
    return fh.name


# ------------------------------------------------------------------------------------------- cases

def case_running_status(bc):
    """Running status must decode: one 0x90, then note pairs with the status byte omitted.

    This is the guard against a "fix" that simply refuses files using running status. Four note-ons
    (two of them with velocity 0, i.e. note-offs) written with a single status byte give two notes.
    """
    t = track(
        (0, b"\x90\x40\x64"),          # note on 64, velocity 100 -- the only status byte
        (24, b"\x43\x64"),             # running: note on 67
        (24, b"\x40\x00"),             # running: note off 64 (velocity 0)
        (0, b"\x43\x00"),              # running: note off 67
    )
    ppq, notes = bc.read_midi(write(smf(t)))
    assert ppq == 96, ppq
    assert len(notes) == 2, notes
    assert sorted(p for _s, _e, p, _v in notes) == [64, 67], notes
    assert notes[0][:2] == (0, 48), notes[0]


def case_running_status_across_meta(bc):
    """A meta event between two running-status events must not cancel the status.

    Meta events are an SMF container feature and carry no MIDI status (Standard MIDI File 1.0,
    section 'Meta-Events'); exporters that write a marker or a tempo change in the middle of a track
    and then continue in running status are writing legal files.
    """
    t = track(
        (0, b"\x90\x40\x64"),
        (0, b"\xFF\x06\x04mark"),      # marker meta event
        (24, b"\x40\x00"),             # running status must still be 0x90
    )
    _ppq, notes = bc.read_midi(write(smf(t)))
    assert len(notes) == 1 and notes[0][2] == 64, notes


def case_data_byte_without_status(bc):
    """The DMS patch dump: a SysEx and then data bytes no status ever covered.

    The parser must not raise, must keep the notes of the other tracks, and must not invent notes out
    of the trailing bytes. The byte sequence is the shape of the 26 real files, shortened: one
    System Exclusive message, then bytes below 0x80 to the end of the track.
    """
    good = track((0, b"\x90\x3C\x64"), (48, b"\x80\x3C\x40"))
    broken = track((0, b"\xF0\x05\x7E\x7F\x09\x01\xF7"), (0, b"\x40\x40\x40\x40\x40\x40"))
    after = track((0, b"\x91\x45\x50"), (24, b"\x81\x45\x40"))
    ppq, notes = bc.read_midi(write(smf(good, broken, after)))
    assert ppq == 96, ppq
    assert sorted(p for _s, _e, p, _v in notes) == [60, 69], notes


def case_sysex_cancels_running_status(bc):
    """A System Exclusive message cancels running status (MIDI 1.0 Detailed Specification 2.1.2).

    Without that rule the bytes after the SysEx below would be read as note data of the 0x90 before
    it, and the file would gain a note that was never written. With it, the track simply ends there.
    """
    t = track(
        (0, b"\x90\x3C\x64"),
        (48, b"\x3C\x00"),             # running note-off, so the live status is 0x90
        (0, b"\xF0\x03\x7E\x7F\xF7"),
        (0, b"\x41\x41"),              # data bytes: the status was cancelled, so undecodable
        (24, b"\x41\x00"),             # under the old rule these four bytes made a phantom note 65
    )
    _ppq, notes = bc.read_midi(write(smf(t)))
    assert [p for _s, _e, p, _v in notes] == [60], notes


def case_truncated_event(bc):
    """A track whose last event is cut in half must end the track, not read the next chunk's bytes."""
    body = vlq(0) + b"\x90\x3C\x64" + vlq(24) + b"\x80\x3C"     # one byte missing, no end-of-track
    trunc = b"MTrk" + struct.pack(">I", len(body)) + body
    after = track((0, b"\x90\x48\x64"), (24, b"\x80\x48\x40"))
    _ppq, notes = bc.read_midi(write(smf(trunc, after)))
    assert [p for _s, _e, p, _v in notes] == [72], notes


def case_channel_ten_is_dropped(bc):
    """Channel 10 (index 9) is percussion and contributes no melodic notes -- unchanged behaviour."""
    t = track((0, b"\x99\x24\x64"), (24, b"\x89\x24\x40"), (0, b"\x90\x40\x64"), (24, b"\x80\x40\x40"))
    _ppq, notes = bc.read_midi(write(smf(t)))
    assert [p for _s, _e, p, _v in notes] == [64], notes


CASES = [case_running_status, case_running_status_across_meta, case_data_byte_without_status,
         case_sysex_cancels_running_status, case_truncated_event, case_channel_ten_is_dropped]


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--module", default=os.path.join(_HERE, "build_corpus.py"),
                    help="the build_corpus.py under test; point it at a copy of the old file to see "
                         "the running-status cases fail")
    a = ap.parse_args()
    bc = _load(a.module)
    bad = 0
    for fn in CASES:
        try:
            fn(bc)
            print(f"  ok   {fn.__name__}")
        except Exception as e:                                  # noqa: BLE001
            bad += 1
            print(f"  FAIL {fn.__name__}: {type(e).__name__}: {e}")
    print(f"{len(CASES) - bad} of {len(CASES)} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
