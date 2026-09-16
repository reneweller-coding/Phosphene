"""Measures whether a stage-B model reproduces the bought MIDI packs instead of generalising.

This is a shipping gate, not a curiosity: PLAN 6.9 says the packs are bought, that nothing from them
leaves the machine, and that the model is checked for memorisation, with the nearest-neighbour
distance of generated patterns to the corpus as the test metric. A model that copies is not
shippable no matter what its NLL says.

Three numbers, and each is reported **against a reference**, because a bare copy rate is
uninterpretable on a corpus this repetitive -- an arp bar of four chord tones will coincide with
some corpus bar by chance. The reference is the held-out lines themselves: real psytrance loops the
model never saw, measured against the training set with the identical procedure. A model that reads
like the held-out lines is copying no more than two genuine loops of this genre resemble each other;
a model that reads clearly above them is copying.

* **Exact bar copies.** The definition ``build_corpus.py --memorisation`` already uses, reused here:
  a bar of sixteen steps, pitches taken relative to the bar's lowest note, at least three distinct
  pitches, compared against every training bar of the same role.
* **Longest matched run.** The longest contiguous run of symbols shared with any training line, as a
  fraction of the generated line -- the "longest common subsequence" family of copy detectors for
  symbolic music (Yin, Reuben, Stepney, Collins, "Measuring when a music generation algorithm copies
  too much", Neural Computing and Applications 35, 2023).
* **Nearest-neighbour distance distribution.** For every window of eight consecutive notes, the
  smallest Hamming distance to any training window of the same role, after subtracting each window's
  lowest symbol so the comparison is transposition-invariant. Reported as a distribution (the share
  at distance 0, 1, 2 and the median), which is what PLAN 6.9 asks for.

**Positive control.** ``--control`` trains a deliberately overfitted model -- no dropout, no weight
decay, forty training lines, many steps -- and runs the same measurement on it. A memorisation
metric that does not light up there is measuring nothing, and that check is the reason this file
exists rather than a single printed percentage.

Usage:
    python Tools/train/memorisation.py --ckpt Tools/train/runs/main/transformer.pt
    python Tools/train/memorisation.py --ckpt Tools/train/runs/main/transformer.pt --control
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import torch                                                  # noqa: E402

import dataset                                                # noqa: E402
import models                                                 # noqa: E402

WINDOW = 8


# ------------------------------------------------------------------------------------------ sampling

@torch.no_grad()
def generate(model, template, device, temperature=1.0, seed=0):
    """A new pitch line for the rhythm and conditioning of ``template``.

    The rhythm, the bar count and the role are taken from a real held-out loop, so the generated set
    has exactly the conditioning distribution of the set it is compared against and the measurement
    is about the pitches. Sampling is unconstrained: the constraint masks of ``Melody.cpp`` can only
    move probability away from what the model wanted, so the model alone is the strict test.
    """
    g = torch.Generator(device="cpu").manual_seed(seed)
    enc = dataset.encode(template)
    n = len(enc["tok"])
    toks = [dataset.START_SYMBOL]
    for t in range(n):
        b = {"tok": torch.tensor([toks], dtype=torch.long, device=device)}
        for k in ("step", "bar", "gap", "idx"):
            b[k] = torch.tensor([enc[k][:t + 1]], dtype=torch.long, device=device)
        for k in ("role", "style", "bars"):
            b[k] = torch.tensor([enc[k]], dtype=torch.long, device=device)
        logits = model(b)[0, -1].float().cpu() / max(temperature, 1e-3)
        nxt = int(torch.multinomial(torch.softmax(logits, -1), 1, generator=g))
        toks.append(nxt)
    return {"role": template["role"], "bars": template["bars"], "file": "generated",
            "pack": 0, "syms": toks[1:], "steps": list(template["steps"][:n])}


# ------------------------------------------------------------------------------------------- metrics

def bars_of(rec):
    """The bars of a line in the representation build_corpus.memorisation compares: sixteen steps,
    pitches relative to the bar's lowest note, ``None`` where nothing sounds, at least three pitches."""
    total = max(rec["steps"]) + 1 if rec["steps"] else 0
    out = []
    for b in range((total + 15) // 16):
        cell = [None] * 16
        for s, y in zip(rec["steps"], rec["syms"]):
            if b * 16 <= s < (b + 1) * 16:
                cell[s - b * 16] = y
        ps = [x for x in cell if x is not None]
        if len(set(ps)) >= 3:
            out.append(tuple(None if x is None else x - min(ps) for x in cell))
    return out


def exact_bar_rate(lines, corpus_bars):
    total = hits = 0
    for rec in lines:
        for bar in bars_of(rec):
            total += 1
            hits += 1 if bar in corpus_bars[rec["role"]] else 0
    return hits / max(1, total), total


def longest_run(syms, corpus_runs):
    """Longest contiguous run of ``syms`` that occurs in some training line of the same role."""
    best = 0
    for i in range(len(syms)):
        j = i + best
        while j < len(syms) and tuple(syms[i:j + 1]) in corpus_runs:
            best = j + 1 - i
            j += 1
    return best


def run_index(records, role, cap=24):
    """Every contiguous run of up to ``cap`` symbols in the training lines of ``role``."""
    out = set()
    for rec in records:
        if rec["role"] != role:
            continue
        s = rec["syms"]
        for i in range(len(s)):
            for L in range(1, min(cap, len(s) - i) + 1):
                out.add(tuple(s[i:i + L]))
    return out


def window_index(records, role, w=WINDOW):
    out = set()
    for rec in records:
        if rec["role"] != role:
            continue
        s = rec["syms"]
        for i in range(len(s) - w + 1):
            win = s[i:i + w]
            out.add(tuple(x - min(win) for x in win))
    return out


def nn_distances(lines, windows, w=WINDOW):
    """Smallest Hamming distance of every generated window to any training window, per role.

    Brute force against every training window, in NumPy blocks: the corpus has tens of thousands of
    windows and an index would only be an approximation of the very quantity being measured.
    """
    import numpy as np
    pools = {r: (np.array(sorted(windows[r]), dtype=np.int16) if windows[r] else np.zeros((0, w), np.int16))
             for r in windows}
    dists = []
    for role, pool in pools.items():
        queries = []
        for rec in lines:
            if rec["role"] != role:
                continue
            s = rec["syms"]
            for i in range(len(s) - w + 1):
                win = s[i:i + w]
                queries.append([x - min(win) for x in win])
        if not queries or len(pool) == 0:
            dists += [w] * len(queries)
            continue
        q = np.array(queries, dtype=np.int16)
        for i in range(0, len(q), 128):
            blk = q[i:i + 128]
            d = (blk[:, None, :] != pool[None, :, :]).sum(axis=2).min(axis=1)
            dists += [int(x) for x in d]
    return dists


def measure(name, lines, train_recs, corpus_bars, runs, windows):
    rate, nbars = exact_bar_rate(lines, corpus_bars)
    lr = [longest_run(r["syms"], runs[r["role"]]) / max(1, len(r["syms"])) for r in lines]
    lr_abs = [longest_run(r["syms"], runs[r["role"]]) for r in lines]
    d = nn_distances(lines, windows)
    d_sorted = sorted(d)
    res = {
        "lines": len(lines), "bars": nbars, "exact_bar_copies": rate,
        "longest_run_median_frac": sorted(lr)[len(lr) // 2] if lr else 0.0,
        "longest_run_median_notes": sorted(lr_abs)[len(lr_abs) // 2] if lr_abs else 0,
        "longest_run_max_notes": max(lr_abs) if lr_abs else 0,
        "nn_windows": len(d),
        "nn_at_0": sum(1 for x in d if x == 0) / max(1, len(d)),
        "nn_at_1": sum(1 for x in d if x <= 1) / max(1, len(d)),
        "nn_at_2": sum(1 for x in d if x <= 2) / max(1, len(d)),
        "nn_median": d_sorted[len(d_sorted) // 2] if d_sorted else -1,
    }
    print(f"  {name:22s} bars {res['bars']:5d}  exact bar copies {100*res['exact_bar_copies']:5.2f} %"
          f"   longest shared run {res['longest_run_median_notes']:3d} notes (max {res['longest_run_max_notes']:3d},"
          f" {100*res['longest_run_median_frac']:4.1f} % of the line)"
          f"   8-note windows at distance 0/<=1/<=2: {100*res['nn_at_0']:5.2f} / {100*res['nn_at_1']:5.2f}"
          f" / {100*res['nn_at_2']:5.2f} %, median {res['nn_median']}")
    return res


# ------------------------------------------------------------------------------------------------ main

def load_model(ckpt, device):
    ck = torch.load(ckpt, map_location="cpu", weights_only=False)
    args = ck["args"]
    kw = dict(ctx=args["ctx"], dropout=0.0, layers=args["layers"])
    if ck["header"]["arch"] == "transformer":
        kw.update(dim=args["dim"], heads=args["heads"], ffn=args["ffn"])
    else:
        kw.update(dim=args["ssm_dim"], state=args["state"])
    m = models.build(ck["header"]["arch"], **kw)
    m.load_state_dict(ck["state"])
    return m.to(device).eval(), args


def overfit_control(tr, va, args, device, lines=40, steps=3000):
    """The positive control: a model trained to memorise, so the metrics can be seen to light up.

    Always a transformer of the shipping size, with dropout and weight decay switched off, on a
    fortieth of the training lines for as many steps as the real run -- about a hundred epochs per
    line. The lines are taken with an even stride so that all three roles are in there.
    """
    import train as trainmod
    small = tr[::max(1, len(tr) // lines)][:lines]
    m = models.build("transformer", ctx=args["ctx"], dropout=0.0, layers=args["layers"],
                     dim=args["dim"], heads=args["heads"], ffn=args["ffn"])
    trainmod.train(m, small, small, steps=steps, bs=8, ctx=args["ctx"], lr=1e-3, wd=0.0,
                   device=device, eval_every=steps, log=None)
    return m.to(device).eval(), small


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--packs", default="psy")
    ap.add_argument("--split", default="group")
    ap.add_argument("--extra", default="none", choices=["none", "trance"],
                    help="the model was trained with this extra material; it must be in the set the "
                         "generated lines are compared against, or the copy rates are understated")
    ap.add_argument("--temperature", type=float, default=1.0)
    ap.add_argument("--control", action="store_true")
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--out", default="")
    a = ap.parse_args()

    device = torch.device("cuda" if a.device == "cuda" and torch.cuda.is_available() else "cpu")
    model, args = load_model(a.ckpt, device)
    recs = dataset.load(a.root, dataset.packs_for(a.packs))
    tr, va, te = dataset.build_split(recs, a.split, rotations=args["rotations"],
                                     seed=args["seed"] * 7919 + 12345)
    if a.extra == "trance":
        tr = tr + dataset.exclude_near(dataset.load(a.root, dataset.TRANCE_PACKS), va + te)

    corpus_bars = {r: set() for r in range(len(dataset.ROLES))}
    for rec in tr:
        corpus_bars[rec["role"]].update(bars_of(rec))
    runs = {r: run_index(tr, r) for r in range(len(dataset.ROLES))}
    windows = {r: window_index(tr, r) for r in range(len(dataset.ROLES))}

    print(f"against {len(tr)} training lines "
          f"({sum(len(v) for v in corpus_bars.values())} distinct bars, "
          f"{sum(len(v) for v in windows.values())} distinct 8-note windows)")
    out = {}
    out["heldout"] = measure("held-out (reference)", te, tr, corpus_bars, runs, windows)
    gen = [generate(model, t, device, a.temperature, seed=i) for i, t in enumerate(te)]
    out["model"] = measure(f"model T={a.temperature}", gen, tr, corpus_bars, runs, windows)
    out["train_self"] = measure("training lines", tr[:len(te)], tr, corpus_bars, runs, windows)

    if a.control:
        cm, small = overfit_control(tr, va, args, device)
        cgen = [generate(cm, t, device, a.temperature, seed=1000 + i) for i, t in enumerate(small)]
        out["control"] = measure("POSITIVE CONTROL", cgen, tr, corpus_bars, runs, windows)

    if a.out:
        with open(a.out, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(out, fh, indent=1)
        print("written", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
