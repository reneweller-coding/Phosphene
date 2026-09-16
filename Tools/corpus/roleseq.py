"""Does a sequence model read a track's role better than summary statistics? Measured, and no.

``rolemodel.py`` describes a track by 32 summary numbers -- register, polyphony, the interval
histogram, the kick grid, whether the pitches fit a scale. Every one of those is an average over the
whole track, and averages throw away order: a line that climbs a scale and one that descends it have
the same interval histogram, and a bass that plays its root on beats one and three and a fill on four
looks like one that plays the same notes in any other arrangement. The obvious question is whether
reading the notes *in order* recovers what the averages lost, which is what a small convolutional
network over the note sequence does (Bai, Kolter, Koltun, "An empirical evaluation of generic
convolutional and recurrent networks for sequence modeling", arXiv 2018, on dilated causal
convolutions as the default sequence encoder before attention is worth its cost).

**The answer is no.** On the same five folds, split by folder, over the same 11 316 name-labelled
tracks (melodic precision / recall at plain argmax):

    sequence CNN (notes in order)     74.6 % / 82.3 %      acid precision 54.1 %
    summary content features only     82.5 % / 84.7 %      acid precision 23.6 %
    all feature blocks (shipped)      85.1 % / 86.4 %      acid precision 94.6 %

The order does carry something the averages miss, and it is exactly where one would expect it: on
``acid`` -- the class the previous round could not separate from the bass -- the sequence encoder
more than doubles the precision of the summary features, 54.1 % against 23.6 %. It is still nowhere
near the 94.6 % that the *folder* block reaches, and on the melodic class as a whole it is eight
points behind summary statistics it had every chance to reconstruct. Two reasons: the corpus has
11 316 labelled tracks, enough to fit 32 numbers and not enough to fit an encoder that must learn
those 32 numbers from scratch before it can learn anything beyond them; and the decisive evidence in
this corpus is not inside the track at all but beside it, in what its folder's other tracks are
called. Sequence modelling is answering a question that is not the binding one here.

This file exists so that the negative result is reproducible rather than asserted. It is not used by
anything that ships.

Usage:
    python Tools/corpus/roleseq.py --compare
"""
import argparse
import hashlib
import json
import os
import sys

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

import labelsample                                              # noqa: E402
import roledetect as rd                                         # noqa: E402
import rolemodel as rm                                          # noqa: E402

MAXLEN = 128
#: Token vocabularies of the five parallel streams. Pitch is relative to the track's own median and
#: clamped, so the encoder sees a shape and not a key -- the same invariance ``dataset.py`` builds
#: into the model's own symbols.
N_PITCH, N_STEP, N_LEN, N_GAP, N_VEL = 49, 16, 9, 10, 8
PITCH_MIN = -24


