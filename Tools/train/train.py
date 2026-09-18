"""Trains and measures the two stage-B candidates of PLAN 6.9 on the same tokens and the same split.

The decision the plan asks for is made here and nowhere else: **held-out NLL per token**, on the
honest split, against the stage-A Markov model refit on the same training lines. Reputation does not
enter it (PLAN, literature round of 16.09.2026, point 4).

Protocol:

* the split is three-way by loop group (``dataset.split``): the validation set chooses the early
  stopping step, the test set is read once per model;
* both models get the same budget -- same tokens, same conditioning, same optimiser, same number of
  steps, parameter counts within a few per cent -- so that the comparison is of architectures and
  not of training effort;
* regularisation is strong, because the corpus is 37 000 notes and the models have of the order of a
  million parameters: dropout, weight decay, and early stopping on the validation NLL.

Usage:
    python Tools/train/train.py --arch transformer --out Tools/train/runs/tf
    python Tools/train/train.py --arch ssm --out Tools/train/runs/ssm
    python Tools/train/train.py --compare          # both, plus the Markov baselines, one table
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

import dataset                                                # noqa: E402
import markov                                                 # noqa: E402
import models                                                 # noqa: E402

PAD = -100


def device_of(want):
    if want == "cpu" or not torch.cuda.is_available():
        return torch.device("cpu")
    return torch.device("cuda")


def batches(records, bs, ctx, device, shuffle=True, seed=0):
    """Length-bucketed batches; right padding is safe because both models are causal."""
    order = sorted(range(len(records)), key=lambda i: len(records[i]["syms"]))
    chunks = [order[i:i + bs] for i in range(0, len(order), bs)]
    if shuffle:
        random.Random(seed).shuffle(chunks)
    for ch in chunks:
        enc = [dataset.encode(records[i], ctx=ctx) for i in ch]
        T = max(len(e["tok"]) for e in enc)
        out = {}
        for k in ("tok", "step", "bar", "gap", "idx"):
            out[k] = torch.tensor([e[k] + [0] * (T - len(e[k])) for e in enc], dtype=torch.long, device=device)
        out["tgt"] = torch.tensor([e["tgt"] + [PAD] * (T - len(e["tgt"])) for e in enc], dtype=torch.long, device=device)
        for k in ("role", "style", "bars", "mode"):
            out[k] = torch.tensor([e[k] for e in enc], dtype=torch.long, device=device)
        yield out


@torch.no_grad()
def evaluate(model, records, ctx, device, bs=64, per_role=False):
    """Held-out NLL per token in nats (the plan's decision metric)."""
    model.eval()
    total, n = 0.0, 0
    byrole = {r: [0.0, 0] for r in range(len(dataset.ROLES))}
    for b in batches(records, bs, ctx, device, shuffle=False):
        logits = model(b)
        loss = F.cross_entropy(logits.reshape(-1, models.ALPHABET), b["tgt"].reshape(-1),
                               ignore_index=PAD, reduction="none").reshape(b["tgt"].shape)
        mask = b["tgt"] != PAD
        total += float(loss[mask].sum())
        n += int(mask.sum())
        if per_role:
            for r in range(len(dataset.ROLES)):
                sel = mask & (b["role"][:, None] == r)
                byrole[r][0] += float(loss[sel].sum())
                byrole[r][1] += int(sel.sum())
    model.train()
    nll = total / max(1, n)
    if per_role:
        return nll, {dataset.ROLES[r]: (v[0] / v[1] if v[1] else float("nan")) for r, v in byrole.items()}
    return nll


def train(model, train_recs, val_recs, *, steps, bs, ctx, lr, wd, device, warmup=200, eval_every=100,
          log=None, seed=0):
    """AdamW with a cosine schedule; keeps the parameters of the best validation NLL."""
    model.to(device).train()
    # Weight decay on the matrices only. A_log and D are not weights but the SSM's time constants and
    # skip gains: decaying A_log pulls every channel towards A = -1, one and the same decay rate,
    # which is the opposite of what the S4D initialisation sets up. The first run of this file had
    # them in the decayed group and the SSM's validation NLL stopped improving after 200 steps.
    no_decay = {"A_log", "D"}
    decay = [p for n, p in model.named_parameters() if p.dim() >= 2 and n.rsplit(".", 1)[-1] not in no_decay]
    plain = [p for n, p in model.named_parameters() if p.dim() < 2 or n.rsplit(".", 1)[-1] in no_decay]
    opt = torch.optim.AdamW([{"params": decay, "weight_decay": wd}, {"params": plain, "weight_decay": 0.0}],
                            lr=lr, betas=(0.9, 0.95))
    best, best_state, best_step = float("inf"), None, 0
    curve, step, t0 = [], 0, time.time()
    epoch = 0
    while step < steps:
        for b in batches(train_recs, bs, ctx, device, seed=seed * 1000 + epoch):
            if step >= steps:
                break
            frac = step / max(1, steps)
            scale = (step + 1) / warmup if step < warmup else 0.5 * (1.0 + math.cos(math.pi * frac))
            for g in opt.param_groups:
                g["lr"] = lr * max(scale, 0.02)
            logits = model(b)
            loss = F.cross_entropy(logits.reshape(-1, models.ALPHABET), b["tgt"].reshape(-1), ignore_index=PAD)
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


def fields_for(cond, with_mode=False):
    """Which conditioning tables an ablation run may use (models.Conditioning explains why)."""
    extra = ("mode",) if with_mode else ()
    if cond == "full":
        return models.ALL_FIELDS + extra
    if cond == "role":          # exactly what the stage-A Markov model sees: symbols plus the role
        return ("role",) + extra
    if cond == "nometre":       # everything except the metrical position of the note
        return ("pos", "role", "style", "bars", "gap", "idx") + extra
    raise SystemExit(f"unknown --cond {cond}")


def shuffle_modes(records, seed):
    """Permutes the mode labels within each role -- the negative control for ``--mode-cond``.

    The marginal distribution of the labels is kept exactly (it is a permutation) and the relation
    between a label and the notes of its line is destroyed. Permuting **within** the role and not
    across it matters: the roles have different mode distributions, and a permutation across roles
    would also destroy that, so a model could recover part of the signal from the role it already
    has and the control would be weaker than it looks.
    """
    for rec in records:
        dataset.mode_of(rec)
    rng = random.Random(seed * 31 + 7)
    for role in range(len(dataset.ROLES)):
        sel = [r for r in records if r["role"] == role]
        labels = [r["mode"] for r in sel]
        rng.shuffle(labels)
        for rec, m in zip(sel, labels):
            rec["mode"] = m


def run_one(arch, tr, va, te, args, device, log=sys.stdout):
    torch.manual_seed(args.seed)
    kw = dict(ctx=args.ctx, dropout=args.dropout, layers=args.layers,
              fields=fields_for(args.cond, args.mode_cond))
    if arch == "transformer":
        kw.update(dim=args.dim, heads=args.heads, ffn=args.ffn, n_mode=dataset.N_MODE if args.mode_cond else 0)
    else:
        kw.update(dim=args.ssm_dim, state=args.state)
    model = models.build(arch, **kw)
    print(f"  {arch}: {models.n_params(model):,} parameters", file=log, flush=True)
    hist = train(model, tr, va, steps=args.steps, bs=args.bs, ctx=args.ctx, lr=args.lr, wd=args.wd,
                 device=device, log=log, seed=args.seed)
    test_nll, per_role = evaluate(model, te, args.ctx, device, per_role=True)
    hist.update({"arch": arch, "params": models.n_params(model), "test": test_nll, "test_per_role": per_role})
    return model, hist


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--packs", default="psy", choices=["psy", "trance", "all"])
    ap.add_argument("--split", default="group", choices=["group", "file", "naive"])
    ap.add_argument("--arch", default="transformer", choices=["transformer", "ssm"])
    ap.add_argument("--compare", action="store_true", help="both architectures and the baselines")
    ap.add_argument("--dim", type=int, default=192)
    ap.add_argument("--heads", type=int, default=4)
    ap.add_argument("--ffn", type=int, default=512)
    ap.add_argument("--ssm-dim", type=int, default=224)   # matches the transformer to 1.4 % of parameters
    ap.add_argument("--state", type=int, default=16)
    ap.add_argument("--layers", type=int, default=4)
    ap.add_argument("--ctx", type=int, default=256)
    ap.add_argument("--dropout", type=float, default=0.3)
    ap.add_argument("--steps", type=int, default=4000)
    ap.add_argument("--bs", type=int, default=32)
    ap.add_argument("--lr", type=float, default=1.5e-3)
    ap.add_argument("--wd", type=float, default=0.1)
    ap.add_argument("--rotations", type=int, default=1)
    ap.add_argument("--seed", type=int, default=1, help="weight init and batch order")
    ap.add_argument("--split-seed", type=int, default=1,
                    help="the held-out split; kept at 1 so every run is scored on the same test set "
                         "and the bootstrap against earlier models stays paired")
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--out", default="Tools/train/runs/compare")
    ap.add_argument("--extra", default="none",
                    help="comma-separated extra material for the TRAINING side only, keeping the psy "
                         "split for val/test so every number stays comparable and the bootstrap stays "
                         "paired: 'trance' (VORTEX), 'star' (the Star Samples psy folder, names that "
                         "state a role), 'starsynth' (its 'synth ...' files admitted as lead by "
                         "content, see rolecheck.py). Near duplicates of held-out lines are dropped.")
    ap.add_argument("--no-baseline", action="store_true",
                    help="skip the Markov baselines. They are the decision metric against stage A, "
                         "but they are recounted from scratch per run and cost minutes once the "
                         "training corpus is half a million notes; a weighting sweep does not need them")
    ap.add_argument("--trance-weight", type=float, default=1.0,
                    help="weight of the trance class (VORTEX + the super pack's trance directories) "
                         "relative to the psytrance lines; below 1 subsamples, above 1 repeats")
    ap.add_argument("--cond", default="full", choices=["full", "role", "nometre"],
                    help="ablation over the conditioning inputs; only 'full' may be exported")
    ap.add_argument("--style-from-pack", action="store_true",
                    help="label the style slot with the source pack (the side experiment of MODEL_FORMAT 3)")
    ap.add_argument("--mode-cond", action="store_true",
                    help="condition on the mode of the line as an eleventh embedding table "
                         "(condMode in the .phosmdl header). This is what Core/data/melody.phosmdl "
                         "carries since 18.09.2026. The label is estimated by Tools/train/mode.py "
                         "and is decided on only about a third of the corpus's lines, so read the "
                         "warning in docs/MODEL_FORMAT.md 3 before comparing NLLs across it; "
                         "transformer only")
    ap.add_argument("--mode-shuffle", action="store_true",
                    help="the negative control for --mode-cond: the same labels, permuted across "
                         "lines within each role. A mode table that helps must stop helping here, "
                         "or what it learned was capacity and not the mode")
    a = ap.parse_args()

    if (a.mode_cond or a.mode_shuffle) and (a.compare or a.arch != "transformer"):
        raise SystemExit("--mode-cond and --mode-shuffle are implemented for --arch transformer only; "
                         "the state-space model was measured at 2.16 nats against 1.49 and is not "
                         "the model this project ships (docs/MODEL_FORMAT.md section 4)")

    device = device_of(a.device)
    print(f"torch {torch.__version__}, cuda {torch.version.cuda}, available {torch.cuda.is_available()}, "
          f"device {device}" + (f" ({torch.cuda.get_device_name(0)})" if device.type == "cuda" else ""))

    recs = dataset.load(a.root, dataset.packs_for(a.packs))
    if a.mode_shuffle:
        shuffle_modes(recs, a.seed)
    tr, va, te = dataset.build_split(recs, a.split, rotations=a.rotations,
                                     seed=a.split_seed * 7919 + 12345)
    kept = dataset.assemble_extra(a.root, a.extra, va + te, a.trance_weight, log=sys.stdout)
    if a.mode_shuffle:
        shuffle_modes(kept, a.seed + 1)
    # The duplicate pass runs over the merged corpus, not per pack: the same loop is resold across
    # vendors, and a per-pack pass would leave every copy of it in the training distribution.
    merged, removed = dataset.dedupe(tr + kept)
    print(f"  merged corpus: {len(tr) + len(kept)} lines -> {len(merged)} after the cross-vendor "
          f"duplicate pass ({removed} removed)")
    tr = merged
    if a.style_from_pack:
        for r in tr + va + te:
            r["style"] = 1 + r["pack"] % (dataset.N_STYLE - 1)
    print(f"{a.packs} corpus, split by {a.split}: {len(tr)} train / {len(va)} val / {len(te)} test lines; "
          f"{sum(len(r['syms']) for r in te)} test notes; "
          f"{100 * dataset.sibling_leak(tr, te):.1f} % of test lines have a sibling in train")

    # The Markov baseline is fit on the *unaugmented* training lines. A bar rotation leaves the
    # cyclic n-gram types of a loop unchanged and only doubles their counts, which would sharpen
    # Witten-Bell's escape and make the baseline look worse than the model that is actually shipped.
    if a.split == "naive":
        tr_raw = tr
    else:
        itr, _iva, _ite = dataset.split(recs, a.split, seed=a.split_seed * 7919 + 12345)
        tr_raw = dataset.dedupe([recs[i] for i in itr] + kept)[0]

    out = {}
    if not a.no_baseline:
        out["markov_order0"] = markov.nll(tr_raw, te, order=0)
        out["markov_order1"] = markov.nll(tr_raw, te, order=1)
        out["markov_fair"] = markov.nll(tr_raw, te, per_role=True, order=2)
        out["markov_shipped"] = markov.nll(recs, te, per_role=True, order=2)
        print(f"  stage A order 0, on train  NLL {out['markov_order0']:.4f}")
        print(f"  stage A order 1, on train  NLL {out['markov_order1']:.4f}")
        print(f"  stage A order 2, on train  NLL {out['markov_fair'][0]:.4f}  {out['markov_fair'][1]}")
        print(f"  stage A order 2, shipped   NLL {out['markov_shipped'][0]:.4f}  (contaminated: the test "
              f"lines are inside these counts)")

    archs = ["transformer", "ssm"] if a.compare else [a.arch]
    os.makedirs(a.out, exist_ok=True)
    for arch in archs:
        model, hist = run_one(arch, tr, va, te, a, device)
        print(f"  {arch:12s} params {hist['params']:,}  val {hist['best_val']:.4f} @ {hist['best_step']}  "
              f"test {hist['test']:.4f}  {hist['test_per_role']}  {hist['seconds']:.0f} s")
        out[arch] = {k: v for k, v in hist.items() if k != "curve"}
        out[arch + "_curve"] = hist["curve"]
        torch.save({"state": model.state_dict(), "header": model.header(), "args": vars(a)},
                   os.path.join(a.out, f"{arch}.pt"))
    with open(os.path.join(a.out, "result.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(out, fh, indent=1)
    print("written", os.path.join(a.out, "result.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
