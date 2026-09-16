"""Writes a trained stage-B model as .phosmdl, with its .ref.txt oracle, and measures what int8 costs.

Three things happen here, and the third is the point:

1. **Export.** The PyTorch parameters are renamed to the tensor names of docs/MODEL_FORMAT.md and
   written in the byte layout of its section 1, quantised to int8 with one scale per output row.
2. **Verification.** ``_read_phosmdl`` reads the file back with NumPy and ``numpy_forward`` runs the
   forward pass of section 4 from those bytes alone. It is a literal implementation of the document
   and nothing else -- no import of ``models.py``, no torch -- so when it agrees with PyTorch, the
   document is complete enough to implement against, which is what the C++ agent needs from it.
3. **The cost of quantisation, measured.** Held-out NLL of the float32 model against the dequantised
   int8 model, and the largest absolute logit difference over the reference cases. Post-training
   int8 with per-row scales is the cheapest of the schemes surveyed by Nagel et al. ("A white paper
   on neural network quantization", 2021); per-*tensor* scales would be cheaper still to read and
   were rejected because the embedding tables of this model have rows of very different norm.

Usage:
    python Tools/train/export.py --ckpt Tools/train/runs/main/transformer.pt --out Tools/train/model/acid_lead_arp.phosmdl
"""
import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np                                            # noqa: E402

MAGIC = b"PHOSMDL1"
END = b"PHOSEND1"
NAME_LEN = 48
ALIGN = 32


# ------------------------------------------------------------------------------------------- naming

def tensor_list(state, header):
    """(name, array, ndim) in the order docs/MODEL_FORMAT.md section 4 lists them."""
    out = []

    def add(name, key, squeeze=False):
        t = state[key].detach().float().cpu().numpy()
        if squeeze:
            t = t.reshape(t.shape[0], -1)
        out.append((name, np.ascontiguousarray(t)))

    for name, key in (("tok.emb", "emb.tok.weight"), ("pos.emb", "emb.pos.weight"),
                      ("role.emb", "emb.role.weight"), ("style.emb", "emb.style.weight"),
                      ("bars.emb", "emb.bars.weight"), ("step.emb", "emb.step.weight"),
                      ("bar.emb", "emb.bar.weight"), ("gap.emb", "emb.gap.weight"),
                      ("idx.emb", "emb.idx.weight")):
        add(name, key)
    for n in range(header["layers"]):
        p = f"blocks.{n}"
        add(f"{p}.norm1.w", f"blocks.{n}.norm1.weight")
        add(f"{p}.norm1.b", f"blocks.{n}.norm1.bias")
        if header["arch"] == "transformer":
            add(f"{p}.attn.qkv.w", f"blocks.{n}.qkv.weight")
            add(f"{p}.attn.qkv.b", f"blocks.{n}.qkv.bias")
            add(f"{p}.attn.out.w", f"blocks.{n}.out.weight")
            add(f"{p}.attn.out.b", f"blocks.{n}.out.bias")
            add(f"{p}.norm2.w", f"blocks.{n}.norm2.weight")
            add(f"{p}.norm2.b", f"blocks.{n}.norm2.bias")
            add(f"{p}.ffn.up.w", f"blocks.{n}.up.weight")
            add(f"{p}.ffn.up.b", f"blocks.{n}.up.bias")
            add(f"{p}.ffn.down.w", f"blocks.{n}.down.weight")
            add(f"{p}.ffn.down.b", f"blocks.{n}.down.bias")
        else:
            add(f"{p}.ssm.in.w", f"blocks.{n}.inp.weight")
            add(f"{p}.ssm.in.b", f"blocks.{n}.inp.bias")
            add(f"{p}.ssm.conv.w", f"blocks.{n}.conv.weight", squeeze=True)
            add(f"{p}.ssm.conv.b", f"blocks.{n}.conv.bias")
            add(f"{p}.ssm.xproj.w", f"blocks.{n}.xproj.weight")
            add(f"{p}.ssm.dt.w", f"blocks.{n}.dt.weight")
            add(f"{p}.ssm.dt.b", f"blocks.{n}.dt.bias")
            add(f"{p}.ssm.Alog.w", f"blocks.{n}.A_log")
            add(f"{p}.ssm.D.w", f"blocks.{n}.D")
            add(f"{p}.ssm.out.w", f"blocks.{n}.outp.weight")
            add(f"{p}.ssm.out.b", f"blocks.{n}.outp.bias")
    add("norm.w", "norm.weight")
    add("norm.b", "norm.bias")
    add("head.w", "head.weight")
    add("head.b", "head.bias")
    return out