def sequences(root, recs, refresh=False):
    """(id -> five parallel token arrays) for every track, cached like the feature scan."""
    os.makedirs(rd.CACHE_DIR, exist_ok=True)
    tag = hashlib.sha1(f"seq|{root}|v{rd.FEATURE_VERSION}|{MAXLEN}".encode()).hexdigest()[:12]
    cp = os.path.join(rd.CACHE_DIR, f"seq_{tag}.json")
    if os.path.exists(cp) and not refresh:
        with open(cp, "r", encoding="utf-8") as fh:
            return json.load(fh)
    by_path = {}
    for r in recs:
        by_path.setdefault(r["path"], []).append(r["track"])
    out = {}
    for rel, tis in sorted(by_path.items()):
        try:
            ppq, tracks = rd.bc.read_midi_tracks(os.path.join(root, rel))
        except Exception:                                       # noqa: BLE001
            continue
        if not ppq:
            continue
        step = ppq / 4.0
        for ti in tis:
            if ti >= len(tracks):
                continue
            notes = sorted(tracks[ti]["notes"])
            if len(notes) < 4:
                continue
            top = {}
            for s, e, p, v in notes:                            # the top voice of each onset step
                k = int(round(s / step))
                if k not in top or p > top[k][0]:
                    top[k] = (p, e - s, v)
            ks = sorted(top)[:MAXLEN]
            med = int(np.median([top[k][0] for k in ks]))
            pit, stp, ln, gp, vel = [], [], [], [], []
            for i, k in enumerate(ks):
                p, d, v = top[k]
                pit.append(int(np.clip(p - med - PITCH_MIN, 0, N_PITCH - 1)))
                stp.append(k % 16)
                ln.append(int(np.clip(round(d / step), 0, N_LEN - 1)))
                gp.append(int(np.clip(ks[i + 1] - k, 0, N_GAP - 1)) if i + 1 < len(ks) else 0)
                vel.append(int(np.clip(v // 16, 0, N_VEL - 1)))
            out[f"{rel}|{ti}"] = [pit, stp, ln, gp, vel]
    ids = {}
    for r in recs:
        key = f"{r['path']}|{r['track']}"
        if key in out:
            ids[r["id"]] = out[key]
    with open(cp, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(ids, fh)
    return ids


def pack(seqs, recs):
    """(tokens[N,5,MAXLEN], lengths[N]) in the order of ``recs``; missing tracks get a zero row."""
    X = np.zeros((len(recs), 5, MAXLEN), dtype=np.int64)
    L = np.zeros(len(recs), dtype=np.int64)
    for i, r in enumerate(recs):
        s = seqs.get(r["id"])
        if not s:
            continue
        n = min(len(s[0]), MAXLEN)
        for c in range(5):
            X[i, c, :n] = s[c][:n]
        L[i] = n
    return X, L


def build_net(n_classes, dim=64):
    """Summed embeddings, three dilated convolutions, masked mean-and-max pooling, a linear head."""
    import torch
    import torch.nn as nn

    class Net(nn.Module):
        def __init__(self):
            super().__init__()
            self.emb = nn.ModuleList([nn.Embedding(n, dim) for n in
                                      (N_PITCH, N_STEP, N_LEN, N_GAP, N_VEL)])
            self.convs = nn.ModuleList([nn.Conv1d(dim, dim, 5, padding=2 * d, dilation=d)
                                        for d in (1, 2, 4)])
            self.norms = nn.ModuleList([nn.BatchNorm1d(dim) for _ in range(3)])
            self.drop = nn.Dropout(0.3)
            self.head = nn.Linear(2 * dim, n_classes)

        def forward(self, x, mask):
            h = sum(e(x[:, i]) for i, e in enumerate(self.emb)).transpose(1, 2)
            for c, n in zip(self.convs, self.norms):
                h = torch.relu(n(c(h))) + h
            h = h * mask[:, None, :]
            mean = h.sum(-1) / mask.sum(-1, keepdim=True).clamp(min=1)
            mx = h.masked_fill(mask[:, None, :] == 0, -1e9).max(-1).values
            return self.head(self.drop(torch.cat([mean, mx], -1)))

    return Net()


def cv_sequence(all_recs, recs, ys, group="dir", folds=5, epochs=12, device="cuda",
                root="M:/Midi"):
    """Out-of-fold probabilities of the sequence encoder on the folds ``rolemodel.cv_predict`` uses."""
    import torch
    import torch.nn.functional as F
    from sklearn.model_selection import GroupKFold
    dev = torch.device(device if torch.cuda.is_available() and device == "cuda" else "cpu")
    seqs = sequences(root, all_recs)
    X, L = pack(seqs, recs)
    y = np.array([rm.CLASSES.index(c) for c in ys])
    groups = np.array([r[group] for r in recs])
    proba = np.zeros((len(recs), len(rm.CLASSES)))
    ar = torch.arange(MAXLEN)
    for tr, te in GroupKFold(n_splits=folds).split(X, y, groups):
        net = build_net(len(rm.CLASSES)).to(dev)
        opt = torch.optim.AdamW(net.parameters(), lr=2e-3, weight_decay=0.01)
        xt = torch.tensor(X[tr], device=dev)
        mt = (ar[None, :] < torch.tensor(L[tr])[:, None]).float().to(dev)
        yt = torch.tensor(y[tr], device=dev)
        n = len(tr)
        for ep in range(epochs):
            perm = torch.randperm(n, device=dev)
            for i in range(0, n, 256):
                b = perm[i:i + 256]
                loss = F.cross_entropy(net(xt[b], mt[b]), yt[b])
                opt.zero_grad(set_to_none=True)
                loss.backward()
                opt.step()
        net.eval()
        with torch.no_grad():
            xe = torch.tensor(X[te], device=dev)
            me = (ar[None, :] < torch.tensor(L[te])[:, None]).float().to(dev)
            for i in range(0, len(te), 512):
                p = torch.softmax(net(xe[i:i + 512], me[i:i + 512]), -1)
                proba[te[i:i + 512]] = p.cpu().numpy()
    return proba, np.array(ys)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--group", default="dir", choices=("dir", "path"))
    ap.add_argument("--epochs", type=int, default=12)
    ap.add_argument("--compare", action="store_true")
    a = ap.parse_args()
    recs = rd.load(a.root)
    labels = labelsample.load_labels()
    tracks, ys = rm.labelled_tracks(recs, set(labels))
    print(f"{len(tracks)} name-labelled tracks outside the hand-labelled sample")

    def row(name, p, yy):
        r0, r7 = rm.pr_table(p, yy, 0.0), rm.pr_table(p, yy, 0.70)
        print(f"  {name:34s} {100 * r0['MELODIC']['precision']:7.1f}% {100 * r0['MELODIC']['recall']:7.1f}% "
              f"{100 * r7['MELODIC']['precision']:8.1f}% {100 * r7['MELODIC']['recall']:8.1f}% "
              f"{100 * r0['acid']['precision']:8.1f}% {100 * r0['bass']['precision']:8.1f}%")

    print(f"  {'encoder':34s} {'mel P@0':>8s} {'mel R@0':>8s} {'mel P@.7':>9s} {'mel R@.7':>9s} "
          f"{'acid P@0':>9s} {'bass P@0':>9s}")
    ps, yv = cv_sequence(recs, tracks, ys, group=a.group, epochs=a.epochs, root=a.root)
    row("sequence CNN (notes in order)", ps, yv)
    if a.compare:
        pc, yc = rm.cv_predict(recs, tracks, ys, blocks=("content",), group=a.group)
        row("summary content features only", pc, yc)
        pa, ya = rm.cv_predict(recs, tracks, ys, group=a.group)
        row("all feature blocks (shipped)", pa, ya)
    return 0


if __name__ == "__main__":
    sys.exit(main())
