#!/usr/bin/env python3
"""Writes a random-weight `.phosmdl` model and its reference vectors.

The trained models of Phase 8 come from ``Tools/train`` (a second agent). This script exists for two
reasons: the self test needs a model small enough to live in the repository, and a second, independent
implementation of ``docs/MODEL_FORMAT.md`` is the only way to tell a misunderstanding of the format
from a bug in the arithmetic. The weights are random, so nothing here says anything about music --
what it says is that the file reader, the forward pass and the decoder agree with NumPy.

**The format is docs/MODEL_FORMAT.md**, written by the training side and normative. This script
follows it; where the two disagree, the document is right and this script is the bug. It writes the
same tensor names, the same nine embedding tables, the same conditioning fields and the same
reference-file layout as ``Tools/train/export.py``.

Usage::

    python Tools/model/make_test_model.py --out Core/data/test_tiny.phosmdl
    python Tools/model/make_test_model.py --out Core/data/test_tiny_f32.phosmdl --quant float32
"""

import argparse
import os
import struct

import numpy as np

MAGIC = b"PHOSMDL1"
END = b"PHOSEND1"
VOCAB = 37        # kCorpusAlphabet
REL_MIN = -12     # kCorpusRelMin
REL_MAX = 24      # kCorpusRelMax
START_TOKEN = -REL_MIN   # the symbol of the interval 0, section 3 of the format
ROLES, STYLES = 3, 6
COND_BARS, COND_STEP, COND_BAR, COND_GAP, COND_IDX = 8, 16, 8, 10, 8


def quantize_rows(w):
    """int8 with one scale per row of the first dimension, and the float64 it stands for."""
    scale = np.max(np.abs(w), axis=1) / 127.0
    safe = np.where(scale > 0.0, scale, 1.0)
    q = np.clip(np.rint(w / safe[:, None]), -127, 127).astype(np.int8)
    scale = scale.astype(np.float32)
    return q, scale, q.astype(np.float64) * scale[:, None].astype(np.float64)


class Writer:
    """Collects tensors and writes the file, keeping every payload 32-byte aligned."""

    def __init__(self, header):
        self.header = header.encode("ascii")
        self.blobs = []   # (name, dtype, dims, scale or None, payload bytes)

    def add(self, name, array, quant):
        """Adds one tensor and returns the values a reader will see (dequantized when int8)."""
        a = np.asarray(array, dtype=np.float64)
        dims = list(a.shape) + [1] * (4 - a.ndim)
        if quant and a.ndim == 2:
            q, scale, deq = quantize_rows(a)
            self.blobs.append((name, 1, dims, scale, q.tobytes()))
            return deq
        f = a.astype(np.float32)
        self.blobs.append((name, 0, dims, None, f.tobytes()))
        return f.astype(np.float64)

    def write(self, path):
        out = bytearray()
        out += MAGIC
        out += struct.pack("<II", 1, len(self.header))
        out += self.header
        for name, dtype, dims, scale, payload in self.blobs:
            nb = name.encode("ascii")
            assert len(nb) < 48, name
            out += nb + b"\0" * (48 - len(nb))
            ndim = 4
            while ndim > 1 and dims[ndim - 1] == 1:
                ndim -= 1
            out += struct.pack("<BBH", dtype, ndim, 0)
            out += struct.pack("<4I", *dims)
            if dtype == 1:
                out += scale.astype("<f4").tobytes()
            out += b"\0" * ((-len(out)) % 32)
            out += payload
        out += END
        with open(path, "wb") as fh:
            fh.write(out)
        return len(out)


def gelu(x):
    """GELU in the tanh form of Hendrycks and Gimpel, "Gaussian error linear units" (2016)."""
    return 0.5 * x * (1.0 + np.tanh(0.7978845608028654 * (x + 0.044715 * x ** 3)))


def layer_norm(x, w, b, eps):
    """LayerNorm with the biased variance, as PyTorch computes it."""
    m = x.mean()
    v = ((x - m) ** 2).mean()
    return (x - m) / np.sqrt(v + eps) * w + b


