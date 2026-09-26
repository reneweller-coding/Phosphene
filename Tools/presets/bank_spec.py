"""The factory preset bank of Phosphene (26.09.2026): 16 groups x 64 presets for each of nine synths.

The user: "Kannst du da bitte mindestens 1024 (sinnvolle!) Presets pro Synth generieren, mit ordentlichem Gebrauch der
neuen Modulationsfähigkeiten" -- and the names "cool ... angelehnt an die Preset-Namen im BerlinSchoolGenerator oder im
AmbientSynth ... sinnvoll untergliedert". The scheme is the BerlinSchoolGenerator's (eph/Presets.h):

- a synth has sixteen groups (the submenus), each of sixty-four presets on an eight by eight grid;
- the grid's rows are the synth's eight adjectives from dark to bright, its columns the group's eight nouns, so every
  name is two words, unique within its synth, and says where on the grid the sound lies;
- a group gives each knob it cares about a range and an axis: 'A' the adjective's (the row, mostly brightness), 'B' the
  noun's (the column, mostly the shape), 'R' a seeded draw, 'C' the constant lo; the rest keep their defaults;
- a group names the filter models that suit it (low-pass responses unless the cutoff range is one a band or high pass
  makes sense in: 26.09.2026, a high pass at 4 .. 18 kHz left a lead at 400 Hz silent) (one is drawn per preset) and its modulation recipes: each comes with a
  chance, takes the next free LFO ('@' in a key) and the next free slot of the matrix ('#'), and a source of -1 means
  "that LFO".

This file is the source; `python Tools/presets/gen_bank.py` writes Core/src/PresetBankData.inl from it.
"""

# ---------------------------------------------------------------- vocabulary of the matrix (Params.cpp)
LFO, MENV, FENV, VEL, KEY, RND = -1, 5, 6, 7, 8, 9                          # sources (kModSourceNames)
P, O2, PW, TP, FMI, CUT, RES, FMODE, LVL, PAN = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10  # poly targets (kModDestNames)
B_PW, B_CUT, B_RES, B_FMODE, B_LVL = 1, 2, 3, 4, 5                          # bass targets (kBassModDestNames)
A_P, A_CUT, A_RES, A_LVL, A_PAN = 1, 2, 3, 4, 5                             # acid targets (kAcidModDestNames)
SINE, TRI, SAWUP, SAWDN, SQUARE, SH, SMOOTH = range(7)                      # LFO shapes
FREE, BARS4, BARS2, BAR1, HALF, QUARTER, EIGHTH, SIXTEENTH, QT, ET = range(10)  # LFO syncs
OWN, MOOG, PROPHET, JUNO, SEM, XPANDER, DIODE, KORG, POLIVOKS, WASP = range(10)  # filter models
XLP4, XLP2, XBP2, XBP4, XHP2, XHP4, XNOTCH, XPHASER = [i / 7.0 for i in range(8)]  # Xpander responses as modes
SUPERSAW, VA, FM, WT = 0, 1, 2, 3                                           # poly.osc


def lfo(chance, dst, lo, hi, shape=SINE, sync=FREE, rate=(0.2, 1.0), retrig=0, fade=None, src=LFO):
    """A recipe: one LFO (or another source) on one target, amount drawn from [lo, hi]."""
    knobs = [("mx#_src", src, src, "C"), ("mx#_dst", dst, dst, "C"), ("mx#_amount", lo, hi, "R")]
    if src == LFO:
        knobs += [("lfo@_shape", shape, shape, "C"), ("lfo@_sync", sync, sync, "C")]
        if sync == FREE:
            knobs.append(("lfo@_rate", rate[0], rate[1], "R"))
        if retrig:
            knobs.append(("lfo@_retrig", 1, 1, "C"))
        if fade:
            knobs.append(("lfo@_fade", fade[0], fade[1], "R"))
    return (chance, knobs)


def menv(chance, dst, lo, hi, decay=(100, 600), attack=(0.1, 5), sustain=0.0):
    """A recipe: the modulation envelope on one target."""
    return (chance, [("menv_attack", attack[0], attack[1], "R"), ("menv_decay", decay[0], decay[1], "B"),
                     ("menv_sustain", sustain, sustain, "C"),
                     ("mx#_src", MENV, MENV, "C"), ("mx#_dst", dst, dst, "C"), ("mx#_amount", lo, hi, "R")])


def src(chance, source, dst, lo, hi):
    """A recipe: a fixed source (velocity, key, random, filter envelope) on one target."""
    return (chance, [("mx#_src", source, source, "C"), ("mx#_dst", dst, dst, "C"), ("mx#_amount", lo, hi, "R")])


# ---------------------------------------------------------------- the melodic voices (module Poly)
# Common envelopes by role, merged under a group's own knobs.
LEAD_AMP = [("amp_attack", 1, 12, "R"), ("amp_decay", 300, 1200, "R"), ("amp_sustain", 0.7, 1.0, "R"), ("amp_release", 120, 500, "B")]
SHORT_AMP = [("amp_attack", 0.3, 3, "R"), ("amp_decay", 80, 400, "B"), ("amp_sustain", 0.0, 0.25, "R"), ("amp_release", 40, 200, "B")]
PAD_AMP = [("amp_attack", 150, 1500, "B"), ("amp_decay", 800, 3000, "R"), ("amp_sustain", 0.7, 1.0, "R"), ("amp_release", 600, 3000, "B")]
DRONE_AMP = [("amp_attack", 400, 2000, "B"), ("amp_decay", 1500, 4000, "R"), ("amp_sustain", 0.85, 1.0, "R"), ("amp_release", 1200, 4000, "B")]