# ------------------------------------------------------------------------------------------- writing

def quantise(w):
    """Per-row symmetric int8 (MODEL_FORMAT section 1): scale = max|row| / 127."""
    flat = w.reshape(w.shape[0], -1)
    scale = np.abs(flat).max(axis=1) / 127.0
    safe = np.where(scale > 0, scale, 1.0)
    q = np.clip(np.rint(flat / safe[:, None]), -127, 127).astype(np.int8)
    return q.reshape(w.shape), scale.astype(np.float32)


def dequantise(q, scale):
    flat = q.reshape(q.shape[0], -1).astype(np.float32) * scale[:, None]
    return flat.reshape(q.shape)


def write_phosmdl(path, tensors, header, quant="int8"):
    """Writes the file; returns the dequantised tensors, i.e. exactly what a reader will see."""
    head = "".join(f"{k}={v}\n" for k, v in header.items()).encode("ascii")
    buf = bytearray()
    buf += MAGIC
    buf += struct.pack("<II", 1, len(head))
    buf += head
    seen = {}
    for name, w in tensors:
        assert len(name) < NAME_LEN and name.encode("ascii") != END
        use_int8 = quant == "int8" and w.ndim >= 2
        if use_int8:
            q, scale = quantise(w)
            data, dtype = q, 1
            seen[name] = dequantise(q, scale)
        else:
            data, dtype = w.astype(np.float32), 0
            scale = None
            seen[name] = w.astype(np.float32)
        dims = list(w.shape) + [1] * (4 - w.ndim)
        buf += name.encode("ascii").ljust(NAME_LEN, b"\0")
        buf += struct.pack("<BBH", dtype, w.ndim, 0)
        buf += struct.pack("<4I", *dims)
        if scale is not None:
            buf += scale.astype("<f4").tobytes()
        buf += b"\0" * ((-len(buf)) % ALIGN)
        buf += np.ascontiguousarray(data).tobytes()
    buf += END
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(buf)
    return seen


def _read_phosmdl(path):
    """A literal NumPy implementation of MODEL_FORMAT section 1 -- the second opinion on the writer."""
    d = np.fromfile(path, dtype=np.uint8).tobytes()
    assert d[:8] == MAGIC, "not a .phosmdl"
    version, head_len = struct.unpack_from("<II", d, 8)
    assert version == 1
    header = {}
    for line in d[16:16 + head_len].decode("ascii").splitlines():
        if line:
            k, _, v = line.partition("=")
            header[k] = v
    o = 16 + head_len
    tensors = {}
    while d[o:o + 8] != END:
        name = d[o:o + NAME_LEN].rstrip(b"\0").decode("ascii")
        dtype, ndim, _res = struct.unpack_from("<BBH", d, o + NAME_LEN)
        dims = struct.unpack_from("<4I", d, o + NAME_LEN + 4)
        o += NAME_LEN + 4 + 16
        scale = None
        if dtype == 1:
            scale = np.frombuffer(d, dtype="<f4", count=dims[0], offset=o).copy()
            o += 4 * dims[0]
        o += (-o) % ALIGN
        shape = tuple(dims[:ndim])
        n = int(np.prod(dims))
        if dtype == 0:
            w = np.frombuffer(d, dtype="<f4", count=n, offset=o).copy().reshape(shape)
            o += 4 * n
        else:
            q = np.frombuffer(d, dtype=np.int8, count=n, offset=o).copy().reshape(shape)
            w = dequantise(q, scale)
            o += n
        tensors[name] = w
    return header, tensors


# --------------------------------------------------------------------------------- numpy forward pass

def _ln(x, g, b, eps):
    m = x.mean(-1, keepdims=True)
    v = ((x - m) ** 2).mean(-1, keepdims=True)
    return (x - m) / np.sqrt(v + eps) * g + b


def _gelu(v):
    return 0.5 * v * (1.0 + np.tanh(0.7978845608028654 * (v + 0.044715 * v ** 3)))


def _silu(v):
    return v / (1.0 + np.exp(-v))


