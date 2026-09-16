"""Trains and measures the bass pitch model: the fourth role, conditioned on the kick.

The protocol is the melodic round's, point for point, because the two numbers have to be comparable:
a three-way split by loop group, augmentation on the training side only, early stopping on the
validation set, the test set read once per model, and every comparison with a paired bootstrap over
**lines**. What is different is the subject and therefore the baselines.

**The baselines.** Two, and the second is the one that decides:

1. *Stage A, had it ever had a bass role.* The order-2 Witten-Bell chain of ``markov.py`` refit on
   the bass training lines. The bass has never been in ``CorpusTables.cpp``, so there is no shipped,
   contaminated variant of this number to print beside it -- the fair one is the only one there is.
2. *What ``Composer::composeBars`` does today*, expressed as a probability model over the same
   events. The composer is a sampler, so its density is measured: ``phos_render --midi`` writes a few
   thousand bars across the five style profiles, the bass track's symbols are read out in the same
   alphabet and with the same reference pitch as the corpus side, and the resulting distribution --
   add-one smoothed, because it is a point mass on the root and would otherwise score an infinity on
   the first note that is not one -- is scored on the held-out corpus lines. That is the number a
   learned bass has to beat to be worth shipping, and ``Tools/train/bass_stats.py`` prints it too.

**Why this file and not a branch in train.py.** ``train.py``, ``dataset.py``, ``models.py`` and
``export.py`` are shared with the melodic training round, which is running in another worktree. The
batching here differs by exactly one tensor (``kick``) and the role loops by one entry, and a branch
for each of those inside the shared files would be four conflict sites instead of none. The
duplication is the price of a clean merge and is deliberate; ``models.build`` is imported rather
than copied, and the one edit made to ``models.py`` is additive and off by default.

Usage:
    python Tools/train/train_bass.py --out Tools/train/runs/bass
    python Tools/train/train_bass.py --out Tools/train/runs/bass --generated <folder of MIDI exports>
"""
import argparse
import json
import math
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import torch                                                  # noqa: E402
import torch.nn.functional as F                               # noqa: E402

import bass                                                   # noqa: E402
import dataset                                                # noqa: E402
import markov                                                 # noqa: E402
import models                                                 # noqa: E402

PAD = -100
FIELDS_POS = ("tok", "step", "bar", "gap", "idx", "kick")
FIELDS_LINE = ("role", "style", "bars")


def device_of(want):
    if want == "cpu" or not torch.cuda.is_available():
        return torch.device("cpu")
    return torch.device("cuda")


def batches(records, bs, ctx, device, shuffle=True, seed=0):
    """Length-bucketed batches; right padding is safe because the model is causal."""
    order = sorted(range(len(records)), key=lambda i: len(records[i]["syms"]))
    chunks = [order[i:i + bs] for i in range(0, len(order), bs)]
    if shuffle:
        random.Random(seed).shuffle(chunks)
    for ch in chunks:
        enc = [bass.encode(records[i], ctx=ctx) for i in ch]
        T = max(len(e["tok"]) for e in enc)
        out = {}
        for k in FIELDS_POS:
            out[k] = torch.tensor([e[k] + [0] * (T - len(e[k])) for e in enc], dtype=torch.long, device=device)
        out["tgt"] = torch.tensor([e["tgt"] + [PAD] * (T - len(e["tgt"])) for e in enc], dtype=torch.long, device=device)
        for k in FIELDS_LINE:
            out[k] = torch.tensor([e[k] for e in enc], dtype=torch.long, device=device)
        yield out


@torch.no_grad()
def evaluate(model, records, ctx, device, bs=64):
    """Held-out NLL per token in nats -- the decision metric, the same one the melodic round uses."""
    model.eval()
    total, n = 0.0, 0
    for b in batches(records, bs, ctx, device, shuffle=False):
        loss = F.cross_entropy(model(b).reshape(-1, models.ALPHABET), b["tgt"].reshape(-1),
                               ignore_index=PAD, reduction="sum")
        total += float(loss)
        n += int((b["tgt"] != PAD).sum())
    model.train()
    return total / max(1, n)