ARCH = {
    # --- leads
    "supersaw": dict(knobs=[("osc", SUPERSAW, SUPERSAW, "C"), ("detune", 0.45, 0.85, "B"), ("mix", 0.55, 0.9, "R"),
                            ("cutoff", 2500, 16000, "A"), ("resonance", 0.05, 0.3, "R"), ("env_amount", 0.5, 2.5, "B"),
                            ("filter_decay", 150, 900, "B"), ("glide", 0, 40, "R"), ("drift", 0.5, 2, "R")] + LEAD_AMP,
                     filters=[(OWN, 0, 0), (PROPHET, 0, 0), (JUNO, 0, 0), (MOOG, 0, 0)],
                     mods=[lfo(0.6, CUT, 0.04, 0.12, rate=(0.05, 0.3)), src(0.3, VEL, CUT, 0.1, 0.25)]),
    "hoover": dict(knobs=[("osc", SUPERSAW, SUPERSAW, "C"), ("detune", 0.7, 1.0, "B"), ("mix", 0.8, 1.0, "R"),
                          ("osc2", 1, 1, "C"), ("osc2_interval", 1, 1, "C"), ("osc2_mix", 0.3, 0.6, "R"),
                          ("cutoff", 1500, 9000, "A"), ("resonance", 0.1, 0.35, "R"), ("env_amount", 1, 3, "B"),
                          ("filter_decay", 200, 800, "B"), ("glide", 40, 160, "B")] + LEAD_AMP,
                   filters=[(JUNO, 0, 0), (PROPHET, 0, 0), (MOOG, 0, 0)],
                   mods=[menv(1.0, P, -0.35, -0.2, decay=(150, 600)), lfo(0.5, CUT, 0.05, 0.12, sync=QUARTER)]),
    "acidlead": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0, 0.3, "R"), ("cutoff", 400, 3000, "A"), ("resonance", 0.55, 0.85, "B"),
                            ("env_amount", 2.5, 5, "B"), ("filter_decay", 80, 350, "B"), ("glide", 20, 80, "R")] + LEAD_AMP,
                     filters=[(DIODE, 0, 0), (MOOG, 0, 0), (KORG, 0, 0)],
                     mods=[lfo(0.5, CUT, 0.05, 0.15, sync=SIXTEENTH, shape=SH), src(0.5, VEL, CUT, 0.15, 0.35)]),
    "fmbell": dict(knobs=[("osc", FM, FM, "C"), ("fm_ratio", 2, 4, "R"), ("fm_index", 1.5, 5, "B"), ("fm_decay", 100, 800, "B"),
                          ("cutoff", 6000, 16000, "A"), ("resonance", 0, 0.2, "R"), ("env_amount", 0, 1, "R"),
                          ("amp_decay", 300, 1500, "B"), ("amp_sustain", 0.3, 0.7, "R"), ("amp_release", 200, 800, "B"),
                          ("amp_attack", 0.3, 3, "R")],
                   filters=[(OWN, 0, 0), (SEM, 0, 0.15)],
                   mods=[menv(0.6, FMI, 0.3, 0.7, decay=(200, 1000)), src(0.5, VEL, FMI, 0.2, 0.5)]),
    "vocal": dict(knobs=[("osc", WT, WT, "C"), ("table", "list:1,5", None, "R"), ("position", 0.1, 0.9, "B"),
                         ("cutoff", 3000, 14000, "A"), ("resonance", 0.1, 0.35, "R"), ("glide", 20, 100, "R")] + LEAD_AMP,
                  filters=[(OWN, 0, 0), (SEM, 0.2, 0.4), (XPANDER, XLP2, XLP2)],
                  mods=[lfo(1.0, TP, 0.15, 0.4, shape=SMOOTH, rate=(0.2, 1.5)), lfo(0.5, CUT, 0.05, 0.12, sync=HALF)]),
    "screamer": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0.2, 0.6, "R"), ("cutoff", 800, 5000, "A"), ("resonance", 0.6, 0.9, "B"),
                            ("glide", 80, 250, "B"), ("env_amount", 1.5, 3.5, "R")] + LEAD_AMP,
                     filters=[(KORG, 0, 0), (DIODE, 0, 0), (POLIVOKS, 0, 0.2)],
                     mods=[lfo(0.7, P, 0.06, 0.1, rate=(4.5, 6.5), retrig=1, fade=(0.3, 1.0))]),
    "pluck": dict(knobs=[("osc", SUPERSAW, VA, "R"), ("cutoff", 800, 6000, "A"), ("resonance", 0.2, 0.5, "R"), ("env_amount", 2, 4.5, "R"),
                         ("filter_decay", 40, 200, "B")] + SHORT_AMP,
                  filters=[(PROPHET, 0, 0), (MOOG, 0, 0), (SEM, 0, 0.1), (OWN, 0, 0)],
                  mods=[src(0.5, VEL, CUT, 0.2, 0.4), src(0.3, RND, CUT, 0.05, 0.15)]),
    "laser": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0.5, 1, "R"), ("pulse_width", 0.2, 0.5, "R"), ("cutoff", 3000, 12000, "A"),
                         ("resonance", 0.2, 0.5, "R"), ("amp_decay", 150, 600, "B"), ("amp_sustain", 0.4, 0.8, "R"), ("amp_release", 60, 300, "R")],
                  filters=[(OWN, 0, 0), (JUNO, 0, 0), (PROPHET, 0, 0)],
                  mods=[menv(1.0, P, 0.5, 0.8, decay=(30, 150), attack=(0.1, 0.5)), lfo(0.5, CUT, 0.1, 0.25, shape=SH, sync=SIXTEENTH)]),
    "wtmorph": dict(knobs=[("osc", WT, WT, "C"), ("table", "lane:1", None, "R"), ("position", 0, 0.6, "R"), ("pos_env", 0.2, 0.7, "B"),
                           ("pos_decay", 200, 2000, "B"), ("cutoff", 3000, 16000, "A"), ("resonance", 0.05, 0.3, "R")] + LEAD_AMP,
                    filters=[(OWN, 0, 0), (JUNO, 0, 0), (SEM, 0, 0.2)],
                    mods=[lfo(1.0, TP, 0.2, 0.45, shape=TRI, sync=BAR1), menv(0.5, TP, 0.2, 0.4, decay=(300, 1500))]),
    "pwm": dict(knobs=[("osc", VA, VA, "C"), ("wave", 1, 1, "C"), ("pulse_width", 0.15, 0.5, "B"), ("cutoff", 1500, 9000, "A"),
                       ("resonance", 0.1, 0.4, "R"), ("env_amount", 1, 3, "R")] + LEAD_AMP,
                filters=[(SEM, 0, 0.1), (PROPHET, 0, 0), (JUNO, 0, 0)],
                mods=[lfo(1.0, PW, 0.3, 0.6, shape=TRI, rate=(0.3, 2.0)), lfo(0.3, CUT, 0.05, 0.1, sync=HALF)]),
    "sync": dict(knobs=[("osc", WT, WT, "C"), ("table", "list:4", None, "C"), ("position", 0.1, 0.5, "A"), ("pos_env", 0.3, 0.8, "B"),
                        ("pos_decay", 100, 800, "B"), ("cutoff", 4000, 16000, "A"), ("resonance", 0.1, 0.3, "R")] + LEAD_AMP,
                 filters=[(OWN, 0, 0), (MOOG, 0, 0), (PROPHET, 0, 0)],
                 mods=[menv(1.0, TP, 0.3, 0.6, decay=(150, 900))]),
    "growl": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0.3, 0.7, "R"), ("osc2", 2, 2, "C"), ("osc2_interval", 1, 1, "C"),
                         ("osc2_mix", 0.3, 0.6, "R"), ("cutoff", 300, 2000, "A"), ("resonance", 0.4, 0.7, "B"), ("env_amount", 1, 3, "R")] + LEAD_AMP,
                  filters=[(POLIVOKS, 0, 0.3), (WASP, 0, 0.3), (DIODE, 0, 0)],
                  mods=[lfo(0.8, CUT, 0.1, 0.25, sync=EIGHTH), lfo(0.5, FMODE, 0.2, 0.4, shape=SH, sync=SIXTEENTH)]),
    "glass": dict(knobs=[("osc", WT, WT, "C"), ("table", "list:2", None, "C"), ("position", 0.2, 0.8, "B"), ("cutoff", 5000, 18000, "A"),
                         ("resonance", 0, 0.25, "R")] + LEAD_AMP,
                  filters=[(OWN, 0, 0), (XPANDER, XLP2, XLP2), (SEM, 0, 0.2)],
                  mods=[lfo(0.6, TP, 0.1, 0.25, rate=(0.1, 0.4))]),
    "flute": dict(knobs=[("osc", FM, FM, "C"), ("fm_ratio", 1, 1, "C"), ("fm_index", 0.5, 1.5, "B"), ("cutoff", 1500, 6000, "A"),
                         ("resonance", 0.1, 0.3, "R"), ("amp_attack", 20, 120, "B"), ("amp_sustain", 0.7, 0.95, "R"), ("amp_release", 150, 500, "R")],
                  filters=[(SEM, 0.25, 0.35), (JUNO, 0, 0)],
                  mods=[lfo(1.0, P, 0.05, 0.08, rate=(4.0, 6.0), retrig=1, fade=(0.5, 1.5)), src(0.5, RND, CUT, 0.1, 0.2)]),
    "octaves": dict(knobs=[("osc", SUPERSAW, SUPERSAW, "C"), ("detune", 0.3, 0.6, "B"), ("osc2", 1, 1, "C"), ("osc2_interval", 5, 5, "C"),
                           ("osc2_mix", 0.2, 0.45, "R"), ("cutoff", 3000, 16000, "A"), ("resonance", 0.05, 0.2, "R")] + LEAD_AMP,
                    filters=[(OWN, 0, 0), (JUNO, 0, 0), (PROPHET, 0, 0)],
                    mods=[lfo(0.5, CUT, 0.04, 0.1, sync=BARS2), lfo(0.4, PAN, 0.2, 0.4, sync=BAR1)]),
    "echo": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0, 0.5, "R"), ("cutoff", 2000, 9000, "A"), ("resonance", 0.1, 0.3, "R"),
                        ("delay_send", 0.3, 0.6, "B"), ("delay_feedback", 0.4, 0.7, "B")] + LEAD_AMP,
                 filters=[(OWN, 0, 0), (SEM, 0, 0.1), (JUNO, 0, 0)],
                 mods=[lfo(0.6, PAN, 0.3, 0.6, sync=BAR1), lfo(0.4, CUT, 0.05, 0.12, sync=HALF)]),
    # --- counter: dry, short, answering (the user's articulation rule: it whips, it does not glide)
    "drysaw": dict(knobs=[("osc", SUPERSAW, SUPERSAW, "C"), ("detune", 0.2, 0.5, "B"), ("cutoff", 1500, 9000, "A"), ("resonance", 0.1, 0.35, "R"),
                          ("env_amount", 1.5, 3.5, "R"), ("filter_decay", 60, 300, "B"), ("glide", 0, 0, "C")] + SHORT_AMP,
                   filters=[(OWN, 0, 0), (PROPHET, 0, 0), (MOOG, 0, 0)],
                   mods=[src(0.5, VEL, CUT, 0.15, 0.3)]),
    "rubber": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0, 0.4, "R"), ("cutoff", 500, 3000, "A"), ("resonance", 0.45, 0.75, "B"),
                          ("env_amount", 2, 4.5, "R"), ("filter_decay", 60, 250, "B")] + SHORT_AMP,
                   filters=[(DIODE, 0, 0), (MOOG, 0, 0), (KORG, 0, 0)],
                   mods=[lfo(0.4, CUT, 0.05, 0.12, sync=EIGHTH, shape=SAWDN)]),
    "fmpluck": dict(knobs=[("osc", FM, FM, "C"), ("fm_ratio", 2, 3.5, "R"), ("fm_index", 1, 4, "B"), ("fm_decay", 30, 250, "B"),
                           ("cutoff", 5000, 16000, "A")] + SHORT_AMP,
                    filters=[(OWN, 0, 0), (SEM, 0, 0.1)],
                    mods=[menv(0.7, FMI, 0.3, 0.8, decay=(40, 250)), src(0.4, VEL, FMI, 0.2, 0.4)]),
    "hollow": dict(knobs=[("osc", VA, VA, "C"), ("wave", 1, 1, "C"), ("pulse_width", 0.1, 0.35, "B"), ("cutoff", 1200, 7000, "A"),
                          ("resonance", 0.15, 0.4, "R")] + SHORT_AMP,
                   filters=[(SEM, 0.3, 0.5), (XPANDER, XBP2, XBP2), (JUNO, 0, 0)],
                   mods=[lfo(0.6, PW, 0.2, 0.4, sync=QUARTER, shape=TRI)]),
    "chirp": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0.4, 0.9, "R"), ("cutoff", 2500, 11000, "A"), ("resonance", 0.3, 0.6, "R")] + SHORT_AMP,
                  filters=[(OWN, 0, 0), (KORG, 0, 0), (SEM, 0, 0.1)],
                  mods=[menv(1.0, P, 0.3, 0.55, decay=(15, 80), attack=(0.1, 0.3))]),
    "metaltick": dict(knobs=[("osc", FM, FM, "C"), ("fm_ratio", 3.5, 7, "R"), ("fm_index", 2, 6, "B"), ("fm_decay", 20, 150, "B"),
                             ("cutoff", 4000, 16000, "A")] + SHORT_AMP,
                      filters=[(OWN, 0, 0), (SEM, 0, 0.1)],
                      mods=[src(0.5, RND, FMI, 0.2, 0.4)]),
    # --- arp
    "sequence": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0, 0.4, "R"), ("cutoff", 700, 5000, "A"), ("resonance", 0.25, 0.55, "B"),
                            ("env_amount", 1.5, 4, "R"), ("filter_decay", 60, 300, "B")] + SHORT_AMP,
                     filters=[(MOOG, 0, 0), (PROPHET, 0, 0), (DIODE, 0, 0), (OWN, 0, 0)],
                     mods=[lfo(0.8, CUT, 0.08, 0.2, sync=BARS2, shape=TRI), src(0.4, VEL, CUT, 0.1, 0.3)]),
    "glassarp": dict(knobs=[("osc", WT, WT, "C"), ("table", "list:2,4", None, "R"), ("position", 0.2, 0.8, "B"), ("cutoff", 4000, 16000, "A"),
                            ("resonance", 0.1, 0.3, "R")] + SHORT_AMP,
                     filters=[(OWN, 0, 0), (XPANDER, XLP2, XLP2), (SEM, 0, 0.2)],
                     mods=[lfo(0.8, TP, 0.15, 0.35, sync=BAR1, shape=TRI)]),
    "wtarp": dict(knobs=[("osc", WT, WT, "C"), ("table", "lane:2", None, "R"), ("position", 0, 0.7, "R"), ("pos_env", 0.2, 0.6, "B"),
                         ("pos_decay", 80, 600, "B"), ("cutoff", 2000, 12000, "A"), ("resonance", 0.1, 0.35, "R")] + SHORT_AMP,
                  filters=[(OWN, 0, 0), (JUNO, 0, 0), (SEM, 0, 0.2)],
                  mods=[lfo(1.0, TP, 0.2, 0.4, sync=BARS2, shape=SINE), src(0.3, RND, TP, 0.1, 0.2)]),
    "ratchet": dict(knobs=[("osc", SUPERSAW, SUPERSAW, "C"), ("detune", 0.3, 0.6, "R"), ("cutoff", 1500, 8000, "A"), ("resonance", 0.2, 0.45, "R"),
                           ("env_amount", 2, 4, "B"), ("filter_decay", 40, 180, "B")] + SHORT_AMP,
                    filters=[(JUNO, 0, 0), (PROPHET, 0, 0), (OWN, 0, 0)],
                    mods=[lfo(0.8, LVL, -0.5, -0.3, sync=SIXTEENTH, shape=SQUARE)]),
    "bubble": dict(knobs=[("osc", FM, FM, "C"), ("fm_ratio", 1, 2, "R"), ("fm_index", 0.5, 2.5, "B"), ("cutoff", 1000, 6000, "A"),
                          ("resonance", 0.4, 0.7, "R")] + SHORT_AMP,
                   filters=[(SEM, 0.1, 0.4), (KORG, 0, 0), (DIODE, 0, 0)],
                   mods=[lfo(1.0, CUT, 0.1, 0.25, shape=SH, sync=SIXTEENTH), src(0.4, KEY, CUT, 0.1, 0.2)]),
    "gatedsaw": dict(knobs=[("osc", SUPERSAW, SUPERSAW, "C"), ("detune", 0.5, 0.8, "B"), ("cutoff", 2000, 12000, "A"), ("resonance", 0.05, 0.25, "R")] + LEAD_AMP,
                     filters=[(OWN, 0, 0), (JUNO, 0, 0)],
                     mods=[lfo(1.0, LVL, -0.8, -0.5, sync=SIXTEENTH, shape=SQUARE), lfo(0.4, CUT, 0.05, 0.12, sync=BARS4)]),
    "panarp": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0.3, 0.8, "R"), ("cutoff", 1500, 8000, "A"), ("resonance", 0.2, 0.45, "R")] + SHORT_AMP,
                   filters=[(PROPHET, 0, 0), (SEM, 0, 0.2), (OWN, 0, 0)],
                   mods=[lfo(1.0, PAN, 0.5, 0.9, sync=QT, shape=TRI), lfo(0.4, CUT, 0.05, 0.15, sync=BAR1)]),
    # --- stab
    "chordstab": dict(knobs=[("osc", SUPERSAW, SUPERSAW, "C"), ("detune", 0.4, 0.75, "B"), ("cutoff", 1500, 10000, "A"), ("resonance", 0.1, 0.3, "R"),
                             ("env_amount", 1.5, 3.5, "R"), ("filter_decay", 60, 300, "B")] + SHORT_AMP,
                      filters=[(OWN, 0, 0), (JUNO, 0, 0), (PROPHET, 0, 0)],
                      mods=[src(0.5, VEL, CUT, 0.1, 0.3)]),
    "organstab": dict(knobs=[("osc", FM, FM, "C"), ("fm_ratio", 2, 2, "C"), ("fm_index", 0.5, 2, "B"), ("osc2", 3, 3, "C"),
                             ("osc2_interval", 5, 5, "C"), ("osc2_mix", 0.2, 0.4, "R"), ("cutoff", 2500, 10000, "A")] + SHORT_AMP,
                      filters=[(OWN, 0, 0), (SEM, 0, 0.1)],
                      mods=[lfo(0.5, P, 0.03, 0.05, rate=(5, 6.5))]),
    "brassstab": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0, 0.2, "R"), ("cutoff", 700, 4000, "A"), ("resonance", 0.1, 0.3, "R"),
                             ("filt_attack", 10, 60, "B"), ("filt_sustain", 0.2, 0.5, "R"), ("env_amount", 1.5, 3, "R")] + SHORT_AMP,
                      filters=[(PROPHET, 0, 0), (MOOG, 0, 0), (SEM, 0, 0)],
                      mods=[src(0.4, VEL, CUT, 0.1, 0.25)]),
    "dubstab": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0.2, 0.5, "R"), ("cutoff", 500, 2500, "A"), ("resonance", 0.3, 0.6, "R"),
                           ("delay_send", 0.3, 0.6, "B"), ("delay_feedback", 0.4, 0.75, "B")] + SHORT_AMP,
                    filters=[(SEM, 0.3, 0.5), (MOOG, 0, 0), (JUNO, 0, 0)],
                    mods=[lfo(0.4, CUT, 0.05, 0.15, sync=BARS2)]),
    # --- pad
    # VA and FM play three unison voices with narrow, quiet sides (Poly.cpp): as pads they sat 8-12 dB narrower in the
    # mid band than the default pad (26.09.2026, testStereoWidth), so their pads bring a detuned supersaw partner.
    "warmpad": dict(knobs=[("osc", SUPERSAW, SUPERSAW, "C"), ("detune", 0.3, 0.6, "B"), ("mix", 0.6, 0.9, "R"), ("cutoff", 700, 5000, "A"),
                           ("resonance", 0.05, 0.2, "R"), ("env_amount", 0, 1, "R"), ("drift", 1, 3, "R")] + PAD_AMP,
                    filters=[(JUNO, 0, 0), (PROPHET, 0, 0), (OWN, 0, 0), (SEM, 0, 0)],
                    mods=[lfo(0.7, CUT, 0.05, 0.12, rate=(0.03, 0.12)), lfo(0.4, PAN, 0.2, 0.4, rate=(0.05, 0.15))]),
    "wtpad": dict(knobs=[("osc", WT, WT, "C"), ("table", "lane:0", None, "R"), ("position", 0, 1, "R"), ("cutoff", 1500, 12000, "A"),
                         ("resonance", 0.05, 0.25, "R")] + PAD_AMP,
                  filters=[(OWN, 0, 0), (JUNO, 0, 0), (SEM, 0, 0.15)],
                  mods=[lfo(1.0, TP, 0.15, 0.4, shape=SMOOTH, rate=(0.03, 0.15)), lfo(0.5, CUT, 0.04, 0.1, sync=BARS4)]),
    "choirpad": dict(knobs=[("osc", WT, WT, "C"), ("table", "list:1,5", None, "R"), ("position", 0.1, 0.8, "B"), ("cutoff", 2000, 10000, "A"),
                            ("resonance", 0.1, 0.3, "R")] + PAD_AMP,
                     filters=[(SEM, 0.2, 0.4), (XPANDER, XLP2, XLP2), (OWN, 0, 0)],
                     mods=[lfo(1.0, TP, 0.2, 0.45, shape=SMOOTH, rate=(0.05, 0.2)), lfo(0.5, FMODE, 0.1, 0.25, rate=(0.03, 0.1))]),
    "shimmer": dict(knobs=[("osc", SUPERSAW, SUPERSAW, "C"), ("detune", 0.2, 0.45, "R"), ("osc2", 4, 4, "C"), ("osc2_interval", 5, 5, "C"),
                           ("osc2_mix", 0.25, 0.5, "B"), ("cutoff", 4000, 16000, "A"), ("resonance", 0, 0.15, "R")] + PAD_AMP,
                    filters=[(OWN, 0, 0), (SEM, 0, 0.2), (JUNO, 0, 0)],
                    mods=[lfo(0.8, TP, 0.2, 0.4, rate=(0.05, 0.2)), lfo(0.5, PAN, 0.3, 0.5, rate=(0.05, 0.12))]),
    "darkpad": dict(knobs=[("osc", VA, VA, "C"), ("detune", 0.45, 0.7, "R"), ("mix", 0.8, 1, "R"), ("osc2", 1, 1, "C"), ("osc2_interval", 3, 3, "C"), ("osc2_mix", 0.3, 0.5, "R"), ("wave", 0, 0.4, "R"), ("cutoff", 250, 1500, "A"), ("resonance", 0.2, 0.5, "B"),
                           ("drift", 2, 5, "R")] + PAD_AMP,
                    filters=[(MOOG, 0, 0), (POLIVOKS, 0, 0.2), (DIODE, 0, 0)],
                    mods=[lfo(0.8, CUT, 0.08, 0.2, rate=(0.02, 0.08)), src(0.3, RND, RES, 0.05, 0.15)]),
    "pwmpad": dict(knobs=[("osc", VA, VA, "C"), ("detune", 0.45, 0.7, "R"), ("mix", 0.8, 1, "R"), ("osc2", 1, 1, "C"), ("osc2_interval", 3, 3, "C"), ("osc2_mix", 0.3, 0.5, "R"), ("wave", 1, 1, "C"), ("pulse_width", 0.2, 0.5, "B"), ("cutoff", 1000, 6000, "A"),
                          ("resonance", 0.05, 0.2, "R")] + PAD_AMP,
                   filters=[(JUNO, 0, 0), (SEM, 0, 0.1), (OWN, 0, 0)],
                   mods=[lfo(1.0, PW, 0.25, 0.5, shape=TRI, rate=(0.1, 0.6)), lfo(0.4, CUT, 0.04, 0.1, rate=(0.02, 0.1))]),
    "sweeppad": dict(knobs=[("osc", SUPERSAW, SUPERSAW, "C"), ("detune", 0.4, 0.7, "R"), ("cutoff", 500, 3000, "A"), ("resonance", 0.3, 0.6, "B")] + PAD_AMP,
                     filters=[(MOOG, 0, 0), (PROPHET, 0, 0), (JUNO, 0, 0)],
                     mods=[lfo(1.0, CUT, 0.2, 0.45, shape=TRI, sync=BARS4)]),
    # Rhythmic pads (26.09.2026): a soft pulse on the level, not the arp's square gate -- its sidebands put the pad's
    # share under 120 Hz at -27 dB (mix_audit, the guide: -30); the hard rhythm is the trance gate's (pad.gate).
    "rhythmpad": dict(knobs=[("osc", SUPERSAW, SUPERSAW, "C"), ("detune", 0.4, 0.7, "B"), ("mix", 0.7, 0.9, "R"),
                             ("cutoff", 2000, 9000, "A"), ("resonance", 0.05, 0.25, "R")] + PAD_AMP,
                      filters=[(OWN, 0, 0), (JUNO, 0, 0)],
                      mods=[lfo(1.0, LVL, -0.45, -0.25, sync=EIGHTH, shape=TRI), lfo(0.4, CUT, 0.05, 0.12, sync=BARS4)]),
    "fmpad": dict(knobs=[("osc", FM, FM, "C"), ("detune", 0.45, 0.7, "R"), ("mix", 0.8, 1, "R"), ("osc2", 1, 1, "C"), ("osc2_interval", 3, 3, "C"), ("osc2_mix", 0.3, 0.5, "R"), ("fm_ratio", 1, 3, "R"), ("fm_index", 0.5, 2.5, "B"), ("fm_decay", 800, 2000, "R"),
                         ("cutoff", 3000, 12000, "A")] + PAD_AMP,
                  filters=[(OWN, 0, 0), (SEM, 0, 0.1)],
                  mods=[lfo(1.0, FMI, 0.2, 0.5, rate=(0.05, 0.3), shape=SMOOTH)]),
    # --- drone
    "subdrone": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0, 0.3, "R"), ("cutoff", 200, 900, "A"), ("resonance", 0.05, 0.25, "R"), ("drift", 1, 4, "R")] + DRONE_AMP,
                     filters=[(MOOG, 0, 0), (OWN, 0, 0), (DIODE, 0, 0)],
                     mods=[lfo(0.8, CUT, 0.05, 0.15, rate=(0.01, 0.05)), lfo(0.4, LVL, -0.2, -0.1, rate=(0.02, 0.07))]),
    "wtdrone": dict(knobs=[("osc", WT, WT, "C"), ("table", "lane:3", None, "R"), ("position", 0, 1, "R"), ("cutoff", 600, 6000, "A"),
                           ("resonance", 0.05, 0.3, "R")] + DRONE_AMP,
                    filters=[(OWN, 0, 0), (JUNO, 0, 0), (SEM, 0, 0.2)],
                    mods=[lfo(1.0, TP, 0.2, 0.5, shape=SMOOTH, rate=(0.01, 0.06)), lfo(0.5, CUT, 0.05, 0.12, rate=(0.01, 0.04))]),
    "ritual": dict(knobs=[("osc", FM, FM, "C"), ("fm_ratio", 1, 2, "R"), ("fm_index", 0.3, 1.5, "B"), ("cutoff", 400, 3000, "A"),
                          ("resonance", 0.2, 0.5, "R")] + DRONE_AMP,
                   filters=[(SEM, 0.1, 0.4), (POLIVOKS, 0, 0.3), (WASP, 0, 0.2)],
                   mods=[lfo(1.0, FMODE, 0.1, 0.35, shape=SMOOTH, rate=(0.01, 0.05)), lfo(0.6, FMI, 0.1, 0.3, rate=(0.02, 0.08))]),
    "didge": dict(knobs=[("osc", VA, VA, "C"), ("wave", 0.3, 0.7, "R"), ("cutoff", 300, 1500, "A"), ("resonance", 0.4, 0.7, "B")] + DRONE_AMP,
                  filters=[(SEM, 0.2, 0.4), (XPANDER, XBP2, XBP2), (KORG, 0, 0)],
                  mods=[lfo(1.0, CUT, 0.15, 0.35, sync=EIGHTH, shape=SINE), lfo(0.5, FMODE, 0.1, 0.2, sync=BAR1)]),
}

