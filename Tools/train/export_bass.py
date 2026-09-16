"""Writes the trained bass model as .phosmdl with its .ref.txt oracle, and measures what int8 costs.

The byte layout, the quantisation and the reader are ``export.py``'s and are **imported, not copied**:
``write_phosmdl``, ``_read_phosmdl``, ``quantise``, ``dequantise``, ``_ln`` and ``_gelu`` are the
literal implementation of docs/MODEL_FORMAT.md section 1 and 4 that the melodic round already checked
against PyTorch, so a bass file that goes through them is a file of the same format by construction
rather than by assertion.

What is new here is exactly the two additive things the bass needs, and both are written into
MODEL_FORMAT.md:

* the header says ``roles=4`` and carries ``condKick=3``;
* a tenth embedding table, ``kick.emb`` of shape ``[3, E]``, follows ``idx.emb``, and its row is
  added into the input of block 0 like the other nine.

Everything else -- the alphabet of 37 symbols, the output distribution, the block tensors, the
reference-file grammar -- is unchanged, which is the point: the C++ reader gains one table and one
header key and nothing else, and a melodic file without ``condKick`` still reads.

Usage:
    python Tools/train/export_bass.py --ckpt Tools/train/runs/bass/bass.pt \
                                      --out Tools/train/model/bass.phosmdl --verify
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np                                            # noqa: E402

import export                                                 # noqa: E402


# ------------------------------------------------------------------------------------------- naming

#: The embedding tables of a bass file, in file order: the melodic nine plus ``kick.emb``.
EMB_NAMES = (("tok.emb", "emb.tok.weight"), ("pos.emb", "emb.pos.weight"),
             ("role.emb", "emb.role.weight"), ("style.emb", "emb.style.weight"),
             ("bars.emb", "emb.bars.weight"), ("step.emb", "emb.step.weight"),
             ("bar.emb", "emb.bar.weight"), ("gap.emb", "emb.gap.weight"),
             ("idx.emb", "emb.idx.weight"), ("kick.emb", "emb.kick.weight"))


def name_to_key(layers):
    """(tensor name, state_dict key) in file order, for a transformer with a kick table."""
    keys = list(EMB_NAMES)
    for n in range(layers):
        p = f"blocks.{n}"
        keys += [(f"{p}.norm1.w", f"blocks.{n}.norm1.weight"), (f"{p}.norm1.b", f"blocks.{n}.norm1.bias"),
                 (f"{p}.attn.qkv.w", f"blocks.{n}.qkv.weight"), (f"{p}.attn.qkv.b", f"blocks.{n}.qkv.bias"),
                 (f"{p}.attn.out.w", f"blocks.{n}.out.weight"), (f"{p}.attn.out.b", f"blocks.{n}.out.bias"),
                 (f"{p}.norm2.w", f"blocks.{n}.norm2.weight"), (f"{p}.norm2.b", f"blocks.{n}.norm2.bias"),
                 (f"{p}.ffn.up.w", f"blocks.{n}.up.weight"), (f"{p}.ffn.up.b", f"blocks.{n}.up.bias"),
                 (f"{p}.ffn.down.w", f"blocks.{n}.down.weight"), (f"{p}.ffn.down.b", f"blocks.{n}.down.bias")]
    keys += [("norm.w", "norm.weight"), ("norm.b", "norm.bias"),
             ("head.w", "head.weight"), ("head.b", "head.bias")]
    return keys


def tensor_list(state, layers):
    out = []
    for name, key in name_to_key(layers):
        out.append((name, np.ascontiguousarray(state[key].detach().float().cpu().numpy())))
    return out


# --------------------------------------------------------------------------------- numpy forward pass

def numpy_forward(header, W, case):
    """Logits of one case from the file's tensors alone -- MODEL_FORMAT section 4 plus ``kick.emb``.

    A literal second implementation, as ``export.numpy_forward`` is for the melodic file: it imports
    nothing from ``models.py`` and knows nothing about torch, so when it agrees with PyTorch the
    document is complete enough to implement the C++ reader against.
    """
    L = len(case["tok"])
    ctx, eps = int(header["ctx"]), float(header["eps"])
    pos = np.minimum(np.arange(L), ctx - 1)
    x = (W["tok.emb"][case["tok"]] + W["pos.emb"][pos]
         + W["role.emb"][case["role"]] + W["style.emb"][case["style"]] + W["bars.emb"][case["bars"]]
         + W["step.emb"][case["step"]] + W["bar.emb"][case["bar"]]
         + W["gap.emb"][case["gap"]] + W["idx.emb"][case["idx"]]
         + W["kick.emb"][case["kick"]]).astype(np.float32)
    layers, E, heads = int(header["layers"]), int(header["dim"]), int(header["heads"])
    hd = E // heads
    mask = np.triu(np.full((L, L), -np.inf, dtype=np.float32), 1)
    for n in range(layers):
        p = f"blocks.{n}"
        h = export._ln(x, W[f"{p}.norm1.w"], W[f"{p}.norm1.b"], eps)
        qkv = h @ W[f"{p}.attn.qkv.w"].T + W[f"{p}.attn.qkv.b"]
        q, k, v = qkv[:, :E], qkv[:, E:2 * E], qkv[:, 2 * E:]
        q, k, v = (z.reshape(L, heads, hd).transpose(1, 0, 2) for z in (q, k, v))
        s = q @ k.transpose(0, 2, 1) / np.sqrt(hd, dtype=np.float32) + mask
        s = np.exp(s - s.max(-1, keepdims=True))
        att = (s / s.sum(-1, keepdims=True)) @ v
        x = x + (att.transpose(1, 0, 2).reshape(L, E) @ W[f"{p}.attn.out.w"].T + W[f"{p}.attn.out.b"])
        h = export._ln(x, W[f"{p}.norm2.w"], W[f"{p}.norm2.b"], eps)
        h = export._gelu(h @ W[f"{p}.ffn.up.w"].T + W[f"{p}.ffn.up.b"])
        x = x + (h @ W[f"{p}.ffn.down.w"].T + W[f"{p}.ffn.down.b"])
    x = export._ln(x, W["norm.w"], W["norm.b"], eps)
    return (x @ W["head.w"].T + W["head.b"]).astype(np.float32)


# ------------------------------------------------------------------------------------- reference file

def write_ref(path, model_name, header, cases, logits):
    """The oracle, with one field more than section 5's grammar: ``kick``."""
    lines = ["# phosmdl reference", f"model={model_name}", "arch=transformer",
             "tokenVersion=1", "vocab=37", "relMin=-12", f"quant={header['quant']}",
             f"cases={len(cases)}", ""]
    fmt = lambda a: " ".join(f"{float(x):.9g}" for x in a)          # noqa: E731
    ints = lambda a: " ".join(str(int(x)) for x in a)               # noqa: E731
    for i, (c, lg) in enumerate(zip(cases, logits)):
        lines += [f"case {i}", f"role={c['role']}", f"style={c['style']}", f"bars={c['bars']}",
                  f"len={len(c['tok'])}", f"tok={ints(c['tok'])}", f"step={ints(c['step'])}",
                  f"bar={ints(c['bar'])}", f"gap={ints(c['gap'])}", f"idx={ints(c['idx'])}",
                  f"kick={ints(c['kick'])}",
                  f"logits={fmt(lg)}", f"probs={fmt(export.softmax(lg))}"]
    with open(path, "w", encoding="ascii", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def make_cases(ctx=256, seed=20260916):
    """Synthetic conditioning sets: all four roles, every kick class, every gap code, short and long.

    Synthetic for the reason section 5 gives -- a held-out loop written out as symbols and step
    positions *is* the loop, and the bought packs stay on this machine. The bass cases additionally
    walk ``kick`` through its three values at every length, and the two longest cases put the note on
    the kick step, which is the case the composer's constraint sets make rarest and therefore the one
    a bug would hide in longest.
    """
    import random as _r
    import bass
    import dataset
    rng = _r.Random(seed)
    # Symbols a bass actually plays: the root, its octaves, the fifth, the flat second, the minor
    # third and seventh -- the range the corpus measurement finds, rather than uniform noise.
    pool = [bass.START_SYMBOL + d for d in (-12, -10, -9, -7, -5, -3, -1, 0, 1, 3, 5, 7, 10, 12)]
    lengths = [1, 2, 3, 5, 9, 16, 31, 48, 96, 150, 200, min(ctx, 260)]
    cases = []
    for i, L in enumerate(lengths):
        L = min(L, ctx)
        bars = i % 8
        stride = (i % 4) + 1
        steps = [(t * stride + (t // 5)) % ((bars + 1) * 16) for t in range(L)]
        if i % 2 == 0:
            steps = sorted(steps)
        cases.append({
            "tok": [bass.START_SYMBOL] + [rng.choice(pool) for _ in range(L - 1)],
            "step": [s % 16 for s in steps],
            "bar": [(s // 16) % 8 for s in steps],
            "gap": [(t + i) % bass.N_GAP for t in range(L)],
            "idx": [dataset.idx_bucket(t) for t in range(L)],
            # The first ten cases use the kick class the step really implies; the last two walk the
            # three classes independently of the step, so a reader that quietly recomputes `kick`
            # from `step` instead of reading it fails the oracle instead of passing it by luck.
            "kick": [bass.kick_code(s % 16) if i < 10 else (t % bass.N_KICK) for t, s in enumerate(steps)],
            "role": 3 if i % 4 == 3 else i % 4, "style": i % bass.N_STYLE, "bars": bars,
        })
    return cases


# ------------------------------------------------------------------------------------------------ main

def main():
    import torch
    import bass
    import models
    import train_bass

    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--quant", default="int8", choices=["int8", "float32"])
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--verify", action="store_true")
    a = ap.parse_args()

    ck = torch.load(a.ckpt, map_location="cpu", weights_only=False)
    args = ck["args"]
    model = models.build("transformer", ctx=args["ctx"], dropout=0.0, layers=args["layers"],
                         dim=args["dim"], heads=args["heads"], ffn=args["ffn"],
                         fields=models.BASS_FIELDS, n_role=bass.N_ROLE_BASS, n_kick=bass.N_KICK)
    model.load_state_dict(ck["state"])
    model.eval()

    recs = bass.load(a.root)
    _tr, _va, te = bass.build_split(recs, args["split"], rotations=args["rotations"],
                                    seed=args["seed"] * 7919 + 12345)
    dev = torch.device("cpu")
    nll_f32 = train_bass.evaluate(model, te, args["ctx"], dev)

    header = dict(ck["header"])
    header.update({"vocab": models.ALPHABET, "roles": bass.N_ROLE_BASS, "styles": bass.N_STYLE,
                   "tokenVersion": bass.TOKEN_VERSION, "quant": a.quant,
                   "relMin": bass.REL_MIN, "relMax": bass.REL_MAX,
                   "condStep": bass.N_STEP, "condBar": bass.N_BAR, "condGap": bass.N_GAP,
                   "condIdx": bass.N_IDX, "condBars": bass.N_BARS, "condKick": bass.N_KICK,
                   "eps": models.EPS, "nll": f"{nll_f32:.6f}"})
    order = ["arch", "layers", "dim", "heads", "ffn", "state", "vocab", "ctx", "roles", "styles",
             "tokenVersion", "quant", "relMin", "relMax", "condStep", "condBar", "condGap",
             "condIdx", "condBars", "condKick", "eps", "act", "nll"]
    header = {k: header[k] for k in order if k in header}

    tensors = tensor_list(ck["state"], args["layers"])
    deq = export.write_phosmdl(a.out, tensors, header, a.quant)
    print(f"written {a.out} ({os.path.getsize(a.out) / 1024:.0f} KiB, {len(tensors)} tensors, quant={a.quant})")

    qstate = {k: torch.from_numpy(deq[n].reshape(ck["state"][k].shape).copy())
              for n, k in name_to_key(args["layers"])}
    qmodel = models.build("transformer", ctx=args["ctx"], dropout=0.0, layers=args["layers"],
                          dim=args["dim"], heads=args["heads"], ffn=args["ffn"],
                          fields=models.BASS_FIELDS, n_role=bass.N_ROLE_BASS, n_kick=bass.N_KICK)
    qmodel.load_state_dict({**ck["state"], **qstate})
    qmodel.eval()
    nll_q = train_bass.evaluate(qmodel, te, args["ctx"], dev)
    print(f"  held-out NLL float32 {nll_f32:.4f}  {a.quant} {nll_q:.4f}  "
          f"({100 * (nll_q - nll_f32) / nll_f32:+.2f} %)")

    cases = make_cases(ctx=args["ctx"])
    hdr, W = export._read_phosmdl(a.out)
    logits = [numpy_forward(hdr, W, c)[-1] for c in cases]
    write_ref(a.out + ".ref.txt", os.path.basename(a.out), header, cases, logits)
    print(f"  {len(cases)} reference cases written to {a.out}.ref.txt")

    if a.verify:
        with torch.no_grad():
            worst = worst32 = 0.0
            for c, ref in zip(cases, logits):
                b = {k: torch.tensor([c[k]], dtype=torch.long) for k in train_bass.FIELDS_POS}
                b.update({k: torch.tensor([c[k]], dtype=torch.long) for k in train_bass.FIELDS_LINE})
                worst = max(worst, float(np.abs(qmodel(b)[0, -1].numpy() - ref).max()))
                worst32 = max(worst32, float(np.abs(model(b)[0, -1].numpy() - ref).max()))
            print(f"  numpy reader vs torch, max |logit difference| {worst:.3e}")
            print(f"  float32 model vs the exported {a.quant} file, max |logit difference| {worst32:.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