@torch.no_grad()
def per_line_nll(model, records, ctx, device):
    """(summed NLL, tokens) of every line, for the paired bootstrap of ``confidence.py``."""
    import numpy as np
    model.eval()
    out = []
    for rec in records:
        e = bass.encode(rec, ctx=ctx)
        b = {k: torch.tensor([e[k]], dtype=torch.long, device=device) for k in FIELDS_POS}
        b.update({k: torch.tensor([e[k]], dtype=torch.long, device=device) for k in FIELDS_LINE})
        tgt = torch.tensor([e["tgt"]], dtype=torch.long, device=device)
        loss = F.cross_entropy(model(b).reshape(-1, models.ALPHABET), tgt.reshape(-1), reduction="sum")
        out.append((float(loss), len(e["tgt"])))
    model.train()
    return np.array(out, dtype=np.float64)


def train(model, train_recs, val_recs, *, steps, bs, ctx, lr, wd, device, warmup=200, eval_every=100,
          log=None, seed=0):
    """AdamW with a cosine schedule; keeps the parameters of the best validation NLL."""
    model.to(device).train()
    decay = [p for n, p in model.named_parameters() if p.dim() >= 2]
    plain = [p for n, p in model.named_parameters() if p.dim() < 2]
    opt = torch.optim.AdamW([{"params": decay, "weight_decay": wd}, {"params": plain, "weight_decay": 0.0}],
                            lr=lr, betas=(0.9, 0.95))
    best, best_state, best_step = float("inf"), None, 0
    curve, step, t0, epoch = [], 0, time.time(), 0
    while step < steps:
        for b in batches(train_recs, bs, ctx, device, seed=seed * 1000 + epoch):
            if step >= steps:
                break
            frac = step / max(1, steps)
            scale = (step + 1) / warmup if step < warmup else 0.5 * (1.0 + math.cos(math.pi * frac))
            for g in opt.param_groups:
                g["lr"] = lr * max(scale, 0.02)
            loss = F.cross_entropy(model(b).reshape(-1, models.ALPHABET), b["tgt"].reshape(-1), ignore_index=PAD)
            opt.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            step += 1
            if step % eval_every == 0 or step == steps:
                v = evaluate(model, val_recs, ctx, device)
                curve.append({"step": step, "train": loss.detach().item(), "val": v})
                if log:
                    print(f"    step {step:5d}  train {loss.detach().item():.4f}  val {v:.4f}", file=log, flush=True)
                if v < best:
                    best, best_step = v, step
                    best_state = {k: t.detach().clone() for k, t in model.state_dict().items()}
        epoch += 1
    if best_state is not None:
        model.load_state_dict(best_state)
    return {"best_val": best, "best_step": best_step, "curve": curve, "seconds": time.time() - t0}


# ------------------------------------------------------------------------------------------ baselines

def markov_nll(train_recs, test_recs, order=2):
    """Stage A's chain, refit on the bass training lines, in nats per token."""
    m = markov.MarkovModel(train_recs, bass.BASS_ROLE)
    total, n = 0.0, 0
    for rec in test_recs:
        a = b = bass.START_SYMBOL
        for c in rec["syms"]:
            total -= math.log(max(m.prob(a, b, c, order), 1e-12))
            n += 1
            a, b = b, c
    return total / max(1, n)


def markov_per_line(train_recs, test_recs, order=2):
    import numpy as np
    m = markov.MarkovModel(train_recs, bass.BASS_ROLE)
    out = []
    for rec in test_recs:
        a = b = bass.START_SYMBOL
        tot = 0.0
        for c in rec["syms"]:
            tot -= math.log(max(m.prob(a, b, c, order), 1e-12))
            a, b = b, c
        out.append((tot, len(rec["syms"])))
    return np.array(out, dtype=np.float64)