def _softplus(v):
    return np.where(v > 0, v + np.log1p(np.exp(-np.abs(v))), np.log1p(np.exp(v)))


def numpy_forward(header, W, case):
    """Logits of one case, from the file's tensors alone (MODEL_FORMAT section 4). Shape (L, 37)."""
    L = len(case["tok"])
    ctx, eps = int(header["ctx"]), float(header["eps"])
    pos = np.minimum(np.arange(L), ctx - 1)
    x = (W["tok.emb"][case["tok"]] + W["pos.emb"][pos]
         + W["role.emb"][case["role"]] + W["style.emb"][case["style"]] + W["bars.emb"][case["bars"]]
         + W["step.emb"][case["step"]] + W["bar.emb"][case["bar"]]
         + W["gap.emb"][case["gap"]] + W["idx.emb"][case["idx"]]).astype(np.float32)
    layers, E = int(header["layers"]), int(header["dim"])
    if header["arch"] == "transformer":
        heads = int(header["heads"])
        hd = E // heads
        mask = np.triu(np.full((L, L), -np.inf, dtype=np.float32), 1)
        for n in range(layers):
            p = f"blocks.{n}"
            h = _ln(x, W[f"{p}.norm1.w"], W[f"{p}.norm1.b"], eps)
            qkv = h @ W[f"{p}.attn.qkv.w"].T + W[f"{p}.attn.qkv.b"]
            q, k, v = qkv[:, :E], qkv[:, E:2 * E], qkv[:, 2 * E:]
            q, k, v = (z.reshape(L, heads, hd).transpose(1, 0, 2) for z in (q, k, v))
            s = q @ k.transpose(0, 2, 1) / math.sqrt(hd) + mask
            s = np.exp(s - s.max(-1, keepdims=True))
            a = (s / s.sum(-1, keepdims=True)) @ v
            x = x + (a.transpose(1, 0, 2).reshape(L, E) @ W[f"{p}.attn.out.w"].T + W[f"{p}.attn.out.b"])
            h = _ln(x, W[f"{p}.norm2.w"], W[f"{p}.norm2.b"], eps)
            h = _gelu(h @ W[f"{p}.ffn.up.w"].T + W[f"{p}.ffn.up.b"])
            x = x + (h @ W[f"{p}.ffn.down.w"].T + W[f"{p}.ffn.down.b"])
    else:
        state, dt_rank, conv_k = int(header["state"]), int(header["dtRank"]), int(header["convK"])
        di = E * int(header["expand"])
        for n in range(layers):
            p = f"blocks.{n}"
            u = _ln(x, W[f"{p}.norm1.w"], W[f"{p}.norm1.b"], eps)
            both = u @ W[f"{p}.ssm.in.w"].T + W[f"{p}.ssm.in.b"]
            xin, z = both[:, :di], both[:, di:]
            cw, cb = W[f"{p}.ssm.conv.w"], W[f"{p}.ssm.conv.b"]
            padded = np.concatenate([np.zeros((conv_k - 1, di), np.float32), xin], axis=0)
            xc = np.stack([sum(cw[:, kk] * padded[t + kk] for kk in range(conv_k)) + cb for t in range(L)])
            xs = _silu(xc)
            proj = xs @ W[f"{p}.ssm.xproj.w"].T
            dtr, Bm, Cm = proj[:, :dt_rank], proj[:, dt_rank:dt_rank + state], proj[:, dt_rank + state:]
            delta = _softplus(dtr @ W[f"{p}.ssm.dt.w"].T + W[f"{p}.ssm.dt.b"])
            A = -np.exp(W[f"{p}.ssm.Alog.w"])
            h = np.zeros((di, state), np.float32)
            ys = []
            for t in range(L):
                dt_t = delta[t][:, None]
                h = np.exp(dt_t * A) * h + dt_t * xs[t][:, None] * Bm[t][None, :]
                ys.append(h @ Cm[t])
            y = np.stack(ys) + W[f"{p}.ssm.D.w"] * xs
            x = x + ((y * _silu(z)) @ W[f"{p}.ssm.out.w"].T + W[f"{p}.ssm.out.b"])
    x = _ln(x, W["norm.w"], W["norm.b"], eps)
    return (x @ W["head.w"].T + W["head.b"]).astype(np.float32)


# ------------------------------------------------------------------------------------- reference file

