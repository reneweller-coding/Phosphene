# `.phosmdl` — the weight file of the stage-B pitch model

Phase 8 of PLAN 6.9 replaces the prediction model of the melodic layer, **not** the representation.
Stage B draws from exactly the alphabet stage A draws from: 37 symbols, the interval to the tonic in
semitones from −12 to +24 (`kCorpusRelMin`/`kCorpusRelMax` in `Core/include/phos/Corpus.h`), in the
same order, with the same three roles (acid, lead, arp). Everything the token stream of Hsiao et al.
("Compound Word Transformer", AAAI 2021) would carry in extra token families — step in the bar,
length, position in the pattern — enters this model as **conditioning inputs**, never as extra
symbols in the output distribution. A `.phosmdl` file therefore always ends in a softmax over 37
symbols and nothing else.

This document is the contract between `Tools/train/` (writes the file) and the C++ inference
(reads it). It is normative; where the trainer and this text disagree, the text is the bug report.

## 1. Byte layout

Little endian throughout. `float` is IEEE-754 binary32.

```
char     magic[8]    = "PHOSMDL1"
uint32   version     = 1
uint32   headerLen                      // bytes of the text header, not counting any padding
char     header[headerLen]              // "key=value\n" lines, ASCII, see section 2
then, per tensor, repeated until the end marker:
  char   name[48]                       // zero padded, e.g. "blocks.0.attn.qkv.w"
  uint8  dtype                          // 0 = float32, 1 = int8 with one scale per output row
  uint8  ndim                           // 1 or 2 in version 1
  uint16 reserved = 0
  uint32 dim[4]                         // unused dimensions are 1
  float  scale[dim[0]]                  // only when dtype == 1
  uint8  pad[...]                       // zeros, so the data begins 32-byte aligned from the file start
  data                                  // dim[0]*dim[1]*dim[2]*dim[3] elements, row major
char     magic2[8]   = "PHOSEND1"
```

**Reading the tensor list.** Before each record the reader peeks eight bytes: if they are
`PHOSEND1` the list is over, otherwise they are the first eight bytes of a tensor name. No tensor
is ever named `PHOSEND1`, so the two cannot be confused. The tensor header is
`48 + 1 + 1 + 2 + 16 = 68` bytes; after it come `dim[0]` floats of scale when `dtype == 1`, then
zero padding until the current offset **counted from the start of the file** is a multiple of 32,
then the data. Element size is 4 bytes for `dtype == 0` and 1 byte for `dtype == 1`. There is no
padding after the data; the next record (or `PHOSEND1`) begins immediately.

**int8 quantisation.** One scale per row of the first dimension:

```
scale[r] = max_c |W[r][c]| / 127          (a row of zeros gets scale[r] = 0)
q[r][c]  = clamp(round(W[r][c] / scale[r]), -127, 127)
W[r][c] ~= q[r][c] * scale[r]
```

Dequantise a row before using it; `-128` never occurs, so a symmetric int8 dot product is safe.
The exporter quantises every two-dimensional tensor and leaves every one-dimensional tensor
(biases, LayerNorm weights, `D`, `A_log` is two-dimensional and therefore quantised) in float32.
A reader must not rely on that rule — it must honour the `dtype` byte of each record.

## 2. Header keys

`key=value\n` lines, ASCII, no spaces around `=`. Unknown keys are ignored; the keys below are
always present.

| Key | Meaning |
|---|---|
| `arch` | `transformer` or `ssm` |
| `layers` | number of blocks |
| `dim` | model width |
| `heads` | attention heads (`0` for `arch=ssm`) |
| `ffn` | width of the feed-forward hidden layer (`0` for `arch=ssm`) |
| `state` | SSM state size per inner channel (`0` for `arch=transformer`) |
| `vocab` | always `37` in token version 1 |
| `ctx` | rows of `pos.emb`; positions beyond it clamp to the last row |
| `roles` | always `3` (acid, lead, arp — the order of `phos::CorpusRoleId`) |
| `styles` | always `6` (0 = unknown, then `phos::StyleId` + 1) |
| `tokenVersion` | `1` — the alphabet and the conditioning fields of section 3 |
| `quant` | `int8` or `float32`; the dtype the exporter used for two-dimensional tensors |
| `relMin`, `relMax` | `-12`, `24` — the alphabet's range, for a sanity check against `Corpus.h` |
| `condStep`, `condBar`, `condGap`, `condIdx`, `condBars` | sizes of the conditioning tables of section 3 |
| `eps` | LayerNorm / RMSNorm epsilon |
| `act` | `gelu_tanh` (transformer) or `silu` (ssm) |
| `expand`, `dtRank`, `convK` | SSM only: inner expansion factor, rank of the Δ projection, depthwise kernel width |
| `nll` | held-out NLL per token in nats that the training run measured for this file |