VOICE_SYNTHS = {
    "lead": dict(instance=0, adj=["Umbral", "Nocturne", "Obsidian", "Twilight", "Lunar", "Solar", "Prismatic", "Supernova"], groups=[
        ("Supersaw Anthem", "supersaw", ["Anthem", "Horizon", "Eclipse", "Zenith", "Beacon", "Summit", "Pinnacle", "Crown"]),
        ("Hoover", "hoover", ["Hoover", "Mentasm", "Dominator", "Siren", "Juggernaut", "Leviathan", "Colossus", "Titan"]),
        ("Acid Lead", "acidlead", ["Squelch", "Rubber", "Venom", "Acidrain", "Corrosion", "Toxin", "Serpent", "Hydra"]),
        ("FM Bell", "fmbell", ["Bell", "Chime", "Carillon", "Gong", "Tingsha", "Glockenspiel", "Temple", "Singing Bowl"]),
        ("Vocal Formant", "vocal", ["Choir", "Chant", "Oracle", "Shaman", "Whisper", "Sibyl", "Muse", "Psalm"]),
        ("Screaming Glide", "screamer", ["Banshee", "Wail", "Howl", "Scream", "Shriek", "Harpy", "Valkyrie", "Fury"]),
        ("Psy Pluck", "pluck", ["Pluck", "Droplet", "Pebble", "Spark", "Needle", "Flicker", "Pollen", "Dewdrop"]),
        ("Laser Zap", "laser", ["Laser", "Photon", "Quasar", "Pulsar", "Ion", "Plasma", "Tachyon", "Neutrino"]),
        ("Wavetable Morph", "wtmorph", ["Morph", "Shift", "Flux", "Mirage", "Chimera", "Kaleidoscope", "Metamorph", "Shapeshifter"]),
        ("Pulse Width", "pwm", ["Pulse", "Throb", "Heartbeat", "Pendulum", "Tremor", "Orbit", "Oscillon", "Metronome"]),
        ("Hard Sync", "sync", ["Sync", "Tear", "Rip", "Blade", "Razor", "Shard", "Fracture", "Rift"]),
        ("Dark Growl", "growl", ["Growl", "Beast", "Wraith", "Ghoul", "Golem", "Minotaur", "Kraken", "Behemoth"]),
        ("Glass Lead", "glass", ["Glass", "Prism", "Icicle", "Mirror", "Lens", "Quartz", "Facet", "Diamond"]),
        ("Tribal Flute", "flute", ["Reed", "Bamboo", "Ocarina", "Pipe", "Shakuhachi", "Quena", "Zampona", "Duduk"]),
        ("Rising Octaves", "octaves", ["Rapture", "Ascension", "Euphoria", "Elation", "Uplift", "Soar", "Nirvana", "Bliss"]),
        ("Echo Lead", "echo", ["Echo", "Canyon", "Reverie", "Ripple", "Cascade", "Labyrinth", "Halo", "Wanderer"]),
    ]),
    "counter": dict(instance=1, adj=["Shadow", "Velvet", "Smoke", "Dusk", "Silver", "Amber", "Neon", "Radiant"], groups=[
        ("Dry Saw", "drysaw", ["Whip", "Lash", "Crack", "Snap", "Strike", "Flick", "Jolt", "Spur"]),
        ("Rubber Bounce", "rubber", ["Bounce", "Spring", "Rebound", "Elastic", "Ricochet", "Pogo", "Trampoline", "Boing"]),
        ("FM Pluck", "fmpluck", ["Kalimba", "Marimba", "Koto", "Sitar", "Santoor", "Balafon", "Harp", "Zither"]),
        ("Hollow Square", "hollow", ["Hollow", "Vessel", "Cavern", "Chamber", "Shell", "Gourd", "Grotto", "Tunnel"]),
        ("Digital Chirp", "chirp", ["Chirp", "Tweet", "Blip", "Bleep", "Glitch", "Pixel", "Byte", "Circuit"]),
        ("Metal Tick", "metaltick", ["Tick", "Rivet", "Anvil", "Chisel", "Hammer", "Spanner", "Forge", "Bolt"]),
        ("Acid Answer", "acidlead", ["Reply", "Retort", "Riposte", "Parry", "Counter", "Rebuttal", "Response", "Echoform"]),
        ("Psy Pluck", "pluck", ["Seed", "Spore", "Petal", "Thorn", "Bud", "Sprout", "Stem", "Burr"]),
        ("Vocal Formant", "vocal", ["Murmur", "Hum", "Vowel", "Syllable", "Utterance", "Invocation", "Incantation", "Hymn"]),
        ("Laser Zap", "laser", ["Zap", "Bolt Of Light", "Beam", "Ray", "Flash", "Flare", "Strobe", "Lumen"]),
        ("Wavetable Morph", "wtmorph", ["Fractal", "Mandala", "Spiral", "Vortex", "Helix", "Tessellation", "Hypercube", "Moebius"]),
        ("Pulse Width", "pwm", ["Flutter", "Wobble", "Waver", "Quiver", "Shiver", "Ripplet", "Undulate", "Vibrato"]),
        ("Glass Lead", "glass", ["Sliver", "Splinter", "Crystalline", "Frost", "Rime", "Hail", "Sleet", "Glaze"]),
        ("Dark Growl", "growl", ["Snarl", "Grunt", "Rumble", "Grumble", "Gnash", "Maw", "Fang", "Talon"]),
        ("Hard Sync", "sync", ["Edge", "Cutter", "Scythe", "Sickle", "Saber", "Katana", "Dagger", "Lance"]),
        ("Echo Answer", "echo", ["Answer", "Callback", "Refrain", "Reprise", "Antiphon", "Chorus", "Verse", "Coda"]),
    ]),
    "arp": dict(instance=2, adj=["Deep", "Murky", "Hazy", "Misty", "Clear", "Crystal", "Glinting", "Dazzling"], groups=[
        ("Rolling Sequence", "sequence", ["Rotor", "Turbine", "Gyro", "Spinner", "Carousel", "Whirl", "Cyclone", "Dynamo"]),
        ("Glass Arp", "glassarp", ["Lattice", "Filigree", "Snowflake", "Crystal Web", "Chandelier", "Trellis", "Mesh", "Grid"]),
        ("Wavetable Arp", "wtarp", ["Motif", "Figure", "Cell", "Pattern", "Loop", "Cycle", "Sequence", "Phrase"]),
        ("Ratchet", "ratchet", ["Ratchet", "Stutter", "Rattle", "Clatter", "Chatter", "Tremolo", "Roll", "Burst"]),
        ("Bubble", "bubble", ["Bubble", "Fizz", "Froth", "Foam", "Geyser", "Spring Water", "Lagoon", "Brook"]),
        ("Gated Saw", "gatedsaw", ["Gate", "Chop", "Slice", "Dice", "Mince", "Shred", "Stripe", "Stencil"]),
        ("Pan Arp", "panarp", ["Swing", "Sway", "Drift", "Glide", "Pendulum Arc", "Tide", "Wave", "Swell"]),
        ("Acid Arp", "rubber", ["Tentacle", "Jelly", "Slime", "Goo", "Ooze", "Gel", "Syrup", "Nectar"]),
        ("FM Pluck", "fmpluck", ["Raindrop", "Hailstone", "Pearl", "Bead", "Marble", "Sequin", "Glint", "Twinkle"]),
        ("Psy Pluck", "pluck", ["Firefly", "Ember", "Cinder", "Sparkler", "Glowworm", "Lantern", "Candle", "Wisp"]),
        ("Hollow Square", "hollow", ["Flute Loop", "Pan Pipe", "Whistle", "Recorder", "Fife", "Piccolo", "Kazoo", "Calliope"]),
        ("Digital Chirp", "chirp", ["Morse", "Signal", "Telegraph", "Beacon Code", "Radar", "Sonar", "Transmission", "Frequency"]),
        ("Metal Tick", "metaltick", ["Clockwork", "Gear", "Cog", "Sprocket", "Escapement", "Pinion", "Balance Wheel", "Mainspring"]),
        ("Laser Arp", "laser", ["Tracer", "Streak", "Comet Tail", "Meteor", "Shooting Star", "Starlight", "Nova", "Corona"]),
        ("Vocal Arp", "vocal", ["Syllabic", "Stammer", "Babble", "Chatterbox", "Echolalia", "Gibberish", "Scat", "Yodel"]),
        ("Echo Arp", "echo", ["Delay Line", "Feedback", "Repeat", "Recursion", "Loopback", "Iteration", "Multiply", "Infinity"]),
    ]),
    "stab": dict(instance=3, adj=["Buried", "Muffled", "Dusty", "Warm", "Crisp", "Sharp", "Searing", "Blinding"], groups=[
        ("Chord Stab", "chordstab", ["Stab", "Hit", "Punch", "Blow", "Jab", "Thrust", "Impact", "Slam"]),
        ("Organ Stab", "organstab", ["Organ", "Harmonium", "Tonewheel", "Cathedral", "Chapel", "Pipe Organ", "Drawbar", "Rotary"]),
        ("Brass Stab", "brassstab", ["Brass", "Horn", "Trumpet", "Fanfare", "Bugle", "Herald", "Clarion", "Trombone"]),
        ("Dub Stab", "dubstab", ["Dub", "Skank", "Riddim", "Echo Chamber", "Space Echo", "Tape Loop", "Spring Tank", "Siren Call"]),
        ("Rave Hit", "chordstab", ["Rave", "Warehouse", "Strobe Hall", "Laser Hall", "Mainstage", "Arena", "Stadium", "Festival"]),
        ("Hoover Stab", "hoover", ["Vacuum", "Turbo", "Jet", "Afterburner", "Rocket", "Thruster", "Booster", "Ignition"]),
        ("Acid Stab", "rubber", ["Acid Hit", "Corrode", "Etch", "Burn", "Scald", "Sizzle", "Hiss", "Fume"]),
        ("FM Stab", "fmpluck", ["Metallic", "Steel", "Tin", "Copper", "Nickel", "Cobalt", "Titanium", "Tungsten"]),
        ("Glass Stab", "glass", ["Shatter", "Crash Glass", "Splinter Hit", "Smash", "Crack Glass", "Pane", "Window", "Mosaic"]),
        ("PWM Stab", "hollow", ["Throb Hit", "Pound", "Knock", "Rap", "Tap", "Thump", "Bump", "Pulse Hit"]),
        ("Growl Stab", "growl", ["Roar", "Bellow", "Bark", "Yowl", "Rage", "Wrath", "Riot", "Havoc"]),
        ("Zap Stab", "laser", ["Blaster", "Phaser Shot", "Ray Gun", "Pulse Rifle", "Disruptor", "Stun", "Blast", "Discharge"]),
        ("Sync Stab", "sync", ["Cleave", "Split", "Carve", "Hack", "Chop Hit", "Axe", "Hatchet", "Guillotine"]),
        ("Vocal Stab", "vocal", ["Shout", "Yell", "Call", "Cry", "Chant Hit", "Hey", "Ho", "Ahh"]),
        ("Gated Stab", "gatedsaw", ["Chopper", "Helicopter", "Propeller", "Blade Runner", "Windmill", "Fan", "Rotor Blade", "Paddle"]),
        ("Supersaw Stab", "supersaw", ["Blaze", "Flame", "Inferno", "Wildfire", "Bonfire", "Torch", "Beacon Fire", "Pyre"]),
    ]),
    "pad": dict(instance=4, adj=["Abyssal", "Oceanic", "Twilight", "Nebular", "Aurora", "Celestial", "Luminous", "Radiant"], groups=[
        ("Warm Pad", "warmpad", ["Haze", "Veil", "Mist", "Cloudbank", "Breath", "Down", "Fleece", "Murmur Pad"]),
        ("Wavetable Pad", "wtpad", ["Tapestry", "Weave", "Silk", "Satin", "Gossamer", "Chiffon", "Brocade", "Velvet Sky"]),
        ("Choir Pad", "choirpad", ["Angels", "Seraphim", "Cherubim", "Hallelujah", "Vespers", "Matins", "Requiem", "Gloria"]),
        ("Shimmer", "shimmer", ["Shimmer", "Glimmer", "Glow", "Gleam", "Sheen", "Lustre", "Radiance", "Iridescence"]),
        ("Dark Pad", "darkpad", ["Abyss", "Void", "Chasm", "Pit", "Depths", "Trench", "Underworld", "Netherworld"]),
        ("PWM Pad", "pwmpad", ["Tide Pad", "Current", "Undertow", "Swell Pad", "Ebb", "Flow", "Surge", "Wake"]),
        ("Sweep Pad", "sweeppad", ["Sweep", "Arc Sweep", "Crescent", "Sunrise", "Sunset", "Dawn", "Dusk Pad", "Equinox"]),
        ("FM Pad", "fmpad", ["Bellscape", "Chimescape", "Harmonic", "Overtone", "Partial", "Spectrum", "Resonance", "Timbre"]),
        ("Supersaw Pad", "supersaw", ["Cathedral", "Basilica", "Temple Pad", "Sanctuary", "Pantheon", "Ziggurat", "Pyramid", "Stupa"]),
        ("Glass Pad", "glass", ["Ice Field", "Glacier", "Tundra", "Aurora Borealis", "Snowfield", "Permafrost", "Frostbite", "Icecap"]),
        ("Vocal Pad", "vocal", ["Spirit", "Soul", "Ghost", "Phantom", "Specter", "Revenant", "Apparition", "Presence"]),
        ("Drifting Pad", "warmpad", ["Nomad", "Pilgrim", "Voyager", "Drifter", "Wayfarer", "Vagabond", "Traveller", "Castaway"]),
        ("Cosmic Pad", "wtpad", ["Nebula", "Galaxy", "Cosmos", "Stardust", "Milky Way", "Andromeda", "Orion", "Pleiades"]),
        ("Underwater Pad", "darkpad", ["Reef", "Kelp", "Coral", "Abyssal Plain", "Seabed", "Mariana", "Atlantis", "Lemuria"]),
        ("Evolving Pad", "choirpad", ["Genesis", "Evolution", "Emergence", "Awakening", "Rebirth", "Transcendence", "Ascent", "Becoming"]),
        ("Rhythmic Pad", "rhythmpad", ["Heartland", "Pulse Field", "Rhythm Sea", "Tribal Sky", "Drum Circle", "Ceremony", "Ritual Pad", "Trance"]),
    ]),
    "drone": dict(instance=5, adj=["Primordial", "Subterranean", "Cavernous", "Earthen", "Misty", "Aethereal", "Astral", "Sidereal"], groups=[
        ("Sub Drone", "subdrone", ["Bedrock", "Mantle", "Core", "Magma", "Tectonic", "Fault", "Strata", "Basalt"]),
        ("Wavetable Drone", "wtdrone", ["Monolith", "Obelisk", "Menhir", "Dolmen", "Cairn", "Megalith", "Stonehenge", "Totem"]),
        ("Ritual Drone", "ritual", ["Ritual", "Rite", "Ceremony Fire", "Offering", "Altar", "Sacrament", "Vigil", "Trance State"]),
        ("Didgeridoo", "didge", ["Didgeridoo", "Yidaki", "Songline", "Dreamtime", "Outback", "Uluru", "Billabong", "Walkabout"]),
        ("Dark Drone", "darkpad", ["Shadowland", "Eventide", "Nightfall", "Gloom", "Umbra", "Penumbra", "Blackout", "Midnight Sun"]),
        ("Cosmic Drone", "wtdrone", ["Singularity", "Event Horizon", "Wormhole", "Black Hole", "Neutron Star", "Magnetar", "Supercluster", "Dark Matter"]),
        ("Om Drone", "choirpad", ["Om", "Aum", "Mantra Drone", "Chakra", "Kundalini", "Samadhi", "Dharma", "Nirvana Drone"]),
        ("Tanpura", "fmpad", ["Tanpura", "Sruti", "Raga", "Tala", "Drone String", "Sitar Drone", "Veena", "Sarangi"]),
        ("Earth Hum", "subdrone", ["Hum", "Resonance Field", "Schumann", "Telluric", "Geomantic", "Ley Line", "Earth Grid", "Gaia"]),
        ("Swelling Drone", "sweeppad", ["Swell", "Tidal Wave", "Surge Tide", "Crescendo", "Upwelling", "Rising Sea", "Flood", "Deluge"]),
        ("Shimmer Drone", "shimmer", ["Halo Drone", "Aura", "Aureole", "Nimbus", "Corona Drone", "Sunbeam", "Moonbeam", "Starbeam"]),
        ("Pulse Drone", "pwmpad", ["Breath Drone", "Lung", "Respire", "Inhale", "Exhale", "Heart Drone", "Pulse Field Drone", "Lifeforce"]),
        ("Metallic Drone", "ritual", ["Iron Drone", "Steel Drone", "Bell Drone", "Gong Drone", "Singing Metal", "Cymbal Wash", "Tam-Tam", "Bronze Drone"]),
        ("Wind Drone", "warmpad", ["Wind", "Gale", "Breeze", "Zephyr", "Mistral", "Sirocco", "Monsoon", "Tempest"]),
        ("Deep Space", "wtdrone", ["Void Drone", "Vacuum Drone", "Emptiness", "Silence", "Stillness", "Infinity Drone", "Eternity", "Beyond"]),
        ("Throat Drone", "didge", ["Throat", "Overtone Chant", "Khoomei", "Kargyraa", "Sygyt", "Tuvan", "Steppe", "Nomad Song"]),
    ]),
}

