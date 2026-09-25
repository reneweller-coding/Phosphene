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
    import train as trainmod                                   # noqa: PLC0415
    kw = dict(ctx=a["ctx"], dropout=0.0, layers=a["layers"],
              fields=trainmod.fields_for(a.get("cond", "full"), a.get("mode_cond", False)))
    if ck["header"]["arch"] == "transformer":
        kw.update(dim=a["dim"], heads=a["heads"], ffn=a["ffn"],
                  n_mode=dataset.N_MODE if a.get("mode_cond") else 0)
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
        b.update({k: torch.tensor([e[k]], dtype=torch.long, device=device) for k in ("role", "style", "bars", "mode")})
        tgt = torch.tensor([e["tgt"]], dtype=torch.long, device=device)
        loss = F.cross_entropy(model(b).reshape(-1, models.ALPHABET), tgt.reshape(-1), reduction="sum")
        out.append((float(loss), len(e["tgt"])))
    return np.array(out, dtype=np.float64)


def per_line_phosmdl(path, records, log=None):
    """(summed NLL, token count) of every line under an **exported** ``.phosmdl``.

    Why read the shipped file rather than a checkpoint: the question this report answers is
    whether a new model beats *what is installed*, and what is installed is
    ``Core/data/melody.phosmdl`` -- int8, with whatever seed and whatever training set produced it.
    The checkpoint it came from is gitignored and is not on this machine, and reproducing it by
    retraining would compare against a reconstruction rather than against the artefact. The forward
    pass is ``export.numpy_forward``, the same literal NumPy implementation of MODEL_FORMAT section 4
    that ``--check-pair`` validates the reference cases with, so the model is scored by the reader
    the format contract is written against and not by the trainer that wrote it.
    """
    import export                                               # noqa: PLC0415
    header, W = export._read_phosmdl(path)
    if int(header.get("roles", 3)) != len(dataset.ROLES) or "kick.emb" in W:
        raise SystemExit(f"{path} is not a three-role melodic model (roles={header.get('roles')})")
    # A file with mode.emb is scored with the label ``dataset.encode`` puts on the line, which is the
    # estimator's; see the warning in the module docstring about what that does to the comparison.
    ctx = int(header["ctx"])
    out = []
    for n, rec in enumerate(records):
        e = dataset.encode(rec, ctx=ctx)
        logits = export.numpy_forward(header, W, e)
        tgt = np.array(e["tgt"])
        m = logits.max(-1, keepdims=True)
        lse = m[:, 0] + np.log(np.exp(logits - m).sum(-1))
        out.append((float((lse - logits[np.arange(len(tgt)), tgt]).sum()), len(tgt)))
        if log and (n + 1) % 25 == 0:
            print(f"    {n + 1}/{len(records)} lines", file=log, flush=True)
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
    ap.add_argument("--b", required=True, help="a .pt checkpoint, a .phosmdl, or markov / markov0")
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--packs", default="psy")
    ap.add_argument("--split", default="group")
    ap.add_argument("--resamples", type=int, default=10000)
    ap.add_argument("--device", default="cpu")
    ap.add_argument("--mode-eval", default="line", choices=["line", "shuffle", "aeolian"],
                    help="what the mode slot of the held-out lines is filled with; see the module "
                         "docstring. Ignored by a model without mode.emb, which never reads it")
    o = ap.parse_args()

    dev = torch.device("cuda" if o.device == "cuda" and torch.cuda.is_available() else "cpu")
    # The split is the one the checkpoint under test was made with; when neither side is a checkpoint
    # (a .phosmdl against the stage-A baseline) it is the default of train.py, which is what every
    # number of this project has been measured on.
    args = {"rotations": 1, "seed": 1, "split_seed": 1, "ctx": 256}
    if o.a.endswith(".pt"):
        ma, args = load(o.a)
        ma.to(dev)
    recs = dataset.load(o.root, dataset.packs_for(o.packs))
    tr, va, te = dataset.build_split(recs, o.split, rotations=args["rotations"],
                                     seed=dataset.split_seed_of(args))
    itr, _, _ = dataset.split(recs, o.split, seed=dataset.split_seed_of(args))
    tr_raw = [recs[i] for i in itr]

    # The mode label lives in the record, so setting it here reaches both models -- and a model
    # without mode.emb reads none of it, which is what makes the two sides comparable at all.
    for rec in te:
        dataset.mode_of(rec)
    if o.mode_eval == "aeolian":
        for rec in te:
            rec["mode"] = 0
    elif o.mode_eval == "shuffle":
        rng0 = np.random.default_rng(20260918)
        for role in range(len(dataset.ROLES)):
            sel = [r for r in te if r["role"] == role]
            lab = [r["mode"] for r in sel]
            rng0.shuffle(lab)
            for rec, m in zip(sel, lab):
                rec["mode"] = m
    print(f"mode slot: {o.mode_eval}")

    xa = per_line(ma, te, args["ctx"], dev) if o.a.endswith(".pt") else per_line_phosmdl(o.a, te)
    if o.b.startswith("markov"):
        xb = per_line_markov(tr_raw, te, 0 if o.b.endswith("0") else 2)
        name_b = f"stage A order {'0' if o.b.endswith('0') else '2'}"
    elif o.b.endswith(".phosmdl"):
        xb = per_line_phosmdl(o.b, te)
        name_b = os.path.basename(o.b)
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