def build(args):
    """Builds the weights, writes the file, and returns what the reference pass needs."""
    rng = np.random.default_rng(args.seed)
    d, h, f, layers = args.dim, args.heads, args.ffn, args.layers
    assert d % h == 0, "dim must be a multiple of heads"
    header = "\n".join([
        "arch=transformer",
        f"layers={layers}", f"dim={d}", f"heads={h}", f"ffn={f}", "state=0",
        f"vocab={VOCAB}", f"ctx={args.ctx}", f"roles={ROLES}", f"styles={STYLES}",
        "tokenVersion=1", f"quant={args.quant}",
        f"relMin={REL_MIN}", f"relMax={REL_MAX}",
        f"condStep={COND_STEP}", f"condBar={COND_BAR}", f"condGap={COND_GAP}",
        f"condIdx={COND_IDX}", f"condBars={COND_BARS}",
        "eps=1e-05", "act=gelu_tanh", "nll=0",
        "source=Tools/model/make_test_model.py (random weights, not trained)",
    ]) + "\n"
    w = Writer(header)
    q = args.quant == "int8"

    def normal(*shape, s=0.02):
        return rng.normal(0.0, s, size=shape)

    p = {"emb": {}}
    for name, rows in (("tok", VOCAB), ("pos", args.ctx), ("role", ROLES), ("style", STYLES),
                       ("bars", COND_BARS), ("step", COND_STEP), ("bar", COND_BAR),
                       ("gap", COND_GAP), ("idx", COND_IDX)):
        p["emb"][name] = w.add(f"{name}.emb", normal(rows, d, s=0.05), q)
    p["blocks"] = []
    for i in range(layers):
        b = {}
        b["n1w"] = w.add(f"blocks.{i}.norm1.w", rng.normal(1.0, 0.05, size=d), False)
        b["n1b"] = w.add(f"blocks.{i}.norm1.b", normal(d), False)
        b["qkvw"] = w.add(f"blocks.{i}.attn.qkv.w", normal(3 * d, d, s=0.08), q)
        b["qkvb"] = w.add(f"blocks.{i}.attn.qkv.b", normal(3 * d), False)
        b["outw"] = w.add(f"blocks.{i}.attn.out.w", normal(d, d, s=0.08), q)
        b["outb"] = w.add(f"blocks.{i}.attn.out.b", normal(d), False)
        b["n2w"] = w.add(f"blocks.{i}.norm2.w", rng.normal(1.0, 0.05, size=d), False)
        b["n2b"] = w.add(f"blocks.{i}.norm2.b", normal(d), False)
        b["upw"] = w.add(f"blocks.{i}.ffn.up.w", normal(f, d, s=0.08), q)
        b["upb"] = w.add(f"blocks.{i}.ffn.up.b", normal(f), False)
        b["dnw"] = w.add(f"blocks.{i}.ffn.down.w", normal(d, f, s=0.08), q)
        b["dnb"] = w.add(f"blocks.{i}.ffn.down.b", normal(d), False)
        p["blocks"].append(b)
    p["normw"] = w.add("norm.w", rng.normal(1.0, 0.05, size=d), False)
    p["normb"] = w.add("norm.b", normal(d), False)
    p["headw"] = w.add("head.w", normal(VOCAB, d, s=0.08), q)
    p["headb"] = w.add("head.b", normal(VOCAB), False)
    return header, w, p


def forward(p, args, case):
    """Logits after the last position of `case`, in float64 -- the oracle."""
    d, h, layers = args.dim, args.heads, args.layers
    hd = d // h
    eps = 1e-5
    n = len(case["tok"])
    e = p["emb"]
    x = np.stack([
        e["tok"][case["tok"][t]] + e["pos"][t]
        + e["role"][case["role"]] + e["style"][case["style"]] + e["bars"][case["bars"]]
        + e["step"][case["step"][t]] + e["bar"][case["bar"][t]]
        + e["gap"][case["gap"][t]] + e["idx"][case["idx"][t]]
        for t in range(n)])
    for i in range(layers):
        b = p["blocks"][i]
        nx = np.stack([layer_norm(x[t], b["n1w"], b["n1b"], eps) for t in range(n)])
        qkv = nx @ b["qkvw"].T + b["qkvb"]
        qh, kh, vh = qkv[:, :d], qkv[:, d:2 * d], qkv[:, 2 * d:]
        att = np.zeros((n, d))
        for head in range(h):
            sl = slice(head * hd, (head + 1) * hd)
            for t in range(n):
                sc = kh[:t + 1, sl] @ qh[t, sl] / np.sqrt(hd)
                sc = np.exp(sc - sc.max())
                sc /= sc.sum()
                att[t, sl] = sc @ vh[:t + 1, sl]
        x = x + att @ b["outw"].T + b["outb"]
        nx = np.stack([layer_norm(x[t], b["n2w"], b["n2b"], eps) for t in range(n)])
        x = x + gelu(nx @ b["upw"].T + b["upb"]) @ b["dnw"].T + b["dnb"]
    last = layer_norm(x[n - 1], p["normw"], p["normb"], eps)
    return p["headw"] @ last + p["headb"]