# ---------------------------------------------------------------- bass (module Bass)
BASS_ARCH = {
    "rolling": dict(knobs=[("wave", 0, 0.3, "R"), ("cutoff", 150, 700, "A"), ("resonance", 0.1, 0.4, "R"), ("env_amount", 2, 5, "B"),
                           ("filter_decay", 40, 150, "B"), ("drive", 0.2, 0.5, "R"), ("bite", 0.3, 0.6, "R"), ("bite_cutoff", 300, 1200, "A")],
                    filters=[(OWN, 0, 0), (MOOG, 0, 0), (PROPHET, 0, 0)], mods=[src(0.4, VEL, B_CUT, 0.1, 0.25)]),
    "punchy": dict(knobs=[("wave", 0, 0.2, "R"), ("cutoff", 200, 900, "A"), ("resonance", 0.1, 0.3, "R"), ("env_amount", 3, 6, "B"),
                          ("filter_decay", 20, 80, "B"), ("amp_decay", 80, 250, "B"), ("drive", 0.3, 0.6, "R")],
                   filters=[(OWN, 0, 0), (MOOG, 0, 0), (JUNO, 0, 0)], mods=[src(0.4, VEL, B_CUT, 0.15, 0.3)]),
    "squelch": dict(knobs=[("wave", 0, 0.4, "R"), ("cutoff", 150, 600, "A"), ("resonance", 0.45, 0.75, "B"), ("env_amount", 3, 6, "R"),
                           ("filter_decay", 40, 180, "B")],
                    filters=[(DIODE, 0, 0), (KORG, 0, 0), (OWN, 0, 0)], mods=[lfo(0.5, B_CUT, 0.05, 0.15, sync=SIXTEENTH, shape=SH)]),
    "square": dict(knobs=[("wave", 0.7, 1, "R"), ("pulse_width", 0.2, 0.5, "B"), ("cutoff", 200, 900, "A"), ("resonance", 0.1, 0.35, "R"),
                          ("env_amount", 2, 4.5, "R")],
                   filters=[(SEM, 0, 0.1), (JUNO, 0, 0), (OWN, 0, 0)], mods=[lfo(0.6, B_PW, 0.2, 0.4, sync=BAR1, shape=TRI)]),
    "gritty": dict(knobs=[("wave", 0.2, 0.6, "R"), ("cutoff", 250, 1200, "A"), ("resonance", 0.2, 0.5, "R"), ("drive", 0.5, 0.9, "B"),
                          ("bite", 0.5, 0.9, "B"), ("bite_drive", 0.4, 0.9, "R"), ("bite_cutoff", 500, 2000, "A")],
                   filters=[(POLIVOKS, 0, 0.2), (WASP, 0, 0.2), (MOOG, 0, 0)], mods=[src(0.4, RND, B_CUT, 0.05, 0.15)]),
    "sub": dict(knobs=[("wave", 0, 0.15, "R"), ("sub", 0.5, 0.9, "B"), ("cutoff", 100, 400, "A"), ("resonance", 0, 0.2, "R"), ("env_amount", 1, 3, "R")],
                filters=[(OWN, 0, 0), (MOOG, 0, 0)], mods=[]),
    "wobble": dict(knobs=[("wave", 0, 0.4, "R"), ("cutoff", 150, 700, "A"), ("resonance", 0.3, 0.6, "B"), ("env_amount", 1.5, 4, "R")],
                   filters=[(MOOG, 0, 0), (PROPHET, 0, 0), (SEM, 0, 0.2)], mods=[lfo(1.0, B_CUT, 0.15, 0.3, sync=EIGHTH, shape=SINE)]),
    "morph": dict(knobs=[("wave", 0, 0.5, "R"), ("cutoff", 200, 900, "A"), ("resonance", 0.2, 0.45, "R"), ("filter_mode", 0, 0.3, "B")],
                  filters=[(SEM, 0, 0.3), (XPANDER, XLP2, XBP2)], mods=[lfo(1.0, B_FMODE, 0.1, 0.3, sync=BAR1, shape=TRI)]),
}
BASS_SYNTH = dict(adj=["Subsonic", "Abyssal", "Deep", "Dark", "Solid", "Punchy", "Gritty", "Razor"], groups=[
    ("Rolling", "rolling", ["Roller", "Groove", "Engine Room", "Locomotive", "Conveyor", "Treadmill", "Piston", "Crankshaft"]),
    ("Punchy", "punchy", ["Punch", "Knockout", "Uppercut", "Hook", "Haymaker", "Body Blow", "Sucker Punch", "Kidney Shot"]),
    ("Squelchy", "squelch", ["Swamp", "Bog", "Marsh", "Quagmire", "Mire", "Sludge", "Mud", "Tar Pit"]),
    ("Square", "square", ["Block", "Cube", "Brick", "Slab", "Pillar", "Column", "Tower", "Monolith Bass"]),
    ("Gritty", "gritty", ["Gravel", "Grit", "Sandpaper", "Rasp", "File", "Grinder", "Crusher", "Shredder"]),
    ("Sub Heavy", "sub", ["Earthquake", "Tremor Bass", "Seismic", "Rumble Bass", "Quake", "Aftershock", "Epicentre", "Richter"]),
    ("Wobble", "wobble", ["Wobbler", "Jelly Bass", "Rubber Band", "Yo-Yo", "Bungee", "Slinky", "Wobbly", "Warble"]),
    ("Morphing", "morph", ["Mutant", "Mutation", "Hybrid", "Chimera Bass", "Transform", "Transfigure", "Alchemy", "Transmute"]),
    ("Night Roller", "rolling", ["Night Train", "Freight", "Express", "Bullet Train", "Tram", "Subway", "Metro", "Monorail"]),
    ("Forest Roller", "squelch", ["Root", "Trunk", "Bark Bass", "Moss", "Fungus", "Mycelium", "Lichen", "Undergrowth"]),
    ("Full-On", "punchy", ["Full Throttle", "Overdrive", "Redline", "Nitro", "Horsepower", "Torque", "Velocity Bass", "Momentum"]),
    ("Progressive", "sub", ["Deep Current", "Flow State", "Undercurrent", "Riverbed", "Delta", "Estuary", "Fjord", "Lagoon Bass"]),
    ("Hi-Tech", "gritty", ["Circuitry", "Microchip", "Transistor", "Diode Bass", "Capacitor", "Resistor", "Oscillator", "Mainframe"]),
    ("Dark Forest", "wobble", ["Goblin", "Troll", "Imp", "Kobold", "Gnome", "Wendigo", "Banshee Bass", "Ghast"]),
    ("Goa", "square", ["Goa", "Anjuna", "Arambol", "Vagator", "Palolem", "Baga", "Candolim", "Chapora"]),
    ("Psychedelic", "morph", ["Mescaline", "Peyote", "Ayahuasca", "Psilocybin", "Salvia", "Datura", "Soma", "Amrita"]),
])

