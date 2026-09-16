"""The two stage-B candidates of PLAN 6.9, over the unchanged stage-A alphabet.

Both models predict one of the 37 symbols of ``phos::kCorpusAlphabet`` and nothing else. What the
REMI and Compound-Word token streams (Huang, Yang, "Pop Music Transformer", ACM MM 2020; Hsiao,
Liu, Yeh, Yang, "Compound Word Transformer", AAAI 2021) carry as extra token families -- the step in
the bar, the distance to the next note, the position in the pattern -- enters here as conditioning
embeddings that are added to the input, because the C++ sampler draws from a fixed 37-symbol
alphabet under constraint masks and Phase 8 is not allowed to change that (PLAN 12, Phase 8).

**Transformer** -- a small decoder-only, pre-LayerNorm stack after Music Transformer (Huang et al.,
"Music Transformer: Generating Music with Long-Term Structure", ICLR 2019, arXiv 2018). Its relative
attention is deliberately *not* used: the corpus is 37 000 notes, the plan's budget is 1 to 3 M
parameters, and the metrical position that relative attention is good at recovering is handed to the
model directly as ``step`` and ``bar``. Absolute learned positions plus the conditioning is the
cheaper way to the same information here, and it exports as one more embedding table.

**State-space model** -- a selective SSM after Gu and Dao ("Mamba: Linear-Time Sequence Modeling with
Selective State Spaces", 2023): input-dependent Delta, B and C around a diagonal A, with the gated
block (causal depthwise convolution, SiLU gate) of that paper. A is kept **real and diagonal**
rather than complex (S4/S4D, Gu, Goel, Re, ICLR 2022) so that the recurrence is one multiply-add per
channel and state per step -- a scan of ten lines in C++, an inference state of di*state floats and
no KV cache, which is the property that made the plan want it measured against the transformer for
the Quest at all. Complex diagonal A (S4D-Lin) would buy oscillatory kernels at the price of a
complex scan in the runtime; at 256 notes of context that is not a trade worth making.

Both are written so that every tensor maps one-to-one onto a record of docs/MODEL_FORMAT.md; the
forward passes here and section 4 of that document are the same arithmetic in the same order.
"""
import math

import torch
import torch.nn as nn
import torch.nn.functional as F

from dataset import ALPHABET, N_BAR, N_BARS, N_GAP, N_IDX, N_ROLE, N_STEP, N_STYLE

EPS = 1e-5


ALL_FIELDS = ("pos", "role", "style", "bars", "step", "bar", "gap", "idx")


class Conditioning(nn.Module):
    """The nine embedding tables whose sum is the input of block 0 (MODEL_FORMAT section 4).

    ``fields`` exists for one measurement and is not a shipping option: with ``fields=("role",)`` the
    model sees exactly what the stage-A Markov model sees -- the preceding symbols and the role -- so
    the gap between that run and the full one says how much of stage B's advantage is the
    architecture and how much is the extra conditioning. An exported file always carries all nine
    tables; a table that was switched off exports as its (untrained) initial values, which is why an
    ablation run is never exported.
    """

    def __init__(self, dim, ctx, dropout, fields=ALL_FIELDS):
        super().__init__()
        self.ctx = ctx
        self.fields = tuple(fields)
        self.tok = nn.Embedding(ALPHABET, dim)
        self.pos = nn.Embedding(ctx, dim)
        self.role = nn.Embedding(N_ROLE, dim)
        self.style = nn.Embedding(N_STYLE, dim)
        self.bars = nn.Embedding(N_BARS, dim)
        self.step = nn.Embedding(N_STEP, dim)
        self.bar = nn.Embedding(N_BAR, dim)
        self.gap = nn.Embedding(N_GAP, dim)
        self.idx = nn.Embedding(N_IDX, dim)
        self.drop = nn.Dropout(dropout)

    def forward(self, b):
        t = torch.arange(b["tok"].shape[1], device=b["tok"].device).clamp_(max=self.ctx - 1)
        x = self.tok(b["tok"])
        if "pos" in self.fields:
            x = x + self.pos(t)[None]
        for f in ("role", "style", "bars"):                 # one value per line
            if f in self.fields:
                x = x + getattr(self, f)(b[f])[:, None]
        for f in ("step", "bar", "gap", "idx"):             # one value per position
            if f in self.fields:
                x = x + getattr(self, f)(b[f])
        return self.drop(x)


