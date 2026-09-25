/**
 * @file MelodyInternal.h
 * @brief What the melody's sources share (Melody.cpp, MelodyHarmony.cpp, MelodyLead.cpp, MelodyParts.cpp).
 *
 * Not part of the core's interface. The line machinery -- symbols, allowed sets, the constrained draw and the
 * repair of a drawn line -- is Melody.cpp's; the makers of the parts are the other three files'.
 */
#pragma once
#include "phos/Melody.h"
#include "phos/Preferences.h"
#include "phos/Corpus.h"
#include "phos/Model.h"
#include "phos/Dsp.h"
#include "phos/Harmony.h"
#include "phos/Rhythm.h"
#include "phos/Sfx.h"
#include "phos/Util.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace phos {
/** @brief The melody's internals, shared by its four sources and by nothing else. */
namespace mel {

/** @name Indices of the melodic parts in the arrays kept per part (Form.h, MelodyPart)
 *  @{ */
constexpr int kAcidI = mpIndex(MelodyPart::Acid), kLeadI = mpIndex(MelodyPart::Lead), kCounterI = mpIndex(MelodyPart::Counter),
              kArpI = mpIndex(MelodyPart::Arp), kStabI = mpIndex(MelodyPart::Stab), kPadI = mpIndex(MelodyPart::Pad),
              kDroneI = mpIndex(MelodyPart::Drone);
/** @} */

using Allowed = std::vector<std::vector<uint8_t>>;

inline int sym(int rel) { return PitchModel::symbol(std::clamp(rel, kCorpusRelMin, kCorpusRelMax)); }

/**
 * @brief Quantises real per-position weights into the sampler's uint8 entries.
 *
 * A constant factor at one position cancels in both normalisations of the sampler
 * (CorpusSample.inl), so the scale is free and the largest weight is put at 200: that spends the
 * whole of the uint8 range on the *shape* of the position, which is all that matters, and still
 * leaves room above for nothing at all -- 255 would only cost resolution to no purpose. A weight
 * more than 200 times smaller than the position's largest lands on 1 rather than on 0, so the tilt
 * can never forbid a tone the constraint allows.
 */
inline void quantise(const std::vector<double>& w, std::vector<uint8_t>& out)
{
    double mx = 0.0;
    for (double v : w) mx = std::max(mx, v);
    if (mx <= 0.0) return;
    for (size_t i = 0; i < w.size(); ++i)
        if (w[i] > 0.0) out[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(200.0 * w[i] / mx)), 1, 255));
}

/**
 * @brief Allowed symbols with a weight per interval to the part root: @p weight(rel) > 0 admits it.
 * @param tension the measured tension curve's tilt at this position (tensionAt): each weight is
 *                multiplied by exp(tension * instability), so unstable tones gain where tension rises
 *
 * Every weight goes through `quantise`, so no admitted tone can be quantised away. An empty set is
 * returned only when @p weight admits nothing, which the callers never ask for.
 * @param rootOffset the part root's distance from the key, in semitones
 * @param lo,hi      the interval range to the part root
 * @param weight     weight(rel), the weight of an interval to the part root
 */
template <class F>
std::vector<uint8_t> weightedSet(int rootOffset, int lo, int hi, F weight, double tension)
{
    std::vector<uint8_t> a(static_cast<size_t>(kCorpusAlphabet), 0);
    std::vector<double> w(static_cast<size_t>(kCorpusAlphabet), 0.0);
    for (int rel = std::max(lo, kCorpusRelMin); rel <= std::min(hi, kCorpusRelMax); ++rel) {
        const double v = weight(rel);
        if (v <= 0.0) continue;
        w[static_cast<size_t>(sym(rel))] = v * std::exp(tension * lerdahlInstability(rel + rootOffset));
    }
    quantise(w, a);
    return a;
}