# ---------------------------------------------------------------- acid (module Acid): the 303 stays, the knobs move
ACID_ARCH = {
    "classic": dict(knobs=[("wave", 0, 0, "C"), ("cutoff", 300, 2500, "A"), ("resonance", 0.55, 0.85, "B"), ("env_amount", 2.5, 5, "R"),
                           ("decay", 100, 500, "B"), ("accent", 0.5, 0.9, "R"), ("drive", 0.5, 0.9, "R")],
                    mods=[lfo(0.4, A_CUT, 0.05, 0.12, sync=BARS2)]),
    "square": dict(knobs=[("wave", 1, 1, "C"), ("cutoff", 300, 2500, "A"), ("resonance", 0.5, 0.8, "B"), ("env_amount", 2, 4.5, "R"),
                          ("decay", 120, 600, "B"), ("drive", 0.4, 0.8, "R")],
                   mods=[lfo(0.4, A_CUT, 0.05, 0.12, sync=BAR1, shape=TRI)]),
    "squelch": dict(knobs=[("wave", 0, 0.3, "R"), ("cutoff", 250, 1800, "A"), ("resonance", 0.7, 0.95, "B"), ("squelch", 1, 1, "C"),
                           ("squelch_start", 6, 24, "B"), ("squelch_time", 15, 60, "R"), ("comb_mix", 0.3, 0.7, "R")],
                    mods=[src(0.4, VEL, A_CUT, 0.1, 0.25)]),
    "liquid": dict(knobs=[("wave", 0, 0.5, "R"), ("cutoff", 400, 3000, "A"), ("resonance", 0.4, 0.7, "R"), ("slide_time", 60, 160, "B"),
                          ("decay", 200, 900, "B"), ("drive", 0.2, 0.5, "R")],
                   mods=[lfo(0.7, A_CUT, 0.08, 0.2, rate=(0.1, 0.4), shape=SMOOTH)]),
    "screaming": dict(knobs=[("wave", 0, 0.2, "R"), ("cutoff", 600, 4000, "A"), ("resonance", 0.8, 0.98, "B"), ("drive", 0.8, 1, "R"),
                             ("accent", 0.7, 1, "R")],
                      mods=[lfo(0.5, A_CUT, 0.05, 0.12, sync=SIXTEENTH, shape=SH)]),
    "wobbly": dict(knobs=[("wave", 0, 0.4, "R"), ("cutoff", 300, 2000, "A"), ("resonance", 0.5, 0.8, "B"), ("env_amount", 1.5, 3.5, "R")],
                   mods=[lfo(1.0, A_CUT, 0.15, 0.3, sync=EIGHTH), lfo(0.4, A_PAN, 0.2, 0.5, sync=BAR1)]),
    "dub": dict(knobs=[("wave", 0, 0.5, "R"), ("cutoff", 300, 1500, "A"), ("resonance", 0.5, 0.8, "R"), ("delay_send", 0.35, 0.7, "B"),
                       ("delay_feedback", 0.5, 0.8, "B")],
                mods=[lfo(0.5, A_PAN, 0.3, 0.6, sync=BARS2)]),
    "vibrato": dict(knobs=[("wave", 0, 0.3, "R"), ("cutoff", 400, 3000, "A"), ("resonance", 0.5, 0.8, "B"), ("slide_time", 40, 120, "R")],
                    mods=[lfo(1.0, A_P, 0.05, 0.08, rate=(4.5, 6.5), retrig=1, fade=(0.2, 0.8))]),
}
ACID_SYNTH = dict(adj=["Murky", "Swampy", "Toxic", "Rubbery", "Liquid", "Neon", "Acidic", "Molten"], groups=[
    ("Classic 303", "classic", ["Silverbox", "Bassline", "Phuture", "Chicago", "Warehouse 303", "Detroit", "Acid Trax", "Hardfloor"]),
    ("Square 303", "square", ["Checker", "Chessboard", "Tile", "Pixel Acid", "Quadrant", "Grid Acid", "Lattice Acid", "Matrix"]),
    ("Squelch", "squelch", ["Gurgle", "Slurp", "Burble", "Glug", "Splash", "Splutter", "Sputter", "Gargle"]),
    ("Liquid", "liquid", ["Mercury", "Quicksilver", "Molten Glass", "Lava", "Honey", "Oil Slick", "Nectar Acid", "Elixir"]),
    ("Screaming", "screaming", ["Screamer", "Siren Acid", "Alarm", "Klaxon", "Air Raid", "Wailer", "Shrieker", "Howler"]),
    ("Wobbly", "wobbly", ["Jellyfish", "Octopus", "Squid", "Cuttlefish", "Nautilus", "Anemone", "Medusa", "Man O War"]),
    ("Dub Acid", "dub", ["Dub Acid", "Echo Acid", "Delay Acid", "Tape Acid", "Spacey", "Lunar Dub", "Cosmic Dub", "Orbit Dub"]),
    ("Vibrato", "vibrato", ["Warbler", "Trill", "Quaver", "Tremble", "Shimmy", "Jitter", "Waver Acid", "Wobblecord"]),
    ("Forest Acid", "squelch", ["Frog", "Toad", "Newt", "Salamander", "Gecko", "Chameleon", "Iguana", "Axolotl"]),
    ("Goa Acid", "classic", ["Kali", "Shiva", "Ganesha", "Hanuman", "Lakshmi", "Durga", "Vishnu", "Brahma"]),
    ("Hi-Tech Acid", "screaming", ["Overclock", "Glitch Acid", "Bitcrush", "Hypervelocity", "Warp Speed", "Lightspeed", "Particle", "Collider"]),
    ("Night Acid", "liquid", ["Owl", "Bat", "Moth", "Nightjar", "Firefly Acid", "Raccoon", "Fox", "Lynx"]),
    ("Rubber Acid", "square", ["Rubber Duck", "Balloon Acid", "Bouncy Ball", "Chewing Gum", "Latex", "Neoprene", "Silicone", "Putty"]),
    ("Hypnotic", "wobbly", ["Hypnosis", "Trance Acid", "Spiral Acid", "Swirl", "Maelstrom", "Whirlpool", "Eddy", "Vortex Acid"]),
    ("Toxic", "screaming", ["Toxic Waste", "Radioactive", "Isotope", "Uranium", "Plutonium", "Chernobyl", "Fallout", "Meltdown"]),
    ("Psychedelic", "vibrato", ["Lysergic", "Tab", "Blotter", "Microdot", "Windowpane", "Orange Sunshine", "Purple Haze", "White Rabbit"]),
])