def softmax(v):
    e = np.exp(v - v.max())
    return e / e.sum()


def write_ref(path, model_name, header, cases, logits):
    lines = ["# phosmdl reference", f"model={model_name}", f"arch={header['arch']}",
             "tokenVersion=1", "vocab=37", "relMin=-12", f"quant={header['quant']}",
             f"cases={len(cases)}", ""]
    fmt = lambda a: " ".join(f"{float(x):.9g}" for x in a)          # noqa: E731
    ints = lambda a: " ".join(str(int(x)) for x in a)               # noqa: E731
    for i, (c, lg) in enumerate(zip(cases, logits)):
        lines += [f"case {i}", f"role={c['role']}", f"style={c['style']}", f"bars={c['bars']}",
                  f"len={len(c['tok'])}", f"tok={ints(c['tok'])}", f"step={ints(c['step'])}",
                  f"bar={ints(c['bar'])}", f"gap={ints(c['gap'])}", f"idx={ints(c['idx'])}",
                  f"logits={fmt(lg)}", f"probs={fmt(softmax(lg))}"]
    with open(path, "w", encoding="ascii", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def read_ref(path):
    """Parses a <name>.ref.txt back into cases -- the reader the C++ self test has to agree with."""
    cases, cur = [], None
    for line in open(path, "r", encoding="ascii"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("case "):
            cur = {}
            cases.append(cur)
            continue
        if cur is None or "=" not in line:
            continue
        k, _, v = line.partition("=")
        if k in ("role", "style", "bars", "len"):
            cur[k] = int(v)
        elif k in ("tok", "step", "bar", "gap", "idx"):
            cur[k] = [int(x) for x in v.split()]
        elif k in ("logits", "probs"):
            cur[k] = [float(x) for x in v.split()]
    return cases


def check_pair(model_path, ref_path):
    """Recomputes every reference case from the .phosmdl on disk. Returns the worst differences.

    This is the check the installed pair in Core/data has to pass: the weights and the oracle next to
    them must be the same model. Copying one without the other is a silent failure -- the C++ self
    test would then measure one model against another model's logits and report a reader bug that
    is not there.
    """
    header, W = _read_phosmdl(model_path)
    cases = read_ref(ref_path)
    worst_l = worst_p = 0.0
    for c in cases:
        got = numpy_forward(header, W, c)[-1]
        worst_l = max(worst_l, float(np.abs(got - np.array(c["logits"], np.float32)).max()))
        worst_p = max(worst_p, float(np.abs(softmax(got) - np.array(c["probs"], np.float32)).max()))
    return len(cases), worst_l, worst_p


def make_cases(ctx=256, seed=20260916):
    """Conditioning sets and contexts for the oracle: every role, short and long, every gap code.

    The contexts are **synthetic** and come from a fixed seed, never from the corpus. A held-out loop
    would make a more musical context, but half of a held-out loop written out as symbols and step
    positions is the loop, and the bought packs do not leave this machine (PLAN 6.9); a committed
    reference file must therefore not contain one. For an oracle over a deterministic forward pass
    that costs nothing: the twelve cases below still cover all three roles, contexts of length 1 to
    beyond the positional clamp, every gap code and every note-index bucket.
    """
    import dataset
    import random as _r
    rng = _r.Random(seed)
    # Symbols of a natural minor scale over two octaves plus the flat second -- the vocabulary the
    # constraint sets of Melody.h allow -- so the contexts are at least inside the alphabet's used
    # range rather than uniform noise.
    pool = [dataset.START_SYMBOL + d for d in (-12, -10, -9, -7, -5, -4, -2, 0, 1, 2, 3, 5, 7, 8, 10, 12, 13, 15, 19, 24)]
    lengths = [1, 2, 4, 7, 12, 16, 33, 64, 100, 150, 200, min(ctx, 260)]
    cases = []
    for i, L in enumerate(lengths):
        L = min(L, ctx)
        role = i % dataset.N_ROLE
        bars = (i % 8)
        stride = (i % 4) + 1
        steps = [(t * stride + (t // 7)) % ((bars + 1) * 16) for t in range(L)]
        steps = sorted(steps) if i % 2 == 0 else steps
        tok = [dataset.START_SYMBOL] + [rng.choice(pool) for _ in range(L - 1)]
        cases.append({
            "tok": tok,
            "step": [s % 16 for s in steps],
            "bar": [(s // 16) % 8 for s in steps],
            "gap": [(t + i) % dataset.N_GAP for t in range(L)],
            "idx": [dataset.idx_bucket(t) for t in range(L)],
            "role": role, "style": i % dataset.N_STYLE, "bars": bars,
        })
    return cases


# ------------------------------------------------------------------------------------------------ main

def main():
    import torch
    import dataset
    import models
    import train as trainmod

    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt")
    ap.add_argument("--check-pair", nargs=2, metavar=("MODEL", "REF"),
                    help="verify that a .phosmdl and the .ref.txt next to it are the same model")
    ap.add_argument("--out", help="the .phosmdl path")
    ap.add_argument("--quant", default="int8", choices=["int8", "float32"])
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--packs", default="psy")
    ap.add_argument("--split", default="group")
    ap.add_argument("--verify", action="store_true", help="read the file back and check it with NumPy")
    a = ap.parse_args()

    if a.check_pair:
        n, wl, wp = check_pair(*a.check_pair)
        print(f"{a.check_pair[0]}: {n} reference cases, max |logit difference| {wl:.3e}, "
              f"max |probability difference| {wp:.3e}")
        return 0 if (wl < 1e-3 and wp < 1e-5) else 1
    if not a.ckpt:
        raise SystemExit("--ckpt is required unless --check-pair is given")

    ck = torch.load(a.ckpt, map_location="cpu", weights_only=False)
    args = ck["args"]
    kw = dict(ctx=args["ctx"], dropout=0.0, layers=args["layers"])
    if ck["header"]["arch"] == "transformer":
        kw.update(dim=args["dim"], heads=args["heads"], ffn=args["ffn"])
    else:
        kw.update(dim=args["ssm_dim"], state=args["state"])
    model = models.build(ck["header"]["arch"], **kw)
    model.load_state_dict(ck["state"])
    model.eval()

    recs = dataset.load(a.root, dataset.packs_for(a.packs))
    tr, va, te = dataset.build_split(recs, a.split, rotations=args["rotations"],
                                     seed=dataset.split_seed_of(args))
    dev = torch.device("cpu")
    nll_f32 = trainmod.evaluate(model, te, args["ctx"], dev)

    header = dict(ck["header"])
    header.update({"vocab": models.ALPHABET, "roles": dataset.N_ROLE, "styles": dataset.N_STYLE,
                   "tokenVersion": dataset.TOKEN_VERSION, "quant": a.quant,
                   "relMin": dataset.REL_MIN, "relMax": dataset.REL_MAX,
                   "condStep": dataset.N_STEP, "condBar": dataset.N_BAR, "condGap": dataset.N_GAP,
                   "condIdx": dataset.N_IDX, "condBars": dataset.N_BARS, "eps": models.EPS,
                   "nll": f"{nll_f32:.6f}"})
    order = ["arch", "layers", "dim", "heads", "ffn", "state", "vocab", "ctx", "roles", "styles",
             "tokenVersion", "quant", "relMin", "relMax", "condStep", "condBar", "condGap",
             "condIdx", "condBars", "eps", "act", "expand", "dtRank", "convK", "nll"]
    header = {k: header[k] for k in order if k in header}

    tensors = tensor_list(ck["state"], header)
    deq = write_phosmdl(a.out, tensors, header, a.quant)
    size = os.path.getsize(a.out)
    print(f"written {a.out} ({size/1024:.0f} KiB, {len(tensors)} tensors, quant={a.quant})")

    # The cost of quantisation: the same held-out set, the dequantised weights loaded back into torch.
    qstate = {k: torch.from_numpy(deq[n].reshape(ck["state"][k].shape).copy())
              for n, k in _name_to_key(ck["state"], header)}
    qmodel = models.build(ck["header"]["arch"], **kw)
    qmodel.load_state_dict({**ck["state"], **qstate})
    qmodel.eval()
    nll_q = trainmod.evaluate(qmodel, te, args["ctx"], dev)
    print(f"  held-out NLL float32 {nll_f32:.4f}  {a.quant} {nll_q:.4f}  "
          f"({100 * (nll_q - nll_f32) / nll_f32:+.2f} %)")

    cases = make_cases(ctx=args["ctx"])
    hdr, W = _read_phosmdl(a.out)
    logits = [numpy_forward(hdr, W, c)[-1] for c in cases]
    write_ref(a.out + ".ref.txt", os.path.basename(a.out), header, cases, logits)
    print(f"  {len(cases)} reference cases written to {a.out}.ref.txt")

    if a.verify:
        with torch.no_grad():
            worst = 0.0
            for c, ref in zip(cases, logits):
                b = {k: torch.tensor([c[k]], dtype=torch.long) for k in ("tok", "step", "bar", "gap", "idx")}
                b.update({k: torch.tensor([c[k]], dtype=torch.long) for k in ("role", "style", "bars")})
                got = qmodel(b)[0, -1].numpy()
                worst = max(worst, float(np.abs(got - ref).max()))
            print(f"  numpy reader vs torch, max |logit difference| {worst:.3e}")
            worst32 = 0.0
            for c, ref in zip(cases, logits):
                b = {k: torch.tensor([c[k]], dtype=torch.long) for k in ("tok", "step", "bar", "gap", "idx")}
                b.update({k: torch.tensor([c[k]], dtype=torch.long) for k in ("role", "style", "bars")})
                worst32 = max(worst32, float(np.abs(model(b)[0, -1].numpy() - ref).max()))
            print(f"  float32 model vs the exported {a.quant} file, max |logit difference| {worst32:.4f}")
    return 0


def _name_to_key(state, header):
    """Pairs (tensor name, state_dict key) in the same order tensor_list builds them."""
    keys = []
    for name, key in (("tok.emb", "emb.tok.weight"), ("pos.emb", "emb.pos.weight"),
                      ("role.emb", "emb.role.weight"), ("style.emb", "emb.style.weight"),
                      ("bars.emb", "emb.bars.weight"), ("step.emb", "emb.step.weight"),
                      ("bar.emb", "emb.bar.weight"), ("gap.emb", "emb.gap.weight"),
                      ("idx.emb", "emb.idx.weight")):
        keys.append((name, key))
    for n in range(int(header["layers"])):
        p = f"blocks.{n}"
        keys += [(f"{p}.norm1.w", f"blocks.{n}.norm1.weight"), (f"{p}.norm1.b", f"blocks.{n}.norm1.bias")]
        if header["arch"] == "transformer":
            keys += [(f"{p}.attn.qkv.w", f"blocks.{n}.qkv.weight"), (f"{p}.attn.qkv.b", f"blocks.{n}.qkv.bias"),
                     (f"{p}.attn.out.w", f"blocks.{n}.out.weight"), (f"{p}.attn.out.b", f"blocks.{n}.out.bias"),
                     (f"{p}.norm2.w", f"blocks.{n}.norm2.weight"), (f"{p}.norm2.b", f"blocks.{n}.norm2.bias"),
                     (f"{p}.ffn.up.w", f"blocks.{n}.up.weight"), (f"{p}.ffn.up.b", f"blocks.{n}.up.bias"),
                     (f"{p}.ffn.down.w", f"blocks.{n}.down.weight"), (f"{p}.ffn.down.b", f"blocks.{n}.down.bias")]
        else:
            keys += [(f"{p}.ssm.in.w", f"blocks.{n}.inp.weight"), (f"{p}.ssm.in.b", f"blocks.{n}.inp.bias"),
                     (f"{p}.ssm.conv.w", f"blocks.{n}.conv.weight"), (f"{p}.ssm.conv.b", f"blocks.{n}.conv.bias"),
                     (f"{p}.ssm.xproj.w", f"blocks.{n}.xproj.weight"),
                     (f"{p}.ssm.dt.w", f"blocks.{n}.dt.weight"), (f"{p}.ssm.dt.b", f"blocks.{n}.dt.bias"),
                     (f"{p}.ssm.Alog.w", f"blocks.{n}.A_log"), (f"{p}.ssm.D.w", f"blocks.{n}.D"),
                     (f"{p}.ssm.out.w", f"blocks.{n}.outp.weight"), (f"{p}.ssm.out.b", f"blocks.{n}.outp.bias")]
    keys += [("norm.w", "norm.weight"), ("norm.b", "norm.bias"),
             ("head.w", "head.weight"), ("head.b", "head.bias")]
    return keys


if __name__ == "__main__":
    sys.exit(main())
