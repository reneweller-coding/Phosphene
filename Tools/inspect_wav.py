"""Draws a render so it can be looked at, and prints the measurements that matter for kick and bass.

Usage:
    python Tools/inspect_wav.py out/loop.wav [--bpm 145] [--png out/loop.png]

Picture (top to bottom): two bars of waveform, one beat zoomed in, a log-frequency spectrogram of
four bars. Numbers: per-beat low-band energy profile (where in the beat the sub band is occupied),
note-to-note consistency of the bass, spectral centroid by power (not by magnitude, which overstates
brightness), and the share of energy below 120 Hz.

Only numpy and Pillow are needed.
"""
import argparse
import math
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw


def read_wav(path):
    """Reads a 32-bit float or 16/24-bit PCM WAV (RIFF or RF64). Returns (mono float64, rate)."""
    data = open(path, "rb").read()
    if data[:4] not in (b"RIFF", b"RF64") or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    pos, fmt, rate, ch, bits, payload = 12, None, 0, 0, 0, None
    while pos + 8 <= len(data):
        tag, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = pos + 8
        if tag == b"fmt ":
            fmt, ch, rate = struct.unpack("<HHI", data[body:body + 8])
            bits = struct.unpack("<H", data[body + 14:body + 16])[0]
        elif tag == b"data":
            end = len(data) if size == 0xFFFFFFFF else body + size
            payload = data[body:end]
            break
        pos = body + size + (size & 1)
    if payload is None:
        raise ValueError("no data chunk")
    if fmt == 3 and bits == 32:
        x = np.frombuffer(payload[: len(payload) // 4 * 4], dtype="<f4").astype(np.float64)
    elif bits == 16:
        x = np.frombuffer(payload[: len(payload) // 2 * 2], dtype="<i2") / 32768.0
    elif bits == 24:
        b = np.frombuffer(payload[: len(payload) // 3 * 3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v) / 8388608.0
    else:
        raise ValueError(f"unsupported format {fmt}/{bits}")
    x = x[: len(x) // ch * ch].reshape(-1, ch).mean(axis=1)
    return x, rate


def one_pole_lowpass(x, rate, hz):
    a = np.exp(-2.0 * np.pi * hz / rate)
    y = np.empty_like(x)
    s = 0.0
    for i, v in enumerate(x):  # small inputs only; fine for minutes of audio
        s = (1.0 - a) * v + a * s
        y[i] = s
    return y


def draw_wave(draw, x, box, colour):
    x0, y0, x1, y1 = box
    w, mid, half = x1 - x0, (y0 + y1) / 2, (y1 - y0) / 2
    draw.rectangle(box, outline=(60, 60, 70))
    draw.line([(x0, mid), (x1, mid)], fill=(50, 50, 60))
    per = max(1, len(x) // w)
    for px in range(w):
        seg = x[px * per:(px + 1) * per]
        if len(seg) == 0:
            break
        draw.line([(x0 + px, mid - seg.max() * half), (x0 + px, mid - seg.min() * half)], fill=colour)


def draw_spectrogram(img, x, rate, box):
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    n = 4096
    hop = max(1, (len(x) - n) // w)
    win = np.hanning(n)
    fmin, fmax = 20.0, 20000.0
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    rows = fmin * (fmax / fmin) ** (np.arange(h)[::-1] / (h - 1))
    idx = np.clip(np.searchsorted(freqs, rows), 1, len(freqs) - 1)
    cols = []
    for c in range(w):
        seg = x[c * hop:c * hop + n]
        if len(seg) < n:
            break
        p = np.abs(np.fft.rfft(seg * win)) ** 2
        cols.append(10 * np.log10(p[idx] + 1e-20))
    s = np.array(cols).T
    top = s.max()
    s = np.clip((s - (top - 90)) / 90, 0, 1)
    rgb = np.stack([s ** 0.6 * 255, s ** 1.5 * 200, (1 - s) * s * 4 * 180], axis=-1).astype(np.uint8)
    img.paste(Image.fromarray(rgb, "RGB"), (x0, y0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--bpm", type=float, default=145.0)
    ap.add_argument("--png")
    ap.add_argument("--start-seconds", type=float, default=None, help="where the pictures start (default: bar 8)")
    a = ap.parse_args()
    x, rate = read_wav(a.wav)
    beat = 60.0 / a.bpm * rate
    bar = 4 * beat
    if a.start_seconds is not None:
        # Snap to the next downbeat of the given tempo so the pictures start on a kick.
        first = a.start_seconds * rate
        x = x[int(math.ceil(first / bar) * bar):]

    img = Image.new("RGB", (1400, 900), (18, 18, 24))
    d = ImageDraw.Draw(img)
    d.text((10, 5), f"{a.wav}  {rate} Hz  {a.bpm} BPM", fill=(220, 220, 220))
    start = int(8 * bar)  # skip the first bars, where nothing has settled yet
    draw_wave(d, x[start:start + int(2 * bar)], (10, 25, 1390, 225), (120, 200, 255))
    d.text((12, 27), "two bars", fill=(180, 180, 180))
    draw_wave(d, x[start:start + int(beat)], (10, 240, 1390, 440), (255, 190, 110))
    d.text((12, 242), "one beat", fill=(180, 180, 180))
    draw_spectrogram(img, x[start:start + int(4 * bar)], rate, (10, 460, 1390, 890))
    d.text((12, 462), "four bars, 20 Hz .. 20 kHz (log)", fill=(220, 220, 220))
    png = a.png or a.wav.rsplit(".", 1)[0] + ".png"
    img.save(png)
    print("picture:", png)

    # Where in the beat the sub band is occupied: energy below 120 Hz in 16 slices of the beat.
    seg = x[start:start + int(8 * bar)]
    lp = one_pole_lowpass(one_pole_lowpass(seg, rate, 120.0), rate, 120.0)
    nb = int(len(seg) // beat)
    slices = np.zeros(16)
    for b in range(nb):
        s0 = int(b * beat)
        for k in range(16):
            part = lp[s0 + int(k * beat / 16):s0 + int((k + 1) * beat / 16)]
            slices[k] += float(np.mean(part ** 2))
    slices /= max(slices.max(), 1e-20)
    print("sub band through the beat (16 slices, dB):", " ".join(f"{10*np.log10(v+1e-12):5.1f}" for v in slices))

    p = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    f = np.fft.rfftfreq(len(seg), 1.0 / rate)
    print(f"spectral centroid (power) {np.sum(f*p)/np.sum(p):.0f} Hz, energy below 120 Hz {100*np.sum(p[f<120])/np.sum(p):.1f} %")
    print(f"peak {20*np.log10(np.max(np.abs(x))+1e-12):.2f} dBFS, RMS {20*np.log10(np.sqrt(np.mean(x**2))+1e-12):.2f} dBFS")


if __name__ == "__main__":
    sys.exit(main())