# ---------------------------------------------------------------- kick (module Kick): no modulation block
# The body's pitch sweep ends before the bass's first sixteenth (26.09.2026): at 145 BPM that is 103 ms, and a
# 60 ms decay still sat up to 37 Hz above the tuned end pitch there -- the kick beat against the bass at 1.5 Hz
# (mix_audit tune_beat_hz, the guide: under 1). At 16 ms the rest is under 0.1 Hz; a boom comes from hold and decay.
KICK_ARCH = {
    "fullon": [("engine", 0, 0, "C"), ("pitch_start", 250, 600, "A"), ("pitch_end", 40, 60, "R"), ("pitch_decay", 8, 14, "B"),
               ("punch", 0.4, 0.8, "R"), ("amp_decay", 180, 350, "B"), ("click_level", 0.3, 0.7, "A"), ("drive", 0.2, 0.5, "R")],
    "forest": [("engine", 0, 0, "C"), ("pitch_start", 150, 400, "A"), ("pitch_end", 35, 50, "R"), ("pitch_decay", 10, 16, "B"),
               ("punch", 0.3, 0.6, "R"), ("amp_decay", 250, 500, "B"), ("click_level", 0.1, 0.4, "A"), ("tone", 3000, 9000, "A")],
    "tight": [("engine", 0, 0, "C"), ("pitch_start", 300, 900, "A"), ("pitch_end", 45, 65, "R"), ("pitch_decay", 5, 12, "B"),
              ("punch", 0.6, 1, "R"), ("amp_decay", 120, 220, "B"), ("click_level", 0.5, 0.9, "A"), ("click_decay", 1, 4, "R")],
    "distorted": [("engine", 0, 0, "C"), ("pitch_start", 250, 700, "A"), ("pitch_end", 40, 60, "R"), ("pitch_decay", 8, 14, "B"),
                  ("drive", 0.6, 1, "B"), ("clip", 0.3, 0.8, "R"), ("amp_decay", 180, 350, "R")],
    "resonant": [("engine", 1, 1, "C"), ("pitch_start", 200, 600, "A"), ("pitch_end", 40, 60, "R"), ("pitch_decay", 10, 16, "B"),
                 ("amp_decay", 200, 450, "B"), ("click_level", 0.2, 0.6, "A")],
    "deep": [("engine", 0, 0, "C"), ("pitch_start", 120, 300, "A"), ("pitch_end", 32, 45, "R"), ("pitch_decay", 12, 16, "B"),
             ("amp_decay", 300, 700, "B"), ("click_level", 0.1, 0.3, "A"), ("tone", 2000, 7000, "A")],
    "clicky": [("engine", 0, 0, "C"), ("click_level", 0.6, 1, "B"), ("click_tone", 2000, 10000, "A"), ("click_decay", 1, 6, "R"),
               ("pitch_start", 300, 800, "A"), ("pitch_decay", 6, 12, "R"), ("amp_decay", 150, 280, "R")],
    "boomy": [("engine", 0, 0, "C"), ("pitch_start", 150, 350, "A"), ("pitch_end", 35, 50, "R"), ("pitch_decay", 12, 16, "B"),
              ("amp_hold", 20, 80, "B"), ("amp_decay", 350, 900, "B"), ("tail_limit", -25, -10, "R")],
}
KICK_SYNTH = dict(adj=["Subterranean", "Deep", "Heavy", "Solid", "Tight", "Punchy", "Snappy", "Razor"], groups=[
    ("Full-On", "fullon", ["Stomp", "Hammerfall", "Pound Kick", "Jackhammer", "Pile Driver", "Battering Ram", "Wrecking Ball", "Sledge"]),
    ("Forest", "forest", ["Log", "Stump", "Boulder", "Timber", "Oak", "Cedar", "Redwood", "Ironwood"]),
    ("Tight", "tight", ["Rivet Kick", "Staple", "Nail", "Tack", "Pin", "Punchcard", "Click Track", "Metronome Kick"]),
    ("Distorted", "distorted", ["Distortion", "Fuzz", "Crunch", "Saturation", "Overload", "Clip Kick", "Burner", "Fryer"]),
    ("Resonant", "resonant", ["Gong Kick", "Bell Kick", "Drumhead", "Membrane", "Timpani", "Taiko", "Djembe", "Tabla"]),
    ("Deep", "deep", ["Abyss Kick", "Ocean Floor", "Deep Sea", "Bathysphere", "Submarine", "Sonar Ping", "Depth Charge", "Trench Kick"]),
    ("Clicky", "clicky", ["Clicker", "Snapper", "Knocker", "Tapper", "Pecker", "Woodpecker", "Castanet", "Clapper"]),
    ("Boomy", "boomy", ["Boom", "Thunder", "Cannon", "Mortar", "Howitzer", "Artillery", "Detonation", "Sonic Boom"]),
    ("Goa", "fullon", ["Palm", "Coconut", "Mango", "Papaya", "Banyan", "Lotus", "Jasmine", "Frangipani"]),
    ("Night", "forest", ["Midnight Kick", "Witching Hour", "Moonrise", "Moonset", "Nocturne Kick", "Night Owl", "Starless", "New Moon"]),
    ("Hi-Tech", "tight", ["Laser Kick", "Pulse Kick", "Hyper", "Turbo Kick", "Quantum", "Nano", "Photonic", "Ionic"]),
    ("Progressive", "deep", ["Glide Kick", "Smooth", "Silk Kick", "Round", "Soft Punch", "Cushion", "Pillow", "Feather"]),
    ("Industrial", "distorted", ["Factory", "Steelworks", "Foundry Kick", "Smelter", "Press", "Stamper", "Rolling Mill", "Blast Furnace"]),
    ("Tribal", "resonant", ["War Drum", "Heartbeat Kick", "Tribe", "Clan", "Warrior", "Chieftain", "Shaman Drum", "Totem Kick"]),
    ("Morning", "clicky", ["Sunrise Kick", "First Light", "Daybreak", "Morning Dew", "Lark", "Sparrow", "Robin", "Wren"]),
    ("Festival", "boomy", ["Main Floor", "Dance Temple", "Mainstage Kick", "Crowd", "Stomp Crowd", "Big Room", "Headliner", "Encore"]),
])