# ---------------------------------------------------------------------------------------- transformer

class Block(nn.Module):
    def __init__(self, dim, heads, ffn, dropout):
        super().__init__()
        self.heads = heads
        self.norm1 = nn.LayerNorm(dim, eps=EPS)
        self.qkv = nn.Linear(dim, 3 * dim)
        self.out = nn.Linear(dim, dim)
        self.norm2 = nn.LayerNorm(dim, eps=EPS)
        self.up = nn.Linear(dim, ffn)
        self.down = nn.Linear(ffn, dim)
        self.drop = nn.Dropout(dropout)

    def forward(self, x):
        B, T, E = x.shape
        h = self.norm1(x)
        q, k, v = self.qkv(h).split(E, dim=-1)
        shape = (B, T, self.heads, E // self.heads)
        q, k, v = (z.view(shape).transpose(1, 2) for z in (q, k, v))
        a = F.scaled_dot_product_attention(q, k, v, is_causal=True)
        x = x + self.drop(self.out(a.transpose(1, 2).reshape(B, T, E)))
        return x + self.drop(self.down(F.gelu(self.up(self.norm2(x)), approximate="tanh")))


class Transformer(nn.Module):
    arch = "transformer"

    def __init__(self, dim=192, layers=4, heads=4, ffn=512, ctx=256, dropout=0.3, fields=ALL_FIELDS):
        super().__init__()
        self.dim, self.layers, self.heads, self.ffn, self.ctx, self.state = dim, layers, heads, ffn, ctx, 0
        self.emb = Conditioning(dim, ctx, dropout, fields)
        self.blocks = nn.ModuleList([Block(dim, heads, ffn, dropout) for _ in range(layers)])
        self.norm = nn.LayerNorm(dim, eps=EPS)
        self.head = nn.Linear(dim, ALPHABET)
        self.apply(_init)

    def forward(self, b):
        x = self.emb(b)
        for blk in self.blocks:
            x = blk(x)
        return self.head(self.norm(x))

    def header(self):
        return {"arch": "transformer", "layers": self.layers, "dim": self.dim, "heads": self.heads,
                "ffn": self.ffn, "state": 0, "ctx": self.ctx, "act": "gelu_tanh"}


# ---------------------------------------------------------------------------------------- selective SSM

class SSMBlock(nn.Module):
    """One Mamba block with a real diagonal A; the scan is the recurrence of MODEL_FORMAT section 4."""

    def __init__(self, dim, state, expand, dt_rank, conv_k, dropout):
        super().__init__()
        di = dim * expand
        self.dim, self.di, self.state, self.dt_rank, self.conv_k = dim, di, state, dt_rank, conv_k
        self.norm1 = nn.LayerNorm(dim, eps=EPS)
        self.inp = nn.Linear(dim, 2 * di)
        self.conv = nn.Conv1d(di, di, conv_k, groups=di, padding=conv_k - 1)
        self.xproj = nn.Linear(di, dt_rank + 2 * state, bias=False)
        self.dt = nn.Linear(dt_rank, di)
        # A = -exp(A_log) with A_log initialised to log(1..state), the S4D-Real initialisation of
        # Gu, Goel, Gupta, Re ("On the parameterization and initialization of diagonal state space
        # models", NeurIPS 2022): a spread of time constants rather than one.
        self.A_log = nn.Parameter(torch.log(torch.arange(1, state + 1, dtype=torch.float32)).repeat(di, 1).clone())
        self.D = nn.Parameter(torch.ones(di))
        self.outp = nn.Linear(di, dim)
        self.drop = nn.Dropout(dropout)
        self.post_init()

    def post_init(self):
        """softplus(dt.bias) starts spread over [1e-3, 1e-1], as in the Mamba reference implementation.

        Called again after the generic initialiser, which would otherwise zero this bias and start
        every channel at the same time constant.
        """
        with torch.no_grad():
            u = torch.rand(self.di) * (math.log(1e-1) - math.log(1e-3)) + math.log(1e-3)
            self.dt.bias.copy_(u.exp() + torch.log(-torch.expm1(-u.exp())))
            self.A_log.copy_(torch.log(torch.arange(1, self.state + 1, dtype=torch.float32)).repeat(self.di, 1))
            self.D.fill_(1.0)

    def forward(self, x):
        B, T, _ = x.shape
        u = self.norm1(x)
        xin, z = self.inp(u).split(self.di, dim=-1)
        xc = self.conv(xin.transpose(1, 2))[:, :, :T].transpose(1, 2)    # causal: drop the right pad
        xs = F.silu(xc)
        dtr, Bm, Cm = self.xproj(xs).split([self.dt_rank, self.state, self.state], dim=-1)
        delta = F.softplus(self.dt(dtr))                                  # (B, T, di)
        A = -torch.exp(self.A_log)                                        # (di, state)
        # The scan, step by step. The (B, T, di, state) tensors are never materialised: at 256 notes
        # and di = 352 they would be a third of a gigabyte each, and the C++ side runs this loop too.
        h = torch.zeros(B, self.di, self.state, device=x.device, dtype=x.dtype)
        ys = []
        for t in range(T):
            dt_t = delta[:, t].unsqueeze(-1)                              # (B, di, 1)
            h = torch.exp(dt_t * A) * h + dt_t * xs[:, t].unsqueeze(-1) * Bm[:, t].unsqueeze(1)
            ys.append(torch.einsum("bdn,bn->bd", h, Cm[:, t]))
        y = torch.stack(ys, dim=1) + self.D * xs
        return x + self.drop(self.outp(y * F.silu(z)))


class SSM(nn.Module):
    arch = "ssm"

    def __init__(self, dim=224, layers=4, state=16, expand=2, dt_rank=12, conv_k=4, ctx=256, dropout=0.3,
                 fields=ALL_FIELDS):
        super().__init__()
        self.dim, self.layers, self.state, self.expand, self.dt_rank, self.conv_k, self.ctx = \
            dim, layers, state, expand, dt_rank, conv_k, ctx
        self.emb = Conditioning(dim, ctx, dropout, fields)
        self.blocks = nn.ModuleList([SSMBlock(dim, state, expand, dt_rank, conv_k, dropout) for _ in range(layers)])
        self.norm = nn.LayerNorm(dim, eps=EPS)
        self.head = nn.Linear(dim, ALPHABET)
        self.apply(_init)
        for blk in self.blocks:
            blk.post_init()

    def forward(self, b):
        x = self.emb(b)
        for blk in self.blocks:
            x = blk(x)
        return self.head(self.norm(x))

    def header(self):
        return {"arch": "ssm", "layers": self.layers, "dim": self.dim, "heads": 0, "ffn": 0,
                "state": self.state, "ctx": self.ctx, "act": "silu",
                "expand": self.expand, "dtRank": self.dt_rank, "convK": self.conv_k}


def _init(m):
    """Small normal init; the A_log and dt.bias of the SSM blocks are set in their constructor."""
    if isinstance(m, nn.Linear):
        nn.init.normal_(m.weight, std=0.02)
        if m.bias is not None:
            nn.init.zeros_(m.bias)
    elif isinstance(m, nn.Embedding):
        nn.init.normal_(m.weight, std=0.02)
    elif isinstance(m, nn.Conv1d):
        nn.init.normal_(m.weight, std=0.1)
        if m.bias is not None:
            nn.init.zeros_(m.bias)


def build(arch, **kw):
    return Transformer(**kw) if arch == "transformer" else SSM(**kw)


def n_params(model):
    return sum(p.numel() for p in model.parameters())