def composer_baseline(generated_folder, test_recs, alpha=1.0):
    """``Composer::composeBars`` as an order-0 density over symbols, in nats per token.

    Add-one over the 37 symbols, because the generator's own distribution is a point mass on the root
    and would score an infinity on the first corpus note that is not one. That smoothing is generous
    to the baseline; ``bass_stats.py`` reports the unsmoothed share of symbols it never produces.
    """
    import numpy as np
    import bass_stats
    _grids, syms, _kicks = bass_stats.generated_bars(generated_folder)
    counts = [0] * bass.ALPHABET
    for s in syms:
        counts[s] += 1
    n = sum(counts)
    p = [(c + alpha) / (n + alpha * bass.ALPHABET) for c in counts]
    out = []
    for rec in test_recs:
        tot = 0.0
        for c in rec["syms"]:
            tot -= math.log(p[c])
        out.append((tot, len(rec["syms"])))
    return np.array(out, dtype=np.float64), n


def boot_pair(xa, xb, resamples=10000, seed=11):
    """Paired percentile bootstrap over lines of (b - a), and the sign test."""
    import numpy as np
    rng = np.random.default_rng(seed)
    idx = rng.integers(0, len(xa), size=(resamples, len(xa)))
    da = xa[idx][:, :, 0].sum(1) / xa[idx][:, :, 1].sum(1)
    db = xb[idx][:, :, 0].sum(1) / xb[idx][:, :, 1].sum(1)
    d = db - da
    better = int((xa[:, 0] / xa[:, 1] < xb[:, 0] / xb[:, 1]).sum())
    return {"diff": float(xb[:, 0].sum() / xb[:, 1].sum() - xa[:, 0].sum() / xa[:, 1].sum()),
            "lo": float(np.percentile(d, 2.5)), "hi": float(np.percentile(d, 97.5)),
            "p_le_0": float((d <= 0).mean()), "better_lines": better, "lines": len(xa)}


def boot_one(x, resamples=10000, seed=7):
    import numpy as np
    rng = np.random.default_rng(seed)
    idx = rng.integers(0, len(x), size=(resamples, len(x)))
    s = x[idx]
    b = s[:, :, 0].sum(1) / s[:, :, 1].sum(1)
    return float(np.percentile(b, 2.5)), float(np.percentile(b, 97.5))