# ---------------------------------------------------------------- style weights (the composer's choice of a group)
# (Goa, Full-On, Progressive, Dark Forest, Hi-Tech) -- Form.h, StyleId. A group weighs as its archetype unless it is
# named here; a group named after a style belongs to it.
ARCH_STYLE = {
    "supersaw": (1, 3, 2, 0.5, 1), "hoover": (0.5, 3, 0.5, 0.5, 2), "acidlead": (3, 1, 0.5, 2, 1), "fmbell": (2, 1, 2, 1, 1),
    "vocal": (2, 1, 2, 1, 0.5), "screamer": (1, 2, 0.3, 2, 2), "pluck": (1, 1, 3, 1, 1), "laser": (1, 2, 0.5, 1, 3),
    "wtmorph": (1, 1, 2, 1, 2), "pwm": (2, 1, 1, 1, 1), "sync": (1, 2, 1, 1, 2), "growl": (0.5, 1, 0.3, 3, 2),
    "glass": (1, 1, 3, 0.5, 1), "flute": (3, 0.5, 1, 1, 0.3), "octaves": (1, 3, 2, 0.3, 1), "echo": (2, 1, 2, 1, 1),
    "drysaw": (1, 2, 1, 1, 2), "rubber": (2, 1, 1, 2, 1), "fmpluck": (2, 1, 2, 1, 1), "hollow": (2, 1, 1, 1, 1),
    "chirp": (1, 1, 1, 1, 3), "metaltick": (1, 1, 1, 2, 2), "sequence": (2, 2, 2, 1, 1), "glassarp": (1, 1, 3, 0.5, 1),
    "wtarp": (1, 1, 2, 1, 2), "ratchet": (1, 2, 1, 1, 3), "bubble": (2, 1, 1, 2, 1), "gatedsaw": (1, 3, 1, 0.5, 1), "rhythmpad": (1, 3, 1, 0.5, 1),
    "panarp": (2, 1, 2, 1, 1), "chordstab": (1, 3, 2, 0.5, 1), "organstab": (2, 1, 2, 1, 0.5), "brassstab": (1, 2, 1, 0.5, 1),
    "dubstab": (2, 1, 2, 1, 0.5), "warmpad": (2, 1, 3, 1, 1), "wtpad": (1, 1, 2, 2, 1), "choirpad": (2, 1, 2, 1, 0.5),
    "shimmer": (1, 2, 3, 0.3, 1), "darkpad": (0.5, 0.5, 0.5, 3, 1), "pwmpad": (2, 1, 1, 1, 1), "sweeppad": (1, 2, 2, 1, 1),
    "fmpad": (1, 1, 2, 1, 1), "subdrone": (1, 1, 1, 2, 1), "wtdrone": (1, 1, 2, 2, 1), "ritual": (2, 0.5, 1, 2, 1),
    "didge": (3, 0.5, 1, 2, 0.5),
    # bass
    "rolling": (1, 2, 1, 1, 1), "punchy": (1, 3, 1, 0.5, 2), "squelch": (2, 1, 0.5, 3, 1), "square": (3, 1, 1, 1, 1),
    "gritty": (0.5, 1, 0.3, 2, 3), "sub": (1, 1, 3, 1, 0.5), "wobble": (1, 1, 1, 2, 1), "morph": (1, 1, 1, 2, 2),
    # acid
    "classic": (3, 1, 0.5, 1, 1), "liquid": (1, 1, 2, 1, 1), "screaming": (0.5, 2, 0.3, 2, 3), "wobbly": (1, 1, 1, 2, 1),
    "dub": (2, 0.5, 2, 1, 0.5), "vibrato": (2, 1, 1, 1, 1),
    # kick
    "fullon": (1, 3, 1, 0.5, 2), "forest": (0.5, 0.5, 1, 3, 1), "tight": (1, 1, 1, 1, 3), "distorted": (0.5, 1, 0.3, 2, 2),
    "resonant": (2, 0.5, 1, 1, 0.5), "deep": (1, 0.5, 3, 1, 0.5), "clicky": (1, 1, 1, 1, 2), "boomy": (2, 2, 1, 1, 0.5),
}
GROUP_STYLE = {
    "Goa": (6, 0.3, 0.3, 0.5, 0.3), "Goa Acid": (6, 0.3, 0.3, 0.5, 0.3), "Full-On": (0.3, 6, 0.5, 0.3, 1),
    "Progressive": (0.3, 0.5, 6, 0.3, 0.3), "Hi-Tech": (0.3, 1, 0.2, 1, 6), "Hi-Tech Acid": (0.3, 1, 0.2, 1, 6),
    "Dark Forest": (0.3, 0.3, 0.3, 6, 1), "Forest Roller": (0.3, 0.3, 0.3, 6, 1), "Forest Acid": (0.3, 0.3, 0.3, 6, 1),
    "Forest": (0.3, 0.3, 0.3, 6, 1), "Night Roller": (0.5, 1, 1, 3, 1), "Night Acid": (0.5, 1, 1, 3, 1), "Night": (0.5, 1, 1, 3, 1),
    "Tribal Flute": (4, 0.3, 1, 1, 0.3), "Didgeridoo": (4, 0.3, 1, 2, 0.3), "Industrial": (0.3, 1, 0.2, 2, 3),
    "Morning": (1, 1, 3, 0.3, 0.5), "Festival": (1, 3, 1, 0.3, 1), "Tribal": (3, 0.5, 1, 1, 0.5), "Psychedelic": (3, 1, 1, 2, 1),
}
