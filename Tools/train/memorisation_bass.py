"""Does the bass model reproduce the bought packs? The melodic battery, adapted to one role.

PLAN 6.9 makes this a shipping gate rather than a curiosity: the MIDI packs are bought, nothing from
them leaves this machine, and a model that copies is not shippable whatever its NLL says. The three
metrics, the reference and the positive control are ``memorisation.py``'s and are **imported**:
``bars_of``, ``run_index``, ``window_index``, ``longest_run``, ``exact_bar_rate``, ``nn_distances``
and ``measure`` are all keyed by role and work unchanged for role 3.

Two things had to be reconsidered for the bass, and both are stated rather than quietly adopted:

* **The bar definition.** ``memorisation.bars_of`` keeps only bars with at least three distinct
  pitches, because a bar of repeated roots matches half the corpus by necessity. A psytrance bass
  bar *is* mostly repeated roots -- the corpus measurement in ``bass_stats.py`` puts the root share
  at 0.59 and the median line at three distinct pitches -- so under that rule most bass bars would
  never be looked at. The bass therefore uses a two-pitch floor (:func:`bass_bars_of`) and the share
  of bars that clears it is printed, so the reader knows how much of the line the exact-copy number
  covers. The held-out reference makes the number interpretable either way.
* **The generated lines carry the kick.** :func:`generate` samples a fresh pitch line for the rhythm,
  the bar count and the **kick classes** of a real held-out loop, so the generated set has exactly
  the conditioning distribution of the set it is compared against.

Usage:
    python Tools/train/memorisation_bass.py --ckpt Tools/train/runs/bass/bass.pt --control
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import torch                                                  # noqa: E402

import bass                                                   # noqa: E402
import dataset                                                # noqa: E402
import memorisation as mem                                    # noqa: E402
import models                                                 # noqa: E402
import train_bass                                             # noqa: E402

WINDOW = mem.WINDOW


def bass_bars_of(rec, min_pitches=2):
    """``memorisation.bars_of`` with the distinct-pitch floor lowered -- see the module docstring."""
    total = max(rec["steps"]) + 1 if rec["steps"] else 0
    out = []
    for b in range((total + 15) // 16):
        cell = [None] * 16
        for s, y in zip(rec["steps"], rec["syms"]):
            if b * 16 <= s < (b + 1) * 16:
                cell[s - b * 16] = y
        ps = [x for x in cell if x is not None]
        if len(set(ps)) >= min_pitches:
            out.append(tuple(None if x is None else x - min(ps) for x in cell))
    return out


def bar_coverage(recs, min_pitches=2):
    """(bars that clear the floor, bars in all) -- how much of the material the copy rate covers."""
    kept = total = 0
    for r in recs:
        total += max(1, r["bars"])
        kept += len(bass_bars_of(r, min_pitches))
    return kept, total


@torch.no_grad()
def generate(model, template, device, temperature=1.0, seed=0):
    """A new pitch line for the rhythm, the bar count and the kick classes of ``template``.

    Sampling is unconstrained on purpose: the constraint masks of the composer can only move
    probability away from what the model wanted, so the bare model is the strict test.
    """
    g = torch.Generator(device="cpu").manual_seed(seed)
    enc = bass.encode(template)
    n = len(enc["tok"])
    toks = [bass.START_SYMBOL]
    for t in range(n):
        b = {"tok": torch.tensor([toks], dtype=torch.long, device=device)}
        for k in ("step", "bar", "gap", "idx", "kick"):
            b[k] = torch.tensor([enc[k][:t + 1]], dtype=torch.long, device=device)
        for k in train_bass.FIELDS_LINE:
            b[k] = torch.tensor([enc[k]], dtype=torch.long, device=device)
        logits = model(b)[0, -1].float().cpu() / max(temperature, 1e-3)
        toks.append(int(torch.multinomial(torch.softmax(logits, -1), 1, generator=g)))
    return {"role": bass.BASS_ROLE, "bars": template["bars"], "file": "generated", "pack": 0,
            "syms": toks[1:], "steps": list(template["steps"][:n]),
            "grid": list(template.get("grid", []))}


def measure(name, lines, corpus_bars, runs, windows, min_pitches=2):
    """``memorisation.measure`` with the bass bar definition."""
    total = hits = 0
    for rec in lines:
        for bar in bass_bars_of(rec, min_pitches):
            total += 1
            hits += 1 if bar in corpus_bars[rec["role"]] else 0
    rate = hits / max(1, total)
    lr_abs = [mem.longest_run(r["syms"], runs[r["role"]]) for r in lines]
    lr = [a / max(1, len(r["syms"])) for a, r in zip(lr_abs, lines)]
    d = sorted(mem.nn_distances(lines, windows))
    res = {"lines": len(lines), "bars": total, "exact_bar_copies": rate,
           "longest_run_median_notes": sorted(lr_abs)[len(lr_abs) // 2] if lr_abs else 0,
           "longest_run_max_notes": max(lr_abs) if lr_abs else 0,
           "longest_run_median_frac": sorted(lr)[len(lr) // 2] if lr else 0.0,
           "nn_windows": len(d),
           "nn_at_0": sum(1 for x in d if x == 0) / max(1, len(d)),
           "nn_at_1": sum(1 for x in d if x <= 1) / max(1, len(d)),
           "nn_at_2": sum(1 for x in d if x <= 2) / max(1, len(d)),
           "nn_median": d[len(d) // 2] if d else -1}
    print(f"  {name:22s} bars {res['bars']:5d}  exact bar copies {100 * res['exact_bar_copies']:5.2f} %"
          f"   longest shared run {res['longest_run_median_notes']:3d} notes (max {res['longest_run_max_notes']:3d},"
          f" {100 * res['longest_run_median_frac']:4.1f} % of the line)"
          f"   8-note windows at distance 0/<=1/<=2: {100 * res['nn_at_0']:5.2f} / {100 * res['nn_at_1']:5.2f}"
          f" / {100 * res['nn_at_2']:5.2f} %, median {res['nn_median']}")
    return res


def load_model(ckpt, device):
    ck = torch.load(ckpt, map_location="cpu", weights_only=False)
    a = ck["args"]
    m = models.build("transformer", ctx=a["ctx"], dropout=0.0, layers=a["layers"], dim=a["dim"],
                     heads=a["heads"], ffn=a["ffn"],
                     fields=models.ALL_FIELDS if a.get("no_kick") else models.BASS_FIELDS,
                     n_role=bass.N_ROLE_BASS, n_kick=bass.N_KICK)
    m.load_state_dict(ck["state"])
    return m.to(device).eval(), a


def overfit_control(tr, args, device, lines=40, steps=3000):
    """A model trained to memorise, so the metrics can be seen to light up.

    Same size as the shipping model, dropout and weight decay off, forty training lines, as many
    steps as the real run -- about a hundred epochs per line. A battery that does not light up here
    is measuring nothing, which is the reason this control exists.
    """
    small = tr[::max(1, len(tr) // lines)][:lines]
    m = models.build("transformer", ctx=args["ctx"], dropout=0.0, layers=args["layers"],
                     dim=args["dim"], heads=args["heads"], ffn=args["ffn"],
                     fields=models.BASS_FIELDS, n_role=bass.N_ROLE_BASS, n_kick=bass.N_KICK)
    train_bass.train(m, small, small, steps=steps, bs=8, ctx=args["ctx"], lr=1e-3, wd=0.0,
                     device=device, eval_every=steps, log=None)
    return m.to(device).eval(), small


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--temperature", type=float, default=1.0)
    ap.add_argument("--control", action="store_true")
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--out", default="")
    a = ap.parse_args()

    device = torch.device("cuda" if a.device == "cuda" and torch.cuda.is_available() else "cpu")
    model, args = load_model(a.ckpt, device)
    recs = bass.load(a.root)
    tr, va, te = bass.build_split(recs, args["split"], rotations=args["rotations"],
                                  seed=args["seed"] * 7919 + 12345)
    if args.get("extra") == "trance":
        tr = tr + dataset.exclude_near(bass.load(a.root, bass.BASS_TRANCE_PACKS), va + te)

    corpus_bars = {bass.BASS_ROLE: set()}
    for rec in tr:
        corpus_bars[bass.BASS_ROLE].update(bass_bars_of(rec))
    runs = {bass.BASS_ROLE: mem.run_index(tr, bass.BASS_ROLE)}
    windows = {bass.BASS_ROLE: mem.window_index(tr, bass.BASS_ROLE)}
    kept, total = bar_coverage(te)
    print(f"against {len(tr)} training bass lines "
          f"({len(corpus_bars[bass.BASS_ROLE])} distinct bars, {len(windows[bass.BASS_ROLE])} distinct "
          f"8-note windows); the two-pitch floor keeps {kept} of {total} held-out bars")

    out = {}
    out["heldout"] = measure("held-out (reference)", te, corpus_bars, runs, windows)
    gen = [generate(model, t, device, a.temperature, seed=i) for i, t in enumerate(te)]
    out["model"] = measure(f"model T={a.temperature}", gen, corpus_bars, runs, windows)
    out["train_self"] = measure("training lines", tr[:len(te)], corpus_bars, runs, windows)
    if a.control:
        cm, small = overfit_control(tr, args, device)
        cgen = [generate(cm, t, device, a.temperature, seed=1000 + i) for i, t in enumerate(small)]
        out["control"] = measure("POSITIVE CONTROL", cgen, corpus_bars, runs, windows)
    if a.out:
        with open(a.out, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(out, fh, indent=1)
        print("written", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