## 3. Inputs

The model is a causal language model over one melodic line. A line is the sequence of **note
onsets** of one pattern, in time order — exactly the `rels` list that
`Tools/corpus/build_corpus.py` builds, i.e. one symbol per onset, not one symbol per sixteenth.

Let the line have notes `s_0 .. s_{N-1}`. At input position `t` (`0 <= t < N`):

* the **token** is `s_{t-1}`, with `s_{-1} = 12`, the symbol of the interval 0 — the same start
  context the stage-A sampler uses (`drawPitches(..., 0, 0, ...)` in `Core/src/Melody.cpp`);
* the **conditioning** describes the note being predicted, `s_t`, not the token that is fed in.
  Every field is known to the composer before it draws any pitch, because all three makers in
  `Melody.cpp` (`makeAcid`, `makeLead`, `makeArp`) draw the rhythm first and the pitches
  afterwards;
* the **output** is the distribution over `s_t`.

| Field | Values | Where the composer has it |
|---|---|---|
| `role` | 0 acid, 1 lead, 2 arp | the maker that is running (`CorpusRoleId`) |
| `style` | 0 unknown, 1 + `StyleId` (Goa, FullOn, Progressive, DarkForest, HiTech) | `styleOf(p)`; **see the warning below** |
| `bars` | `clamp(bars_of_pattern, 1, 8) - 1` | `m.acidSteps / 16`, 2 for a lead window, 1 for an arp cell |
| `step` | `step_of_note % 16` | the onset list `on` |
| `bar` | `(step_of_note / 16) % 8` | the onset list `on` |
| `gap` | `0` for the last note of the line, otherwise `clamp(next_step - step, 1, 8)`, and `9` for a gap above 8 | the onset list `on` |
| `idx` | bucket of the note index `t`: `0,1,2,3` for `t = 0,1,2,3`, `4` for `t` in 4..5, `5` for 6..9, `6` for 10..15, `7` for 16 and above | the loop counter |

`role`, `style` and `bars` are the same for every position of a line; `step`, `bar`, `gap` and `idx`
change per position.

> **Warning about `style`.** The MIDI packs carry no style label that maps onto the five style
> profiles of `Form.h`. Every training line was therefore labelled `style = 0` (unknown), and the
> C++ side must pass `0`. The slot exists so that a later, labelled corpus can use it without a
> format change. `Tools/train/` measured what a style-like label is worth by conditioning on the
> pack instead (section 6 of the Phase 8 status block in `docs/PLAN.md`).

## 4. Tensors