def make_case(rng, args, index):
    """One synthetic line: a rhythm, then the conditioning the composer would build from it."""
    # Lengths that cover the cold start, the short lines the composer really draws, and the last
    # position the file allows, so an off-by-one in the position index cannot hide. Never longer than
    # ctx: what a model should do past its positional table is not defined by the format, and the C++
    # reader refuses such a line rather than inventing an answer (Model.cpp).
    lengths = [1, 2, 3, 5, 9, 17, 33, args.ctx]
    n = lengths[index % len(lengths)] if index < len(lengths) else int(rng.integers(2, args.ctx + 1))
    bars = int(rng.integers(1, COND_BARS + 1))
    # A rising list of onset steps inside the pattern, as an onset list is.
    total = bars * 16
    steps = sorted(rng.choice(np.arange(total), size=min(n, total), replace=False).tolist())
    while len(steps) < n:
        steps.append(steps[-1] + 1 + int(rng.integers(0, 12)))   # past the pattern: exercises the clamps
    syms = [int(v) for v in rng.integers(0, VOCAB, size=n)]
    tok = [START_TOKEN] + syms[:-1]
    gap = []
    for t in range(n):
        if t + 1 >= n:
            gap.append(0)
        else:
            dsteps = steps[t + 1] - steps[t]
            gap.append(9 if dsteps > 8 else max(1, dsteps))
    return {
        "role": int(rng.integers(0, ROLES)),
        "style": int(rng.integers(0, STYLES)),
        "bars": bars - 1,
        "tok": tok,
        "step": [s % 16 for s in steps],
        "bar": [(s // 16) % 8 for s in steps],
        "gap": gap,
        "idx": [t if t < 4 else (4 if t < 6 else (5 if t < 10 else (6 if t < 16 else 7))) for t in range(n)],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True)
    ap.add_argument("--layers", type=int, default=2)
    ap.add_argument("--dim", type=int, default=32)
    ap.add_argument("--heads", type=int, default=4)
    ap.add_argument("--ffn", type=int, default=64)
    ap.add_argument("--ctx", type=int, default=96)
    ap.add_argument("--quant", choices=["int8", "float32"], default="int8")
    ap.add_argument("--cases", type=int, default=10)
    ap.add_argument("--seed", type=int, default=20260916)
    args = ap.parse_args()

    header, writer, p = build(args)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    size = writer.write(args.out)

    rng = np.random.default_rng(args.seed + 1)
    lines = ["# phosmdl reference",
             f"model={os.path.basename(args.out)}",
             "arch=transformer", "tokenVersion=1", f"vocab={VOCAB}", f"relMin={REL_MIN}",
             f"quant={args.quant}", f"cases={args.cases}", ""]
    for c in range(args.cases):
        case = make_case(rng, args, c)
        lg = forward(p, args, case)
        pr = np.exp(lg - lg.max())
        pr /= pr.sum()
        lines.append(f"case {c}")
        lines.append(f"role={case['role']}")
        lines.append(f"style={case['style']}")
        lines.append(f"bars={case['bars']}")
        lines.append(f"len={len(case['tok'])}")
        for key in ("tok", "step", "bar", "gap", "idx"):
            lines.append(key + "=" + " ".join(str(v) for v in case[key]))
        lines.append("logits=" + " ".join("%.9g" % v for v in lg))
        lines.append("probs=" + " ".join("%.9g" % v for v in pr))
    with open(args.out + ".ref.txt", "w", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"{args.out}: {size} bytes, {args.cases} reference cases")
    print(header, end="")


if __name__ == "__main__":
    main()