# ------------------------------------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--split", default="group", choices=["group", "file"])
    ap.add_argument("--dim", type=int, default=192)
    ap.add_argument("--heads", type=int, default=4)
    ap.add_argument("--ffn", type=int, default=512)
    ap.add_argument("--layers", type=int, default=4)
    ap.add_argument("--ctx", type=int, default=256)
    ap.add_argument("--dropout", type=float, default=0.3)
    ap.add_argument("--steps", type=int, default=4000)
    ap.add_argument("--bs", type=int, default=32)
    ap.add_argument("--lr", type=float, default=1.5e-3)
    ap.add_argument("--wd", type=float, default=0.1)
    ap.add_argument("--rotations", type=int, default=1)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--extra", default="none", choices=["none", "trance"],
                    help="add the trance bundle's bass lines to the TRAINING side only")
    ap.add_argument("--no-kick", action="store_true",
                    help="ablation: drop the kick conditioning, keep everything else")
    ap.add_argument("--generated", default="", help="folder of phos_render --midi exports (the Bass.cpp baseline)")
    ap.add_argument("--out", default="Tools/train/runs/bass")
    a = ap.parse_args()

    device = device_of(a.device)
    print(f"torch {torch.__version__}, cuda {torch.version.cuda}, device {device}"
          + (f" ({torch.cuda.get_device_name(0)})" if device.type == "cuda" else ""))

    recs = bass.load(a.root)
    tr, va, te = bass.build_split(recs, a.split, rotations=a.rotations, seed=a.seed * 7919 + 12345)
    kept = []
    if a.extra == "trance":
        extra = bass.load(a.root, bass.BASS_TRANCE_PACKS)
        kept = dataset.exclude_near(extra, va + te)
        tr = tr + kept
        print(f"  + {len(kept)} trance bass lines on the training side "
              f"({len(extra) - len(kept)} dropped as near duplicates of a held-out line)")
    print(f"bass corpus, split by {a.split}: {len(tr)} train / {len(va)} val / {len(te)} test lines; "
          f"{sum(len(r['syms']) for r in te)} test notes; "
          f"{100 * dataset.sibling_leak(tr, te):.1f} % of test lines have a sibling in train")

    itr, _iva, _ite = dataset.split(recs, a.split, seed=a.seed * 7919 + 12345)
    tr_raw = [recs[i] for i in itr] + kept

    out = {"lines": {"train": len(tr), "val": len(va), "test": len(te)},
           "test_notes": sum(len(r["syms"]) for r in te)}
    for order in (0, 1, 2):
        out[f"markov_order{order}"] = markov_nll(tr_raw, te, order)
        print(f"  stage A order {order} on the bass training lines   NLL {out[f'markov_order{order}']:.4f} nats")

    torch.manual_seed(a.seed)
    fields = models.BASS_FIELDS if not a.no_kick else models.ALL_FIELDS
    model = models.build("transformer", ctx=a.ctx, dropout=a.dropout, layers=a.layers, dim=a.dim,
                         heads=a.heads, ffn=a.ffn, fields=fields,
                         n_role=bass.N_ROLE_BASS, n_kick=bass.N_KICK)
    print(f"  transformer: {models.n_params(model):,} parameters, "
          f"kick conditioning {'off (ablation)' if a.no_kick else 'on'}")
    hist = train(model, tr, va, steps=a.steps, bs=a.bs, ctx=a.ctx, lr=a.lr, wd=a.wd, device=device,
                 log=sys.stdout, seed=a.seed)
    test_nll = evaluate(model, te, a.ctx, device)
    print(f"  transformer  val {hist['best_val']:.4f} @ {hist['best_step']}  test {test_nll:.4f}  "
          f"{hist['seconds']:.0f} s")
    out["transformer"] = {k: v for k, v in hist.items() if k != "curve"}
    out["transformer"]["test"] = test_nll
    out["transformer"]["params"] = models.n_params(model)
    out["transformer_curve"] = hist["curve"]

    xa = per_line_nll(model, te, a.ctx, device)
    lo, hi = boot_one(xa)
    print(f"  transformer test NLL {test_nll:.4f}  95 % CI [{lo:.4f}, {hi:.4f}]")
    out["transformer"]["ci"] = [lo, hi]
    xm = markov_per_line(tr_raw, te, 2)
    out["vs_markov2"] = boot_pair(xa, xm)
    print(f"  against stage A order 2: gap {out['vs_markov2']['diff']:.4f} nats, "
          f"95 % CI [{out['vs_markov2']['lo']:.4f}, {out['vs_markov2']['hi']:.4f}], "
          f"P(gap <= 0) = {out['vs_markov2']['p_le_0']:.4f}, better on "
          f"{out['vs_markov2']['better_lines']} of {out['vs_markov2']['lines']} lines")
    if a.generated:
        xc, nsyms = composer_baseline(a.generated, te)
        out["composer_nll"] = float(xc[:, 0].sum() / xc[:, 1].sum())
        out["vs_composer"] = boot_pair(xa, xc)
        print(f"  Composer::composeBars over {nsyms} generated notes: NLL {out['composer_nll']:.4f} nats")
        print(f"  against it: gap {out['vs_composer']['diff']:.4f} nats, "
              f"95 % CI [{out['vs_composer']['lo']:.4f}, {out['vs_composer']['hi']:.4f}], "
              f"P(gap <= 0) = {out['vs_composer']['p_le_0']:.4f}, better on "
              f"{out['vs_composer']['better_lines']} of {out['vs_composer']['lines']} lines")

    os.makedirs(a.out, exist_ok=True)
    torch.save({"state": model.state_dict(), "header": model.header(), "args": vars(a)},
               os.path.join(a.out, "bass.pt"))
    with open(os.path.join(a.out, "result.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(out, fh, indent=1)
    print("written", os.path.join(a.out, "result.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