/**
 * @brief The colour slots of a line: where a neighbour tone may stand (rule 1).
 *
 * A slot is a note on a weak (odd) sixteenth, one sixteenth long, whose next note follows directly
 * on the next sixteenth -- the next note will be the tonic, so the colour tone is heard on the way
 * to it and never held, never on a strong step. Two slots never share a note. The number is the
 * role's target share of the line's notes, rounded at random so that the share holds on average.
 *
 * @param steps  onset of each note; @param lens its length in sixteenths
 * @param taken  positions that are already fixed (and may be neither slot nor resolution)
 * @param loop   the line repeats, so its last note resolves into its first
 * @param cellSteps the loop's length in sixteenths
 * @param resolves  resolves(i) says whether the tonic may stand at position i (a lead's strong step
 *                  must be a chord tone, and not every chord has the tonic)
 * @param target the role's target share of notes on colour slots
 * @param cr     the stream the rounding of the slot count draws from
 */
template <class R>
std::vector<int> colourSlots(const std::vector<int>& steps, const std::vector<int>& lens, const std::vector<uint8_t>& taken,
                             bool loop, int cellSteps, double target, Rng& cr, R resolves)
{
    const int n = static_cast<int>(steps.size());
    std::vector<int> cand;
    for (int i = 0; i < n; ++i) {
        if (steps[static_cast<size_t>(i)] % 2 == 0 || lens[static_cast<size_t>(i)] != 1 || taken[static_cast<size_t>(i)]) continue;
        const bool last = i + 1 == n;
        if (last && !loop) continue;
        const int nx = last ? 0 : i + 1;
        const int nextStep = last ? steps[0] + cellSteps : steps[static_cast<size_t>(nx)];
        if (nextStep != steps[static_cast<size_t>(i)] + 1 || taken[static_cast<size_t>(nx)] || !resolves(nx)) continue;
        cand.push_back(i);
    }
    const double expected = target * n;
    int count = static_cast<int>(std::floor(expected));
    if (cr.uniform() < expected - count) ++count;
    std::vector<int> out;
    std::vector<uint8_t> used(static_cast<size_t>(n), 0);
    while (count > 0 && !cand.empty()) {
        const int k = cr.below(static_cast<int>(cand.size()));
        const int i = cand[static_cast<size_t>(k)];
        cand.erase(cand.begin() + k);
        const int nx = (i + 1) % n;
        if (used[static_cast<size_t>(i)] || used[static_cast<size_t>(nx)]) continue;
        used[static_cast<size_t>(i)] = used[static_cast<size_t>(nx)] = 1;
        if (i > 0) used[static_cast<size_t>(i - 1)] = 1;
        out.push_back(i);
        --count;
    }
    std::sort(out.begin(), out.end());
    return out;
}

/**
 * @brief Which predictive model a track's lines are drawn from (compose.melody_model).
 *
 * Phase 8 puts a trained transformer (Model.h) where the order-2 Markov model of the corpus stood.
 * Everything around it -- the alphabet, the constraint sets, the rhythms, the phrase structure -- is
 * unchanged; only the distribution the pitches come from is another one, and with it the decoder:
 * stage A samples exactly from the constrained distribution, stage B masks position by position and
 * gives that guarantee up (Model.h, sampleMasked). `nn` is null whenever the knob says Markov or no
 * weight file was found, and then nothing at all changes.
 */
struct DrawSource {
    NeuralModel* nn = nullptr;   ///< the neural model, or null for the corpus Markov model
    int style = 0;               ///< the style label of the conditioning; 0 ("unknown") is all the
                                 ///< trained model was ever shown (docs/MODEL_FORMAT.md, section 3)
    int mode = 0;                ///< the mode of the line, as the section plays it (kScaleSteps). Read
                                 ///< only by a weight file whose header carries @c condMode; every
                                 ///< older file ignores it, which is why the makers may always set it.
};

/**
 * @brief The rules of a line that no per-position set can express, because they are about pairs and
 *        windows of notes rather than about one note (rules 6 and 7).
 */
struct LineRules {
    bool loop = false;     ///< the line repeats: its end runs on into its start
    int  maxRun = 2;       ///< at most this many identical pitches in a row (an octave is a change)
    int  window = 16;      ///< sixteenths per window of the pitch-class rule
    int  pcLo = 0;         ///< at least this many pitch classes per window (0 = no rule)
    int  pcHi = 12;        ///< at most this many
};