`E` is `dim`. Two-dimensional weights are stored **output-major**: `W[out][in]`, so `y = W x + b`
reads row by row. Every tensor listed is present. The order in the file is: the **nine embedding
tables**, then the **blocks** `0 .. layers-1` (each block's tensors in the order of its table below),
then `norm.w`, `norm.b`, `head.w`, `head.b`. A reader should still look tensors up by name.

### Common to both architectures

| Name | Shape | Use |
|---|---|---|
| `tok.emb` | `[37, E]` | embedding of the input token `s_{t-1}` |
| `pos.emb` | `[ctx, E]` | embedding of `min(t, ctx-1)` |
| `role.emb` | `[3, E]` | |
| `style.emb` | `[6, E]` | |
| `bars.emb` | `[8, E]` | |
| `step.emb` | `[16, E]` | |
| `bar.emb` | `[8, E]` | |
| `gap.emb` | `[10, E]` | |
| `idx.emb` | `[8, E]` | |
| `norm.w`, `norm.b` | `[E]` | final LayerNorm |
| `head.w`, `head.b` | `[37, E]`, `[37]` | output projection (untied from `tok.emb`) |

The input of block 0 is the **sum** of the nine embedding rows above (`tok.emb` through `idx.emb`);
`norm` and `head` are the two tensors after the last block.

### `arch=transformer`

Pre-LayerNorm blocks, `n = 0 .. layers-1`:

| Name | Shape |
|---|---|
| `blocks.n.norm1.w`, `blocks.n.norm1.b` | `[E]` |
| `blocks.n.attn.qkv.w`, `blocks.n.attn.qkv.b` | `[3E, E]`, `[3E]` |
| `blocks.n.attn.out.w`, `blocks.n.attn.out.b` | `[E, E]`, `[E]` |
| `blocks.n.norm2.w`, `blocks.n.norm2.b` | `[E]` |
| `blocks.n.ffn.up.w`, `blocks.n.ffn.up.b` | `[ffn, E]`, `[ffn]` |
| `blocks.n.ffn.down.w`, `blocks.n.ffn.down.b` | `[E, ffn]`, `[E]` |

```
u        = LN(x, norm1.w, norm1.b)
[q|k|v]  = qkv.w * u + qkv.b
a        = causal_softmax_attention(q, k, v)
x       <- x + (out.w * a + out.b)
u        = LN(x, norm2.w, norm2.b)
x       <- x + (ffn.down.w * gelu(ffn.up.w * u + ffn.up.b) + ffn.down.b)
```

`qkv` produces `[q | k | v]` in that order, each `E` wide; head `i` of `q` is
`q[i*hd : (i+1)*hd]` with `hd = E / heads`. Attention is causal, scaled by `1/sqrt(hd)`, softmax
over keys `0..t`, no dropout at inference, no relative-position bias (the positional information is
in `pos.emb` and in the conditioning).

`LN(x, g, b) = (x - mean) / sqrt(var + eps) * g + b` with the **biased** variance (divide by `E`).

`gelu` is the tanh approximation, exactly:

```
gelu(v) = 0.5 * v * (1 + tanh(0.7978845608028654 * (v + 0.044715 * v^3)))
```

### `arch=ssm`

A selective state-space block after Gu and Dao ("Mamba: Linear-Time Sequence Modeling with
Selective State Spaces", 2023) with a **real diagonal** `A`, which makes the recurrence a scalar
first-order scan per channel — a few lines of C++ and an inference state of `dim*expand*state`
floats with no KV cache, which is why the plan wanted it measured against the transformer at all.
`di = dim * expand`.

| Name | Shape |
|---|---|
| `blocks.n.norm1.w`, `blocks.n.norm1.b` | `[E]` |
| `blocks.n.ssm.in.w`, `blocks.n.ssm.in.b` | `[2*di, E]`, `[2*di]` |
| `blocks.n.ssm.conv.w`, `blocks.n.ssm.conv.b` | `[di, convK]`, `[di]` |
| `blocks.n.ssm.xproj.w` | `[dtRank + 2*state, di]` |
| `blocks.n.ssm.dt.w`, `blocks.n.ssm.dt.b` | `[di, dtRank]`, `[di]` |
| `blocks.n.ssm.Alog.w` | `[di, state]` |
| `blocks.n.ssm.D.w` | `[di]` |
| `blocks.n.ssm.out.w`, `blocks.n.ssm.out.b` | `[E, di]`, `[E]` |

Per block, with `u = LN(x, norm1)`:

```
[xin | z] = in.w * u + in.b                       // each di wide, xin first
xc[c][t]  = sum_{k=0..convK-1} conv.w[c][k] * xin[c][t - (convK-1) + k] + conv.b[c]
                                                  // causal depthwise, zero padding before t = 0
xs        = silu(xc)
[dtr | B | C] = xproj.w * xs                      // dtRank, then state, then state
delta[c]  = softplus(dt.w[c] . dtr + dt.b[c])
A[c][n]   = -exp(Alog.w[c][n])
h[c][n]  <- exp(delta[c] * A[c][n]) * h[c][n] + delta[c] * B[n] * xs[c]      // h starts at 0
y[c]      = sum_n C[n] * h[c][n] + D.w[c] * xs[c]
x        <- x + out.w * (y * silu(z)) + out.b
```

`silu(v) = v / (1 + exp(-v))`, `softplus(v) = log(1 + exp(v))` (compute as
`v + log1p(exp(-v))` for `v > 0` to avoid overflow). `B` and `C` are shared across the `di`
channels and vary per time step — that is the "selective" part; `delta` varies per channel and per
time step.

The block output feeds the next block; after the last block comes `norm`, then `head`.

## 5. The reference file `<name>.ref.txt`

Every export writes a sibling `<name>.ref.txt`. It is the oracle the C++ inference is measured
against: the float32 logits and probabilities the **PyTorch** model produced for a handful of
conditioning sets and contexts. It is plain ASCII, LF line endings, one token per whitespace-
separated field, and it is written with `%.9g`, which round-trips float32 exactly.

```
# phosmdl reference
model=<file name of the .phosmdl this belongs to>
arch=<transformer|ssm>
tokenVersion=1
vocab=37
relMin=-12
quant=<int8|float32>
cases=<n>

case 0
role=<0..2>
style=<0..5>
bars=<0..7>
len=<L>
tok=<L integers, 0..36>       # the input tokens, tok[0] is always 12 (the start context)
step=<L integers, 0..15>
bar=<L integers, 0..7>
gap=<L integers, 0..9>
idx=<L integers, 0..7>
logits=<37 floats, %.9g>      # at the last position, L-1
probs=<37 floats, %.9g>       # softmax of the above
case 1
...
```

Fields inside a case may be read in any order; a case ends at the next `case` line or at end of
file. The contexts are **synthetic**, from a fixed seed in `Tools/train/export.py` — a held-out loop
would be a more musical context, but a loop written out as symbols and step positions *is* the loop,
and the bought packs do not leave the machine (PLAN 6.9). The cases cover all three roles, lengths
from one position to past the positional clamp, every gap code and every note-index bucket, which is
all an oracle over a deterministic forward pass needs. `logits` come from the **same tensors that
are in the file** — that is, when `quant=int8` the
reference was produced from the dequantised weights, so a correct C++ reader reproduces them
without a quantisation allowance. Tolerance for the C++ side: `max |logit_cpp - logit_ref| < 1e-3`
and `max |prob_cpp - prob_ref| < 1e-5` in float32 arithmetic; a larger difference is an
implementation bug, not accumulated rounding, at these sizes. `Tools/train/export.py --verify`
recomputes the reference from the written file with a NumPy reader that follows this document
literally, which is a check that the document and the trainer agree.

## 6. Decoding under the stage-A constraints

`sampleConstrained` (`Core/include/phos/CorpusSample.inl`) samples *exactly* from a model
conditioned on per-position allowed sets, and it can only do that because an order-2 Markov chain
has a finite state: the backward pass `beta_i(a, b)` enumerates it. A transformer or an SSM has no
such state, so the exact Pachet–Roy construction does not carry over. Two options, both over the
unchanged alphabet:

1. **Masked left-to-right sampling.** Draw `s_t` from the model's 37 logits with the position's
   allowed-set weights applied and renormalised. Cheap, and every hard constraint (scale, ambitus,
   chord tone on a strong step) still holds for every note; what is lost is only the *exactness* of
   the conditional — a line can paint itself into a corner where the constraint set for a later
   position has little mass. Because the allowed sets here are never empty (the scale set always
   has ten or more of 37 symbols) the corner is a loss of quality, never a failure.
2. **Beam or sequential Monte Carlo** over the masked distribution when that quality loss shows up
   in the metrics of PLAN 11.4.

Start option 1; the constraint sets and their weights (including the colour weights of `Melody.h`)
are unchanged. The `model.prob(a, b, c)` interface of `PitchModel` is **not** the interface of a
stage-B model: a stage-B reader should expose `logits(context, conditioning, out[37])` and let the
sampler mask it, rather than pretending to be order 2.

## 7. Files the inference has to agree with

* `Core/include/phos/Corpus.h` — the alphabet (`kCorpusRelMin`, `kCorpusAlphabet`) and the role
  order.
* `Core/src/Melody.cpp` — where the conditioning of section 3 comes from.
* `Tools/train/export.py` — the writer; its `_read_phosmdl` is a literal NumPy implementation of
  section 1 and is the second opinion on any disagreement.
* `Tools/train/models.py` — the forward pass of section 4 in PyTorch, in the same order.
