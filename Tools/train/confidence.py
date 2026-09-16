"""Confidence interval on the held-out NLL and on the gap between two models.

A single NLL over 102 held-out lines is a point estimate, and the decision of PLAN 6.9 -- transformer
or state-space model -- rests on the difference between two of them. The difference has to come with
an interval or it is not a measurement.

Two intervals, both over the **line** as the resampling unit and not over the token: the tokens of
one loop are anything but independent, so a bootstrap over tokens would report an interval several
times too narrow (Koehn, "Statistical significance tests for machine translation evaluation", EMNLP
2004, on exactly this mistake in per-token scores).

* a percentile bootstrap of each model's NLL, 10 000 resamples of the held-out lines;
* a **paired** bootstrap of the difference: both models are scored on the same resampled lines, so
  the variance of the held-out set itself cancels and what is left is the difference between the
  models. The sign test over lines is printed next to it.

Usage:
    python Tools/train/confidence.py --a Tools/train/runs/final_tf/transformer.pt \
                                     --b Tools/train/runs/final_ssm/ssm.pt
    python Tools/train/confidence.py --a ... --b markov      # against the stage-A baseline
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np                                            # noqa: E402
import torch                                                  # noqa: E402
import torch.nn.functional as F                               # noqa: E402

import dataset                                                # noqa: E402
import markov                                                 # noqa: E402
import models                                                 # noqa: E402


def load(ckpt):
    ck = torch.load(ckpt, map_location="cpu", weights_only=False)
    a = ck["args"]
    kw = dict(ctx=a["ctx"], dropout=0.0, layers=a["layers"])
    if ck["header"]["arch"] == "transformer":
        kw.update(dim=a["dim"], heads=a["heads"], ffn=a["ffn"])
    else:
        kw.update(dim=a["ssm_dim"], state=a["state"])
    m = models.build(ck["header"]["arch"], **kw)
    m.load_state_dict(ck["state"])
    return m.eval(), a


@torch.no_grad()
def per_line(model, records, ctx, device):
    """(summed NLL, token count) of every held-out line, in the order of ``records``."""
    out = []
    for rec in records:
        e = dataset.encode(rec, ctx=ctx)
        b = {k: torch.tensor([e[k]], dtype=torch.long, device=device) for k in ("tok", "step", "bar", "gap", "idx")}
        b.update({k: torch.tensor([e[k]], dtype=torch.long, device=device) for k in ("role", "style", "bars")})
        tgt = torch.tensor([e["tgt"]], dtype=torch.long, device=device)
        loss = F.cross_entropy(model(b).reshape(-1, models.ALPHABET), tgt.reshape(-1), reduction="sum")
        out.append((float(loss), len(e["tgt"])))
    return np.array(out, dtype=np.float64)


def per_line_markov(train_recs, records, order=2):
    ms = {r: markov.MarkovModel(train_recs, r) for r in range(len(dataset.ROLES))}
    out = []
    for rec in records:
        m = ms[rec["role"]]
        a = b = dataset.START_SYMBOL
        tot = 0.0
        for c in rec["syms"]:
            tot -= np.log(max(m.prob(a, b, c, order), 1e-12))
            a, b = b, c
        out.append((tot, len(rec["syms"])))
    return np.array(out, dtype=np.float64)


def boot(x, n=10000, seed=7):
    """Percentile bootstrap of sum(nll)/sum(tokens) over lines."""
    rng = np.random.default_rng(seed)
    idx = rng.integers(0, len(x), size=(n, len(x)))
    s = x[idx]
    return s[:, :, 0].sum(1) / s[:, :, 1].sum(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True, help="a checkpoint, or 'markov' / 'markov0'")
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--packs", default="psy")
    ap.add_argument("--split", default="group")
    ap.add_argument("--resamples", type=int, default=10000)
    ap.add_argument("--device", default="cpu")
    o = ap.parse_args()

    dev = torch.device("cuda" if o.device == "cuda" and torch.cuda.is_available() else "cpu")
    ma, args = load(o.a)
    ma.to(dev)
    recs = dataset.load(o.root, dataset.packs_for(o.packs))
    tr, va, te = dataset.build_split(recs, o.split, rotations=args["rotations"],
                                     seed=dataset.split_seed_of(args))
    itr, _, _ = dataset.split(recs, o.split, seed=dataset.split_seed_of(args))
    tr_raw = [recs[i] for i in itr]

    xa = per_line(ma, te, args["ctx"], dev)
    if o.b.startswith("markov"):
        xb = per_line_markov(tr_raw, te, 0 if o.b.endswith("0") else 2)
        name_b = f"stage A order {'0' if o.b.endswith('0') else '2'}"
    else:
        mb, _ = load(o.b)
        mb.to(dev)
        xb = per_line(mb, te, args["ctx"], dev)
        name_b = os.path.basename(o.b)

    print(f"{len(te)} held-out lines, {int(xa[:, 1].sum())} tokens")
    for name, x in ((os.path.basename(o.a), xa), (name_b, xb)):
        bs = boot(x, o.resamples)
        print(f"  {name:22s} NLL {x[:, 0].sum() / x[:, 1].sum():.4f}  "
              f"95 % CI [{np.percentile(bs, 2.5):.4f}, {np.percentile(bs, 97.5):.4f}]")
    # Paired: the same resampled lines for both, so the variance of the held-out set cancels.
    rng = np.random.default_rng(11)
    idx = rng.integers(0, len(te), size=(o.resamples, len(te)))
    da = xa[idx][:, :, 0].sum(1) / xa[idx][:, :, 1].sum(1)
    db = xb[idx][:, :, 0].sum(1) / xb[idx][:, :, 1].sum(1)
    d = db - da
    better = int((xa[:, 0] / xa[:, 1] < xb[:, 0] / xb[:, 1]).sum())
    print(f"  difference (b - a)   {xb[:, 0].sum() / xb[:, 1].sum() - xa[:, 0].sum() / xa[:, 1].sum():.4f} nats  "
          f"95 % CI [{np.percentile(d, 2.5):.4f}, {np.percentile(d, 97.5):.4f}]  "
          f"P(difference <= 0) = {float((d <= 0).mean()):.4f}")
    print(f"  a is the better model on {better} of {len(te)} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