/** @brief The curve's tilt at one step of a pattern: the measured deviation times the role's tilt. Defined in Melody.cpp. */
double tensionAt(CorpusRoleId role, int step, int bars);
/** @brief The target colour share of a role at the arc's colour @p colour (0..1). Defined in Melody.cpp. */
double colourTarget(CorpusRoleId role, float colour);
/** @brief Whether a set admits nothing at all. Defined in Melody.cpp. */
bool emptySet(const std::vector<uint8_t>& a);
/** @brief An allowed-symbol set that admits the one interval @p rel. Defined in Melody.cpp. */
std::vector<uint8_t> single(int rel);
/** @brief Where a colour tone and its resolution stand, inside the given windows; false where none fits. Defined in Melody.cpp. */
bool placeColour(int scale, int rootOffset, int lo, int hi, int resHi, int near, Rng& cr, int& colourRel, int& tonicRel, bool line);
/** @brief Draws pitches for a sequence of allowed sets; falls back to the lowest allowed symbol of each. Defined in Melody.cpp. */
std::vector<int> drawPitches(const PitchModel& model, CorpusRoleId role, const DrawSource& src, const Allowed& allowed, const std::vector<int>& steps, int bars, int ctx2, int ctx1, double temperature, Rng& r);
/** @brief Redraws single notes of a drawn line until it keeps @p rules. Defined in Melody.cpp. */
void enforceLine(const PitchModel& model, CorpusRoleId role, const DrawSource& src, const Allowed& allowed, const std::vector<int>& steps, int bars, std::vector<uint8_t> fixed, const LineRules& rules, int ctx2, int ctx1, double temperature, Rng& r, std::vector<int>& rels);
/** @brief Whether a line keeps @p rules (the check enforceLine works towards). Defined in Melody.cpp. */
bool lineKeeps(const std::vector<int>& rels, const std::vector<int>& steps, const LineRules& rules);
/** @brief Draws the track's harmony: its progression (pendulum or loop), the chord length, a second half and the breakdown's chords. Defined in MelodyHarmony.cpp. */
void makeChords(MelodyPlan& m, int scale, uint64_t seed, double temperature, const StyleProfile& style);
/** @brief The stab's chord over one of the track's chords: root position, the arp's material (Melody.h). Defined in MelodyHarmony.cpp. */
std::vector<int> stabChordImpl(int scale, int degree, int key, int tones);
/** @brief The lead: one cell, one archetype, one operator per bar. Defined in MelodyLead.cpp. */
void makeLead(MelodyPlan& m, int key, int scale, uint64_t seed, double temperature, float colour, DrawSource src, const LeadStyle& ls, int densityKnob, int entropyKnob, unsigned bassMask, const SetMotif& motif);
/** @brief The counter-lead: a second lead that answers the first. Defined in MelodyLead.cpp. */
void makeCounter(MelodyPlan& m, int key, int scale, uint64_t seed);
/** @brief The acid line: a one- or two-bar cell inside the TB-303 rules of 18.09.2026 (Melody.h). Defined in MelodyParts.cpp. */
void makeAcid(MelodyPlan& m, int key, int scale, uint64_t seed, double temperature, float colour, DrawSource src);
/** @brief The arp: continuous sixteenths split into a low anchor stream and a high stream (rules 11-14). Defined in MelodyParts.cpp. */
void makeArp(MelodyPlan& m, int key, int scale, uint64_t seed, double temperature, float colour, DrawSource src);
/** @brief The pad's gate pattern and the pitch ranges the masking rule works on. Defined in MelodyParts.cpp. */
void makeRangesAndPad(MelodyPlan& m, uint64_t seed);
/** @brief The onsets of one chord block for a figure, as (sixteenth from the block's start, beats held). Defined in MelodyParts.cpp. */
void padOnsets(int figure, int chordBars, std::vector<std::pair<int, double>>& out);
/** @brief The stab's rhythm (19.09.2026): short chord hits on syncopated sixteenths, never on a beat. Defined in MelodyParts.cpp. */
void makeStab(MelodyPlan& m, uint64_t seed);
/** @brief The tonic drone's own decisions: whether it adds the octave, and the period of its evolution. Defined in MelodyParts.cpp. */
void makeDrone(MelodyPlan& m, uint64_t seed);

} // namespace mel
} // namespace phos
